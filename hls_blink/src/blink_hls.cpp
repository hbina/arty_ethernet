#include "ap_int.h"

extern "C" void blink_hls(ap_uint<4> &led) {
#pragma HLS INTERFACE ap_none port=led
#pragma HLS INTERFACE ap_ctrl_none port=return

    static ap_uint<28> counter = 0;
    counter++;
    led = counter.range(27, 24);
}
