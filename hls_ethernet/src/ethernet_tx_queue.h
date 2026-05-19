#ifndef ETHERNET_TX_QUEUE_H
#define ETHERNET_TX_QUEUE_H

#include "ap_int.h"
#include "ethernet_framer.h"
#include "ethernet_packet_queue.h"

// TX integration step drains complete packets from the TX ring and feeds the
// reusable Ethernet framer.
static void ethernet_tx_queue_step(
    TxPacketQueue &tx_queue,
    ap_uint<1> &eth_tx_en,
    ap_uint<4> &eth_txd,
    ap_uint<1> &tx_frame_toggle,
    ap_uint<1> &tx_active) {
#pragma HLS INLINE
  static bool framer_active = false;
  static ap_uint<2> framer_slot_idx = 0;

  ap_uint<2> read_idx = packet_queue_read_slot(tx_queue);
  unsigned read_idx_int = read_idx;
  unsigned framer_slot_idx_int = framer_slot_idx;
  bool start_request = !framer_active && packet_queue_read_slot_valid(tx_queue);
  ap_uint<11> start_len = tx_queue.meta[read_idx_int].frame_body_len;

  if (start_request) {
    framer_active = true;
    framer_slot_idx = read_idx;
    framer_slot_idx_int = read_idx_int;
  }

  bool tx_idle = false;
  ethernet_tx_framer_step(
      start_request,
      start_len,
      tx_queue.bytes[framer_slot_idx_int],
      eth_tx_en,
      eth_txd,
      tx_frame_toggle,
      tx_active,
      tx_idle);

  if (framer_active && tx_idle) {
    packet_queue_consume_read_slot(tx_queue);
    framer_active = false;
  }
}

#endif
