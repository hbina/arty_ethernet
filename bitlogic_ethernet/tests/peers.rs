//! ARP peer cache on the cycle model of `eth_core.v`. After every frame the
//! cache contents are compared with an independent model of the RFC 826 merge
//! rules (refresh a cached sender; add a sender only when we are the target;
//! never learn 0.0.0.0; fill vacant slots, then replace round-robin). Replies, the
//! protocol drop counter and the beacon's PEERS count are checked on the wire.
mod common;

use bitlogic_ethernet::proto::PEER_CACHE_ENTRIES;
use bitlogic_ethernet::sim::{CoreConfig, EthCore, Native, RxNibble};
use common::*;

/// An ARP frame with every field the cache logic looks at.
#[derive(Clone, Copy, Debug)]
struct Arp {
    dst: [u8; 6],
    opcode: u16,
    hw_len: u8,
    sender_mac: [u8; 6],
    sender_ip: [u8; 4],
    target_ip: [u8; 4],
}

impl Arp {
    fn frame(&self) -> Vec<u8> {
        let mut arp = vec![0x00, 0x01, 0x08, 0x00, self.hw_len, 4];
        arp.extend(self.opcode.to_be_bytes());
        arp.extend(self.sender_mac);
        arp.extend(self.sender_ip);
        arp.extend([0; 6]);
        arp.extend(self.target_ip);
        let mut body = ethernet(self.dst, self.sender_mac, 0x0806, &arp);
        body.resize(60, 0);
        body
    }
    /// Well-formed Ethernet/IPv4 request or reply addressed to us.
    fn valid(&self) -> bool {
        (self.dst == BROADCAST || self.dst == FPGA_MAC)
            && matches!(self.opcode, 1 | 2)
            && self.hw_len == 6
    }
    fn for_us(&self) -> bool {
        self.valid() && self.target_ip == FPGA_IP
    }
    fn wants_reply(&self) -> bool {
        self.for_us() && self.opcode == 1
    }
}

/// Independent model: slot array plus the next slot to replace when full.
#[derive(Default)]
struct Model {
    slots: [Option<([u8; 4], [u8; 6])>; PEER_CACHE_ENTRIES],
    next: usize,
}

impl Model {
    /// Apply one frame; returns whether the frame was used by the cache.
    fn apply(&mut self, f: &Arp) -> bool {
        if !f.valid() || f.sender_ip == [0; 4] {
            return false;
        }
        if let Some(slot) = self
            .slots
            .iter_mut()
            .flatten()
            .find(|(ip, _)| *ip == f.sender_ip)
        {
            slot.1 = f.sender_mac;
            true
        } else if f.for_us() {
            let entry = Some((f.sender_ip, f.sender_mac));
            if let Some(free) = self.slots.iter_mut().find(|s| s.is_none()) {
                *free = entry;
            } else {
                self.slots[self.next] = entry;
                self.next = (self.next + 1) % PEER_CACHE_ENTRIES;
            }
            true
        } else {
            false
        }
    }
    fn count(&self) -> u32 {
        self.slots.iter().flatten().count() as u32
    }
}

fn assert_cache(core: &EthCore<Native>, model: &Model, context: &str) {
    let cache = core.proto.peers;
    assert_eq!(
        cache.next.get() as usize,
        model.next,
        "next slot: {context}"
    );
    for (k, (entry, want)) in cache.entries.iter().zip(&model.slots).enumerate() {
        let got = entry.valid.then(|| {
            let ip = (entry.key.get() as u32).to_be_bytes();
            let mac = entry.value.get().to_be_bytes();
            (ip, [mac[2], mac[3], mac[4], mac[5], mac[6], mac[7]])
        });
        assert_eq!(got, *want, "slot {k}: {context}");
    }
}

#[test]
fn random_arp_traffic_matches_rfc826_model() {
    const FRAMES: usize = 400;
    const GAP: usize = 600;
    let config = CoreConfig {
        // One beacon after all traffic, to read the counters.
        beacon_interval: (FRAMES * (168 + GAP) + 2_000) as u32,
        garp_interval: u32::MAX,
        ..CoreConfig::default()
    };
    let mut core = EthCore::new(Native, config);
    let mut model = Model::default();
    let mut rng = Rng(0xa49_cac4e);
    // 24 candidate peers, so a full cache must replace entries; 0.0.0.0 is
    // an RFC 5227 probe.
    let ips: Vec<[u8; 4]> = (0..24u8)
        .map(|k| if k == 23 { [0; 4] } else { [10, 1, 2, k] })
        .collect();
    let (mut trace, mut expected, mut drops, mut replies) = (Vec::new(), Vec::new(), 0, 0);
    for n in 0..FRAMES {
        let pick = |rng: &mut Rng, n: u64| rng.next() % n;
        let f = Arp {
            dst: [BROADCAST, FPGA_MAC, [2, 0, 0, 0, 0, 9]][match pick(&mut rng, 10) {
                0 => 2,
                1..=3 => 1,
                _ => 0,
            }],
            opcode: [1, 2, 3][match pick(&mut rng, 10) {
                0 => 2,
                1..=3 => 1,
                _ => 0,
            }],
            hw_len: if pick(&mut rng, 20) == 0 { 8 } else { 6 },
            sender_mac: [2, 0x5e, 0, 0, pick(&mut rng, 4) as u8, n as u8],
            sender_ip: ips[pick(&mut rng, ips.len() as u64) as usize],
            target_ip: if pick(&mut rng, 3) == 0 {
                [10, 1, 2, 200]
            } else {
                FPGA_IP
            },
        };
        let used = model.apply(&f);
        if f.wants_reply() {
            replies += 1;
            expected.push(arp_reply(f.sender_mac, f.sender_mac, f.sender_ip));
        } else if !used {
            drops += 1;
        }
        let mut stim = mii_frame(&f.frame(), 24);
        stim.resize(168 + GAP, RxNibble::default());
        trace.extend(run(&mut core, &stim));
        assert_cache(&core, &model, &format!("frame {n}: {f:?}"));
    }
    assert!(
        model.count() as usize == PEER_CACHE_ENTRIES,
        "cache never filled"
    );
    trace.extend(run(&mut core, &idle(4_000)));
    expected.push(beacon(Counts {
        rx: FRAMES as u32,
        rx_proto_drops: drops,
        arp: replies,
        peers: model.count(),
        ..Counts::default()
    }));
    let frames: Vec<_> = decode(&trace).into_iter().map(|f| f.body).collect();
    assert_eq!(frames.len(), expected.len());
    for (k, (got, want)) in frames.iter().zip(&expected).enumerate() {
        assert_eq!(got, want, "frame {k}");
    }
}

#[test]
fn replies_refresh_and_probes_follow_rfc826() {
    let mut core = EthCore::new(
        Native,
        CoreConfig {
            beacon_interval: u32::MAX,
            garp_interval: u32::MAX,
            ..CoreConfig::default()
        },
    );
    let mut model = Model::default();
    let base = Arp {
        dst: BROADCAST,
        opcode: 1,
        hw_len: 6,
        sender_mac: HOST_MAC,
        sender_ip: HOST_IP,
        target_ip: [10, 9, 9, 9],
    };
    let steps = [
        // Not for us and unknown: ignored.
        ("unknown, not for us", base),
        // An unsolicited reply to us adds the sender.
        (
            "reply to us",
            Arp {
                dst: FPGA_MAC,
                opcode: 2,
                target_ip: FPGA_IP,
                ..base
            },
        ),
        // Known sender talking to someone else: MAC refreshed.
        (
            "known, not for us",
            Arp {
                sender_mac: [2, 0xaa, 0, 0, 0, 1],
                ..base
            },
        ),
        // A probe for our address must not add 0.0.0.0.
        (
            "probe",
            Arp {
                sender_ip: [0; 4],
                target_ip: FPGA_IP,
                ..base
            },
        ),
        // Malformed hardware length: ignored even from a known sender.
        (
            "bad hw_len",
            Arp {
                hw_len: 8,
                sender_mac: [2, 0xbb, 0, 0, 0, 2],
                ..base
            },
        ),
    ];
    for (name, f) in steps {
        model.apply(&f);
        let mut stim = mii_frame(&f.frame(), 24);
        stim.resize(800, RxNibble::default());
        run(&mut core, &stim);
        assert_cache(&core, &model, name);
    }
    assert_eq!(model.count(), 1);
    assert_eq!(model.slots[0], Some((HOST_IP, [2, 0xaa, 0, 0, 0, 1])));
}
