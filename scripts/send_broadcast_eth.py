#!/usr/bin/env python3
import argparse
import fcntl
import socket
import struct
import time


SIOCGIFHWADDR = 0x8927
ETH_P_ALL = 0x0003
DEFAULT_ETHERTYPE = 0x88B5
MIN_ETH_PAYLOAD = 46


def interface_mac(ifname):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        ifreq = struct.pack("256s", ifname[:15].encode("utf-8"))
        info = fcntl.ioctl(sock.fileno(), SIOCGIFHWADDR, ifreq)
    return info[18:24]


def mac_text(mac):
    return ":".join(f"{octet:02x}" for octet in mac)


def build_frame(src_mac, ethertype, payload):
    payload = payload.encode("utf-8")
    if len(payload) < MIN_ETH_PAYLOAD:
        payload += b"\x00" * (MIN_ETH_PAYLOAD - len(payload))

    dst_mac = b"\xff\xff\xff\xff\xff\xff"
    return dst_mac + src_mac + struct.pack("!H", ethertype) + payload


def main():
    parser = argparse.ArgumentParser(
        description="Send raw broadcast Ethernet frames on a Linux interface."
    )
    parser.add_argument("interface", help="Interface connected to the Arty Ethernet port")
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
        default=100,
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
        "-t",
        "--ethertype",
        type=lambda value: int(value, 0),
        default=DEFAULT_ETHERTYPE,
        help="EtherType value, for example 0x88B5",
    )
    args = parser.parse_args()

    src_mac = interface_mac(args.interface)
    frame = build_frame(src_mac, args.ethertype, args.message)

    print(
        f"Sending {len(frame)} byte broadcast frames on {args.interface} "
        f"from {mac_text(src_mac)} ethertype=0x{args.ethertype:04x}"
    )

    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL)) as sock:
        sock.bind((args.interface, 0))

        sent = 0
        while args.count == 0 or sent < args.count:
            sock.send(frame)
            sent += 1
            if args.count and sent % 100 == 0:
                print(f"sent {sent}")
            time.sleep(args.interval)

    print(f"sent {sent}")


if __name__ == "__main__":
    main()
