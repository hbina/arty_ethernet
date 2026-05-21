#ifndef IEX_DEEP_FILTER_H
#define IEX_DEEP_FILTER_H

#include "ap_int.h"
#include "ethernet_constants.h"
#include "packet_views.h"
#include "protocol_helpers.h"

struct IexDeepView {
  bool valid;
  ap_uint<11> message_offset;
  ap_uint<11> payload_end;
  ap_uint<16> message_count;
};

static ap_uint<16> read_le16(
    const ap_uint<8> bytes[MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<11> index) {
#pragma HLS INLINE
  return ap_uint<16>(bytes[index]) | (ap_uint<16>(bytes[index + 1]) << 8);
}

static bool symbol_at_offset_matches_any_top_20(
    const ap_uint<8> bytes[MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<11> symbol_offset) {
#pragma HLS INLINE
  for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
    bool match = true;
    for (int i = 0; i < 8; ++i) {
#pragma HLS UNROLL
      match = match && (bytes[symbol_offset + i] == TOP_20_SYMBOLS[k][i]);
    }
    if (match) {
      return true;
    }
  }
  return false;
}

static IexDeepView parse_iex_deep_view(
    const ap_uint<8> bytes[MAX_ETH_PAYLOAD_BYTES_INT],
    PacketView ethernet_payload) {
#pragma HLS INLINE
  IexDeepView view;
  view.valid = false;
  view.message_offset = 0;
  view.payload_end = 0;
  view.message_count = 0;

  Ipv4View ipv4 = parse_ipv4_view(bytes, ethernet_payload);
  if (!ipv4.valid || ipv4.protocol != IPV4_PROTOCOL_UDP ||
      !ipv4_is_unfragmented(ipv4)) {
    return view;
  }

  UdpView udp = parse_udp_view(bytes, ipv4.payload_view);
  if (!udp.valid || udp.payload_view.len < IEX_TP_HEADER_BYTES) {
    return view;
  }

  ap_uint<11> base = udp.payload_view.offset;
  ap_uint<16> protocol_id = read_le16(bytes, base + 2);
  if (bytes[base] != 1 || protocol_id != IEX_DEEP_PROTOCOL_ID) {
    return view;
  }

  view.valid = true;
  view.message_offset = base + IEX_TP_HEADER_BYTES;
  view.payload_end = base + udp.payload_view.len;
  view.message_count = read_le16(bytes, base + IEX_TP_MESSAGE_COUNT_OFFSET);
  return view;
}

static bool iex_deep_message_matches_any_top_20(
    const ap_uint<8> bytes[MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<11> message_offset,
    ap_uint<16> message_length) {
#pragma HLS INLINE
  return message_length >= IEX_SYMBOL_OFFSET + IEX_SYMBOL_BYTES &&
         symbol_at_offset_matches_any_top_20(
             bytes,
             message_offset + IEX_SYMBOL_OFFSET);
}

#endif
