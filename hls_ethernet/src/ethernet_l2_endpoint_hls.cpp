#include "ap_int.h"
#include "ethernet_packet_queue.h"
#include "ethernet_protocol_handler.h"
#include "ethernet_rx_queue.h"
#include "ethernet_tx_queue.h"

// External Vitis HLS top level.
// hls_top.v instantiates this block directly on the 25 MHz Ethernet clock.
// The function is ap_ctrl_none and pipelined with II=1 so it behaves like
// always-running hardware rather than a call/return accelerator.
extern "C" void ethernet_l2_endpoint_hls(
    ap_uint<1> eth_rx_dv,
    ap_uint<4> eth_rxd,
    ap_uint<1> eth_rxerr,
    ap_uint<1> &eth_tx_en,
    ap_uint<4> &eth_txd,
    ap_uint<1> &tx_frame_toggle,
    ap_uint<1> &rx_active,
    ap_uint<1> &tx_active) {
#pragma HLS INTERFACE ap_none port = eth_rx_dv
#pragma HLS INTERFACE ap_none port = eth_rxd
#pragma HLS INTERFACE ap_none port = eth_rxerr
#pragma HLS INTERFACE ap_none port = eth_tx_en
#pragma HLS INTERFACE ap_none port = eth_txd
#pragma HLS INTERFACE ap_none port = tx_frame_toggle
#pragma HLS INTERFACE ap_none port = rx_active
#pragma HLS INTERFACE ap_none port = tx_active
#pragma HLS INTERFACE ap_ctrl_none port = return
#pragma HLS PIPELINE II = 1

  static RxPacketQueue rx_queue = {};
#pragma HLS ARRAY_PARTITION variable = rx_queue.valid complete
#pragma HLS DEPENDENCE variable = rx_queue.valid inter false
#pragma HLS ARRAY_PARTITION variable = rx_queue.bytes complete dim = 1
#pragma HLS BIND_STORAGE variable = rx_queue.bytes type = ram_t2p impl = bram

  static TxPacketQueue tx_queue = {};
#pragma HLS ARRAY_PARTITION variable = tx_queue.valid complete
#pragma HLS DEPENDENCE variable = tx_queue.valid inter false
#pragma HLS ARRAY_PARTITION variable = tx_queue.bytes complete dim = 1
#pragma HLS BIND_STORAGE variable = tx_queue.bytes type = ram_t2p impl = bram

  ethernet_rx_queue_step(eth_rx_dv, eth_rxd, eth_rxerr, rx_queue, rx_active);

  protocol_queue_step(rx_queue, tx_queue);

  ethernet_tx_queue_step(
      tx_queue,
      eth_tx_en,
      eth_txd,
      tx_frame_toggle,
      tx_active);
}
