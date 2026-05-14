import binascii
import fcntl
import select
import socket
import struct
import time
from dataclasses import dataclass


FPGA_MAC = bytes.fromhex("020000000001")
BROADCAST_MAC = bytes.fromhex("ffffffffffff")
CUSTOM_ETHERTYPE = 0x88B5
WRONG_ETHERTYPE = 0x88B6
ARTY_BEACON = b"ARTY_BEACON"
ARTY_ACK = b"ARTY_ACK"

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


def build_frame(dst_mac, src_mac, payload=b"pytest_probe", ethertype=CUSTOM_ETHERTYPE):
    if isinstance(payload, str):
        payload = payload.encode("ascii")
    padded = payload
    if len(padded) < MIN_ETH_PAYLOAD:
        padded += b"\x00" * (MIN_ETH_PAYLOAD - len(padded))
    return dst_mac + src_mac + struct.pack("!H", ethertype) + padded


def build_unicast_probe(src_mac, payload=b"pytest_unicast"):
    return build_frame(FPGA_MAC, src_mac, payload, CUSTOM_ETHERTYPE)


def build_broadcast_probe(src_mac, payload=b"pytest_broadcast"):
    return build_frame(BROADCAST_MAC, src_mac, payload, CUSTOM_ETHERTYPE)


def build_wrong_ethertype_probe(src_mac, payload=b"pytest_wrong_type"):
    return build_frame(FPGA_MAC, src_mac, payload, WRONG_ETHERTYPE)


def build_wrong_destination_probe(src_mac, payload=b"pytest_wrong_dest"):
    wrong_dst = bytes.fromhex("0200000000fe")
    return build_frame(wrong_dst, src_mac, payload, CUSTOM_ETHERTYPE)


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


def short_hex(frame, limit=64):
    data = frame[:limit]
    suffix = "..." if len(frame) > limit else ""
    return binascii.hexlify(data, sep=" ").decode("ascii") + suffix


def describe_frame(parsed):
    return (
        f"{mac_text(parsed.src_mac)} -> {mac_text(parsed.dst_mac)} "
        f"ethertype=0x{parsed.ethertype:04x} payload={parsed.payload!r}"
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

        if parsed.ethertype == CUSTOM_ETHERTYPE or parsed.src_mac == FPGA_MAC:
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
        details.append("no relevant custom EtherType or FPGA-source frames were received")
    raise AssertionError("\n".join(details))
