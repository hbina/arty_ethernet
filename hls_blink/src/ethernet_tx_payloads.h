#ifndef ETHERNET_TX_PAYLOADS_H
#define ETHERNET_TX_PAYLOADS_H

#include "ap_int.h"
#include "arp.h"
#include "ethernet_constants.h"
#include "ethernet_packet_queue.h"
#include "protocol_helpers.h"

static const ap_uint<11> UDP_REPLY_PAYLOAD_BYTES =
    IPV4_HEADER_BYTES + UDP_HEADER_BYTES + 8;
static const ap_uint<16> UDP_REPLY_TOTAL_LEN =
    IPV4_HEADER_BYTES + UDP_HEADER_BYTES + 8;
static const ap_uint<16> UDP_REPLY_UDP_LEN = UDP_HEADER_BYTES + 8;

enum ProtocolTxKind {
  PROTO_TX_NONE = 0,
  PROTO_TX_ACK = 1,
  PROTO_TX_BEACON = 2,
  PROTO_TX_ARP_REPLY = 3,
  PROTO_TX_UDP_REPLY = 4
};

struct ProtocolTxRequest {
  bool valid;
  EthHeader header;
  ap_uint<11> len;
  ProtocolTxKind kind;
  ap_uint<48> arp_requester_mac;
  ap_uint<32> arp_requester_ip;
  ap_uint<32> udp_requester_ip;
  ap_uint<16> udp_requester_port;
  ap_uint<32> beacon_rx_count;
};

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

static ap_uint<8> hex_digit_byte(ap_uint<4> value) {
#pragma HLS INLINE
  if (value < 10) {
    return (ap_uint<8>)('0' + value);
  }
  return (ap_uint<8>)('A' + value - 10);
}

// Return one byte from the beacon payload literal, ARTY_BEACON_RX=<counter>.
static ap_uint<8>
beacon_payload_literal_byte(ap_uint<6> index, ap_uint<32> rx_count) {
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
  case 10:
    return 'N';
  case 11:
    return '_';
  case 12:
    return 'R';
  case 13:
    return 'X';
  case 14:
    return '=';
  case 15:
    return hex_digit_byte(rx_count.range(31, 28));
  case 16:
    return hex_digit_byte(rx_count.range(27, 24));
  case 17:
    return hex_digit_byte(rx_count.range(23, 20));
  case 18:
    return hex_digit_byte(rx_count.range(19, 16));
  case 19:
    return hex_digit_byte(rx_count.range(15, 12));
  case 20:
    return hex_digit_byte(rx_count.range(11, 8));
  case 21:
    return hex_digit_byte(rx_count.range(7, 4));
  default:
    return hex_digit_byte(rx_count.range(3, 0));
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

static ap_uint<11> protocol_tx_payload_len(ProtocolTxKind request_kind) {
#pragma HLS INLINE
  if (request_kind == PROTO_TX_ACK) {
    return 8;
  }
  if (request_kind == PROTO_TX_BEACON) {
    return 23;
  }
  if (request_kind == PROTO_TX_ARP_REPLY) {
    return ARP_PAYLOAD_BYTES;
  }
  if (request_kind == PROTO_TX_UDP_REPLY) {
    return UDP_REPLY_PAYLOAD_BYTES;
  }
  return 0;
}

static ap_uint<11> protocol_tx_frame_body_len(ProtocolTxKind request_kind) {
#pragma HLS INLINE
  return ETH_HEADER_BYTES + protocol_tx_payload_len(request_kind);
}

static ap_uint<8>
protocol_tx_payload_byte(const ProtocolTxRequest &request, ap_uint<11> index) {
#pragma HLS INLINE
  ProtocolTxKind request_kind = request.kind;
  ap_uint<8> payload_byte =
      beacon_payload_literal_byte(index, request.beacon_rx_count);

  if (request_kind == PROTO_TX_ACK) {
    payload_byte = ack_payload_literal_byte(index);
  } else if (request_kind == PROTO_TX_ARP_REPLY) {
    payload_byte = arp_reply_payload_byte(
        index,
        request.arp_requester_mac,
        request.arp_requester_ip);
  } else if (request_kind == PROTO_TX_UDP_REPLY) {
    payload_byte = udp_reply_payload_byte(
        index,
        request.udp_requester_ip,
        request.udp_requester_port);
  }

  return payload_byte;
}

// Write one byte of the currently selected fixed response into a TX slot.
// This intentionally writes one byte per top-level call so the endpoint can
// remain a one-cycle initiation-interval pipeline.
static void prepare_tx_slot_frame_body_step(
    ap_uint<8> tx_frame_body_buf[TX_FRAME_BODY_BYTES_INT],
    const ProtocolTxRequest &request,
    ap_uint<11> frame_body_index,
    bool &done) {
#pragma HLS INLINE
  ap_uint<8> frame_body_byte = 0;
  if (frame_body_index < ETH_HEADER_BYTES) {
    frame_body_byte = ethernet_header_byte(request.header, frame_body_index);
  } else {
    ap_uint<11> payload_index = frame_body_index - ETH_HEADER_BYTES;
    frame_body_byte = protocol_tx_payload_byte(request, payload_index);
  }

  tx_frame_body_buf[frame_body_index] = frame_body_byte;
  done = (request.len == 0) || (frame_body_index == request.len - 1);
}

static ProtocolTxRequest protocol_tx_beacon_request() {
#pragma HLS INLINE
  ProtocolTxRequest request;
  request.valid = true;
  request.header.dst_mac = BROADCAST_MAC;
  request.header.src_mac = FPGA_MAC;
  request.header.ethertype = CUSTOM_ETHERTYPE;
  request.len = protocol_tx_frame_body_len(PROTO_TX_BEACON);
  request.kind = PROTO_TX_BEACON;
  request.arp_requester_mac = 0;
  request.arp_requester_ip = 0;
  request.udp_requester_ip = 0;
  request.udp_requester_port = 0;
  request.beacon_rx_count = 0;
  return request;
}

#endif
