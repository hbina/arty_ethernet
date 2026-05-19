#include "../src/ethernet_packet_queue.h"
#include "../src/ethernet_periodic_timers.h"
#include "../src/ethernet_rx.h"
#include "../src/ethernet_tx_payloads.h"
#include "../src/packet_views.h"
#include "ap_int.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

extern "C" void ethernet_l2_endpoint_hls(
    ap_uint<1> eth_rx_dv,
    ap_uint<4> eth_rxd,
    ap_uint<1> eth_rxerr,
    ap_uint<1> &eth_tx_en,
    ap_uint<4> &eth_txd,
    ap_uint<1> &tx_frame_toggle,
    ap_uint<1> &rx_active,
    ap_uint<1> &tx_active);

extern "C" void ethernet_l2_endpoint_hls_test_frame(
    ap_uint<48> dst_mac,
    ap_uint<16> ethertype,
    ap_uint<11> payload_len,
    ap_uint<16> cycle,
    ap_uint<1> &eth_tx_en,
    ap_uint<4> &eth_txd,
    ap_uint<1> &tx_frame_toggle,
    ap_uint<1> &tx_active);

static void step(
    ap_uint<1> rx_dv,
    ap_uint<4> rxd,
    ap_uint<1> rxerr,
    ap_uint<1> &tx_en,
    ap_uint<4> &txd,
    ap_uint<1> &tx_frame,
    ap_uint<1> &rx_active,
    ap_uint<1> &tx_active) {
  ethernet_l2_endpoint_hls(
      rx_dv,
      rxd,
      rxerr,
      tx_en,
      txd,
      tx_frame,
      rx_active,
      tx_active);
}

static std::vector<uint8_t> collect_tx_frame(unsigned max_cycles) {
  std::vector<uint8_t> frame;
  ap_uint<1> tx_en = 0, tx_frame = 0, rx_active = 0, tx_active = 0;
  ap_uint<4> txd = 0;
  bool in_frame = false;
  bool low = false;
  uint8_t byte = 0;

  for (unsigned i = 0; i < max_cycles; ++i) {
    step(0, 0, 0, tx_en, txd, tx_frame, rx_active, tx_active);
    if (tx_en) {
      in_frame = true;
      if (!low) {
        byte = (uint8_t)txd;
      } else {
        frame.push_back(byte | ((uint8_t)txd << 4));
      }
      low = !low;
    } else if (in_frame) {
      break;
    }
  }

  return frame;
}

static std::vector<uint8_t>
collect_test_frame(unsigned payload_len, unsigned max_cycles) {
  std::vector<uint8_t> frame;
  ap_uint<1> tx_en = 0, tx_frame = 0, tx_active = 0;
  ap_uint<4> txd = 0;
  bool in_frame = false;
  bool low = false;
  uint8_t byte = 0;

  for (unsigned i = 0; i < max_cycles; ++i) {
    ethernet_l2_endpoint_hls_test_frame(
        0x112233445566ULL,
        IPV4_ETHERTYPE,
        payload_len,
        i,
        tx_en,
        txd,
        tx_frame,
        tx_active);
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
        ethernet_l2_endpoint_hls_test_frame(
            0x112233445566ULL,
            IPV4_ETHERTYPE,
            payload_len,
            i + drain + 1,
            tx_en,
            txd,
            tx_frame,
            tx_active);
      }
      break;
    }
  }

  return frame;
}

static uint32_t crc32_ethernet(
    const std::vector<uint8_t> &frame,
    unsigned first,
    unsigned count) {
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

static uint16_t read_be16(const std::vector<uint8_t> &frame, unsigned index) {
  return ((uint16_t)frame[index] << 8) | frame[index + 1];
}

static void append_eth_fcs(std::vector<uint8_t> &frame) {
  unsigned eth =
      (frame.size() >= 8 && frame[0] == 0x55 && frame[7] == 0xd5) ? 8 : 0;
  uint32_t fcs = crc32_ethernet(frame, eth, frame.size() - eth);
  frame.push_back((uint8_t)(fcs & 0xff));
  frame.push_back((uint8_t)((fcs >> 8) & 0xff));
  frame.push_back((uint8_t)((fcs >> 16) & 0xff));
  frame.push_back((uint8_t)((fcs >> 24) & 0xff));
}

static uint16_t
ipv4_header_checksum(const std::vector<uint8_t> &frame, unsigned ip) {
  uint32_t sum = 0;
  for (unsigned i = 0; i < 20; i += 2) {
    if (i != 10) {
      sum += read_be16(frame, ip + i);
    }
  }
  while (sum >> 16) {
    sum = (sum & 0xffff) + (sum >> 16);
  }
  return (uint16_t)~sum;
}

static void
append_ipv4_header_checksum(std::vector<uint8_t> &frame, unsigned ip) {
  uint16_t checksum = ipv4_header_checksum(frame, ip);
  frame[ip + 10] = (uint8_t)(checksum >> 8);
  frame[ip + 11] = (uint8_t)checksum;
}

static void
assert_fcs(const std::vector<uint8_t> &frame, unsigned eth, unsigned body_len) {
  uint32_t expected_fcs = crc32_ethernet(frame, eth, body_len);
  uint32_t actual_fcs = read_le32(frame, eth + body_len);
  if (expected_fcs != actual_fcs) {
    std::fprintf(
        stderr,
        "bad FCS: expected 0x%08x got 0x%08x\n",
        expected_fcs,
        actual_fcs);
  }
  assert(actual_fcs == expected_fcs);
}

static std::vector<uint8_t> udp_request_frame(
    const std::vector<uint8_t> &dst,
    const std::vector<uint8_t> &src,
    const std::vector<uint8_t> &src_ip,
    const std::vector<uint8_t> &dst_ip,
    uint16_t src_port,
    uint16_t dst_port,
    uint16_t flags_fragment) {
  std::vector<uint8_t> frame = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xd5};
  frame.insert(frame.end(), dst.begin(), dst.end());
  frame.insert(frame.end(), src.begin(), src.end());
  frame.push_back(0x08);
  frame.push_back(0x00);

  const unsigned ip = frame.size();
  frame.insert(frame.end(), {0x45, 0x00, 0x00, 0x24, 0x12, 0x34});
  frame.push_back((uint8_t)(flags_fragment >> 8));
  frame.push_back((uint8_t)flags_fragment);
  frame.insert(frame.end(), {0x40, 0x11, 0x00, 0x00});
  frame.insert(frame.end(), src_ip.begin(), src_ip.end());
  frame.insert(frame.end(), dst_ip.begin(), dst_ip.end());
  append_ipv4_header_checksum(frame, ip);

  frame.push_back((uint8_t)(src_port >> 8));
  frame.push_back((uint8_t)src_port);
  frame.push_back((uint8_t)(dst_port >> 8));
  frame.push_back((uint8_t)dst_port);
  frame.insert(frame.end(), {0x00, 0x10, 0x00, 0x00});
  frame.insert(frame.end(), {'h', 'e', 'l', 'l', 'o', '!', 0x00, 0x01});
  append_eth_fcs(frame);
  return frame;
}

static void assert_valid_tx_frame(
    const std::vector<uint8_t> &frame,
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

  assert(frame[eth + 12] == 0x08);
  assert(frame[eth + 13] == 0x00);

  for (unsigned i = 0; i < payload.size(); ++i) {
    assert(frame[eth + 14 + i] == payload[i]);
  }
  for (unsigned i = payload.size(); i < 46; ++i) {
    assert(frame[eth + 14 + i] == 0x00);
  }

  assert_fcs(frame, eth, body_len);
}

static std::vector<uint8_t>
tx_frame_from_request(const ProtocolTxRequest &request) {
  ap_uint<8> frame_body[TX_FRAME_BODY_BYTES_INT];
  bool done = false;
  for (ap_uint<11> i = 0; i < request.len && !done; ++i) {
    prepare_tx_slot_frame_body_step(frame_body, request, i, done);
  }

  std::vector<uint8_t> frame = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xd5};
  for (unsigned i = 0; i < request.len; ++i) {
    frame.push_back((uint8_t)frame_body[i]);
  }
  for (unsigned i = request.len; i < MIN_ETH_FRAME_BODY_BYTES; ++i) {
    frame.push_back(0);
  }
  append_eth_fcs(frame);
  return frame;
}

static std::vector<uint8_t> arp_request_frame(
    const std::vector<uint8_t> &dst,
    const std::vector<uint8_t> &src,
    const std::vector<uint8_t> &src_ip,
    const std::vector<uint8_t> &target_ip) {
  std::vector<uint8_t> frame = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xd5};
  frame.insert(frame.end(), dst.begin(), dst.end());
  frame.insert(frame.end(), src.begin(), src.end());
  frame.push_back(0x08);
  frame.push_back(0x06);
  frame.insert(frame.end(), {0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01});
  frame.insert(frame.end(), src.begin(), src.end());
  frame.insert(frame.end(), src_ip.begin(), src_ip.end());
  frame.insert(frame.end(), {0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  frame.insert(frame.end(), target_ip.begin(), target_ip.end());
  append_eth_fcs(frame);
  return frame;
}

static void assert_valid_arp_reply(
    const std::vector<uint8_t> &frame,
    const std::vector<uint8_t> &target_mac,
    const std::vector<uint8_t> &target_ip,
    bool broadcast_ethernet) {
  const unsigned body_len = 14 + 46;
  if (frame.size() != 8 + body_len + 4) {
    std::fprintf(
        stderr,
        "bad ARP frame size: expected %u got %u\n",
        8 + body_len + 4,
        (unsigned)frame.size());
  }
  assert(frame.size() == 8 + body_len + 4);

  for (int i = 0; i < 7; ++i) {
    assert(frame[i] == 0x55);
  }
  assert(frame[7] == 0xd5);

  const unsigned eth = 8;
  for (unsigned i = 0; i < 6; ++i) {
    assert(frame[eth + i] == target_mac[i]);
  }

  const uint8_t fpga_mac[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  for (unsigned i = 0; i < 6; ++i) {
    assert(frame[eth + 6 + i] == fpga_mac[i]);
  }

  assert(frame[eth + 12] == 0x08);
  assert(frame[eth + 13] == 0x06);
  if (broadcast_ethernet) {
    for (unsigned i = 0; i < 6; ++i) {
      assert(frame[eth + i] == 0xff);
    }
  }

  const unsigned arp = eth + 14;
  assert(frame[arp + 0] == 0x00);
  assert(frame[arp + 1] == 0x01);
  assert(frame[arp + 2] == 0x08);
  assert(frame[arp + 3] == 0x00);
  assert(frame[arp + 4] == 0x06);
  assert(frame[arp + 5] == 0x04);
  assert(frame[arp + 6] == 0x00);
  assert(frame[arp + 7] == 0x02);

  for (unsigned i = 0; i < 6; ++i) {
    assert(frame[arp + 8 + i] == fpga_mac[i]);
  }
  assert(frame[arp + 14] == 192);
  assert(frame[arp + 15] == 168);
  assert(frame[arp + 16] == 1);
  assert(frame[arp + 17] == 100);

  for (unsigned i = 0; i < 6; ++i) {
    assert(frame[arp + 18 + i] == target_mac[i]);
  }
  for (unsigned i = 0; i < 4; ++i) {
    assert(frame[arp + 24 + i] == target_ip[i]);
  }
  for (unsigned i = 28; i < 46; ++i) {
    assert(frame[arp + i] == 0x00);
  }

  assert_fcs(frame, eth, body_len);
}

static void assert_valid_unicast_arp_reply(
    const std::vector<uint8_t> &frame,
    const std::vector<uint8_t> &requester_mac,
    const std::vector<uint8_t> &requester_ip) {
  assert_valid_arp_reply(frame, requester_mac, requester_ip, false);
}

static void assert_valid_gratuitous_arp(const std::vector<uint8_t> &frame) {
  assert_valid_arp_reply(
      frame,
      {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
      {192, 168, 1, 100},
      true);
}

static void assert_valid_test_frame(
    const std::vector<uint8_t> &frame,
    unsigned payload_len) {
  std::vector<uint8_t> payload(payload_len);
  for (unsigned i = 0; i < payload_len; ++i) {
    payload[i] = (uint8_t)i;
  }

  const unsigned body_len = 14 + (payload_len < 46 ? 46 : payload_len);
  if (frame.size() != 8 + body_len + 4) {
    std::fprintf(
        stderr,
        "bad test frame size for payload %u: expected %u got %u\n",
        payload_len,
        8 + body_len + 4,
        (unsigned)frame.size());
  }
  assert(frame.size() == 8 + body_len + 4);
  assert_valid_tx_frame(frame, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, payload);
}

static void send_rx_frame(const std::vector<uint8_t> &frame) {
  ap_uint<1> tx_en = 0, tx_frame = 0, rx_active = 0, tx_active = 0;
  ap_uint<4> txd = 0;

  for (uint8_t byte : frame) {
    step(1, byte & 0x0f, 0, tx_en, txd, tx_frame, rx_active, tx_active);
    step(1, byte >> 4, 0, tx_en, txd, tx_frame, rx_active, tx_active);
  }
  step(0, 0, 0, tx_en, txd, tx_frame, rx_active, tx_active);
}

static void assert_no_tx_frame(unsigned max_cycles) {
  std::vector<uint8_t> frame = collect_tx_frame(max_cycles);
  assert(frame.empty());
}

static EthernetFrameMeta parse_rx_frame_direct(
    const std::vector<uint8_t> &frame,
    ap_uint<8> payload[MAX_ETH_PAYLOAD_BYTES_INT]) {
  EthernetFrameMeta meta;
  for (unsigned i = 0; i < frame.size(); ++i) {
    ethernet_rx_parser_step(i == 0, true, frame[i], false, 0, payload, meta);
  }
  ethernet_rx_parser_step(false, false, 0, true, 0, payload, meta);
  return meta;
}

static void test_rx_capture_strips_fcs() {
  ap_uint<8> payload[MAX_ETH_PAYLOAD_BYTES_INT];
  std::vector<uint8_t> frame = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
                                0xd5, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
                                0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x08,
                                0x00, 'p',  'i',  'n',  'g'};
  append_eth_fcs(frame);

  EthernetFrameMeta meta = parse_rx_frame_direct(frame, payload);
  assert(meta.valid);
  assert(!meta.truncated);
  assert(meta.payload_len == 4);
  assert(payload[0] == 'p');
  assert(payload[1] == 'i');
  assert(payload[2] == 'n');
  assert(payload[3] == 'g');
}

static void test_rx_oversized_payload_truncated() {
  ap_uint<8> payload[MAX_ETH_PAYLOAD_BYTES_INT];
  std::vector<uint8_t> frame = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xd5,
                                0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x0b,
                                0x0c, 0x0d, 0x0e, 0x0f, 0x08, 0x00};
  for (unsigned i = 0; i < 1501; ++i) {
    frame.push_back((uint8_t)i);
  }
  append_eth_fcs(frame);

  EthernetFrameMeta meta = parse_rx_frame_direct(frame, payload);
  assert(meta.valid);
  assert(meta.truncated);
  assert(meta.payload_len == MAX_ETH_PAYLOAD_BYTES);
}

static void test_ipv4_view() {
  ap_uint<8> payload[MAX_ETH_PAYLOAD_BYTES_INT];
  for (unsigned i = 0; i < MAX_ETH_PAYLOAD_BYTES_INT; ++i) {
    payload[i] = 0;
  }

  payload[0] = 0x45;
  payload[2] = 0x00;
  payload[3] = 0x14;
  payload[9] = 0x11;
  payload[12] = 192;
  payload[13] = 168;
  payload[14] = 1;
  payload[15] = 10;
  payload[16] = 192;
  payload[17] = 168;
  payload[18] = 1;
  payload[19] = 100;

  PacketView view = {0, 20};
  Ipv4View ipv4 = parse_ipv4_view(payload, view);
  assert(ipv4.valid);
  assert(ipv4.header_len == 5);
  assert(ipv4.total_len == 20);
  assert(ipv4.protocol == 0x11);
  assert(ipv4.flags_fragment == 0);
  assert(ipv4.src_ip == 0xc0a8010a);
  assert(ipv4.dst_ip == 0xc0a80164);
  assert(ipv4.payload_view.offset == 20);
  assert(ipv4.payload_view.len == 0);

  payload[0] = 0x65;
  assert(!parse_ipv4_view(payload, view).valid);

  payload[0] = 0x44;
  assert(!parse_ipv4_view(payload, view).valid);

  payload[0] = 0x45;
  payload[2] = 0x00;
  payload[3] = 0x13;
  assert(!parse_ipv4_view(payload, view).valid);

  payload[3] = 0x15;
  assert(!parse_ipv4_view(payload, view).valid);
}

static void test_udp_view() {
  ap_uint<8> payload[MAX_ETH_PAYLOAD_BYTES_INT];
  for (unsigned i = 0; i < MAX_ETH_PAYLOAD_BYTES_INT; ++i) {
    payload[i] = 0;
  }

  payload[0] = 0x12;
  payload[1] = 0x34;
  payload[2] = 0x9c;
  payload[3] = 0x40;
  payload[4] = 0x00;
  payload[5] = 0x10;
  PacketView view = {0, 16};
  UdpView udp = parse_udp_view(payload, view);
  assert(udp.valid);
  assert(udp.src_port == 0x1234);
  assert(udp.dst_port == 40000);
  assert(udp.length == 16);
  assert(udp.payload_view.offset == 8);
  assert(udp.payload_view.len == 8);

  payload[5] = 0x07;
  assert(!parse_udp_view(payload, view).valid);

  payload[5] = 0x11;
  assert(!parse_udp_view(payload, view).valid);
}

static std::string beacon_payload_string(const ProtocolTxRequest &request) {
  std::string payload;
  for (unsigned i = 0; i < protocol_tx_payload_len(PROTO_TX_BEACON); ++i) {
    payload.push_back((char)protocol_tx_payload_byte(request, i));
  }
  return payload;
}

static void test_beacon_payload_fields() {
  ProtocolTxRequest request = protocol_tx_beacon_request();
  assert(request.header.ethertype == DIAGNOSTIC_BEACON_ETHERTYPE);
  assert(
      beacon_payload_string(request) ==
      "ARTY IP=192.168.001.100 MAC=020000000001 RX=00000000 RXQ=00000000 "
      "RXP=00000000 TXD=00000000 ARP=00000000 UDP=00000000 UP=00000000");

  request.rx_packet_count = 3;
  request.rx_queue_drop_count = 4;
  request.rx_protocol_drop_count = 1;
  request.tx_drop_count = 2;
  request.arp_reply_count = 1;
  request.udp_reply_count = 1;
  request.uptime_beacon_count = 1;
  assert(
      beacon_payload_string(request) ==
      "ARTY IP=192.168.001.100 MAC=020000000001 RX=00000003 RXQ=00000004 "
      "RXP=00000001 TXD=00000002 ARP=00000001 UDP=00000001 UP=00000001");
}

static void test_gratuitous_arp_request_builder() {
  ProtocolTxRequest request = protocol_tx_gratuitous_arp_request();
  assert_valid_gratuitous_arp(tx_frame_from_request(request));
}

static void test_periodic_tx_timer_ticks() {
  PeriodicTxTicks ticks = periodic_tx_timer_step();
  assert(!ticks.beacon_tick);
  assert(!ticks.gratuitous_arp_tick);

  ticks = periodic_tx_timer_step();
  assert(!ticks.beacon_tick);
  assert(!ticks.gratuitous_arp_tick);

  ticks = periodic_tx_timer_step();
  assert(!ticks.beacon_tick);
  assert(!ticks.gratuitous_arp_tick);

  ticks = periodic_tx_timer_step();
  assert(!ticks.beacon_tick);
  assert(!ticks.gratuitous_arp_tick);

  ticks = periodic_tx_timer_step();
  assert(!ticks.beacon_tick);
  assert(!ticks.gratuitous_arp_tick);

  ap_uint<32> beacon_count = BEACON_INTERVAL_CYCLES - 1;
  assert(periodic_counter_tick(BEACON_INTERVAL_CYCLES, beacon_count));
  assert(beacon_count == 0);
  assert(!periodic_counter_tick(BEACON_INTERVAL_CYCLES, beacon_count));
  assert(beacon_count == 1);

  ap_uint<32> gratuitous_arp_count = GRATUITOUS_ARP_INTERVAL_CYCLES - 1;
  assert(periodic_counter_tick(
      GRATUITOUS_ARP_INTERVAL_CYCLES,
      gratuitous_arp_count));
  assert(gratuitous_arp_count == 0);
  assert(!periodic_counter_tick(
      GRATUITOUS_ARP_INTERVAL_CYCLES,
      gratuitous_arp_count));
  assert(gratuitous_arp_count == 1);
}

static RxPacketMeta test_rx_meta(unsigned index) {
  RxPacketMeta meta;
  meta.header.dst_mac = FPGA_MAC;
  meta.header.src_mac = index;
  meta.header.ethertype = ARP_ETHERTYPE;
  meta.payload_len = index;
  meta.truncated = false;
  return meta;
}

static TxPacketMeta test_tx_meta(unsigned frame_body_len) {
  TxPacketMeta meta;
  meta.frame_body_len = frame_body_len;
  return meta;
}

static void test_rx_packet_slot_queue_helpers() {
  RxPacketQueue queue = {};

  assert(!packet_queue_read_slot_valid(queue));
  assert(packet_queue_write_slot_available(queue));
  assert(packet_queue_write_slot(queue) == 0);
  assert(packet_queue_read_slot(queue) == 0);

  queue.bytes[0][0] = 0xab;
  packet_queue_publish_write_slot(queue, test_rx_meta(5));
  assert(packet_queue_read_slot_valid(queue));
  assert(packet_queue_write_slot(queue) == 1);
  assert(queue.meta[0].payload_len == 5);
  assert(queue.bytes[0][0] == 0xab);

  packet_queue_consume_read_slot(queue);
  assert(!packet_queue_read_slot_valid(queue));
  assert(packet_queue_read_slot(queue) == 1);

  for (unsigned i = 0; i < RxPacketQueue::Slots; ++i) {
    assert(packet_queue_write_slot_available(queue));
    packet_queue_publish_write_slot(queue, test_rx_meta(i));
  }
  assert(packet_queue_write_slot(queue) == 1);
  assert(!packet_queue_write_slot_available(queue));

  packet_queue_publish_write_slot(queue, test_rx_meta(99));
  assert(packet_queue_drop_count(queue) == 1);

  for (unsigned i = 0; i < RxPacketQueue::Slots; ++i) {
    assert(packet_queue_read_slot_valid(queue));
    packet_queue_consume_read_slot(queue);
  }
  assert(packet_queue_read_slot(queue) == 1);
  assert(!packet_queue_read_slot_valid(queue));
}

static void test_tx_packet_slot_queue_helpers() {
  TxPacketQueue queue = {};

  assert(!packet_queue_read_slot_valid(queue));
  assert(packet_queue_write_slot_available(queue));

  for (unsigned i = 0; i < TxPacketQueue::Slots; ++i) {
    unsigned slot = packet_queue_write_slot(queue);
    queue.bytes[slot][0] = i;
    packet_queue_publish_write_slot(queue, test_tx_meta(60 + i));
  }
  assert(packet_queue_write_slot(queue) == 0);
  assert(!packet_queue_write_slot_available(queue));

  packet_queue_publish_write_slot(queue, test_tx_meta(99));
  assert(packet_queue_drop_count(queue) == 1);

  for (unsigned i = 0; i < TxPacketQueue::Slots; ++i) {
    unsigned slot = packet_queue_read_slot(queue);
    assert(packet_queue_read_slot_valid(queue));
    assert(queue.meta[slot].frame_body_len == 60 + i);
    assert(queue.bytes[slot][0] == i);
    packet_queue_consume_read_slot(queue);
  }
  assert(packet_queue_read_slot(queue) == 0);
  assert(!packet_queue_read_slot_valid(queue));
}

int main() {
  ap_uint<1> tx_en = 0, tx_frame = 0, rx_active = 0, tx_active = 0;
  ap_uint<4> txd = 0;

  assert_no_tx_frame(512);

  for (int i = 0; i < 16; ++i) {
    step(0, 0, 0, tx_en, txd, tx_frame, rx_active, tx_active);
  }

  std::vector<uint8_t> valid_rx_frame = {
      0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xd5, 0x02,
      0x00, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x0b, 0x0c, 0x0d,
      0x0e, 0x0f, 0x08, 0x00, 'p',  'i',  'n',  'g'};
  append_eth_fcs(valid_rx_frame);
  send_rx_frame(valid_rx_frame);
  assert_no_tx_frame(512);

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
  assert_no_tx_frame(512);

  const std::vector<uint8_t> fpga_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  const std::vector<uint8_t> host_mac = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
  const std::vector<uint8_t> host_ip = {192, 168, 1, 10};
  const std::vector<uint8_t> fpga_ip = {192, 168, 1, 100};
  const std::vector<uint8_t> other_ip = {192, 168, 1, 101};

  send_rx_frame(arp_request_frame(
      {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
      host_mac,
      host_ip,
      fpga_ip));
  std::vector<uint8_t> arp_reply = collect_tx_frame(768);
  assert_valid_unicast_arp_reply(arp_reply, host_mac, host_ip);

  send_rx_frame(arp_request_frame(
      {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
      host_mac,
      host_ip,
      other_ip));
  assert_no_tx_frame(768);

  send_rx_frame(
      udp_request_frame(fpga_mac, host_mac, host_ip, fpga_ip, 49152, 40000, 0));
  assert_no_tx_frame(1024);

  send_rx_frame(udp_request_frame(
      fpga_mac,
      host_mac,
      host_ip,
      other_ip,
      49152,
      40000,
      0));
  assert_no_tx_frame(768);

  send_rx_frame(
      udp_request_frame(fpga_mac, host_mac, host_ip, fpga_ip, 49152, 4321, 0));
  assert_no_tx_frame(768);

  send_rx_frame(udp_request_frame(
      fpga_mac,
      host_mac,
      host_ip,
      fpga_ip,
      49152,
      40000,
      0x2000));
  assert_no_tx_frame(768);

  assert_valid_test_frame(collect_test_frame(0, 256), 0);
  assert_valid_test_frame(collect_test_frame(8, 256), 8);
  assert_valid_test_frame(collect_test_frame(47, 256), 47);
  assert_valid_test_frame(collect_test_frame(1024, 4096), 1024);
  test_rx_capture_strips_fcs();
  test_rx_oversized_payload_truncated();
  test_ipv4_view();
  test_udp_view();
  test_beacon_payload_fields();
  test_gratuitous_arp_request_builder();
  test_periodic_tx_timer_ticks();
  test_rx_packet_slot_queue_helpers();
  test_tx_packet_slot_queue_helpers();

  return 0;
}
