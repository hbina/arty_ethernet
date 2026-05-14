# Arty A7-100T Vitis HLS Blink

This project is a command-line Vitis HLS smoke test for a Digilent Arty A7 board. The connected board was verified through Vivado Hardware Manager as:

```text
Cable:  Digilent/210319BE1CD4A
Device: xc7a100t_0
Part:   xc7a100t
```

The default FPGA part is therefore:

```text
xc7a100ticsg324-1L
```

## What This Uses

The primary flow is Vitis HLS:

1. `hls_blink/src/blink_hls.cpp` is C++ using `ap_uint`.
2. `hls_blink/src/ethernet_l2_endpoint_hls.cpp` is a custom Layer-2 Ethernet endpoint in C++.
3. Vitis HLS synthesizes those C++ blocks into Verilog RTL.
4. `hls_blink/src/hls_top.v` wraps the HLS blocks for the Arty clock, Ethernet pins, and LEDs.
5. Vivado places/routes the generated RTL and writes a bitstream.
6. Vivado Hardware Manager or XSCT programs the FPGA over USB-JTAG.

This is not a MicroBlaze design. There is no soft CPU and no firmware ELF. The C++ becomes hardware.

## Files

```text
hls_blink/src/blink_hls.cpp        HLS C++ top function
hls_blink/src/ethernet_l2_endpoint_hls.cpp
hls_blink/src/hls_top.v            Small Verilog wrapper around HLS RTL
hls_blink/tb/blink_hls_tb.cpp      C simulation testbench
hls_blink/tb/ethernet_l2_endpoint_hls_tb.cpp
hls_blink/scripts/run_hls.tcl      Vitis HLS batch script
hls_blink/scripts/build_hls_bitstream.tcl
constraints/arty_a7.xdc            Arty A7 100 MHz clock and LED pins
scripts/send_broadcast_eth.py      Raw custom EtherType send/listen utility
scripts/probe_vivado_hw.tcl        Discover connected JTAG hardware
scripts/program_vivado_hw.tcl      Program verified xc7a100t target
Makefile                           Command wrappers
```

## Folder Structure

```text
.
├── AGENTS.md
├── Makefile
├── README.md
├── constraints/
├── hls_blink/
│   ├── scripts/
│   ├── src/
│   └── tb/
├── rtl/
├── scripts/
└── build/                         Generated, ignored
```

`hls_blink/` is the main design area. The `src/` directory contains synthesizable HLS C++ source and the small Verilog wrapper used to connect the generated HLS modules to the Arty clock, Ethernet pins, and LEDs. The `tb/` directory contains C simulation testbenches with `main()` functions. The `scripts/` directory inside `hls_blink/` contains the HLS and Vivado build scripts for this design.

`constraints/` contains board-level XDC constraints. These are shared by both the HLS build and the older RTL-only blink flow.

`scripts/` contains general hardware scripts that are not specific to the HLS C++ source. The Vivado scripts probe and program the connected board through Hardware Manager. The XSCT scripts are kept as generic command-line examples, but Vivado Hardware Manager gives better board-identification diagnostics.

`rtl/` contains the older handwritten Verilog blink example. It is useful as a reference, but the primary project flow is the Vitis HLS flow in `hls_blink/`.

`build/`, `hls_blink/build/`, `.Xil/`, `logs/`, `*.jou`, and `*.log` are generated outputs. They are ignored by git and can be recreated with `make hls-bit` or `make bit`.

## Discover The Connected Board

USB enumeration only shows the FTDI/Digilent adapter; it does not prove whether the FPGA is 35T or 100T:

```sh
lsusb
```

To identify the actual FPGA, start `hw_server` in one terminal:

```sh
cd /home/hbina085/Downloads/arty_blink
make hw-server HW_PORT=3124
```

Then probe the JTAG chain from another terminal:

```sh
cd /home/hbina085/Downloads/arty_blink
make probe-board HW_PORT=3124
```

Expected output for this board includes:

```text
DEVICE xc7a100t_0
PART: xc7a100t
```

Use a different `HW_PORT` if `3121` or another port is already in use.

## Build

Build the HLS Verilog, export the HLS IP, and build the FPGA bitstream:

```sh
cd /home/hbina085/Downloads/arty_blink
make hls-bit
```

The generated bitstream is:

```text
hls_blink/build/hls_blink.bit
```

## Custom Ethernet Layer-2 Endpoint

The bitstream includes a minimal custom Ethernet endpoint with no MicroBlaze, lwIP, IP, ARP, DHCP, UDP, or TCP. The FPGA MAC address is:

```text
02:00:00:00:00:01
```

It uses EtherType `0x88B5`, periodically transmits a broadcast `ARTY_BEACON` payload, and replies to valid unicast or broadcast custom frames with `ARTY_ACK`. Frames are padded to the Ethernet minimum payload length and include preamble, SFD, and FCS from the FPGA TX path.

On a Linux host connected through `eno1`, listen for beacons and send a test frame:

```sh
sudo scripts/send_broadcast_eth.py eno1 --listen --message ping
```

Send a broadcast custom frame instead:

```sh
sudo scripts/send_broadcast_eth.py eno1 --broadcast --listen --message ping
```

For an Arty A7-35T instead:

```sh
make hls-bit PART=xc7a35ticsg324-1L
```

For this verified Arty A7-100T:

```sh
make hls-bit PART=xc7a100ticsg324-1L
```

## Install To The FPGA

Start the hardware server:

```sh
make hw-server HW_PORT=3124
```

In another terminal, program the board:

```sh
make program-vivado HW_PORT=3124
```

The programming script checks for an `xc7a100t` target before programming.

## Test

After programming, the Arty user LEDs should blink/change. For command-line verification:

```sh
make hls
```

This runs the HLS C simulation and C synthesis. A successful run reports `CSim done with 0 errors` and writes generated Verilog under:

```text
hls_blink/build/hls/blink_hls_project/solution1/syn/verilog/blink_hls.v
```

To inspect timing/utilization after a full bitstream build:

```sh
less hls_blink/build/timing_summary.rpt
less hls_blink/build/utilization.rpt
```

To verify the programmed custom Layer-2 Ethernet endpoint from Linux, run the
pytest hardware suite on the host interface connected to the Arty Ethernet port:

```sh
python3 -m pytest tests/hw --iface eno1
```

To build the latest HLS bitstream, program the verified `xc7a100t` target through
Vivado Hardware Manager, and then run the same raw-frame assertions:

```sh
python3 -m pytest tests/hw --iface eno1 --program --hw-port 3124
```

This full install/test path rebuilds the HLS bitstream. If the board is already
programmed, skip build/programming and only run the raw Ethernet assertions:

```sh
make test-hw
```

The tests use raw `AF_PACKET` Ethernet sockets. On Linux, the process running
pytest therefore needs `CAP_NET_RAW`. This is a kernel permission; membership in
a normal Unix group is not enough by itself.

First install pytest. On Debian/Ubuntu, the simplest system-wide option is:

```sh
sudo apt install python3-pytest
```

Or install it into a Python virtual environment:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install pytest
```

Then choose one of these permission setups.

Option 1: run pytest with sudo:

```sh
sudo python3 -m pytest tests/hw --iface eno1
sudo python3 -m pytest tests/hw --iface eno1 --program --hw-port 3124
```

If pytest is installed only in a virtual environment, run that interpreter
through sudo instead:

```sh
sudo .venv/bin/python -m pytest tests/hw --iface eno1
sudo .venv/bin/python -m pytest tests/hw --iface eno1 --program --hw-port 3124
```

Option 2: for a shared lab machine, create one dedicated hardware-test Python
that only approved users can execute:

```sh
sudo groupadd --system arty-hw
sudo usermod -aG arty-hw "$USER"
sudo python3 -m venv --copies /opt/arty-hw-test
sudo /opt/arty-hw-test/bin/python -m pip install pytest
sudo chgrp -R arty-hw /opt/arty-hw-test
sudo chmod -R o-rwx /opt/arty-hw-test
sudo setcap cap_net_raw+ep /opt/arty-hw-test/bin/python3
getcap /opt/arty-hw-test/bin/python3
```

Log out and back in so the new group membership takes effect. After that,
approved users can run:

```sh
make test-hw PYTHON=/opt/arty-hw-test/bin/python
make install-test-board PYTHON=/opt/arty-hw-test/bin/python
```

Option 3: for a personal checkout, allow that specific Python interpreter to
open raw sockets:

```sh
sudo setcap cap_net_raw+ep "$(readlink -f .venv/bin/python)"
getcap "$(readlink -f .venv/bin/python)"
```

After that, no sudo is needed:

```sh
make test-hw PYTHON=.venv/bin/python
make install-test-board PYTHON=.venv/bin/python
```

For a different machine, pass whichever interpreter will run pytest as `PYTHON=...`.
For example, a system Python setup could use `PYTHON=/usr/local/bin/python3`.
Avoid setting `CAP_NET_RAW` on a broad system interpreter such as
`/usr/bin/python3` unless that is acceptable for that host.

## Clean

```sh
make clean
```

This removes generated build outputs and Vivado logs.

## Notes

`v++` is not used for this Arty blink project. This project uses `vitis-run --mode hls --tcl` for HLS synthesis and Vivado for implementation/programming.

The older RTL-only blink remains in `rtl/blink.v` and can be built with:

```sh
make bit
```
