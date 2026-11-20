//! Test helpers written independently of the design: frame builders, an
//! MSB-first CRC-32 (the design uses the reflected LSB-first form), MII
//! nibble encoding and a TX stream decoder.
#![allow(dead_code)]

use bitlogic_ethernet::sim::{EthCore, RxNibble, Steps, TxNibble};
use std::{
    path::{Path, PathBuf},
    process::Command,
};

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
    pub fcs: u32,
    pub icmp: u32,
    pub peers: u32,
}

/// Expected diagnostics beacon frame body (185 bytes, above the minimum).
pub fn beacon(c: Counts) -> Vec<u8> {
    let text = format!(
        "ARTY IP=192.168.001.100 MAC=020000000001 RX={:08X} RXQ={:08X} RXP={:08X} \
         TXD={:08X} ARP={:08X} UDP=00000000 UP={:08X} FCS={:08X} ICMP={:08X} PEERS={:08X}",
        c.rx, c.rx_queue_drops, c.rx_proto_drops, c.tx_drops, c.arp, c.up, c.fcs, c.icmp, c.peers
    );
    assert_eq!(text.len(), 171);
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

/// As [`mii_frame`], but with the FCS corrupted by XOR with `flip`.
pub fn mii_frame_bad_fcs(body: &[u8], gap: usize, flip: u32) -> Vec<RxNibble> {
    let mut wire = vec![0x55; 7];
    wire.push(0xd5);
    wire.extend(body);
    wire.extend((crc32(body) ^ flip).to_le_bytes());
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

/// RFC 1071 Internet checksum: complement of the ones'-complement sum of
/// big-endian 16-bit words, an odd final byte padded with zero.
pub fn internet_checksum(data: &[u8]) -> u16 {
    let mut sum: u64 = 0;
    for chunk in data.chunks(2) {
        let word = u16::from_be_bytes([chunk[0], *chunk.get(1).unwrap_or(&0)]);
        sum += u64::from(word);
    }
    while sum > 0xffff {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

/// Fields of an IPv4/ICMP echo frame. `echo_frame` fills in both checksums
/// unless overridden, so tests can corrupt any single field.
#[derive(Clone, Debug)]
pub struct Echo {
    pub dst_mac: [u8; 6],
    pub src_mac: [u8; 6],
    pub tos: u8,
    pub id: u16,
    pub flags_frag: u16,
    pub ttl: u8,
    pub protocol: u8,
    pub src_ip: [u8; 4],
    pub dst_ip: [u8; 4],
    pub icmp_type: u8,
    pub icmp_code: u8,
    pub ident: u16,
    pub seq: u16,
    pub payload: Vec<u8>,
    pub ip_checksum: Option<u16>,
    pub icmp_checksum: Option<u16>,
    /// IPv4 header options (IHL > 5 when non-empty).
    pub options: Vec<u8>,
    /// Override the IPv4 total length.
    pub total_len: Option<u16>,
}

impl Echo {
    /// An echo request from the host to the FPGA.
    pub fn request(ident: u16, seq: u16, payload: Vec<u8>) -> Self {
        Self {
            dst_mac: FPGA_MAC,
            src_mac: HOST_MAC,
            tos: 0,
            id: 0x1c46,
            flags_frag: 0x4000,
            ttl: 64,
            protocol: 1,
            src_ip: HOST_IP,
            dst_ip: FPGA_IP,
            icmp_type: 8,
            icmp_code: 0,
            ident,
            seq,
            payload,
            ip_checksum: None,
            icmp_checksum: None,
            options: Vec::new(),
            total_len: None,
        }
    }

    /// The reply a standard host sends: addresses swapped, TTL 64, type 0,
    /// everything else copied, checksums recomputed from scratch.
    pub fn reply(&self) -> Self {
        Self {
            dst_mac: self.src_mac,
            src_mac: FPGA_MAC,
            ttl: 64,
            src_ip: self.dst_ip,
            dst_ip: self.src_ip,
            icmp_type: 0,
            ip_checksum: None,
            icmp_checksum: None,
            total_len: None,
            ..self.clone()
        }
    }

    pub fn frame(&self) -> Vec<u8> {
        let mut icmp = vec![self.icmp_type, self.icmp_code, 0, 0];
        icmp.extend(self.ident.to_be_bytes());
        icmp.extend(self.seq.to_be_bytes());
        icmp.extend(&self.payload);
        let icmp_sum = self
            .icmp_checksum
            .unwrap_or_else(|| internet_checksum(&icmp));
        icmp[2..4].copy_from_slice(&icmp_sum.to_be_bytes());

        let header_len = 20 + self.options.len();
        let total = self.total_len.unwrap_or((header_len + icmp.len()) as u16);
        let mut ip = vec![0x40 | (header_len / 4) as u8, self.tos];
        ip.extend(total.to_be_bytes());
        ip.extend(self.id.to_be_bytes());
        ip.extend(self.flags_frag.to_be_bytes());
        ip.extend([self.ttl, self.protocol, 0, 0]);
        ip.extend(self.src_ip);
        ip.extend(self.dst_ip);
        ip.extend(&self.options);
        let ip_sum = self.ip_checksum.unwrap_or_else(|| internet_checksum(&ip));
        ip[10..12].copy_from_slice(&ip_sum.to_be_bytes());

        ip.extend(icmp);
        ethernet(self.dst_mac, self.src_mac, 0x0800, &ip)
    }
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
    stimulus
        .iter()
        .map(|&input| core.clock(input).into())
        .collect()
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

/// Vivado `bin/` directory, from `XILINX_ROOT` (default `~/Xilinx/2025.2`).
pub fn xilinx_bin() -> PathBuf {
    let root = std::env::var_os("XILINX_ROOT")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join("Xilinx/2025.2")))
        .expect("set XILINX_ROOT");
    let bin = root.join("Vivado/bin");
    assert!(
        bin.join("xvlog").is_file(),
        "Vivado xsim not found under {}; set XILINX_ROOT or BITLOGIC_ETHERNET_SKIP_HDL=1",
        root.display()
    );
    bin
}

/// Run one Vivado tool in `dir`, panicking with its output on failure.
pub fn run_tool(bin: &Path, dir: &Path, tool: &str, args: &[&str]) {
    let output = Command::new(bin.join(tool))
        .args(args)
        .current_dir(dir)
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{tool} failed:\n{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

/// Hex stimulus file for the testbenches: `{rxerr, dv, rxd}` per cycle.
pub fn stim_hex(stim: &[RxNibble]) -> String {
    stim.iter()
        .map(|s| {
            format!(
                "{:02x}\n",
                u8::from(s.rxerr) << 5 | u8::from(s.dv) << 4 | s.rxd
            )
        })
        .collect()
}

/// Parse a testbench TX trace: one `{tx_en}{txd hex}` line per cycle.
pub fn parse_tx(text: &str) -> Vec<TxNibble> {
    text.lines()
        .map(|line| TxNibble {
            en: &line[..1] == "1",
            txd: u8::from_str_radix(&line[1..], 16).expect("defined txd"),
        })
        .collect()
}
