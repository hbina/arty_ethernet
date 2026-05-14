#ifndef ETHERNET_RX_H
#define ETHERNET_RX_H

#include "ap_int.h"
#include "ethernet_constants.h"
#include "packet_views.h"

enum RxState { RX_SEARCH = 0, RX_DATA = 1 };

static void
rx_capture_payload_byte(ap_uint<8> rx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
                        ap_uint<8> data_byte, ap_uint<8> rx_fcs_tail[4],
                        ap_uint<3> &rx_fcs_tail_count,
                        ap_uint<11> &rx_payload_count, bool &rx_truncated) {
#pragma HLS INLINE
  if (rx_fcs_tail_count < 4) {
    rx_fcs_tail[rx_fcs_tail_count] = data_byte;
    rx_fcs_tail_count++;
    return;
  }

  ap_uint<8> payload_byte = rx_fcs_tail[0];
  rx_fcs_tail[0] = rx_fcs_tail[1];
  rx_fcs_tail[1] = rx_fcs_tail[2];
  rx_fcs_tail[2] = rx_fcs_tail[3];
  rx_fcs_tail[3] = data_byte;

  if (rx_payload_count < MAX_ETH_PAYLOAD_BYTES) {
    rx_payload_buf[rx_payload_count] = payload_byte;
    rx_payload_count++;
  } else {
    rx_truncated = true;
  }
}

// Capture an Ethernet frame from assembled RX bytes.
// The parser skips a standard preamble/SFD if present, records the Ethernet
// header, buffers up to 1500 payload bytes, and strips the four wire FCS bytes
// with a tail delay. CRC validation is intentionally out of scope here.
static void
ethernet_rx_parser_step(bool frame_start, bool byte_valid, ap_uint<8> data_byte,
                        bool frame_end, ap_uint<1> eth_rxerr,
                        ap_uint<8> rx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
                        EthernetFrameMeta &meta) {
#pragma HLS INLINE
  static RxState rx_state = RX_SEARCH;
  static ap_uint<4> rx_preamble_count = 0;
  static ap_uint<16> rx_byte_index = 0;
  static bool rx_frame_error = false;
  static EthHeader rx_header = {0, 0, 0};
  static ap_uint<8> rx_fcs_tail[4] = {0, 0, 0, 0};
#pragma HLS ARRAY_PARTITION variable = rx_fcs_tail complete
  static ap_uint<3> rx_fcs_tail_count = 0;
  static ap_uint<11> rx_payload_count = 0;
  static bool rx_truncated = false;
  static ap_uint<16> rx_arp_hw_type = 0;
  static ap_uint<16> rx_arp_proto_type = 0;
  static ap_uint<8> rx_arp_hw_len = 0;
  static ap_uint<8> rx_arp_proto_len = 0;
  static ap_uint<16> rx_arp_opcode = 0;
  static ap_uint<48> rx_arp_sender_mac = 0;
  static ap_uint<32> rx_arp_sender_ip = 0;
  static ap_uint<32> rx_arp_target_ip = 0;
  static ap_uint<8> rx_ipv4_version_ihl = 0;
  static ap_uint<16> rx_ipv4_total_len = 0;
  static ap_uint<16> rx_ipv4_flags_fragment = 0;
  static ap_uint<8> rx_ipv4_protocol = 0;
  static ap_uint<32> rx_ipv4_src_ip = 0;
  static ap_uint<32> rx_ipv4_dst_ip = 0;
  static ap_uint<16> rx_udp_src_port = 0;
  static ap_uint<16> rx_udp_dst_port = 0;
  static ap_uint<16> rx_udp_len = 0;

  meta.valid = false;
  meta.truncated = false;
  meta.dst_mac = 0;
  meta.src_mac = 0;
  meta.ethertype = 0;
  meta.payload_len = 0;
  meta.arp_hw_type = 0;
  meta.arp_proto_type = 0;
  meta.arp_hw_len = 0;
  meta.arp_proto_len = 0;
  meta.arp_opcode = 0;
  meta.arp_sender_mac = 0;
  meta.arp_sender_ip = 0;
  meta.arp_target_ip = 0;
  meta.ipv4_version_ihl = 0;
  meta.ipv4_total_len = 0;
  meta.ipv4_flags_fragment = 0;
  meta.ipv4_protocol = 0;
  meta.ipv4_src_ip = 0;
  meta.ipv4_dst_ip = 0;
  meta.udp_src_port = 0;
  meta.udp_dst_port = 0;
  meta.udp_len = 0;

  if (frame_start) {
    rx_state = RX_SEARCH;
    rx_preamble_count = 0;
    rx_byte_index = 0;
    rx_frame_error = false;
    rx_header.dst_mac = 0;
    rx_header.src_mac = 0;
    rx_header.ethertype = 0;
    rx_fcs_tail_count = 0;
    rx_payload_count = 0;
    rx_truncated = false;
    rx_arp_hw_type = 0;
    rx_arp_proto_type = 0;
    rx_arp_hw_len = 0;
    rx_arp_proto_len = 0;
    rx_arp_opcode = 0;
    rx_arp_sender_mac = 0;
    rx_arp_sender_ip = 0;
    rx_arp_target_ip = 0;
    rx_ipv4_version_ihl = 0;
    rx_ipv4_total_len = 0;
    rx_ipv4_flags_fragment = 0;
    rx_ipv4_protocol = 0;
    rx_ipv4_src_ip = 0;
    rx_ipv4_dst_ip = 0;
    rx_udp_src_port = 0;
    rx_udp_dst_port = 0;
    rx_udp_len = 0;
  }

  rx_frame_error = rx_frame_error || (eth_rxerr != 0);

  if (byte_valid) {
    bool process_data_byte = false;

    if (rx_state == RX_SEARCH) {
      if (data_byte == 0x55) {
        if (rx_preamble_count != 15) {
          rx_preamble_count++;
        }
      } else if (data_byte == 0xd5 && rx_preamble_count != 0) {
        rx_state = RX_DATA;
        rx_byte_index = 0;
      } else {
        rx_state = RX_DATA;
        process_data_byte = true;
      }
    } else {
      process_data_byte = true;
    }

    if (process_data_byte) {
      switch ((unsigned)rx_byte_index) {
      case 0:
        rx_header.dst_mac.range(47, 40) = data_byte;
        break;
      case 1:
        rx_header.dst_mac.range(39, 32) = data_byte;
        break;
      case 2:
        rx_header.dst_mac.range(31, 24) = data_byte;
        break;
      case 3:
        rx_header.dst_mac.range(23, 16) = data_byte;
        break;
      case 4:
        rx_header.dst_mac.range(15, 8) = data_byte;
        break;
      case 5:
        rx_header.dst_mac.range(7, 0) = data_byte;
        break;
      case 6:
        rx_header.src_mac.range(47, 40) = data_byte;
        break;
      case 7:
        rx_header.src_mac.range(39, 32) = data_byte;
        break;
      case 8:
        rx_header.src_mac.range(31, 24) = data_byte;
        break;
      case 9:
        rx_header.src_mac.range(23, 16) = data_byte;
        break;
      case 10:
        rx_header.src_mac.range(15, 8) = data_byte;
        break;
      case 11:
        rx_header.src_mac.range(7, 0) = data_byte;
        break;
      case 12:
        rx_header.ethertype.range(15, 8) = data_byte;
        break;
      case 13:
        rx_header.ethertype.range(7, 0) = data_byte;
        break;
      default:
        ap_uint<11> payload_index = rx_byte_index - ETH_HEADER_BYTES;
        switch ((unsigned)payload_index) {
        case 0:
          rx_arp_hw_type.range(15, 8) = data_byte;
          rx_ipv4_version_ihl = data_byte;
          break;
        case 1:
          rx_arp_hw_type.range(7, 0) = data_byte;
          break;
        case 2:
          rx_arp_proto_type.range(15, 8) = data_byte;
          rx_ipv4_total_len.range(15, 8) = data_byte;
          break;
        case 3:
          rx_arp_proto_type.range(7, 0) = data_byte;
          rx_ipv4_total_len.range(7, 0) = data_byte;
          break;
        case 4:
          rx_arp_hw_len = data_byte;
          break;
        case 5:
          rx_arp_proto_len = data_byte;
          break;
        case 6:
          rx_arp_opcode.range(15, 8) = data_byte;
          rx_ipv4_flags_fragment.range(15, 8) = data_byte;
          break;
        case 7:
          rx_arp_opcode.range(7, 0) = data_byte;
          rx_ipv4_flags_fragment.range(7, 0) = data_byte;
          break;
        case 8:
          rx_arp_sender_mac.range(47, 40) = data_byte;
          break;
        case 9:
          rx_arp_sender_mac.range(39, 32) = data_byte;
          rx_ipv4_protocol = data_byte;
          break;
        case 10:
          rx_arp_sender_mac.range(31, 24) = data_byte;
          break;
        case 11:
          rx_arp_sender_mac.range(23, 16) = data_byte;
          break;
        case 12:
          rx_arp_sender_mac.range(15, 8) = data_byte;
          rx_ipv4_src_ip.range(31, 24) = data_byte;
          break;
        case 13:
          rx_arp_sender_mac.range(7, 0) = data_byte;
          rx_ipv4_src_ip.range(23, 16) = data_byte;
          break;
        case 14:
          rx_arp_sender_ip.range(31, 24) = data_byte;
          rx_ipv4_src_ip.range(15, 8) = data_byte;
          break;
        case 15:
          rx_arp_sender_ip.range(23, 16) = data_byte;
          rx_ipv4_src_ip.range(7, 0) = data_byte;
          break;
        case 16:
          rx_arp_sender_ip.range(15, 8) = data_byte;
          rx_ipv4_dst_ip.range(31, 24) = data_byte;
          break;
        case 17:
          rx_arp_sender_ip.range(7, 0) = data_byte;
          rx_ipv4_dst_ip.range(23, 16) = data_byte;
          break;
        case 18:
          rx_ipv4_dst_ip.range(15, 8) = data_byte;
          break;
        case 19:
          rx_ipv4_dst_ip.range(7, 0) = data_byte;
          break;
        case 20:
          rx_udp_src_port.range(15, 8) = data_byte;
          break;
        case 21:
          rx_udp_src_port.range(7, 0) = data_byte;
          break;
        case 22:
          rx_udp_dst_port.range(15, 8) = data_byte;
          break;
        case 23:
          rx_udp_dst_port.range(7, 0) = data_byte;
          break;
        case 24:
          rx_arp_target_ip.range(31, 24) = data_byte;
          rx_udp_len.range(15, 8) = data_byte;
          break;
        case 25:
          rx_arp_target_ip.range(23, 16) = data_byte;
          rx_udp_len.range(7, 0) = data_byte;
          break;
        case 26:
          rx_arp_target_ip.range(15, 8) = data_byte;
          break;
        case 27:
          rx_arp_target_ip.range(7, 0) = data_byte;
          break;
        default:
          break;
        }
        rx_capture_payload_byte(rx_payload_buf, data_byte, rx_fcs_tail,
                                rx_fcs_tail_count, rx_payload_count,
                                rx_truncated);
        break;
      }
      rx_byte_index++;
    }
  }

  if (frame_end) {
    meta.valid = (rx_byte_index >= ETH_HEADER_BYTES + 4) &&
                 (rx_fcs_tail_count == 4) && !rx_frame_error;
    meta.truncated = rx_truncated;
    meta.dst_mac = rx_header.dst_mac;
    meta.src_mac = rx_header.src_mac;
    meta.ethertype = rx_header.ethertype;
    meta.payload_len = rx_payload_count;
    meta.arp_hw_type = rx_arp_hw_type;
    meta.arp_proto_type = rx_arp_proto_type;
    meta.arp_hw_len = rx_arp_hw_len;
    meta.arp_proto_len = rx_arp_proto_len;
    meta.arp_opcode = rx_arp_opcode;
    meta.arp_sender_mac = rx_arp_sender_mac;
    meta.arp_sender_ip = rx_arp_sender_ip;
    meta.arp_target_ip = rx_arp_target_ip;
    meta.ipv4_version_ihl = rx_ipv4_version_ihl;
    meta.ipv4_total_len = rx_ipv4_total_len;
    meta.ipv4_flags_fragment = rx_ipv4_flags_fragment;
    meta.ipv4_protocol = rx_ipv4_protocol;
    meta.ipv4_src_ip = rx_ipv4_src_ip;
    meta.ipv4_dst_ip = rx_ipv4_dst_ip;
    meta.udp_src_port = rx_udp_src_port;
    meta.udp_dst_port = rx_udp_dst_port;
    meta.udp_len = rx_udp_len;

    rx_state = RX_SEARCH;
    rx_preamble_count = 0;
    rx_byte_index = 0;
    rx_frame_error = false;
    rx_fcs_tail_count = 0;
    rx_payload_count = 0;
    rx_truncated = false;
  }
}

#endif
