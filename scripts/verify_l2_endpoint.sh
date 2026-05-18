#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:-eno1}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FPGA_MAC="${FPGA_MAC:-02:00:00:00:00:01}"
CAPTURE_SECONDS="${CAPTURE_SECONDS:-8}"
OUT_DIR="${OUT_DIR:-logs/l2_verify}"
STAMP="$(date +%Y%m%d_%H%M%S)"
PCAP="$OUT_DIR/${IFACE}_${STAMP}.pcap"
LOG="$OUT_DIR/${IFACE}_${STAMP}.tcpdump.txt"

mkdir -p "$OUT_DIR"

echo "Interface: $IFACE"
echo "FPGA MAC: $FPGA_MAC"
echo "Capture: $PCAP"
echo "Log: $LOG"

cleanup() {
    if [[ -n "${TCPDUMP_PID:-}" ]] && kill -0 "$TCPDUMP_PID" 2>/dev/null; then
        kill "$TCPDUMP_PID" 2>/dev/null || true
        wait "$TCPDUMP_PID" 2>/dev/null || true
    fi
    if [[ -n "${PCAP_PID:-}" ]] && kill -0 "$PCAP_PID" 2>/dev/null; then
        kill "$PCAP_PID" 2>/dev/null || true
        wait "$PCAP_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "Starting tcpdump. You should see ARP replies and diagnostics beacons once the FPGA link is up."
sudo tcpdump -l -i "$IFACE" -e -n -vv -XX "ether src $FPGA_MAC and (arp or ether proto 0x88b5)" > >(tee "$LOG") 2>&1 &
TCPDUMP_PID=$!

sudo tcpdump -i "$IFACE" -w "$PCAP" "ether src $FPGA_MAC and (arp or ether proto 0x88b5)" >/dev/null 2>&1 &
PCAP_PID=$!

sleep 2

echo "Capturing for ${CAPTURE_SECONDS}s more to catch FPGA ARP announcements and beacons."
sleep "$CAPTURE_SECONDS"

kill "$TCPDUMP_PID" "$PCAP_PID" 2>/dev/null || true
wait "$TCPDUMP_PID" 2>/dev/null || true
wait "$PCAP_PID" 2>/dev/null || true
unset TCPDUMP_PID
unset PCAP_PID

echo
echo "Expected evidence:"
echo "- FPGA gratuitous ARP replies from 02:00:00:00:00:01 for 192.168.1.100"
echo "- FPGA diagnostics beacons on EtherType 0x88b5"
echo
echo "Saved capture files:"
echo "$LOG"
echo "$PCAP"

if grep -qi "$FPGA_MAC" "$LOG"; then
    echo "PASS: saw ARP or diagnostics traffic from FPGA MAC."
else
    echo "FAIL: no frames from FPGA MAC $FPGA_MAC were captured."
    exit 1
fi
