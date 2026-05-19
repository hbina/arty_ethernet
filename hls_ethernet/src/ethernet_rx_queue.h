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
    RxPacketQueue &rx_queue,
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
  ap_uint<3> write_idx = packet_queue_write_slot(rx_queue);
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
    rx_drop_current = !packet_queue_write_slot_available(rx_queue);
    if (rx_drop_current) {
      packet_queue_drop_write(rx_queue);
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
        rx_queue.bytes[write_idx_int],
        meta);
  }

  if (frame_end) {
    if (!rx_drop_current && meta.valid) {
      RxPacketMeta rx_meta;
      rx_meta.header.dst_mac = meta.dst_mac;
      rx_meta.header.src_mac = meta.src_mac;
      rx_meta.header.ethertype = meta.ethertype;
      rx_meta.payload_len = meta.payload_len;
      rx_meta.truncated = meta.truncated;
      packet_queue_publish_write_slot(rx_queue, rx_meta);
    }
    rx_drop_current = false;
  }
}

#endif
