#ifndef ETHERNET_TX_QUEUE_H
#define ETHERNET_TX_QUEUE_H

#include "ap_int.h"
#include "ethernet_framer.h"
#include "ethernet_packet_queue.h"

// TX integration step drains complete packets from the TX ring and feeds the
// reusable Ethernet framer.
static void ethernet_tx_queue_step(
    EthHeader tx_headers[TX_PACKET_SLOTS],
    ap_uint<11> tx_payload_lens[TX_PACKET_SLOTS],
    bool tx_valid[TX_PACKET_SLOTS],
    ap_uint<8> tx_payloads[TX_PACKET_SLOTS][MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<2> &tx_read_idx,
    ap_uint<1> &eth_tx_en,
    ap_uint<4> &eth_txd,
    ap_uint<1> &tx_frame_toggle,
    ap_uint<1> &tx_active) {
#pragma HLS INLINE
  static bool framer_active = false;
  static ap_uint<2> framer_slot_idx = 0;

  ap_uint<2> read_idx = tx_read_idx;
  unsigned read_idx_int = read_idx;
  unsigned framer_slot_idx_int = framer_slot_idx;
  bool start_request = !framer_active && tx_valid[read_idx_int];
  EthHeader start_header = tx_headers[read_idx_int];
  ap_uint<11> start_payload_len = tx_payload_lens[read_idx_int];

  if (start_request) {
    framer_active = true;
    framer_slot_idx = read_idx;
    framer_slot_idx_int = read_idx_int;
  }

  bool tx_idle = false;
  ethernet_tx_framer_step(
      start_request,
      start_header,
      start_payload_len,
      tx_payloads[framer_slot_idx_int],
      eth_tx_en,
      eth_txd,
      tx_frame_toggle,
      tx_active,
      tx_idle);

  if (framer_active && tx_idle) {
    tx_valid[framer_slot_idx_int] = false;
    tx_read_idx = framer_slot_idx + 1;
    framer_active = false;
  }
}

#endif
