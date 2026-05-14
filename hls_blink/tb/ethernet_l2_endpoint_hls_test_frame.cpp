#include "../src/ethernet_framer.h"
#include "ap_int.h"

// C-simulation-only harness for testing the generic framer with arbitrary
// payload lengths. This file is added as a testbench source, not synthesized
// RTL.
extern "C" void ethernet_l2_endpoint_hls_test_frame(
    ap_uint<48> dst_mac, ap_uint<16> ethertype, ap_uint<11> payload_len,
    ap_uint<16> cycle, ap_uint<1> &eth_tx_en, ap_uint<4> &eth_txd,
    ap_uint<1> &tx_frame_toggle, ap_uint<1> &tx_active) {
  static ap_uint<8> test_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT];
  static bool initialized = false;
  static bool started = false;

  if (!initialized) {
    // Fill test payload with a deterministic byte ramp: 0, 1, 2, ...
    for (ap_uint<11> i = 0; i < MAX_ETH_PAYLOAD_BYTES; ++i) {
      test_payload_buf[i] = i.range(7, 0);
    }
    initialized = true;
  }

  EthHeader header = {dst_mac, FPGA_MAC, ethertype};
  bool tx_idle = false;
  bool start_request = !started && (cycle == 0);
  if (start_request) {
    started = true;
  }

  ethernet_tx_framer_step(start_request, header, payload_len, test_payload_buf,
                          eth_tx_en, eth_txd, tx_frame_toggle, tx_active,
                          tx_idle);

  if (tx_idle && started && cycle != 0) {
    started = false;
  }
}
