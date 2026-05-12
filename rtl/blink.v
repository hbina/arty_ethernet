`timescale 1ns / 1ps

module blink #(
    parameter integer CLK_HZ = 100_000_000
) (
    input  wire       CLK100MHZ,
    output wire [3:0] led
);
    localparam integer COUNTER_WIDTH = 32;

    reg [COUNTER_WIDTH-1:0] counter = {COUNTER_WIDTH{1'b0}};

    always @(posedge CLK100MHZ) begin
        counter <= counter + 1'b1;
    end

    assign led[0] = counter[24]; // about 3 Hz at 100 MHz
    assign led[1] = counter[25];
    assign led[2] = counter[26];
    assign led[3] = counter[27];
endmodule
