//! Arty A7 Layer-2 Ethernet endpoint expressed as bitlogic next-state functions.
//!
//! The design mirrors `hls_ethernet/` at the Ethernet-frame level. Every
//! `#[logic]` function here is a pure, untimed transformation `state -> next`.
//! Registers, RAMs, the template ROM, clocks, resets and clock-domain crossings
//! are handwritten Verilog under `rtl/` (plus the thin register units emitted by
//! [`hdl`]). See `README.md` in this directory for the block diagram.
//!
//! Domains:
//! - `eth_rx_clk` (25 MHz at 100 Mb/s): [`rx::rx_step`], [`proto::proto_step`],
//!   [`tx::tx_step`].
//! - `CLK100MHZ`: [`status::status_step`] drives the LEDs.

pub mod consts;
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
