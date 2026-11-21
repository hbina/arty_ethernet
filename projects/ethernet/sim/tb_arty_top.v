`timescale 1ns / 1ps

// Board-level testbench for arty_top: real Xilinx PLLE2_BASE/BUFG/ODDR models
// (unisims_ver + glbl), a 100 MHz board clock and an independent PHY RX clock.
//
// The PHY RX clock only starts once the design releases eth_rstn (as a real
// PHY would), with an arbitrary phase and a 100 ppm frequency offset from the
// board clock, so the reset synchronizer and both clock-domain crossings run
// asynchronously.
//
// Inputs:  stim.hex, one {rxerr, dv, rxd[3:0]} byte per eth_rx_clk cycle,
//          applied from the first cycle after the Ethernet reset releases.
// The PHY TX clock runs 300 ppm fast (independent of the RX clock, as on a
// real link), and TX is sampled on it.
//
// Outputs: tx.txt      one "{tx_en}{txd}" line per eth_tx_clk cycle
//          led.txt     "<time ps> <led hex>" on every LED change
//          summary.txt PLL lock time, eth_ref_clk period, first RX cycle
module tb_arty_top;
    parameter integer CYCLES = 1000;
    parameter [31:0] BEACON_INTERVAL = 32'd3000;
    parameter [31:0] GARP_INTERVAL = 32'd7000;
    parameter [24:0] LED_HOLDOFF = 25'd200;
    parameter integer TAIL_NS = 5000;

    reg clk100 = 1'b0;
    always #5 clk100 = ~clk100;

    wire eth_rstn;
    reg rx_clk = 1'b0;
    initial begin
        wait (eth_rstn === 1'b1);
        #13.7;
        forever #20.002 rx_clk = ~rx_clk;  // 40.004 ns: 25 MHz - 100 ppm
    end

    reg tx_clk = 1'b0;
    initial begin
        wait (eth_rstn === 1'b1);
        #5.3;
        forever #19.994 tx_clk = ~tx_clk;  // 39.988 ns: 25 MHz + 300 ppm
    end

    reg dv = 1'b0, rxerr = 1'b0;
    reg [3:0] rxd = 4'd0;
    wire eth_ref_clk, tx_en;
    wire [3:0] txd, led;

    arty_top #(
        .BEACON_INTERVAL(BEACON_INTERVAL),
        .GARP_INTERVAL(GARP_INTERVAL),
        .LED_HOLDOFF(LED_HOLDOFF)
    ) dut (
        .CLK100MHZ(clk100),
        .eth_ref_clk(eth_ref_clk),
        .eth_rstn(eth_rstn),
        .eth_rx_clk(rx_clk),
        .eth_rx_dv(dv),
        .eth_rxd(rxd),
        .eth_rxerr(rxerr),
        .eth_tx_clk(tx_clk),
        .eth_tx_en(tx_en),
        .eth_txd(txd),
        .led(led)
    );

    integer summary, ledf;
    initial begin
        summary = $fopen("summary.txt", "w");
        ledf = $fopen("led.txt", "w");
    end
    always @(led) $fwrite(ledf, "%0t %h\n", $time, led);

    // eth_ref_clk: average period over 16 cycles once the PLL has locked.
    realtime first_edge;
    integer n;
    initial begin
        wait (eth_rstn === 1'b1);
        $fwrite(summary, "lock_ns %0.3f\n", $realtime);
        repeat (4) @(posedge eth_ref_clk);
        first_edge = $realtime;
        for (n = 0; n < 16; n = n + 1) @(posedge eth_ref_clk);
        $fwrite(summary, "ref_period_ns %0.3f\n", ($realtime - first_edge) / 16.0);
    end

    reg [7:0] stim [0:CYCLES-1];
    integer k, txf;
    reg running = 1'b0;
    always @(posedge tx_clk)
        if (running) #1 $fwrite(txf, "%0d%h\n", tx_en, txd);
    initial begin
        $readmemh("stim.hex", stim);
        txf = $fopen("tx.txt", "w");
        // Cycle 0 is the first eth_rx_clk cycle out of reset, matching the
        // cycle model (and tests/hdl.rs).
        @(negedge dut.eth_rst);
        $fwrite(summary, "rx_start_ns %0.3f\n", $realtime);
        #1;
        running = 1'b1;
        for (k = 0; k < CYCLES; k = k + 1) begin
            {rxerr, dv, rxd} = stim[k][5:0];
            @(posedge rx_clk);
        end
        // Let the TX FIFO drain.
        repeat (40) @(posedge tx_clk);
        running = 1'b0;
        // Let the last frame event and the activity holdoff reach the LEDs.
        #(TAIL_NS);
        $fwrite(summary, "end_ns %0.3f\n", $realtime);
        $fclose(txf);
        $fclose(ledf);
        $fclose(summary);
        $finish;
    end
endmodule
