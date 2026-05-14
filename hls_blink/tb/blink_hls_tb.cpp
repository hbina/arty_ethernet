#include "ap_int.h"

extern "C" void blink_hls(ap_uint<1> frame_event, ap_uint<1> rx_active,
                          ap_uint<4> &led);

int main() {
  ap_uint<4> led = 0;

  for (int i = 0; i < 1024; ++i) {
    blink_hls((i & 63) == 0, i & 1, led);
  }

  return 0;
}
