#ifndef ETHERNET_PACKET_QUEUE_H
#define ETHERNET_PACKET_QUEUE_H

#include "ap_int.h"
#include "ethernet_constants.h"

static const int RX_PACKET_SLOTS = 8;
static const int TX_PACKET_SLOTS = 4;

struct TxFrameRequest {
  bool valid;
  ap_uint<11> len;
};

#endif
