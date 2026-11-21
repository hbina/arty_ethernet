`timescale 1ns / 1ps

// Arty A7 board top for the bitlogic Ethernet endpoint. Same pins and XDC as
// the former HLS top level (removed; see git history). This is the only module that touches board
// clocking, the PHY reference clock, resets and clock-domain crossings.
//
// Clock domains:
//   CLK100MHZ  (100 MHz board oscillator): PLL, LED status unit.
//   eth_rx_clk (25 MHz from the PHY at 100 Mb/s): the whole Ethernet core.
//              The PHY recovers it from the link partner's transmitter.
//   eth_tx_clk (25 MHz from the PHY, derived from eth_ref_clk): MII TX pins.
//              The PHY samples TXD on this clock, so the core's TX nibbles
//              cross into it through a small FIFO (the two clocks differ by
//              the ppm offset between the two ends of the link).
//
// The parameters default to the board values; simulation shortens them.
module arty_top #(
    parameter [31:0] BEACON_INTERVAL = 32'd25000000,   // eth_rx_clk cycles (1 s)
    parameter [31:0] GARP_INTERVAL   = 32'd125000000,  // eth_rx_clk cycles (5 s)
    parameter [24:0] LED_HOLDOFF     = 25'd12500000    // CLK100MHZ cycles (125 ms)
) (
    input  wire       CLK100MHZ,
    output wire       eth_ref_clk,
    output wire       eth_rstn,
    input  wire       eth_rx_clk,
    input  wire       eth_rx_dv,
    input  wire [3:0] eth_rxd,
    input  wire       eth_rxerr,
    input  wire       eth_tx_clk,
    output wire       eth_tx_en,
    output wire [3:0] eth_txd,
    output wire [3:0] led
);
    // 25 MHz PHY reference clock, forwarded through an ODDR.
    wire clk25_unbuffered;
    wire clk25;
    wire pll_feedback;
    wire pll_locked;

    PLLE2_BASE #(
        .BANDWIDTH("OPTIMIZED"),
        .CLKFBOUT_MULT(10),
        .CLKFBOUT_PHASE(0.000),
        .CLKIN1_PERIOD(10.000),
        .CLKOUT0_DIVIDE(40),
        .CLKOUT0_DUTY_CYCLE(0.500),
        .CLKOUT0_PHASE(0.000),
        .DIVCLK_DIVIDE(1),
        .REF_JITTER1(0.010),
        .STARTUP_WAIT("FALSE")
    ) eth_ref_pll_i (
        .CLKIN1(CLK100MHZ),
        .CLKFBIN(pll_feedback),
        .CLKFBOUT(pll_feedback),
        .CLKOUT0(clk25_unbuffered),
        .CLKOUT1(),
        .CLKOUT2(),
        .CLKOUT3(),
        .CLKOUT4(),
        .CLKOUT5(),
        .LOCKED(pll_locked),
        .PWRDWN(1'b0),
        .RST(1'b0)
    );

    BUFG eth_ref_bufg_i (
        .I(clk25_unbuffered),
        .O(clk25)
    );

    ODDR #(
        .DDR_CLK_EDGE("SAME_EDGE"),
        .INIT(1'b0),
        .SRTYPE("SYNC")
    ) eth_ref_clk_oddr_i (
        .C(clk25),
        .CE(1'b1),
        .D1(1'b1),
        .D2(1'b0),
        .Q(eth_ref_clk),
        .R(1'b0),
        .S(1'b0)
    );

    // The PHY leaves reset once the reference clock is stable.
    assign eth_rstn = pll_locked;

    // Ethernet-domain reset: asserted asynchronously while the PLL is unlocked
    // (eth_rx_clk may be stopped then), released synchronously to eth_rx_clk.
    (* ASYNC_REG = "TRUE" *) reg [2:0] eth_rst_sync = 3'b111;
    always @(posedge eth_rx_clk or negedge pll_locked) begin
        if (!pll_locked)
            eth_rst_sync <= 3'b111;
        else
            eth_rst_sync <= {eth_rst_sync[1:0], 1'b0};
    end
    wire eth_rst = eth_rst_sync[2];

    wire tx_frame_toggle_eth;
    wire rx_active_eth;
    wire core_tx_en;
    wire [3:0] core_txd;

    // Generated from the `system!` declaration in src/core.rs.
    eth_core #(
        .CONFIG_BEACON_INTERVAL(BEACON_INTERVAL),
        .CONFIG_GARP_INTERVAL(GARP_INTERVAL)
    ) eth_core_i (
        .clk(eth_rx_clk),
        .rst(eth_rst),
        .mii_dv(eth_rx_dv),
        .mii_rxd(eth_rxd),
        .mii_rxerr(eth_rxerr),
        .tx_en(core_tx_en),
        .txd(core_txd),
        .rx_active(rx_active_eth),
        .tx_frame_toggle(tx_frame_toggle_eth)
    );

    // ---- MII TX: eth_rx_clk -> eth_tx_clk ----------------------------------
    // The core's nibbles of each frame, plus one terminating entry with
    // tx_en low, go into a 16-entry FIFO (LUT RAM, Gray-coded pointers). The
    // TX side starts a frame once 6 entries are queued and sends one entry
    // per eth_tx_clk cycle until the terminator. A 1518-byte frame is 3,056
    // nibbles, so even 1,000 ppm between the clocks moves the fill by ~3
    // entries, well inside the 6 below and 10 above the start level.
    wire eth_tx_clk_buf;
    BUFG eth_tx_bufg_i (
        .I(eth_tx_clk),
        .O(eth_tx_clk_buf)
    );

    function [4:0] gray5(input [4:0] b);
        gray5 = b ^ (b >> 1);
    endfunction
    function [4:0] ungray5(input [4:0] g);
        ungray5 = {g[4], ^g[4:3], ^g[4:2], ^g[4:1], ^g[4:0]};
    endfunction

    reg [4:0] txq_mem [0:15];
    reg core_tx_en_d = 1'b0;
    reg [4:0] txq_wbin = 5'd0;
    reg [4:0] txq_wgray = 5'd0;
    wire txq_write = core_tx_en | core_tx_en_d;
    always @(posedge eth_rx_clk) begin
        if (txq_write)
            txq_mem[txq_wbin[3:0]] <= {core_tx_en, core_txd};
        if (eth_rst) begin
            core_tx_en_d <= 1'b0;
            txq_wbin <= 5'd0;
            txq_wgray <= 5'd0;
        end else begin
            core_tx_en_d <= core_tx_en;
            if (txq_write) begin
                txq_wbin <= txq_wbin + 5'd1;
                txq_wgray <= gray5(txq_wbin + 5'd1);
            end
        end
    end

    // TX-domain reset: asserted with the PLL unlocked, like eth_rst.
    (* ASYNC_REG = "TRUE" *) reg [2:0] tx_rst_sync = 3'b111;
    always @(posedge eth_tx_clk_buf or negedge pll_locked) begin
        if (!pll_locked)
            tx_rst_sync <= 3'b111;
        else
            tx_rst_sync <= {tx_rst_sync[1:0], 1'b0};
    end
    wire tx_rst = tx_rst_sync[2];

    (* ASYNC_REG = "TRUE" *) reg [4:0] txq_wgray_s1 = 5'd0;
    (* ASYNC_REG = "TRUE" *) reg [4:0] txq_wgray_s2 = 5'd0;
    reg [4:0] txq_rbin = 5'd0;
    reg txq_active = 1'b0;
    (* IOB = "TRUE" *) reg tx_en_q = 1'b0;
    (* IOB = "TRUE" *) reg [3:0] txd_q = 4'd0;
    wire [4:0] txq_fill = ungray5(txq_wgray_s2) - txq_rbin;
    wire [4:0] txq_entry = txq_mem[txq_rbin[3:0]];
    wire txq_read = txq_fill != 5'd0 && (txq_active || txq_fill >= 5'd6);
    always @(posedge eth_tx_clk_buf) begin
        if (tx_rst) begin
            txq_wgray_s1 <= 5'd0;
            txq_wgray_s2 <= 5'd0;
            txq_rbin <= 5'd0;
            txq_active <= 1'b0;
            tx_en_q <= 1'b0;
            txd_q <= 4'd0;
        end else begin
            txq_wgray_s1 <= txq_wgray;
            txq_wgray_s2 <= txq_wgray_s1;
            if (txq_read) begin
                txq_rbin <= txq_rbin + 5'd1;
                txq_active <= txq_entry[4];
                tx_en_q <= txq_entry[4];
                txd_q <= txq_entry[3:0];
            end else begin
                // Idle, or an underrun mid-frame (not expected: the frame is
                // cut short and the receiver drops it on its FCS).
                tx_en_q <= 1'b0;
                txd_q <= 4'd0;
            end
        end
    end
    assign eth_tx_en = tx_en_q;
    assign eth_txd = txd_q;

    // 25 MHz -> 100 MHz crossings, all sourced from eth-domain registers:
    // a toggle synchronizer turns each frame into one 100 MHz event, and the
    // activity level passes through a two-flop synchronizer.
    (* ASYNC_REG = "TRUE" *) reg [2:0] tx_frame_toggle_sync = 3'b000;
    (* ASYNC_REG = "TRUE" *) reg [1:0] activity_sync = 2'b00;
    reg activity_eth = 1'b0;
    always @(posedge eth_rx_clk)
        activity_eth <= rx_active_eth | core_tx_en;
    always @(posedge CLK100MHZ) begin
        tx_frame_toggle_sync <= {tx_frame_toggle_sync[1:0], tx_frame_toggle_eth};
        activity_sync <= {activity_sync[0], activity_eth};
    end
    wire frame_event_sys = tx_frame_toggle_sync[2] ^ tx_frame_toggle_sync[1];

    status_unit status_i (
        .clk(CLK100MHZ),
        .rst(1'b0),
        .i_frame_event(frame_event_sys),
        .i_rx_active(activity_sync[1]),
        .i_holdoff_cycles(LED_HOLDOFF),
        .o_led(led),
        .s_frame_count(),
        .s_holdoff()
    );
endmodule
