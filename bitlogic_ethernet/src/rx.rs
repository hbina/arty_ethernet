//! MII receive: nibble assembly, preamble/SFD search, FCS stripping and RX ring
//! publication. Mirrors `ethernet_mii.h`, `ethernet_rx.h` and `ethernet_rx_queue.h`.
//!
//! The whole frame body (header + payload, FCS removed) is written to RX RAM at
//! `{slot, offset}`. Bytes pass through a four-byte delay so the trailing FCS is
//! never stored. On frame end a valid frame publishes `{truncated, body_len}`
//! into the slot's metadata entry and advances `wr_ptr`. The ring has two
//! consumers, each with its own copy of the RAM and its own pointer: the
//! protocol unit (`rd_ptr`) and the feed unit (`feed_rd_ptr`). The ring is
//! full when either consumer is eight frames behind.
//!
//! The CRC-32 runs over every data byte including the FCS. A frame is intact
//! when the register ends at the CRC-32 residue `0xDEBB20E3`; frames that fail
//! are not published and are counted in `fcs_errors`. (The HLS design does
//! not check the FCS.)
use crate::ops::crc32_byte;
use bitlogic::{Bits, LogicValue, bits, logic};

/// Reflected CRC-32 register value after a correct FCS has been processed.
pub const CRC32_RESIDUE: u64 = 0xDEBB_20E3;

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct MiiRx {
    pub dv: bool,
    pub rxd: Bits<4>,
    pub rxerr: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct RxState {
    // Nibble assembler.
    pub in_frame: bool,
    pub have_low: bool,
    pub low_nibble: Bits<4>,
    // Parser.
    pub in_data: bool,
    pub seen_preamble: bool,
    pub byte_index: Bits<16>,
    pub error: bool,
    /// Four-byte FCS delay line; newest byte in bits 7:0.
    pub tail: Bits<32>,
    pub tail_count: Bits<3>,
    pub body_count: Bits<11>,
    pub truncated: bool,
    /// The ring was full at frame start; nothing from this frame is stored.
    pub dropping: bool,
    /// Running reflected CRC-32 over data bytes, FCS included.
    pub crc: Bits<32>,
    // Producer side of the RX ring.
    pub wr_ptr: Bits<4>,
    /// Frames dropped because the ring was full.
    pub drop_count: Bits<32>,
    /// Frames dropped because the FCS did not match.
    pub fcs_errors: Bits<32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct RxIn {
    pub state: RxState,
    pub mii: MiiRx,
    /// Consumer pointers, registered in the protocol and feed units.
    pub rd_ptr: Bits<4>,
    pub feed_rd_ptr: Bits<4>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct RxOut {
    pub state: RxState,
    pub ram_we: bool,
    pub ram_waddr: Bits<14>,
    pub ram_wdata: Bits<8>,
    pub meta_we: bool,
    pub meta_waddr: Bits<3>,
    /// `{truncated, body_len[10:0]}`
    pub meta_wdata: Bits<12>,
}

#[logic]
pub fn rx_step(input: RxIn) -> RxOut {
    let s = input.state;
    let dv = input.mii.dv;
    let rxd = input.mii.rxd;

    // Nibble assembly: low nibble first, byte valid on the high nibble.
    let start = dv && !s.in_frame;
    let end = !dv && s.in_frame;
    let have_low = s.have_low && !start;
    let byte_valid = dv && have_low;
    let byte = rxd.concat::<4, 8>(s.low_nibble);

    // Frame-start bookkeeping (HLS resets the parser on frame_start).
    let full =
        s.wr_ptr - input.rd_ptr == bits::<4>(8) || s.wr_ptr - input.feed_rd_ptr == bits::<4>(8);
    let dropping = if start { full } else { s.dropping };
    let drop_count = if start && full {
        s.drop_count + bits::<32>(1)
    } else {
        s.drop_count
    };
    let in_data = s.in_data && !start;
    let seen_preamble = s.seen_preamble && !start;
    let byte_index = if start { bits::<16>(0) } else { s.byte_index };
    let error = (s.error && !start) || input.mii.rxerr;
    let tail_count = if start { bits::<3>(0) } else { s.tail_count };
    let body_count = if start { bits::<11>(0) } else { s.body_count };
    let truncated = s.truncated && !start;
    let crc = if start {
        bits::<32>(0xffff_ffff)
    } else {
        s.crc
    };

    // Preamble/SFD search. Any non-0x55 byte leaves the search; the SFD after
    // at least one preamble byte is consumed, anything else is frame data.
    let is_preamble = byte == bits::<8>(0x55);
    let is_sfd = byte == bits::<8>(0xd5);
    let process = byte_valid && (in_data || !(is_preamble || (is_sfd && seen_preamble)));
    let next_in_data = in_data || (byte_valid && !is_preamble);
    let next_seen_preamble = seen_preamble || (byte_valid && !in_data && is_preamble);

    // FCS delay line: the oldest byte leaves once four bytes are buffered.
    let tail_full = tail_count == bits::<3>(4);
    let shifted_tail = (s.tail << 8) | bits::<24>(0).concat::<8, 32>(byte);
    let can_store = body_count < bits::<11>(1514);
    let emit = process && tail_full;
    let store = emit && can_store;

    let next_error = error;
    let frame_ok = byte_index >= bits::<16>(18) && tail_full && !next_error;
    let fcs_ok = crc == bits::<32>(0xDEBB_20E3);
    let publish = end && !dropping && frame_ok && fcs_ok;
    let fcs_error = end && !dropping && frame_ok && !fcs_ok;
    let truncated_bit = if truncated {
        bits::<1>(1)
    } else {
        bits::<1>(0)
    };

    RxOut {
        state: RxState {
            in_frame: dv,
            have_low: dv && !have_low,
            low_nibble: if dv && !have_low { rxd } else { s.low_nibble },
            in_data: next_in_data && !end,
            seen_preamble: next_seen_preamble && !end,
            byte_index: if end {
                bits::<16>(0)
            } else if process {
                byte_index + bits::<16>(1)
            } else {
                byte_index
            },
            error: next_error && !end,
            tail: if process { shifted_tail } else { s.tail },
            tail_count: if end {
                bits::<3>(0)
            } else if process && !tail_full {
                tail_count + bits::<3>(1)
            } else {
                tail_count
            },
            body_count: if end {
                bits::<11>(0)
            } else if store {
                body_count + bits::<11>(1)
            } else {
                body_count
            },
            truncated: (truncated || (emit && !can_store)) && !end,
            dropping: dropping && !end,
            crc: if process { crc32_byte(crc, byte) } else { crc },
            wr_ptr: if publish {
                s.wr_ptr + bits::<4>(1)
            } else {
                s.wr_ptr
            },
            drop_count,
            fcs_errors: if fcs_error {
                s.fcs_errors + bits::<32>(1)
            } else {
                s.fcs_errors
            },
        },
        ram_we: store && !dropping,
        ram_waddr: s.wr_ptr.slice::<3>(0).concat::<11, 14>(body_count),
        ram_wdata: s.tail.slice::<8>(24),
        meta_we: publish,
        meta_waddr: s.wr_ptr.slice::<3>(0),
        meta_wdata: truncated_bit.concat::<11, 12>(body_count),
    }
}
