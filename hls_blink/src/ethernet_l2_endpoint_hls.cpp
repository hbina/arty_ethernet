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
    ap_uint<1> &rx_accept_toggle,
    ap_uint<1> &tx_frame_toggle,
    ap_uint<1> &rx_active,
    ap_uint<1> &tx_active) {
#pragma HLS INTERFACE ap_none port = eth_rx_dv
#pragma HLS INTERFACE ap_none port = eth_rxd
#pragma HLS INTERFACE ap_none port = eth_rxerr
#pragma HLS INTERFACE ap_none port = eth_tx_en
#pragma HLS INTERFACE ap_none port = eth_txd
#pragma HLS INTERFACE ap_none port = rx_accept_toggle
#pragma HLS INTERFACE ap_none port = tx_frame_toggle
#pragma HLS INTERFACE ap_none port = rx_active
#pragma HLS INTERFACE ap_none port = tx_active
#pragma HLS INTERFACE ap_ctrl_none port = return
#pragma HLS PIPELINE II = 1

  static EthHeader rx_headers[RX_PACKET_SLOTS];
  static ap_uint<11> rx_payload_lens[RX_PACKET_SLOTS];
  static bool rx_valid[RX_PACKET_SLOTS] = {false};
#pragma HLS ARRAY_PARTITION variable = rx_valid complete
#pragma HLS DEPENDENCE variable = rx_valid inter false
  static bool rx_truncated[RX_PACKET_SLOTS] = {false};
#pragma HLS ARRAY_PARTITION variable = rx_truncated complete
#pragma HLS DEPENDENCE variable = rx_truncated inter false
  static ap_uint<8> rx_payloads[RX_PACKET_SLOTS][MAX_ETH_PAYLOAD_BYTES_INT];
#pragma HLS ARRAY_PARTITION variable = rx_payloads complete dim = 1
#pragma HLS BIND_STORAGE variable = rx_payloads type = ram_t2p impl = bram
  static ap_uint<3> rx_write_idx = 0;
  static ap_uint<3> rx_read_idx = 0;
  static ap_uint<32> rx_drop_count = 0;

  static EthHeader tx_headers[TX_PACKET_SLOTS];
  static ap_uint<11> tx_payload_lens[TX_PACKET_SLOTS];
  static bool tx_valid[TX_PACKET_SLOTS] = {false};
#pragma HLS ARRAY_PARTITION variable = tx_valid complete
#pragma HLS DEPENDENCE variable = tx_valid inter false
  static ap_uint<8> tx_payloads[TX_PACKET_SLOTS][MAX_ETH_PAYLOAD_BYTES_INT];
#pragma HLS ARRAY_PARTITION variable = tx_payloads complete dim = 1
#pragma HLS BIND_STORAGE variable = tx_payloads type = ram_t2p impl = bram
  static ap_uint<2> tx_write_idx = 0;
  static ap_uint<2> tx_read_idx = 0;
  static ap_uint<32> tx_drop_count = 0;

  ethernet_rx_queue_step(
      eth_rx_dv,
      eth_rxd,
      eth_rxerr,
      rx_headers,
      rx_payload_lens,
      rx_valid,
      rx_truncated,
      rx_payloads,
      rx_write_idx,
      rx_drop_count,
      rx_active);

  protocol_queue_step(
      rx_headers,
      rx_payload_lens,
      rx_valid,
      rx_truncated,
      rx_payloads,
      rx_read_idx,
      tx_headers,
      tx_payload_lens,
      tx_valid,
      tx_payloads,
      tx_write_idx,
      tx_drop_count,
      rx_accept_toggle);

  ethernet_tx_queue_step(
      tx_headers,
      tx_payload_lens,
      tx_valid,
      tx_payloads,
      tx_read_idx,
      eth_tx_en,
      eth_txd,
      tx_frame_toggle,
      tx_active);
}
