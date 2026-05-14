#ifndef ETHERNET_RX_H
#define ETHERNET_RX_H

#include "ap_int.h"
#include "ethernet_constants.h"

enum RxState { RX_SEARCH = 0, RX_DATA = 1 };

struct RxEvent {
  bool valid;
  ap_uint<48> dst_mac;
  ap_uint<48> src_mac;
  ap_uint<16> ethertype;
  bool accepted;
};

// Parse an Ethernet frame header from assembled RX bytes.
// The parser skips a standard preamble/SFD if present, records destination MAC,
// source MAC, and EtherType, and marks the frame accepted only when it is a
// valid custom frame for our MAC or broadcast. FCS is not checked here.
static void ethernet_rx_parser_step(bool frame_start, bool byte_valid,
                                    ap_uint<8> data_byte, bool frame_end,
                                    ap_uint<1> eth_rxerr, RxEvent &event) {
#pragma HLS INLINE
  static RxState rx_state = RX_SEARCH;
  static ap_uint<4> rx_preamble_count = 0;
  static ap_uint<16> rx_byte_index = 0;
  static bool rx_frame_error = false;
  static bool rx_dest_broadcast = true;
  static EthHeader rx_header = {0, 0, 0};

  event.valid = false;
  event.dst_mac = 0;
  event.src_mac = 0;
  event.ethertype = 0;
  event.accepted = false;

  if (frame_start) {
    // Clear all header state at the first nibble of a new RX frame.
    rx_state = RX_SEARCH;
    rx_preamble_count = 0;
    rx_byte_index = 0;
    rx_frame_error = false;
    rx_dest_broadcast = true;
    rx_header.dst_mac = 0;
    rx_header.src_mac = 0;
    rx_header.ethertype = 0;
  }

  rx_frame_error = rx_frame_error || (eth_rxerr != 0);

  if (byte_valid) {
    bool process_data_byte = false;

    if (rx_state == RX_SEARCH) {
      // Ignore 0x55 preamble bytes until SFD. If no preamble is present,
      // treat the first non-preamble byte as destination MAC byte 0.
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
      // Capture the Ethernet header byte-by-byte. Later payload/FCS bytes
      // are ignored for now because RX payload buffering is deferred.
      switch ((unsigned)rx_byte_index) {
      case 0:
        rx_header.dst_mac.range(47, 40) = data_byte;
        rx_dest_broadcast = (data_byte == 0xff);
        break;
      case 1:
        rx_header.dst_mac.range(39, 32) = data_byte;
        rx_dest_broadcast = rx_dest_broadcast && (data_byte == 0xff);
        break;
      case 2:
        rx_header.dst_mac.range(31, 24) = data_byte;
        rx_dest_broadcast = rx_dest_broadcast && (data_byte == 0xff);
        break;
      case 3:
        rx_header.dst_mac.range(23, 16) = data_byte;
        rx_dest_broadcast = rx_dest_broadcast && (data_byte == 0xff);
        break;
      case 4:
        rx_header.dst_mac.range(15, 8) = data_byte;
        rx_dest_broadcast = rx_dest_broadcast && (data_byte == 0xff);
        break;
      case 5:
        rx_header.dst_mac.range(7, 0) = data_byte;
        rx_dest_broadcast = rx_dest_broadcast && (data_byte == 0xff);
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
        break;
      }
      rx_byte_index++;
    }
  }

  if (frame_end) {
    // Produce one metadata event at end-of-frame. eth_rxerr and header
    // checks decide whether higher-level logic should respond.
    bool dest_ok = (rx_header.dst_mac == FPGA_MAC) || rx_dest_broadcast;
    bool ethertype_ok = (rx_header.ethertype == CUSTOM_ETHERTYPE);
    event.valid = (rx_byte_index >= ETH_HEADER_BYTES);
    event.dst_mac = rx_header.dst_mac;
    event.src_mac = rx_header.src_mac;
    event.ethertype = rx_header.ethertype;
    event.accepted = event.valid && !rx_frame_error && dest_ok && ethertype_ok;

    rx_state = RX_SEARCH;
    rx_preamble_count = 0;
    rx_byte_index = 0;
    rx_frame_error = false;
    rx_dest_broadcast = true;
  }
}

#endif
