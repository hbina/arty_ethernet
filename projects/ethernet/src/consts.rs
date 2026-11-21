//! Endpoint identity and sizing. These match the former HLS design's
//! `ethernet_constants.h` (removed; see git history).

/// Locally administered unicast MAC 02:00:00:00:00:01.
pub const FPGA_MAC: u64 = 0x0200_0000_0001;
pub const BROADCAST_MAC: u64 = 0xffff_ffff_ffff;
/// 192.168.1.100
pub const FPGA_IP: u32 = 0xC0A8_0164;

pub const ARP_ETHERTYPE: u16 = 0x0806;
pub const DIAGNOSTIC_BEACON_ETHERTYPE: u16 = 0x88B5;

/// 1 s and 5 s at the 25 MHz MII clock (100 Mb/s link).
pub const BEACON_INTERVAL_CYCLES: u32 = 25_000_000;
pub const GRATUITOUS_ARP_INTERVAL_CYCLES: u32 = 125_000_000;
/// LED activity hold time in 100 MHz cycles (125 ms).
pub const LED_HOLDOFF_CYCLES: u32 = 12_500_000;

/// RX ring: 8 slots of 2 KiB, addressed as {slot[2:0], offset[10:0]}.
pub const RX_SLOTS: usize = 8;
/// TX ring: 4 slots of 2 KiB, addressed as {slot[1:0], offset[10:0]}.
pub const TX_SLOTS: usize = 4;
pub const SLOT_BYTES: usize = 2048;

/// Largest stored frame body (14-byte header + 1500-byte payload, FCS stripped).
pub const MAX_FRAME_BODY: u64 = 1514;
/// Ethernet minimum frame body before FCS; shorter TX frames are zero padded.
pub const MIN_FRAME_BODY: u64 = 60;
/// Inter-frame gap in MII nibble cycles (96 bit times).
pub const IFG_NIBBLES: u64 = 24;

/// Frame bodies (header + payload) built from the template ROM.
pub const ARP_FRAME_BODY_LEN: u64 = 42;
pub const BEACON_FRAME_BODY_LEN: u64 = 185;

/// IEX DEEP market data: multicast group 233.215.21.4, UDP port 10378,
/// IEX-TP message protocol ID 0x8004 (DEEP v1.0).
pub const FEED_GROUP_IP: u32 = 0xE9D7_1504;
pub const FEED_PORT: u16 = 10378;
pub const DEEP_PROTOCOL_ID: u64 = 0x8004;
/// Destination of best bid/offer frames (broadcast by default).
pub const BBO_MAC: u64 = BROADCAST_MAC;
pub const BBO_ETHERTYPE: u16 = 0x88B6;
/// Frame offset of the first IEX-TP message block: Ethernet (14) + IPv4
/// without options (20) + UDP (8) + IEX-TP header (40).
pub const FEED_PAYLOAD_START: u64 = 82;
/// Best bid/offer frame: a 40-byte header and up to 46 records of 32 bytes.
pub const BBO_HEADER_LEN: u64 = 40;
pub const BBO_RECORD_LEN: u64 = 32;
pub const BBO_MAX_RECORDS: u64 = 46;
