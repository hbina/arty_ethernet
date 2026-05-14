#include "ap_int.h"
#include "ethernet_constants.h"
#include "ethernet_framer.h"
#include "ethernet_mii.h"
#include "ethernet_rx.h"

enum TxRequestKind { TX_REQ_NONE = 0, TX_REQ_ACK = 1, TX_REQ_BEACON = 2 };

struct TxRequest {
    bool valid;
    ap_uint<48> dst_mac;
    ap_uint<16> ethertype;
    ap_uint<11> payload_len;
    TxRequestKind request_kind;
};

// Return one byte from the fixed ACK payload literal, ARTY_ACK.
static ap_uint<8> ack_payload_literal_byte(ap_uint<4> index) {
#pragma HLS INLINE
    switch ((unsigned)index) {
    case 0: return 'A';
    case 1: return 'R';
    case 2: return 'T';
    case 3: return 'Y';
    case 4: return '_';
    case 5: return 'A';
    case 6: return 'C';
    default: return 'K';
    }
}

// Return one byte from the fixed beacon payload literal, ARTY_BEACON.
static ap_uint<8> beacon_payload_literal_byte(ap_uint<4> index) {
#pragma HLS INLINE
    switch ((unsigned)index) {
    case 0: return 'A';
    case 1: return 'R';
    case 2: return 'T';
    case 3: return 'Y';
    case 4: return '_';
    case 5: return 'B';
    case 6: return 'E';
    case 7: return 'A';
    case 8: return 'C';
    case 9: return 'O';
    default: return 'N';
    }
}

// Write one byte of the currently selected fixed payload into BRAM.
// This intentionally writes one byte per top-level call so the endpoint can
// remain a one-cycle initiation-interval pipeline.
static void prepare_payload_step(ap_uint<8> tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
                                 TxRequestKind request_kind,
                                 ap_uint<4> payload_index,
                                 bool &done) {
#pragma HLS INLINE
    ap_uint<11> payload_len = (request_kind == TX_REQ_ACK) ? ap_uint<11>(8)
                                                           : ap_uint<11>(11);
    ap_uint<8> payload_byte =
        (request_kind == TX_REQ_ACK) ? ack_payload_literal_byte(payload_index)
                                     : beacon_payload_literal_byte(payload_index);
    tx_payload_buf[payload_index] = payload_byte;
    done = (payload_index == payload_len - 1);
}

// RX integration step for the top-level endpoint.
// Accepted custom frames arm a pending ACK to the received source MAC and toggle
// rx_accept_toggle for the LED/status clock-domain crossing in hls_top.v.
static void ethernet_rx_step(ap_uint<1> eth_rx_dv,
                             ap_uint<4> eth_rxd,
                             ap_uint<1> eth_rxerr,
                             bool &ack_pending,
                             ap_uint<48> &ack_dst_pending,
                             ap_uint<1> &rx_accept_toggle,
                             ap_uint<1> &rx_active) {
#pragma HLS INLINE
    static bool rx_accept = false;

    bool frame_start;
    bool byte_valid;
    ap_uint<8> data_byte;
    bool frame_end;
    RxEvent event;

    rx_active = eth_rx_dv;
    mii_rx_byte_assembler_step(eth_rx_dv, eth_rxd, frame_start, byte_valid, data_byte,
                               frame_end);
    ethernet_rx_parser_step(frame_start, byte_valid, data_byte, frame_end, eth_rxerr,
                            event);

    if (event.valid && event.accepted) {
        ack_pending = true;
        ack_dst_pending = event.src_mac;
        rx_accept = !rx_accept;
    }

    rx_accept_toggle = rx_accept;
}

// Choose the next TX producer. ACKs are higher priority than periodic beacons.
// The returned request only carries metadata; payload bytes are prepared later.
static TxRequest tx_request_arbiter_step(bool ack_pending,
                                         ap_uint<48> ack_dst_pending,
                                         bool beacon_pending) {
#pragma HLS INLINE
    TxRequest request;
    request.valid = ack_pending || beacon_pending;
    request.dst_mac = 0;
    request.ethertype = CUSTOM_ETHERTYPE;
    request.payload_len = 0;
    request.request_kind = TX_REQ_NONE;

    if (ack_pending) {
        request.dst_mac = ack_dst_pending;
        request.request_kind = TX_REQ_ACK;
    } else if (beacon_pending) {
        request.dst_mac = BROADCAST_MAC;
        request.request_kind = TX_REQ_BEACON;
    }

    return request;
}

// TX integration step for the top-level endpoint.
// It creates periodic beacon requests, accepts pending ACK requests from RX,
// prepares the selected payload in BRAM, and feeds the generic framer.
static void ethernet_tx_step(bool &ack_pending,
                             ap_uint<48> ack_dst_pending,
                             ap_uint<1> &eth_tx_en,
                             ap_uint<4> &eth_txd,
                             ap_uint<1> &tx_frame_toggle,
                             ap_uint<1> &tx_active) {
    static ap_uint<8> tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT];
#pragma HLS BIND_STORAGE variable=tx_payload_buf type=ram_t2p impl=bram
    static ap_uint<32> beacon_count = BEACON_INTERVAL_CYCLES - 4;
    static bool beacon_pending = false;
    static bool framer_idle_last = true;
    static bool preparing_payload = false;
    static TxRequestKind prepare_kind = TX_REQ_NONE;
    static ap_uint<4> prepare_index = 0;
    static EthHeader pending_header = {0, FPGA_MAC, CUSTOM_ETHERTYPE};
    static ap_uint<11> pending_payload_len = 0;

    // Convert the free-running counter into a sticky beacon request.
    if (beacon_count == BEACON_INTERVAL_CYCLES - 1) {
        beacon_count = 0;
        beacon_pending = true;
    } else {
        beacon_count++;
    }

    bool tx_idle = false;
    bool start_request = false;
    EthHeader start_header = pending_header;
    ap_uint<11> start_payload_len = 0;

    if (!preparing_payload && framer_idle_last) {
        // Only choose a new request when no payload preparation is in progress
        // and the framer was idle on the previous cycle.
        TxRequest request = tx_request_arbiter_step(ack_pending, ack_dst_pending,
                                                    beacon_pending);
        if (request.valid) {
            preparing_payload = true;
            prepare_kind = request.request_kind;
            prepare_index = 0;
            pending_header.dst_mac = request.dst_mac;
            pending_header.src_mac = FPGA_MAC;
            pending_header.ethertype = request.ethertype;
            pending_payload_len =
                (request.request_kind == TX_REQ_ACK) ? ap_uint<11>(8)
                                                     : ap_uint<11>(11);

            // The request is now owned by the TX path, even though its payload
            // will take several cycles to write into BRAM.
            if (request.request_kind == TX_REQ_ACK) {
                ack_pending = false;
            } else if (request.request_kind == TX_REQ_BEACON) {
                beacon_pending = false;
            }
        }
    }

    if (preparing_payload) {
        // Write one fixed-payload byte per cycle; launch the frame after the
        // last byte is present in BRAM.
        bool prepare_done = false;
        prepare_payload_step(tx_payload_buf, prepare_kind, prepare_index,
                             prepare_done);
        if (prepare_done) {
            start_header = pending_header;
            start_payload_len = pending_payload_len;
            start_request = true;
            preparing_payload = false;
            prepare_kind = TX_REQ_NONE;
        } else {
            prepare_index++;
        }
    } else if (framer_idle_last) {
        // Keep the reusable payload RAM inferred as a full 8-bit byte store.
        tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT - 1] = 0x80;
    }

    ethernet_tx_framer_step(start_request, start_header, start_payload_len,
                            tx_payload_buf, eth_tx_en, eth_txd, tx_frame_toggle,
                            tx_active, tx_idle);
    framer_idle_last = tx_idle;
}

// External Vitis HLS top level.
// hls_top.v instantiates this block directly on the 25 MHz Ethernet clock.
// The function is ap_ctrl_none and pipelined with II=1 so it behaves like
// always-running hardware rather than a call/return accelerator.
extern "C" void ethernet_l2_endpoint_hls(ap_uint<1> eth_rx_dv,
                                         ap_uint<4> eth_rxd,
                                         ap_uint<1> eth_rxerr,
                                         ap_uint<1> &eth_tx_en,
                                         ap_uint<4> &eth_txd,
                                         ap_uint<1> &rx_accept_toggle,
                                         ap_uint<1> &tx_frame_toggle,
                                         ap_uint<1> &rx_active,
                                         ap_uint<1> &tx_active) {
#pragma HLS INTERFACE ap_none port=eth_rx_dv
#pragma HLS INTERFACE ap_none port=eth_rxd
#pragma HLS INTERFACE ap_none port=eth_rxerr
#pragma HLS INTERFACE ap_none port=eth_tx_en
#pragma HLS INTERFACE ap_none port=eth_txd
#pragma HLS INTERFACE ap_none port=rx_accept_toggle
#pragma HLS INTERFACE ap_none port=tx_frame_toggle
#pragma HLS INTERFACE ap_none port=rx_active
#pragma HLS INTERFACE ap_none port=tx_active
#pragma HLS INTERFACE ap_ctrl_none port=return
#pragma HLS PIPELINE II=1

    static bool ack_pending = false;
    static ap_uint<48> ack_dst_pending = 0;

    ethernet_rx_step(eth_rx_dv,
                     eth_rxd,
                     eth_rxerr,
                     ack_pending,
                     ack_dst_pending,
                     rx_accept_toggle,
                     rx_active);

    ethernet_tx_step(ack_pending,
                     ack_dst_pending,
                     eth_tx_en,
                     eth_txd,
                     tx_frame_toggle,
                     tx_active);
}
