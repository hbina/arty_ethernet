#ifndef ETHERNET_PROTOCOL_HANDLER_H
#define ETHERNET_PROTOCOL_HANDLER_H

#include "ap_int.h"
#include "arp.h"
#include "ethernet_constants.h"
#include "ethernet_packet_queue.h"
#include "ethernet_tx_payloads.h"

enum ProtocolState {
  PROTO_IDLE = 0,
  PROTO_PARSE_ARP = 1,
  PROTO_PARSE_IPV4_UDP = 2,
  PROTO_PREPARE_TX = 3
};

static void start_protocol_tx_request(
    const TxRequest &request, bool tx_valid[TX_PACKET_SLOTS],
    ap_uint<2> tx_write_idx, bool &preparing_payload,
    TxRequest &prepare_request, ap_uint<6> &prepare_index,
    ap_uint<2> &prepare_tx_idx, ap_uint<32> &tx_drop_count) {
#pragma HLS INLINE
  unsigned write_idx_int = tx_write_idx;

  if (!request.valid) {
    return;
  }

  if (tx_valid[write_idx_int]) {
    tx_drop_count++;
  } else {
    preparing_payload = true;
    prepare_request = request;
    prepare_index = 0;
    prepare_tx_idx = tx_write_idx;
  }
}

// Protocol/application handling consumes one saved RX packet at a time and
// builds complete response packets into the TX ring.
static void protocol_queue_step(
    EthHeader rx_headers[RX_PACKET_SLOTS],
    ap_uint<11> rx_payload_lens[RX_PACKET_SLOTS],
    bool rx_valid[RX_PACKET_SLOTS], bool rx_truncated[RX_PACKET_SLOTS],
    ap_uint<8> rx_payloads[RX_PACKET_SLOTS][MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<3> &rx_read_idx, EthHeader tx_headers[TX_PACKET_SLOTS],
    ap_uint<11> tx_payload_lens[TX_PACKET_SLOTS],
    bool tx_valid[TX_PACKET_SLOTS],
    ap_uint<8> tx_payloads[TX_PACKET_SLOTS][MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<2> &tx_write_idx, ap_uint<32> &tx_drop_count,
    ap_uint<1> &rx_accept_toggle) {
#pragma HLS INLINE
  static ap_uint<32> beacon_count = BEACON_INTERVAL_CYCLES - 4;
  static ProtocolState protocol_state = PROTO_IDLE;
  static bool preparing_payload = false;
  static TxRequest prepare_request = {
      false, {0, FPGA_MAC, CUSTOM_ETHERTYPE}, 0, TX_REQ_NONE, 0, 0, 0, 0};
  static ap_uint<6> prepare_index = 0;
  static ap_uint<2> prepare_tx_idx = 0;
  static ap_uint<32> rx_protocol_drop_count = 0;
  static bool rx_accept = false;
  static ap_uint<3> parse_rx_idx = 0;
  static EthHeader parse_header = {0, 0, 0};
  static ap_uint<11> parse_payload_len = 0;
  static ap_uint<5> parse_index = 0;
  static ap_uint<16> parse_arp_hw_type = 0;
  static ap_uint<16> parse_arp_proto_type = 0;
  static ap_uint<8> parse_arp_hw_len = 0;
  static ap_uint<8> parse_arp_proto_len = 0;
  static ap_uint<16> parse_arp_opcode = 0;
  static ap_uint<48> parse_arp_sender_mac = 0;
  static ap_uint<32> parse_arp_sender_ip = 0;
  static ap_uint<32> parse_arp_target_ip = 0;
  static ap_uint<8> parse_ipv4_version_ihl = 0;
  static ap_uint<16> parse_ipv4_total_len = 0;
  static ap_uint<16> parse_ipv4_flags_fragment = 0;
  static ap_uint<8> parse_ipv4_protocol = 0;
  static ap_uint<32> parse_ipv4_src_ip = 0;
  static ap_uint<32> parse_ipv4_dst_ip = 0;
  static ap_uint<16> parse_udp_src_port = 0;
  static ap_uint<16> parse_udp_dst_port = 0;
  static ap_uint<16> parse_udp_len = 0;

  bool beacon_tick = false;
  if (beacon_count == BEACON_INTERVAL_CYCLES - 1) {
    beacon_count = 0;
    beacon_tick = true;
  } else {
    beacon_count++;
  }

  if (!preparing_payload && protocol_state == PROTO_IDLE) {
    ap_uint<3> read_idx = rx_read_idx;
    unsigned read_idx_int = read_idx;

    if (rx_valid[read_idx_int]) {
      EthHeader header = rx_headers[read_idx_int];
      ap_uint<11> payload_len = rx_payload_lens[read_idx_int];
      bool dest_ok =
          (header.dst_mac == FPGA_MAC) || (header.dst_mac == BROADCAST_MAC);

      if (rx_truncated[read_idx_int]) {
        rx_protocol_drop_count++;
        rx_valid[read_idx_int] = false;
        rx_truncated[read_idx_int] = false;
        rx_read_idx = read_idx + 1;
      } else if (dest_ok && header.ethertype == CUSTOM_ETHERTYPE) {
        TxRequest request;
        request.valid = true;
        request.header = header;
        request.header.dst_mac = header.src_mac;
        request.payload_len = tx_request_payload_len(TX_REQ_ACK);
        request.request_kind = TX_REQ_ACK;
        request.arp_requester_mac = 0;
        request.arp_requester_ip = 0;
        request.udp_requester_ip = 0;
        request.udp_requester_port = 0;
        rx_accept = !rx_accept;
        rx_valid[read_idx_int] = false;
        rx_truncated[read_idx_int] = false;
        rx_read_idx = read_idx + 1;
        start_protocol_tx_request(request, tx_valid, tx_write_idx,
                                  preparing_payload, prepare_request,
                                  prepare_index, prepare_tx_idx, tx_drop_count);
      } else if (dest_ok && header.ethertype == ARP_ETHERTYPE &&
                 payload_len >= ARP_PAYLOAD_BYTES) {
        parse_rx_idx = read_idx;
        parse_header = header;
        parse_payload_len = payload_len;
        parse_index = 0;
        parse_arp_hw_type = 0;
        parse_arp_proto_type = 0;
        parse_arp_hw_len = 0;
        parse_arp_proto_len = 0;
        parse_arp_opcode = 0;
        parse_arp_sender_mac = 0;
        parse_arp_sender_ip = 0;
        parse_arp_target_ip = 0;
        protocol_state = PROTO_PARSE_ARP;
      } else if (dest_ok && header.ethertype == IPV4_ETHERTYPE &&
                 payload_len >= IPV4_HEADER_BYTES + UDP_HEADER_BYTES) {
        parse_rx_idx = read_idx;
        parse_header = header;
        parse_payload_len = payload_len;
        parse_index = 0;
        parse_ipv4_version_ihl = 0;
        parse_ipv4_total_len = 0;
        parse_ipv4_flags_fragment = 0;
        parse_ipv4_protocol = 0;
        parse_ipv4_src_ip = 0;
        parse_ipv4_dst_ip = 0;
        parse_udp_src_port = 0;
        parse_udp_dst_port = 0;
        parse_udp_len = 0;
        protocol_state = PROTO_PARSE_IPV4_UDP;
      } else {
        rx_valid[read_idx_int] = false;
        rx_truncated[read_idx_int] = false;
        rx_read_idx = read_idx + 1;
      }
    } else if (beacon_tick) {
      TxRequest request = beacon_request();
      start_protocol_tx_request(request, tx_valid, tx_write_idx,
                                preparing_payload, prepare_request,
                                prepare_index, prepare_tx_idx, tx_drop_count);
    }
  } else if (!preparing_payload && protocol_state == PROTO_PARSE_ARP) {
    unsigned parse_rx_idx_int = parse_rx_idx;
    ap_uint<8> data_byte = rx_payloads[parse_rx_idx_int][parse_index];

    switch ((unsigned)parse_index) {
    case 0:
      parse_arp_hw_type.range(15, 8) = data_byte;
      break;
    case 1:
      parse_arp_hw_type.range(7, 0) = data_byte;
      break;
    case 2:
      parse_arp_proto_type.range(15, 8) = data_byte;
      break;
    case 3:
      parse_arp_proto_type.range(7, 0) = data_byte;
      break;
    case 4:
      parse_arp_hw_len = data_byte;
      break;
    case 5:
      parse_arp_proto_len = data_byte;
      break;
    case 6:
      parse_arp_opcode.range(15, 8) = data_byte;
      break;
    case 7:
      parse_arp_opcode.range(7, 0) = data_byte;
      break;
    case 8:
      parse_arp_sender_mac.range(47, 40) = data_byte;
      break;
    case 9:
      parse_arp_sender_mac.range(39, 32) = data_byte;
      break;
    case 10:
      parse_arp_sender_mac.range(31, 24) = data_byte;
      break;
    case 11:
      parse_arp_sender_mac.range(23, 16) = data_byte;
      break;
    case 12:
      parse_arp_sender_mac.range(15, 8) = data_byte;
      break;
    case 13:
      parse_arp_sender_mac.range(7, 0) = data_byte;
      break;
    case 14:
      parse_arp_sender_ip.range(31, 24) = data_byte;
      break;
    case 15:
      parse_arp_sender_ip.range(23, 16) = data_byte;
      break;
    case 16:
      parse_arp_sender_ip.range(15, 8) = data_byte;
      break;
    case 17:
      parse_arp_sender_ip.range(7, 0) = data_byte;
      break;
    case 24:
      parse_arp_target_ip.range(31, 24) = data_byte;
      break;
    case 25:
      parse_arp_target_ip.range(23, 16) = data_byte;
      break;
    case 26:
      parse_arp_target_ip.range(15, 8) = data_byte;
      break;
    case 27:
      parse_arp_target_ip.range(7, 0) = data_byte;
      break;
    default:
      break;
    }

    if (parse_index == ARP_PAYLOAD_BYTES - 1) {
      bool arp_valid = (parse_arp_hw_type == ARP_HW_TYPE_ETHERNET) &&
                       (parse_arp_proto_type == ARP_PROTO_TYPE_IPV4) &&
                       (parse_arp_hw_len == ARP_HW_LEN_ETHERNET) &&
                       (parse_arp_proto_len == ARP_PROTO_LEN_IPV4) &&
                       (parse_arp_opcode == ARP_OPCODE_REQUEST) &&
                       (parse_arp_target_ip == FPGA_IP);
      if (arp_valid) {
        TxRequest request;
        request.valid = true;
        request.header = parse_header;
        request.payload_len = tx_request_payload_len(TX_REQ_ARP_REPLY);
        request.request_kind = TX_REQ_ARP_REPLY;
        request.arp_requester_mac = parse_arp_sender_mac;
        request.arp_requester_ip = parse_arp_sender_ip;
        request.udp_requester_ip = 0;
        request.udp_requester_port = 0;
        start_protocol_tx_request(request, tx_valid, tx_write_idx,
                                  preparing_payload, prepare_request,
                                  prepare_index, prepare_tx_idx, tx_drop_count);
      }
      rx_valid[parse_rx_idx_int] = false;
      rx_truncated[parse_rx_idx_int] = false;
      rx_read_idx = parse_rx_idx + 1;
      protocol_state = PROTO_IDLE;
    } else {
      parse_index++;
    }
  } else if (!preparing_payload && protocol_state == PROTO_PARSE_IPV4_UDP) {
    unsigned parse_rx_idx_int = parse_rx_idx;
    ap_uint<8> data_byte = rx_payloads[parse_rx_idx_int][parse_index];

    switch ((unsigned)parse_index) {
    case 0:
      parse_ipv4_version_ihl = data_byte;
      break;
    case 2:
      parse_ipv4_total_len.range(15, 8) = data_byte;
      break;
    case 3:
      parse_ipv4_total_len.range(7, 0) = data_byte;
      break;
    case 6:
      parse_ipv4_flags_fragment.range(15, 8) = data_byte;
      break;
    case 7:
      parse_ipv4_flags_fragment.range(7, 0) = data_byte;
      break;
    case 9:
      parse_ipv4_protocol = data_byte;
      break;
    case 12:
      parse_ipv4_src_ip.range(31, 24) = data_byte;
      break;
    case 13:
      parse_ipv4_src_ip.range(23, 16) = data_byte;
      break;
    case 14:
      parse_ipv4_src_ip.range(15, 8) = data_byte;
      break;
    case 15:
      parse_ipv4_src_ip.range(7, 0) = data_byte;
      break;
    case 16:
      parse_ipv4_dst_ip.range(31, 24) = data_byte;
      break;
    case 17:
      parse_ipv4_dst_ip.range(23, 16) = data_byte;
      break;
    case 18:
      parse_ipv4_dst_ip.range(15, 8) = data_byte;
      break;
    case 19:
      parse_ipv4_dst_ip.range(7, 0) = data_byte;
      break;
    case 20:
      parse_udp_src_port.range(15, 8) = data_byte;
      break;
    case 21:
      parse_udp_src_port.range(7, 0) = data_byte;
      break;
    case 22:
      parse_udp_dst_port.range(15, 8) = data_byte;
      break;
    case 23:
      parse_udp_dst_port.range(7, 0) = data_byte;
      break;
    case 24:
      parse_udp_len.range(15, 8) = data_byte;
      break;
    case 25:
      parse_udp_len.range(7, 0) = data_byte;
      break;
    default:
      break;
    }

    if (parse_index == IPV4_HEADER_BYTES + UDP_HEADER_BYTES - 1) {
      ap_uint<4> ipv4_version = parse_ipv4_version_ihl.range(7, 4);
      ap_uint<4> ipv4_ihl = parse_ipv4_version_ihl.range(3, 0);
      bool ipv4_valid =
          (ipv4_version == 4) && (ipv4_ihl == 5) &&
          (parse_ipv4_total_len >= IPV4_HEADER_BYTES + UDP_HEADER_BYTES) &&
          (parse_ipv4_total_len <= parse_payload_len) &&
          (parse_ipv4_dst_ip == FPGA_IP) &&
          (parse_ipv4_protocol == IPV4_PROTOCOL_UDP) &&
          ((parse_ipv4_flags_fragment & 0x3fff) == 0);
      bool udp_valid =
          (parse_udp_len >= UDP_HEADER_BYTES) &&
          (parse_udp_len <= parse_ipv4_total_len - IPV4_HEADER_BYTES) &&
          (parse_udp_dst_port == UDP_FPGA_PORT);
      if (ipv4_valid && udp_valid) {
        TxRequest request;
        request.valid = true;
        request.header = parse_header;
        request.header.dst_mac = parse_header.src_mac;
        request.payload_len = tx_request_payload_len(TX_REQ_UDP_REPLY);
        request.request_kind = TX_REQ_UDP_REPLY;
        request.arp_requester_mac = 0;
        request.arp_requester_ip = 0;
        request.udp_requester_ip = parse_ipv4_src_ip;
        request.udp_requester_port = parse_udp_src_port;
        start_protocol_tx_request(request, tx_valid, tx_write_idx,
                                  preparing_payload, prepare_request,
                                  prepare_index, prepare_tx_idx, tx_drop_count);
      }
      rx_valid[parse_rx_idx_int] = false;
      rx_truncated[parse_rx_idx_int] = false;
      rx_read_idx = parse_rx_idx + 1;
      protocol_state = PROTO_IDLE;
    } else {
      parse_index++;
    }
  }

  if (preparing_payload) {
    bool prepare_done = false;
    unsigned prepare_tx_idx_int = prepare_tx_idx;
    prepare_tx_slot_payload_step(tx_payloads[prepare_tx_idx_int],
                                 prepare_request, prepare_index, prepare_done);
    if (prepare_done) {
      tx_headers[prepare_tx_idx_int] = tx_request_header(prepare_request);
      tx_payload_lens[prepare_tx_idx_int] = prepare_request.payload_len;
      tx_valid[prepare_tx_idx_int] = true;
      tx_write_idx = prepare_tx_idx + 1;
      preparing_payload = false;
      prepare_request.valid = false;
      prepare_request.request_kind = TX_REQ_NONE;
    } else {
      prepare_index++;
    }
  }

  rx_accept_toggle = rx_accept;
}

#endif
