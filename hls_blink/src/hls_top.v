`timescale 1ns / 1ps

module hls_top (
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

    assign eth_rstn = pll_locked;

    wire endpoint_eth_tx_en;
    wire [3:0] endpoint_eth_txd;
    wire       rx_accept_toggle_eth;
    wire       tx_frame_toggle_eth;
    wire       rx_active_eth;
    wire       tx_active_eth;

    assign eth_tx_en = endpoint_eth_tx_en;
    assign eth_txd = endpoint_eth_txd;

    ethernet_l2_endpoint_hls ethernet_l2_endpoint_hls_i (
        .ap_clk(eth_rx_clk),
        .ap_rst(~pll_locked),
        .eth_rx_dv(eth_rx_dv),
        .eth_rxd(eth_rxd),
        .eth_rxerr(eth_rxerr),
        .eth_tx_en(endpoint_eth_tx_en),
        .eth_txd(endpoint_eth_txd),
        .rx_accept_toggle(rx_accept_toggle_eth),
        .tx_frame_toggle(tx_frame_toggle_eth),
        .rx_active(rx_active_eth),
        .tx_active(tx_active_eth)
    );

    reg [2:0] rx_accept_toggle_sync = 3'b000;
    reg [2:0] tx_frame_toggle_sync = 3'b000;
    reg [1:0] activity_sync = 2'b00;
    wire      frame_event_sys;

    assign frame_event_sys = (rx_accept_toggle_sync[2] ^ rx_accept_toggle_sync[1]) |
                             (tx_frame_toggle_sync[2] ^ tx_frame_toggle_sync[1]);

    always @(posedge CLK100MHZ) begin
        rx_accept_toggle_sync <= {rx_accept_toggle_sync[1:0], rx_accept_toggle_eth};
        tx_frame_toggle_sync <= {tx_frame_toggle_sync[1:0], tx_frame_toggle_eth};
        activity_sync <= {activity_sync[0], rx_active_eth | tx_active_eth};
    end

    blink_hls blink_hls_i (
        .ap_clk(CLK100MHZ),
        .ap_rst(1'b0),
        .frame_event(frame_event_sys),
        .rx_active(activity_sync[1]),
        .led(led)
    );

endmodule
