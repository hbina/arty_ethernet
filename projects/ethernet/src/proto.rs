//! Protocol handling: consume RX frames, answer ARP and ICMP echo, emit
//! periodic beacons and gratuitous ARP. ARP and beacons mirror
//! `ethernet_protocol_handler.h` and `ethernet_tx_payloads.h` at the frame
//! level; ICMP echo is new in this design.
//!
//! ARP senders are learned into a 16-entry peer cache using the RFC 826 merge
//! rules: a valid ARP request or reply whose sender IP is cached refreshes
//! that entry's MAC, and one whose target IP is ours adds the sender
//! (into a vacant slot, else replacing slots round-robin, which is oldest
//! first because entries are never removed). Sender IP 0.0.0.0
//! (an RFC 5227 probe) is never learned. Beacons report the cached count.
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
    bump32, byte_of32, byte_of32_le, byte_of48, byte_of64_le, checksum_update, fold16, hex_digit,
    nibble_of32, shift_in16, shift_in32, shift_in48, within, word_part,
};
use bitlogic::{Bits, LogicValue, bits, logic};

/// Protocol unit phase (see the module documentation). `Idle` is 0, the
/// reset value.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub enum Phase {
    Idle,
    Parse,
    Prep,
    Copy,
    /// Write a best bid/offer frame from the BBO FIFO into a TX slot.
    Bbo,
}

/// Beacon frame body length (`consts::BEACON_FRAME_BODY_LEN`).
const BEACON_LEN: Bits<11> = bits(crate::consts::BEACON_FRAME_BODY_LEN);
const BBO_HEADER: Bits<11> = bits(crate::consts::BBO_HEADER_LEN);

// ARP peer cache: sender IPv4 address to MAC address.
bitlogic::fixed_map!(pub mod peer_cache: Bits<32> => Bits<48>, capacity = 16);
pub const PEER_CACHE_ENTRIES: usize = peer_cache::CAPACITY;
pub type PeerCache = peer_cache::Map;

/// TTL written into echo replies.
pub const REPLY_TTL: u64 = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct ProtoConfig {
    pub mac: Bits<48>,
    pub ip: Bits<32>,
    pub beacon_interval: Bits<32>,
    pub garp_interval: Bits<32>,
    /// IEX feed: multicast group and UDP port (see `feed.rs`).
    pub feed_ip: Bits<32>,
    pub feed_port: Bits<16>,
    /// Destination MAC of best bid/offer frames.
    pub bbo_mac: Bits<48>,
}

/// Header values of the best bid/offer frame being written, captured when
/// it starts.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct BboFrame {
    /// Records in this frame (1..=46).
    pub count: Bits<6>,
    /// Sequence number of the first record: records sent before it.
    pub first: Bits<32>,
    pub feed_packets: Bits<32>,
    pub feed_gaps: Bits<32>,
    pub feed_malformed: Bits<32>,
    pub book_errors: Bits<32>,
    pub rx_queue_drops: Bits<32>,
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
    /// Valid peer cache entries.
    pub peers: Bits<32>,
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
    pub phase: Phase,
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
    pub peers: PeerCache,
    /// Consumer side of the BBO FIFO, and BBO records sent.
    pub bbo_rd_ptr: Bits<9>,
    pub bbo_sent: Bits<32>,
    pub bbo: BboFrame,
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
    /// Producer side of the BBO FIFO (the book unit).
    pub bbo_wr_ptr: Bits<9>,
    /// BBO FIFO word at `bbo_raddr` of the previous state.
    pub bbo_rdata: Bits<64>,
    /// Feed and book counters reported in BBO frame headers.
    pub feed_packets: Bits<32>,
    pub feed_gaps: Bits<32>,
    pub feed_malformed: Bits<32>,
    pub book_errors: Bits<32>,
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
    pub bbo_raddr: Bits<10>,
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
    // Sources 3..=11, in `rom::COUNTER_SOURCES` order.
    let counter = [
        t.rx_packets,
        t.rx_queue_drops,
        t.rx_proto_drops,
        t.tx_drops,
        t.arp_replies,
        t.beacons,
        t.fcs_errors,
        t.icmp_replies,
        t.peers,
    ]
    .at(source - bits::<4>(3));
    if !rom.bit(8) {
        rom.slice::<8>(0)
    } else if source < bits::<4>(3) {
        byte_of48(word, position)
    } else {
        hex_digit(nibble_of32(counter, position))
    }
}

/// BBO FIFO word holding frame byte `index` (40 and up) of the frame whose
/// first record is at `rd_ptr`: record `(index - 40) / 32`, word
/// `(index - 40) / 8 % 4`.
#[logic]
fn bbo_raddr(index: Bits<11>, rd_ptr: Bits<9>) -> Bits<10> {
    let j = index - BBO_HEADER;
    let record = rd_ptr + bits::<3>(0).concat::<6, 9>(j.slice::<6>(5));
    record.slice::<8>(0).concat::<2, 10>(j.slice::<2>(3))
}

/// Byte `index` of a best bid/offer frame (layout in `DESIGN.md`): header
/// fields from the captured values, records from the BBO FIFO word.
#[logic]
pub fn bbo_byte(index: Bits<11>, f: BboFrame, c: ProtoConfig, word: Bits<64>) -> Bits<8> {
    let low4 = index.slice::<4>(0);
    let le = index.slice::<2>(0);
    if index < bits::<11>(6) {
        byte_of48(c.bbo_mac, low4)
    } else if index < bits::<11>(12) {
        byte_of48(c.mac, (index - bits::<11>(6)).slice::<4>(0))
    } else if index == bits::<11>(12) {
        bits::<8>(0x88)
    } else if index == bits::<11>(13) {
        bits::<8>(0xB6)
    } else if index == bits::<11>(14) {
        bits::<8>(1)
    } else if index == bits::<11>(15) {
        bits::<2>(0).concat::<6, 8>(f.count)
    } else if index < bits::<11>(20) {
        byte_of32_le(f.first, le)
    } else if index < bits::<11>(24) {
        byte_of32_le(f.feed_packets, le)
    } else if index < bits::<11>(28) {
        byte_of32_le(f.feed_gaps, le)
    } else if index < bits::<11>(32) {
        byte_of32_le(f.feed_malformed, le)
    } else if index < bits::<11>(36) {
        byte_of32_le(f.book_errors, le)
    } else if index < bits::<11>(40) {
        byte_of32_le(f.rx_queue_drops, le)
    } else {
        byte_of64_le(word, index.slice::<3>(0))
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

    let idle = s.phase == Phase::Idle;
    let parsing = s.phase == Phase::Parse;
    let preparing = s.phase == Phase::Prep;
    let copying = s.phase == Phase::Copy;
    let bbo_writing = s.phase == Phase::Bbo;

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
    // Best bid/offer records wait in their FIFO until the TX ring has room.
    let bbo_avail = input.bbo_wr_ptr - s.bbo_rd_ptr;
    let want_bbo = idle && !rx_ready && !garp_due && !beacon_due && bbo_avail != bits::<9>(0);

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
    // IPv4 frames other than ICMP need no more than the first 42 bytes
    // (the IEX feed is read by the feed unit from its own RX copy).
    let ipv4_other =
        p.ethertype == bits::<16>(0x0800) && p.ip_ttl_proto.slice::<8>(0) != bits::<8>(1);
    let parse_done = parsing && (i == parse_end || (i == bits::<11>(42) && ipv4_other));
    // UDP to the feed group and port (bytes 36..=37 are captured as the
    // ICMP checksum field).
    let feed_frame = p.ethertype == bits::<16>(0x0800)
        && p.ip_ttl_proto.slice::<8>(0) == bits::<8>(17)
        && p.ip_dst == c.feed_ip
        && p.icmp_checksum == c.feed_port;
    let dest_ok = p.dst_mac == c.mac || p.dst_mac == bits::<48>(0xffff_ffff_ffff);
    // A well-formed Ethernet/IPv4 ARP request or reply.
    let arp_valid = !truncated
        && dest_ok
        && p.ethertype == bits::<16>(0x0806)
        && body_len >= bits::<11>(42)
        && p.hw_type == bits::<16>(1)
        && p.proto_type == bits::<16>(0x0800)
        && p.hw_len == bits::<8>(6)
        && p.proto_len == bits::<8>(4)
        && (p.opcode == bits::<16>(1) || p.opcode == bits::<16>(2));
    let arp_for_us = arp_valid && p.target_ip == c.ip;
    let arp_ok = arp_for_us && p.opcode == bits::<16>(1);
    let sender = peer_cache::find(s.peers, p.sender_ip);
    let known_sender = sender.hit;
    // The frame changed the cache (or would have, but for refreshing an
    // identical entry): it was used, not dropped.
    let learn = parse_done && arp_valid && p.sender_ip != bits::<32>(0);
    let arp_learns = arp_valid && p.sender_ip != bits::<32>(0) && (known_sender || arp_for_us);
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
    let start_bbo = want_bbo && !tx_full;
    let bbo_count = if bbo_avail > bits::<9>(46) {
        bits::<6>(46)
    } else {
        bbo_avail.slice::<6>(0)
    };
    let bbo_len = BBO_HEADER + (bits::<5>(0).concat::<6, 11>(s.bbo.count) << 5);
    let bbo_last = bbo_writing && i == bbo_len - bits::<11>(1);

    let snapshot = Snapshot {
        rx_packets: n.rx_packets,
        rx_queue_drops: input.rx_queue_drops,
        rx_proto_drops: n.rx_proto_drops,
        tx_drops: n.tx_drops,
        arp_replies: n.arp_replies,
        beacons: n.beacons,
        fcs_errors: input.rx_fcs_errors,
        icmp_replies: n.icmp_replies,
        peers: bits::<27>(0).concat::<5, 32>(peer_cache::count(s.peers)),
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

    let prep_len = if r.arp { bits::<11>(42) } else { BEACON_LEN };
    let prep_last = preparing && i == prep_len - bits::<11>(1);
    let copy_last = copying && i == r.len - bits::<11>(1);

    let next_phase = match s.phase {
        Phase::Idle => {
            if pop {
                Phase::Parse
            } else if start_tx {
                Phase::Prep
            } else if start_bbo {
                Phase::Bbo
            } else {
                Phase::Idle
            }
        }
        Phase::Parse => {
            if !parse_done {
                Phase::Parse
            } else if start_echo {
                Phase::Copy
            } else if start_tx {
                Phase::Prep
            } else {
                Phase::Idle
            }
        }
        Phase::Prep => {
            if prep_last {
                Phase::Idle
            } else {
                Phase::Prep
            }
        }
        Phase::Copy => {
            if copy_last {
                Phase::Idle
            } else {
                Phase::Copy
            }
        }
        Phase::Bbo => {
            if bbo_last {
                Phase::Idle
            } else {
                Phase::Bbo
            }
        }
    };
    let next_index = if idle || parse_done || prep_last || copy_last || bbo_last {
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
    let publish_tx = prep_last || copy_last || bbo_last;
    let next_tx_wr_ptr = if publish_tx {
        s.tx_wr_ptr + bits::<3>(1)
    } else {
        s.tx_wr_ptr
    };

    let counters = Counters {
        rx_packets: bump32(n.rx_packets, pop),
        rx_proto_drops: bump32(
            n.rx_proto_drops,
            parse_done && !arp_learns && !arp_ok && !echo_ok && !feed_frame,
        ),
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
        // RFC 826 merge: refresh a cached sender; add one only when the
        // request or reply is addressed to our IP.
        peers: peer_cache::store(
            s.peers,
            sender,
            learn,
            learn && arp_for_us,
            p.sender_ip,
            p.sender_mac,
        ),
        bbo_rd_ptr: if bbo_last {
            s.bbo_rd_ptr + bits::<3>(0).concat::<6, 9>(s.bbo.count)
        } else {
            s.bbo_rd_ptr
        },
        bbo_sent: if bbo_last {
            s.bbo_sent + bits::<26>(0).concat::<6, 32>(s.bbo.count)
        } else {
            s.bbo_sent
        },
        bbo: if start_bbo {
            BboFrame {
                count: bbo_count,
                first: s.bbo_sent,
                feed_packets: input.feed_packets,
                feed_gaps: input.feed_gaps,
                feed_malformed: input.feed_malformed,
                book_errors: input.book_errors,
                rx_queue_drops: input.rx_queue_drops,
            }
        } else {
            s.bbo
        },
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
        tx_we: preparing || copying || bbo_writing,
        tx_waddr: s.tx_wr_ptr.slice::<2>(0).concat::<11, 13>(i),
        tx_wdata: if copying {
            echo_reply_byte(i, b, r, c.mac, c.ip)
        } else if bbo_writing {
            bbo_byte(i, s.bbo, c, input.bbo_rdata)
        } else {
            template_byte(input.rom_rdata, r)
        },
        tx_meta_we: publish_tx,
        tx_meta_waddr: s.tx_wr_ptr.slice::<2>(0),
        tx_meta_wdata: if copying {
            r.len
        } else if bbo_writing {
            bbo_len
        } else {
            prep_len
        },
        bbo_raddr: bbo_raddr(next.index, next.bbo_rd_ptr),
    }
}
