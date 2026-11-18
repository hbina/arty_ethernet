//! Protocol handling: consume RX frames, answer ARP and ICMP echo, emit
//! periodic beacons and gratuitous ARP. ARP and beacons mirror
//! `ethernet_protocol_handler.h` and `ethernet_tx_payloads.h` at the frame
//! level; ICMP echo is new in this design.
//!
//! Phases:
//! - `IDLE` (0): pop an RX frame, else start a pending gratuitous ARP, else a
//!   pending beacon. Timer ticks stay pending until IDLE serves them.
//! - `PARSE` (1): read every RX byte (at least 0..=41) one per cycle, capturing
//!   ARP and IPv4/ICMP fields and both Internet checksums, then decide.
//! - `PREP` (2): copy the selected template from the ROM into the TX slot one
//!   byte per cycle, substituting dynamic fields, then publish its length.
//! - `COPY` (3): stream an ICMP echo request from its RX slot into a TX slot
//!   as an echo reply, rewriting addresses, TTL, type and both checksums.
//!   The RX slot is released only after the copy.
//!
//! Memory reads have one cycle of latency. Every read address is derived from
//! the *next* state, so during each cycle the read data always corresponds to
//! the current state's `index` and pointers.
use crate::ops::{
    bump32, byte_of32, byte_of48, checksum_update, fold16, hex_digit, nibble_of32, shift_in16,
    shift_in32, shift_in48, within, word_part,
};
use bitlogic::{Bits, LogicValue, bits, logic};

pub const PHASE_IDLE: u64 = 0;
pub const PHASE_PARSE: u64 = 1;
pub const PHASE_PREP: u64 = 2;
pub const PHASE_COPY: u64 = 3;

/// TTL written into echo replies.
pub const REPLY_TTL: u64 = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct ProtoConfig {
    pub mac: Bits<48>,
    pub ip: Bits<32>,
    pub beacon_interval: Bits<32>,
    pub garp_interval: Bits<32>,
}

/// Fields captured from the RX frame being parsed. ARP and IPv4 fields are
/// captured in parallel at their offsets; the EtherType decides which apply.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct Parsed {
    pub dst_mac: Bits<48>,
    pub src_mac: Bits<48>,
    pub ethertype: Bits<16>,
    // ARP (offsets 14..=41).
    pub hw_type: Bits<16>,
    pub proto_type: Bits<16>,
    pub hw_len: Bits<8>,
    pub proto_len: Bits<8>,
    pub opcode: Bits<16>,
    pub sender_mac: Bits<48>,
    pub sender_ip: Bits<32>,
    pub target_ip: Bits<32>,
    // IPv4 (offsets 14..=33) and ICMP (34..=37).
    pub ip_ver_ihl: Bits<8>,
    pub ip_total_len: Bits<16>,
    pub ip_frag: Bits<16>,
    pub ip_ttl_proto: Bits<16>,
    pub ip_checksum: Bits<16>,
    pub ip_src: Bits<32>,
    pub ip_dst: Bits<32>,
    pub icmp_type_code: Bits<16>,
    pub icmp_checksum: Bits<16>,
    /// Word sums over the IPv4 header and the ICMP message, unfolded.
    pub ip_sum: Bits<32>,
    pub icmp_sum: Bits<32>,
}

/// Counter values reported by a beacon, captured when it starts.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct Snapshot {
    pub rx_packets: Bits<32>,
    pub rx_queue_drops: Bits<32>,
    pub rx_proto_drops: Bits<32>,
    pub tx_drops: Bits<32>,
    pub arp_replies: Bits<32>,
    pub beacons: Bits<32>,
    pub fcs_errors: Bits<32>,
    pub icmp_replies: Bits<32>,
}

/// The TX frame being prepared, with the values it substitutes.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct Request {
    /// ARP template (reply or gratuitous) when set, beacon template otherwise.
    pub arp: bool,
    pub dst_mac: Bits<48>,
    pub peer_mac: Bits<48>,
    pub peer_ip: Bits<32>,
    /// Echo reply: new IPv4 and ICMP checksums, and the frame body length.
    pub ip_checksum: Bits<16>,
    pub icmp_checksum: Bits<16>,
    pub len: Bits<11>,
    pub snapshot: Snapshot,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct Counters {
    pub rx_packets: Bits<32>,
    pub rx_proto_drops: Bits<32>,
    pub tx_drops: Bits<32>,
    pub arp_replies: Bits<32>,
    pub beacons: Bits<32>,
    pub icmp_replies: Bits<32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct ProtoState {
    pub phase: Bits<2>,
    pub index: Bits<11>,
    /// Consumer side of the RX ring.
    pub rx_rd_ptr: Bits<4>,
    /// Producer side of the TX ring.
    pub tx_wr_ptr: Bits<3>,
    pub beacon_timer: Bits<32>,
    pub garp_timer: Bits<32>,
    /// A timer expired and IDLE has not served it yet.
    pub beacon_pending: bool,
    pub garp_pending: bool,
    pub parsed: Parsed,
    pub request: Request,
    pub counters: Counters,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct ProtoIn {
    pub state: ProtoState,
    pub config: ProtoConfig,
    pub rx_wr_ptr: Bits<4>,
    pub rx_queue_drops: Bits<32>,
    pub rx_fcs_errors: Bits<32>,
    /// RX RAM byte at `{rx_rd_ptr, index}`.
    pub rx_rdata: Bits<8>,
    /// RX metadata `{truncated, body_len}` for `rx_rd_ptr`.
    pub rx_meta: Bits<12>,
    /// Template ROM entry at `{request.arp, index}`.
    pub rom_rdata: Bits<9>,
    pub tx_rd_ptr: Bits<3>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct ProtoOut {
    pub state: ProtoState,
    pub rx_raddr: Bits<14>,
    pub rx_meta_raddr: Bits<3>,
    pub rom_addr: Bits<9>,
    pub tx_we: bool,
    pub tx_waddr: Bits<13>,
    pub tx_wdata: Bits<8>,
    pub tx_meta_we: bool,
    pub tx_meta_waddr: Bits<2>,
    pub tx_meta_wdata: Bits<11>,
}

/// `parsing && lo <= i < hi`: the current byte belongs to a field.
#[logic]
fn parse_at(parsing: bool, i: Bits<11>, lo: Bits<11>, hi: Bits<11>) -> bool {
    parsing && within(i, lo, hi)
}

/// Byte `index` of the echo reply, given byte `index` of the request.
#[logic]
pub fn echo_reply_byte(
    index: Bits<11>,
    request_byte: Bits<8>,
    r: Request,
    mac: Bits<48>,
    ip: Bits<32>,
) -> Bits<8> {
    let low4 = index.slice::<4>(0);
    if index < bits::<11>(6) {
        byte_of48(r.dst_mac, low4)
    } else if index < bits::<11>(12) {
        byte_of48(mac, (index - bits::<11>(6)).slice::<4>(0))
    } else if index == bits::<11>(22) {
        bits::<8>(64)
    } else if index == bits::<11>(24) {
        r.ip_checksum.slice::<8>(8)
    } else if index == bits::<11>(25) {
        r.ip_checksum.slice::<8>(0)
    } else if within(index, bits::<11>(26), bits::<11>(30)) {
        byte_of32(ip, (index - bits::<11>(26)).slice::<2>(0))
    } else if within(index, bits::<11>(30), bits::<11>(34)) {
        byte_of32(r.peer_ip, (index - bits::<11>(30)).slice::<2>(0))
    } else if index == bits::<11>(34) {
        bits::<8>(0)
    } else if index == bits::<11>(36) {
        r.icmp_checksum.slice::<8>(8)
    } else if index == bits::<11>(37) {
        r.icmp_checksum.slice::<8>(0)
    } else {
        request_byte
    }
}

/// Byte of the selected ROM template, with dynamic entries resolved.
/// Bit 8 marks a dynamic byte whose source is bits 7:4 and whose position is
/// bits 3:0 (see `rom.rs`).
#[logic]
pub fn template_byte(rom: Bits<9>, r: Request) -> Bits<8> {
    let source = rom.slice::<4>(4);
    let position = rom.slice::<4>(0);
    let t = r.snapshot;
    let word = if source == bits::<4>(0) {
        r.dst_mac
    } else if source == bits::<4>(1) {
        r.peer_mac
    } else {
        r.peer_ip.concat::<16, 48>(bits::<16>(0))
    };
    let counter = if source == bits::<4>(3) {
        t.rx_packets
    } else if source == bits::<4>(4) {
        t.rx_queue_drops
    } else if source == bits::<4>(5) {
        t.rx_proto_drops
    } else if source == bits::<4>(6) {
        t.tx_drops
    } else if source == bits::<4>(7) {
        t.arp_replies
    } else if source == bits::<4>(8) {
        t.beacons
    } else if source == bits::<4>(9) {
        t.fcs_errors
    } else {
        t.icmp_replies
    };
    if !rom.bit(8) {
        rom.slice::<8>(0)
    } else if source < bits::<4>(3) {
        byte_of48(word, position)
    } else {
        hex_digit(nibble_of32(counter, position))
    }
}

#[logic]
pub fn proto_step(input: ProtoIn) -> ProtoOut {
    let s = input.state;
    let c = input.config;
    let p = s.parsed;
    let r = s.request;
    let n = s.counters;
    let i = s.index;

    let idle = s.phase == bits::<2>(0);
    let parsing = s.phase == bits::<2>(1);
    let preparing = s.phase == bits::<2>(2);
    let copying = s.phase == bits::<2>(3);

    // Free-running interval timers. A tick stays pending until IDLE serves it.
    let beacon_tick = s.beacon_timer == c.beacon_interval - bits::<32>(1);
    let garp_tick = s.garp_timer == c.garp_interval - bits::<32>(1);
    let beacon_due = s.beacon_pending || beacon_tick;
    let garp_due = s.garp_pending || garp_tick;

    let rx_ready = s.rx_rd_ptr != input.rx_wr_ptr;
    let tx_full = s.tx_wr_ptr - input.tx_rd_ptr == bits::<3>(4);

    // IDLE arbitration: RX first, then gratuitous ARP, then beacon.
    let pop = idle && rx_ready;
    let want_garp = idle && !rx_ready && garp_due;
    let want_beacon = idle && !rx_ready && !garp_due && beacon_due;

    // PARSE: shift each byte into the fields that cover its offset.
    let b = input.rx_rdata;
    let truncated = input.rx_meta.bit(11);
    let body_len = input.rx_meta.slice::<11>(0);
    let total_len = p.ip_total_len;
    let icmp_end = bits::<16>(14) + total_len;
    let i16 = bits::<5>(0).concat::<11, 16>(i);
    let in_ip_header = parsing && within(i, bits::<11>(14), bits::<11>(34));
    let in_icmp = parsing && i >= bits::<11>(34) && i16 < icmp_end;
    let parsed = Parsed {
        dst_mac: shift_in48(
            p.dst_mac,
            b,
            parse_at(parsing, i, bits::<11>(0), bits::<11>(6)),
        ),
        src_mac: shift_in48(
            p.src_mac,
            b,
            parse_at(parsing, i, bits::<11>(6), bits::<11>(12)),
        ),
        ethertype: shift_in16(
            p.ethertype,
            b,
            parse_at(parsing, i, bits::<11>(12), bits::<11>(14)),
        ),
        hw_type: shift_in16(
            p.hw_type,
            b,
            parse_at(parsing, i, bits::<11>(14), bits::<11>(16)),
        ),
        proto_type: shift_in16(
            p.proto_type,
            b,
            parse_at(parsing, i, bits::<11>(16), bits::<11>(18)),
        ),
        hw_len: if parsing && i == bits::<11>(18) {
            b
        } else {
            p.hw_len
        },
        proto_len: if parsing && i == bits::<11>(19) {
            b
        } else {
            p.proto_len
        },
        opcode: shift_in16(
            p.opcode,
            b,
            parse_at(parsing, i, bits::<11>(20), bits::<11>(22)),
        ),
        sender_mac: shift_in48(
            p.sender_mac,
            b,
            parse_at(parsing, i, bits::<11>(22), bits::<11>(28)),
        ),
        sender_ip: shift_in32(
            p.sender_ip,
            b,
            parse_at(parsing, i, bits::<11>(28), bits::<11>(32)),
        ),
        target_ip: shift_in32(
            p.target_ip,
            b,
            parse_at(parsing, i, bits::<11>(38), bits::<11>(42)),
        ),
        ip_ver_ihl: if parsing && i == bits::<11>(14) {
            b
        } else {
            p.ip_ver_ihl
        },
        ip_total_len: shift_in16(
            p.ip_total_len,
            b,
            parse_at(parsing, i, bits::<11>(16), bits::<11>(18)),
        ),
        ip_frag: shift_in16(
            p.ip_frag,
            b,
            parse_at(parsing, i, bits::<11>(20), bits::<11>(22)),
        ),
        ip_ttl_proto: shift_in16(
            p.ip_ttl_proto,
            b,
            parse_at(parsing, i, bits::<11>(22), bits::<11>(24)),
        ),
        ip_checksum: shift_in16(
            p.ip_checksum,
            b,
            parse_at(parsing, i, bits::<11>(24), bits::<11>(26)),
        ),
        ip_src: shift_in32(
            p.ip_src,
            b,
            parse_at(parsing, i, bits::<11>(26), bits::<11>(30)),
        ),
        ip_dst: shift_in32(
            p.ip_dst,
            b,
            parse_at(parsing, i, bits::<11>(30), bits::<11>(34)),
        ),
        icmp_type_code: shift_in16(
            p.icmp_type_code,
            b,
            parse_at(parsing, i, bits::<11>(34), bits::<11>(36)),
        ),
        icmp_checksum: shift_in16(
            p.icmp_checksum,
            b,
            parse_at(parsing, i, bits::<11>(36), bits::<11>(38)),
        ),
        ip_sum: if pop {
            bits::<32>(0)
        } else if in_ip_header {
            p.ip_sum + word_part(b, i)
        } else {
            p.ip_sum
        },
        icmp_sum: if pop {
            bits::<32>(0)
        } else if in_icmp {
            p.icmp_sum + word_part(b, i)
        } else {
            p.icmp_sum
        },
    };

    // PARSE decision once every byte has been read (at least 42 for ARP),
    // using fully captured registered fields.
    let parse_end = if body_len > bits::<11>(42) {
        body_len
    } else {
        bits::<11>(42)
    };
    let parse_done = parsing && i == parse_end;
    let dest_ok = p.dst_mac == c.mac || p.dst_mac == bits::<48>(0xffff_ffff_ffff);
    let arp_ok = !truncated
        && dest_ok
        && p.ethertype == bits::<16>(0x0806)
        && body_len >= bits::<11>(42)
        && p.hw_type == bits::<16>(1)
        && p.proto_type == bits::<16>(0x0800)
        && p.hw_len == bits::<8>(6)
        && p.proto_len == bits::<8>(4)
        && p.opcode == bits::<16>(1)
        && p.target_ip == c.ip;
    // Echo request: unicast to us, IPv4 without options or fragmentation,
    // both checksums valid, and the whole datagram inside the frame.
    let body16 = bits::<5>(0).concat::<11, 16>(body_len);
    let echo_ok = !truncated
        && p.dst_mac == c.mac
        && p.ethertype == bits::<16>(0x0800)
        && p.ip_ver_ihl == bits::<8>(0x45)
        && total_len >= bits::<16>(28)
        && icmp_end <= body16
        && (p.ip_frag & bits::<16>(0x3fff)) == bits::<16>(0)
        && p.ip_ttl_proto.slice::<8>(0) == bits::<8>(1)
        && p.ip_dst == c.ip
        && fold16(p.ip_sum) == bits::<16>(0xffff)
        && p.icmp_type_code == bits::<16>(0x0800)
        && fold16(p.icmp_sum) == bits::<16>(0xffff);
    let want_reply = parse_done && arp_ok;
    let want_echo = parse_done && echo_ok;

    let want_tx = want_reply || want_echo || want_garp || want_beacon;
    let start_tx = want_tx && !tx_full;
    let tx_dropped = want_tx && tx_full;
    let start_echo = want_echo && !tx_full;

    let snapshot = Snapshot {
        rx_packets: n.rx_packets,
        rx_queue_drops: input.rx_queue_drops,
        rx_proto_drops: n.rx_proto_drops,
        tx_drops: n.tx_drops,
        arp_replies: n.arp_replies,
        beacons: n.beacons,
        fcs_errors: input.rx_fcs_errors,
        icmp_replies: n.icmp_replies,
    };
    let request = if start_tx {
        if want_beacon {
            Request {
                arp: false,
                dst_mac: bits::<48>(0xffff_ffff_ffff),
                peer_mac: bits::<48>(0),
                peer_ip: bits::<32>(0),
                ip_checksum: bits::<16>(0),
                icmp_checksum: bits::<16>(0),
                len: bits::<11>(0),
                snapshot,
            }
        } else if want_garp {
            Request {
                arp: true,
                dst_mac: bits::<48>(0xffff_ffff_ffff),
                peer_mac: bits::<48>(0xffff_ffff_ffff),
                peer_ip: c.ip,
                ip_checksum: bits::<16>(0),
                icmp_checksum: bits::<16>(0),
                len: bits::<11>(0),
                snapshot,
            }
        } else if want_echo {
            // Only TTL/protocol and the ICMP type change; swapping the
            // addresses leaves both sums unchanged.
            Request {
                arp: false,
                dst_mac: p.src_mac,
                peer_mac: p.src_mac,
                peer_ip: p.ip_src,
                ip_checksum: checksum_update(p.ip_checksum, p.ip_ttl_proto, bits::<16>(0x4001)),
                icmp_checksum: checksum_update(
                    p.icmp_checksum,
                    bits::<16>(0x0800),
                    bits::<16>(0x0000),
                ),
                len: icmp_end.slice::<11>(0),
                snapshot,
            }
        } else {
            Request {
                arp: true,
                dst_mac: p.sender_mac,
                peer_mac: p.sender_mac,
                peer_ip: p.sender_ip,
                ip_checksum: bits::<16>(0),
                icmp_checksum: bits::<16>(0),
                len: bits::<11>(0),
                snapshot,
            }
        }
    } else {
        r
    };

    let prep_len = if r.arp {
        bits::<11>(42)
    } else {
        bits::<11>(170)
    };
    let prep_last = preparing && i == prep_len - bits::<11>(1);
    let copy_last = copying && i == r.len - bits::<11>(1);

    let next_phase = if idle {
        if pop {
            bits::<2>(1)
        } else if start_tx {
            bits::<2>(2)
        } else {
            bits::<2>(0)
        }
    } else if parsing {
        if parse_done {
            if start_echo {
                bits::<2>(3)
            } else if start_tx {
                bits::<2>(2)
            } else {
                bits::<2>(0)
            }
        } else {
            bits::<2>(1)
        }
    } else if preparing {
        if prep_last {
            bits::<2>(0)
        } else {
            bits::<2>(2)
        }
    } else if copy_last {
        bits::<2>(0)
    } else {
        bits::<2>(3)
    };
    let next_index = if idle || parse_done || prep_last || copy_last {
        bits::<11>(0)
    } else {
        i + bits::<11>(1)
    };
    // The RX slot is released after parsing, or after the echo copy.
    let release_rx = (parse_done && !start_echo) || copy_last;
    let next_rx_rd_ptr = if release_rx {
        s.rx_rd_ptr + bits::<4>(1)
    } else {
        s.rx_rd_ptr
    };
    let publish_tx = prep_last || copy_last;
    let next_tx_wr_ptr = if publish_tx {
        s.tx_wr_ptr + bits::<3>(1)
    } else {
        s.tx_wr_ptr
    };

    let counters = Counters {
        rx_packets: bump32(n.rx_packets, pop),
        rx_proto_drops: bump32(n.rx_proto_drops, parse_done && !arp_ok && !echo_ok),
        tx_drops: bump32(n.tx_drops, tx_dropped),
        arp_replies: bump32(n.arp_replies, want_reply),
        beacons: bump32(n.beacons, want_beacon && start_tx),
        icmp_replies: bump32(n.icmp_replies, want_echo),
    };

    let next = ProtoState {
        phase: next_phase,
        index: next_index,
        rx_rd_ptr: next_rx_rd_ptr,
        tx_wr_ptr: next_tx_wr_ptr,
        beacon_timer: if beacon_tick {
            bits::<32>(0)
        } else {
            s.beacon_timer + bits::<32>(1)
        },
        garp_timer: if garp_tick {
            bits::<32>(0)
        } else {
            s.garp_timer + bits::<32>(1)
        },
        beacon_pending: beacon_due && !want_beacon,
        garp_pending: garp_due && !want_garp,
        parsed,
        request,
        counters,
    };
    let arp_bit = if next.request.arp {
        bits::<1>(1)
    } else {
        bits::<1>(0)
    };

    ProtoOut {
        state: next,
        rx_raddr: next.rx_rd_ptr.slice::<3>(0).concat::<11, 14>(next.index),
        rx_meta_raddr: next.rx_rd_ptr.slice::<3>(0),
        rom_addr: arp_bit.concat::<8, 9>(next.index.slice::<8>(0)),
        tx_we: preparing || copying,
        tx_waddr: s.tx_wr_ptr.slice::<2>(0).concat::<11, 13>(i),
        tx_wdata: if copying {
            echo_reply_byte(i, b, r, c.mac, c.ip)
        } else {
            template_byte(input.rom_rdata, r)
        },
        tx_meta_we: publish_tx,
        tx_meta_waddr: s.tx_wr_ptr.slice::<2>(0),
        tx_meta_wdata: if copying { r.len } else { prep_len },
    }
}
