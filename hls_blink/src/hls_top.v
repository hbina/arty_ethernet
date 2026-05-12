`timescale 1ns / 1ps

module hls_top (
    input  wire       CLK100MHZ,
    output wire [3:0] led
);
    blink_hls blink_hls_i (
        .ap_clk(CLK100MHZ),
        .ap_rst(1'b0),
        .led(led)
    );
endmodule
