#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:-eno1}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ETHERTYPE="${ETHERTYPE:-0x88b5}"
FPGA_MAC="${FPGA_MAC:-02:00:00:00:00:01}"
COUNT="${COUNT:-3}"
INTERVAL="${INTERVAL:-0.25}"
CAPTURE_SECONDS="${CAPTURE_SECONDS:-8}"
OUT_DIR="${OUT_DIR:-logs/l2_verify}"
STAMP="$(date +%Y%m%d_%H%M%S)"
PCAP="$OUT_DIR/${IFACE}_${STAMP}.pcap"
LOG="$OUT_DIR/${IFACE}_${STAMP}.tcpdump.txt"

mkdir -p "$OUT_DIR"

echo "Interface: $IFACE"
echo "EtherType: $ETHERTYPE"
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

echo "Starting tcpdump. You should see ARTY diagnostics beacons once the FPGA link is up."
sudo tcpdump -l -i "$IFACE" -e -n -vv -XX "ether proto $ETHERTYPE" > >(tee "$LOG") 2>&1 &
TCPDUMP_PID=$!

sudo tcpdump -i "$IFACE" -w "$PCAP" "ether proto $ETHERTYPE" >/dev/null 2>&1 &
PCAP_PID=$!

sleep 2

echo "Sending unicast frames to $FPGA_MAC."
sudo python3 "$REPO_DIR/scripts/send_broadcast_eth.py" "$IFACE" \
    --dest-mac "$FPGA_MAC" \
    --ethertype "$ETHERTYPE" \
    --message "host_unicast_ping" \
    --count "$COUNT" \
    --interval "$INTERVAL"

echo "Sending broadcast frames."
sudo python3 "$REPO_DIR/scripts/send_broadcast_eth.py" "$IFACE" \
    --broadcast \
    --ethertype "$ETHERTYPE" \
    --message "host_broadcast_ping" \
    --count "$COUNT" \
    --interval "$INTERVAL"

echo "Capturing for ${CAPTURE_SECONDS}s more to catch FPGA beacons and ACKs."
sleep "$CAPTURE_SECONDS"

kill "$TCPDUMP_PID" "$PCAP_PID" 2>/dev/null || true
wait "$TCPDUMP_PID" 2>/dev/null || true
wait "$PCAP_PID" 2>/dev/null || true
unset TCPDUMP_PID
unset PCAP_PID

echo
echo "Expected evidence:"
echo "- FPGA beacons: 02:00:00:00:00:01 > ff:ff:ff:ff:ff:ff, ethertype 0x88b5, payload ARTY IP=... RX=..."
echo "- FPGA ACKs: 02:00:00:00:00:01 > host MAC, ethertype 0x88b5, payload ARTY_ACK"
echo
echo "Saved capture files:"
echo "$LOG"
echo "$PCAP"

if grep -qi "$FPGA_MAC" "$LOG"; then
    if grep -q "ARTY IP=\\|ARTY_ACK\\|TY IP=\\|TY_ACK" "$LOG"; then
        echo "PASS: saw FPGA custom Layer-2 payloads."
    else
        echo "PARTIAL: saw FPGA MAC, but not ARTY diagnostics or ARTY_ACK payload text."
    fi
else
    echo "FAIL: no frames from FPGA MAC $FPGA_MAC were captured."
    exit 1
fi
