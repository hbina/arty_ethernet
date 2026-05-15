#ifndef PROTOCOL_HELPERS_H
#define PROTOCOL_HELPERS_H

#include "ap_int.h"
#include "ethernet_constants.h"

// Return one network-order MAC byte from a packed 48-bit MAC address.
// index 0 is the first byte transmitted on Ethernet.
static ap_uint<8> mac_byte(ap_uint<48> mac, ap_uint<3> index) {
#pragma HLS INLINE
  switch ((unsigned)index) {
  case 0:
    return mac.range(47, 40);
  case 1:
    return mac.range(39, 32);
  case 2:
    return mac.range(31, 24);
  case 3:
    return mac.range(23, 16);
  case 4:
    return mac.range(15, 8);
  default:
    return mac.range(7, 0);
  }
}

static ap_uint<16> read_be16(
    const ap_uint<8> bytes[MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<11> index) {
#pragma HLS INLINE
  return (ap_uint<16>(bytes[index]) << 8) | bytes[index + 1];
}

static ap_uint<32> read_be32(
    const ap_uint<8> bytes[MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<11> index) {
#pragma HLS INLINE
  return (ap_uint<32>(bytes[index]) << 24) |
         (ap_uint<32>(bytes[index + 1]) << 16) |
         (ap_uint<32>(bytes[index + 2]) << 8) | bytes[index + 3];
}

static ap_uint<48> read_mac48(
    const ap_uint<8> bytes[MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<11> index) {
#pragma HLS INLINE
  return (ap_uint<48>(bytes[index]) << 40) |
         (ap_uint<48>(bytes[index + 1]) << 32) |
         (ap_uint<48>(bytes[index + 2]) << 24) |
         (ap_uint<48>(bytes[index + 3]) << 16) |
         (ap_uint<48>(bytes[index + 4]) << 8) | bytes[index + 5];
}

#endif
