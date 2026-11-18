//! Board-level simulation of `rtl/arty_top.v` in Vivado xsim with the real
//! Xilinx PLL/BUFG/ODDR simulation models, an asynchronous PHY RX clock, the
//! reset synchronizer, both clock-domain crossings and the LED unit.
//!
//! Checks, independent of the design where possible:
//! - the PLL locks, `eth_rstn` releases, `eth_ref_clk` runs at 25 MHz;
//! - the TX stream matches the Rust cycle model exactly from the first
//!   post-reset cycle (so the board wrapper adds nothing to the core's I/O);
//! - the decoded frames are the expected ARP reply, ICMP echo reply, beacons
//!   with the right counters and a gratuitous ARP (frames from
//!   `tests/common`, FCS checked); a frame with a bad FCS is not answered;
//! - the LEDs count every transmitted frame through the 25 → 100 MHz toggle
//!   synchronizer without skipping, and the activity LED lights and clears.
//!
//! Set `BITLOGIC_ETHERNET_SKIP_HDL=1` to skip on machines without Vivado.
mod common;

use bitlogic_ethernet::hdl::generated_files;
use bitlogic_ethernet::sim::{CoreConfig, EthCore, Native, RxNibble};
use common::*;
use std::{fs, path::PathBuf};

const CYCLES: usize = 10_000;
const BEACON_INTERVAL: u32 = 3_000;
const GARP_INTERVAL: u32 = 7_000;
const LED_HOLDOFF: u32 = 200;

fn summary_value(text: &str, key: &str) -> f64 {
    text.lines()
        .find_map(|l| l.strip_prefix(key)?.trim().parse().ok())
        .unwrap_or_else(|| panic!("{key} missing from summary:\n{text}"))
}

#[test]
fn board_top_simulates_with_vivado_primitives() {
    if std::env::var_os("BITLOGIC_ETHERNET_SKIP_HDL").is_some() {
        eprintln!("SKIPPED: BITLOGIC_ETHERNET_SKIP_HDL is set");
        return;
    }
    let bin = xilinx_bin();

    // Good ARP request, ARP for another IP, and a frame for another MAC.
    let mut stim: Vec<RxNibble> = idle(200);
    stim.extend(mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24));
    stim.extend(mii_frame(
        &arp_request(HOST_MAC, HOST_IP, [192, 168, 1, 99]),
        24,
    ));
    stim.extend(mii_frame(
        &ethernet([0x02, 0x99, 0, 0, 0, 7], HOST_MAC, 0x0800, &[3; 80]),
        24,
    ));
    let ping = Echo::request(0x77, 1, (0..56).collect());
    stim.extend(mii_frame(&ping.frame(), 24));
    stim.extend(mii_frame_bad_fcs(&ping.frame(), 24, 0x0100_0000));
    stim.resize(CYCLES, RxNibble::default());

    let config = CoreConfig {
        beacon_interval: BEACON_INTERVAL,
        garp_interval: GARP_INTERVAL,
        ..CoreConfig::default()
    };
    let expected = run(&mut EthCore::new(Native, config), &stim);

    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("xsim_arty_top");
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    let mut sources = Vec::new();
    for (name, text) in generated_files().unwrap() {
        fs::write(dir.join(&name), text).unwrap();
        sources.push(name);
    }
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    for path in [
        "rtl/sdp_ram.v",
        "rtl/eth_core.v",
        "rtl/arty_top.v",
        "sim/tb_arty_top.v",
    ] {
        let name = path.rsplit('/').next().unwrap().to_owned();
        fs::copy(manifest.join(path), dir.join(&name)).unwrap();
        sources.push(name);
    }
    let glbl = bin.join("../data/verilog/src/glbl.v");
    fs::copy(&glbl, dir.join("glbl.v")).unwrap();
    sources.push("glbl.v".into());
    fs::write(dir.join("stim.hex"), stim_hex(&stim)).unwrap();

    let step = |tool: &str, args: &[&str]| run_tool(&bin, &dir, tool, args);
    let mut xvlog = vec!["--nolog"];
    xvlog.extend(sources.iter().map(String::as_str));
    step("xvlog", &xvlog);
    let generics = [
        format!("CYCLES={CYCLES}"),
        format!("BEACON_INTERVAL={BEACON_INTERVAL}"),
        format!("GARP_INTERVAL={GARP_INTERVAL}"),
        format!("LED_HOLDOFF={LED_HOLDOFF}"),
    ];
    let mut xelab = vec![
        "--nolog",
        "--debug",
        "typical",
        "-L",
        "unisims_ver",
        "tb_arty_top",
        "glbl",
        "-s",
        "tb_arty_top_sim",
    ];
    for g in &generics {
        xelab.extend(["-generic_top", g.as_str()]);
    }
    step("xelab", &xelab);
    step("xsim", &["--nolog", "tb_arty_top_sim", "-R"]);

    // Clocking and reset.
    let summary = fs::read_to_string(dir.join("summary.txt")).unwrap();
    let lock = summary_value(&summary, "lock_ns");
    let period = summary_value(&summary, "ref_period_ns");
    let rx_start = summary_value(&summary, "rx_start_ns");
    assert!(lock > 0.0 && lock < 100_000.0, "PLL lock at {lock} ns");
    assert!(
        (period - 40.0).abs() < 0.01,
        "eth_ref_clk period {period} ns"
    );
    assert!(rx_start > lock, "Ethernet reset released before PLL lock");

    // TX: cycle-exact against the model, then frame contents.
    let actual = parse_tx(&fs::read_to_string(dir.join("tx.txt")).unwrap());
    assert_eq!(actual.len(), expected.len());
    if let Some(k) = (0..expected.len()).find(|&k| actual[k] != expected[k]) {
        panic!(
            "first mismatch at cycle {k}: board {:?} model {:?}",
            actual[k], expected[k]
        );
    }
    let frames = decode(&actual);
    let counts = Counts {
        rx: 4,
        rx_proto_drops: 2,
        arp: 1,
        fcs: 1,
        icmp: 1,
        ..Counts::default()
    };
    let bodies: Vec<_> = frames.iter().map(|f| f.body.clone()).collect();
    assert_eq!(
        bodies,
        vec![
            arp_reply(HOST_MAC, HOST_MAC, HOST_IP),
            ping.reply().frame(),
            beacon(counts),
            beacon(Counts { up: 1, ..counts }),
            arp_reply(BROADCAST, BROADCAST, FPGA_IP),
            beacon(Counts { up: 2, ..counts }),
        ]
    );

    // LEDs: led[3:1] counts TX frames, led[0] is the activity holdoff.
    let leds: Vec<(u64, u8)> = fs::read_to_string(dir.join("led.txt"))
        .unwrap()
        .lines()
        .map(|l| {
            let (t, v) = l.split_once(' ').unwrap();
            let v = u8::from_str_radix(v, 16)
                .unwrap_or_else(|_| panic!("undefined LED value {v:?} at {t} ps"));
            (t.parse().unwrap(), v)
        })
        .collect();
    let mut counts_seen = vec![0u8];
    for &(_, v) in &leds {
        let count = v >> 1;
        if count != *counts_seen.last().unwrap() {
            counts_seen.push(count);
        }
    }
    let expected_counts: Vec<u8> = (0..=frames.len() as u8).map(|n| n % 8).collect();
    assert_eq!(counts_seen, expected_counts, "LED frame counter: {leds:?}");
    assert!(
        leds.iter().any(|&(_, v)| v & 1 == 1),
        "activity LED never lit"
    );
    let last = leds.last().unwrap().1;
    assert_eq!(last & 1, 0, "activity LED did not clear after the holdoff");
}
