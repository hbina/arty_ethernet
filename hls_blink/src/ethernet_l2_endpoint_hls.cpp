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

enum RxState { RX_SEARCH = 0, RX_DATA = 1 };
enum TxRequestKind { TX_REQ_NONE = 0, TX_REQ_ACK = 1, TX_REQ_BEACON = 2 };
enum TxState { TX_IDLE = 0, TX_PREAMBLE = 1, TX_DATA = 2, TX_FCS = 3, TX_IFG = 4 };

struct EthHeader {
    ap_uint<48> dst_mac;
    ap_uint<48> src_mac;
    ap_uint<16> ethertype;
};

struct TxRequest {
    bool valid;
    ap_uint<48> dst_mac;
    ap_uint<16> ethertype;
    ap_uint<11> payload_len;
    TxRequestKind request_kind;
};

struct RxEvent {
    bool valid;
    ap_uint<48> dst_mac;
    ap_uint<48> src_mac;
    ap_uint<16> ethertype;
    bool accepted;
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

// Return one network-order MAC byte from a packed 48-bit MAC address.
// index 0 is the first byte transmitted on Ethernet.
static ap_uint<8> mac_byte(ap_uint<48> mac, ap_uint<3> index) {
#pragma HLS INLINE
    switch ((unsigned)index) {
    case 0: return mac.range(47, 40);
    case 1: return mac.range(39, 32);
    case 2: return mac.range(31, 24);
    case 3: return mac.range(23, 16);
    case 4: return mac.range(15, 8);
    default: return mac.range(7, 0);
    }
}

// Return one byte of the 14-byte Ethernet header:
// destination MAC, source MAC, then EtherType.
static ap_uint<8> ethernet_header_byte(const EthHeader &header, ap_uint<4> index) {
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

// Read one TX payload byte from BRAM. Bytes past payload_len are zero so the
// framer can naturally emit Ethernet minimum-frame padding.
static ap_uint<8> tx_payload_byte(ap_uint<8> tx_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT],
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

// Parse an Ethernet frame header from assembled RX bytes.
// The parser skips a standard preamble/SFD if present, records destination MAC,
// source MAC, and EtherType, and marks the frame accepted only when it is a
// valid custom frame for our MAC or broadcast. FCS is not checked here.
static void ethernet_rx_parser_step(bool frame_start,
                                    bool byte_valid,
                                    ap_uint<8> data_byte,
                                    bool frame_end,
                                    ap_uint<1> eth_rxerr,
                                    RxEvent &event) {
#pragma HLS INLINE
    static RxState rx_state = RX_SEARCH;
    static ap_uint<4> rx_preamble_count = 0;
    static ap_uint<16> rx_byte_index = 0;
    static bool rx_frame_error = false;
    static bool rx_dest_broadcast = true;
    static EthHeader rx_header = {0, 0, 0};

    event.valid = false;
    event.dst_mac = 0;
    event.src_mac = 0;
    event.ethertype = 0;
    event.accepted = false;

    if (frame_start) {
        // Clear all header state at the first nibble of a new RX frame.
        rx_state = RX_SEARCH;
        rx_preamble_count = 0;
        rx_byte_index = 0;
        rx_frame_error = false;
        rx_dest_broadcast = true;
        rx_header.dst_mac = 0;
        rx_header.src_mac = 0;
        rx_header.ethertype = 0;
    }

    rx_frame_error = rx_frame_error || (eth_rxerr != 0);

    if (byte_valid) {
        bool process_data_byte = false;

        if (rx_state == RX_SEARCH) {
            // Ignore 0x55 preamble bytes until SFD. If no preamble is present,
            // treat the first non-preamble byte as destination MAC byte 0.
            if (data_byte == 0x55) {
                if (rx_preamble_count != 15) {
                    rx_preamble_count++;
                }
            } else if (data_byte == 0xd5 && rx_preamble_count != 0) {
                rx_state = RX_DATA;
                rx_byte_index = 0;
            } else {
                rx_state = RX_DATA;
                process_data_byte = true;
            }
        } else {
            process_data_byte = true;
        }

        if (process_data_byte) {
            // Capture the Ethernet header byte-by-byte. Later payload/FCS bytes
            // are ignored for now because RX payload buffering is deferred.
            switch ((unsigned)rx_byte_index) {
            case 0:
                rx_header.dst_mac.range(47, 40) = data_byte;
                rx_dest_broadcast = (data_byte == 0xff);
                break;
            case 1:
                rx_header.dst_mac.range(39, 32) = data_byte;
                rx_dest_broadcast = rx_dest_broadcast && (data_byte == 0xff);
                break;
            case 2:
                rx_header.dst_mac.range(31, 24) = data_byte;
                rx_dest_broadcast = rx_dest_broadcast && (data_byte == 0xff);
                break;
            case 3:
                rx_header.dst_mac.range(23, 16) = data_byte;
                rx_dest_broadcast = rx_dest_broadcast && (data_byte == 0xff);
                break;
            case 4:
                rx_header.dst_mac.range(15, 8) = data_byte;
                rx_dest_broadcast = rx_dest_broadcast && (data_byte == 0xff);
                break;
            case 5:
                rx_header.dst_mac.range(7, 0) = data_byte;
                rx_dest_broadcast = rx_dest_broadcast && (data_byte == 0xff);
                break;
            case 6: rx_header.src_mac.range(47, 40) = data_byte; break;
            case 7: rx_header.src_mac.range(39, 32) = data_byte; break;
            case 8: rx_header.src_mac.range(31, 24) = data_byte; break;
            case 9: rx_header.src_mac.range(23, 16) = data_byte; break;
            case 10: rx_header.src_mac.range(15, 8) = data_byte; break;
            case 11: rx_header.src_mac.range(7, 0) = data_byte; break;
            case 12: rx_header.ethertype.range(15, 8) = data_byte; break;
            case 13: rx_header.ethertype.range(7, 0) = data_byte; break;
            default: break;
            }
            rx_byte_index++;
        }
    }

    if (frame_end) {
        // Produce one metadata event at end-of-frame. eth_rxerr and header
        // checks decide whether higher-level logic should respond.
        bool dest_ok = (rx_header.dst_mac == FPGA_MAC) || rx_dest_broadcast;
        bool ethertype_ok = (rx_header.ethertype == CUSTOM_ETHERTYPE);
        event.valid = (rx_byte_index >= ETH_HEADER_BYTES);
        event.dst_mac = rx_header.dst_mac;
        event.src_mac = rx_header.src_mac;
        event.ethertype = rx_header.ethertype;
        event.accepted = event.valid && !rx_frame_error && dest_ok && ethertype_ok;

        rx_state = RX_SEARCH;
        rx_preamble_count = 0;
        rx_byte_index = 0;
        rx_frame_error = false;
        rx_dest_broadcast = true;
    }
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

// Generic Ethernet TX framer.
// Given header metadata and a payload buffer, it emits preamble/SFD, Ethernet
// header, payload, minimum-frame padding, FCS, and IFG. It is reusable by future
// ARP/IPv4/UDP/TCP producers because it only depends on metadata + payload RAM.
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
            tx_frame_body_len = ETH_HEADER_BYTES +
                                ((start_payload_len < MIN_ETH_PAYLOAD_BYTES)
                                     ? MIN_ETH_PAYLOAD_BYTES
                                     : start_payload_len);
            tx_state = TX_PREAMBLE;
            tx_idle = false;
        }
        break;

    case TX_PREAMBLE: {
        // Emit seven 0x55 preamble bytes followed by the 0xd5 SFD.
        tx_active = 1;
        ap_uint<8> preamble_byte = (tx_byte_index == 7) ? ap_uint<8>(0xd5)
                                                        : ap_uint<8>(0x55);
        mii_tx_emit_byte_step(preamble_byte, tx_nibble_phase, eth_tx_en, eth_txd,
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
        ap_uint<8> out_byte = ethernet_frame_body_byte(tx_header, tx_payload_buf,
                                                       tx_payload_len, tx_byte_index);
        mii_tx_emit_byte_step(out_byte, tx_nibble_phase, eth_tx_en, eth_txd,
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

        mii_tx_emit_byte_step(fcs_byte, tx_nibble_phase, eth_tx_en, eth_txd,
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

#ifndef __SYNTHESIS__
// C-simulation-only harness for testing the generic framer with arbitrary
// payload lengths. This function is excluded from synthesized RTL.
extern "C" void ethernet_l2_endpoint_hls_test_frame(ap_uint<48> dst_mac,
                                                    ap_uint<16> ethertype,
                                                    ap_uint<11> payload_len,
                                                    ap_uint<16> cycle,
                                                    ap_uint<1> &eth_tx_en,
                                                    ap_uint<4> &eth_txd,
                                                    ap_uint<1> &tx_frame_toggle,
                                                    ap_uint<1> &tx_active) {
    static ap_uint<8> test_payload_buf[MAX_ETH_PAYLOAD_BYTES_INT];
    static bool initialized = false;
    static bool started = false;

    if (!initialized) {
        // Fill test payload with a deterministic byte ramp: 0, 1, 2, ...
        for (ap_uint<11> i = 0; i < MAX_ETH_PAYLOAD_BYTES; ++i) {
            test_payload_buf[i] = i.range(7, 0);
        }
        initialized = true;
    }

    EthHeader header = {dst_mac, FPGA_MAC, ethertype};
    bool tx_idle = false;
    bool start_request = !started && (cycle == 0);
    if (start_request) {
        started = true;
    }

    ethernet_tx_framer_step(start_request, header, payload_len, test_payload_buf,
                            eth_tx_en, eth_txd, tx_frame_toggle, tx_active, tx_idle);

    if (tx_idle && started && cycle != 0) {
        started = false;
    }
}
#endif
