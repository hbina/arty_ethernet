`timescale 1ns / 1ps

module mii_rx_monitor (
    input  wire        eth_rx_clk,
    input  wire        eth_rx_dv,
    input  wire [3:0]  eth_rxd,
    input  wire        eth_rxerr,
    output reg  [31:0] frame_count = 32'd0,
    output reg  [31:0] byte_count = 32'd0,
    output reg         frame_toggle = 1'b0,
    output reg         rx_active = 1'b0
);
    reg        in_frame = 1'b0;
    reg        have_low_nibble = 1'b0;
    reg        frame_error = 1'b0;
    (* keep = "true" *) reg [7:0] rx_byte = 8'd0;
    reg [15:0] frame_bytes = 16'd0;

    always @(posedge eth_rx_clk) begin
        rx_active <= eth_rx_dv;

        if (eth_rx_dv) begin
            if (!in_frame) begin
                in_frame <= 1'b1;
                have_low_nibble <= 1'b0;
                frame_error <= 1'b0;
                frame_bytes <= 16'd0;
            end

            frame_error <= frame_error | eth_rxerr;
            have_low_nibble <= ~have_low_nibble;
            if (have_low_nibble) begin
                rx_byte <= {eth_rxd, rx_byte[3:0]};
                frame_bytes <= frame_bytes + 16'd1;
            end else begin
                rx_byte[3:0] <= eth_rxd;
            end
        end else begin
            have_low_nibble <= 1'b0;
            frame_error <= 1'b0;

            if (in_frame) begin
                in_frame <= 1'b0;
                if (!frame_error) begin
                    frame_count <= frame_count + 32'd1;
                    byte_count <= byte_count + {16'd0, frame_bytes};
                    frame_toggle <= ~frame_toggle;
                end
            end
        end
    end
endmodule
