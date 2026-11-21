//! IEX DEEP feed handler: the cycle model of the whole core against the
//! independent reference book in `tests/common/iex.rs`.
mod common;

use bitlogic_ethernet::sim::{CoreConfig, EthCore, Interpreted, Native, RxNibble, Steps, TxNibble};
use common::iex::*;
use common::*;

fn quiet() -> CoreConfig {
    CoreConfig {
        beacon_interval: u32::MAX,
        garp_interval: u32::MAX,
        ..CoreConfig::default()
    }
}

/// Everything consumed and transmitted.
fn drained<S: Steps>(core: &EthCore<S>) -> bool {
    core.feed.rx_rd_ptr == core.rx.wr_ptr
        && core.proto.rx_rd_ptr == core.rx.wr_ptr
        && core.book.plu_rd_ptr == core.feed.plu_wr_ptr
        && core.proto.bbo_rd_ptr == core.book.bbo_wr_ptr
        && core.tx.rd_ptr == core.proto.tx_wr_ptr
}

/// Frames separated by `gap` idle nibbles (24 is the minimum inter-frame
/// gap), then idle until everything has drained and been sent.
struct Trace {
    tx: Vec<TxNibble>,
}

fn drive<S: Steps>(core: &mut EthCore<S>, frames: &[Vec<u8>], gap: usize) -> Trace {
    let mut tx = Vec::new();
    for f in frames {
        for n in mii_frame(f, gap) {
            tx.push(core.clock(n).into());
        }
    }
    let mut quiet_cycles = 0;
    while quiet_cycles < 400 {
        let out: TxNibble = core.clock(RxNibble::default()).into();
        quiet_cycles = if drained(core) && !out.en {
            quiet_cycles + 1
        } else {
            0
        };
        tx.push(out);
        assert!(tx.len() < 400_000_000, "core never drained");
    }
    Trace { tx }
}

/// BBO records in order, checking the record sequence numbers.
fn bbo_records(frames: &[TxFrame]) -> Vec<Bbo> {
    let mut records = Vec::new();
    for f in frames {
        if let Some(b) = decode_bbo(&f.body, FPGA_MAC, BROADCAST) {
            assert_eq!(b.first as usize, records.len(), "record sequence");
            records.extend(b.records);
        }
    }
    records
}

fn reference(frames: &[Vec<u8>]) -> (Book, Vec<Bbo>) {
    let mut book = Book::default();
    let records = frames
        .iter()
        .filter_map(|f| frame_plus(f))
        .flatten()
        .filter_map(|p| book.apply(&p))
        .collect();
    (book, records)
}

fn run_and_compare(frames: &[Vec<u8>]) -> EthCore {
    let mut core = EthCore::new(Native, quiet());
    let trace = drive(&mut core, frames, 24);
    let got = bbo_records(&decode(&trace.tx));
    let (book, want) = reference(frames);
    assert_eq!(got.len(), want.len(), "record count");
    for (k, (g, w)) in got.iter().zip(&want).enumerate() {
        assert_eq!(g, w, "record {k}: got {g}, want {w}");
    }
    assert_eq!(core.book.counters.levels.get() as usize, book.levels());
    assert_eq!(core.book.counters.symbols.get() as usize, book.symbols());
    assert_eq!(core.book.counters.errors.get(), 0);
    assert_eq!(core.rx.drop_count.get(), 0, "RX ring overflowed");
    core
}

fn seg(seq: u64, messages: Vec<Vec<u8>>) -> Vec<u8> {
    feed_frame(&Segment::new(seq, messages))
}

#[test]
fn single_update_emits_bbo() {
    let frames = vec![seg(
        1,
        vec![plu(false, true, symbol("ZIEXT"), 9700, 990_500)],
    )];
    let mut core = EthCore::new(Native, quiet());
    let trace = drive(&mut core, &frames, 24);
    let tx = decode(&trace.tx);
    assert_eq!(tx.len(), 1);
    let b = decode_bbo(&tx[0].body, FPGA_MAC, BROADCAST).unwrap();
    assert_eq!(
        b,
        BboFrame {
            first: 0,
            feed_packets: 1,
            feed_gaps: 0,
            feed_malformed: 0,
            book_errors: 0,
            rx_queue_drops: 0,
            records: vec![Bbo {
                symbol: symbol("ZIEXT"),
                bid_price: 990_500,
                bid_size: 9700,
                ask_price: 0,
                ask_size: 0,
            }],
        }
    );
    // A frame to the feed is not a protocol drop.
    assert_eq!(core.proto.counters.rx_proto_drops.get(), 0);
}

#[test]
fn book_keeps_levels_sorted_and_reports_best() {
    let s = symbol("AAPL");
    let ev = |sell, size, price| plu(sell, true, s, size, price);
    let steps = [
        (ev(false, 10, 100), (100, 10, 0, 0)),
        (ev(false, 20, 102), (102, 20, 0, 0)),
        (ev(false, 30, 101), (102, 20, 0, 0)),
        (ev(true, 5, 105), (102, 20, 105, 5)),
        (ev(true, 6, 104), (102, 20, 104, 6)),
        (ev(true, 7, 106), (102, 20, 104, 6)),
        // Update the best bid's size, then delete it.
        (ev(false, 25, 102), (102, 25, 104, 6)),
        (ev(false, 0, 102), (101, 30, 104, 6)),
        // Deleting an absent level changes nothing.
        (ev(false, 0, 999), (101, 30, 104, 6)),
        // Delete the middle offer, then the best, then the last.
        (ev(true, 0, 105), (101, 30, 104, 6)),
        (ev(true, 0, 104), (101, 30, 106, 7)),
        (ev(true, 0, 106), (101, 30, 0, 0)),
        // Re-add an offer: reuses a freed node.
        (ev(true, 9, 103), (101, 30, 103, 9)),
    ];
    let frames: Vec<_> = steps
        .iter()
        .enumerate()
        .map(|(k, (m, _))| seg(k as u64 + 1, vec![m.clone()]))
        .collect();
    let mut core = EthCore::new(Native, quiet());
    let trace = drive(&mut core, &frames, 24);
    let got = bbo_records(&decode(&trace.tx));
    let want: Vec<Bbo> = steps
        .iter()
        .map(|&(_, (bp, bs, ap, asz))| Bbo {
            symbol: s,
            bid_price: bp,
            bid_size: bs,
            ask_price: ap,
            ask_size: asz,
        })
        .collect();
    assert_eq!(got, want);
    assert_eq!(core.book.counters.levels.get(), 3);
}

#[test]
fn bbo_waits_for_the_event_flag() {
    let s = symbol("ZIEXT");
    let frames = vec![
        seg(
            1,
            vec![
                plu(true, true, s, 100, 2530),
                plu(true, true, s, 100, 2520),
                plu(true, true, s, 100, 2510),
                plu(false, true, s, 100, 2500),
            ],
        ),
        // The example from the DEEP specification: two levels removed
        // atomically; only the second PLU ends the event.
        seg(5, vec![plu(true, false, s, 0, 2510), trade(s, 100, 2510)]),
        seg(7, vec![plu(true, true, s, 0, 2520)]),
    ];
    let core = run_and_compare(&frames);
    let (_, want) = reference(&frames);
    assert_eq!(want.len(), 5);
    assert_eq!((want[4].ask_price, want[4].bid_price), (2530, 2500));
    assert_eq!(core.feed.counters.messages.get(), 7);
    assert_eq!(core.feed.counters.plus.get(), 6);
}

/// Random PLUs over few symbols and prices so that inserts, updates,
/// deletes of present and absent levels, and node reuse all happen often;
/// trade reports are interleaved and must be skipped.
fn random_frames(rng: &mut Rng, plus: usize, symbols: u64, prices: u64) -> Vec<Vec<u8>> {
    let mut frames = Vec::new();
    let mut seq = 1;
    let mut left = plus;
    while left > 0 {
        let n = (1 + rng.next() % 12).min(left as u64) as usize;
        let mut msgs = Vec::new();
        for _ in 0..n {
            let sym = symbol(&format!("S{:03}", rng.next() % symbols));
            if rng.next().is_multiple_of(5) {
                msgs.push(trade(sym, 100, 1000));
            }
            let size = if rng.next().is_multiple_of(3) {
                0
            } else {
                (1 + rng.next() % 5000) as u32
            };
            let price = 10_000 + 100 * (rng.next() % prices);
            msgs.push(plu(
                rng.next().is_multiple_of(2),
                !rng.next().is_multiple_of(5),
                sym,
                size,
                price,
            ));
        }
        seq += msgs.len() as u64;
        frames.push(seg(seq, msgs));
        left -= n;
    }
    frames
}

#[test]
fn random_updates_match_reference() {
    let mut rng = Rng(0x01EC_5EED);
    let frames = random_frames(&mut rng, 4000, 40, 30);
    let core = run_and_compare(&frames);
    assert!(core.book.free_sp.get() > 0, "the scenario must free nodes");
    assert!(core.book.counters.levels.get() > 300);
}

#[test]
fn interpreted_core_matches_native() {
    let mut rng = Rng(77);
    let mut frames = random_frames(&mut rng, 150, 6, 8);
    frames.insert(3, arp_request(HOST_MAC, HOST_IP, FPGA_IP));
    let mut native = EthCore::new(Native, quiet());
    let mut interpreted = EthCore::new(Interpreted::new(), quiet());
    let a = drive(&mut native, &frames, 24);
    let b = drive(&mut interpreted, &frames, 24);
    assert_eq!(a.tx, b.tx);
    assert!(bbo_records(&decode(&a.tx)).len() > 100);
}

#[test]
fn foreign_and_malformed_segments() {
    let s = symbol("IBM");
    let good = |seq| Segment::new(seq, vec![plu(false, true, s, 1, 100 + seq)]);
    let mut frames = Vec::new();
    frames.push(feed_frame(&good(1))); // accepted
    // Other port and other group: ignored, and counted by the protocol unit
    // as unused frames.
    frames.push(udp_frame(FEED_GROUP, 10377, &good(2).bytes()));
    frames.push(udp_frame([233, 215, 21, 5], FEED_PORT, &good(2).bytes()));
    // Malformed: bad IPv4 checksum, wrong protocol ID, wrong version,
    // a block running past the payload, a count that disagrees.
    let mut bad_sum = feed_frame(&good(2));
    bad_sum[24] ^= 1;
    frames.push(bad_sum);
    frames.push(feed_frame(&Segment {
        protocol: 0x8003,
        ..good(2)
    }));
    frames.push(feed_frame(&Segment {
        version: 2,
        ..good(2)
    }));
    frames.push(feed_frame(&Segment {
        payload_len: Some(20),
        ..good(2)
    }));
    frames.push(feed_frame(&Segment {
        msg_count: Some(2),
        ..good(2)
    }));
    frames.push(feed_frame(&good(3))); // accepted: seq 2 was never valid -> gap
    frames.push(feed_frame(&Segment::new(4, vec![]))); // heartbeat, accepted
    frames.push(feed_frame(&good(9))); // accepted, gap
    let mut core = EthCore::new(Native, quiet());
    let trace = drive(&mut core, &frames, 24);
    let got = bbo_records(&decode(&trace.tx));
    let prices: Vec<u64> = got.iter().map(|b| b.bid_price).collect();
    // The count-mismatch segment's PLU was queued before the walk noticed
    // the payload ended early.
    assert_eq!(prices, vec![101, 102, 103, 109]);
    let c = core.feed.counters;
    assert_eq!(c.packets.get(), 6, "accepted segments");
    assert_eq!(c.malformed.get(), 5);
    // Expected after each accepted segment: 2, 3 (overrun), then the
    // count-mismatch segment starts at 2 (gap) and claims 2 messages, so 4;
    // seq 3 (gap); the heartbeat at 4 matches; seq 9 (gap).
    assert_eq!(c.seq_gaps.get(), 3);
    assert_eq!(core.proto.counters.rx_proto_drops.get(), 2);
}

#[test]
fn arp_and_echo_still_answered_between_feed_frames() {
    let mut rng = Rng(5);
    let mut frames = random_frames(&mut rng, 60, 4, 5);
    let echo = Echo::request(7, 1, vec![0x5a; 64]);
    frames.insert(2, arp_request(HOST_MAC, HOST_IP, FPGA_IP));
    frames.insert(5, echo.frame());
    let mut core = EthCore::new(Native, quiet());
    let trace = drive(&mut core, &frames, 24);
    let tx = decode(&trace.tx);
    let bodies: Vec<_> = tx.iter().map(|f| f.body.clone()).collect();
    assert!(bodies.contains(&arp_reply(HOST_MAC, HOST_MAC, HOST_IP)));
    assert!(bodies.contains(&echo.reply().frame()));
    let (_, want) = reference(&frames);
    assert_eq!(bbo_records(&tx), want);
}

/// `book::symbol_hash`, written from its documentation.
fn hash(sym: [u8; 8]) -> u64 {
    let s = u64::from_le_bytes(sym);
    let x = s ^ (s >> 29) ^ (s >> 17) ^ (s << 7);
    (0..6).fold(0, |h, k| h ^ ((x >> (11 * k)) & 0x7ff))
}

#[test]
fn colliding_symbols_exhaust_the_probe_limit() {
    // 17 symbols with the same first slot: the 17th finds 16 occupied slots.
    let target = hash(symbol("A0"));
    let colliding: Vec<[u8; 8]> = (0..)
        .map(|k| symbol(&format!("A{k}")))
        .filter(|&s| hash(s) == target)
        .take(17)
        .collect();
    let frames: Vec<_> = colliding
        .iter()
        .enumerate()
        .map(|(k, &s)| seg(k as u64 + 1, vec![plu(false, true, s, 1, 500)]))
        .collect();
    let mut core = EthCore::new(Native, quiet());
    let trace = drive(&mut core, &frames, 24);
    let got = bbo_records(&decode(&trace.tx));
    let (_, want) = reference(&frames[..16]);
    assert_eq!(got, want);
    assert_eq!(core.book.counters.errors.get(), 1);
    assert_eq!(core.book.counters.symbols.get(), 16);
}

#[test]
fn list_walk_limit() {
    // Bids appended from the worst end: each insert walks the whole list.
    let s = symbol("DEEP");
    let msgs: Vec<_> = (0..65)
        .map(|k| plu(false, false, s, 1, 10_000 - k))
        .chain([plu(false, true, s, 7, 20_000)])
        .collect();
    let frames: Vec<_> = msgs
        .chunks(40)
        .enumerate()
        .map(|(k, m)| seg(1 + 40 * k as u64, m.to_vec()))
        .collect();
    let mut core = EthCore::new(Native, quiet());
    drive(&mut core, &frames, 24);
    // 64 levels fit; the 65th insert would walk past 64 nodes.
    assert_eq!(core.book.counters.errors.get(), 1);
    assert_eq!(core.book.counters.levels.get(), 65); // 64 + the new best
}

#[test]
fn node_pool_exhaustion_and_reuse() {
    // 130 symbols x 63 bids + 1 = 8191 nodes, the whole pool.
    let mut msgs = Vec::new();
    for k in 0..130 {
        let s = symbol(&format!("P{k:03}"));
        for p in 0..63 {
            msgs.push(plu(false, false, s, 1, 1000 + p));
        }
    }
    let x = symbol("X");
    msgs.push(plu(false, true, x, 1, 1)); // node 8191
    msgs.push(plu(true, true, x, 2, 2)); // pool empty: dropped
    msgs.push(plu(false, true, x, 0, 1)); // frees a node
    msgs.push(plu(true, true, x, 3, 3)); // reuses it
    let frames: Vec<_> = msgs
        .chunks(44)
        .enumerate()
        .map(|(k, m)| seg(1 + 44 * k as u64, m.to_vec()))
        .collect();
    let mut core = EthCore::new(Native, quiet());
    let trace = drive(&mut core, &frames, 24);
    let got = bbo_records(&decode(&trace.tx));
    let rec = |bp, bs, ap, asz| Bbo {
        symbol: x,
        bid_price: bp,
        bid_size: bs,
        ask_price: ap,
        ask_size: asz,
    };
    assert_eq!(
        got,
        vec![
            rec(1, 1, 0, 0),
            rec(1, 1, 0, 0),
            rec(0, 0, 0, 0),
            rec(0, 0, 3, 3)
        ]
    );
    assert_eq!(core.book.counters.errors.get(), 1);
    assert_eq!(core.book.used.get(), 8191);
    assert_eq!(core.book.counters.levels.get(), 8191);
}

/// Maximum-size segments (44 PLUs, 1490-byte frames) back to back at the
/// minimum gap, every PLU ending an event over books about 20 levels deep:
/// the worst sustained load the feed can offer. Nothing may be dropped.
#[test]
fn line_rate_full_frames_keep_up() {
    let mut rng = Rng(0xF00D);
    let mut frames = Vec::new();
    for k in 0..300u64 {
        let msgs = (0..44)
            .map(|_| {
                let sym = symbol(&format!("L{:02}", rng.next() % 50));
                let size = if rng.next().is_multiple_of(4) {
                    0
                } else {
                    (1 + rng.next() % 900) as u32
                };
                plu(
                    rng.next().is_multiple_of(2),
                    true,
                    sym,
                    size,
                    5000 + rng.next() % 24,
                )
            })
            .collect();
        frames.push(seg(1 + 44 * k, msgs));
    }
    assert_eq!(frames[0].len(), 82 + 44 * 32);
    let core = run_and_compare(&frames);
    assert_eq!(core.feed.counters.plus.get(), 300 * 44);
}

fn replay_sample(limit: usize) {
    let Some(path) = sample_pcap() else {
        eprintln!("SKIPPED: iex_data_broadcast.pcap not found");
        return;
    };
    let mut frames = read_pcap(&path);
    frames.truncate(limit);
    let core = run_and_compare(&frames);
    let c = core.feed.counters;
    assert_eq!(c.packets.get() as usize, frames.len());
    assert_eq!(c.seq_gaps.get(), 0);
    assert_eq!(c.malformed.get(), 0);
}

/// All 100,000 packets of the sample capture, replayed at 100 Mb/s line rate
/// (minimum inter-frame gap) instead of their recorded timing: about 29
/// million cycles, a few seconds in a release build.
#[test]
fn sample_capture_at_line_rate() {
    replay_sample(usize::MAX);
}

/// Books 60 levels deep, then maximum-size segments at the minimum gap
/// whose every PLU updates the deepest level: the book needs about 69
/// cycles per PLU against 68.8 on the wire, so the PLU FIFO fills and the
/// feed must stall rather than overwrite queued PLUs.
#[test]
fn full_plu_fifo_stalls_the_feed() {
    let depth = 60u64;
    let sym = |k: u64| symbol(&format!("F{k}"));
    let mut setup = Vec::new();
    for k in 0..4 {
        for sell in [false, true] {
            let msgs: Vec<_> = (0..depth)
                .map(|d| {
                    let price = if sell { 2000 + d } else { 1000 - d };
                    plu(sell, false, sym(k), 100, price)
                })
                .collect();
            for m in msgs.chunks(44) {
                setup.push(seg(1, m.to_vec()));
            }
        }
    }
    let mut rng = Rng(60);
    let load: Vec<_> = (0..200)
        .map(|_| {
            let msgs = (0..44)
                .map(|_| {
                    let sell = rng.next().is_multiple_of(2);
                    let price = if sell { 2000 + depth - 1 } else { 1001 - depth };
                    plu(
                        sell,
                        true,
                        sym(rng.next() % 4),
                        (1 + rng.next() % 999) as u32,
                        price,
                    )
                })
                .collect();
            seg(1, msgs)
        })
        .collect();
    let mut core = EthCore::new(Native, quiet());
    drive(&mut core, &setup, 4_000);
    let mut tx = Vec::new();
    let mut peak = 0;
    for f in &load {
        for n in mii_frame(f, 24) {
            tx.push(core.clock(n).into());
            peak = peak.max((core.feed.plu_wr_ptr.get() + 128 - core.book.plu_rd_ptr.get()) % 128);
        }
    }
    tx.extend(drive(&mut core, &[], 24).tx);
    assert_eq!(peak, 64, "the scenario must fill the PLU FIFO");
    assert_eq!(core.rx.drop_count.get(), 0);
    let all: Vec<_> = setup.iter().chain(&load).cloned().collect();
    let (_, want) = reference(&all);
    assert_eq!(bbo_records(&decode(&tx)), want);
}
