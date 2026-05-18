import pytest

from .ethernet import (
    ARP_ETHERTYPE,
    BROADCAST_MAC,
    FPGA_MAC,
    FPGA_IP,
    IPV4_ETHERTYPE,
    MIN_ETH_PAYLOAD,
    build_arp_request,
    build_frame,
    is_arp_reply_to,
    parse_arp_payload,
    parse_frame,
    parse_mac,
)


def test_parse_mac_accepts_canonical_text():
    assert parse_mac("02:00:00:00:00:01") == FPGA_MAC


def test_parse_mac_rejects_wrong_length():
    with pytest.raises(ValueError):
        parse_mac("02:00:00:00:01")


def test_build_frame_pads_minimum_payload_and_parser_strips_zeroes():
    src = parse_mac("0a:0b:0c:0d:0e:0f")
    payload = b"pytest_payload"
    frame = build_frame(FPGA_MAC, src, payload, IPV4_ETHERTYPE)
    parsed = parse_frame(frame)

    assert len(frame) == 14 + MIN_ETH_PAYLOAD
    assert parsed.dst_mac == FPGA_MAC
    assert parsed.src_mac == src
    assert parsed.ethertype == IPV4_ETHERTYPE
    assert parsed.payload == payload


def test_build_arp_request_sets_expected_fields():
    src = parse_mac("0a:0b:0c:0d:0e:0f")
    frame = build_arp_request(src, "192.168.1.10")
    parsed = parse_frame(frame)
    arp = parse_arp_payload(parsed.payload)

    assert parsed.dst_mac == BROADCAST_MAC
    assert parsed.src_mac == src
    assert parsed.ethertype == ARP_ETHERTYPE
    assert arp["opcode"] == 1
    assert arp["sender_mac"] == src
    assert arp["sender_ip"] == "192.168.1.10"
    assert arp["target_mac"] == b"\x00" * 6
    assert arp["target_ip"] == FPGA_IP


def test_is_arp_reply_to_matches_expected_reply():
    host = parse_mac("0a:0b:0c:0d:0e:0f")
    arp_payload = (
        b"\x00\x01\x08\x00\x06\x04\x00\x02"
        + FPGA_MAC
        + bytes([192, 168, 1, 100])
        + host
        + bytes([192, 168, 1, 10])
    )
    parsed = parse_frame(build_frame(host, FPGA_MAC, arp_payload, ARP_ETHERTYPE))

    assert is_arp_reply_to(parsed, host, "192.168.1.10")
