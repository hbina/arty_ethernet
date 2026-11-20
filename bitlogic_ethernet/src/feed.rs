//! IEX DEEP feed parser: consume RX frames from a copy of the RX ring, accept
//! IEX-TP segments on the configured multicast group and UDP port, walk their
//! message blocks and queue every Price Level Update (PLU) for the order book.
//!
//! The unit reads its own copy of the RX RAM (`feed_rx_ram`), written by the RX
//! unit alongside the protocol unit's copy, and owns a second consumer
//! pointer into the RX ring. The RX unit treats the ring as full when either
//! consumer is eight frames behind, so a slow feed back-pressures the ring and
//! the RX unit drops (and counts) whole frames rather than corrupting one.
//!
//! Phases:
//! - `Idle` (0): wait for an RX frame.
//! - `Header`: read bytes 0..=81, one per cycle, capturing the Ethernet, IPv4,
//!   UDP and IEX-TP header fields. At byte 38 a frame that is not IPv4/UDP to
//!   the feed group and port is released. At byte 81 the rest is checked: no
//!   IPv4 options or fragments, a valid IPv4 header checksum, IEX-TP version 1,
//!   message protocol 0x8004, and the payload inside the frame.
//! - `Message`: walk the message blocks from byte 82. Each block's length and
//!   type are read (3 cycles); anything but a 30-byte PLU (`8` buy, `5` sell)
//!   is skipped by jumping the read index to the next block. A PLU reads its
//!   flags, skips the timestamp, then reads symbol, size and price (24 cycles in
//!   all) and is written into the PLU FIFO on its last byte. A full FIFO
//!   stalls the walk before the PLU starts.
//!
//! IEX-TP fields are little-endian; Ethernet, IPv4 and UDP fields big-endian.
//! Sequence numbers are tracked across segments: a segment whose first
//! sequence number differs from the one expected after the previous segment
//! counts a gap (the first segment seen only sets the expectation).
use crate::ops::{
    bump32, fold16, shift_in_le16, shift_in_le32, shift_in_le64, shift_in16, shift_in32, within,
    word_part,
};
use crate::proto::ProtoConfig;
use bitlogic::{Bits, LogicValue, bits, logic};

/// `consts::FEED_PAYLOAD_START`.
const PAYLOAD_START: Bits<11> = bits(crate::consts::FEED_PAYLOAD_START);
const PROTOCOL_ID: Bits<16> = bits(crate::consts::DEEP_PROTOCOL_ID);

/// PLU FIFO depth; pointers carry one wrap bit.
pub const PLU_FIFO_DEPTH: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub enum FeedPhase {
    Idle,
    Header,
    Message,
}

/// Header fields captured while reading bytes 0..=81.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct FeedHeader {
    pub ethertype: Bits<16>,
    pub ver_ihl: Bits<8>,
    pub frag: Bits<16>,
    pub protocol: Bits<8>,
    /// Unfolded word sum over the IPv4 header (bytes 14..=33).
    pub ip_sum: Bits<32>,
    pub dst_ip: Bits<32>,
    pub dst_port: Bits<16>,
    pub tp_version: Bits<8>,
    pub protocol_id: Bits<16>,
    pub payload_len: Bits<16>,
    pub msg_count: Bits<16>,
    pub first_seq: Bits<64>,
}

/// The message block being read.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct FeedMessage {
    pub len: Bits<16>,
    pub sell: bool,
    pub flags: Bits<8>,
    pub symbol: Bits<64>,
    pub size: Bits<32>,
    pub price: Bits<64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct FeedCounters {
    /// IEX-TP segments accepted (heartbeats included).
    pub packets: Bits<32>,
    /// Message blocks walked.
    pub messages: Bits<32>,
    /// PLUs queued for the book.
    pub plus: Bits<32>,
    /// Segments whose first sequence number was not the expected one.
    pub seq_gaps: Bits<32>,
    /// Frames to the feed group and port that failed a header check, and
    /// segments whose message blocks overran the payload or disagreed with
    /// the message count.
    pub malformed: Bits<32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct FeedState {
    pub phase: FeedPhase,
    /// Frame byte offset being read this cycle.
    pub index: Bits<11>,
    /// This unit's consumer pointer into the RX ring.
    pub rx_rd_ptr: Bits<4>,
    /// Start of the current message block, and end of the payload.
    pub pos: Bits<11>,
    pub payload_end: Bits<11>,
    pub msgs_left: Bits<16>,
    pub header: FeedHeader,
    pub msg: FeedMessage,
    pub expected_seq: Bits<64>,
    pub seq_valid: bool,
    /// Producer side of the PLU FIFO.
    pub plu_wr_ptr: Bits<7>,
    pub counters: FeedCounters,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct FeedIn {
    pub state: FeedState,
    pub config: ProtoConfig,
    pub rx_wr_ptr: Bits<4>,
    /// Feed copy of the RX RAM: the byte at `{rx_rd_ptr, index}`.
    pub rx_rdata: Bits<8>,
    /// Feed copy of the RX metadata `{truncated, body_len}` for `rx_rd_ptr`.
    pub rx_meta: Bits<12>,
    /// Consumer side of the PLU FIFO (the book unit).
    pub plu_rd_ptr: Bits<7>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct FeedOut {
    pub state: FeedState,
    pub rx_raddr: Bits<14>,
    pub rx_meta_raddr: Bits<3>,
    /// PLU FIFO entry: three RAMs written together.
    pub plu_we: bool,
    pub plu_waddr: Bits<6>,
    pub plu_symbol: Bits<64>,
    pub plu_price: Bits<64>,
    /// `{sell, event, size[31:0]}`.
    pub plu_misc: Bits<34>,
}

/// `active && lo <= i < hi`.
#[logic]
fn at(active: bool, i: Bits<11>, lo: Bits<11>, hi: Bits<11>) -> bool {
    active && within(i, lo, hi)
}

#[logic]
fn widen11(v: Bits<11>) -> Bits<17> {
    bits::<6>(0).concat::<11, 17>(v)
}

#[logic]
fn widen16(v: Bits<16>) -> Bits<17> {
    bits::<1>(0).concat::<16, 17>(v)
}

#[logic]
pub fn feed_step(input: FeedIn) -> FeedOut {
    let s = input.state;
    let c = input.config;
    let h = s.header;
    let m = s.msg;
    let i = s.index;
    let b = input.rx_rdata;

    let idle = s.phase == FeedPhase::Idle;
    let in_header = s.phase == FeedPhase::Header;
    let in_msg = s.phase == FeedPhase::Message;

    let rx_ready = s.rx_rd_ptr != input.rx_wr_ptr;
    let truncated = input.rx_meta.bit(11);
    let body_len = input.rx_meta.slice::<11>(0);

    // Header capture.
    let header = FeedHeader {
        ethertype: shift_in16(
            h.ethertype,
            b,
            at(in_header, i, bits::<11>(12), bits::<11>(14)),
        ),
        ver_ihl: if in_header && i == bits::<11>(14) {
            b
        } else {
            h.ver_ihl
        },
        frag: shift_in16(h.frag, b, at(in_header, i, bits::<11>(20), bits::<11>(22))),
        protocol: if in_header && i == bits::<11>(23) {
            b
        } else {
            h.protocol
        },
        ip_sum: if idle {
            bits::<32>(0)
        } else if at(in_header, i, bits::<11>(14), bits::<11>(34)) {
            h.ip_sum + word_part(b, i)
        } else {
            h.ip_sum
        },
        dst_ip: shift_in32(
            h.dst_ip,
            b,
            at(in_header, i, bits::<11>(30), bits::<11>(34)),
        ),
        dst_port: shift_in16(
            h.dst_port,
            b,
            at(in_header, i, bits::<11>(36), bits::<11>(38)),
        ),
        tp_version: if in_header && i == bits::<11>(42) {
            b
        } else {
            h.tp_version
        },
        protocol_id: shift_in_le16(
            h.protocol_id,
            b,
            at(in_header, i, bits::<11>(44), bits::<11>(46)),
        ),
        payload_len: shift_in_le16(
            h.payload_len,
            b,
            at(in_header, i, bits::<11>(54), bits::<11>(56)),
        ),
        msg_count: shift_in_le16(
            h.msg_count,
            b,
            at(in_header, i, bits::<11>(56), bits::<11>(58)),
        ),
        first_seq: shift_in_le64(
            h.first_seq,
            b,
            at(in_header, i, bits::<11>(66), bits::<11>(74)),
        ),
    };

    // Header decisions use registered fields only.
    let addressed = h.ethertype == bits::<16>(0x0800)
        && h.protocol == bits::<8>(17)
        && h.dst_ip == c.feed_ip
        && h.dst_port == c.feed_port;
    let reject_early = in_header && i == bits::<11>(38) && !addressed;
    let header_done = in_header && i == bits::<11>(81);
    let payload_end17 = widen16(h.payload_len) + widen11(PAYLOAD_START);
    let valid = !truncated
        && addressed
        && h.ver_ihl == bits::<8>(0x45)
        && (h.frag & bits::<16>(0x3fff)) == bits::<16>(0)
        && fold16(h.ip_sum) == bits::<16>(0xffff)
        && h.tp_version == bits::<8>(1)
        && h.protocol_id == PROTOCOL_ID
        && payload_end17 <= widen11(body_len);
    let accept = header_done && valid;
    let bad_header = header_done && !valid;
    let has_msgs = h.msg_count != bits::<16>(0) && h.payload_len != bits::<16>(0);
    let gap = accept && s.seq_valid && h.first_seq != s.expected_seq;

    // Message walk. `k` is the offset inside the current block.
    let k = i - s.pos;
    let msg = FeedMessage {
        len: shift_in_le16(m.len, b, in_msg && k < bits::<11>(2)),
        sell: if in_msg && k == bits::<11>(2) {
            b == bits::<8>(0x35)
        } else {
            m.sell
        },
        flags: if in_msg && k == bits::<11>(3) {
            b
        } else {
            m.flags
        },
        symbol: shift_in_le64(m.symbol, b, at(in_msg, k, bits::<11>(12), bits::<11>(20))),
        size: shift_in_le32(m.size, b, at(in_msg, k, bits::<11>(20), bits::<11>(24))),
        price: shift_in_le64(m.price, b, at(in_msg, k, bits::<11>(24), bits::<11>(32))),
    };
    let type_cycle = in_msg && k == bits::<11>(2);
    let msg_end17 = widen11(s.pos) + bits::<17>(2) + widen16(m.len);
    let overrun = type_cycle && msg_end17 > widen11(s.payload_end);
    let is_plu = (b == bits::<8>(0x38) || b == bits::<8>(0x35)) && m.len == bits::<16>(30);
    let plu_full = s.plu_wr_ptr - input.plu_rd_ptr == bits::<7>(64);
    let stall = type_cycle && !overrun && is_plu && plu_full;
    let skip = type_cycle && !overrun && !is_plu;
    let plu_last = in_msg && k == bits::<11>(31);
    let msg_done = skip || plu_last;
    let msg_end = msg_end17.slice::<11>(0);
    let last_counted = s.msgs_left == bits::<16>(1);
    let payload_consumed = msg_end == s.payload_end;
    let frame_end = msg_done && (last_counted || payload_consumed);
    let count_mismatch = msg_done && last_counted != payload_consumed;

    let release = reject_early || bad_header || (accept && !has_msgs) || overrun || frame_end;

    let next_phase = match s.phase {
        FeedPhase::Idle => {
            if rx_ready {
                FeedPhase::Header
            } else {
                FeedPhase::Idle
            }
        }
        FeedPhase::Header => {
            if release {
                FeedPhase::Idle
            } else if accept {
                FeedPhase::Message
            } else {
                FeedPhase::Header
            }
        }
        FeedPhase::Message => {
            if release {
                FeedPhase::Idle
            } else {
                FeedPhase::Message
            }
        }
    };
    let next_index = if idle || release {
        bits::<11>(0)
    } else if accept {
        PAYLOAD_START
    } else if stall {
        i
    } else if msg_done {
        msg_end
    } else if in_msg && k == bits::<11>(3) {
        // Skip the 8-byte timestamp.
        s.pos + bits::<11>(12)
    } else {
        i + bits::<11>(1)
    };
    let next_rx_rd_ptr = if release {
        s.rx_rd_ptr + bits::<4>(1)
    } else {
        s.rx_rd_ptr
    };

    let n = s.counters;
    let counters = FeedCounters {
        packets: bump32(n.packets, accept),
        messages: bump32(n.messages, msg_done),
        plus: bump32(n.plus, plu_last),
        seq_gaps: bump32(n.seq_gaps, gap),
        malformed: bump32(n.malformed, bad_header || overrun || count_mismatch),
    };

    let next = FeedState {
        phase: next_phase,
        index: next_index,
        rx_rd_ptr: next_rx_rd_ptr,
        pos: if accept {
            PAYLOAD_START
        } else if msg_done {
            msg_end
        } else {
            s.pos
        },
        payload_end: if accept {
            payload_end17.slice::<11>(0)
        } else {
            s.payload_end
        },
        msgs_left: if accept {
            h.msg_count
        } else if msg_done {
            s.msgs_left - bits::<16>(1)
        } else {
            s.msgs_left
        },
        header,
        msg,
        expected_seq: if accept {
            h.first_seq + bits::<48>(0).concat::<16, 64>(h.msg_count)
        } else {
            s.expected_seq
        },
        seq_valid: s.seq_valid || accept,
        plu_wr_ptr: if plu_last {
            s.plu_wr_ptr + bits::<7>(1)
        } else {
            s.plu_wr_ptr
        },
        counters,
    };

    let sell_bit = if msg.sell { bits::<1>(1) } else { bits::<1>(0) };
    let event_bit = if msg.flags.bit(0) {
        bits::<1>(1)
    } else {
        bits::<1>(0)
    };

    FeedOut {
        state: next,
        rx_raddr: next.rx_rd_ptr.slice::<3>(0).concat::<11, 14>(next.index),
        rx_meta_raddr: next.rx_rd_ptr.slice::<3>(0),
        plu_we: plu_last,
        plu_waddr: s.plu_wr_ptr.slice::<6>(0),
        plu_symbol: msg.symbol,
        plu_price: msg.price,
        plu_misc: sell_bit
            .concat::<1, 2>(event_bit)
            .concat::<32, 34>(msg.size),
    }
}
