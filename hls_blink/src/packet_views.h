#ifndef PACKET_VIEWS_H
#define PACKET_VIEWS_H

#include "ap_int.h"
#include "ethernet_constants.h"
#include "protocol_helpers.h"

struct PacketView {
  ap_uint<11> offset;
  ap_uint<11> len;
};

struct EthernetFrameMeta {
  bool valid;
  bool truncated;
  ap_uint<48> dst_mac;
  ap_uint<48> src_mac;
  ap_uint<16> ethertype;
  ap_uint<11> payload_len;
};

struct Ipv4View {
  bool valid;
  ap_uint<4> header_len;
  ap_uint<16> total_len;
  ap_uint<8> protocol;
  ap_uint<32> src_ip;
  ap_uint<32> dst_ip;
  PacketView payload_view;
};

static Ipv4View
parse_ipv4_view(const ap_uint<8> bytes[MAX_ETH_PAYLOAD_BYTES_INT],
                PacketView view) {
#pragma HLS INLINE
  Ipv4View ipv4;
  ipv4.valid = false;
  ipv4.header_len = 0;
  ipv4.total_len = 0;
  ipv4.protocol = 0;
  ipv4.src_ip = 0;
  ipv4.dst_ip = 0;
  ipv4.payload_view.offset = view.offset;
  ipv4.payload_view.len = 0;

  if (view.len < 20) {
    return ipv4;
  }

  ap_uint<11> base = view.offset;
  ap_uint<8> version_ihl = bytes[base];
  ap_uint<4> version = version_ihl.range(7, 4);
  ap_uint<4> ihl = version_ihl.range(3, 0);
  ap_uint<8> header_bytes = ap_uint<8>(ihl) << 2;
  ap_uint<16> total_len = read_be16(bytes, base + 2);

  bool header_ok = (version == 4) && (ihl >= 5) && (header_bytes <= view.len);
  bool total_ok = (total_len >= header_bytes) && (total_len <= view.len);
  if (!header_ok || !total_ok) {
    return ipv4;
  }

  ipv4.valid = true;
  ipv4.header_len = ihl;
  ipv4.total_len = total_len;
  ipv4.protocol = bytes[base + 9];
  ipv4.src_ip = read_be32(bytes, base + 12);
  ipv4.dst_ip = read_be32(bytes, base + 16);
  ipv4.payload_view.offset = base + header_bytes;
  ipv4.payload_view.len = total_len - header_bytes;
  return ipv4;
}

#endif
