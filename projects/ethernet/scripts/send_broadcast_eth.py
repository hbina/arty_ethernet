#!/usr/bin/env python3
import argparse
import fcntl
import select
import socket
import struct
import time


SIOCGIFHWADDR = 0x8927
ETH_P_ALL = 0x0003
DEFAULT_ETHERTYPE = 0x0800
DEFAULT_FPGA_MAC = "02:00:00:00:00:01"
MIN_ETH_PAYLOAD = 46


def interface_mac(ifname):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        ifreq = struct.pack("256s", ifname[:15].encode("utf-8"))
        info = fcntl.ioctl(sock.fileno(), SIOCGIFHWADDR, ifreq)
    return info[18:24]


def mac_text(mac):
    return ":".join(f"{octet:02x}" for octet in mac)


def parse_mac(text):
    parts = text.split(":")
    if len(parts) != 6:
        raise argparse.ArgumentTypeError("MAC address must have 6 colon-separated bytes")
    try:
        return bytes(int(part, 16) for part in parts)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("MAC address contains a non-hex byte") from exc


def build_frame(dst_mac, src_mac, ethertype, payload):
    payload = payload.encode("utf-8")
    if len(payload) < MIN_ETH_PAYLOAD:
        payload += b"\x00" * (MIN_ETH_PAYLOAD - len(payload))

    return dst_mac + src_mac + struct.pack("!H", ethertype) + payload


def describe_frame(frame, ethertype):
    if len(frame) < 14:
        return None
    dst = frame[0:6]
    src = frame[6:12]
    frame_ethertype = struct.unpack("!H", frame[12:14])[0]
    if frame_ethertype != ethertype:
        return None
    payload = frame[14:].rstrip(b"\x00")
    try:
        payload_text = payload.decode("ascii", errors="replace")
    except UnicodeDecodeError:
        payload_text = repr(payload)
    return dst, src, frame_ethertype, payload_text


def listen(sock, ethertype, timeout):
    deadline = None if timeout is None else time.monotonic() + timeout
    while True:
        if deadline is None:
            wait_time = None
        else:
            wait_time = deadline - time.monotonic()
            if wait_time <= 0:
                return
        readable, _, _ = select.select([sock], [], [], wait_time)
        if not readable:
            return
        frame = sock.recv(65535)
        parsed = describe_frame(frame, ethertype)
        if parsed is None:
            continue
        dst, src, frame_ethertype, payload_text = parsed
        print(
            f"{mac_text(src)} -> {mac_text(dst)} "
            f"ethertype=0x{frame_ethertype:04x} payload={payload_text!r}"
        )


def main():
    parser = argparse.ArgumentParser(description="Send and listen for raw Ethernet frames.")
    parser.add_argument("interface", help="Interface connected to the Arty Ethernet port")
    parser.add_argument(
        "--listen",
        action="store_true",
        help="Listen for matching frames before and after transmitting",
    )
    parser.add_argument(
        "-m",
        "--message",
        default="arty-a7-broadcast-test",
        help="Payload text to send in each frame",
    )
    parser.add_argument(
        "-c",
        "--count",
        type=int,
        default=1,
        help="Number of frames to send; use 0 to send forever",
    )
    parser.add_argument(
        "-i",
        "--interval",
        type=float,
        default=0.05,
        help="Seconds between frames",
    )
    parser.add_argument(
        "-d",
        "--dest-mac",
        type=parse_mac,
        default=parse_mac(DEFAULT_FPGA_MAC),
        help=f"Destination MAC; default is FPGA MAC {DEFAULT_FPGA_MAC}",
    )
    parser.add_argument(
        "--broadcast",
        action="store_true",
        help="Send to ff:ff:ff:ff:ff:ff instead of --dest-mac",
    )
    parser.add_argument(
        "--listen-timeout",
        type=float,
        default=5.0,
        help="Seconds to listen after sending; use 0 for forever with --listen",
    )
    parser.add_argument(
        "-t",
        "--ethertype",
        type=lambda value: int(value, 0),
        default=DEFAULT_ETHERTYPE,
        help="EtherType value, for example 0x0800",
    )
    args = parser.parse_args()

    src_mac = interface_mac(args.interface)
    dst_mac = b"\xff\xff\xff\xff\xff\xff" if args.broadcast else args.dest_mac
    frame = build_frame(dst_mac, src_mac, args.ethertype, args.message)

    print(
        f"interface={args.interface} local={mac_text(src_mac)} "
        f"dest={mac_text(dst_mac)} ethertype=0x{args.ethertype:04x}"
    )

    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL)) as sock:
        sock.bind((args.interface, 0))

        if args.listen:
            print("listening before send")
            listen(sock, args.ethertype, args.listen_timeout)

        sent = 0
        while args.count == 0 or sent < args.count:
            sock.send(frame)
            sent += 1
            print(f"sent {sent}")
            time.sleep(args.interval)

        if args.listen:
            print("listening after send")
            listen_timeout = None if args.listen_timeout == 0 else args.listen_timeout
            listen(sock, args.ethertype, listen_timeout)


if __name__ == "__main__":
    main()
