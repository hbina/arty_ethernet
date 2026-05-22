#ifndef ETHERNET_CONSTANTS_H
#define ETHERNET_CONSTANTS_H

#include "ap_int.h"

static const ap_uint<48> FPGA_MAC = 0x020000000001ULL;
static const ap_uint<48> BROADCAST_MAC = 0xffffffffffffULL;
static const ap_uint<16> DIAGNOSTIC_BEACON_ETHERTYPE = 0x88B5;
static const ap_uint<16> ARP_ETHERTYPE = 0x0806;
static const ap_uint<16> IPV4_ETHERTYPE = 0x0800;
static const ap_uint<32> FPGA_IP = 0xC0A80164;
static const ap_uint<8> IPV4_HEADER_BYTES = 20;
static const ap_uint<8> IPV4_PROTOCOL_UDP = 17;
static const ap_uint<8> IPV4_DEFAULT_TTL = 64;
static const ap_uint<16> UDP_FPGA_PORT = 40000;
static const ap_uint<8> UDP_HEADER_BYTES = 8;
static const ap_uint<16> IEX_DEEP_PROTOCOL_ID = 0x8004;
static const ap_uint<11> IEX_TP_HEADER_BYTES = 40;
static const ap_uint<11> IEX_TP_MESSAGE_COUNT_OFFSET = 14;
static const ap_uint<11> IEX_MESSAGE_BLOCK_HEADER_BYTES = 2;
static const ap_uint<11> IEX_SYMBOL_OFFSET = 10;
static const ap_uint<11> IEX_SYMBOL_BYTES = 8;
static const ap_uint<8> IEX_MSG_TYPE_PRICE_BUY = '8';
static const ap_uint<8> IEX_MSG_TYPE_PRICE_SELL = '5';
static const ap_uint<8> IEX_MSG_TYPE_TRADE_REPORT = 'T';
static const ap_uint<32> BEACON_INTERVAL_CYCLES = 25000000;
static const ap_uint<32> GRATUITOUS_ARP_INTERVAL_CYCLES = 125000000;
static const int MAX_ETH_PAYLOAD_BYTES_INT = 1500;
static const ap_uint<11> MAX_ETH_PAYLOAD_BYTES = 1500;
static const ap_uint<11> MIN_ETH_PAYLOAD_BYTES = 46;
static const int ETH_HEADER_BYTES_INT = 14;
static const ap_uint<5> ETH_HEADER_BYTES = 14;
static const int TX_FRAME_BODY_BYTES_INT =
    ETH_HEADER_BYTES_INT + MAX_ETH_PAYLOAD_BYTES_INT;
static const ap_uint<11> MIN_ETH_FRAME_BODY_BYTES = 60;
static const ap_uint<11> TX_FRAME_BODY_BYTES = TX_FRAME_BODY_BYTES_INT;
static const ap_uint<6> IFG_NIBBLES = 24;

struct EthHeader {
  ap_uint<48> dst_mac;
  ap_uint<48> src_mac;
  ap_uint<16> ethertype;
};

static const ap_uint<8> TOP_20_SYMBOLS[20][8] = {
    {'C', 'Y', 'C', 'N', ' ', ' ', ' ', ' '},
    {'I', 'B', 'I', 'T', ' ', ' ', ' ', ' '},
    {'Q', 'Q', 'Q', 'M', ' ', ' ', ' ', ' '},
    {'S', 'P', 'Y', ' ', ' ', ' ', ' ', ' '},
    {'D', 'I', 'A', ' ', ' ', ' ', ' ', ' '},
    {'T', 'Q', 'Q', 'Q', ' ', ' ', ' ', ' '},
    {'S', 'Q', 'Q', 'Q', ' ', ' ', ' ', ' '},
    {'B', 'I', 'T', 'X', ' ', ' ', ' ', ' '},
    {'U', 'S', 'O', ' ', ' ', ' ', ' ', ' '},
    {'Q', 'L', 'D', ' ', ' ', ' ', ' ', ' '},
    {'S', 'P', 'Y', 'M', ' ', ' ', ' ', ' '},
    {'R', 'E', 'N', 'X', ' ', ' ', ' ', ' '},
    {'E', 'T', 'H', 'A', ' ', ' ', ' ', ' '},
    {'U', 'V', 'I', 'X', ' ', ' ', ' ', ' '},
    {'I', 'Q', 'M', 'M', ' ', ' ', ' ', ' '},
    {'C', 'M', 'C', 'I', ' ', ' ', ' ', ' '},
    {'U', 'C', 'I', 'B', ' ', ' ', ' ', ' '},
    {'S', 'O', 'X', 'L', ' ', ' ', ' ', ' '},
    {'Q', 'I', 'D', ' ', ' ', ' ', ' ', ' '},
    {'B', 'I', 'T', 'O', ' ', ' ', ' ', ' '}};

#endif
