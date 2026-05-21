#ifndef ETHERNET_RX_H
#define ETHERNET_RX_H

#include "ap_int.h"
#include "ethernet_constants.h"
#include "packet_views.h"

enum RxState { RX_SEARCH = 0, RX_DATA = 1 };

static ap_uint<8> aapl_symbol_byte(ap_uint<3> index) {
#pragma HLS INLINE
  switch ((unsigned)index) {
  case 0:
  case 1:
    return 'A';
  case 2:
    return 'P';
  case 3:
    return 'L';
  default:
    return ' ';
  }
}

static void rx_capture_payload_byte(
    ap_uint<8> rx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<8> data_byte,
    ap_uint<8> rx_fcs_tail[4],
    ap_uint<3> &rx_fcs_tail_count,
    ap_uint<11> &rx_payload_count,
    bool &rx_truncated) {
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
static void ethernet_rx_parser_step(
    bool frame_start,
    bool byte_valid,
    ap_uint<8> data_byte,
    bool frame_end,
    ap_uint<1> eth_rxerr,
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
  static bool rx_ipv4_candidate = false;
  static ap_uint<4> rx_ipv4_ihl = 0;
  static ap_uint<11> rx_ip_header_bytes = 0;
  static ap_uint<16> rx_ipv4_total_len = 0;
  static ap_uint<16> rx_ipv4_flags_fragment = 0;
  static ap_uint<8> rx_ipv4_protocol = 0;
  static ap_uint<11> rx_udp_offset = 0;
  static ap_uint<11> rx_udp_payload_offset = 0;
  static ap_uint<16> rx_udp_len = 0;
  static ap_uint<8> rx_iex_version = 0;
  static ap_uint<16> rx_iex_protocol_id = 0;
  static ap_uint<16> rx_iex_message_count = 0;
  static ap_uint<11> rx_iex_payload_end = 0;
  static bool rx_iex_header_valid = false;
  static ap_uint<16> rx_iex_messages_left = 0;
  static ap_uint<11> rx_iex_next_block_offset = 0;
  static ap_uint<16> rx_iex_message_length = 0;
  static ap_uint<11> rx_iex_message_offset = 0;
  static bool rx_iex_message_has_symbol = false;
  static ap_uint<20> rx_iex_symbol_match_mask = 0;
  static ap_uint<8> rx_iex_symbol_counts[20] = {0};
#pragma HLS ARRAY_PARTITION variable = rx_iex_symbol_counts complete

  meta.valid = false;
  meta.truncated = false;
  meta.iex_deep = false;
  meta.dst_mac = 0;
  meta.src_mac = 0;
  meta.ethertype = 0;
  meta.payload_len = 0;
  for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
    meta.iex_symbol_counts[k] = 0;
  }

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
    rx_ipv4_candidate = false;
    rx_ipv4_ihl = 0;
    rx_ip_header_bytes = 0;
    rx_ipv4_total_len = 0;
    rx_ipv4_flags_fragment = 0;
    rx_ipv4_protocol = 0;
    rx_udp_offset = 0;
    rx_udp_payload_offset = 0;
    rx_udp_len = 0;
    rx_iex_version = 0;
    rx_iex_protocol_id = 0;
    rx_iex_message_count = 0;
    rx_iex_payload_end = 0;
    rx_iex_header_valid = false;
    rx_iex_messages_left = 0;
    rx_iex_next_block_offset = 0;
    rx_iex_message_length = 0;
    rx_iex_message_offset = 0;
    rx_iex_message_has_symbol = false;
    rx_iex_symbol_match_mask = 0;
    for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
      rx_iex_symbol_counts[k] = 0;
    }
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
        rx_capture_payload_byte(
            rx_payload_buf,
            data_byte,
            rx_fcs_tail,
            rx_fcs_tail_count,
            rx_payload_count,
            rx_truncated);
        break;
      }

      if (rx_header.ethertype == IPV4_ETHERTYPE &&
          rx_byte_index >= ETH_HEADER_BYTES) {
        ap_uint<11> ip_index = rx_byte_index - ETH_HEADER_BYTES;
        if (ip_index == 0) {
          ap_uint<4> version = data_byte.range(7, 4);
          ap_uint<4> ihl = data_byte.range(3, 0);
          rx_ipv4_ihl = ihl;
          rx_ip_header_bytes = ap_uint<11>(ihl) << 2;
          ap_uint<11> udp_offset = ETH_HEADER_BYTES + (ap_uint<11>(ihl) << 2);
          rx_udp_offset = udp_offset;
          rx_udp_payload_offset = udp_offset + UDP_HEADER_BYTES;
          rx_ipv4_candidate = (version == 4) && (ihl >= 5);
        } else if (ip_index == 2) {
          rx_ipv4_total_len.range(15, 8) = data_byte;
        } else if (ip_index == 3) {
          rx_ipv4_total_len.range(7, 0) = data_byte;
        } else if (ip_index == 6) {
          rx_ipv4_flags_fragment.range(15, 8) = data_byte;
        } else if (ip_index == 7) {
          rx_ipv4_flags_fragment.range(7, 0) = data_byte;
        } else if (ip_index == 9) {
          rx_ipv4_protocol = data_byte;
        }

        if (rx_byte_index == rx_udp_offset + 4) {
          rx_udp_len.range(15, 8) = data_byte;
        } else if (rx_byte_index == rx_udp_offset + 5) {
          ap_uint<16> udp_len =
              (ap_uint<16>(rx_udp_len.range(15, 8)) << 8) | data_byte;
          rx_udp_len = udp_len;
          rx_iex_payload_end =
              rx_udp_payload_offset + (ap_uint<11>(udp_len) - UDP_HEADER_BYTES);
        }

        if (rx_byte_index == rx_udp_payload_offset + 0) {
          rx_iex_version = data_byte;
        } else if (rx_byte_index == rx_udp_payload_offset + 2) {
          rx_iex_protocol_id.range(7, 0) = data_byte;
        } else if (rx_byte_index == rx_udp_payload_offset + 3) {
          rx_iex_protocol_id =
              (ap_uint<16>(data_byte) << 8) | rx_iex_protocol_id.range(7, 0);
        } else if (
            rx_byte_index ==
            rx_udp_payload_offset + IEX_TP_MESSAGE_COUNT_OFFSET) {
          rx_iex_message_count.range(7, 0) = data_byte;
        } else if (
            rx_byte_index ==
            rx_udp_payload_offset + IEX_TP_MESSAGE_COUNT_OFFSET + 1) {
          rx_iex_message_count =
              (ap_uint<16>(data_byte) << 8) | rx_iex_message_count.range(7, 0);
        } else if (
            rx_byte_index == rx_udp_payload_offset + IEX_TP_HEADER_BYTES - 1) {
          bool ipv4_ok =
              rx_ipv4_candidate && (rx_ipv4_protocol == IPV4_PROTOCOL_UDP) &&
              ((rx_ipv4_flags_fragment & 0x3fff) == 0) &&
              (rx_ipv4_total_len >= rx_ip_header_bytes + UDP_HEADER_BYTES);
          bool udp_ok =
              (rx_udp_len >= UDP_HEADER_BYTES + IEX_TP_HEADER_BYTES) &&
              (rx_udp_len <= rx_ipv4_total_len - rx_ip_header_bytes);
          rx_iex_header_valid = ipv4_ok && udp_ok && (rx_iex_version == 1) &&
                                (rx_iex_protocol_id == IEX_DEEP_PROTOCOL_ID);
          if (rx_iex_header_valid) {
            rx_iex_messages_left = rx_iex_message_count;
            rx_iex_next_block_offset =
                rx_udp_payload_offset + IEX_TP_HEADER_BYTES;
          }
        }

        if (rx_iex_header_valid) {
          if (rx_iex_messages_left != 0 &&
              rx_byte_index == rx_iex_next_block_offset) {
            rx_iex_message_length.range(7, 0) = data_byte;
          } else if (
              rx_iex_messages_left != 0 &&
              rx_byte_index == rx_iex_next_block_offset + 1) {
            ap_uint<16> message_length = (ap_uint<16>(data_byte) << 8) |
                                         rx_iex_message_length.range(7, 0);
            rx_iex_message_length = message_length;
            ap_uint<11> message_offset =
                rx_iex_next_block_offset + IEX_MESSAGE_BLOCK_HEADER_BYTES;
            rx_iex_message_offset = message_offset;
            ap_uint<12> next_block =
                ap_uint<12>(message_offset) + message_length;
            bool message_valid =
                (message_length != 0) && (next_block <= rx_iex_payload_end);
            rx_iex_message_has_symbol =
                message_valid &&
                (message_length >= IEX_SYMBOL_OFFSET + IEX_SYMBOL_BYTES);
            rx_iex_symbol_match_mask =
                0xfffff; // Initialize match mask for all 20 symbols
            rx_iex_next_block_offset = next_block.range(10, 0);
            rx_iex_messages_left = rx_iex_messages_left - 1;
          } else if (
              rx_iex_message_has_symbol &&
              rx_byte_index >= rx_iex_message_offset + IEX_SYMBOL_OFFSET &&
              rx_byte_index < rx_iex_message_offset + IEX_SYMBOL_OFFSET +
                                  IEX_SYMBOL_BYTES) {
            ap_uint<3> symbol_index =
                rx_byte_index - (rx_iex_message_offset + IEX_SYMBOL_OFFSET);
            for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
              bool byte_match = (data_byte == TOP_20_SYMBOLS[k][symbol_index]);
              rx_iex_symbol_match_mask[k] =
                  rx_iex_symbol_match_mask[k] && byte_match;
            }
            if (symbol_index == 7) {
              for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
                if (rx_iex_symbol_match_mask[k]) {
                  rx_iex_symbol_counts[k]++;
                }
              }
              rx_iex_message_has_symbol = false;
            }
          }
        }
      }
      rx_byte_index++;
    }
  }

  if (frame_end) {
    meta.valid = (rx_byte_index >= ETH_HEADER_BYTES + 4) &&
                 (rx_fcs_tail_count == 4) && !rx_frame_error;
    meta.truncated = rx_truncated;
    meta.iex_deep = rx_iex_header_valid;
    meta.dst_mac = rx_header.dst_mac;
    meta.src_mac = rx_header.src_mac;
    meta.ethertype = rx_header.ethertype;
    meta.payload_len = rx_payload_count;
    for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
      meta.iex_symbol_counts[k] = rx_iex_symbol_counts[k];
    }

    rx_state = RX_SEARCH;
    rx_preamble_count = 0;
    rx_byte_index = 0;
    rx_frame_error = false;
    rx_fcs_tail_count = 0;
    rx_payload_count = 0;
    rx_truncated = false;
    rx_iex_header_valid = false;
    rx_iex_messages_left = 0;
    rx_iex_message_has_symbol = false;
    rx_iex_symbol_match_mask = 0;
    for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
      rx_iex_symbol_counts[k] = 0;
    }
  }
}

#endif
