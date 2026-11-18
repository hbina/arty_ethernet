//! Protocol handling: consume RX frames, answer ARP, emit periodic beacons and
//! gratuitous ARP. Mirrors `ethernet_protocol_handler.h` and
//! `ethernet_tx_payloads.h` at the frame level.
//!
//! Phases:
//! - `IDLE` (0): pop an RX frame, else start a gratuitous ARP, else a beacon.
//!   Timer ticks that arrive while busy are lost, exactly as in the HLS design.
//! - `PARSE` (1): read RX bytes 0..=41 (header + ARP body) one per cycle, then
//!   decide at index 42.
//! - `PREP` (2): copy the selected template from the ROM into the TX slot one
//!   byte per cycle, substituting dynamic fields, then publish its length.
//!
//! Memory reads have one cycle of latency. Every read address is derived from
//! the *next* state, so during each cycle the read data always corresponds to
//! the current state's `index` and pointers.
use crate::ops::{
    bump32, byte_of48, hex_digit, nibble_of32, shift_in16, shift_in32, shift_in48, within,
};
use bitlogic::{Bits, LogicValue, bits, logic};

pub const PHASE_IDLE: u64 = 0;
pub const PHASE_PARSE: u64 = 1;
pub const PHASE_PREP: u64 = 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct ProtoConfig {
    pub mac: Bits<48>,
    pub ip: Bits<32>,
    pub beacon_interval: Bits<32>,
    pub garp_interval: Bits<32>,
}

/// Fields captured from the RX frame being parsed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct Parsed {
    pub dst_mac: Bits<48>,
    pub ethertype: Bits<16>,
    pub hw_type: Bits<16>,
    pub proto_type: Bits<16>,
    pub hw_len: Bits<8>,
    pub proto_len: Bits<8>,
    pub opcode: Bits<16>,
    pub sender_mac: Bits<48>,
    pub sender_ip: Bits<32>,
    pub target_ip: Bits<32>,
}

/// The TX frame being prepared, with the values its template substitutes.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct Request {
    /// ARP template (reply or gratuitous) when set, beacon template otherwise.
    pub arp: bool,
    pub dst_mac: Bits<48>,
    pub peer_mac: Bits<48>,
    pub peer_ip: Bits<32>,
    pub rx_packets: Bits<32>,
    pub rx_queue_drops: Bits<32>,
    pub rx_proto_drops: Bits<32>,
    pub tx_drops: Bits<32>,
    pub arp_replies: Bits<32>,
    pub beacons: Bits<32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct Counters {
    pub rx_packets: Bits<32>,
    pub rx_proto_drops: Bits<32>,
    pub tx_drops: Bits<32>,
    pub arp_replies: Bits<32>,
    pub beacons: Bits<32>,
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

    // Free-running interval timers; a tick is used only if IDLE takes it.
    let beacon_tick = s.beacon_timer == c.beacon_interval - bits::<32>(1);
    let garp_tick = s.garp_timer == c.garp_interval - bits::<32>(1);

    let rx_ready = s.rx_rd_ptr != input.rx_wr_ptr;
    let tx_full = s.tx_wr_ptr - input.tx_rd_ptr == bits::<3>(4);

    // IDLE arbitration: RX first, then gratuitous ARP, then beacon.
    let pop = idle && rx_ready;
    let want_garp = idle && !rx_ready && garp_tick;
    let want_beacon = idle && !rx_ready && !garp_tick && beacon_tick;

    // PARSE: shift each byte into the field that covers its index.
    let b = input.rx_rdata;
    let parsed = Parsed {
        dst_mac: shift_in48(p.dst_mac, b, parsing && i < bits::<11>(6)),
        ethertype: shift_in16(
            p.ethertype,
            b,
            parsing && within(i, bits::<11>(12), bits::<11>(14)),
        ),
        hw_type: shift_in16(
            p.hw_type,
            b,
            parsing && within(i, bits::<11>(14), bits::<11>(16)),
        ),
        proto_type: shift_in16(
            p.proto_type,
            b,
            parsing && within(i, bits::<11>(16), bits::<11>(18)),
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
            parsing && within(i, bits::<11>(20), bits::<11>(22)),
        ),
        sender_mac: shift_in48(
            p.sender_mac,
            b,
            parsing && within(i, bits::<11>(22), bits::<11>(28)),
        ),
        sender_ip: shift_in32(
            p.sender_ip,
            b,
            parsing && within(i, bits::<11>(28), bits::<11>(32)),
        ),
        target_ip: shift_in32(
            p.target_ip,
            b,
            parsing && within(i, bits::<11>(38), bits::<11>(42)),
        ),
    };

    // PARSE decision at index 42, using fully captured registered fields.
    let parse_done = parsing && i == bits::<11>(42);
    let truncated = input.rx_meta.bit(11);
    let body_len = input.rx_meta.slice::<11>(0);
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
    let want_reply = parse_done && arp_ok;

    let want_tx = want_reply || want_garp || want_beacon;
    let start_tx = want_tx && !tx_full;
    let tx_dropped = want_tx && tx_full;

    let request = if start_tx {
        if want_beacon {
            Request {
                arp: false,
                dst_mac: bits::<48>(0xffff_ffff_ffff),
                peer_mac: bits::<48>(0),
                peer_ip: bits::<32>(0),
                rx_packets: n.rx_packets,
                rx_queue_drops: input.rx_queue_drops,
                rx_proto_drops: n.rx_proto_drops,
                tx_drops: n.tx_drops,
                arp_replies: n.arp_replies,
                beacons: n.beacons,
            }
        } else if want_garp {
            Request {
                arp: true,
                dst_mac: bits::<48>(0xffff_ffff_ffff),
                peer_mac: bits::<48>(0xffff_ffff_ffff),
                peer_ip: c.ip,
                rx_packets: bits::<32>(0),
                rx_queue_drops: bits::<32>(0),
                rx_proto_drops: bits::<32>(0),
                tx_drops: bits::<32>(0),
                arp_replies: bits::<32>(0),
                beacons: bits::<32>(0),
            }
        } else {
            Request {
                arp: true,
                dst_mac: p.sender_mac,
                peer_mac: p.sender_mac,
                peer_ip: p.sender_ip,
                rx_packets: bits::<32>(0),
                rx_queue_drops: bits::<32>(0),
                rx_proto_drops: bits::<32>(0),
                tx_drops: bits::<32>(0),
                arp_replies: bits::<32>(0),
                beacons: bits::<32>(0),
            }
        }
    } else {
        r
    };

    // PREP: resolve the ROM entry. Bit 8 marks a dynamic byte whose source is
    // bits 7:4 and whose position is bits 3:0 (see `rom.rs`).
    let rom = input.rom_rdata;
    let dynamic = rom.bit(8);
    let source = rom.slice::<4>(4);
    let position = rom.slice::<4>(0);
    let word = if source == bits::<4>(0) {
        r.dst_mac
    } else if source == bits::<4>(1) {
        r.peer_mac
    } else {
        r.peer_ip.concat::<16, 48>(bits::<16>(0))
    };
    let counter = if source == bits::<4>(3) {
        r.rx_packets
    } else if source == bits::<4>(4) {
        r.rx_queue_drops
    } else if source == bits::<4>(5) {
        r.rx_proto_drops
    } else if source == bits::<4>(6) {
        r.tx_drops
    } else if source == bits::<4>(7) {
        r.arp_replies
    } else {
        r.beacons
    };
    let template_byte = if !dynamic {
        rom.slice::<8>(0)
    } else if source < bits::<4>(3) {
        byte_of48(word, position)
    } else {
        hex_digit(nibble_of32(counter, position))
    };
    let prep_len = if r.arp {
        bits::<11>(42)
    } else {
        bits::<11>(143)
    };
    let prep_last = preparing && i == prep_len - bits::<11>(1);

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
            if start_tx { bits::<2>(2) } else { bits::<2>(0) }
        } else {
            bits::<2>(1)
        }
    } else if preparing {
        if prep_last {
            bits::<2>(0)
        } else {
            bits::<2>(2)
        }
    } else {
        bits::<2>(0)
    };
    let next_index = if idle || parse_done || prep_last || !(parsing || preparing) {
        bits::<11>(0)
    } else {
        i + bits::<11>(1)
    };
    let next_rx_rd_ptr = if parse_done {
        s.rx_rd_ptr + bits::<4>(1)
    } else {
        s.rx_rd_ptr
    };
    let next_tx_wr_ptr = if prep_last {
        s.tx_wr_ptr + bits::<3>(1)
    } else {
        s.tx_wr_ptr
    };

    let counters = Counters {
        rx_packets: bump32(n.rx_packets, pop),
        rx_proto_drops: bump32(n.rx_proto_drops, parse_done && !arp_ok),
        tx_drops: bump32(n.tx_drops, tx_dropped),
        arp_replies: bump32(n.arp_replies, want_reply),
        beacons: bump32(n.beacons, want_beacon && start_tx),
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
        tx_we: preparing,
        tx_waddr: s.tx_wr_ptr.slice::<2>(0).concat::<11, 13>(i),
        tx_wdata: template_byte,
        tx_meta_we: prep_last,
        tx_meta_waddr: s.tx_wr_ptr.slice::<2>(0),
        tx_meta_wdata: prep_len,
    }
}
