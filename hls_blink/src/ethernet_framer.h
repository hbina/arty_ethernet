#ifndef ETHERNET_FRAMER_H
#define ETHERNET_FRAMER_H

#include "ap_int.h"
#include "ethernet_constants.h"
#include "ethernet_mii.h"
#include "protocol_helpers.h"

enum TxState {
  TX_IDLE = 0,
  TX_PREAMBLE = 1,
  TX_DATA = 2,
  TX_FCS = 3,
  TX_IFG = 4
};

// Advance the Ethernet CRC-32 LFSR by one byte.
// The CRC is kept in the reflected Ethernet bit order used on the wire.
static ap_uint<32> crc32_next_byte(ap_uint<32> crc_in, ap_uint<8> data_in) {
#pragma HLS INLINE
  ap_uint<32> crc = crc_in;
  for (int i = 0; i < 8; ++i) {
#pragma HLS UNROLL
    if ((crc[0] ^ data_in[i]) != 0) {
      crc = (crc >> 1) ^ 0xEDB88320;
    } else {
      crc = crc >> 1;
    }
  }
  return crc;
}

// Read one TX frame-body byte from BRAM. Bytes past frame_body_len are zero so
// the framer can naturally emit Ethernet minimum-frame padding.
static ap_uint<8> tx_frame_body_byte(
    ap_uint<8> tx_frame_body_buf[TX_FRAME_BODY_BYTES_INT],
    ap_uint<11> index,
    ap_uint<11> frame_body_len) {
#pragma HLS INLINE
  if (index >= frame_body_len || index >= TX_FRAME_BODY_BYTES) {
    return 0;
  }
  return tx_frame_body_buf[index];
}

// Generic Ethernet TX framer.
// Given a complete Ethernet frame body buffer, it emits preamble/SFD, body
// bytes, minimum-frame padding, FCS, and IFG.
static void ethernet_tx_framer_step(
    bool start_request,
    ap_uint<11> start_len,
    ap_uint<8> tx_frame_body_buf[TX_FRAME_BODY_BYTES_INT],
    ap_uint<1> &eth_tx_en,
    ap_uint<4> &eth_txd,
    ap_uint<1> &tx_frame_toggle,
    ap_uint<1> &tx_active,
    bool &tx_idle) {
#pragma HLS INLINE
  static TxState tx_state = TX_IDLE;
  static bool tx_nibble_phase = false;
  static ap_uint<11> tx_byte_index = 0;
  static ap_uint<2> tx_fcs_index = 0;
  static ap_uint<6> tx_ifg_count = 0;
  static ap_uint<32> tx_crc = 0xffffffff;
  static ap_uint<32> tx_fcs = 0;
  static ap_uint<11> tx_len = 0;
  static ap_uint<11> tx_frame_body_len = 0;
  static bool tx_frame = false;

  bool byte_done = false;
  tx_idle = (tx_state == TX_IDLE);

  switch (tx_state) {
  case TX_IDLE:
    // Wait here until the arbiter has prepared a complete request.
    eth_tx_en = 0;
    eth_txd = 0;
    tx_active = 0;
    tx_nibble_phase = false;
    tx_byte_index = 0;
    tx_fcs_index = 0;
    tx_crc = 0xffffffff;
    tx_idle = true;

    if (start_request) {
      // Latch request length at frame start. The frame body already includes
      // the Ethernet header and is padded to Ethernet's 60-byte minimum.
      tx_len = start_len;
      tx_frame_body_len = (start_len < MIN_ETH_FRAME_BODY_BYTES)
                              ? MIN_ETH_FRAME_BODY_BYTES
                              : start_len;
      tx_state = TX_PREAMBLE;
      tx_idle = false;
    }
    break;

  case TX_PREAMBLE: {
    // Emit seven 0x55 preamble bytes followed by the 0xd5 SFD.
    tx_active = 1;
    ap_uint<8> preamble_byte =
        (tx_byte_index == 7) ? ap_uint<8>(0xd5) : ap_uint<8>(0x55);
    mii_tx_emit_byte_step(
        preamble_byte,
        tx_nibble_phase,
        eth_tx_en,
        eth_txd,
        byte_done);
    if (byte_done) {
      if (tx_byte_index == 7) {
        tx_byte_index = 0;
        tx_state = TX_DATA;
      } else {
        tx_byte_index++;
      }
    }
    break;
  }

  case TX_DATA: {
    // Emit header/payload/padding bytes and fold each complete byte into FCS.
    tx_active = 1;
    ap_uint<8> out_byte =
        tx_frame_body_byte(tx_frame_body_buf, tx_byte_index, tx_len);
    mii_tx_emit_byte_step(
        out_byte,
        tx_nibble_phase,
        eth_tx_en,
        eth_txd,
        byte_done);
    if (byte_done) {
      ap_uint<32> next_crc = crc32_next_byte(tx_crc, out_byte);
      tx_crc = next_crc;
      if (tx_byte_index == tx_frame_body_len - 1) {
        tx_fcs = ~next_crc;
        tx_fcs_index = 0;
        tx_state = TX_FCS;
      } else {
        tx_byte_index++;
      }
    }
    break;
  }

  case TX_FCS: {
    // Ethernet transmits the complemented CRC least-significant byte first.
    tx_active = 1;
    ap_uint<8> fcs_byte = 0;
    if (tx_fcs_index == 0) {
      fcs_byte = tx_fcs.range(7, 0);
    } else if (tx_fcs_index == 1) {
      fcs_byte = tx_fcs.range(15, 8);
    } else if (tx_fcs_index == 2) {
      fcs_byte = tx_fcs.range(23, 16);
    } else {
      fcs_byte = tx_fcs.range(31, 24);
    }

    mii_tx_emit_byte_step(
        fcs_byte,
        tx_nibble_phase,
        eth_tx_en,
        eth_txd,
        byte_done);
    if (byte_done) {
      if (tx_fcs_index == 3) {
        // Mark one complete transmitted frame for status logic.
        tx_frame = !tx_frame;
        tx_ifg_count = 0;
        tx_state = TX_IFG;
      } else {
        tx_fcs_index++;
      }
    }
    break;
  }

  case TX_IFG:
    // Hold TX idle for the inter-frame gap before accepting another request.
    eth_tx_en = 0;
    eth_txd = 0;
    tx_active = 0;
    if (tx_ifg_count == IFG_NIBBLES - 1) {
      tx_state = TX_IDLE;
      tx_idle = true;
    } else {
      tx_ifg_count++;
    }
    break;

  default:
    tx_state = TX_IDLE;
    break;
  }

  tx_frame_toggle = tx_frame;
}

#endif
