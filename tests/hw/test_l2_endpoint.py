from .ethernet import (
    BROADCAST_MAC,
    DIAGNOSTIC_BEACON_ETHERTYPE,
    FPGA_MAC,
    FPGA_IP,
    build_arp_request,
    expect_frame,
    is_beacon_payload,
    is_gratuitous_arp_reply,
    is_arp_reply_to,
    mac_text,
)


def test_periodic_beacon(raw_eth, timeout):
    expect_frame(
        raw_eth,
        timeout,
        lambda frame: (
            frame.src_mac == FPGA_MAC
            and frame.dst_mac == BROADCAST_MAC
            and frame.ethertype == DIAGNOSTIC_BEACON_ETHERTYPE
            and is_beacon_payload(frame.payload)
        ),
        f"diagnostics beacon from {mac_text(FPGA_MAC)} to broadcast",
    )


def test_gratuitous_arp_announcement(raw_eth, timeout):
    expect_frame(
        raw_eth,
        timeout,
        is_gratuitous_arp_reply,
        f"gratuitous ARP reply from {mac_text(FPGA_MAC)} for {FPGA_IP}",
    )


def test_arp_request_gets_unicast_reply(raw_eth, timeout):
    raw_eth.drain(0.1)
    host_ip = "192.168.1.10"
    raw_eth.send(build_arp_request(raw_eth.host_mac, host_ip))
    expect_frame(
        raw_eth,
        min(timeout, 2.0),
        lambda frame: is_arp_reply_to(frame, raw_eth.host_mac, host_ip),
        f"unicast ARP reply from {mac_text(FPGA_MAC)} to host",
    )
