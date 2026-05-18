import binascii
import fcntl
import select
import socket
import struct
import time
from dataclasses import dataclass


FPGA_MAC = bytes.fromhex("020000000001")
BROADCAST_MAC = bytes.fromhex("ffffffffffff")
ARP_ETHERTYPE = 0x0806
IPV4_ETHERTYPE = 0x0800
DIAGNOSTIC_BEACON_ETHERTYPE = 0x88B5
ARTY_BEACON_PREFIX = b"ARTY IP=192.168.001.100 MAC=020000000001 "
FPGA_IP = "192.168.1.100"

ETH_P_ALL = 0x0003
MIN_ETH_PAYLOAD = 46
SIOCGIFHWADDR = 0x8927


@dataclass(frozen=True)
class ParsedFrame:
    dst_mac: bytes
    src_mac: bytes
    ethertype: int
    payload: bytes
    raw: bytes


def mac_text(mac):
    return ":".join(f"{octet:02x}" for octet in mac)


def parse_mac(text):
    parts = text.split(":")
    if len(parts) != 6:
        raise ValueError(f"MAC address must have 6 bytes: {text!r}")
    return bytes(int(part, 16) for part in parts)


def host_mac(iface):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as control:
        ifreq = struct.pack("256s", iface[:15].encode("utf-8"))
        try:
            info = fcntl.ioctl(control.fileno(), SIOCGIFHWADDR, ifreq)
        except OSError as exc:
            raise OSError(f"could not read MAC address for interface {iface!r}: {exc}") from exc
    return info[18:24]


def build_frame(dst_mac, src_mac, payload=b"", ethertype=IPV4_ETHERTYPE):
    if isinstance(payload, str):
        payload = payload.encode("ascii")
    padded = payload
    if len(padded) < MIN_ETH_PAYLOAD:
        padded += b"\x00" * (MIN_ETH_PAYLOAD - len(padded))
    return dst_mac + src_mac + struct.pack("!H", ethertype) + padded


def build_arp_request(src_mac, src_ip, target_ip=FPGA_IP):
    src_ip_bytes = socket.inet_aton(src_ip)
    target_ip_bytes = socket.inet_aton(target_ip)
    payload = (
        struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1)
        + src_mac
        + src_ip_bytes
        + (b"\x00" * 6)
        + target_ip_bytes
    )
    return build_frame(BROADCAST_MAC, src_mac, payload, ARP_ETHERTYPE)


def parse_frame(frame):
    if len(frame) < 14:
        raise ValueError(f"short Ethernet frame: {len(frame)} bytes")
    return ParsedFrame(
        dst_mac=frame[0:6],
        src_mac=frame[6:12],
        ethertype=struct.unpack("!H", frame[12:14])[0],
        payload=frame[14:].rstrip(b"\x00"),
        raw=frame,
    )


def parse_arp_payload(payload):
    if len(payload) < 28:
        return None
    hw_type, proto_type, hw_len, proto_len, opcode = struct.unpack("!HHBBH", payload[:8])
    if hw_type != 1 or proto_type != 0x0800 or hw_len != 6 or proto_len != 4:
        return None
    return {
        "opcode": opcode,
        "sender_mac": payload[8:14],
        "sender_ip": socket.inet_ntoa(payload[14:18]),
        "target_mac": payload[18:24],
        "target_ip": socket.inet_ntoa(payload[24:28]),
    }


def short_hex(frame, limit=64):
    data = frame[:limit]
    suffix = "..." if len(frame) > limit else ""
    return binascii.hexlify(data, sep=" ").decode("ascii") + suffix


def describe_frame(parsed):
    return (
        f"{mac_text(parsed.src_mac)} -> {mac_text(parsed.dst_mac)} "
        f"ethertype=0x{parsed.ethertype:04x} payload={parsed.payload!r}"
    )


def is_gratuitous_arp_reply(parsed):
    if (
        parsed.src_mac != FPGA_MAC
        or parsed.dst_mac != BROADCAST_MAC
        or parsed.ethertype != ARP_ETHERTYPE
    ):
        return False
    arp = parse_arp_payload(parsed.payload)
    return (
        arp is not None
        and arp["opcode"] == 2
        and arp["sender_mac"] == FPGA_MAC
        and arp["sender_ip"] == FPGA_IP
        and arp["target_mac"] == BROADCAST_MAC
        and arp["target_ip"] == FPGA_IP
    )


def is_arp_reply_to(parsed, dst_mac, dst_ip):
    if parsed.src_mac != FPGA_MAC or parsed.dst_mac != dst_mac or parsed.ethertype != ARP_ETHERTYPE:
        return False
    arp = parse_arp_payload(parsed.payload)
    return (
        arp is not None
        and arp["opcode"] == 2
        and arp["sender_mac"] == FPGA_MAC
        and arp["sender_ip"] == FPGA_IP
        and arp["target_mac"] == dst_mac
        and arp["target_ip"] == dst_ip
    )


def is_beacon_payload(payload):
    if not payload.startswith(ARTY_BEACON_PREFIX):
        return False
    fields = payload.decode("ascii", errors="ignore").split()
    expected_keys = ("ARTY", "IP", "MAC", "RX", "RXQ", "RXP", "TXD", "ARP", "UDP", "UP")
    if len(fields) != len(expected_keys) or fields[0] != "ARTY":
        return False

    values = {}
    for field in fields[1:]:
        if "=" not in field:
            return False
        key, value = field.split("=", 1)
        values[key] = value

    if tuple(["ARTY"] + list(values.keys())) != expected_keys:
        return False
    if values["IP"] != "192.168.001.100" or values["MAC"] != "020000000001":
        return False

    hex_keys = ("RX", "RXQ", "RXP", "TXD", "ARP", "UDP", "UP")
    return all(
        len(values[key]) == 8 and all(byte in b"0123456789ABCDEF" for byte in values[key].encode("ascii"))
        for key in hex_keys
    )


class RawEthernet:
    def __init__(self, iface):
        self.iface = iface
        self.host_mac = host_mac(iface)
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        self.sock.bind((iface, 0))
        self.sock.setblocking(False)

    def close(self):
        self.sock.close()

    def send(self, frame):
        return self.sock.send(frame)

    def recv(self, timeout):
        readable, _, _ = select.select([self.sock], [], [], timeout)
        if not readable:
            return None
        return self.sock.recv(65535)

    def drain(self, seconds=0.2):
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            if self.recv(remaining) is None:
                return


def expect_frame(raw_eth, timeout, predicate, expected):
    deadline = time.monotonic() + timeout
    mismatches = []

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break

        frame = raw_eth.recv(remaining)
        if frame is None:
            break

        try:
            parsed = parse_frame(frame)
        except ValueError:
            continue

        if predicate(parsed):
            return parsed

        if parsed.ethertype in (ARP_ETHERTYPE, DIAGNOSTIC_BEACON_ETHERTYPE) or parsed.src_mac == FPGA_MAC:
            mismatches.append((parsed, frame))
            mismatches = mismatches[-8:]

    details = [
        f"timed out after {timeout:.2f}s waiting for {expected}",
        f"host={mac_text(raw_eth.host_mac)} iface={raw_eth.iface}",
    ]
    if mismatches:
        details.append("recent relevant mismatches:")
        for parsed, frame in mismatches:
            details.append(f"  {describe_frame(parsed)} hex={short_hex(frame)}")
    else:
        details.append("no relevant ARP, beacon, or FPGA-source frames were received")
    raise AssertionError("\n".join(details))
