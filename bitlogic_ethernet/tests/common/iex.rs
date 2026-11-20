//! IEX DEEP test helpers written independently of the design: segment and
//! message builders, a pcap reader, a reference order book (ordered maps per
//! symbol and side) and a decoder for the design's best bid/offer frames.
#![allow(dead_code)]

use super::{ethernet, internet_checksum};
use std::collections::{BTreeMap, HashMap};

pub const FEED_GROUP: [u8; 4] = [233, 215, 21, 4];
pub const FEED_PORT: u16 = 10378;
/// Ethernet multicast MAC for 233.215.21.4 (01:00:5e + low 23 bits).
pub const FEED_MAC: [u8; 6] = [0x01, 0x00, 0x5e, 0x57, 0x15, 0x04];
pub const SOURCE_MAC: [u8; 6] = [0xa0, 0x88, 0xc2, 0xac, 0xb7, 0x65];
pub const SOURCE_IP: [u8; 4] = [23, 226, 155, 132];
pub const DEEP_PROTOCOL: u16 = 0x8004;

/// An 8-byte IEX symbol: ASCII, left-justified, space padded.
pub fn symbol(name: &str) -> [u8; 8] {
    assert!(name.len() <= 8);
    let mut s = [b' '; 8];
    s[..name.len()].copy_from_slice(name.as_bytes());
    s
}

/// Price Level Update message data (30 bytes).
pub fn plu(sell: bool, event: bool, sym: [u8; 8], size: u32, price: u64) -> Vec<u8> {
    let mut m = vec![if sell { b'5' } else { b'8' }, u8::from(event)];
    m.extend(0x1234_5678_9abc_def0_u64.to_le_bytes());
    m.extend(sym);
    m.extend(size.to_le_bytes());
    m.extend(price.to_le_bytes());
    assert_eq!(m.len(), 30);
    m
}

/// Trade report (38 bytes), which the design must skip.
pub fn trade(sym: [u8; 8], size: u32, price: u64) -> Vec<u8> {
    let mut m = vec![b'T', 0];
    m.extend(0u64.to_le_bytes());
    m.extend(sym);
    m.extend(size.to_le_bytes());
    m.extend(price.to_le_bytes());
    m.extend(7u64.to_le_bytes());
    assert_eq!(m.len(), 38);
    m
}

/// IEX-TP segment fields; `segment` builds the UDP payload.
#[derive(Clone, Debug)]
pub struct Segment {
    pub version: u8,
    pub protocol: u16,
    pub first_seq: u64,
    pub messages: Vec<Vec<u8>>,
    /// Override the message count or payload length.
    pub msg_count: Option<u16>,
    pub payload_len: Option<u16>,
}

impl Segment {
    pub fn new(first_seq: u64, messages: Vec<Vec<u8>>) -> Self {
        Self {
            version: 1,
            protocol: DEEP_PROTOCOL,
            first_seq,
            messages,
            msg_count: None,
            payload_len: None,
        }
    }

    pub fn bytes(&self) -> Vec<u8> {
        let payload: Vec<u8> = self
            .messages
            .iter()
            .flat_map(|m| {
                let mut block = (m.len() as u16).to_le_bytes().to_vec();
                block.extend(m);
                block
            })
            .collect();
        let mut h = vec![self.version, 0];
        h.extend(self.protocol.to_le_bytes());
        h.extend(1u32.to_le_bytes());
        h.extend(0x4287u32.to_le_bytes());
        h.extend(
            self.payload_len
                .unwrap_or(payload.len() as u16)
                .to_le_bytes(),
        );
        h.extend(
            self.msg_count
                .unwrap_or(self.messages.len() as u16)
                .to_le_bytes(),
        );
        h.extend(0u64.to_le_bytes());
        h.extend(self.first_seq.to_le_bytes());
        h.extend(0u64.to_le_bytes());
        assert_eq!(h.len(), 40);
        h.extend(payload);
        h
    }
}

/// A UDP datagram in an Ethernet/IPv4 frame with a correct IPv4 checksum.
pub fn udp_frame(dst_ip: [u8; 4], dst_port: u16, payload: &[u8]) -> Vec<u8> {
    let mut udp = FEED_PORT.to_be_bytes().to_vec();
    udp.extend(dst_port.to_be_bytes());
    udp.extend(((8 + payload.len()) as u16).to_be_bytes());
    udp.extend([0, 0]);
    udp.extend(payload);
    let total = (20 + udp.len()) as u16;
    let mut ip = vec![0x45, 0];
    ip.extend(total.to_be_bytes());
    ip.extend([0, 0, 0x40, 0, 64, 17, 0, 0]);
    ip.extend(SOURCE_IP);
    ip.extend(dst_ip);
    let sum = internet_checksum(&ip);
    ip[10..12].copy_from_slice(&sum.to_be_bytes());
    ip.extend(udp);
    ethernet(FEED_MAC, SOURCE_MAC, 0x0800, &ip)
}

pub fn feed_frame(seg: &Segment) -> Vec<u8> {
    udp_frame(FEED_GROUP, FEED_PORT, &seg.bytes())
}

/// Ethernet frames of a classic (microsecond, little-endian) pcap file.
pub fn read_pcap(path: &std::path::Path) -> Vec<Vec<u8>> {
    let data = std::fs::read(path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    assert_eq!(
        u32::from_le_bytes(data[..4].try_into().unwrap()),
        0xa1b2_c3d4
    );
    assert_eq!(u32::from_le_bytes(data[20..24].try_into().unwrap()), 1);
    let mut frames = Vec::new();
    let mut at = 24;
    while at + 16 <= data.len() {
        let caplen = u32::from_le_bytes(data[at + 8..at + 12].try_into().unwrap()) as usize;
        let origlen = u32::from_le_bytes(data[at + 12..at + 16].try_into().unwrap()) as usize;
        assert_eq!(caplen, origlen, "truncated capture record");
        frames.push(data[at + 16..at + 16 + caplen].to_vec());
        at += 16 + caplen;
    }
    frames
}

/// The sample capture at the repository root, if present.
pub fn sample_pcap() -> Option<std::path::PathBuf> {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../iex_data_broadcast.pcap");
    path.is_file().then_some(path)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Plu {
    pub sell: bool,
    pub event: bool,
    pub symbol: [u8; 8],
    pub size: u32,
    pub price: u64,
}

/// The PLUs of a frame, if it is a well-formed IEX DEEP segment on the feed
/// group and port. Mirrors the documented acceptance rules, in plain Rust.
pub fn frame_plus(frame: &[u8]) -> Option<Vec<Plu>> {
    if frame.len() < 82
        || frame[12..14] != [0x08, 0x00]
        || frame[14] != 0x45
        || frame[23] != 17
        || frame[30..34] != FEED_GROUP
        || u16::from_be_bytes([frame[36], frame[37]]) != FEED_PORT
        || u16::from_be_bytes([frame[20], frame[21]]) & 0x3fff != 0
        || internet_checksum(&frame[14..34]) != 0
    {
        return None;
    }
    let tp = &frame[42..];
    let payload_len = u16::from_le_bytes([tp[12], tp[13]]) as usize;
    let count = u16::from_le_bytes([tp[14], tp[15]]) as usize;
    if tp[0] != 1
        || u16::from_le_bytes([tp[2], tp[3]]) != DEEP_PROTOCOL
        || 82 + payload_len > frame.len()
    {
        return None;
    }
    let payload = &tp[40..40 + payload_len];
    let mut out = Vec::new();
    let mut at = 0;
    for _ in 0..count {
        let len = u16::from_le_bytes([payload[at], payload[at + 1]]) as usize;
        let m = &payload[at + 2..at + 2 + len];
        if len == 30 && (m[0] == b'8' || m[0] == b'5') {
            out.push(Plu {
                sell: m[0] == b'5',
                event: m[1] & 1 == 1,
                symbol: m[10..18].try_into().unwrap(),
                size: u32::from_le_bytes(m[18..22].try_into().unwrap()),
                price: u64::from_le_bytes(m[22..30].try_into().unwrap()),
            });
        }
        at += 2 + len;
    }
    Some(out)
}

/// Best bid and offer of one symbol after an event. An empty side is
/// price 0, size 0.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Bbo {
    pub symbol: [u8; 8],
    pub bid_price: u64,
    pub bid_size: u32,
    pub ask_price: u64,
    pub ask_size: u32,
}

impl std::fmt::Display for Bbo {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "{} {}@{} x {}@{}",
            String::from_utf8_lossy(&self.symbol).trim_end(),
            self.bid_size,
            self.bid_price,
            self.ask_size,
            self.ask_price
        )
    }
}

/// Reference order book: one ordered map per symbol and side.
#[derive(Default)]
pub struct Book {
    sides: HashMap<[u8; 8], [BTreeMap<u64, u32>; 2]>,
}

impl Book {
    /// Apply a PLU; return the BBO when it ends an event.
    pub fn apply(&mut self, p: &Plu) -> Option<Bbo> {
        let sides = self.sides.entry(p.symbol).or_default();
        let side = &mut sides[usize::from(p.sell)];
        if p.size == 0 {
            side.remove(&p.price);
        } else {
            side.insert(p.price, p.size);
        }
        p.event.then(|| {
            let bid = sides[0]
                .iter()
                .next_back()
                .map_or((0, 0), |(&p, &s)| (p, s));
            let ask = sides[1].iter().next().map_or((0, 0), |(&p, &s)| (p, s));
            Bbo {
                symbol: p.symbol,
                bid_price: bid.0,
                bid_size: bid.1,
                ask_price: ask.0,
                ask_size: ask.1,
            }
        })
    }

    pub fn levels(&self) -> usize {
        self.sides.values().map(|s| s[0].len() + s[1].len()).sum()
    }

    pub fn symbols(&self) -> usize {
        self.sides.len()
    }
}

/// A decoded best bid/offer frame.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BboFrame {
    pub first: u32,
    pub feed_packets: u32,
    pub feed_gaps: u32,
    pub feed_malformed: u32,
    pub book_errors: u32,
    pub rx_queue_drops: u32,
    pub records: Vec<Bbo>,
}

fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes(b[..4].try_into().unwrap())
}

/// Decode a frame body as a BBO frame; `None` for other EtherTypes.
pub fn decode_bbo(body: &[u8], fpga_mac: [u8; 6], dst: [u8; 6]) -> Option<BboFrame> {
    if body.len() < 14 || body[12..14] != [0x88, 0xB6] {
        return None;
    }
    assert_eq!(&body[..6], &dst, "BBO destination MAC");
    assert_eq!(&body[6..12], &fpga_mac, "BBO source MAC");
    assert_eq!(body[14], 1, "BBO version");
    let count = usize::from(body[15]);
    assert!((1..=46).contains(&count), "record count {count}");
    assert_eq!(body.len(), (40 + 32 * count).max(60), "BBO frame length");
    let records = (0..count)
        .map(|k| {
            let r = &body[40 + 32 * k..72 + 32 * k];
            Bbo {
                symbol: r[..8].try_into().unwrap(),
                bid_price: u64::from_le_bytes(r[8..16].try_into().unwrap()),
                ask_price: u64::from_le_bytes(r[16..24].try_into().unwrap()),
                bid_size: le32(&r[24..]),
                ask_size: le32(&r[28..]),
            }
        })
        .collect();
    Some(BboFrame {
        first: le32(&body[16..]),
        feed_packets: le32(&body[20..]),
        feed_gaps: le32(&body[24..]),
        feed_malformed: le32(&body[28..]),
        book_errors: le32(&body[32..]),
        rx_queue_drops: le32(&body[36..]),
        records,
    })
}
