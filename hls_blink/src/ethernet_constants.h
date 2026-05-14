#ifndef ETHERNET_CONSTANTS_H
#define ETHERNET_CONSTANTS_H

#include "ap_int.h"

static const ap_uint<48> FPGA_MAC = 0x020000000001ULL;
static const ap_uint<48> BROADCAST_MAC = 0xffffffffffffULL;
static const ap_uint<16> CUSTOM_ETHERTYPE = 0x88B5;
static const ap_uint<32> BEACON_INTERVAL_CYCLES = 25000000;
static const int MAX_ETH_PAYLOAD_BYTES_INT = 1500;
static const ap_uint<11> MAX_ETH_PAYLOAD_BYTES = 1500;
static const ap_uint<11> MIN_ETH_PAYLOAD_BYTES = 46;
static const ap_uint<5> ETH_HEADER_BYTES = 14;
static const ap_uint<6> IFG_NIBBLES = 24;

struct EthHeader {
    ap_uint<48> dst_mac;
    ap_uint<48> src_mac;
    ap_uint<16> ethertype;
};

#endif
