//! Small `#[logic]` helpers shared by the units. Each call is inlined into the
//! caller's graph, so a helper used twice is two copies of its logic.
use bitlogic::{Bits, bits, logic};

/// Uppercase ASCII hex digit for a nibble.
#[logic]
pub fn hex_digit(n: Bits<4>) -> Bits<8> {
    let wide = bits::<4>(0).concat::<4, 8>(n);
    if n < bits::<4>(10) {
        wide + bits::<8>(0x30)
    } else {
        wide + bits::<8>(0x37)
    }
}

/// Byte `position` of a 48-bit word, most significant byte first (0..=5;
/// later positions read zero).
#[logic]
pub fn byte_of48(word: Bits<48>, position: Bits<4>) -> Bits<8> {
    [
        word.slice::<8>(40),
        word.slice::<8>(32),
        word.slice::<8>(24),
        word.slice::<8>(16),
        word.slice::<8>(8),
        word.slice::<8>(0),
    ]
    .at(position)
}

/// Nibble `position` of a 32-bit word, most significant nibble first (0..=7;
/// later positions read zero).
#[logic]
pub fn nibble_of32(word: Bits<32>, position: Bits<4>) -> Bits<4> {
    [
        word.slice::<4>(28),
        word.slice::<4>(24),
        word.slice::<4>(20),
        word.slice::<4>(16),
        word.slice::<4>(12),
        word.slice::<4>(8),
        word.slice::<4>(4),
        word.slice::<4>(0),
    ]
    .at(position)
}

/// Byte `index` of a 32-bit word, least significant byte first.
#[logic]
pub fn byte_of32_le(word: Bits<32>, index: Bits<2>) -> Bits<8> {
    [
        word.slice::<8>(0),
        word.slice::<8>(8),
        word.slice::<8>(16),
        word.slice::<8>(24),
    ]
    .at(index)
}

/// Shift `byte` into the low end of a big-endian field when `enable` is set.
#[logic]
pub fn shift_in16(field: Bits<16>, byte: Bits<8>, enable: bool) -> Bits<16> {
    if enable {
        field.slice::<8>(0).concat::<8, 16>(byte)
    } else {
        field
    }
}

#[logic]
pub fn shift_in32(field: Bits<32>, byte: Bits<8>, enable: bool) -> Bits<32> {
    if enable {
        field.slice::<24>(0).concat::<8, 32>(byte)
    } else {
        field
    }
}

#[logic]
pub fn shift_in48(field: Bits<48>, byte: Bits<8>, enable: bool) -> Bits<48> {
    if enable {
        field.slice::<40>(0).concat::<8, 48>(byte)
    } else {
        field
    }
}

/// `lo <= i < hi`.
#[logic]
pub fn within(i: Bits<11>, lo: Bits<11>, hi: Bits<11>) -> bool {
    i >= lo && i < hi
}

/// Increment a 32-bit counter when `enable` is set (wrapping).
#[logic]
pub fn bump32(counter: Bits<32>, enable: bool) -> Bits<32> {
    if enable {
        counter + bits::<32>(1)
    } else {
        counter
    }
}

/// One bit of the reflected CRC-32 (polynomial 0xEDB88320).
#[logic]
pub fn crc32_bit(crc: Bits<32>, bit: bool) -> Bits<32> {
    if crc.bit(0) ^ bit {
        (crc >> 1) ^ bits::<32>(0xEDB8_8320)
    } else {
        crc >> 1
    }
}

/// One byte of the reflected CRC-32, least significant bit first.
#[logic]
pub fn crc32_byte(crc: Bits<32>, data: Bits<8>) -> Bits<32> {
    let c = crc32_bit(crc, data.bit(0));
    let c = crc32_bit(c, data.bit(1));
    let c = crc32_bit(c, data.bit(2));
    let c = crc32_bit(c, data.bit(3));
    let c = crc32_bit(c, data.bit(4));
    let c = crc32_bit(c, data.bit(5));
    let c = crc32_bit(c, data.bit(6));
    crc32_bit(c, data.bit(7))
}

/// Byte `position` of a 32-bit word, most significant byte first (0..=3).
#[logic]
pub fn byte_of32(word: Bits<32>, position: Bits<2>) -> Bits<8> {
    [
        word.slice::<8>(24),
        word.slice::<8>(16),
        word.slice::<8>(8),
        word.slice::<8>(0),
    ]
    .at(position)
}

/// Ones'-complement 16-bit addition (end-around carry), as used by the
/// Internet checksum.
#[logic]
pub fn ones_add(a: Bits<16>, b: Bits<16>) -> Bits<16> {
    let sum = bits::<1>(0).concat::<16, 17>(a) + bits::<1>(0).concat::<16, 17>(b);
    let carry = if sum.bit(16) {
        bits::<16>(1)
    } else {
        bits::<16>(0)
    };
    sum.slice::<16>(0) + carry
}

/// Fold a 32-bit accumulator of 16-bit words into a ones'-complement sum.
/// The accumulator itself must not wrap; a frame adds at most 757 words.
#[logic]
pub fn fold16(acc: Bits<32>) -> Bits<16> {
    ones_add(acc.slice::<16>(0), acc.slice::<16>(16))
}

/// The byte at frame offset `index` as its contribution to a 16-bit word sum:
/// even offsets are high bytes, odd offsets low bytes.
#[logic]
pub fn word_part(byte: Bits<8>, index: Bits<11>) -> Bits<32> {
    let wide = bits::<24>(0).concat::<8, 32>(byte);
    if index.bit(0) { wide } else { wide << 8 }
}

/// RFC 1624 incremental checksum update: replacing 16-bit field `old` with
/// `new` changes checksum `hc` to `~(~hc + ~old + new)`.
#[logic]
pub fn checksum_update(hc: Bits<16>, old: Bits<16>, new: Bits<16>) -> Bits<16> {
    !ones_add(ones_add(!hc, !old), new)
}
