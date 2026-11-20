//! LED status in the 100 MHz domain. Mirrors `ethernet_status_hls.cpp`:
//! `led[0]` holds for 125 ms after RX activity or a transmitted frame, and
//! `led[3:1]` show the low bits of the transmitted-frame count.
//!
//! `frame_event` and `rx_active` arrive through synchronizers in the top module.
use bitlogic::{Bits, LogicValue, bits, logic};

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct StatusState {
    pub frame_count: Bits<32>,
    pub holdoff: Bits<25>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct StatusIn {
    pub state: StatusState,
    pub frame_event: bool,
    pub rx_active: bool,
    /// Holdoff reload value (12,500,000 on hardware).
    pub holdoff_cycles: Bits<25>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct StatusOut {
    pub state: StatusState,
    pub led: Bits<4>,
}

#[logic]
pub fn status_step(input: StatusIn) -> StatusOut {
    let s = input.state;
    // As in the HLS block, the LEDs show the updated values.
    let next = StatusState {
        frame_count: if input.frame_event {
            s.frame_count + bits::<32>(1)
        } else {
            s.frame_count
        },
        holdoff: if input.frame_event || input.rx_active {
            input.holdoff_cycles
        } else if s.holdoff != bits::<25>(0) {
            s.holdoff - bits::<25>(1)
        } else {
            s.holdoff
        },
    };
    let activity = if next.holdoff != bits::<25>(0) {
        bits::<1>(1)
    } else {
        bits::<1>(0)
    };
    StatusOut {
        state: next,
        led: next.frame_count.slice::<3>(0).concat::<1, 4>(activity),
    }
}
