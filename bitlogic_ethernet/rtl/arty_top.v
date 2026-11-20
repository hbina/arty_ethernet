`timescale 1ns / 1ps

// Arty A7 board top for the bitlogic Ethernet endpoint. Same pins and XDC as
// the former HLS top level (removed; see git history). This is the only module that touches board
// clocking, the PHY reference clock, resets and clock-domain crossings.
//
// Clock domains:
//   CLK100MHZ  (100 MHz board oscillator): PLL, LED status unit.
//   eth_rx_clk (25 MHz from the PHY at 100 Mb/s): the whole Ethernet core.
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
        .tx_en(eth_tx_en),
        .txd(eth_txd),
        .rx_active(rx_active_eth),
        .tx_frame_toggle(tx_frame_toggle_eth)
    );

    // 25 MHz -> 100 MHz crossings, all sourced from eth-domain registers:
    // a toggle synchronizer turns each frame into one 100 MHz event, and the
    // activity level passes through a two-flop synchronizer.
    (* ASYNC_REG = "TRUE" *) reg [2:0] tx_frame_toggle_sync = 3'b000;
    (* ASYNC_REG = "TRUE" *) reg [1:0] activity_sync = 2'b00;
    reg activity_eth = 1'b0;
    always @(posedge eth_rx_clk)
        activity_eth <= rx_active_eth | eth_tx_en;
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
