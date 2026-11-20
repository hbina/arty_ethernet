#!/usr/bin/env bash
# Arty A7-100T bring-up and IEX feed check, in one run:
#
#   sudo scripts/board_bringup.sh [iface]        (default iface: eno1)
#
# Steps (each logged under logs/board_<timestamp>/, with a summary at the end):
#   1. Program the bitstream (as you, through hw_server) and wait for link.
#   2. Add a temporary host address 192.168.1.10/24 to the interface
#      (removed on exit if this script added it).
#   3. Hardware test suite (tests/hw: ARP, beacons, ICMP echo, peers).
#   4. Kernel ping: 5 normal, 5 full-size (1472 data bytes, DF), and a
#      flood of 5000 full-size echoes.
#   5. IEX capture replay at each rate in RATES, each on a freshly
#      programmed board, compared record by record with a reference book.
#
# Environment overrides: XILINX_ROOT, HW_PORT (3124), BITFILE, PCAP,
# RATES ("20 80 topspeed": Mb/s of frame data, or "topspeed"; 80 Mb/s of
# frames is about 95 Mb/s on the wire for this capture), SKIP_PYTEST=1,
# SKIP_PING=1.
set -uo pipefail

IFACE="${1:-eno1}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HW_PORT="${HW_PORT:-3124}"
BITFILE="${BITFILE:-$REPO/bitlogic_ethernet/build/bitlogic_ethernet.bit}"
PCAP="${PCAP:-$REPO/iex_data_broadcast.pcap}"
RATES="${RATES:-20 80 topspeed}"
HOST_ADDR="192.168.1.10/24"
FPGA_IP="192.168.1.100"
PY="$REPO/.venv/bin/python"
LOG_DIR="$REPO/logs/board_$(date +%Y%m%d_%H%M%S)"

die() { echo "error: $*" >&2; exit 2; }

[[ $EUID -eq 0 ]] || die "run with sudo: sudo $0 $*"
RUN_AS="${SUDO_USER:-}"
[[ -n "$RUN_AS" && "$RUN_AS" != root ]] || die "run through sudo from your own account (needs SUDO_USER)"
USER_HOME="$(getent passwd "$RUN_AS" | cut -d: -f6)"
if [[ -z "${XILINX_ROOT:-}" ]]; then
    for candidate in /mnt/extra_pool/Xilinx/2025.2 "$USER_HOME/Xilinx/2025.2"; do
        [[ -x "$candidate/Vivado/bin/vivado" ]] && XILINX_ROOT="$candidate" && break
    done
fi
[[ -x "${XILINX_ROOT:-}/Vivado/bin/vivado" ]] || die "Vivado not found; set XILINX_ROOT"
[[ -e "/sys/class/net/$IFACE" ]] || die "no interface $IFACE"
[[ -f "$BITFILE" ]] || die "bitstream not found: $BITFILE (run: python3 build.py build)"
[[ -f "$PCAP" ]] || die "capture not found: $PCAP"
[[ -x "$PY" ]] || die "project venv not found: $PY"
command -v tcpreplay >/dev/null || die "tcpreplay not installed (apt install tcpreplay)"
"$PY" -c "import pytest" 2>/dev/null || die "pytest missing from $PY"

mkdir -p "$LOG_DIR"
export PYTHONDONTWRITEBYTECODE=1

# Vivado and hw_server run as the invoking user so nothing in the
# repository ends up owned by root.
as_user() {
    sudo -u "$RUN_AS" env HOME="$USER_HOME" XILINX_ROOT="$XILINX_ROOT" PATH="$PATH" "$@"
}

HW_SERVER_PID=""
ADDED_ADDR=0
cleanup() {
    if [[ -n "$HW_SERVER_PID" ]]; then
        # hw_server runs under sudo -> build.py; stop it by its command line.
        pkill -TERM -u "$RUN_AS" -f "hw_server -s tcp::$HW_PORT" 2>/dev/null
        kill "$HW_SERVER_PID" 2>/dev/null
    fi
    if [[ $ADDED_ADDR -eq 1 ]]; then
        ip addr del "$HOST_ADDR" dev "$IFACE" 2>/dev/null && echo "removed $HOST_ADDR from $IFACE"
    fi
    chown -R "$RUN_AS": "$LOG_DIR" 2>/dev/null
}
trap cleanup EXIT
trap 'exit 130' INT TERM

declare -a SUMMARY=()
record() { SUMMARY+=("$(printf '%-44s %s' "$1" "$2")"); }
step() { echo; echo "=== $* ==="; }

port_open() { (exec 3<>"/dev/tcp/127.0.0.1/$HW_PORT") 2>/dev/null; }

start_hw_server() {
    port_open && return 0
    as_user python3 "$REPO/build.py" hw-server --hw-port "$HW_PORT" \
        --xilinx-root "$XILINX_ROOT" >"$LOG_DIR/hw_server.log" 2>&1 &
    HW_SERVER_PID=$!
    for _ in $(seq 50); do
        port_open && return 0
        sleep 0.2
    done
    echo "hw_server did not start; see $LOG_DIR/hw_server.log"
    return 1
}

wait_link() {
    for _ in $(seq 30); do
        [[ "$(cat "/sys/class/net/$IFACE/carrier" 2>/dev/null)" == 1 ]] && break
        sleep 0.5
    done
    if [[ "$(cat "/sys/class/net/$IFACE/carrier" 2>/dev/null)" != 1 ]]; then
        echo "no link on $IFACE"
        return 1
    fi
    ethtool "$IFACE" 2>/dev/null | grep -E 'Speed|Duplex'
    sleep 2  # let the first beacons and the gratuitous ARP go out
}

# Programs the bitstream and waits for link; $1 names the log.
program_board() {
    local log="$LOG_DIR/program_$1.log"
    (cd "$REPO" && as_user python3 build.py program --hw-port "$HW_PORT" \
        --xilinx-root "$XILINX_ROOT" --bitfile "$BITFILE") >"$log" 2>&1 || {
        echo "programming failed; see $log"
        tail -5 "$log"
        return 1
    }
    grep -E '^Programmed' "$log"
    wait_link
}

cd "$REPO" || exit 2
echo "interface $IFACE, bitstream $BITFILE"
echo "logs in $LOG_DIR"

step "1. Program the board and wait for link"
if start_hw_server && program_board initial; then
    record "1. program + link" PASS
else
    record "1. program + link" FAIL
    printf '%s\n' "${SUMMARY[@]}"
    exit 1
fi

step "2. Host address"
if ip -4 addr show dev "$IFACE" | grep -q "${HOST_ADDR%/*}/"; then
    echo "$HOST_ADDR already on $IFACE"
else
    ip addr add "$HOST_ADDR" dev "$IFACE" && ADDED_ADDR=1 && echo "added $HOST_ADDR to $IFACE (temporary)"
fi
record "2. host address" "$(ip -4 -br addr show dev "$IFACE" | awk '{print $3}')"

if [[ "${SKIP_PYTEST:-0}" != 1 ]]; then
    step "3. Hardware test suite (tests/hw)"
    if "$PY" -m pytest -p no:cacheprovider tests/hw --iface "$IFACE" --timeout 8 -v 2>&1 \
        | tee "$LOG_DIR/pytest.log"; then
        record "3. tests/hw" "PASS ($(grep -Eo '[0-9]+ passed' "$LOG_DIR/pytest.log" | tail -1))"
    else
        record "3. tests/hw" "FAIL ($(tail -1 "$LOG_DIR/pytest.log"))"
    fi
fi

if [[ "${SKIP_PING:-0}" != 1 ]]; then
    step "4. Kernel ping"
    {
        ping -c 5 -W 1 -I "$IFACE" "$FPGA_IP"
        echo
        ping -c 5 -W 1 -s 1472 -M do -I "$IFACE" "$FPGA_IP"
        echo
        ping -f -c 5000 -W 1 -s 1472 -I "$IFACE" "$FPGA_IP"
    } 2>&1 | tee "$LOG_DIR/ping.log"
    loss=$(grep -Eo '[0-9.]+% packet loss' "$LOG_DIR/ping.log" | paste -sd' ' -)
    rtt=$(grep -E '^rtt' "$LOG_DIR/ping.log" | awk -F'= ' '{print $2}' | paste -sd'|' -)
    record "4. ping (5, 5 x 1472 B, flood 5000 x 1472 B)" "loss: $loss"
    record "   rtt min/avg/max/mdev" "$rtt"
fi

for rate in $RATES; do
    step "5. IEX replay at $rate (fresh book)"
    log="$LOG_DIR/replay_$rate.log"
    if ! program_board "$rate"; then
        record "5. replay $rate" "FAIL (programming)"
        continue
    fi
    if [[ "$rate" == topspeed ]]; then
        args=(--topspeed)
    else
        args=(--mbps "$rate")
    fi
    if "$PY" scripts/iex_board_replay.py --iface "$IFACE" --pcap "$PCAP" "${args[@]}" 2>&1 | tee "$log"; then
        status=PASS
    else
        status=FAIL
    fi
    record "5. replay $rate" "$status: $(grep -E '^records:' "$log" | cut -d' ' -f2-)"
    record "   board counters" "$(grep -E '^board counters' "$log" | cut -d: -f2-)"
    record "   host NIC errors" "$(grep -E "^host NIC" "$log" | cut -d: -f2-)"
done

step "Summary"
printf '%s\n' "${SUMMARY[@]}" | tee "$LOG_DIR/summary.txt"
echo "logs: $LOG_DIR"
