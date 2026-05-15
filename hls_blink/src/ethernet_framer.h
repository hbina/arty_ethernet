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

// Return one byte of the 14-byte Ethernet header:
// destination MAC, source MAC, then EtherType.
static ap_uint<8>
ethernet_header_byte(const EthHeader &header, ap_uint<4> index) {
#pragma HLS INLINE
  if (index < 6) {
    return mac_byte(header.dst_mac, index);
  }
  if (index < 12) {
    return mac_byte(header.src_mac, index - 6);
  }
  if (index == 12) {
    return header.ethertype.range(15, 8);
  }
  return header.ethertype.range(7, 0);
}

// Read one TX payload byte from BRAM. Bytes past payload_len are zero so the
// framer can naturally emit Ethernet minimum-frame padding.
static ap_uint<8> tx_payload_byte(
    ap_uint<8> tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<11> index,
    ap_uint<11> payload_len) {
#pragma HLS INLINE
  if (index >= payload_len || index >= MAX_ETH_PAYLOAD_BYTES) {
    return 0;
  }
  return tx_payload_buf[index];
}

// Return one byte of the Ethernet frame body, excluding preamble/SFD and FCS.
// The body is header first, then payload bytes or zero padding.
static ap_uint<8> ethernet_frame_body_byte(
    const EthHeader &header,
    ap_uint<8> tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
    ap_uint<11> payload_len,
    ap_uint<11> index) {
#pragma HLS INLINE
  if (index < ETH_HEADER_BYTES) {
    return ethernet_header_byte(header, index);
  }

  ap_uint<11> payload_index = index - ETH_HEADER_BYTES;
  return tx_payload_byte(tx_payload_buf, payload_index, payload_len);
}

// Generic Ethernet TX framer.
// Given header metadata and a payload buffer, it emits preamble/SFD, Ethernet
// header, payload, minimum-frame padding, FCS, and IFG. It is reusable by
// future ARP/IPv4/UDP/TCP producers because it only depends on metadata +
// payload RAM.
static void ethernet_tx_framer_step(
    bool start_request,
    const EthHeader &start_header,
    ap_uint<11> start_payload_len,
    ap_uint<8> tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
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
  static EthHeader tx_header = {0, FPGA_MAC, CUSTOM_ETHERTYPE};
  static ap_uint<11> tx_payload_len = 0;
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
      // Latch request metadata at frame start. The payload body is padded
      // to Ethernet's 46-byte minimum when needed.
      tx_header = start_header;
      tx_payload_len = start_payload_len;
      tx_frame_body_len =
          ETH_HEADER_BYTES + ((start_payload_len < MIN_ETH_PAYLOAD_BYTES)
                                  ? MIN_ETH_PAYLOAD_BYTES
                                  : start_payload_len);
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
    ap_uint<8> out_byte = ethernet_frame_body_byte(
        tx_header,
        tx_payload_buf,
        tx_payload_len,
        tx_byte_index);
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
