#ifndef ETHERNET_RX_H
#define ETHERNET_RX_H

#include "ap_int.h"
#include "ethernet_constants.h"
#include "packet_views.h"

enum RxState { RX_SEARCH = 0, RX_DATA = 1 };

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

  meta.valid = false;
  meta.truncated = false;
  meta.dst_mac = 0;
  meta.src_mac = 0;
  meta.ethertype = 0;
  meta.payload_len = 0;

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
