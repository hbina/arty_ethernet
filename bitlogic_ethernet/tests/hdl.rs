//! The generated Verilog plus the handwritten `rtl/eth_core.v` must match the
//! Rust cycle model output-for-output. Simulated with Vivado xsim, located via
//! `XILINX_ROOT` (default `~/Xilinx/2025.2`). Set `BITLOGIC_ETHERNET_SKIP_HDL=1`
//! to skip explicitly on machines without Vivado.
mod common;

use bitlogic_ethernet::hdl::generated_files;
use bitlogic_ethernet::sim::{CoreConfig, EthCore, Native, RxNibble, TxNibble};
use common::*;
use std::{fs, path::PathBuf};

fn testbench(cycles: usize, config: CoreConfig) -> String {
    format!(
        r#"`timescale 1ns / 1ps
module tb;
    reg clk = 1'b0;
    always #20 clk = ~clk;
    reg rst = 1'b1;
    reg dv = 1'b0, rxerr = 1'b0;
    reg [3:0] rxd = 4'd0;
    wire tx_en, rx_active, tx_active, toggle;
    wire [3:0] txd;
    reg [7:0] stim [0:{last}];
    integer k, f;
    eth_core #(
        .BEACON_INTERVAL(32'd{beacon}),
        .GARP_INTERVAL(32'd{garp})
    ) dut (
        .clk(clk), .rst(rst), .eth_rx_dv(dv), .eth_rxd(rxd), .eth_rxerr(rxerr),
        .eth_tx_en(tx_en), .eth_txd(txd), .rx_active(rx_active),
        .tx_active(tx_active), .tx_frame_toggle(toggle)
    );
    initial begin
        $readmemh("stim.hex", stim);
        f = $fopen("out.txt", "w");
        repeat (4) @(posedge clk);
        #1 rst = 1'b0;
        for (k = 0; k < {cycles}; k = k + 1) begin
            {{rxerr, dv, rxd}} = stim[k][5:0];
            @(posedge clk);
            #1 $fwrite(f, "%0d%h\n", tx_en, txd);
        end
        $fclose(f);
        $finish;
    end
endmodule
"#,
        last = cycles - 1,
        beacon = config.beacon_interval,
        garp = config.garp_interval,
    )
}

#[test]
fn generated_verilog_matches_cycle_model() {
    if std::env::var_os("BITLOGIC_ETHERNET_SKIP_HDL").is_some() {
        eprintln!("SKIPPED: BITLOGIC_ETHERNET_SKIP_HDL is set");
        return;
    }
    let bin = xilinx_bin();

    let config = CoreConfig {
        beacon_interval: 6_000,
        garp_interval: 9_000,
        ..CoreConfig::default()
    };
    let mut stim: Vec<RxNibble> = Vec::new();
    stim.extend(mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24));
    stim.extend(mii_frame(
        &ethernet(FPGA_MAC, HOST_MAC, 0x0800, &[7; 300]),
        24,
    ));
    let mut errored = mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24);
    errored[60].rxerr = true;
    stim.extend(errored);
    for k in 0..12 {
        let mut mac = HOST_MAC;
        mac[5] = k;
        stim.extend(mii_frame(&arp_request(mac, HOST_IP, FPGA_IP), 12));
    }
    // 20 distinct peers: fills the 16-entry ARP cache and replaces entries.
    for k in 0..20 {
        let mut mac = HOST_MAC;
        mac[4] = k;
        stim.extend(mii_frame(&arp_request(mac, [10, 0, 0, k], FPGA_IP), 24));
    }
    stim.extend(mii_frame(
        &ethernet(FPGA_MAC, HOST_MAC, 0x0800, &[5; 1600]),
        24,
    ));
    // ICMP echo: odd payload, a large one, and one with a corrupted FCS.
    stim.extend(mii_frame(&Echo::request(1, 1, vec![0xa5; 37]).frame(), 24));
    stim.extend(mii_frame(&Echo::request(1, 2, vec![0x3c; 900]).frame(), 24));
    stim.extend(mii_frame_bad_fcs(
        &Echo::request(1, 3, vec![1; 20]).frame(),
        24,
        0x10,
    ));
    stim.resize(30_000, RxNibble::default());

    let mut model = EthCore::new(Native, config);
    let expected: Vec<TxNibble> = run(&mut model, &stim);
    assert!(
        model.proto.peers.entries.iter().all(|e| e.valid) && model.proto.peers.next.get() == 5,
        "the scenario must fill the peer cache and wrap its insertion slot"
    );
    let frames = decode(&expected);
    assert!(
        frames.len() >= 10,
        "scenario should transmit; got {}",
        frames.len()
    );

    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("xsim_eth_core");
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    let mut sources = Vec::new();
    for (name, text) in generated_files().unwrap() {
        fs::write(dir.join(&name), text).unwrap();
        sources.push(name);
    }
    let rtl = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("rtl");
    for name in ["sdp_ram.v", "eth_core.v"] {
        fs::copy(rtl.join(name), dir.join(name)).unwrap();
        sources.push(name.into());
    }
    fs::write(dir.join("tb.v"), testbench(stim.len(), config)).unwrap();
    sources.push("tb.v".into());
    fs::write(dir.join("stim.hex"), stim_hex(&stim)).unwrap();

    let step = |tool: &str, args: &[&str]| run_tool(&bin, &dir, tool, args);
    let mut xvlog = vec!["--nolog"];
    xvlog.extend(sources.iter().map(String::as_str));
    step("xvlog", &xvlog);
    step(
        "xelab",
        &["--nolog", "--debug", "off", "tb", "-s", "tb_sim"],
    );
    step("xsim", &["--nolog", "tb_sim", "-R"]);

    let actual = parse_tx(&fs::read_to_string(dir.join("out.txt")).unwrap());
    assert_eq!(actual.len(), expected.len());
    if let Some(k) = (0..expected.len()).find(|&k| actual[k] != expected[k]) {
        panic!(
            "first mismatch at cycle {k}: hdl {:?} model {:?}",
            actual[k], expected[k]
        );
    }
}
