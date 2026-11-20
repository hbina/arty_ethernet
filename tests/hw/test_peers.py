"""ARP peer cache on the board. Only designs whose beacon reports PEERS=
(the bitlogic design) have one; others are skipped."""

from .ethernet import (
    DIAGNOSTIC_BEACON_ETHERTYPE,
    FPGA_MAC,
    beacon_fields,
    build_arp_request,
    expect_frame,
)

import pytest

HOST_IP = "192.168.1.10"


def next_beacon(raw_eth, timeout):
    frame = expect_frame(
        raw_eth,
        timeout,
        lambda frame: (
            frame.src_mac == FPGA_MAC
            and frame.ethertype == DIAGNOSTIC_BEACON_ETHERTYPE
            and beacon_fields(frame.payload) is not None
        ),
        "diagnostics beacon",
    )
    return beacon_fields(frame.payload)


def test_arp_request_adds_host_to_peer_cache(raw_eth, timeout):
    if "PEERS" not in next_beacon(raw_eth, timeout):
        pytest.skip("the programmed design has no peer cache (no PEERS= beacon field)")
    raw_eth.drain(0.1)
    raw_eth.send(build_arp_request(raw_eth.host_mac, HOST_IP))
    # Beacons are sent once per second; the one after the request counts it.
    raw_eth.drain(0.05)
    assert int(next_beacon(raw_eth, timeout)["PEERS"], 16) >= 1
