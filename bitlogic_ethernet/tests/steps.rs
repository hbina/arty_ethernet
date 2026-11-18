//! Per-function checks: native Rust and the bitlogic IR agree on random
//! inputs (including unreachable states), and the LED unit behaves as the HLS
//! status block.
mod common;

use bitlogic::{Bits, Logic, LogicValue, bits};
use bitlogic_ethernet::{proto, rx, status, tx, zero};
use common::Rng;
use std::fmt::Debug;

fn random<T: LogicValue>(rng: &mut Rng) -> T {
    let values: Vec<u64> = T::schema()
        .leaves()
        .iter()
        .map(|(_, leaf)| {
            let width = leaf.width().unwrap();
            // Bias toward small values so equality guards are exercised.
            let raw = if rng.next().is_multiple_of(4) {
                rng.next() % 4
            } else {
                rng.next()
            };
            raw & (u64::MAX >> (64 - width))
        })
        .collect();
    T::from_values(&values).unwrap()
}

fn agree<I, O>(ir: Logic, native: fn(I) -> O, seed: u64, cases: usize)
where
    I: LogicValue + Copy + Debug,
    O: LogicValue + PartialEq + Debug,
{
    let mut rng = Rng(seed);
    for _ in 0..cases {
        let input: I = random(&mut rng);
        let interpreted: O = ir.evaluate_typed(input).unwrap();
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
