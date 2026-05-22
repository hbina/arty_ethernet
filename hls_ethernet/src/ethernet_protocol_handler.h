#ifndef ETHERNET_PROTOCOL_HANDLER_H
#define ETHERNET_PROTOCOL_HANDLER_H

#include "ap_int.h"
#include "arp.h"
#include "ethernet_constants.h"
#include "ethernet_packet_queue.h"
#include "ethernet_periodic_timers.h"
#include "ethernet_tx_payloads.h"

enum ProtocolState {
  PROTO_IDLE = 0,
  PROTO_PARSE_ARP = 1,
  PROTO_PREPARE_TX = 2,
  PROTO_PARSE_IEX = 3,
  PROTO_CALC_VWAP = 4
};

static bool ethernet_dst_is_multicast(ap_uint<48> mac) {
#pragma HLS INLINE
  return (mac.range(47, 40) & 1) != 0;
}

static void start_protocol_tx_request(
    const ProtocolTxRequest &request,
    TxPacketQueue &tx_queue,
    bool &preparing_payload,
    ProtocolTxRequest &prepare_request,
    ap_uint<11> &prepare_index,
    ap_uint<TX_PACKET_INDEX_BITS> &prepare_tx_idx) {
#pragma HLS INLINE
  if (!request.valid) {
    return;
  }

  if (!packet_queue_write_slot_available(tx_queue)) {
    packet_queue_drop_write(tx_queue);
  } else {
    preparing_payload = true;
    prepare_request = request;
    prepare_index = 0;
    prepare_tx_idx = packet_queue_write_slot(tx_queue);
  }
}

// Protocol/application handling consumes one saved RX packet at a time and
// builds complete response packets into the TX ring.
static void
protocol_queue_step(RxPacketQueue &rx_queue, TxPacketQueue &tx_queue) {
#pragma HLS INLINE
  static ProtocolState protocol_state = PROTO_IDLE;
  static bool preparing_payload = false;
  static ProtocolTxRequest prepare_request = {};
  static ap_uint<11> prepare_index = 0;
  static ap_uint<TX_PACKET_INDEX_BITS> prepare_tx_idx = 0;
  static ap_uint<32> rx_protocol_drop_count = 0;
  static ap_uint<32> rx_packet_count = 0;
  static ap_uint<32> arp_reply_count = 0;
  static ap_uint<32> iex_aapl_match_count = 0;
  static ap_uint<32> iex_symbol_match_counts[20] = {0};
#pragma HLS ARRAY_PARTITION variable = iex_symbol_match_counts complete
  static ap_uint<64> committed_best_bid_price[20] = {0};
#pragma HLS ARRAY_PARTITION variable = committed_best_bid_price complete
  static ap_uint<32> committed_best_bid_size[20] = {0};
#pragma HLS ARRAY_PARTITION variable = committed_best_bid_size complete
  static ap_uint<64> committed_best_offer_price[20] = {0};
#pragma HLS ARRAY_PARTITION variable = committed_best_offer_price complete
  static ap_uint<32> committed_best_offer_size[20] = {0};
#pragma HLS ARRAY_PARTITION variable = committed_best_offer_size complete
  static ap_uint<128> committed_accum_value[20] = {0};
#pragma HLS ARRAY_PARTITION variable = committed_accum_value complete
  static ap_uint<64> committed_accum_volume[20] = {0};
#pragma HLS ARRAY_PARTITION variable = committed_accum_volume complete
  static ap_uint<64> committed_vwap[20] = {0};
#pragma HLS ARRAY_PARTITION variable = committed_vwap complete
  static ap_uint<32> uptime_beacon_count = 0;
  static ap_uint<RX_PACKET_INDEX_BITS> parse_rx_idx = 0;
  static ap_uint<5> parse_index = 0;
  static ap_uint<16> parse_arp_hw_type = 0;
  static ap_uint<16> parse_arp_proto_type = 0;
  static ap_uint<8> parse_arp_hw_len = 0;
  static ap_uint<8> parse_arp_proto_len = 0;
  static ap_uint<16> parse_arp_opcode = 0;
  static ap_uint<48> parse_arp_sender_mac = 0;
  static ap_uint<32> parse_arp_sender_ip = 0;
  static ap_uint<32> parse_arp_target_ip = 0;

  static ap_uint<4> iex_substate = 0;
  static ap_uint<11> iex_msg_offset = 0;
  static ap_uint<16> iex_msg_len = 0;
  static ap_uint<11> iex_byte_idx = 0;
  static ap_uint<16> iex_msgs_left = 0;
  static ap_uint<8> iex_msg_type = 0;
  static ap_uint<64> iex_price = 0;
  static ap_uint<32> iex_size = 0;
  static ap_uint<8> iex_symbol[8] = {0};
#pragma HLS ARRAY_PARTITION variable = iex_symbol complete

  static ap_uint<5> div_symbol_idx = 0;
  static ap_uint<8> div_bit_idx = 0;
  static ap_uint<128> div_rem = 0;
  static ap_uint<128> div_quot = 0;
  static ap_uint<64> div_denom = 0;
  static ap_uint<128> div_num = 0;

  PeriodicTxTicks tx_ticks = periodic_tx_timer_step();

  if (!preparing_payload && protocol_state == PROTO_IDLE) {
    ap_uint<RX_PACKET_INDEX_BITS> read_idx = packet_queue_read_slot(rx_queue);
    unsigned read_idx_int = read_idx;

    if (packet_queue_read_slot_valid(rx_queue)) {
      EthHeader header = rx_queue.meta[read_idx_int].header;
      ap_uint<11> payload_len = rx_queue.meta[read_idx_int].payload_len;
      bool iex_deep = rx_queue.meta[read_idx_int].iex_deep;
      bool dest_ok =
          (header.dst_mac == FPGA_MAC) || (header.dst_mac == BROADCAST_MAC);
      bool market_data_dest_ok =
          dest_ok || ethernet_dst_is_multicast(header.dst_mac);
      rx_packet_count++;

      if (rx_queue.meta[read_idx_int].truncated) {
        rx_protocol_drop_count++;
        packet_queue_consume_read_slot(rx_queue);
      } else if (
          dest_ok && header.ethertype == ARP_ETHERTYPE &&
          payload_len >= ARP_PAYLOAD_BYTES) {
        parse_rx_idx = read_idx;
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
      } else if (market_data_dest_ok && header.ethertype == IPV4_ETHERTYPE) {
        if (iex_deep) {
          ap_uint<32> frame_match_sum = 0;
          for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
            ap_uint<8> cnt = rx_queue.meta[read_idx_int].iex_symbol_counts[k];
            iex_symbol_match_counts[k] += cnt;
            frame_match_sum += cnt;
          }
          iex_aapl_match_count += frame_match_sum;

          parse_rx_idx = read_idx;
          iex_substate = 0;
          protocol_state = PROTO_PARSE_IEX;
        } else {
          rx_protocol_drop_count++;
          packet_queue_consume_read_slot(rx_queue);
        }
      } else {
        rx_protocol_drop_count++;
        packet_queue_consume_read_slot(rx_queue);
      }
    } else if (tx_ticks.gratuitous_arp_tick) {
      ProtocolTxRequest request = protocol_tx_gratuitous_arp_request();
      start_protocol_tx_request(
          request,
          tx_queue,
          preparing_payload,
          prepare_request,
          prepare_index,
          prepare_tx_idx);
    } else if (tx_ticks.beacon_tick) {
      protocol_state = PROTO_CALC_VWAP;
      div_symbol_idx = 0;
      div_bit_idx = 0;
      div_rem = 0;
      div_quot = 0;
      div_denom = committed_accum_volume[0];
      div_num = committed_accum_value[0];
    }
  } else if (!preparing_payload && protocol_state == PROTO_PARSE_ARP) {
    unsigned parse_rx_idx_int = parse_rx_idx;
    ap_uint<8> data_byte = rx_queue.bytes[parse_rx_idx_int][parse_index];

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
        ProtocolTxRequest request;
        request.valid = true;
        request.header.dst_mac = parse_arp_sender_mac;
        request.header.src_mac = FPGA_MAC;
        request.header.ethertype = ARP_ETHERTYPE;
        request.len = protocol_tx_frame_body_len(PROTO_TX_ARP_REPLY);
        request.kind = PROTO_TX_ARP_REPLY;
        request.arp_requester_mac = parse_arp_sender_mac;
        request.arp_requester_ip = parse_arp_sender_ip;
        request.rx_packet_count = 0;
        request.rx_queue_drop_count = 0;
        request.rx_protocol_drop_count = 0;
        request.tx_drop_count = 0;
        request.arp_reply_count = 0;
        request.udp_reply_count = 0;
        request.uptime_beacon_count = 0;
        arp_reply_count++;
        start_protocol_tx_request(
            request,
            tx_queue,
            preparing_payload,
            prepare_request,
            prepare_index,
            prepare_tx_idx);
      } else {
        rx_protocol_drop_count++;
      }
      packet_queue_consume_read_slot(rx_queue);
      protocol_state = PROTO_IDLE;
    } else {
      parse_index++;
    }
  } else if (!preparing_payload && protocol_state == PROTO_PARSE_IEX) {
    unsigned parse_rx_idx_int = parse_rx_idx;

    if (iex_substate == 0) {
      ap_uint<4> ihl = rx_queue.bytes[parse_rx_idx_int][0].range(3, 0);
      iex_msg_offset = (ap_uint<11>(ihl) << 2) + UDP_HEADER_BYTES;
      iex_substate = 1;
    } else if (iex_substate == 1) {
      iex_msgs_left.range(7, 0) =
          rx_queue.bytes[parse_rx_idx_int]
                        [iex_msg_offset + IEX_TP_MESSAGE_COUNT_OFFSET];
      iex_substate = 2;
    } else if (iex_substate == 2) {
      iex_msgs_left.range(15, 8) =
          rx_queue.bytes[parse_rx_idx_int]
                        [iex_msg_offset + IEX_TP_MESSAGE_COUNT_OFFSET + 1];
      iex_msg_offset += IEX_TP_HEADER_BYTES;
      if (iex_msgs_left == 0) {
        packet_queue_consume_read_slot(rx_queue);
        protocol_state = PROTO_IDLE;
      } else {
        iex_substate = 3;
      }
    } else if (iex_substate == 3) {
      iex_msg_len.range(7, 0) =
          rx_queue.bytes[parse_rx_idx_int][iex_msg_offset];
      iex_substate = 4;
    } else if (iex_substate == 4) {
      iex_msg_len.range(15, 8) =
          rx_queue.bytes[parse_rx_idx_int][iex_msg_offset + 1];
      iex_byte_idx = 0;
      iex_size = 0;
      iex_price = 0;
      for (int i = 0; i < IEX_SYMBOL_BYTES; ++i) {
#pragma HLS UNROLL
        iex_symbol[i] = 0;
      }
      iex_substate = 5;
    } else if (iex_substate == 5) {
      if (iex_byte_idx < iex_msg_len) {
        ap_uint<8> data_byte =
            rx_queue.bytes[parse_rx_idx_int]
                          [iex_msg_offset + IEX_MESSAGE_BLOCK_HEADER_BYTES +
                           iex_byte_idx];
        if (iex_byte_idx == 0) {
          iex_msg_type = data_byte;
        } else if (
            iex_byte_idx >= IEX_SYMBOL_OFFSET &&
            iex_byte_idx < IEX_SYMBOL_OFFSET + IEX_SYMBOL_BYTES) {
          iex_symbol[iex_byte_idx - IEX_SYMBOL_OFFSET] = data_byte;
        } else if (iex_byte_idx >= 18 && iex_byte_idx < 22) {
          iex_size.range((iex_byte_idx - 18) * 8 + 7, (iex_byte_idx - 18) * 8) =
              data_byte;
        } else if (iex_byte_idx >= 22 && iex_byte_idx < 30) {
          iex_price.range(
              (iex_byte_idx - 22) * 8 + 7,
              (iex_byte_idx - 22) * 8) = data_byte;
        }
      }

      if (iex_byte_idx == iex_msg_len) {
        if (iex_msg_type == IEX_MSG_TYPE_PRICE_BUY ||
            iex_msg_type == IEX_MSG_TYPE_PRICE_SELL ||
            iex_msg_type == IEX_MSG_TYPE_TRADE_REPORT) {
          for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
            bool match = true;
            for (int i = 0; i < IEX_SYMBOL_BYTES; ++i) {
#pragma HLS UNROLL
              match = match && (iex_symbol[i] == TOP_20_SYMBOLS[k][i]);
            }
            if (match) {
              if (iex_msg_type == IEX_MSG_TYPE_PRICE_BUY) {
                if (iex_size == 0 && iex_price == committed_best_bid_price[k]) {
                  committed_best_bid_price[k] = 0;
                  committed_best_bid_size[k] = 0;
                } else if (
                    iex_size > 0 && (iex_price >= committed_best_bid_price[k] ||
                                     committed_best_bid_size[k] == 0)) {
                  committed_best_bid_price[k] = iex_price;
                  committed_best_bid_size[k] = iex_size;
                }
              } else if (iex_msg_type == IEX_MSG_TYPE_PRICE_SELL) {
                if (iex_size == 0 &&
                    iex_price == committed_best_offer_price[k]) {
                  committed_best_offer_price[k] = 0;
                  committed_best_offer_size[k] = 0;
                } else if (
                    iex_size > 0 &&
                    (iex_price <= committed_best_offer_price[k] ||
                     committed_best_offer_size[k] == 0)) {
                  committed_best_offer_price[k] = iex_price;
                  committed_best_offer_size[k] = iex_size;
                }
              } else if (iex_msg_type == IEX_MSG_TYPE_TRADE_REPORT) {
                committed_accum_volume[k] += iex_size;
                committed_accum_value[k] += (ap_uint<128>)iex_price * iex_size;
              }
            }
          }
        }

        iex_msg_offset += IEX_MESSAGE_BLOCK_HEADER_BYTES + iex_msg_len;
        iex_msgs_left--;
        if (iex_msgs_left == 0) {
          packet_queue_consume_read_slot(rx_queue);
          protocol_state = PROTO_IDLE;
        } else {
          iex_substate = 3;
        }
      } else {
        iex_byte_idx++;
      }
    }
  } else if (!preparing_payload && protocol_state == PROTO_CALC_VWAP) {
    if (div_denom == 0) {
      committed_vwap[div_symbol_idx] = 0;
      div_symbol_idx++;
      if (div_symbol_idx == 20) {
        ProtocolTxRequest request = protocol_tx_beacon_request();
        request.rx_packet_count = rx_packet_count;
        request.rx_queue_drop_count = packet_queue_drop_count(rx_queue);
        request.rx_protocol_drop_count = rx_protocol_drop_count;
        request.tx_drop_count = packet_queue_drop_count(tx_queue);
        request.arp_reply_count = arp_reply_count;
        request.udp_reply_count = iex_aapl_match_count;
        request.uptime_beacon_count = uptime_beacon_count;
        for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
          request.iex_symbol_match_counts[k] = iex_symbol_match_counts[k];
          request.iex_symbol_vwap[k] = committed_vwap[k].range(31, 0);
          request.iex_symbol_best_bid_price[k] =
              committed_best_bid_price[k].range(31, 0);
          request.iex_symbol_best_bid_size[k] = committed_best_bid_size[k];
          request.iex_symbol_best_offer_price[k] =
              committed_best_offer_price[k].range(31, 0);
          request.iex_symbol_best_offer_size[k] = committed_best_offer_size[k];
        }
        start_protocol_tx_request(
            request,
            tx_queue,
            preparing_payload,
            prepare_request,
            prepare_index,
            prepare_tx_idx);
        if (preparing_payload) {
          uptime_beacon_count++;
        }
        protocol_state = PROTO_IDLE;
      } else {
        div_bit_idx = 0;
        div_rem = 0;
        div_quot = 0;
        div_denom = committed_accum_volume[div_symbol_idx];
        div_num = committed_accum_value[div_symbol_idx];
      }
    } else {
      div_rem =
          (div_rem << 1) | div_num.range(127 - div_bit_idx, 127 - div_bit_idx);
      if (div_rem >= (ap_uint<128>)div_denom) {
        div_rem -= div_denom;
        div_quot.range(127 - div_bit_idx, 127 - div_bit_idx) = 1;
      }
      div_bit_idx++;
      if (div_bit_idx == 128) {
        committed_vwap[div_symbol_idx] = div_quot.range(63, 0);
        div_symbol_idx++;
        if (div_symbol_idx == 20) {
          ProtocolTxRequest request = protocol_tx_beacon_request();
          request.rx_packet_count = rx_packet_count;
          request.rx_queue_drop_count = packet_queue_drop_count(rx_queue);
          request.rx_protocol_drop_count = rx_protocol_drop_count;
          request.tx_drop_count = packet_queue_drop_count(tx_queue);
          request.arp_reply_count = arp_reply_count;
          request.udp_reply_count = iex_aapl_match_count;
          request.uptime_beacon_count = uptime_beacon_count;
          for (int k = 0; k < 20; ++k) {
#pragma HLS UNROLL
            request.iex_symbol_match_counts[k] = iex_symbol_match_counts[k];
            request.iex_symbol_vwap[k] = committed_vwap[k].range(31, 0);
            request.iex_symbol_best_bid_price[k] =
                committed_best_bid_price[k].range(31, 0);
            request.iex_symbol_best_bid_size[k] = committed_best_bid_size[k];
            request.iex_symbol_best_offer_price[k] =
                committed_best_offer_price[k].range(31, 0);
            request.iex_symbol_best_offer_size[k] =
                committed_best_offer_size[k];
          }
          start_protocol_tx_request(
              request,
              tx_queue,
              preparing_payload,
              prepare_request,
              prepare_index,
              prepare_tx_idx);
          if (preparing_payload) {
            uptime_beacon_count++;
          }
          protocol_state = PROTO_IDLE;
        } else {
          div_bit_idx = 0;
          div_rem = 0;
          div_quot = 0;
          div_denom = committed_accum_volume[div_symbol_idx];
          div_num = committed_accum_value[div_symbol_idx];
        }
      }
    }
  }

  if (preparing_payload) {
    bool prepare_done = false;
    unsigned prepare_tx_idx_int = prepare_tx_idx;
    prepare_tx_slot_frame_body_step(
        tx_queue.bytes[prepare_tx_idx_int],
        prepare_request,
        prepare_index,
        prepare_done);
    if (prepare_done) {
      TxPacketMeta tx_meta;
      tx_meta.frame_body_len = prepare_request.len;
      packet_queue_publish_write_slot(tx_queue, tx_meta);
      preparing_payload = false;
      prepare_request.valid = false;
      prepare_request.kind = PROTO_TX_NONE;
    } else {
      prepare_index++;
    }
  }
}

#endif
