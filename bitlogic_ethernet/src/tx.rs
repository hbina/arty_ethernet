//! MII transmit framer: drains the TX ring and emits preamble/SFD, the frame
//! body (zero padded to 60 bytes), the CRC-32 FCS and a 24-nibble inter-frame
//! gap. Mirrors `ethernet_framer.h` and `ethernet_tx_queue.h`.
//!
//! TX RAM is read with one cycle of latency at `{next rd_ptr, next byte_index}`,
//! so `ram_rdata` always holds the current state's body byte. The low nibble is
//! sent from `ram_rdata` and the byte is kept in `current` for the high nibble.
use crate::ops::{byte_of32_le, crc32_byte};
use bitlogic::{Bits, LogicValue, bits, logic};

pub const PHASE_IDLE: u64 = 0;
pub const PHASE_LOAD: u64 = 1;
pub const PHASE_PREAMBLE: u64 = 2;
pub const PHASE_DATA: u64 = 3;
pub const PHASE_FCS: u64 = 4;
pub const PHASE_IFG: u64 = 5;

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct TxState {
    pub phase: Bits<3>,
    pub high_nibble: bool,
    pub byte_index: Bits<11>,
    pub fcs_index: Bits<2>,
    pub ifg_count: Bits<6>,
    /// Running CRC in DATA; the complemented FCS in FCS.
    pub crc: Bits<32>,
    pub len: Bits<11>,
    pub body_len: Bits<11>,
    pub current: Bits<8>,
    /// Consumer side of the TX ring.
    pub rd_ptr: Bits<3>,
    /// Flips once per transmitted frame; crosses to the LED domain.
    pub frame_toggle: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct TxIn {
    pub state: TxState,
    pub wr_ptr: Bits<3>,
    pub ram_rdata: Bits<8>,
    pub meta_rdata: Bits<11>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct TxOut {
    pub state: TxState,
    pub ram_raddr: Bits<13>,
    pub meta_raddr: Bits<2>,
    pub tx_en: bool,
    pub txd: Bits<4>,
}

#[logic]
pub fn tx_step(input: TxIn) -> TxOut {
    let s = input.state;
    let idle = s.phase == bits::<3>(0);
    let load = s.phase == bits::<3>(1);
    let preamble = s.phase == bits::<3>(2);
    let data = s.phase == bits::<3>(3);
    let fcs = s.phase == bits::<3>(4);
    let ifg = s.phase == bits::<3>(5);
    let high = s.high_nibble;

    let start = idle && s.rd_ptr != input.wr_ptr;
    let len = input.meta_rdata;
    let padded = if len < bits::<11>(60) {
        bits::<11>(60)
    } else {
        len
    };

    // Byte on the wire this cycle.
    let preamble_byte = if s.byte_index == bits::<11>(7) {
        bits::<8>(0xd5)
    } else {
        bits::<8>(0x55)
    };
    let body_byte = if high {
        s.current
    } else if s.byte_index < s.len {
        input.ram_rdata
    } else {
        bits::<8>(0)
    };
    let fcs_byte = byte_of32_le(s.crc, s.fcs_index);
    let out_byte = if preamble {
        preamble_byte
    } else if data {
        body_byte
    } else {
        fcs_byte
    };
    let sending = preamble || data || fcs;
    let byte_done = sending && high;

    // Reflected CRC-32 (polynomial 0xEDB88320), one byte per data byte.
    let crc_next = crc32_byte(s.crc, s.current);

    let preamble_done = preamble && byte_done && s.byte_index == bits::<11>(7);
    let data_done = data && byte_done && s.byte_index == s.body_len - bits::<11>(1);
    let fcs_done = fcs && byte_done && s.fcs_index == bits::<2>(3);
    let ifg_done = ifg && s.ifg_count == bits::<6>(23);

    let next_phase = if start {
        bits::<3>(1)
    } else if load {
        bits::<3>(2)
    } else if preamble_done {
        bits::<3>(3)
    } else if data_done {
        bits::<3>(4)
    } else if fcs_done {
        bits::<3>(5)
    } else if ifg_done {
        bits::<3>(0)
    } else {
        s.phase
    };
    let next_byte_index = if load || preamble_done || data_done {
        bits::<11>(0)
    } else if byte_done && (preamble || data) {
        s.byte_index + bits::<11>(1)
    } else {
        s.byte_index
    };
    let next_crc = if load {
        bits::<32>(0xffff_ffff)
    } else if data && high {
        if data_done { !crc_next } else { crc_next }
    } else {
        s.crc
    };
    let next_rd_ptr = if ifg_done {
        s.rd_ptr + bits::<3>(1)
    } else {
        s.rd_ptr
    };

    let next = TxState {
        phase: next_phase,
        high_nibble: sending && !high,
        byte_index: next_byte_index,
        fcs_index: if data_done {
            bits::<2>(0)
        } else if fcs && high {
            s.fcs_index + bits::<2>(1)
        } else {
            s.fcs_index
        },
        ifg_count: if fcs_done {
            bits::<6>(0)
        } else if ifg {
            s.ifg_count + bits::<6>(1)
        } else {
            s.ifg_count
        },
        crc: next_crc,
        len: if load { len } else { s.len },
        body_len: if load { padded } else { s.body_len },
        current: if data && !high { body_byte } else { s.current },
        rd_ptr: next_rd_ptr,
        frame_toggle: s.frame_toggle != fcs_done,
    };

    TxOut {
        state: next,
        ram_raddr: next.rd_ptr.slice::<2>(0).concat::<11, 13>(next.byte_index),
        meta_raddr: next.rd_ptr.slice::<2>(0),
        tx_en: sending,
        txd: if high {
            out_byte.slice::<4>(4)
        } else {
            out_byte.slice::<4>(0)
        },
    }
}
