#include "ap_int.h"

extern "C" void blink_hls(ap_uint<4> &led);

int main() {
    ap_uint<4> led = 0;

    for (int i = 0; i < 1024; ++i) {
        blink_hls(led);
    }

    return 0;
}
