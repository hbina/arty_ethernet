//! Measure the IEX feed handler in the cycle model: latency, buffer
//! occupancy and unit utilization for the sample capture at 100 Mb/s line
//! rate, then the book-depth limit under the worst sustained load.
//!
//! ```sh
//! cargo run --release --example feed_bench
//! ```
//!
//! One cycle is one MII nibble at 25 MHz (40 ns). Latency is measured from
//! the last nibble of the input frame (end of its FCS) on `rxd` to the first
//! preamble nibble of the BBO frame carrying the record on `txd`, and to the
//! record's last nibble, at the core boundary (the boundary registers in
//! `eth_core` included, PHY and pads excluded).
#[path = "../tests/common/mod.rs"]
mod common;

use bitlogic_ethernet::sim::{CoreConfig, EthCore, Native, RxNibble, TxNibble};
use common::iex::*;
use common::*;

const NS_PER_CYCLE: f64 = 40.0;

#[derive(Default)]
struct Occupancy {
    rx_ring: u64,
    plu_fifo: u64,
    bbo_fifo: u64,
    feed_busy: u64,
    book_busy: u64,
    tx_busy: u64,
    cycles: u64,
}

fn sample(core: &EthCore, o: &mut Occupancy, out: TxNibble) {
    let rx = (core.rx.wr_ptr.get() + 16 - core.feed.rx_rd_ptr.get()) % 16;
    let plu = (core.feed.plu_wr_ptr.get() + 128 - core.book.plu_rd_ptr.get()) % 128;
    let bbo = (core.book.bbo_wr_ptr.get() + 512 - core.proto.bbo_rd_ptr.get()) % 512;
    o.rx_ring = o.rx_ring.max(rx);
    o.plu_fifo = o.plu_fifo.max(plu);
    o.bbo_fifo = o.bbo_fifo.max(bbo);
    use bitlogic_ethernet::{book::BookPhase, feed::FeedPhase};
    o.feed_busy += u64::from(core.feed.phase != FeedPhase::Idle);
    o.book_busy += u64::from(!matches!(
        core.book.phase,
        BookPhase::Idle | BookPhase::Clear
    ));
    o.tx_busy += u64::from(out.en);
    o.cycles += 1;
}

struct Replay {
    core: EthCore,
    tx: Vec<TxNibble>,
    frame_end: Vec<usize>,
    occ: Occupancy,
}

fn replay(frames: &[Vec<u8>], gap: usize) -> Replay {
    let config = CoreConfig {
        beacon_interval: u32::MAX,
        garp_interval: u32::MAX,
        ..CoreConfig::default()
    };
    let mut core = EthCore::new(Native, config);
    let mut occ = Occupancy::default();
    let mut tx = Vec::new();
    let mut frame_end = Vec::new();
    for f in frames {
        let nibbles = mii_frame(f, gap);
        let last = nibbles.len() - gap - 1;
        for (k, &n) in nibbles.iter().enumerate() {
            let out: TxNibble = core.clock(n).into();
            sample(&core, &mut occ, out);
            tx.push(out);
            if k == last {
                frame_end.push(tx.len() - 1);
            }
        }
    }
    let mut quiet = 0;
    while quiet < 400 {
        let out: TxNibble = core.clock(RxNibble::default()).into();
        tx.push(out);
        let drained = core.feed.rx_rd_ptr == core.rx.wr_ptr
            && core.book.plu_rd_ptr == core.feed.plu_wr_ptr
            && core.proto.bbo_rd_ptr == core.book.bbo_wr_ptr
            && core.tx.rd_ptr == core.proto.tx_wr_ptr;
        quiet = if drained && !out.en { quiet + 1 } else { 0 };
    }
    Replay {
        core,
        tx,
        frame_end,
        occ,
    }
}

fn pct(v: &mut [u64], p: f64) -> u64 {
    v.sort_unstable();
    v[((v.len() - 1) as f64 * p).round() as usize]
}

fn show(name: &str, v: &mut [u64]) {
    let us = |c: u64| c as f64 * NS_PER_CYCLE / 1000.0;
    let (min, p50, p99, max) = (pct(v, 0.0), pct(v, 0.5), pct(v, 0.99), pct(v, 1.0));
    println!(
        "  {name:<26} min {min:>5}  p50 {p50:>5}  p99 {p99:>6}  max {max:>6} cycles \
         ({:.2} / {:.2} / {:.2} / {:.2} us)",
        us(min),
        us(p50),
        us(p99),
        us(max)
    );
}

fn sample_capture() {
    let Some(path) = sample_pcap() else {
        println!("sample capture not found; skipping");
        return;
    };
    let frames = read_pcap(&path);
    let r = replay(&frames, 24);
    let wire_cycles = *r.frame_end.last().unwrap() as u64 + 1;
    let bytes: usize = frames.iter().map(Vec::len).sum();

    // Reference records and the input frame each one comes from.
    let mut book = Book::default();
    let mut want = Vec::new();
    let mut source = Vec::new();
    for (k, f) in frames.iter().enumerate() {
        for p in frame_plus(f).unwrap_or_default() {
            if let Some(b) = book.apply(&p) {
                want.push(b);
                source.push(k);
            }
        }
    }

    let mut got = Vec::new();
    let mut first_nibble = Vec::new();
    let mut record_done = Vec::new();
    let mut per_frame = Vec::new();
    for f in decode(&r.tx) {
        let Some(b) = decode_bbo(&f.body, FPGA_MAC, BROADCAST) else {
            continue;
        };
        per_frame.push(b.records.len() as u64);
        for (p, rec) in b.records.into_iter().enumerate() {
            let input_end = r.frame_end[source[got.len()]];
            first_nibble.push((f.start - input_end) as u64);
            // Preamble/SFD (8) + header (40) + records up to this one.
            record_done.push((f.start + 2 * (48 + 32 * (p + 1)) - 1 - input_end) as u64);
            got.push(rec);
        }
    }
    assert_eq!(got, want, "BBO records differ from the reference book");
    let c = &r.core;
    println!("Sample capture, {} packets at line rate", frames.len());
    println!(
        "  input: {bytes} bytes, {:.1} ms on the wire ({} cycles)",
        wire_cycles as f64 * NS_PER_CYCLE / 1e6,
        wire_cycles
    );
    println!(
        "  feed: {} segments, {} messages, {} PLUs; gaps {}, malformed {}",
        c.feed.counters.packets.get(),
        c.feed.counters.messages.get(),
        c.feed.counters.plus.get(),
        c.feed.counters.seq_gaps.get(),
        c.feed.counters.malformed.get()
    );
    println!(
        "  book: {} symbols, {} live levels at end, {} nodes ever used, errors {}",
        c.book.counters.symbols.get(),
        c.book.counters.levels.get(),
        c.book.used.get(),
        c.book.counters.errors.get()
    );
    println!(
        "  output: {} BBO records in {} frames (mean {:.2}, max {} per frame), all match the reference",
        got.len(),
        per_frame.len(),
        got.len() as f64 / per_frame.len() as f64,
        per_frame.iter().max().unwrap()
    );
    println!("  RX ring drops: {}", c.rx.drop_count.get());
    let o = &r.occ;
    println!(
        "  peak occupancy: RX ring {}/8, PLU FIFO {}/64, BBO FIFO {}/256",
        o.rx_ring, o.plu_fifo, o.bbo_fifo
    );
    println!(
        "  busy: feed {:.1}%, book {:.1}%, TX line {:.1}% of {} cycles",
        100.0 * o.feed_busy as f64 / o.cycles as f64,
        100.0 * o.book_busy as f64 / o.cycles as f64,
        100.0 * o.tx_busy as f64 / o.cycles as f64,
        o.cycles
    );
    println!("  latency from the input frame's last nibble:");
    show("to BBO frame first nibble", &mut first_nibble);
    show("to record's last nibble", &mut record_done);
}

/// Worst sustained load: maximum-size segments (44 PLUs) back to back, every
/// PLU ending an event and updating the deepest level of a side with `depth`
/// levels, so every update walks the whole list.
fn depth_sweep() {
    println!();
    println!("Book depth limit: 300 back-to-back 44-PLU segments, every PLU an event");
    println!("updating the deepest level (walks the full list), 8 symbols x 2 sides.");
    println!("  depth  book busy  peak PLU FIFO  RX drops  book cycles/PLU  wire cycles/PLU");
    for depth in [1u64, 8, 16, 24, 32, 40, 48, 52, 56, 60, 64] {
        let sym = |k: u64| symbol(&format!("D{k}"));
        // Prefill with a generous gap so setup itself never drops.
        let mut setup = Vec::new();
        for k in 0..8 {
            for sell in [false, true] {
                // Best first: each insert appends at the worst end.
                let msgs: Vec<_> = (0..depth)
                    .map(|d| {
                        let price = if sell { 2000 + d } else { 1000 - d };
                        plu(sell, false, sym(k), 100, price)
                    })
                    .collect();
                for m in msgs.chunks(44) {
                    setup.push(feed_frame(&Segment::new(1, m.to_vec())));
                }
            }
        }
        let mut load = Vec::new();
        let mut rng = Rng(depth);
        for _ in 0..300 {
            let msgs = (0..44)
                .map(|_| {
                    let k = rng.next() % 8;
                    let sell = rng.next().is_multiple_of(2);
                    let price = if sell { 2000 + depth - 1 } else { 1001 - depth };
                    plu(sell, true, sym(k), (1 + rng.next() % 999) as u32, price)
                })
                .collect();
            load.push(feed_frame(&Segment::new(1, msgs)));
        }
        // Setup with a 4,000-cycle gap, then the load at the minimum gap.
        let config = CoreConfig {
            beacon_interval: u32::MAX,
            garp_interval: u32::MAX,
            ..CoreConfig::default()
        };
        let mut core = EthCore::new(Native, config);
        for f in &setup {
            for n in mii_frame(f, 4_000) {
                core.clock(n);
            }
        }
        assert_eq!(core.rx.drop_count.get(), 0);
        assert_eq!(core.book.counters.levels.get(), 16 * depth);
        let mut occ = Occupancy::default();
        let start_updates = core.book.counters.updates.get();
        let mut cycles = 0u64;
        for f in &load {
            for n in mii_frame(f, 24) {
                let out: TxNibble = core.clock(n).into();
                sample(&core, &mut occ, out);
                cycles += 1;
            }
        }
        let updates = core.book.counters.updates.get() - start_updates;
        println!(
            "  {depth:>5}  {:>8.1}%  {:>13}  {:>8}  {:>15.1}  {:>15.1}",
            100.0 * occ.book_busy as f64 / occ.cycles as f64,
            occ.plu_fifo,
            core.rx.drop_count.get(),
            occ.book_busy as f64 / updates as f64,
            cycles as f64 / (300.0 * 44.0),
        );
        assert_eq!(core.book.counters.errors.get(), 0);
    }
}

fn main() {
    sample_capture();
    depth_sweep();
}
