#ifndef ETHERNET_PACKET_QUEUE_H
#define ETHERNET_PACKET_QUEUE_H

#include "ap_int.h"
#include "ethernet_constants.h"

static const int RX_PACKET_SLOTS = 8;
static const int TX_PACKET_SLOTS = 4;

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

#endif
