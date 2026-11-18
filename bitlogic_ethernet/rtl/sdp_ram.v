`timescale 1ns / 1ps

// Simple dual-port RAM: one write port, one always-enabled read port, one clock.
// Read data is registered (one cycle of latency) and read-first: a read of an
// address written on the same edge returns the old contents. The design only
// discards reads that could collide with writes. Contents start at zero.
// Vivado infers block RAM for the packet buffers and LUT RAM for metadata.
module sdp_ram #(
    parameter ADDR_W = 11,
    parameter DATA_W = 8
) (
    input  wire              clk,
    input  wire              we,
    input  wire [ADDR_W-1:0] waddr,
    input  wire [DATA_W-1:0] wdata,
    input  wire [ADDR_W-1:0] raddr,
    output reg  [DATA_W-1:0] rdata
);
    reg [DATA_W-1:0] mem [0:(1 << ADDR_W) - 1];

    integer i;
    initial begin
        for (i = 0; i < (1 << ADDR_W); i = i + 1)
            mem[i] = {DATA_W{1'b0}};
        rdata = {DATA_W{1'b0}};
    end

    always @(posedge clk) begin
        if (we)
            mem[waddr] <= wdata;
        rdata <= mem[raddr];
    end
endmodule
