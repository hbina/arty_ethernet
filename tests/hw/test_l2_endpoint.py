import time

from .ethernet import (
    ARTY_ACK,
    BROADCAST_MAC,
    CUSTOM_ETHERTYPE,
    FPGA_MAC,
    build_broadcast_probe,
    build_unicast_probe,
    build_wrong_destination_probe,
    build_wrong_ethertype_probe,
    expect_frame,
    is_beacon_payload,
    mac_text,
)


def _is_ack_to_host(raw_eth):
    return lambda frame: (
        frame.src_mac == FPGA_MAC
        and frame.dst_mac == raw_eth.host_mac
        and frame.ethertype == CUSTOM_ETHERTYPE
        and frame.payload == ARTY_ACK
    )


def test_periodic_beacon(raw_eth, timeout):
    expect_frame(
        raw_eth,
        timeout,
        lambda frame: (
            frame.src_mac == FPGA_MAC
            and frame.dst_mac == BROADCAST_MAC
            and frame.ethertype == CUSTOM_ETHERTYPE
            and is_beacon_payload(frame.payload)
        ),
        f"diagnostics beacon from {mac_text(FPGA_MAC)} to broadcast",
    )


def test_ignores_unicast_custom_probe(raw_eth, timeout):
    raw_eth.drain(0.1)
    raw_eth.send(build_unicast_probe(raw_eth.host_mac))
    _assert_no_ack(raw_eth, min(timeout, 2.0), "custom unicast probe")


def test_ignores_broadcast_custom_probe(raw_eth, timeout):
    raw_eth.drain(0.1)
    raw_eth.send(build_broadcast_probe(raw_eth.host_mac))
    _assert_no_ack(raw_eth, min(timeout, 2.0), "custom broadcast probe")


def test_ignores_wrong_ethertype(raw_eth, timeout):
    raw_eth.drain(0.1)
    raw_eth.send(build_wrong_ethertype_probe(raw_eth.host_mac))
    _assert_no_ack(raw_eth, min(timeout, 2.0), "wrong EtherType")


def test_ignores_wrong_destination(raw_eth, timeout):
    raw_eth.drain(0.1)
    raw_eth.send(build_wrong_destination_probe(raw_eth.host_mac))
    _assert_no_ack(raw_eth, min(timeout, 2.0), "wrong destination MAC")


def _assert_no_ack(raw_eth, timeout, condition):
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        frame = raw_eth.recv(remaining)
        if frame is None:
            return
        from .ethernet import parse_frame

        try:
            parsed = parse_frame(frame)
        except ValueError:
            continue
        assert not _is_ack_to_host(raw_eth)(parsed), (
            f"FPGA sent ARTY_ACK after {condition}: "
            f"{mac_text(parsed.src_mac)} -> {mac_text(parsed.dst_mac)}"
        )
