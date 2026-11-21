//! Per-function checks: native Rust and the bitlogic IR agree on random
//! inputs (including unreachable states), and the LED unit behaves as the HLS
//! status block.
mod common;

use bitlogic::{BitVec, Bits, Logic, LogicValue, Schema, bits};
use bitlogic_ethernet::{book, feed, proto, rx, status, tx, zero};
use common::Rng;
use std::fmt::Debug;

/// Random valid leaves: enums take only their declared encodings, and an enum
/// with fields packs the chosen variant's fields into its payload, with
/// random bits above them (don't-care: the IR must ignore them).
fn random_leaves(schema: &Schema, rng: &mut Rng, out: &mut Vec<BitVec>) {
    match schema {
        Schema::Struct { fields, .. } => {
            for (_, field) in fields {
                random_leaves(field, rng, out);
            }
        }
        Schema::Array { element, len } => {
            for _ in 0..*len {
                random_leaves(element, rng, out);
            }
        }
        Schema::Enum { variants, .. } => {
            out.push(BitVec::from(
                variants[(rng.next() % variants.len() as u64) as usize].1,
            ));
        }
        Schema::Tagged { tag, payloads, .. } => {
            let Schema::Enum { variants, .. } = &**tag else {
                unreachable!("a tag is an enum");
            };
            let chosen = (rng.next() % variants.len() as u64) as usize;
            let mut fields = Vec::new();
            for (_, field) in &payloads[chosen].1 {
                random_leaves(field, rng, &mut fields);
            }
            let leaves = schema.encode_variant(&variants[chosen].0, &fields).unwrap();
            let masks = schema.valid_bits(&leaves).unwrap();
            for ((value, mask), (_, leaf)) in leaves.iter().zip(&masks).zip(schema.leaves()) {
                let words: Vec<u64> = (0..usize::from(leaf.width().unwrap()).div_ceil(64))
                    .map(|i| {
                        let v = value.words().get(i).copied().unwrap_or(0);
                        let m = mask.words().get(i).copied().unwrap_or(0);
                        v | (rng.next() & !m)
                    })
                    .collect();
                out.push(BitVec::from_words(&words).masked(leaf.width().unwrap()));
            }
        }
        leaf => {
            let width = leaf.width().unwrap();
            // Bias toward small values so equality guards are exercised.
            let words: Vec<u64> = (0..usize::from(width).div_ceil(64))
                .map(|i| {
                    if rng.next().is_multiple_of(4) {
                        if i == 0 { rng.next() % 4 } else { 0 }
                    } else {
                        rng.next()
                    }
                })
                .collect();
            out.push(BitVec::from_words(&words).masked(width));
        }
    }
}

fn agree<I, O>(ir: Logic, native: fn(I) -> O, seed: u64, cases: usize)
where
    I: LogicValue + Copy + Debug,
    O: LogicValue + PartialEq + Debug,
{
    let mut rng = Rng(seed);
    for _ in 0..cases {
        // Raw leaves, so don't-care payload bits reach the IR as generated.
        let mut leaves = Vec::new();
        random_leaves(&I::schema(), &mut rng, &mut leaves);
        let input = I::from_values(&leaves).unwrap();
        let interpreted = O::from_values(&ir.evaluate(&leaves).unwrap()).unwrap();
        assert_eq!(native(input), interpreted, "input {input:?}");
    }
}

#[test]
fn rx_native_matches_ir() {
    agree(rx::rx_step::ir().unwrap(), rx::rx_step, 1, 20_000);
}

#[test]
fn proto_native_matches_ir() {
    agree(
        proto::proto_step::ir().unwrap(),
        proto::proto_step,
        2,
        20_000,
    );
}

#[test]
fn feed_native_matches_ir() {
    agree(feed::feed_step::ir().unwrap(), feed::feed_step, 5, 20_000);
}

#[test]
fn book_native_matches_ir() {
    agree(book::book_step::ir().unwrap(), book::book_step, 6, 20_000);
}

#[test]
fn symbol_hash_matches_its_definition() {
    let mut rng = Rng(7);
    let ir = book::symbol_hash::ir().unwrap();
    for _ in 0..10_000 {
        let s = rng.next();
        let x = s ^ (s >> 29) ^ (s >> 17) ^ (s << 7);
        let want = (0..6).fold(0, |h, k| h ^ ((x >> (11 * k)) & 0x7ff));
        assert_eq!(book::symbol_hash(bits(s)).get(), want);
        let interpreted: Bits<11> = ir.evaluate_typed(bits::<64>(s)).unwrap();
        assert_eq!(interpreted.get(), want);
    }
}

#[test]
fn tx_native_matches_ir() {
    agree(tx::tx_step::ir().unwrap(), tx::tx_step, 3, 20_000);
}

#[test]
fn status_native_matches_ir() {
    agree(
        status::status_step::ir().unwrap(),
        status::status_step,
        4,
        20_000,
    );
}

#[test]
fn status_leds_follow_events() {
    let step = |state, frame_event, rx_active| {
        status::status_step(status::StatusIn {
            state,
            frame_event,
            rx_active,
            holdoff_cycles: bits(3),
        })
    };
    let mut state: status::StatusState = zero();
    let mut leds = Vec::new();
    for (event, active) in [
        (true, false),
        (false, false),
        (false, false),
        (false, false),
        (false, true),
        (true, false),
        (true, false),
    ] {
        let out = step(state, event, active);
        leds.push(out.led.get());
        state = out.state;
    }
    // bit 0: activity (3-cycle holdoff); bits 3:1: frame count.
    assert_eq!(
        leds,
        vec![0b0011, 0b0011, 0b0011, 0b0010, 0b0011, 0b0101, 0b0111]
    );
}

#[test]
fn helpers_match_independent_definitions() {
    use bitlogic_ethernet::ops;
    let hex = ops::hex_digit::ir().unwrap();
    for n in 0..16 {
        let want = u64::from(format!("{n:X}").as_bytes()[0]);
        assert_eq!(ops::hex_digit(bits(n)).get(), want);
        assert_eq!(
            hex.evaluate_typed::<_, Bits<8>>(bits::<4>(n))
                .unwrap()
                .get(),
            want
        );
    }
    let mut rng = Rng(5);
    for _ in 0..2_000 {
        let word = rng.next() & 0xffff_ffff_ffff;
        for position in 0..6 {
            let want = word.to_be_bytes()[2 + position as usize];
            assert_eq!(
                ops::byte_of48(bits(word), bits(position)).get(),
                u64::from(want)
            );
        }
        let word32 = word & 0xffff_ffff;
        for position in 0..8 {
            let want = (word32 >> (28 - 4 * position)) & 0xf;
            assert_eq!(ops::nibble_of32(bits(word32), bits(position)).get(), want);
        }
        // Folding the reflected byte step over a message gives the standard
        // CRC-32, compared with the independent MSB-first implementation.
        let message = word.to_le_bytes();
        let crc = message.iter().fold(bits::<32>(0xffff_ffff), |c, &b| {
            ops::crc32_byte(c, bits(b.into()))
        });
        assert_eq!((!crc).get() as u32, common::crc32(&message));
    }
}

#[test]
fn checksum_helpers_match_rfc_definitions() {
    use bitlogic_ethernet::ops;
    let mut rng = Rng(11);
    for _ in 0..20_000 {
        let a = rng.next() as u16;
        let b = rng.next() as u16;
        // Ones'-complement addition: 32-bit sum with end-around carry.
        let s = u32::from(a) + u32::from(b);
        let want = ((s & 0xffff) + (s >> 16)) as u16;
        assert_eq!(
            ops::ones_add(bits(a.into()), bits(b.into())).get(),
            u64::from(want)
        );
    }
    // Incremental update (RFC 1624) agrees with recomputing the checksum
    // from scratch after changing one word.
    for _ in 0..2_000 {
        let len = 2 + 2 * (rng.next() % 30) as usize;
        let mut data: Vec<u8> = (0..len).map(|_| rng.next() as u8).collect();
        let at = 2 * (rng.next() as usize % (len / 2));
        let old = u16::from_be_bytes([data[at], data[at + 1]]);
        let new = rng.next() as u16;
        let before = common::internet_checksum(&data);
        data[at..at + 2].copy_from_slice(&new.to_be_bytes());
        let after = common::internet_checksum(&data);
        let updated = ops::checksum_update(bits(before.into()), bits(old.into()), bits(new.into()));
        // Both represent the same ones'-complement value (0x0000 == 0xFFFF).
        let norm = |x: u64| if x == 0xffff { 0 } else { x };
        assert_eq!(norm(updated.get()), norm(after.into()), "{data:?} at {at}");
    }
}

#[test]
fn rx_crc_residue_matches_independent_crc() {
    use bitlogic_ethernet::{ops, rx::CRC32_RESIDUE};
    let mut rng = Rng(12);
    for _ in 0..500 {
        let len = (rng.next() % 200) as usize;
        let mut wire: Vec<u8> = (0..len).map(|_| rng.next() as u8).collect();
        wire.extend(common::crc32(&wire).to_le_bytes());
        let reg = wire.iter().fold(bits::<32>(0xffff_ffff), |c, &b| {
            ops::crc32_byte(c, bits(b.into()))
        });
        assert_eq!(reg.get(), CRC32_RESIDUE);
    }
}
