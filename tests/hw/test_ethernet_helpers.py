import pytest

from .ethernet import (
    ARTY_ACK,
    BROADCAST_MAC,
    CUSTOM_ETHERTYPE,
    FPGA_MAC,
    MIN_ETH_PAYLOAD,
    build_broadcast_probe,
    build_frame,
    build_unicast_probe,
    build_wrong_destination_probe,
    build_wrong_ethertype_probe,
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
    frame = build_frame(FPGA_MAC, src, ARTY_ACK, CUSTOM_ETHERTYPE)
    parsed = parse_frame(frame)

    assert len(frame) == 14 + MIN_ETH_PAYLOAD
    assert parsed.dst_mac == FPGA_MAC
    assert parsed.src_mac == src
    assert parsed.ethertype == CUSTOM_ETHERTYPE
    assert parsed.payload == ARTY_ACK


def test_probe_builders_set_expected_destination_and_ethertype():
    src = parse_mac("0a:0b:0c:0d:0e:0f")

    assert parse_frame(build_unicast_probe(src)).dst_mac == FPGA_MAC
    assert parse_frame(build_broadcast_probe(src)).dst_mac == BROADCAST_MAC
    assert parse_frame(build_wrong_ethertype_probe(src)).ethertype != CUSTOM_ETHERTYPE
    assert parse_frame(build_wrong_destination_probe(src)).dst_mac not in (FPGA_MAC, BROADCAST_MAC)
