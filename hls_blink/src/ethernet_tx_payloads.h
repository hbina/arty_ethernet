#ifndef ETHERNET_TX_PAYLOADS_H
#define ETHERNET_TX_PAYLOADS_H

#include "ap_int.h"
#include "arp.h"
#include "ethernet_constants.h"
#include "ethernet_packet_queue.h"

static const ap_uint<11> UDP_REPLY_PAYLOAD_BYTES =
    IPV4_HEADER_BYTES + UDP_HEADER_BYTES + 8;
static const ap_uint<16> UDP_REPLY_TOTAL_LEN =
    IPV4_HEADER_BYTES + UDP_HEADER_BYTES + 8;
static const ap_uint<16> UDP_REPLY_UDP_LEN = UDP_HEADER_BYTES + 8;

// Return one byte from the fixed ACK payload literal, ARTY_ACK.
static ap_uint<8> ack_payload_literal_byte(ap_uint<4> index) {
#pragma HLS INLINE
  switch ((unsigned)index) {
  case 0:
    return 'A';
  case 1:
    return 'R';
  case 2:
    return 'T';
  case 3:
    return 'Y';
  case 4:
    return '_';
  case 5:
    return 'A';
  case 6:
    return 'C';
  default:
    return 'K';
  }
}

// Return one byte from the fixed beacon payload literal, ARTY_BEACON.
static ap_uint<8> beacon_payload_literal_byte(ap_uint<4> index) {
#pragma HLS INLINE
  switch ((unsigned)index) {
  case 0:
    return 'A';
  case 1:
    return 'R';
  case 2:
    return 'T';
  case 3:
    return 'Y';
  case 4:
    return '_';
  case 5:
    return 'B';
  case 6:
    return 'E';
  case 7:
    return 'A';
  case 8:
    return 'C';
  case 9:
    return 'O';
  default:
    return 'N';
  }
}

static ap_uint<16> ipv4_reply_checksum(ap_uint<32> dst_ip) {
#pragma HLS INLINE
  ap_uint<32> sum = 0;
  sum += 0x4500;
  sum += UDP_REPLY_TOTAL_LEN;
  sum += 0x0000;
  sum += 0x0000;
  sum += (ap_uint<16>(IPV4_DEFAULT_TTL) << 8) | IPV4_PROTOCOL_UDP;
  sum += FPGA_IP.range(31, 16);
  sum += FPGA_IP.range(15, 0);
  sum += dst_ip.range(31, 16);
  sum += dst_ip.range(15, 0);
  sum = (sum & 0xffff) + (sum >> 16);
  sum = (sum & 0xffff) + (sum >> 16);
  return ~sum;
}

static ap_uint<8> udp_reply_payload_byte(
    ap_uint<6> index,
    ap_uint<32> requester_ip,
    ap_uint<16> requester_port) {
#pragma HLS INLINE
  ap_uint<16> checksum = ipv4_reply_checksum(requester_ip);

  switch ((unsigned)index) {
  case 0:
    return 0x45;
  case 1:
    return 0x00;
  case 2:
    return UDP_REPLY_TOTAL_LEN.range(15, 8);
  case 3:
    return UDP_REPLY_TOTAL_LEN.range(7, 0);
  case 4:
  case 5:
  case 6:
  case 7:
    return 0x00;
  case 8:
    return IPV4_DEFAULT_TTL;
  case 9:
    return IPV4_PROTOCOL_UDP;
  case 10:
    return checksum.range(15, 8);
  case 11:
    return checksum.range(7, 0);
  case 12:
    return FPGA_IP.range(31, 24);
  case 13:
    return FPGA_IP.range(23, 16);
  case 14:
    return FPGA_IP.range(15, 8);
  case 15:
    return FPGA_IP.range(7, 0);
  case 16:
    return requester_ip.range(31, 24);
  case 17:
    return requester_ip.range(23, 16);
  case 18:
    return requester_ip.range(15, 8);
  case 19:
    return requester_ip.range(7, 0);
  case 20:
    return UDP_FPGA_PORT.range(15, 8);
  case 21:
    return UDP_FPGA_PORT.range(7, 0);
  case 22:
    return requester_port.range(15, 8);
  case 23:
    return requester_port.range(7, 0);
  case 24:
    return UDP_REPLY_UDP_LEN.range(15, 8);
  case 25:
    return UDP_REPLY_UDP_LEN.range(7, 0);
  case 26:
  case 27:
    return 0x00;
  default:
    return ack_payload_literal_byte(index - 28);
  }
}

static ap_uint<11> tx_request_payload_len(TxRequestKind request_kind) {
#pragma HLS INLINE
  if (request_kind == TX_REQ_ACK) {
    return 8;
  }
  if (request_kind == TX_REQ_BEACON) {
    return 11;
  }
  if (request_kind == TX_REQ_ARP_REPLY) {
    return ARP_PAYLOAD_BYTES;
  }
  if (request_kind == TX_REQ_UDP_REPLY) {
    return UDP_REPLY_PAYLOAD_BYTES;
  }
  return 0;
}

static EthHeader tx_request_header(const TxRequest &request) {
#pragma HLS INLINE
  EthHeader header;
  header.src_mac = FPGA_MAC;
  header.dst_mac = BROADCAST_MAC;
  header.ethertype = CUSTOM_ETHERTYPE;

  if (request.request_kind == TX_REQ_ACK) {
    header.dst_mac = request.header.dst_mac;
  } else if (request.request_kind == TX_REQ_ARP_REPLY) {
    header.dst_mac = request.arp_requester_mac;
    header.ethertype = ARP_ETHERTYPE;
  } else if (request.request_kind == TX_REQ_UDP_REPLY) {
    header.dst_mac = request.header.dst_mac;
    header.ethertype = IPV4_ETHERTYPE;
  }

  return header;
}

// Write one byte of the currently selected fixed response into a TX slot.
// This intentionally writes one byte per top-level call so the endpoint can
// remain a one-cycle initiation-interval pipeline.
static void prepare_tx_slot_payload_step(
    ap_uint<8> tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
    const TxRequest &request,
    ap_uint<6> payload_index,
    bool &done) {
#pragma HLS INLINE
  TxRequestKind request_kind = request.request_kind;
  ap_uint<11> payload_len = tx_request_payload_len(request_kind);
  ap_uint<8> payload_byte = beacon_payload_literal_byte(payload_index);

  if (request_kind == TX_REQ_ACK) {
    payload_byte = ack_payload_literal_byte(payload_index);
  } else if (request_kind == TX_REQ_ARP_REPLY) {
    payload_byte = arp_reply_payload_byte(
        payload_index,
        request.arp_requester_mac,
        request.arp_requester_ip);
  } else if (request_kind == TX_REQ_UDP_REPLY) {
    payload_byte = udp_reply_payload_byte(
        payload_index,
        request.udp_requester_ip,
        request.udp_requester_port);
  }

  tx_payload_buf[payload_index] = payload_byte;
  done = (payload_len == 0) || (payload_index == payload_len - 1);
}

static TxRequest beacon_request() {
#pragma HLS INLINE
  TxRequest request;
  request.valid = true;
  request.header.dst_mac = BROADCAST_MAC;
  request.header.src_mac = FPGA_MAC;
  request.header.ethertype = CUSTOM_ETHERTYPE;
  request.payload_len = tx_request_payload_len(TX_REQ_BEACON);
  request.request_kind = TX_REQ_BEACON;
  request.arp_requester_mac = 0;
  request.arp_requester_ip = 0;
  request.udp_requester_ip = 0;
  request.udp_requester_port = 0;
  return request;
}

#endif
