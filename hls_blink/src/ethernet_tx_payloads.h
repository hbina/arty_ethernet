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
  ap_uint<32> rx_packet_count;
  ap_uint<32> rx_queue_drop_count;
  ap_uint<32> rx_protocol_drop_count;
  ap_uint<32> tx_drop_count;
  ap_uint<32> arp_reply_count;
  ap_uint<32> udp_reply_count;
  ap_uint<32> uptime_beacon_count;
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

static ap_uint<8> hex32_byte(ap_uint<32> value, ap_uint<3> index) {
#pragma HLS INLINE
  switch ((unsigned)index) {
  case 0:
    return hex_digit_byte(value.range(31, 28));
  case 1:
    return hex_digit_byte(value.range(27, 24));
  case 2:
    return hex_digit_byte(value.range(23, 20));
  case 3:
    return hex_digit_byte(value.range(19, 16));
  case 4:
    return hex_digit_byte(value.range(15, 12));
  case 5:
    return hex_digit_byte(value.range(11, 8));
  case 6:
    return hex_digit_byte(value.range(7, 4));
  default:
    return hex_digit_byte(value.range(3, 0));
  }
}

static ap_uint<8> hex48_byte(ap_uint<48> value, ap_uint<4> index) {
#pragma HLS INLINE
  switch ((unsigned)index) {
  case 0:
    return hex_digit_byte(value.range(47, 44));
  case 1:
    return hex_digit_byte(value.range(43, 40));
  case 2:
    return hex_digit_byte(value.range(39, 36));
  case 3:
    return hex_digit_byte(value.range(35, 32));
  case 4:
    return hex_digit_byte(value.range(31, 28));
  case 5:
    return hex_digit_byte(value.range(27, 24));
  case 6:
    return hex_digit_byte(value.range(23, 20));
  case 7:
    return hex_digit_byte(value.range(19, 16));
  case 8:
    return hex_digit_byte(value.range(15, 12));
  case 9:
    return hex_digit_byte(value.range(11, 8));
  case 10:
    return hex_digit_byte(value.range(7, 4));
  default:
    return hex_digit_byte(value.range(3, 0));
  }
}

static ap_uint<8> decimal3_byte(ap_uint<8> value, ap_uint<2> index) {
#pragma HLS INLINE
  ap_uint<8> digit = 0;
  if (index == 0) {
    digit = value / 100;
  } else if (index == 1) {
    digit = (value / 10) % 10;
  } else {
    digit = value % 10;
  }
  return (ap_uint<8>)('0' + digit);
}

static ap_uint<8> ipv4_fixed_byte(ap_uint<32> ip, ap_uint<4> index) {
#pragma HLS INLINE
  switch ((unsigned)index) {
  case 0:
    return decimal3_byte(ip.range(31, 24), 0);
  case 1:
    return decimal3_byte(ip.range(31, 24), 1);
  case 2:
    return decimal3_byte(ip.range(31, 24), 2);
  case 3:
    return '.';
  case 4:
    return decimal3_byte(ip.range(23, 16), 0);
  case 5:
    return decimal3_byte(ip.range(23, 16), 1);
  case 6:
    return decimal3_byte(ip.range(23, 16), 2);
  case 7:
    return '.';
  case 8:
    return decimal3_byte(ip.range(15, 8), 0);
  case 9:
    return decimal3_byte(ip.range(15, 8), 1);
  case 10:
    return decimal3_byte(ip.range(15, 8), 2);
  case 11:
    return '.';
  case 12:
    return decimal3_byte(ip.range(7, 0), 0);
  case 13:
    return decimal3_byte(ip.range(7, 0), 1);
  default:
    return decimal3_byte(ip.range(7, 0), 2);
  }
}

// Return one byte from the diagnostics beacon payload.
static ap_uint<8> beacon_payload_literal_byte(
    ap_uint<8> index,
    const ProtocolTxRequest &request) {
#pragma HLS INLINE
  if (index >= 8 && index < 23) {
    return ipv4_fixed_byte(FPGA_IP, index - 8);
  }
  if (index >= 28 && index < 40) {
    return hex48_byte(FPGA_MAC, index - 28);
  }
  if (index >= 44 && index < 52) {
    return hex32_byte(request.rx_packet_count, index - 44);
  }
  if (index >= 57 && index < 65) {
    return hex32_byte(request.rx_queue_drop_count, index - 57);
  }
  if (index >= 70 && index < 78) {
    return hex32_byte(request.rx_protocol_drop_count, index - 70);
  }
  if (index >= 83 && index < 91) {
    return hex32_byte(request.tx_drop_count, index - 83);
  }
  if (index >= 96 && index < 104) {
    return hex32_byte(request.arp_reply_count, index - 96);
  }
  if (index >= 109 && index < 117) {
    return hex32_byte(request.udp_reply_count, index - 109);
  }
  if (index >= 121 && index < 129) {
    return hex32_byte(request.uptime_beacon_count, index - 121);
  }

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
    return ' ';
  case 5:
    return 'I';
  case 6:
    return 'P';
  case 7:
    return '=';
  case 23:
  case 40:
  case 52:
  case 65:
  case 78:
  case 91:
  case 104:
  case 117:
    return ' ';
  case 24:
    return 'M';
  case 25:
    return 'A';
  case 26:
    return 'C';
  case 27:
  case 43:
  case 56:
  case 69:
  case 82:
  case 95:
  case 108:
  case 120:
    return '=';
  case 41:
    return 'R';
  case 42:
    return 'X';
  case 53:
    return 'R';
  case 54:
    return 'X';
  case 55:
    return 'Q';
  case 66:
    return 'R';
  case 67:
    return 'X';
  case 68:
    return 'P';
  case 79:
    return 'T';
  case 80:
    return 'X';
  case 81:
    return 'D';
  case 92:
    return 'A';
  case 93:
    return 'R';
  case 94:
    return 'P';
  case 105:
    return 'U';
  case 106:
    return 'D';
  case 107:
    return 'P';
  case 118:
    return 'U';
  case 119:
    return 'P';
  default:
    return '0';
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
    return 129;
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
  ap_uint<8> payload_byte = beacon_payload_literal_byte(index, request);

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
  request.rx_packet_count = 0;
  request.rx_queue_drop_count = 0;
  request.rx_protocol_drop_count = 0;
  request.tx_drop_count = 0;
  request.arp_reply_count = 0;
  request.udp_reply_count = 0;
  request.uptime_beacon_count = 0;
  return request;
}

#endif
