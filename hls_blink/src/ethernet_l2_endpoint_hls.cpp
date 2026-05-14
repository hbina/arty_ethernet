#include "ap_int.h"
#include "arp.h"
#include "ethernet_constants.h"
#include "ethernet_framer.h"
#include "ethernet_mii.h"
#include "ethernet_rx.h"
#include "packet_views.h"

enum TxRequestKind {
  TX_REQ_NONE = 0,
  TX_REQ_ACK = 1,
  TX_REQ_BEACON = 2,
  TX_REQ_ARP_REPLY = 3,
  TX_REQ_UDP_REPLY = 4
};

struct TxRequest {
  bool valid;
  EthHeader header;
  ap_uint<11> payload_len;
  TxRequestKind request_kind;
  ap_uint<48> arp_requester_mac;
  ap_uint<32> arp_requester_ip;
  ap_uint<32> udp_requester_ip;
  ap_uint<16> udp_requester_port;
};

static const ap_uint<11> UDP_REPLY_PAYLOAD_BYTES =
    IPV4_HEADER_BYTES + UDP_HEADER_BYTES + 8;
static const ap_uint<16> UDP_REPLY_TOTAL_LEN =
    IPV4_HEADER_BYTES + UDP_HEADER_BYTES + 8;
static const ap_uint<16> UDP_REPLY_UDP_LEN = UDP_HEADER_BYTES + 8;

// Return one byte from the fixed ACK payload literal, ARTY_ACK.
static ap_uint<8> ack_payload_literal_byte(ap_uint<4> index) {
#pragma HLS INLINE
  switch ((unsigned)index) {
  case 0:
    return 'A';
  case 1:
    return 'R';
  case 2:
    return 'T';
  case 3:
    return 'Y';
  case 4:
    return '_';
  case 5:
    return 'A';
  case 6:
    return 'C';
  default:
    return 'K';
  }
}

// Return one byte from the fixed beacon payload literal, ARTY_BEACON.
static ap_uint<8> beacon_payload_literal_byte(ap_uint<4> index) {
#pragma HLS INLINE
  switch ((unsigned)index) {
  case 0:
    return 'A';
  case 1:
    return 'R';
  case 2:
    return 'T';
  case 3:
    return 'Y';
  case 4:
    return '_';
  case 5:
    return 'B';
  case 6:
    return 'E';
  case 7:
    return 'A';
  case 8:
    return 'C';
  case 9:
    return 'O';
  default:
    return 'N';
  }
}

static ap_uint<16> ipv4_reply_checksum(ap_uint<32> dst_ip) {
#pragma HLS INLINE
  ap_uint<32> sum = 0;
  sum += 0x4500;
  sum += UDP_REPLY_TOTAL_LEN;
  sum += 0x0000;
  sum += 0x0000;
  sum += (ap_uint<16>(IPV4_DEFAULT_TTL) << 8) | IPV4_PROTOCOL_UDP;
  sum += FPGA_IP.range(31, 16);
  sum += FPGA_IP.range(15, 0);
  sum += dst_ip.range(31, 16);
  sum += dst_ip.range(15, 0);
  sum = (sum & 0xffff) + (sum >> 16);
  sum = (sum & 0xffff) + (sum >> 16);
  return ~sum;
}

static ap_uint<8> udp_reply_payload_byte(ap_uint<6> index,
                                         ap_uint<32> requester_ip,
                                         ap_uint<16> requester_port) {
#pragma HLS INLINE
  ap_uint<16> checksum = ipv4_reply_checksum(requester_ip);

  switch ((unsigned)index) {
  case 0:
    return 0x45;
  case 1:
    return 0x00;
  case 2:
    return UDP_REPLY_TOTAL_LEN.range(15, 8);
  case 3:
    return UDP_REPLY_TOTAL_LEN.range(7, 0);
  case 4:
  case 5:
  case 6:
  case 7:
    return 0x00;
  case 8:
    return IPV4_DEFAULT_TTL;
  case 9:
    return IPV4_PROTOCOL_UDP;
  case 10:
    return checksum.range(15, 8);
  case 11:
    return checksum.range(7, 0);
  case 12:
    return FPGA_IP.range(31, 24);
  case 13:
    return FPGA_IP.range(23, 16);
  case 14:
    return FPGA_IP.range(15, 8);
  case 15:
    return FPGA_IP.range(7, 0);
  case 16:
    return requester_ip.range(31, 24);
  case 17:
    return requester_ip.range(23, 16);
  case 18:
    return requester_ip.range(15, 8);
  case 19:
    return requester_ip.range(7, 0);
  case 20:
    return UDP_FPGA_PORT.range(15, 8);
  case 21:
    return UDP_FPGA_PORT.range(7, 0);
  case 22:
    return requester_port.range(15, 8);
  case 23:
    return requester_port.range(7, 0);
  case 24:
    return UDP_REPLY_UDP_LEN.range(15, 8);
  case 25:
    return UDP_REPLY_UDP_LEN.range(7, 0);
  case 26:
  case 27:
    return 0x00;
  default:
    return ack_payload_literal_byte(index - 28);
  }
}

// Write one byte of the currently selected fixed payload into BRAM.
// This intentionally writes one byte per top-level call so the endpoint can
// remain a one-cycle initiation-interval pipeline.
static void
prepare_payload_step(ap_uint<8> tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
                     TxRequestKind request_kind, ap_uint<6> payload_index,
                     ap_uint<48> arp_requester_mac,
                     ap_uint<32> arp_requester_ip, ap_uint<32> udp_requester_ip,
                     ap_uint<16> udp_requester_port, bool &done) {
#pragma HLS INLINE
  ap_uint<11> payload_len = 11;
  ap_uint<8> payload_byte = beacon_payload_literal_byte(payload_index);

  if (request_kind == TX_REQ_ACK) {
    payload_len = 8;
    payload_byte = ack_payload_literal_byte(payload_index);
  } else if (request_kind == TX_REQ_ARP_REPLY) {
    payload_len = ARP_PAYLOAD_BYTES;
    payload_byte = arp_reply_payload_byte(payload_index, arp_requester_mac,
                                          arp_requester_ip);
  } else if (request_kind == TX_REQ_UDP_REPLY) {
    payload_len = UDP_REPLY_PAYLOAD_BYTES;
    payload_byte = udp_reply_payload_byte(payload_index, udp_requester_ip,
                                          udp_requester_port);
  }

  tx_payload_buf[payload_index] = payload_byte;
  done = (payload_index == payload_len - 1);
}

// RX integration step for the top-level endpoint.
// Accepted custom frames arm a pending ACK to the received source MAC and
// toggle rx_accept_toggle for the LED/status clock-domain crossing in
// hls_top.v.
static void
ethernet_rx_step(ap_uint<1> eth_rx_dv, ap_uint<4> eth_rxd, ap_uint<1> eth_rxerr,
                 bool &ack_pending, ap_uint<48> &ack_dst_pending,
                 bool &arp_reply_pending,
                 ap_uint<48> &arp_requester_mac_pending,
                 ap_uint<32> &arp_requester_ip_pending, bool &udp_reply_pending,
                 ap_uint<48> &udp_requester_mac_pending,
                 ap_uint<32> &udp_requester_ip_pending,
                 ap_uint<16> &udp_requester_port_pending,
                 ap_uint<1> &rx_accept_toggle, ap_uint<1> &rx_active) {
#pragma HLS INLINE
  static bool rx_accept = false;
  static ap_uint<8> rx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT];
#pragma HLS BIND_STORAGE variable = rx_payload_buf type = ram_t2p impl = bram

  bool frame_start;
  bool byte_valid;
  ap_uint<8> data_byte;
  bool frame_end;
  EthernetFrameMeta meta;

  rx_active = eth_rx_dv;
  mii_rx_byte_assembler_step(eth_rx_dv, eth_rxd, frame_start, byte_valid,
                             data_byte, frame_end);
  ethernet_rx_parser_step(frame_start, byte_valid, data_byte, frame_end,
                          eth_rxerr, rx_payload_buf, meta);

  if (meta.valid && !meta.truncated) {
    bool dest_ok =
        (meta.dst_mac == FPGA_MAC) || (meta.dst_mac == BROADCAST_MAC);

    if (dest_ok && meta.ethertype == CUSTOM_ETHERTYPE) {
      ack_pending = true;
      ack_dst_pending = meta.src_mac;
      rx_accept = !rx_accept;
    } else if (dest_ok && meta.ethertype == ARP_ETHERTYPE) {
      bool arp_valid = (meta.payload_len >= ARP_PAYLOAD_BYTES) &&
                       (meta.arp_hw_type == ARP_HW_TYPE_ETHERNET) &&
                       (meta.arp_proto_type == ARP_PROTO_TYPE_IPV4) &&
                       (meta.arp_hw_len == ARP_HW_LEN_ETHERNET) &&
                       (meta.arp_proto_len == ARP_PROTO_LEN_IPV4) &&
                       (meta.arp_opcode == ARP_OPCODE_REQUEST) &&
                       (meta.arp_target_ip == FPGA_IP);
      if (arp_valid) {
        arp_reply_pending = true;
        arp_requester_mac_pending = meta.arp_sender_mac;
        arp_requester_ip_pending = meta.arp_sender_ip;
      }
    } else if (dest_ok && meta.ethertype == IPV4_ETHERTYPE) {
      ap_uint<4> ipv4_version = meta.ipv4_version_ihl.range(7, 4);
      ap_uint<4> ipv4_ihl = meta.ipv4_version_ihl.range(3, 0);
      bool ipv4_valid =
          (meta.payload_len >= IPV4_HEADER_BYTES + UDP_HEADER_BYTES) &&
          (ipv4_version == 4) && (ipv4_ihl == 5) &&
          (meta.ipv4_total_len >= IPV4_HEADER_BYTES + UDP_HEADER_BYTES) &&
          (meta.ipv4_total_len <= meta.payload_len) &&
          (meta.ipv4_dst_ip == FPGA_IP) &&
          (meta.ipv4_protocol == IPV4_PROTOCOL_UDP) &&
          ((meta.ipv4_flags_fragment & 0x3fff) == 0);
      bool udp_valid =
          (meta.udp_len >= UDP_HEADER_BYTES) &&
          (meta.udp_len <= meta.ipv4_total_len - IPV4_HEADER_BYTES);
      if (ipv4_valid && udp_valid && meta.udp_dst_port == UDP_FPGA_PORT) {
        udp_reply_pending = true;
        udp_requester_mac_pending = meta.src_mac;
        udp_requester_ip_pending = meta.ipv4_src_ip;
        udp_requester_port_pending = meta.udp_src_port;
      }
    }
  }

  rx_accept_toggle = rx_accept;
}

// Choose the next TX producer. ARP replies have priority over ACKs and beacons.
// The returned request only carries metadata; payload bytes are prepared later.
static TxRequest tx_request_arbiter_step(
    bool arp_reply_pending, ap_uint<48> arp_requester_mac_pending,
    ap_uint<32> arp_requester_ip_pending, bool ack_pending,
    ap_uint<48> ack_dst_pending, bool udp_reply_pending,
    ap_uint<48> udp_requester_mac_pending, ap_uint<32> udp_requester_ip_pending,
    ap_uint<16> udp_requester_port_pending, bool beacon_pending) {
#pragma HLS INLINE
  TxRequest request;
  request.valid =
      arp_reply_pending || ack_pending || udp_reply_pending || beacon_pending;
  request.header.dst_mac = 0;
  request.header.src_mac = FPGA_MAC;
  request.header.ethertype = CUSTOM_ETHERTYPE;
  request.payload_len = 0;
  request.request_kind = TX_REQ_NONE;
  request.arp_requester_mac = 0;
  request.arp_requester_ip = 0;
  request.udp_requester_ip = 0;
  request.udp_requester_port = 0;

  if (arp_reply_pending) {
    request.header.dst_mac = arp_requester_mac_pending;
    request.header.ethertype = ARP_ETHERTYPE;
    request.payload_len = ARP_PAYLOAD_BYTES;
    request.request_kind = TX_REQ_ARP_REPLY;
    request.arp_requester_mac = arp_requester_mac_pending;
    request.arp_requester_ip = arp_requester_ip_pending;
  } else if (ack_pending) {
    request.header.dst_mac = ack_dst_pending;
    request.payload_len = 8;
    request.request_kind = TX_REQ_ACK;
  } else if (udp_reply_pending) {
    request.header.dst_mac = udp_requester_mac_pending;
    request.header.ethertype = IPV4_ETHERTYPE;
    request.payload_len = UDP_REPLY_PAYLOAD_BYTES;
    request.request_kind = TX_REQ_UDP_REPLY;
    request.udp_requester_ip = udp_requester_ip_pending;
    request.udp_requester_port = udp_requester_port_pending;
  } else if (beacon_pending) {
    request.header.dst_mac = BROADCAST_MAC;
    request.payload_len = 11;
    request.request_kind = TX_REQ_BEACON;
  }

  return request;
}

// TX integration step for the top-level endpoint.
// It creates periodic beacon requests, accepts pending ACK requests from RX,
// prepares the selected payload in BRAM, and feeds the generic framer.
static void ethernet_tx_step(
    bool &ack_pending, ap_uint<48> ack_dst_pending, bool &arp_reply_pending,
    ap_uint<48> arp_requester_mac_pending, ap_uint<32> arp_requester_ip_pending,
    bool &udp_reply_pending, ap_uint<48> udp_requester_mac_pending,
    ap_uint<32> udp_requester_ip_pending,
    ap_uint<16> udp_requester_port_pending, ap_uint<1> &eth_tx_en,
    ap_uint<4> &eth_txd, ap_uint<1> &tx_frame_toggle, ap_uint<1> &tx_active) {
  static ap_uint<8> tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT];
#pragma HLS BIND_STORAGE variable = tx_payload_buf type = ram_t2p impl = bram
  static ap_uint<32> beacon_count = BEACON_INTERVAL_CYCLES - 4;
  static bool beacon_pending = false;
  static bool framer_idle_last = true;
  static bool preparing_payload = false;
  static TxRequestKind prepare_kind = TX_REQ_NONE;
  static ap_uint<6> prepare_index = 0;
  static ap_uint<48> prepare_arp_requester_mac = 0;
  static ap_uint<32> prepare_arp_requester_ip = 0;
  static ap_uint<32> prepare_udp_requester_ip = 0;
  static ap_uint<16> prepare_udp_requester_port = 0;
  static EthHeader pending_header = {0, FPGA_MAC, CUSTOM_ETHERTYPE};
  static ap_uint<11> pending_payload_len = 0;

  // Convert the free-running counter into a sticky beacon request.
  if (beacon_count == BEACON_INTERVAL_CYCLES - 1) {
    beacon_count = 0;
    beacon_pending = true;
  } else {
    beacon_count++;
  }

  bool tx_idle = false;
  bool start_request = false;
  EthHeader start_header = pending_header;
  ap_uint<11> start_payload_len = 0;

  if (!preparing_payload && framer_idle_last) {
    // Only choose a new request when no payload preparation is in progress
    // and the framer was idle on the previous cycle.
    TxRequest request = tx_request_arbiter_step(
        arp_reply_pending, arp_requester_mac_pending, arp_requester_ip_pending,
        ack_pending, ack_dst_pending, udp_reply_pending,
        udp_requester_mac_pending, udp_requester_ip_pending,
        udp_requester_port_pending, beacon_pending);
    if (request.valid) {
      preparing_payload = true;
      prepare_kind = request.request_kind;
      prepare_index = 0;
      prepare_arp_requester_mac = request.arp_requester_mac;
      prepare_arp_requester_ip = request.arp_requester_ip;
      prepare_udp_requester_ip = request.udp_requester_ip;
      prepare_udp_requester_port = request.udp_requester_port;
      pending_header = request.header;
      pending_payload_len = request.payload_len;

      // The request is now owned by the TX path, even though its payload
      // will take several cycles to write into BRAM.
      if (request.request_kind == TX_REQ_ARP_REPLY) {
        arp_reply_pending = false;
      } else if (request.request_kind == TX_REQ_ACK) {
        ack_pending = false;
      } else if (request.request_kind == TX_REQ_UDP_REPLY) {
        udp_reply_pending = false;
      } else if (request.request_kind == TX_REQ_BEACON) {
        beacon_pending = false;
      }
    }
  }

  if (preparing_payload) {
    // Write one fixed-payload byte per cycle; launch the frame after the
    // last byte is present in BRAM.
    bool prepare_done = false;
    prepare_payload_step(tx_payload_buf, prepare_kind, prepare_index,
                         prepare_arp_requester_mac, prepare_arp_requester_ip,
                         prepare_udp_requester_ip, prepare_udp_requester_port,
                         prepare_done);
    if (prepare_done) {
      start_header = pending_header;
      start_payload_len = pending_payload_len;
      start_request = true;
      preparing_payload = false;
      prepare_kind = TX_REQ_NONE;
    } else {
      prepare_index++;
    }
  } else if (framer_idle_last) {
    // Keep the reusable payload RAM inferred as a full 8-bit byte store.
    tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT - 1] = 0x80;
  }

  ethernet_tx_framer_step(start_request, start_header, start_payload_len,
                          tx_payload_buf, eth_tx_en, eth_txd, tx_frame_toggle,
                          tx_active, tx_idle);
  framer_idle_last = tx_idle;
}

// External Vitis HLS top level.
// hls_top.v instantiates this block directly on the 25 MHz Ethernet clock.
// The function is ap_ctrl_none and pipelined with II=1 so it behaves like
// always-running hardware rather than a call/return accelerator.
extern "C" void ethernet_l2_endpoint_hls(
    ap_uint<1> eth_rx_dv, ap_uint<4> eth_rxd, ap_uint<1> eth_rxerr,
    ap_uint<1> &eth_tx_en, ap_uint<4> &eth_txd, ap_uint<1> &rx_accept_toggle,
    ap_uint<1> &tx_frame_toggle, ap_uint<1> &rx_active, ap_uint<1> &tx_active) {
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

  static bool ack_pending = false;
  static ap_uint<48> ack_dst_pending = 0;
  static bool arp_reply_pending = false;
  static ap_uint<48> arp_requester_mac_pending = 0;
  static ap_uint<32> arp_requester_ip_pending = 0;
  static bool udp_reply_pending = false;
  static ap_uint<48> udp_requester_mac_pending = 0;
  static ap_uint<32> udp_requester_ip_pending = 0;
  static ap_uint<16> udp_requester_port_pending = 0;

  ethernet_rx_step(eth_rx_dv, eth_rxd, eth_rxerr, ack_pending, ack_dst_pending,
                   arp_reply_pending, arp_requester_mac_pending,
                   arp_requester_ip_pending, udp_reply_pending,
                   udp_requester_mac_pending, udp_requester_ip_pending,
                   udp_requester_port_pending, rx_accept_toggle, rx_active);

  ethernet_tx_step(ack_pending, ack_dst_pending, arp_reply_pending,
                   arp_requester_mac_pending, arp_requester_ip_pending,
                   udp_reply_pending, udp_requester_mac_pending,
                   udp_requester_ip_pending, udp_requester_port_pending,
                   eth_tx_en, eth_txd, tx_frame_toggle, tx_active);
}
