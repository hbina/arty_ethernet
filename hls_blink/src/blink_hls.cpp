#include "ap_int.h"

extern "C" void blink_hls(ap_uint<1> frame_event,
                          ap_uint<1> rx_active,
                          ap_uint<4> &led) {
#pragma HLS INTERFACE ap_none port=frame_event
#pragma HLS INTERFACE ap_none port=rx_active
#pragma HLS INTERFACE ap_none port=led
#pragma HLS INTERFACE ap_ctrl_none port=return

    static ap_uint<32> frame_count = 0;
    static ap_uint<25> activity_holdoff = 0;

    if (frame_event) {
        frame_count++;
        activity_holdoff = 12500000;
    } else if (rx_active) {
        activity_holdoff = 12500000;
    } else if (activity_holdoff != 0) {
        activity_holdoff--;
    }

    led[0] = activity_holdoff != 0;
    led[1] = frame_count[0];
    led[2] = frame_count[1];
    led[3] = frame_count[2];
}
