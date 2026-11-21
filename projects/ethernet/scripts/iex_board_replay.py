#!/usr/bin/env python3
"""Replay an IEX DEEP capture into the Arty and check its BBO frames.

The board must be freshly programmed (empty order book). The script opens a
raw socket for EtherType 0x88B6 on the interface, replays the capture with
tcpreplay, collects the board's best bid/offer (BBO) frames until they stop,
and compares every record with a reference order book computed here from
the same capture. Needs root (raw sockets, tcpreplay).

    sudo .venv/bin/python projects/ethernet/scripts/iex_board_replay.py --iface eno1 --mbps 20
    sudo .venv/bin/python projects/ethernet/scripts/iex_board_replay.py --iface eno1 --topspeed

Exit status 0 when every record matches and none is missing.
"""

import argparse
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_PCAP = PROJECT_DIR / "iex_data_broadcast.pcap"

BBO_ETHERTYPE = 0x88B6
FPGA_MAC = bytes.fromhex("020000000001")
FEED_GROUP = bytes([233, 215, 21, 4])
FEED_PORT = 10378
DEEP_PROTOCOL = 0x8004

SOL_PACKET = 263
PACKET_STATISTICS = 6
SO_RCVBUFFORCE = 33


def read_pcap(path):
    data = Path(path).read_bytes()
    magic, = struct.unpack_from("<I", data, 0)
    if magic != 0xA1B2C3D4:
        raise SystemExit(f"{path}: not a little-endian microsecond pcap")
    frames = []
    at = 24
    while at + 16 <= len(data):
        _, _, caplen, origlen = struct.unpack_from("<IIII", data, at)
        if caplen != origlen:
            raise SystemExit(f"{path}: truncated capture record at offset {at}")
        frames.append(data[at + 16:at + 16 + caplen])
        at += 16 + caplen
    return frames


def internet_checksum(data):
    total = 0
    for k in range(0, len(data), 2):
        total += (data[k] << 8) | (data[k + 1] if k + 1 < len(data) else 0)
    while total > 0xFFFF:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def frame_plus(frame):
    """PLUs of a well-formed DEEP segment on the feed group and port, else None."""
    if (
        len(frame) < 82
        or frame[12:14] != b"\x08\x00"
        or frame[14] != 0x45
        or frame[23] != 17
        or frame[30:34] != FEED_GROUP
        or struct.unpack_from(">H", frame, 36)[0] != FEED_PORT
        or struct.unpack_from(">H", frame, 20)[0] & 0x3FFF
        or internet_checksum(frame[14:34]) != 0
    ):
        return None
    tp = frame[42:]
    payload_len, count = struct.unpack_from("<HH", tp, 12)
    if tp[0] != 1 or struct.unpack_from("<H", tp, 2)[0] != DEEP_PROTOCOL or 82 + payload_len > len(frame):
        return None
    payload = tp[40:40 + payload_len]
    plus = []
    at = 0
    for _ in range(count):
        length, = struct.unpack_from("<H", payload, at)
        m = payload[at + 2:at + 2 + length]
        if length == 30 and m[0:1] in (b"8", b"5"):
            size, price = struct.unpack_from("<IQ", m, 18)
            plus.append((m[0:1] == b"5", bool(m[1] & 1), bytes(m[10:18]), size, price))
        at += 2 + length
    return plus


def reference(frames):
    """BBO records, in order, from a book that starts empty."""
    books = {}
    records = []
    for frame in frames:
        for sell, event, symbol, size, price in frame_plus(frame) or []:
            sides = books.setdefault(symbol, ({}, {}))
            side = sides[1 if sell else 0]
            if size == 0:
                side.pop(price, None)
            else:
                side[price] = size
            if event:
                bid = max(sides[0]) if sides[0] else None
                ask = min(sides[1]) if sides[1] else None
                records.append((
                    symbol,
                    0 if bid is None else bid, 0 if bid is None else sides[0][bid],
                    0 if ask is None else ask, 0 if ask is None else sides[1][ask],
                ))
    return records


def parse_bbo(frame):
    """(first record sequence, header counters, records) of a BBO frame."""
    if frame[6:12] != FPGA_MAC or frame[12:14] != b"\x88\xb6" or frame[14] != 1:
        return None
    count = frame[15]
    first, packets, gaps, malformed, errors, rx_drops = struct.unpack_from("<6I", frame, 16)
    records = []
    for k in range(count):
        at = 40 + 32 * k
        symbol = bytes(frame[at:at + 8])
        bid_price, ask_price, bid_size, ask_size = struct.unpack_from("<QQII", frame, at + 8)
        records.append((symbol, bid_price, bid_size, ask_price, ask_size))
    counters = dict(packets=packets, gaps=gaps, malformed=malformed, book_errors=errors, rx_drops=rx_drops)
    return first, counters, records


def nic_errors(iface):
    """Receive error counters from `ethtool -S` (frames the NIC discarded)."""
    try:
        out = subprocess.run(["ethtool", "-S", iface], capture_output=True, text=True).stdout
    except OSError:
        return {}
    stats = {}
    for line in out.splitlines():
        name, _, value = line.strip().partition(":")
        if ("err" in name or "drop" in name or "miss" in name) and value.strip().isdigit():
            stats[name] = int(value)
    return stats


def show(record):
    symbol, bid_price, bid_size, ask_price, ask_size = record
    return f"{symbol.decode(errors='replace').rstrip()} {bid_size}@{bid_price} x {ask_size}@{ask_price}"


class Capture(threading.Thread):
    def __init__(self, iface):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(BBO_ETHERTYPE))
        # Python converts the bind protocol to network order itself; 0 keeps
        # the protocol given to socket() above.
        self.sock.bind((iface, 0))
        self.sock.setsockopt(socket.SOL_SOCKET, SO_RCVBUFFORCE, 256 << 20)
        self.frames = []
        self.last = time.monotonic()
        self.stop = threading.Event()

    def drain(self):
        self.sock.setblocking(False)
        try:
            while True:
                self.sock.recv(65535)
        except BlockingIOError:
            pass
        self.sock.getsockopt(SOL_PACKET, PACKET_STATISTICS, 8)  # reset counters

    def run(self):
        while not self.stop.is_set():
            ready, _, _ = select.select([self.sock], [], [], 0.1)
            if not ready:
                continue
            while True:
                try:
                    self.frames.append(self.sock.recv(65535))
                    self.last = time.monotonic()
                except BlockingIOError:
                    break

    def kernel_stats(self):
        return struct.unpack("II", self.sock.getsockopt(SOL_PACKET, PACKET_STATISTICS, 8))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iface", default="eno1")
    parser.add_argument("--pcap", type=Path, default=DEFAULT_PCAP)
    rate = parser.add_mutually_exclusive_group()
    rate.add_argument("--mbps", type=float, help="replay at this rate (tcpreplay --mbps)")
    rate.add_argument("--topspeed", action="store_true", help="replay as fast as the host can send")
    parser.add_argument("--quiet-seconds", type=float, default=3.0,
                        help="stop collecting after this long without a BBO frame")
    args = parser.parse_args()

    frames = read_pcap(args.pcap)
    want = reference(frames)
    print(f"capture: {len(frames)} packets; reference: {len(want)} BBO records")

    nic_before = nic_errors(args.iface)
    cap = Capture(args.iface)
    cap.drain()
    cap.start()

    cmd = ["tcpreplay", "--intf1", args.iface]
    if args.topspeed:
        cmd.append("--topspeed")
        label = "topspeed"
    else:
        mbps = args.mbps if args.mbps else 20.0
        cmd.append(f"--mbps={mbps:g}")
        label = f"{mbps:g} Mb/s"
    cmd.append(str(args.pcap))
    print(f"replaying at {label}: {' '.join(cmd)}", flush=True)
    start = time.monotonic()
    replay = subprocess.run(cmd, capture_output=True, text=True)
    sent_seconds = time.monotonic() - start
    print(replay.stdout.strip())
    if replay.returncode != 0:
        print(replay.stderr.strip(), file=sys.stderr)
        raise SystemExit(f"tcpreplay failed with status {replay.returncode}")
    successful = re.search(r"Successful packets:\s+(\d+)", replay.stdout)
    failed = re.search(r"Failed packets:\s+(\d+)", replay.stdout)

    while time.monotonic() - cap.last < args.quiet_seconds:
        time.sleep(0.1)
    cap.stop.set()
    cap.join()
    kernel_packets, kernel_drops = cap.kernel_stats()
    nic_after = nic_errors(args.iface)
    nic_delta = {k: v - nic_before.get(k, 0) for k, v in nic_after.items() if v != nic_before.get(k, 0)}

    got = {}
    holes = 0
    expected_next = None
    last_counters = None
    foreign = 0
    for frame in cap.frames:
        parsed = parse_bbo(frame)
        if parsed is None:
            foreign += 1
            continue
        first, counters, records = parsed
        if expected_next is not None and first != expected_next:
            holes += 1
        expected_next = first + len(records)
        last_counters = counters
        for k, record in enumerate(records):
            got[first + k] = record

    mismatches = [(k, r) for k, r in sorted(got.items()) if k >= len(want) or want[k] != r]
    missing = [k for k in range(len(want)) if k not in got]
    extra = [k for k in got if k >= len(want)]

    print()
    print(f"replay: {label}, {sent_seconds:.2f} s, tcpreplay successful "
          f"{successful.group(1) if successful else '?'}, failed {failed.group(1) if failed else '?'}")
    print(f"host: {len(cap.frames)} BBO frames received, kernel drops {kernel_drops}, "
          f"sequence holes {holes}, other frames {foreign}")
    print("host NIC receive errors during the run (corrupted frames from the board): "
          + (", ".join(f"{k} {v}" for k, v in nic_delta.items()) or "none"))
    print(f"records: {len(got)} received of {len(want)} expected; "
          f"missing {len(missing)}, mismatched {len(mismatches)}, extra {len(extra)}")
    if got and min(got) != 0:
        print(f"WARNING: first record sequence is {min(got)}, not 0: the board was not freshly programmed")
    if last_counters:
        print("board counters (last frame): " + ", ".join(f"{k} {v}" for k, v in last_counters.items()))
        if last_counters["packets"] != len(frames):
            print(f"  note: board accepted {last_counters['packets']} segments of {len(frames)} sent")
    for k, record in mismatches[:5]:
        print(f"  record {k}: board {show(record)}, reference {show(want[k]) if k < len(want) else '-'}")
    if missing:
        print(f"  first missing records: {missing[:10]}")

    ok = not missing and not mismatches and not extra and len(got) == len(want)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
