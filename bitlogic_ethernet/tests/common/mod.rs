//! Test helpers written independently of the design: frame builders, an
//! MSB-first CRC-32 (the design uses the reflected LSB-first form), MII
//! nibble encoding and a TX stream decoder.
#![allow(dead_code)]

use bitlogic_ethernet::sim::{EthCore, RxNibble, Steps, TxNibble};

pub const HOST_MAC: [u8; 6] = [0x02, 0x11, 0x22, 0x33, 0x44, 0x55];
pub const HOST_IP: [u8; 4] = [192, 168, 1, 10];
pub const FPGA_MAC: [u8; 6] = [0x02, 0x00, 0x00, 0x00, 0x00, 0x01];
pub const FPGA_IP: [u8; 4] = [192, 168, 1, 100];
pub const BROADCAST: [u8; 6] = [0xff; 6];

/// IEEE 802.3 CRC-32 computed MSB-first on bit-reversed bytes.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xffff_ffff;
    for &byte in data {
        crc ^= u32::from(byte.reverse_bits()) << 24;
        for _ in 0..8 {
            crc = if crc & 0x8000_0000 != 0 {
                (crc << 1) ^ 0x04C1_1DB7
            } else {
                crc << 1
            };
        }
    }
    !crc.reverse_bits()
}

#[test]
fn crc_check_value() {
    assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
}

pub fn ethernet(dst: [u8; 6], src: [u8; 6], ethertype: u16, payload: &[u8]) -> Vec<u8> {
    let mut body = Vec::new();
    body.extend(dst);
    body.extend(src);
    body.extend(ethertype.to_be_bytes());
    body.extend(payload);
    body
}

/// ARP request as a typical host sends it: padded to the 60-byte minimum.
pub fn arp_request(sender_mac: [u8; 6], sender_ip: [u8; 4], target_ip: [u8; 4]) -> Vec<u8> {
    let mut arp = vec![0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x01];
    arp.extend(sender_mac);
    arp.extend(sender_ip);
    arp.extend([0; 6]);
    arp.extend(target_ip);
    let mut body = ethernet(BROADCAST, sender_mac, 0x0806, &arp);
    body.resize(60, 0);
    body
}

/// The ARP reply/announcement the endpoint should send, padded to 60 bytes.
pub fn arp_reply(dst: [u8; 6], target_mac: [u8; 6], target_ip: [u8; 4]) -> Vec<u8> {
    let mut arp = vec![0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x02];
    arp.extend(FPGA_MAC);
    arp.extend(FPGA_IP);
    arp.extend(target_mac);
    arp.extend(target_ip);
    let mut body = ethernet(dst, FPGA_MAC, 0x0806, &arp);
    body.resize(60, 0);
    body
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Counts {
    pub rx: u32,
    pub rx_queue_drops: u32,
    pub rx_proto_drops: u32,
    pub tx_drops: u32,
    pub arp: u32,
    pub up: u32,
}

/// Expected diagnostics beacon frame body (143 bytes, above the minimum).
pub fn beacon(c: Counts) -> Vec<u8> {
    let text = format!(
        "ARTY IP=192.168.001.100 MAC=020000000001 RX={:08X} RXQ={:08X} RXP={:08X} \
         TXD={:08X} ARP={:08X} UDP=00000000 UP={:08X}",
        c.rx, c.rx_queue_drops, c.rx_proto_drops, c.tx_drops, c.arp, c.up
    );
    assert_eq!(text.len(), 129);
    ethernet(BROADCAST, FPGA_MAC, 0x88B5, text.as_bytes())
}

/// Wire encoding: preamble, SFD, body, FCS (least significant byte first),
/// low nibble first, followed by `gap` idle cycles.
pub fn mii_frame(body: &[u8], gap: usize) -> Vec<RxNibble> {
    let mut wire = vec![0x55; 7];
    wire.push(0xd5);
    wire.extend(body);
    wire.extend(crc32(body).to_le_bytes());
    let mut out = Vec::new();
    for byte in wire {
        for rxd in [byte & 0xf, byte >> 4] {
            out.push(RxNibble {
                dv: true,
                rxd,
                rxerr: false,
            });
        }
    }
    out.extend(idle(gap));
    out
}

pub fn idle(cycles: usize) -> Vec<RxNibble> {
    vec![RxNibble::default(); cycles]
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TxFrame {
    /// Cycle of the first asserted tx_en.
    pub start: usize,
    /// Cycle after the last asserted tx_en.
    pub end: usize,
    /// Frame body with padding, preamble/SFD and FCS removed and checked.
    pub body: Vec<u8>,
}

/// Decode the TX trace into frames, checking preamble, SFD and FCS.
pub fn decode(trace: &[TxNibble]) -> Vec<TxFrame> {
    let mut frames = Vec::new();
    let mut k = 0;
    while k < trace.len() {
        if !trace[k].en {
            k += 1;
            continue;
        }
        let start = k;
        let mut nibbles = Vec::new();
        while k < trace.len() && trace[k].en {
            nibbles.push(trace[k].txd);
            k += 1;
        }
        assert!(k < trace.len(), "trace ended inside a frame");
        assert_eq!(nibbles.len() % 2, 0, "odd nibble count at cycle {start}");
        let bytes: Vec<u8> = nibbles.chunks(2).map(|n| n[0] | n[1] << 4).collect();
        assert_eq!(
            &bytes[..8],
            &[0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xd5]
        );
        let (body, fcs) = bytes[8..].split_at(bytes.len() - 12);
        assert_eq!(
            u32::from_le_bytes(fcs.try_into().unwrap()),
            crc32(body),
            "bad FCS on frame at cycle {start}"
        );
        frames.push(TxFrame {
            start,
            end: k,
            body: body.to_vec(),
        });
    }
    frames
}

pub fn run<S: Steps>(core: &mut EthCore<S>, stimulus: &[RxNibble]) -> Vec<TxNibble> {
    stimulus.iter().map(|&input| core.clock(input)).collect()
}

/// Deterministic 64-bit generator (xorshift64*).
pub struct Rng(pub u64);

impl Rng {
    pub fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
}
