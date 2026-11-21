//! The Ethernet core (one clock: the PHY's `eth_rx_clk`), declared once with
//! `bitlogic::system!`. The declaration generates both the Rust cycle model
//! [`eth_core::Core`] and the Verilog module `eth_core` with its unit
//! wrappers, RAMs and template ROM.
//!
//! ```text
//! MII RX -> [mii regs] -> rx -> rx_ram/rx_meta -> proto
//!                            -> feed_rx_ram/feed_rx_meta -> feed -> PLU FIFO -> book
//! book -> BBO FIFO -> proto
//! template ROM -> proto -> tx_ram/tx_meta -> tx -> [tx regs] -> MII TX
//! ```
//!
//! Every value passed between units is registered state or a RAM's
//! registered read data, so no combinational path crosses units.
use crate::book::{BookIn, BookState, book_step};
use crate::consts::*;
use crate::feed::{FeedIn, FeedState, feed_step};
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
        feed_ip: bits(FEED_GROUP_IP.into()),
        feed_port: bits(FEED_PORT.into()),
        bbo_mac: bits(BBO_MAC),
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
        /// The feed unit's copy of the RX ring, written with `rx_ram`/`rx_meta`.
        ram feed_rx_ram: Bits<8>, addr_bits = 14;
        ram feed_rx_meta: Bits<12>, addr_bits = 3;
        /// PLU FIFO, 64 entries: symbol, price, `{sell, event, size}`.
        ram plu_symbol: Bits<64>, addr_bits = 6;
        ram plu_price: Bits<64>, addr_bits = 6;
        ram plu_misc: Bits<34>, addr_bits = 6;
        /// Order book symbol table (2048 slots): symbol, `{valid, bid, ask}`.
        ram sym_key: Bits<64>, addr_bits = 11;
        ram sym_meta: Bits<27>, addr_bits = 11;
        /// Price-level node pool (8191 nodes; node 0 is null) and free stack.
        ram node_price: Bits<64>, addr_bits = 13;
        ram node_size: Bits<32>, addr_bits = 13;
        ram node_next: Bits<13>, addr_bits = 13;
        ram free_list: Bits<13>, addr_bits = 13;
        /// BBO FIFO: 256 records of four 64-bit words.
        ram bbo_ram: Bits<64>, addr_bits = 10;

        unit rx: RxState = rx_step(RxIn {
            state,
            mii,
            rd_ptr: proto.state.rx_rd_ptr,
            feed_rd_ptr: feed.state.rx_rd_ptr,
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
            bbo_wr_ptr: book.state.bbo_wr_ptr,
            bbo_rdata: bbo_ram.rdata,
            feed_packets: feed.state.counters.packets,
            feed_gaps: feed.state.counters.seq_gaps,
            feed_malformed: feed.state.counters.malformed,
            book_errors: book.state.counters.errors,
        });
        unit feed: FeedState = feed_step(FeedIn {
            state,
            config,
            rx_wr_ptr: rx.state.wr_ptr,
            rx_rdata: feed_rx_ram.rdata,
            rx_meta: feed_rx_meta.rdata,
            plu_rd_ptr: book.state.plu_rd_ptr,
        });
        unit book: BookState = book_step(BookIn {
            state,
            plu_wr_ptr: feed.state.plu_wr_ptr,
            plu_symbol: plu_symbol.rdata,
            plu_price: plu_price.rdata,
            plu_misc: plu_misc.rdata,
            sym_key: sym_key.rdata,
            sym_meta: sym_meta.rdata,
            node_price: node_price.rdata,
            node_size: node_size.rdata,
            node_next: node_next.rdata,
            free_top: free_list.rdata,
            bbo_rd_ptr: proto.state.bbo_rd_ptr,
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
        write feed_rx_ram(rx.ram_we, rx.ram_waddr, rx.ram_wdata);
        read feed_rx_ram(feed.rx_raddr);
        write feed_rx_meta(rx.meta_we, rx.meta_waddr, rx.meta_wdata);
        read feed_rx_meta(feed.rx_meta_raddr);
        write plu_symbol(feed.plu_we, feed.plu_waddr, feed.plu_symbol);
        read plu_symbol(book.plu_raddr);
        write plu_price(feed.plu_we, feed.plu_waddr, feed.plu_price);
        read plu_price(book.plu_raddr);
        write plu_misc(feed.plu_we, feed.plu_waddr, feed.plu_misc);
        read plu_misc(book.plu_raddr);
        write sym_key(book.sym_key_we, book.sym_waddr, book.sym_key_wdata);
        read sym_key(book.sym_raddr);
        write sym_meta(book.sym_meta_we, book.sym_waddr, book.sym_meta_wdata);
        read sym_meta(book.sym_raddr);
        write node_price(book.node_price_we, book.node_waddr, book.node_price_wdata);
        read node_price(book.node_raddr);
        write node_size(book.node_size_we, book.node_waddr, book.node_size_wdata);
        read node_size(book.node_raddr);
        write node_next(book.node_next_we, book.node_next_waddr, book.node_next_wdata);
        read node_next(book.node_raddr);
        write free_list(book.free_we, book.free_waddr, book.free_wdata);
        read free_list(book.free_raddr);
        write bbo_ram(book.bbo_we, book.bbo_waddr, book.bbo_wdata);
        read bbo_ram(proto.bbo_raddr);

        /// MII transmit pins, registered so they can pack into IOB flops.
        output tx_en: bool = tx.tx_en, registered;
        output txd: Bits<4> = tx.txd, registered;
        /// Receive activity for the LEDs: the registered `rx_dv`.
        output rx_active: bool = mii.dv;
        /// Flips once per transmitted frame; crosses to the LED domain.
        output tx_frame_toggle: bool = tx.state.frame_toggle;
    }
}
