#ifndef ETHERNET_PERIODIC_TIMERS_H
#define ETHERNET_PERIODIC_TIMERS_H

#include "ap_int.h"
#include "ethernet_constants.h"

struct PeriodicTxTicks {
  bool beacon_tick;
  bool gratuitous_arp_tick;
};

static bool
periodic_counter_tick(ap_uint<32> interval_cycles, ap_uint<32> &counter) {
#pragma HLS INLINE
  if (counter == interval_cycles - 1) {
    counter = 0;
    return true;
  }

  counter++;
  return false;
}

static PeriodicTxTicks periodic_tx_timer_step() {
#pragma HLS INLINE
  static ap_uint<32> beacon_count = 0;
  static ap_uint<32> gratuitous_arp_count = 0;

  PeriodicTxTicks ticks;
  ticks.beacon_tick =
      periodic_counter_tick(BEACON_INTERVAL_CYCLES, beacon_count);
  ticks.gratuitous_arp_tick = periodic_counter_tick(
      GRATUITOUS_ARP_INTERVAL_CYCLES,
      gratuitous_arp_count);
  return ticks;
}

#endif
