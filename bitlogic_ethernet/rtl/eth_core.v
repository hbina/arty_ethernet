`timescale 1ns / 1ps

// Ethernet endpoint core: one clock domain (the PHY's eth_rx_clk).
//
// The rx/proto/tx units are generated from bitlogic step functions; each holds
// its own state register. This module owns the memories and the MII boundary
// registers. Every signal passed between units is a registered state field
// (s_*) or a registered memory output, so no combinational path crosses units.
//
//   MII RX -> [mii regs] -> rx_unit -> rx_ram/rx_meta -> proto_unit
//   template_rom -> proto_unit -> tx_ram/tx_meta -> tx_unit -> [mii regs] -> MII TX
//
// bitlogic_ethernet/src/sim.rs is a cycle-accurate Rust model of this module.
module eth_core #(
    parameter [47:0] MAC             = 48'h020000000001,
    parameter [31:0] IP              = 32'hC0A80164,
    parameter [31:0] BEACON_INTERVAL = 32'd25000000,
    parameter [31:0] GARP_INTERVAL   = 32'd125000000
) (
    input  wire       clk,
    input  wire       rst,
    input  wire       eth_rx_dv,
    input  wire [3:0] eth_rxd,
    input  wire       eth_rxerr,
    output reg        eth_tx_en = 1'b0,
    output reg  [3:0] eth_txd = 4'd0,
    output wire       rx_active,
    output wire       tx_active,
    output wire       tx_frame_toggle
);
    // MII receive inputs are registered once before the RX unit.
    reg       mii_dv = 1'b0;
    reg [3:0] mii_rxd = 4'd0;
    reg       mii_rxerr = 1'b0;
    always @(posedge clk) begin
        if (rst) begin
            mii_dv <= 1'b0;
            mii_rxd <= 4'd0;
            mii_rxerr <= 1'b0;
        end else begin
            mii_dv <= eth_rx_dv;
            mii_rxd <= eth_rxd;
            mii_rxerr <= eth_rxerr;
        end
    end

    // Cross-unit registered state.
    wire [3:0]  rx_wr_ptr;
    wire [31:0] rx_drop_count;
    wire [3:0]  rx_rd_ptr;
    wire [2:0]  tx_wr_ptr;
    wire [2:0]  tx_rd_ptr;

    // Memory ports.
    wire        rx_we, rx_meta_we, tx_we, tx_meta_we;
    wire [13:0] rx_waddr, rx_raddr;
    wire [7:0]  rx_wdata, rx_rdata;
    wire [2:0]  rx_meta_waddr, rx_meta_raddr;
    wire [11:0] rx_meta_wdata, rx_meta_rdata;
    wire [12:0] tx_waddr, tx_raddr;
    wire [7:0]  tx_wdata, tx_rdata;
    wire [1:0]  tx_meta_waddr, tx_meta_raddr;
    wire [10:0] tx_meta_wdata, tx_meta_rdata;
    wire [8:0]  rom_addr, rom_data;

    wire       next_tx_en;
    wire [3:0] next_txd;

    rx_unit rx (
        .clk(clk), .rst(rst),
        .i_mii_dv(mii_dv), .i_mii_rxd(mii_rxd), .i_mii_rxerr(mii_rxerr),
        .i_rd_ptr(rx_rd_ptr),
        .o_ram_we(rx_we), .o_ram_waddr(rx_waddr), .o_ram_wdata(rx_wdata),
        .o_meta_we(rx_meta_we), .o_meta_waddr(rx_meta_waddr), .o_meta_wdata(rx_meta_wdata),
        .s_wr_ptr(rx_wr_ptr), .s_drop_count(rx_drop_count)
    );

    proto_unit proto (
        .clk(clk), .rst(rst),
        .i_config_mac(MAC), .i_config_ip(IP),
        .i_config_beacon_interval(BEACON_INTERVAL),
        .i_config_garp_interval(GARP_INTERVAL),
        .i_rx_wr_ptr(rx_wr_ptr), .i_rx_queue_drops(rx_drop_count),
        .i_rx_rdata(rx_rdata), .i_rx_meta(rx_meta_rdata),
        .i_rom_rdata(rom_data), .i_tx_rd_ptr(tx_rd_ptr),
        .o_rx_raddr(rx_raddr), .o_rx_meta_raddr(rx_meta_raddr),
        .o_rom_addr(rom_addr),
        .o_tx_we(tx_we), .o_tx_waddr(tx_waddr), .o_tx_wdata(tx_wdata),
        .o_tx_meta_we(tx_meta_we), .o_tx_meta_waddr(tx_meta_waddr),
        .o_tx_meta_wdata(tx_meta_wdata),
        .s_rx_rd_ptr(rx_rd_ptr), .s_tx_wr_ptr(tx_wr_ptr)
    );

    tx_unit tx (
        .clk(clk), .rst(rst),
        .i_wr_ptr(tx_wr_ptr), .i_ram_rdata(tx_rdata), .i_meta_rdata(tx_meta_rdata),
        .o_ram_raddr(tx_raddr), .o_meta_raddr(tx_meta_raddr),
        .o_tx_en(next_tx_en), .o_txd(next_txd),
        .s_rd_ptr(tx_rd_ptr), .s_frame_toggle(tx_frame_toggle)
    );

    // RX ring: 8 slots x 2 KiB, plus {truncated, body_len} per slot.
    sdp_ram #(.ADDR_W(14), .DATA_W(8)) rx_ram (
        .clk(clk), .we(rx_we), .waddr(rx_waddr), .wdata(rx_wdata),
        .raddr(rx_raddr), .rdata(rx_rdata)
    );
    sdp_ram #(.ADDR_W(3), .DATA_W(12)) rx_meta (
        .clk(clk), .we(rx_meta_we), .waddr(rx_meta_waddr), .wdata(rx_meta_wdata),
        .raddr(rx_meta_raddr), .rdata(rx_meta_rdata)
    );

    // TX ring: 4 slots x 2 KiB, plus frame body length per slot.
    sdp_ram #(.ADDR_W(13), .DATA_W(8)) tx_ram (
        .clk(clk), .we(tx_we), .waddr(tx_waddr), .wdata(tx_wdata),
        .raddr(tx_raddr), .rdata(tx_rdata)
    );
    sdp_ram #(.ADDR_W(2), .DATA_W(11)) tx_meta (
        .clk(clk), .we(tx_meta_we), .waddr(tx_meta_waddr), .wdata(tx_meta_wdata),
        .raddr(tx_meta_raddr), .rdata(tx_meta_rdata)
    );

    template_rom rom (.clk(clk), .addr(rom_addr), .data(rom_data));

    // MII transmit outputs are registered (and can pack into IOB flops).
    always @(posedge clk) begin
        if (rst) begin
            eth_tx_en <= 1'b0;
            eth_txd <= 4'd0;
        end else begin
            eth_tx_en <= next_tx_en;
            eth_txd <= next_txd;
        end
    end

    assign rx_active = mii_dv;
    assign tx_active = eth_tx_en;
endmodule
