#ifndef ARP_H
#define ARP_H

#include "ap_int.h"
#include "ethernet_constants.h"
#include "packet_views.h"
#include "protocol_helpers.h"

static const ap_uint<16> ARP_HW_TYPE_ETHERNET = 1;
static const ap_uint<16> ARP_PROTO_TYPE_IPV4 = 0x0800;
static const ap_uint<8> ARP_HW_LEN_ETHERNET = 6;
static const ap_uint<8> ARP_PROTO_LEN_IPV4 = 4;
static const ap_uint<16> ARP_OPCODE_REQUEST = 1;
static const ap_uint<16> ARP_OPCODE_REPLY = 2;
static const int ARP_PAYLOAD_BYTES_INT = 28;
static const ap_uint<6> ARP_PAYLOAD_BYTES = 28;

struct ArpRequestInfo {
  bool valid;
  ap_uint<48> sender_mac;
  ap_uint<32> sender_ip;
};

static ArpRequestInfo parse_arp_ipv4_request(
    const ap_uint<8> bytes[MAX_ETH_PAYLOAD_BYTES_INT],
    PacketView view) {
#pragma HLS INLINE
  ArpRequestInfo info;
  info.valid = false;
  info.sender_mac = 0;
  info.sender_ip = 0;

  if (view.len < ARP_PAYLOAD_BYTES) {
    return info;
  }

  ap_uint<11> base = view.offset;
  ap_uint<16> hw_type = read_be16(bytes, base + 0);
  ap_uint<16> proto_type = read_be16(bytes, base + 2);
  ap_uint<8> hw_len = bytes[base + 4];
  ap_uint<8> proto_len = bytes[base + 5];
  ap_uint<16> opcode = read_be16(bytes, base + 6);
  ap_uint<48> sender_mac = read_mac48(bytes, base + 8);
  ap_uint<32> sender_ip = read_be32(bytes, base + 14);
  ap_uint<32> target_ip = read_be32(bytes, base + 24);

  info.valid = (hw_type == ARP_HW_TYPE_ETHERNET) &&
               (proto_type == ARP_PROTO_TYPE_IPV4) &&
               (hw_len == ARP_HW_LEN_ETHERNET) &&
               (proto_len == ARP_PROTO_LEN_IPV4) &&
               (opcode == ARP_OPCODE_REQUEST) && (target_ip == FPGA_IP);
  info.sender_mac = sender_mac;
  info.sender_ip = sender_ip;
  return info;
}

static ap_uint<8> arp_reply_payload_byte(
    ap_uint<5> index,
    ap_uint<48> requester_mac,
    ap_uint<32> requester_ip) {
#pragma HLS INLINE
  switch ((unsigned)index) {
  case 0:
    return ARP_HW_TYPE_ETHERNET.range(15, 8);
  case 1:
    return ARP_HW_TYPE_ETHERNET.range(7, 0);
  case 2:
    return ARP_PROTO_TYPE_IPV4.range(15, 8);
  case 3:
    return ARP_PROTO_TYPE_IPV4.range(7, 0);
  case 4:
    return ARP_HW_LEN_ETHERNET;
  case 5:
    return ARP_PROTO_LEN_IPV4;
  case 6:
    return ARP_OPCODE_REPLY.range(15, 8);
  case 7:
    return ARP_OPCODE_REPLY.range(7, 0);
  case 8:
    return mac_byte(FPGA_MAC, 0);
  case 9:
    return mac_byte(FPGA_MAC, 1);
  case 10:
    return mac_byte(FPGA_MAC, 2);
  case 11:
    return mac_byte(FPGA_MAC, 3);
  case 12:
    return mac_byte(FPGA_MAC, 4);
  case 13:
    return mac_byte(FPGA_MAC, 5);
  case 14:
    return FPGA_IP.range(31, 24);
  case 15:
    return FPGA_IP.range(23, 16);
  case 16:
    return FPGA_IP.range(15, 8);
  case 17:
    return FPGA_IP.range(7, 0);
  case 18:
    return mac_byte(requester_mac, 0);
  case 19:
    return mac_byte(requester_mac, 1);
  case 20:
    return mac_byte(requester_mac, 2);
  case 21:
    return mac_byte(requester_mac, 3);
  case 22:
    return mac_byte(requester_mac, 4);
  case 23:
    return mac_byte(requester_mac, 5);
  case 24:
    return requester_ip.range(31, 24);
  case 25:
    return requester_ip.range(23, 16);
  case 26:
    return requester_ip.range(15, 8);
  default:
    return requester_ip.range(7, 0);
  }
}

#endif
