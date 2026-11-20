//! The Ethernet core (one clock: the PHY's `eth_rx_clk`), declared once with
//! `bitlogic::system!`. The declaration generates both the Rust cycle model
//! [`eth_core::Core`] and the Verilog module `eth_core` with its unit
//! wrappers, RAMs and template ROM.
//!
//! ```text
//! MII RX -> [mii regs] -> rx -> rx_ram/rx_meta -> proto
//! template ROM -> proto -> tx_ram/tx_meta -> tx -> [tx regs] -> MII TX
//! ```
//!
//! Every value passed between units is registered state or a RAM's
//! registered read data, so no combinational path crosses units.
use crate::consts::*;
use crate::proto::{ProtoConfig, ProtoIn, ProtoState, proto_step};
use crate::rx::{MiiRx, RxIn, RxState, rx_step};
use crate::tx::{TxIn, TxState, tx_step};
use bitlogic::{Bits, bits};

/// The board's identity and timer intervals.
pub fn default_config() -> ProtoConfig {
    ProtoConfig {
        mac: bits(FPGA_MAC),
        ip: bits(FPGA_IP.into()),
        beacon_interval: bits(BEACON_INTERVAL_CYCLES.into()),
        garp_interval: bits(GRATUITOUS_ARP_INTERVAL_CYCLES.into()),
    }
}

fn template() -> impl Iterator<Item = Bits<9>> {
    crate::rom::rom_contents()
        .into_iter()
        .map(|w| bits(w.into()))
}

bitlogic::system! {
    /// Ethernet endpoint core.
    pub mod eth_core {
        param config: ProtoConfig = default_config();
        /// MII receive pins, registered once before the RX unit.
        input mii: MiiRx, registered;

        /// RX ring: 8 slots x 2 KiB, plus `{truncated, body_len}` per slot.
        ram rx_ram: Bits<8>, addr_bits = 14;
        ram rx_meta: Bits<12>, addr_bits = 3;
        /// TX ring: 4 slots x 2 KiB, plus the frame body length per slot.
        ram tx_ram: Bits<8>, addr_bits = 13;
        ram tx_meta: Bits<11>, addr_bits = 2;
        /// Beacon and ARP reply templates.
        rom template: Bits<9>, addr_bits = 9, contents = template();

        unit rx: RxState = rx_step(RxIn {
            state,
            mii,
            rd_ptr: proto.state.rx_rd_ptr,
        });
        unit proto: ProtoState = proto_step(ProtoIn {
            state,
            config,
            rx_wr_ptr: rx.state.wr_ptr,
            rx_queue_drops: rx.state.drop_count,
            rx_fcs_errors: rx.state.fcs_errors,
            rx_rdata: rx_ram.rdata,
            rx_meta: rx_meta.rdata,
            rom_rdata: template.rdata,
            tx_rd_ptr: tx.state.rd_ptr,
        });
        unit tx: TxState = tx_step(TxIn {
            state,
            wr_ptr: proto.state.tx_wr_ptr,
            ram_rdata: tx_ram.rdata,
            meta_rdata: tx_meta.rdata,
        });

        write rx_ram(rx.ram_we, rx.ram_waddr, rx.ram_wdata);
        read rx_ram(proto.rx_raddr);
        write rx_meta(rx.meta_we, rx.meta_waddr, rx.meta_wdata);
        read rx_meta(proto.rx_meta_raddr);
        write tx_ram(proto.tx_we, proto.tx_waddr, proto.tx_wdata);
        read tx_ram(tx.ram_raddr);
        write tx_meta(proto.tx_meta_we, proto.tx_meta_waddr, proto.tx_meta_wdata);
        read tx_meta(tx.meta_raddr);
        read template(proto.rom_addr);

        /// MII transmit pins, registered so they can pack into IOB flops.
        output tx_en: bool = tx.tx_en, registered;
        output txd: Bits<4> = tx.txd, registered;
        /// Receive activity for the LEDs: the registered `rx_dv`.
        output rx_active: bool = mii.dv;
        /// Flips once per transmitted frame; crosses to the LED domain.
        output tx_frame_toggle: bool = tx.state.frame_toggle;
    }
}
