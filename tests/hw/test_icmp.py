"""ICMP echo on the board. Only designs whose beacon reports ICMP= support it
(the bitlogic design); others are skipped."""

import pytest

from .ethernet import (
    DIAGNOSTIC_BEACON_ETHERTYPE,
    FPGA_MAC,
    FPGA_IP,
    beacon_fields,
    build_icmp_echo_request,
    expect_frame,
    is_icmp_echo_reply_to,
    mac_text,
)

HOST_IP = "192.168.1.10"


@pytest.fixture
def icmp_capable(raw_eth, timeout):
    beacon = expect_frame(
        raw_eth,
        timeout,
        lambda frame: (
            frame.src_mac == FPGA_MAC
            and frame.ethertype == DIAGNOSTIC_BEACON_ETHERTYPE
            and beacon_fields(frame.payload) is not None
        ),
        "diagnostics beacon",
    )
    if "ICMP" not in beacon_fields(beacon.payload):
        pytest.skip("the programmed design does not implement ICMP echo (no ICMP= beacon field)")


@pytest.mark.parametrize("size", [0, 1, 56, 1472])
def test_icmp_echo_request_gets_reply(raw_eth, timeout, icmp_capable, size):
    data = bytes((k * 7 + size) & 0xFF for k in range(size))
    ident, seq = 0x5150, size
    raw_eth.drain(0.1)
    raw_eth.send(build_icmp_echo_request(raw_eth.host_mac, HOST_IP, ident, seq, data))
    expect_frame(
        raw_eth,
        min(timeout, 2.0),
        lambda frame: is_icmp_echo_reply_to(frame, raw_eth.host_mac, HOST_IP, ident, seq, data),
        f"ICMP echo reply ({size} data bytes) from {mac_text(FPGA_MAC)} {FPGA_IP}",
    )
