#ifndef ETHERNET_RX_QUEUE_H
#define ETHERNET_RX_QUEUE_H

#include "ap_int.h"
#include "ethernet_mii.h"
#include "ethernet_packet_queue.h"
#include "ethernet_rx.h"

// RX integration step for the top-level endpoint. The MII capture path only
// publishes complete Ethernet packets into an 8-slot ring.
static void ethernet_rx_queue_step(
    ap_uint<1> eth_rx_dv,
    ap_uint<4> eth_rxd,
    ap_uint<1> eth_rxerr,
    EthHeader rx_headers[RX_PACKET_SLOTS],
    ap_uint<11> rx_payload_lens[RX_PACKET_SLOTS],
    bool rx_valid[RX_PACKET_SLOTS],
    bool rx_truncated[RX_PACKET_SLOTS],
    ap_uint<8> rx_payloads[RX_PACKET_SLOTS][MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<3> &rx_write_idx,
    ap_uint<32> &rx_drop_count,
    ap_uint<1> &rx_active) {
#pragma HLS INLINE
  static ap_uint<8> rx_drop_payload[MAX_ETH_PAYLOAD_BYTES_INT];
#pragma HLS BIND_STORAGE variable = rx_drop_payload type = ram_t2p impl = bram
  static bool rx_drop_current = false;

  bool frame_start;
  bool byte_valid;
  ap_uint<8> data_byte;
  bool frame_end;
  EthernetFrameMeta meta;
  ap_uint<3> write_idx = rx_write_idx;
  unsigned write_idx_int = write_idx;

  rx_active = eth_rx_dv;
  mii_rx_byte_assembler_step(
      eth_rx_dv,
      eth_rxd,
      frame_start,
      byte_valid,
      data_byte,
      frame_end);

  if (frame_start) {
    rx_drop_current = rx_valid[write_idx_int];
    if (rx_drop_current) {
      rx_drop_count++;
    }
  }

  if (rx_drop_current) {
    ethernet_rx_parser_step(
        frame_start,
        byte_valid,
        data_byte,
        frame_end,
        eth_rxerr,
        rx_drop_payload,
        meta);
  } else {
    ethernet_rx_parser_step(
        frame_start,
        byte_valid,
        data_byte,
        frame_end,
        eth_rxerr,
        rx_payloads[write_idx_int],
        meta);
  }

  if (frame_end) {
    if (!rx_drop_current && meta.valid) {
      if (rx_valid[write_idx_int]) {
        rx_drop_count++;
      } else {
        rx_headers[write_idx_int].dst_mac = meta.dst_mac;
        rx_headers[write_idx_int].src_mac = meta.src_mac;
        rx_headers[write_idx_int].ethertype = meta.ethertype;
        rx_payload_lens[write_idx_int] = meta.payload_len;
        rx_truncated[write_idx_int] = meta.truncated;
        rx_valid[write_idx_int] = true;
        rx_write_idx = write_idx + 1;
      }
    }
    rx_drop_current = false;
  }
}

#endif
