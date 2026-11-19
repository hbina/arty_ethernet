//! Frame-level behavior of the whole core (cycle model of `rtl/eth_core.v`).
mod common;

use bitlogic_ethernet::sim::{CoreConfig, EthCore, Interpreted, Native, RxNibble};
use common::*;

/// Timers far beyond each test unless a test shortens them.
fn quiet() -> CoreConfig {
    CoreConfig {
        beacon_interval: u32::MAX,
        garp_interval: u32::MAX,
        ..CoreConfig::default()
    }
}

fn bodies(frames: &[TxFrame]) -> Vec<Vec<u8>> {
    frames.iter().map(|f| f.body.clone()).collect()
}

#[test]
fn arp_request_gets_unicast_reply() {
    let mut core = EthCore::new(Native, quiet());
    let mut stim = mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24);
    stim.extend(idle(1000));
    let frames = decode(&run(&mut core, &stim));
    assert_eq!(
        bodies(&frames),
        vec![arp_reply(HOST_MAC, HOST_MAC, HOST_IP)]
    );
}

#[test]
fn ignored_frames_produce_no_traffic() {
    let mut core = EthCore::new(Native, quiet());
    let mut stim = Vec::new();
    // ARP for another IP, an IPv4 frame, an ARP *reply*, a unicast ARP to
    // another MAC, and an ARP request that is too short.
    stim.extend(mii_frame(
        &arp_request(HOST_MAC, HOST_IP, [192, 168, 1, 7]),
        24,
    ));
    stim.extend(mii_frame(
        &ethernet(FPGA_MAC, HOST_MAC, 0x0800, &[0x45; 46]),
        24,
    ));
    let mut reply = arp_request(HOST_MAC, HOST_IP, FPGA_IP);
    reply[21] = 2;
    stim.extend(mii_frame(&reply, 24));
    let mut other = arp_request(HOST_MAC, HOST_IP, FPGA_IP);
    other[..6].copy_from_slice(&[0x02, 0, 0, 0, 0, 0x09]);
    stim.extend(mii_frame(&other, 24));
    stim.extend(mii_frame(
        &arp_request(HOST_MAC, HOST_IP, FPGA_IP)[..40],
        24,
    ));
    stim.extend(idle(2000));
    assert!(decode(&run(&mut core, &stim)).is_empty());
}

#[test]
fn beacon_reports_counters() {
    let config = CoreConfig {
        beacon_interval: 40_000,
        ..quiet()
    };
    let mut core = EthCore::new(Native, config);
    let mut stim = Vec::new();
    // Published: good ARP, wrong-IP ARP, IPv4, oversized (truncated) frame.
    stim.extend(mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24));
    stim.extend(mii_frame(
        &arp_request(HOST_MAC, HOST_IP, [10, 0, 0, 1]),
        24,
    ));
    stim.extend(mii_frame(
        &ethernet(FPGA_MAC, HOST_MAC, 0x0800, &[1; 100]),
        24,
    ));
    stim.extend(mii_frame(
        &ethernet(FPGA_MAC, HOST_MAC, 0x0800, &[2; 1600]),
        24,
    ));
    // Not published: receive error, and a runt shorter than header + FCS.
    let mut errored = mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24);
    errored[40].rxerr = true;
    stim.extend(errored);
    stim.extend(mii_frame(&[0xaa; 10], 24));
    stim.resize(81_000, RxNibble::default());
    let frames = decode(&run(&mut core, &stim));

    // The wrong-IP request is not answered, but its sender is already
    // cached, so it refreshes that entry (RFC 826) instead of being dropped.
    let first = Counts {
        rx: 4,
        rx_proto_drops: 2,
        arp: 1,
        peers: 1,
        ..Counts::default()
    };
    let second = Counts { up: 1, ..first };
    assert_eq!(
        bodies(&frames),
        vec![
            arp_reply(HOST_MAC, HOST_MAC, HOST_IP),
            beacon(first),
            beacon(second)
        ]
    );
    // The first beacon starts on the 40,000th cycle's tick (plus pipeline).
    assert!(
        (40_000..40_400).contains(&frames[1].start),
        "{}",
        frames[1].start
    );
    assert!(
        (80_000..80_400).contains(&frames[2].start),
        "{}",
        frames[2].start
    );
}

#[test]
fn maximum_size_frame_is_not_truncated() {
    let config = CoreConfig {
        beacon_interval: 8_000,
        ..quiet()
    };
    let mut core = EthCore::new(Native, config);
    let mut stim = mii_frame(&ethernet(FPGA_MAC, HOST_MAC, 0x0800, &[3; 1500]), 24);
    stim.resize(8_600, RxNibble::default());
    let frames = decode(&run(&mut core, &stim));
    // Counted as a protocol drop (not ARP), but RX accepted the full size.
    let expected = Counts {
        rx: 1,
        rx_proto_drops: 1,
        ..Counts::default()
    };
    assert_eq!(bodies(&frames), vec![beacon(expected)]);
    let stored = &core.rx;
    assert_eq!(stored.wr_ptr.get(), 1);
}

#[test]
fn gratuitous_arp_is_broadcast_with_own_ip() {
    let config = CoreConfig {
        garp_interval: 5_000,
        ..quiet()
    };
    let mut core = EthCore::new(Native, config);
    let frames = decode(&run(&mut core, &idle(10_400)));
    let announce = arp_reply(BROADCAST, BROADCAST, FPGA_IP);
    assert_eq!(bodies(&frames), vec![announce.clone(), announce]);
}

#[test]
fn simultaneous_ticks_send_gratuitous_arp_then_beacon() {
    // Ticks stay pending until served: the gratuitous ARP goes first and the
    // coinciding beacon follows it. (The HLS design drops the beacon.)
    let config = CoreConfig {
        beacon_interval: 3_000,
        garp_interval: 3_000,
        ..CoreConfig::default()
    };
    let mut core = EthCore::new(Native, config);
    let frames = decode(&run(&mut core, &idle(9_800)));
    let announce = arp_reply(BROADCAST, BROADCAST, FPGA_IP);
    let mut expected = Vec::new();
    for up in 0..3 {
        expected.push(announce.clone());
        expected.push(beacon(Counts {
            up,
            ..Counts::default()
        }));
    }
    assert_eq!(bodies(&frames), expected);
}

#[test]
fn tx_frames_are_separated_by_interframe_gap() {
    let mut core = EthCore::new(Native, quiet());
    let mut stim = Vec::new();
    for _ in 0..3 {
        stim.extend(mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24));
    }
    stim.extend(idle(2000));
    let frames = decode(&run(&mut core, &stim));
    assert_eq!(frames.len(), 3);
    for pair in frames.windows(2) {
        assert!(pair[1].start - pair[0].end >= 24, "{pair:?}");
    }
    // Preamble 16 + body 120 + FCS 8 nibbles.
    assert!(frames.iter().all(|f| f.end - f.start == 144));
}

#[test]
fn tx_ring_overflow_counts_drops() {
    // Back-to-back requests arrive faster than replies leave (ARP request on
    // the wire: 168 cycles; reply: 144 + 24 IFG + queue latency).
    let config = CoreConfig {
        beacon_interval: 12_000,
        ..quiet()
    };
    let mut core = EthCore::new(Native, config);
    let mut stim = Vec::new();
    let requests = 40;
    for k in 0..requests {
        let mut mac = HOST_MAC;
        mac[5] = k as u8;
        stim.extend(mii_frame(&arp_request(mac, HOST_IP, FPGA_IP), 12));
    }
    stim.resize(12_600, RxNibble::default());
    let frames = decode(&run(&mut core, &stim));
    let (beacons, replies): (Vec<_>, Vec<_>) =
        frames.iter().partition(|f| f.body[12..14] == [0x88, 0xb5]);
    assert_eq!(beacons.len(), 1);
    let dropped = requests - replies.len() as u32;
    // Every reply that was sent went to a distinct requester, in order.
    let mut last = None;
    for reply in &replies {
        assert_eq!(&reply.body[..5], &HOST_MAC[..5]);
        assert!(last < Some(reply.body[5]));
        last = Some(reply.body[5]);
    }
    let expected = Counts {
        rx: requests,
        tx_drops: dropped,
        arp: requests,
        // One IP with changing MACs: a single, refreshed cache entry.
        peers: 1,
        ..Counts::default()
    };
    assert!(dropped > 0, "the test must actually overflow the TX ring");
    assert_eq!(beacons[0].body, beacon(expected));
}

#[test]
fn interpreted_ir_matches_native_cycle_for_cycle() {
    let config = CoreConfig {
        beacon_interval: 2_500,
        garp_interval: 4_000,
        ..CoreConfig::default()
    };
    let mut stim = Vec::new();
    stim.extend(mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24));
    stim.extend(mii_frame(
        &ethernet(FPGA_MAC, HOST_MAC, 0x0800, &[9; 60]),
        24,
    ));
    stim.extend(mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24));
    let ping = Echo::request(1, 2, vec![0x11; 33]);
    stim.extend(mii_frame(&ping.frame(), 24));
    stim.extend(mii_frame_bad_fcs(&ping.frame(), 24, 0x8000));
    stim.resize(6_000, RxNibble::default());
    let native = run(&mut EthCore::new(Native, config), &stim);
    let interpreted = run(&mut EthCore::new(Interpreted::new(), config), &stim);
    assert_eq!(native, interpreted);
    // Two ARP replies, the echo reply, beacon, gratuitous ARP, beacon.
    let frames = decode(&native);
    assert_eq!(frames.len(), 6);
    assert_eq!(frames[2].body, ping.reply().frame());
}
