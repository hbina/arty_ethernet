#ifndef ETHERNET_MII_H
#define ETHERNET_MII_H

#include "ap_int.h"

// Convert the 4-bit MII RX stream into bytes.
// MII presents each byte as low nibble then high nibble while eth_rx_dv is high.
// This block also emits one-cycle frame_start/frame_end markers for the parser.
static void mii_rx_byte_assembler_step(ap_uint<1> eth_rx_dv,
                                       ap_uint<4> eth_rxd,
                                       bool &frame_start,
                                       bool &byte_valid,
                                       ap_uint<8> &data_byte,
                                       bool &frame_end) {
#pragma HLS INLINE
    static bool rx_in_frame = false;
    static bool rx_have_low = false;
    static ap_uint<4> rx_low_nibble = 0;

    frame_start = false;
    byte_valid = false;
    frame_end = false;
    data_byte = 0;

    if (eth_rx_dv) {
        if (!rx_in_frame) {
            // First active RX cycle starts a new MII frame.
            rx_in_frame = true;
            rx_have_low = false;
            frame_start = true;
        }

        if (rx_have_low) {
            // High nibble arrived; combine it with the stored low nibble.
            data_byte = (ap_uint<8>(eth_rxd) << 4) | rx_low_nibble;
            byte_valid = true;
        } else {
            // Low nibble arrived; hold it until the next cycle.
            rx_low_nibble = eth_rxd;
        }
        rx_have_low = !rx_have_low;
    } else {
        if (rx_in_frame) {
            // eth_rx_dv falling indicates the frame has ended.
            frame_end = true;
        }
        rx_in_frame = false;
        rx_have_low = false;
    }
}

// Emit one byte onto MII TX as two nibbles, low nibble first.
// byte_done is true on the second nibble cycle, when the caller may advance.
static void mii_tx_emit_byte_step(ap_uint<8> out_byte,
                                  bool &tx_nibble_phase,
                                  ap_uint<1> &eth_tx_en,
                                  ap_uint<4> &eth_txd,
                                  bool &byte_done) {
#pragma HLS INLINE
    eth_tx_en = 1;
    eth_txd = tx_nibble_phase ? out_byte.range(7, 4) : out_byte.range(3, 0);
    byte_done = tx_nibble_phase;
    tx_nibble_phase = !tx_nibble_phase;
}

#endif
