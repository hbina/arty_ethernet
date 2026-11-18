//! Cycle-accurate model of `rtl/eth_core.v` for tests.
//!
//! It holds the same registers and memories as the Verilog and evaluates the
//! step functions either natively or through the bitlogic IR interpreter.
//! Memories are read-first with one cycle of latency; MII inputs and outputs
//! are registered at the core boundary, as in the Verilog.
use crate::consts::*;
use crate::proto::{ProtoConfig, ProtoIn, ProtoOut, ProtoState, proto_step};
use crate::rx::{MiiRx, RxIn, RxOut, RxState, rx_step};
use crate::tx::{TxIn, TxOut, TxState, tx_step};
use crate::{rom, zero};
use bitlogic::{Error, Logic, bits};

/// How each cycle's step functions are evaluated.
pub trait Steps {
    fn rx(&self, input: RxIn) -> RxOut;
    fn proto(&self, input: ProtoIn) -> ProtoOut;
    fn tx(&self, input: TxIn) -> TxOut;
}

/// Native Rust evaluation of the `#[logic]` functions.
pub struct Native;

impl Steps for Native {
    fn rx(&self, input: RxIn) -> RxOut {
        rx_step(input)
    }
    fn proto(&self, input: ProtoIn) -> ProtoOut {
        proto_step(input)
    }
    fn tx(&self, input: TxIn) -> TxOut {
        tx_step(input)
    }
}

/// Evaluation through the captured bitlogic IR graphs.
pub struct Interpreted {
    rx: Logic,
    proto: Logic,
    tx: Logic,
}

impl Interpreted {
    pub fn new() -> Result<Self, Error> {
        Ok(Self {
            rx: rx_step::ir()?,
            proto: proto_step::ir()?,
            tx: tx_step::ir()?,
        })
    }
}

impl Steps for Interpreted {
    fn rx(&self, input: RxIn) -> RxOut {
        self.rx.evaluate_typed(input).expect("rx IR evaluates")
    }
    fn proto(&self, input: ProtoIn) -> ProtoOut {
        self.proto
            .evaluate_typed(input)
            .expect("proto IR evaluates")
    }
    fn tx(&self, input: TxIn) -> TxOut {
        self.tx.evaluate_typed(input).expect("tx IR evaluates")
    }
}

/// One MII nibble cycle on the receive side.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RxNibble {
    pub dv: bool,
    pub rxd: u8,
    pub rxerr: bool,
}

/// The registered MII transmit outputs after a clock edge.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TxNibble {
    pub en: bool,
    pub txd: u8,
}

/// Parameters of `eth_core`; tests shorten the timer intervals.
#[derive(Clone, Copy, Debug)]
pub struct CoreConfig {
    pub mac: u64,
    pub ip: u32,
    pub beacon_interval: u32,
    pub garp_interval: u32,
}

impl Default for CoreConfig {
    fn default() -> Self {
        Self {
            mac: FPGA_MAC,
            ip: FPGA_IP,
            beacon_interval: BEACON_INTERVAL_CYCLES,
            garp_interval: GRATUITOUS_ARP_INTERVAL_CYCLES,
        }
    }
}

pub struct EthCore<S: Steps> {
    steps: S,
    config: ProtoConfig,
    pub rx: RxState,
    pub proto: ProtoState,
    pub tx: TxState,
    mii: MiiRx,
    out: TxNibble,
    rx_ram: Vec<u8>,
    rx_meta: Vec<u16>,
    tx_ram: Vec<u8>,
    tx_meta: Vec<u16>,
    rom: Vec<u16>,
    rx_rdata: u8,
    rx_meta_rdata: u16,
    rom_rdata: u16,
    tx_rdata: u8,
    tx_meta_rdata: u16,
}

impl<S: Steps> EthCore<S> {
    /// State immediately after reset: registers zero, memories zero, and each
    /// memory output holding the entry addressed by the reset state.
    pub fn new(steps: S, config: CoreConfig) -> Self {
        let rom = rom::rom_contents();
        let rom_rdata = rom[0];
        Self {
            steps,
            config: ProtoConfig {
                mac: bits(config.mac),
                ip: bits(config.ip.into()),
                beacon_interval: bits(config.beacon_interval.into()),
                garp_interval: bits(config.garp_interval.into()),
            },
            rx: zero(),
            proto: zero(),
            tx: zero(),
            mii: zero(),
            out: TxNibble::default(),
            rx_ram: vec![0; RX_SLOTS * SLOT_BYTES],
            rx_meta: vec![0; RX_SLOTS],
            tx_ram: vec![0; TX_SLOTS * SLOT_BYTES],
            tx_meta: vec![0; TX_SLOTS],
            rom,
            rx_rdata: 0,
            rx_meta_rdata: 0,
            rom_rdata,
            tx_rdata: 0,
            tx_meta_rdata: 0,
        }
    }

    /// Advance one clock edge with `input` on the MII receive pins and return
    /// the registered MII transmit outputs after the edge.
    pub fn clock(&mut self, input: RxNibble) -> TxNibble {
        let rx = self.steps.rx(RxIn {
            state: self.rx,
            mii: self.mii,
            rd_ptr: self.proto.rx_rd_ptr,
        });
        let proto = self.steps.proto(ProtoIn {
            state: self.proto,
            config: self.config,
            rx_wr_ptr: self.rx.wr_ptr,
            rx_queue_drops: self.rx.drop_count,
            rx_fcs_errors: self.rx.fcs_errors,
            rx_rdata: bits(self.rx_rdata.into()),
            rx_meta: bits(self.rx_meta_rdata.into()),
            rom_rdata: bits(self.rom_rdata.into()),
            tx_rd_ptr: self.tx.rd_ptr,
        });
        let tx = self.steps.tx(TxIn {
            state: self.tx,
            wr_ptr: self.proto.tx_wr_ptr,
            ram_rdata: bits(self.tx_rdata.into()),
            meta_rdata: bits(self.tx_meta_rdata.into()),
        });

        // Read-first memories: sample before this edge's writes.
        self.rx_rdata = self.rx_ram[proto.rx_raddr.get() as usize];
        self.rx_meta_rdata = self.rx_meta[proto.rx_meta_raddr.get() as usize];
        self.rom_rdata = self.rom[proto.rom_addr.get() as usize];
        self.tx_rdata = self.tx_ram[tx.ram_raddr.get() as usize];
        self.tx_meta_rdata = self.tx_meta[tx.meta_raddr.get() as usize];
        if rx.ram_we {
            self.rx_ram[rx.ram_waddr.get() as usize] = rx.ram_wdata.get() as u8;
        }
        if rx.meta_we {
            self.rx_meta[rx.meta_waddr.get() as usize] = rx.meta_wdata.get() as u16;
        }
        if proto.tx_we {
            self.tx_ram[proto.tx_waddr.get() as usize] = proto.tx_wdata.get() as u8;
        }
        if proto.tx_meta_we {
            self.tx_meta[proto.tx_meta_waddr.get() as usize] = proto.tx_meta_wdata.get() as u16;
        }

        self.rx = rx.state;
        self.proto = proto.state;
        self.tx = tx.state;
        self.mii = MiiRx {
            dv: input.dv,
            rxd: bits(input.rxd.into()),
            rxerr: input.rxerr,
        };
        self.out = TxNibble {
            en: tx.tx_en,
            txd: tx.txd.get() as u8,
        };
        self.out
    }
}
