#include "ap_int.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" void ethernet_l2_endpoint_hls(
    ap_uint<1> eth_rx_dv, ap_uint<4> eth_rxd, ap_uint<1> eth_rxerr,
    ap_uint<1> &eth_tx_en, ap_uint<4> &eth_txd, ap_uint<1> &rx_accept_toggle,
    ap_uint<1> &tx_frame_toggle, ap_uint<1> &rx_active, ap_uint<1> &tx_active);

extern "C" void ethernet_l2_endpoint_hls_test_frame(
    ap_uint<48> dst_mac, ap_uint<16> ethertype, ap_uint<11> payload_len,
    ap_uint<16> cycle, ap_uint<1> &eth_tx_en, ap_uint<4> &eth_txd,
    ap_uint<1> &tx_frame_toggle, ap_uint<1> &tx_active);

static void step(ap_uint<1> rx_dv, ap_uint<4> rxd, ap_uint<1> rxerr,
                 ap_uint<1> &tx_en, ap_uint<4> &txd, ap_uint<1> &rx_accept,
                 ap_uint<1> &tx_frame, ap_uint<1> &rx_active,
                 ap_uint<1> &tx_active) {
  ethernet_l2_endpoint_hls(rx_dv, rxd, rxerr, tx_en, txd, rx_accept, tx_frame,
                           rx_active, tx_active);
}

static std::vector<uint8_t> collect_tx_frame(unsigned max_cycles) {
  std::vector<uint8_t> frame;
  ap_uint<1> tx_en = 0, rx_accept = 0, tx_frame = 0, rx_active = 0,
             tx_active = 0;
  ap_uint<4> txd = 0;
  bool in_frame = false;
  bool low = false;
  uint8_t byte = 0;

  for (unsigned i = 0; i < max_cycles; ++i) {
    step(0, 0, 0, tx_en, txd, rx_accept, tx_frame, rx_active, tx_active);
    if (tx_en) {
      in_frame = true;
      if (!low) {
        byte = (uint8_t)txd;
      } else {
        frame.push_back(byte | ((uint8_t)txd << 4));
      }
      low = !low;
    } else if (in_frame) {
      for (unsigned drain = 0; drain < 64; ++drain) {
        step(0, 0, 0, tx_en, txd, rx_accept, tx_frame, rx_active, tx_active);
      }
      break;
    }
  }

  return frame;
}

static std::vector<uint8_t> collect_test_frame(unsigned payload_len,
                                               unsigned max_cycles) {
  std::vector<uint8_t> frame;
  ap_uint<1> tx_en = 0, tx_frame = 0, tx_active = 0;
  ap_uint<4> txd = 0;
  bool in_frame = false;
  bool low = false;
  uint8_t byte = 0;

  for (unsigned i = 0; i < max_cycles; ++i) {
    ethernet_l2_endpoint_hls_test_frame(0x112233445566ULL, 0x88b5, payload_len,
                                        i, tx_en, txd, tx_frame, tx_active);
    if (tx_en) {
      in_frame = true;
      if (!low) {
        byte = (uint8_t)txd;
      } else {
        frame.push_back(byte | ((uint8_t)txd << 4));
      }
      low = !low;
    } else if (in_frame) {
      for (unsigned drain = 0; drain < 64; ++drain) {
        ethernet_l2_endpoint_hls_test_frame(0x112233445566ULL, 0x88b5,
                                            payload_len, i + drain + 1, tx_en,
                                            txd, tx_frame, tx_active);
      }
      break;
    }
  }

  return frame;
}

static uint32_t crc32_ethernet(const std::vector<uint8_t> &frame,
                               unsigned first, unsigned count) {
  uint32_t crc = 0xffffffffu;
  for (unsigned i = 0; i < count; ++i) {
    uint8_t data = frame[first + i];
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc ^ data) & 1u) {
        crc = (crc >> 1) ^ 0xedb88320u;
      } else {
        crc >>= 1;
      }
      data >>= 1;
    }
  }
  return ~crc;
}

static uint32_t read_le32(const std::vector<uint8_t> &frame, unsigned index) {
  return (uint32_t)frame[index] | ((uint32_t)frame[index + 1] << 8) |
         ((uint32_t)frame[index + 2] << 16) |
         ((uint32_t)frame[index + 3] << 24);
}

static void assert_valid_tx_frame(const std::vector<uint8_t> &frame,
                                  const std::vector<uint8_t> &dst,
                                  const std::vector<uint8_t> &payload) {
  const unsigned body_len = 14 + (payload.size() < 46 ? 46 : payload.size());
  assert(frame.size() == 8 + body_len + 4);

  for (int i = 0; i < 7; ++i) {
    assert(frame[i] == 0x55);
  }
  assert(frame[7] == 0xd5);

  const unsigned eth = 8;
  for (unsigned i = 0; i < 6; ++i) {
    assert(frame[eth + i] == dst[i]);
  }

  const uint8_t src[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  for (unsigned i = 0; i < 6; ++i) {
    assert(frame[eth + 6 + i] == src[i]);
  }

  assert(frame[eth + 12] == 0x88);
  assert(frame[eth + 13] == 0xb5);

  for (unsigned i = 0; i < payload.size(); ++i) {
    assert(frame[eth + 14 + i] == payload[i]);
  }
  for (unsigned i = payload.size(); i < 46; ++i) {
    assert(frame[eth + 14 + i] == 0x00);
  }

  uint32_t expected_fcs = crc32_ethernet(frame, eth, body_len);
  uint32_t actual_fcs = read_le32(frame, eth + body_len);
  if (expected_fcs != actual_fcs) {
    std::fprintf(stderr, "bad FCS: expected 0x%08x got 0x%08x\n", expected_fcs,
                 actual_fcs);
  }
  assert(actual_fcs == expected_fcs);
}

static void assert_valid_test_frame(const std::vector<uint8_t> &frame,
                                    unsigned payload_len) {
  std::vector<uint8_t> payload(payload_len);
  for (unsigned i = 0; i < payload_len; ++i) {
    payload[i] = (uint8_t)i;
  }

  const unsigned body_len = 14 + (payload_len < 46 ? 46 : payload_len);
  if (frame.size() != 8 + body_len + 4) {
    std::fprintf(stderr,
                 "bad test frame size for payload %u: expected %u got %u\n",
                 payload_len, 8 + body_len + 4, (unsigned)frame.size());
  }
  assert(frame.size() == 8 + body_len + 4);
  assert_valid_tx_frame(frame, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, payload);
}

static void send_rx_frame(const std::vector<uint8_t> &frame) {
  ap_uint<1> tx_en = 0, rx_accept = 0, tx_frame = 0, rx_active = 0,
             tx_active = 0;
  ap_uint<4> txd = 0;

  for (uint8_t byte : frame) {
    step(1, byte & 0x0f, 0, tx_en, txd, rx_accept, tx_frame, rx_active,
         tx_active);
    step(1, byte >> 4, 0, tx_en, txd, rx_accept, tx_frame, rx_active,
         tx_active);
  }
  step(0, 0, 0, tx_en, txd, rx_accept, tx_frame, rx_active, tx_active);
}

static void assert_no_tx_frame(unsigned max_cycles) {
  std::vector<uint8_t> frame = collect_tx_frame(max_cycles);
  assert(frame.empty());
}

int main() {
  ap_uint<1> tx_en = 0, rx_accept = 0, tx_frame = 0, rx_active = 0,
             tx_active = 0;
  ap_uint<4> txd = 0;

  std::vector<uint8_t> beacon = collect_tx_frame(512);
  assert_valid_tx_frame(
      beacon, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
      {'A', 'R', 'T', 'Y', '_', 'B', 'E', 'A', 'C', 'O', 'N'});

  for (int i = 0; i < 16; ++i) {
    step(0, 0, 0, tx_en, txd, rx_accept, tx_frame, rx_active, tx_active);
  }

  std::vector<uint8_t> valid_rx_frame = {
      0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xd5, 0x02,
      0x00, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x0b, 0x0c, 0x0d,
      0x0e, 0x0f, 0x88, 0xb5, 'p',  'i',  'n',  'g'};
  send_rx_frame(valid_rx_frame);

  std::vector<uint8_t> ack = collect_tx_frame(512);
  assert_valid_tx_frame(ack, {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},
                        {'A', 'R', 'T', 'Y', '_', 'A', 'C', 'K'});

  std::vector<uint8_t> wrong_ethertype = valid_rx_frame;
  wrong_ethertype[20] = 0x08;
  wrong_ethertype[21] = 0x00;
  send_rx_frame(wrong_ethertype);
  assert_no_tx_frame(256);

  std::vector<uint8_t> wrong_dst = valid_rx_frame;
  wrong_dst[8] = 0x02;
  wrong_dst[13] = 0x02;
  send_rx_frame(wrong_dst);
  assert_no_tx_frame(256);

  std::vector<uint8_t> broadcast_rx = valid_rx_frame;
  for (unsigned i = 8; i < 14; ++i) {
    broadcast_rx[i] = 0xff;
  }
  send_rx_frame(broadcast_rx);
  std::vector<uint8_t> broadcast_ack = collect_tx_frame(512);
  assert_valid_tx_frame(broadcast_ack, {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},
                        {'A', 'R', 'T', 'Y', '_', 'A', 'C', 'K'});

  assert_valid_test_frame(collect_test_frame(0, 256), 0);
  assert_valid_test_frame(collect_test_frame(8, 256), 8);
  assert_valid_test_frame(collect_test_frame(47, 256), 47);
  assert_valid_test_frame(collect_test_frame(1024, 4096), 1024);

  return 0;
}
