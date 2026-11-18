//! ICMP echo and receive-FCS behavior on the cycle model of `eth_core.v`.
//! Expected replies are built from scratch by `common::Echo::reply`, with
//! checksums computed by an RFC 1071 implementation, not by the design's
//! incremental update.
mod common;

use bitlogic_ethernet::sim::{CoreConfig, EthCore, Native, RxNibble};
use common::*;

fn quiet() -> CoreConfig {
    CoreConfig {
        beacon_interval: u32::MAX,
        garp_interval: u32::MAX,
        ..CoreConfig::default()
    }
}

/// Pad a frame body to the Ethernet minimum with `fill`, as senders do.
fn padded(mut body: Vec<u8>, fill: u8) -> Vec<u8> {
    if body.len() < 60 {
        body.resize(60, fill);
    }
    body
}

fn bodies(frames: &[TxFrame]) -> Vec<Vec<u8>> {
    frames.iter().map(|f| f.body.clone()).collect()
}

/// Run `stim` (padded with idle cycles to `cycles`) and decode the TX frames.
fn transmit(config: CoreConfig, mut stim: Vec<RxNibble>, cycles: usize) -> Vec<Vec<u8>> {
    stim.resize(cycles, RxNibble::default());
    bodies(&decode(&run(&mut EthCore::new(Native, config), &stim)))
}

#[test]
fn echo_requests_get_byte_exact_replies() {
    // Empty, odd and typical payloads, and the largest unfragmented one.
    for (k, size) in [0usize, 1, 17, 56, 1472].into_iter().enumerate() {
        let payload: Vec<u8> = (0..size).map(|b| (b * 7 + k) as u8).collect();
        let mut request = Echo::request(0x4242, k as u16 + 1, payload);
        request.tos = 0x10;
        request.id = 0xbeef - k as u16;
        // Nonzero sender padding must not leak into checksums or the reply.
        let frame = padded(request.frame(), 0xab);
        let replies = transmit(quiet(), mii_frame(&frame, 24), 10_000);
        assert_eq!(
            replies,
            vec![padded(request.reply().frame(), 0)],
            "payload {size}"
        );
    }
}

/// Each mutation of a valid request that a host must not answer.
fn invalid_requests() -> Vec<(&'static str, Echo)> {
    let base = Echo::request(7, 9, b"abcdefghijklmnopqrstuvwxyz".to_vec());
    let good_ip = {
        let f = base.frame();
        u16::from_be_bytes([f[24], f[25]])
    };
    let good_icmp = {
        let f = base.frame();
        u16::from_be_bytes([f[36], f[37]])
    };
    let with = |f: &dyn Fn(&mut Echo)| {
        let mut e = base.clone();
        f(&mut e);
        e
    };
    vec![
        ("other IP", with(&|e| e.dst_ip = [192, 168, 1, 101])),
        ("broadcast MAC", with(&|e| e.dst_mac = BROADCAST)),
        ("other MAC", with(&|e| e.dst_mac = [2, 0, 0, 0, 0, 2])),
        (
            "bad IP checksum",
            with(&|e| e.ip_checksum = Some(good_ip ^ 1)),
        ),
        (
            "bad ICMP checksum",
            with(&|e| e.icmp_checksum = Some(good_icmp ^ 0x100)),
        ),
        ("more fragments", with(&|e| e.flags_frag = 0x2000)),
        ("fragment offset", with(&|e| e.flags_frag = 0x0001)),
        ("IP options", with(&|e| e.options = vec![1, 1, 1, 0])),
        ("UDP protocol", with(&|e| e.protocol = 17)),
        ("echo reply", with(&|e| e.icmp_type = 0)),
        ("nonzero code", with(&|e| e.icmp_code = 1)),
        (
            "length beyond frame",
            with(&|e| e.total_len = Some(20 + 8 + 26 + 1)),
        ),
        ("length below header", with(&|e| e.total_len = Some(27))),
    ]
}

#[test]
fn invalid_requests_are_counted_not_answered() {
    for (name, request) in invalid_requests() {
        let config = CoreConfig {
            beacon_interval: 1_000,
            ..quiet()
        };
        let frame = padded(request.frame(), 0);
        let replies = transmit(config, mii_frame(&frame, 24), 1_700);
        let counts = Counts {
            rx: 1,
            rx_proto_drops: 1,
            ..Counts::default()
        };
        assert_eq!(replies, vec![beacon(counts)], "{name}");
    }
}

#[test]
fn bad_fcs_frames_are_dropped_and_counted() {
    let config = CoreConfig {
        beacon_interval: 3_000,
        ..quiet()
    };
    let ping = Echo::request(1, 1, vec![5; 40]);
    let mut stim = Vec::new();
    // A single flipped bit, then a burst error, then a good ARP request.
    stim.extend(mii_frame_bad_fcs(&ping.frame(), 24, 1));
    stim.extend(mii_frame_bad_fcs(
        &arp_request(HOST_MAC, HOST_IP, FPGA_IP),
        24,
        0xff00_0000,
    ));
    stim.extend(mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24));
    let replies = transmit(config, stim, 3_700);
    let counts = Counts {
        rx: 1,
        arp: 1,
        fcs: 2,
        ..Counts::default()
    };
    assert_eq!(
        replies,
        vec![arp_reply(HOST_MAC, HOST_MAC, HOST_IP), beacon(counts)]
    );
}

#[test]
fn beacon_reports_echo_and_fcs_counters() {
    let config = CoreConfig {
        beacon_interval: 4_000,
        ..quiet()
    };
    let mut stim = Vec::new();
    let mut expected = Vec::new();
    for seq in 0..3 {
        let ping = Echo::request(9, seq, vec![seq as u8; 20]);
        stim.extend(mii_frame(&padded(ping.frame(), 0), 24));
        expected.push(padded(ping.reply().frame(), 0));
    }
    stim.extend(mii_frame_bad_fcs(
        &Echo::request(9, 9, vec![]).frame(),
        24,
        4,
    ));
    expected.push(beacon(Counts {
        rx: 3,
        icmp: 3,
        fcs: 1,
        ..Counts::default()
    }));
    assert_eq!(transmit(config, stim, 4_700), expected);
}

#[test]
fn timer_tick_during_long_echo_is_not_lost() {
    // The 1472-byte request is parsed and copied over roughly cycles
    // 3,050..6,100; the beacon tick at 4,999 lands inside that window.
    let config = CoreConfig {
        beacon_interval: 5_000,
        ..quiet()
    };
    let ping = Echo::request(3, 3, vec![0x5a; 1472]);
    let replies = transmit(config, mii_frame(&ping.frame(), 24), 9_800);
    let counts = Counts {
        rx: 1,
        icmp: 1,
        ..Counts::default()
    };
    assert_eq!(replies, vec![ping.reply().frame(), beacon(counts)]);
}

#[test]
fn random_traffic_matches_oracle() {
    let mut rng = Rng(0x1c3b_9e01);
    let invalid = invalid_requests();
    let mut stim = Vec::new();
    let mut expected = Vec::new();
    for seq in 0..40u16 {
        let size = (rng.next() % 300) as usize;
        let payload: Vec<u8> = (0..size).map(|_| rng.next() as u8).collect();
        let mut request = Echo::request(rng.next() as u16, seq, payload);
        request.id = rng.next() as u16;
        request.tos = rng.next() as u8;
        let roll = rng.next() % 8;
        // 0..=4: valid; 5: invalid mutation; 6: bad FCS; 7: ARP request.
        if roll == 5 {
            let (_, mut bad) = invalid[(rng.next() as usize) % invalid.len()].clone();
            bad.seq = seq;
            stim.extend(mii_frame(&padded(bad.frame(), 0x33), 24));
        } else if roll == 6 {
            let flip = 1u32 << (rng.next() % 32);
            stim.extend(mii_frame_bad_fcs(&request.frame(), 24, flip));
        } else if roll == 7 {
            stim.extend(mii_frame(&arp_request(HOST_MAC, HOST_IP, FPGA_IP), 24));
            expected.push(arp_reply(HOST_MAC, HOST_MAC, HOST_IP));
        } else {
            stim.extend(mii_frame(&padded(request.frame(), 0xee), 24));
            expected.push(padded(request.reply().frame(), 0));
        }
        // Leave room for the reply so the TX ring never overflows here.
        stim.extend(idle(800));
    }
    let cycles = stim.len() + 2_000;
    assert_eq!(transmit(quiet(), stim, cycles), expected);
}
