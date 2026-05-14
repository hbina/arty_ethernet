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

enum ProtocolState {
  PROTO_IDLE = 0,
  PROTO_PARSE_ARP = 1,
  PROTO_PARSE_IPV4_UDP = 2,
  PROTO_PREPARE_TX = 3
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

static const int RX_PACKET_SLOTS = 8;
static const int TX_PACKET_SLOTS = 4;

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

static ap_uint<11> tx_request_payload_len(TxRequestKind request_kind) {
#pragma HLS INLINE
  if (request_kind == TX_REQ_ACK) {
    return 8;
  }
  if (request_kind == TX_REQ_BEACON) {
    return 11;
  }
  if (request_kind == TX_REQ_ARP_REPLY) {
    return ARP_PAYLOAD_BYTES;
  }
  if (request_kind == TX_REQ_UDP_REPLY) {
    return UDP_REPLY_PAYLOAD_BYTES;
  }
  return 0;
}

static EthHeader tx_request_header(const TxRequest &request) {
#pragma HLS INLINE
  EthHeader header;
  header.src_mac = FPGA_MAC;
  header.dst_mac = BROADCAST_MAC;
  header.ethertype = CUSTOM_ETHERTYPE;

  if (request.request_kind == TX_REQ_ACK) {
    header.dst_mac = request.header.dst_mac;
  } else if (request.request_kind == TX_REQ_ARP_REPLY) {
    header.dst_mac = request.arp_requester_mac;
    header.ethertype = ARP_ETHERTYPE;
  } else if (request.request_kind == TX_REQ_UDP_REPLY) {
    header.dst_mac = request.header.dst_mac;
    header.ethertype = IPV4_ETHERTYPE;
  }

  return header;
}

// Write one byte of the currently selected fixed response into a TX slot.
// This intentionally writes one byte per top-level call so the endpoint can
// remain a one-cycle initiation-interval pipeline.
static void prepare_tx_slot_payload_step(
    ap_uint<8> tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
    const TxRequest &request, ap_uint<6> payload_index, bool &done) {
#pragma HLS INLINE
  TxRequestKind request_kind = request.request_kind;
  ap_uint<11> payload_len = tx_request_payload_len(request_kind);
  ap_uint<8> payload_byte = beacon_payload_literal_byte(payload_index);

  if (request_kind == TX_REQ_ACK) {
    payload_byte = ack_payload_literal_byte(payload_index);
  } else if (request_kind == TX_REQ_ARP_REPLY) {
    payload_byte = arp_reply_payload_byte(
        payload_index, request.arp_requester_mac, request.arp_requester_ip);
  } else if (request_kind == TX_REQ_UDP_REPLY) {
    payload_byte = udp_reply_payload_byte(
        payload_index, request.udp_requester_ip, request.udp_requester_port);
  }

  tx_payload_buf[payload_index] = payload_byte;
  done = (payload_len == 0) || (payload_index == payload_len - 1);
}

static TxRequest beacon_request() {
#pragma HLS INLINE
  TxRequest request;
  request.valid = true;
  request.header.dst_mac = BROADCAST_MAC;
  request.header.src_mac = FPGA_MAC;
  request.header.ethertype = CUSTOM_ETHERTYPE;
  request.payload_len = tx_request_payload_len(TX_REQ_BEACON);
  request.request_kind = TX_REQ_BEACON;
  request.arp_requester_mac = 0;
  request.arp_requester_ip = 0;
  request.udp_requester_ip = 0;
  request.udp_requester_port = 0;
  return request;
}

// RX integration step for the top-level endpoint. The MII capture path only
// publishes complete Ethernet packets into an 8-slot ring.
static void ethernet_rx_queue_step(
    ap_uint<1> eth_rx_dv, ap_uint<4> eth_rxd, ap_uint<1> eth_rxerr,
    EthHeader rx_headers[RX_PACKET_SLOTS],
    ap_uint<11> rx_payload_lens[RX_PACKET_SLOTS],
    bool rx_valid[RX_PACKET_SLOTS], bool rx_truncated[RX_PACKET_SLOTS],
    ap_uint<8> rx_payloads[RX_PACKET_SLOTS][MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<3> &rx_write_idx, ap_uint<32> &rx_drop_count,
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
  mii_rx_byte_assembler_step(eth_rx_dv, eth_rxd, frame_start, byte_valid,
                             data_byte, frame_end);

  if (frame_start) {
    rx_drop_current = rx_valid[write_idx_int];
    if (rx_drop_current) {
      rx_drop_count++;
    }
  }

  if (rx_drop_current) {
    ethernet_rx_parser_step(frame_start, byte_valid, data_byte, frame_end,
                            eth_rxerr, rx_drop_payload, meta);
  } else {
    ethernet_rx_parser_step(frame_start, byte_valid, data_byte, frame_end,
                            eth_rxerr, rx_payloads[write_idx_int], meta);
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

static void start_protocol_tx_request(
    const TxRequest &request, bool tx_valid[TX_PACKET_SLOTS],
    ap_uint<2> tx_write_idx, bool &preparing_payload,
    TxRequest &prepare_request, ap_uint<6> &prepare_index,
    ap_uint<2> &prepare_tx_idx, ap_uint<32> &tx_drop_count) {
#pragma HLS INLINE
  unsigned write_idx_int = tx_write_idx;

  if (!request.valid) {
    return;
  }

  if (tx_valid[write_idx_int]) {
    tx_drop_count++;
  } else {
    preparing_payload = true;
    prepare_request = request;
    prepare_index = 0;
    prepare_tx_idx = tx_write_idx;
  }
}

// Protocol/application handling consumes one saved RX packet at a time and
// builds complete response packets into the TX ring.
static void protocol_queue_step(
    EthHeader rx_headers[RX_PACKET_SLOTS],
    ap_uint<11> rx_payload_lens[RX_PACKET_SLOTS],
    bool rx_valid[RX_PACKET_SLOTS], bool rx_truncated[RX_PACKET_SLOTS],
    ap_uint<8> rx_payloads[RX_PACKET_SLOTS][MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<3> &rx_read_idx, EthHeader tx_headers[TX_PACKET_SLOTS],
    ap_uint<11> tx_payload_lens[TX_PACKET_SLOTS],
    bool tx_valid[TX_PACKET_SLOTS],
    ap_uint<8> tx_payloads[TX_PACKET_SLOTS][MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<2> &tx_write_idx, ap_uint<32> &tx_drop_count,
    ap_uint<1> &rx_accept_toggle) {
#pragma HLS INLINE
  static ap_uint<32> beacon_count = BEACON_INTERVAL_CYCLES - 4;
  static ProtocolState protocol_state = PROTO_IDLE;
  static bool preparing_payload = false;
  static TxRequest prepare_request = {
      false, {0, FPGA_MAC, CUSTOM_ETHERTYPE}, 0, TX_REQ_NONE, 0, 0, 0, 0};
  static ap_uint<6> prepare_index = 0;
  static ap_uint<2> prepare_tx_idx = 0;
  static ap_uint<32> rx_protocol_drop_count = 0;
  static bool rx_accept = false;
  static ap_uint<3> parse_rx_idx = 0;
  static EthHeader parse_header = {0, 0, 0};
  static ap_uint<11> parse_payload_len = 0;
  static ap_uint<5> parse_index = 0;
  static ap_uint<16> parse_arp_hw_type = 0;
  static ap_uint<16> parse_arp_proto_type = 0;
  static ap_uint<8> parse_arp_hw_len = 0;
  static ap_uint<8> parse_arp_proto_len = 0;
  static ap_uint<16> parse_arp_opcode = 0;
  static ap_uint<48> parse_arp_sender_mac = 0;
  static ap_uint<32> parse_arp_sender_ip = 0;
  static ap_uint<32> parse_arp_target_ip = 0;
  static ap_uint<8> parse_ipv4_version_ihl = 0;
  static ap_uint<16> parse_ipv4_total_len = 0;
  static ap_uint<16> parse_ipv4_flags_fragment = 0;
  static ap_uint<8> parse_ipv4_protocol = 0;
  static ap_uint<32> parse_ipv4_src_ip = 0;
  static ap_uint<32> parse_ipv4_dst_ip = 0;
  static ap_uint<16> parse_udp_src_port = 0;
  static ap_uint<16> parse_udp_dst_port = 0;
  static ap_uint<16> parse_udp_len = 0;

  bool beacon_tick = false;
  if (beacon_count == BEACON_INTERVAL_CYCLES - 1) {
    beacon_count = 0;
    beacon_tick = true;
  } else {
    beacon_count++;
  }

  if (!preparing_payload && protocol_state == PROTO_IDLE) {
    ap_uint<3> read_idx = rx_read_idx;
    unsigned read_idx_int = read_idx;

    if (rx_valid[read_idx_int]) {
      EthHeader header = rx_headers[read_idx_int];
      ap_uint<11> payload_len = rx_payload_lens[read_idx_int];
      bool dest_ok =
          (header.dst_mac == FPGA_MAC) || (header.dst_mac == BROADCAST_MAC);

      if (rx_truncated[read_idx_int]) {
        rx_protocol_drop_count++;
        rx_valid[read_idx_int] = false;
        rx_truncated[read_idx_int] = false;
        rx_read_idx = read_idx + 1;
      } else if (dest_ok && header.ethertype == CUSTOM_ETHERTYPE) {
        TxRequest request;
        request.valid = true;
        request.header = header;
        request.header.dst_mac = header.src_mac;
        request.payload_len = tx_request_payload_len(TX_REQ_ACK);
        request.request_kind = TX_REQ_ACK;
        request.arp_requester_mac = 0;
        request.arp_requester_ip = 0;
        request.udp_requester_ip = 0;
        request.udp_requester_port = 0;
        rx_accept = !rx_accept;
        rx_valid[read_idx_int] = false;
        rx_truncated[read_idx_int] = false;
        rx_read_idx = read_idx + 1;
        start_protocol_tx_request(request, tx_valid, tx_write_idx,
                                  preparing_payload, prepare_request,
                                  prepare_index, prepare_tx_idx, tx_drop_count);
      } else if (dest_ok && header.ethertype == ARP_ETHERTYPE &&
                 payload_len >= ARP_PAYLOAD_BYTES) {
        parse_rx_idx = read_idx;
        parse_header = header;
        parse_payload_len = payload_len;
        parse_index = 0;
        parse_arp_hw_type = 0;
        parse_arp_proto_type = 0;
        parse_arp_hw_len = 0;
        parse_arp_proto_len = 0;
        parse_arp_opcode = 0;
        parse_arp_sender_mac = 0;
        parse_arp_sender_ip = 0;
        parse_arp_target_ip = 0;
        protocol_state = PROTO_PARSE_ARP;
      } else if (dest_ok && header.ethertype == IPV4_ETHERTYPE &&
                 payload_len >= IPV4_HEADER_BYTES + UDP_HEADER_BYTES) {
        parse_rx_idx = read_idx;
        parse_header = header;
        parse_payload_len = payload_len;
        parse_index = 0;
        parse_ipv4_version_ihl = 0;
        parse_ipv4_total_len = 0;
        parse_ipv4_flags_fragment = 0;
        parse_ipv4_protocol = 0;
        parse_ipv4_src_ip = 0;
        parse_ipv4_dst_ip = 0;
        parse_udp_src_port = 0;
        parse_udp_dst_port = 0;
        parse_udp_len = 0;
        protocol_state = PROTO_PARSE_IPV4_UDP;
      } else {
        rx_valid[read_idx_int] = false;
        rx_truncated[read_idx_int] = false;
        rx_read_idx = read_idx + 1;
      }
    } else if (beacon_tick) {
      TxRequest request = beacon_request();
      start_protocol_tx_request(request, tx_valid, tx_write_idx,
                                preparing_payload, prepare_request,
                                prepare_index, prepare_tx_idx, tx_drop_count);
    }
  } else if (!preparing_payload && protocol_state == PROTO_PARSE_ARP) {
    unsigned parse_rx_idx_int = parse_rx_idx;
    ap_uint<8> data_byte = rx_payloads[parse_rx_idx_int][parse_index];

    switch ((unsigned)parse_index) {
    case 0:
      parse_arp_hw_type.range(15, 8) = data_byte;
      break;
    case 1:
      parse_arp_hw_type.range(7, 0) = data_byte;
      break;
    case 2:
      parse_arp_proto_type.range(15, 8) = data_byte;
      break;
    case 3:
      parse_arp_proto_type.range(7, 0) = data_byte;
      break;
    case 4:
      parse_arp_hw_len = data_byte;
      break;
    case 5:
      parse_arp_proto_len = data_byte;
      break;
    case 6:
      parse_arp_opcode.range(15, 8) = data_byte;
      break;
    case 7:
      parse_arp_opcode.range(7, 0) = data_byte;
      break;
    case 8:
      parse_arp_sender_mac.range(47, 40) = data_byte;
      break;
    case 9:
      parse_arp_sender_mac.range(39, 32) = data_byte;
      break;
    case 10:
      parse_arp_sender_mac.range(31, 24) = data_byte;
      break;
    case 11:
      parse_arp_sender_mac.range(23, 16) = data_byte;
      break;
    case 12:
      parse_arp_sender_mac.range(15, 8) = data_byte;
      break;
    case 13:
      parse_arp_sender_mac.range(7, 0) = data_byte;
      break;
    case 14:
      parse_arp_sender_ip.range(31, 24) = data_byte;
      break;
    case 15:
      parse_arp_sender_ip.range(23, 16) = data_byte;
      break;
    case 16:
      parse_arp_sender_ip.range(15, 8) = data_byte;
      break;
    case 17:
      parse_arp_sender_ip.range(7, 0) = data_byte;
      break;
    case 24:
      parse_arp_target_ip.range(31, 24) = data_byte;
      break;
    case 25:
      parse_arp_target_ip.range(23, 16) = data_byte;
      break;
    case 26:
      parse_arp_target_ip.range(15, 8) = data_byte;
      break;
    case 27:
      parse_arp_target_ip.range(7, 0) = data_byte;
      break;
    default:
      break;
    }

    if (parse_index == ARP_PAYLOAD_BYTES - 1) {
      bool arp_valid = (parse_arp_hw_type == ARP_HW_TYPE_ETHERNET) &&
                       (parse_arp_proto_type == ARP_PROTO_TYPE_IPV4) &&
                       (parse_arp_hw_len == ARP_HW_LEN_ETHERNET) &&
                       (parse_arp_proto_len == ARP_PROTO_LEN_IPV4) &&
                       (parse_arp_opcode == ARP_OPCODE_REQUEST) &&
                       (parse_arp_target_ip == FPGA_IP);
      if (arp_valid) {
        TxRequest request;
        request.valid = true;
        request.header = parse_header;
        request.payload_len = tx_request_payload_len(TX_REQ_ARP_REPLY);
        request.request_kind = TX_REQ_ARP_REPLY;
        request.arp_requester_mac = parse_arp_sender_mac;
        request.arp_requester_ip = parse_arp_sender_ip;
        request.udp_requester_ip = 0;
        request.udp_requester_port = 0;
        start_protocol_tx_request(request, tx_valid, tx_write_idx,
                                  preparing_payload, prepare_request,
                                  prepare_index, prepare_tx_idx, tx_drop_count);
      }
      rx_valid[parse_rx_idx_int] = false;
      rx_truncated[parse_rx_idx_int] = false;
      rx_read_idx = parse_rx_idx + 1;
      protocol_state = PROTO_IDLE;
    } else {
      parse_index++;
    }
  } else if (!preparing_payload && protocol_state == PROTO_PARSE_IPV4_UDP) {
    unsigned parse_rx_idx_int = parse_rx_idx;
    ap_uint<8> data_byte = rx_payloads[parse_rx_idx_int][parse_index];

    switch ((unsigned)parse_index) {
    case 0:
      parse_ipv4_version_ihl = data_byte;
      break;
    case 2:
      parse_ipv4_total_len.range(15, 8) = data_byte;
      break;
    case 3:
      parse_ipv4_total_len.range(7, 0) = data_byte;
      break;
    case 6:
      parse_ipv4_flags_fragment.range(15, 8) = data_byte;
      break;
    case 7:
      parse_ipv4_flags_fragment.range(7, 0) = data_byte;
      break;
    case 9:
      parse_ipv4_protocol = data_byte;
      break;
    case 12:
      parse_ipv4_src_ip.range(31, 24) = data_byte;
      break;
    case 13:
      parse_ipv4_src_ip.range(23, 16) = data_byte;
      break;
    case 14:
      parse_ipv4_src_ip.range(15, 8) = data_byte;
      break;
    case 15:
      parse_ipv4_src_ip.range(7, 0) = data_byte;
      break;
    case 16:
      parse_ipv4_dst_ip.range(31, 24) = data_byte;
      break;
    case 17:
      parse_ipv4_dst_ip.range(23, 16) = data_byte;
      break;
    case 18:
      parse_ipv4_dst_ip.range(15, 8) = data_byte;
      break;
    case 19:
      parse_ipv4_dst_ip.range(7, 0) = data_byte;
      break;
    case 20:
      parse_udp_src_port.range(15, 8) = data_byte;
      break;
    case 21:
      parse_udp_src_port.range(7, 0) = data_byte;
      break;
    case 22:
      parse_udp_dst_port.range(15, 8) = data_byte;
      break;
    case 23:
      parse_udp_dst_port.range(7, 0) = data_byte;
      break;
    case 24:
      parse_udp_len.range(15, 8) = data_byte;
      break;
    case 25:
      parse_udp_len.range(7, 0) = data_byte;
      break;
    default:
      break;
    }

    if (parse_index == IPV4_HEADER_BYTES + UDP_HEADER_BYTES - 1) {
      ap_uint<4> ipv4_version = parse_ipv4_version_ihl.range(7, 4);
      ap_uint<4> ipv4_ihl = parse_ipv4_version_ihl.range(3, 0);
      bool ipv4_valid =
          (ipv4_version == 4) && (ipv4_ihl == 5) &&
          (parse_ipv4_total_len >= IPV4_HEADER_BYTES + UDP_HEADER_BYTES) &&
          (parse_ipv4_total_len <= parse_payload_len) &&
          (parse_ipv4_dst_ip == FPGA_IP) &&
          (parse_ipv4_protocol == IPV4_PROTOCOL_UDP) &&
          ((parse_ipv4_flags_fragment & 0x3fff) == 0);
      bool udp_valid =
          (parse_udp_len >= UDP_HEADER_BYTES) &&
          (parse_udp_len <= parse_ipv4_total_len - IPV4_HEADER_BYTES) &&
          (parse_udp_dst_port == UDP_FPGA_PORT);
      if (ipv4_valid && udp_valid) {
        TxRequest request;
        request.valid = true;
        request.header = parse_header;
        request.header.dst_mac = parse_header.src_mac;
        request.payload_len = tx_request_payload_len(TX_REQ_UDP_REPLY);
        request.request_kind = TX_REQ_UDP_REPLY;
        request.arp_requester_mac = 0;
        request.arp_requester_ip = 0;
        request.udp_requester_ip = parse_ipv4_src_ip;
        request.udp_requester_port = parse_udp_src_port;
        start_protocol_tx_request(request, tx_valid, tx_write_idx,
                                  preparing_payload, prepare_request,
                                  prepare_index, prepare_tx_idx, tx_drop_count);
      }
      rx_valid[parse_rx_idx_int] = false;
      rx_truncated[parse_rx_idx_int] = false;
      rx_read_idx = parse_rx_idx + 1;
      protocol_state = PROTO_IDLE;
    } else {
      parse_index++;
    }
  }

  if (preparing_payload) {
    bool prepare_done = false;
    unsigned prepare_tx_idx_int = prepare_tx_idx;
    prepare_tx_slot_payload_step(tx_payloads[prepare_tx_idx_int],
                                 prepare_request, prepare_index, prepare_done);
    if (prepare_done) {
      tx_headers[prepare_tx_idx_int] = tx_request_header(prepare_request);
      tx_payload_lens[prepare_tx_idx_int] = prepare_request.payload_len;
      tx_valid[prepare_tx_idx_int] = true;
      tx_write_idx = prepare_tx_idx + 1;
      preparing_payload = false;
      prepare_request.valid = false;
      prepare_request.request_kind = TX_REQ_NONE;
    } else {
      prepare_index++;
    }
  }

  rx_accept_toggle = rx_accept;
}

// TX integration step drains complete packets from the TX ring and feeds the
// reusable Ethernet framer.
static void ethernet_tx_queue_step(
    EthHeader tx_headers[TX_PACKET_SLOTS],
    ap_uint<11> tx_payload_lens[TX_PACKET_SLOTS],
    bool tx_valid[TX_PACKET_SLOTS],
    ap_uint<8> tx_payloads[TX_PACKET_SLOTS][MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<2> &tx_read_idx, ap_uint<1> &eth_tx_en, ap_uint<4> &eth_txd,
    ap_uint<1> &tx_frame_toggle, ap_uint<1> &tx_active) {
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
  ethernet_tx_framer_step(start_request, start_header, start_payload_len,
                          tx_payloads[framer_slot_idx_int], eth_tx_en, eth_txd,
                          tx_frame_toggle, tx_active, tx_idle);

  if (framer_active && tx_idle) {
    tx_valid[framer_slot_idx_int] = false;
    tx_read_idx = framer_slot_idx + 1;
    framer_active = false;
  }
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

  ethernet_rx_queue_step(eth_rx_dv, eth_rxd, eth_rxerr, rx_headers,
                         rx_payload_lens, rx_valid, rx_truncated, rx_payloads,
                         rx_write_idx, rx_drop_count, rx_active);

  protocol_queue_step(rx_headers, rx_payload_lens, rx_valid, rx_truncated,
                      rx_payloads, rx_read_idx, tx_headers, tx_payload_lens,
                      tx_valid, tx_payloads, tx_write_idx, tx_drop_count,
                      rx_accept_toggle);

  ethernet_tx_queue_step(tx_headers, tx_payload_lens, tx_valid, tx_payloads,
                         tx_read_idx, eth_tx_en, eth_txd, tx_frame_toggle,
                         tx_active);
}
