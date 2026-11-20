//! Arty A7 Layer-2 Ethernet endpoint expressed as bitlogic next-state functions.
//!
//! The design mirrors the former HLS endpoint (removed; see git history) at
//! the Ethernet-frame level. Every
//! `#[logic]` function here is a pure, untimed transformation `state -> next`.
//! [`core`] declares how the Ethernet units, RAMs and template ROM connect with
//! `bitlogic::system!`, which generates both the Rust cycle model and the
//! `eth_core` Verilog. Clocks, resets and clock-domain crossings stay in the
//! handwritten `rtl/arty_top.v`. See `README.md` in this directory for the
//! block diagram.
//!
//! Domains:
//! - `eth_rx_clk` (25 MHz at 100 Mb/s): [`rx::rx_step`], [`proto::proto_step`],
//!   [`feed::feed_step`], [`book::book_step`], [`tx::tx_step`].
//! - `CLK100MHZ`: [`status::status_step`] drives the LEDs.

pub mod book;
pub mod consts;
pub mod core;
pub mod feed;
pub mod hdl;
pub mod ops;
pub mod proto;
pub mod rom;
pub mod rx;
pub mod sim;
pub mod status;
pub mod tx;

use bitlogic::LogicValue;

/// The all-zero value of a structured type. Every unit resets to all zeros.
pub fn zero<T: LogicValue>() -> T {
    T::from_values(&vec![0; T::schema().leaves().len()]).expect("zero fits every leaf")
}
