#ifndef ETHERNET_PACKET_QUEUE_H
#define ETHERNET_PACKET_QUEUE_H

#include "ap_int.h"
#include "ethernet_constants.h"

template <int IndexBits>
static constexpr int packet_queue_slots_for_index_bits() {
  return 1 << IndexBits;
}

static const int RX_PACKET_INDEX_BITS = 3;
static const int TX_PACKET_INDEX_BITS = 2;

template <int MaxBytes, typename Meta, int IndexBits> struct PacketSlotQueue {
  static const int Slots = packet_queue_slots_for_index_bits<IndexBits>();

  Meta meta[Slots];
  bool valid[Slots];
  ap_uint<8> bytes[Slots][MaxBytes];
  ap_uint<IndexBits> read_idx;
  ap_uint<IndexBits> write_idx;
  ap_uint<32> drop_count;
};

struct RxPacketMeta {
  EthHeader header;
  ap_uint<11> payload_len;
  bool truncated;
};

struct TxPacketMeta {
  ap_uint<11> frame_body_len;
};

using RxPacketQueue = PacketSlotQueue<
    MAX_ETH_PAYLOAD_BYTES_INT,
    RxPacketMeta,
    RX_PACKET_INDEX_BITS>;

using TxPacketQueue = PacketSlotQueue<
    TX_FRAME_BODY_BYTES_INT,
    TxPacketMeta,
    TX_PACKET_INDEX_BITS>;

template <int MaxBytes, typename Meta, int IndexBits>
static ap_uint<IndexBits>
packet_queue_write_slot(PacketSlotQueue<MaxBytes, Meta, IndexBits> &queue) {
#pragma HLS INLINE
  return queue.write_idx;
}

template <int MaxBytes, typename Meta, int IndexBits>
static bool packet_queue_write_slot_available(
    PacketSlotQueue<MaxBytes, Meta, IndexBits> &queue) {
#pragma HLS INLINE
  unsigned write_idx = queue.write_idx;
  return !queue.valid[write_idx];
}

template <int MaxBytes, typename Meta, int IndexBits>
static void packet_queue_publish_write_slot(
    PacketSlotQueue<MaxBytes, Meta, IndexBits> &queue,
    const Meta &meta) {
#pragma HLS INLINE
  unsigned write_idx = queue.write_idx;
  if (queue.valid[write_idx]) {
    queue.drop_count++;
  } else {
    queue.meta[write_idx] = meta;
    queue.valid[write_idx] = true;
    queue.write_idx = queue.write_idx + 1;
  }
}

template <int MaxBytes, typename Meta, int IndexBits>
static ap_uint<IndexBits>
packet_queue_read_slot(PacketSlotQueue<MaxBytes, Meta, IndexBits> &queue) {
#pragma HLS INLINE
  return queue.read_idx;
}

template <int MaxBytes, typename Meta, int IndexBits>
static bool packet_queue_read_slot_valid(
    PacketSlotQueue<MaxBytes, Meta, IndexBits> &queue) {
#pragma HLS INLINE
  unsigned read_idx = queue.read_idx;
  return queue.valid[read_idx];
}

template <int MaxBytes, typename Meta, int IndexBits>
static void packet_queue_consume_read_slot(
    PacketSlotQueue<MaxBytes, Meta, IndexBits> &queue) {
#pragma HLS INLINE
  unsigned read_idx = queue.read_idx;
  queue.valid[read_idx] = false;
  queue.read_idx = queue.read_idx + 1;
}

template <int MaxBytes, typename Meta, int IndexBits>
static void
packet_queue_drop_write(PacketSlotQueue<MaxBytes, Meta, IndexBits> &queue) {
#pragma HLS INLINE
  queue.drop_count++;
}

template <int MaxBytes, typename Meta, int IndexBits>
static ap_uint<32>
packet_queue_drop_count(PacketSlotQueue<MaxBytes, Meta, IndexBits> &queue) {
#pragma HLS INLINE
  return queue.drop_count;
}

struct TxFrameRequest {
  bool valid;
  ap_uint<11> len;
};

#endif
