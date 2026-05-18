# Arty A7-100T Vitis HLS Ethernet Endpoint

This repository contains a command-line FPGA project for a Digilent Arty A7-100T. The design is written primarily in Vitis HLS C++, synthesized to Verilog RTL, implemented with Vivado, and programmed over USB-JTAG.

The verified board is:

```text
Cable:  Digilent/210319BE1CD4A
Device: xc7a100t_0
Part:   xc7a100t
Build:  xc7a100ticsg324-1L
```

The project is a pure hardware design. It does not use MicroBlaze, a soft CPU, firmware, lwIP, DHCP, TCP, or `v++`.

## Design Overview

The primary design lives under `hls_ethernet/`.

- `hls_ethernet/src/ethernet_status_hls.cpp` drives LED status from frame events.
- `hls_ethernet/src/ethernet_l2_endpoint_hls.cpp` implements a small Layer-2 Ethernet endpoint.
- `hls_ethernet/src/hls_top.v` connects the generated HLS blocks to the Arty board clocks, Ethernet pins, reset, and LEDs.
- `constraints/arty_a7.xdc` contains board constraints.
- `tests/hw/` contains hardware-in-the-loop pytest tests.
- `rtl/status_led.v` is an older handwritten Verilog reference design.

The Ethernet endpoint uses MAC `02:00:00:00:00:01`, announces IP `192.168.1.100` with periodic broadcast gratuitous ARP replies, sends periodic broadcast diagnostics beacons on EtherType `0x88B5`, and replies to valid ARP requests for `192.168.1.100`.

Generated outputs are written under `build/`, `hls_ethernet/build/`, `.Xil/`, and Vivado log/journal files. Do not hand-edit generated output; edit source C++/Verilog/Tcl/XDC and rebuild.

## Toolchain

Set `XILINX_ROOT` to your Xilinx/Vivado/Vitis 2025.2 installation directory. The Makefile uses it to locate Vivado, Vitis HLS, `hw_server`, XSCT, and related tools.

For example:

```sh
make hls XILINX_ROOT=/opt/Xilinx/2025.2
```

Important defaults:

```text
PART=xc7a100ticsg324-1L
HW_PORT=3124
IFACE=eno1
BITFILE=hls_ethernet/build/hls_ethernet.bit
```

Override these on the command line when needed, for example:

```sh
make hls-bit PART=xc7a35ticsg324-1L
make test-hw IFACE=enp3s0
make program-vivado HW_PORT=3125
```

## Make Commands

### Main HLS Flow

```sh
make hls
```

Runs Vitis HLS C simulation and C synthesis for the HLS blocks using `hls_ethernet/scripts/run_hls.tcl`. Generated HLS RTL and IP output are placed under `hls_ethernet/build/hls/`.

```sh
make hls-bit
```

Runs `make hls`, then runs Vivado implementation through `hls_ethernet/scripts/build_hls_bitstream.tcl`. The main output bitstream is:

```text
hls_ethernet/build/hls_ethernet.bit
```

```sh
make hls-program
```

Programs `hls_ethernet/build/hls_ethernet.bit` through the older XSCT programming script. Prefer `make program-vivado` for this board because it verifies the target more clearly.

### Board Discovery And Programming

```sh
make hw-server HW_PORT=3124
```

Starts Xilinx `hw_server`. Run this in its own terminal when probing or programming manually. Use a different `HW_PORT` if the port is already busy.

```sh
make probe-board HW_PORT=3124
```

Uses Vivado Hardware Manager to inspect the JTAG chain. For the verified board, expect an `xc7a100t` target such as `xc7a100t_0`.

```sh
make program-vivado HW_PORT=3124
```

Programs `$(BITFILE)` with `scripts/program_vivado_hw.tcl`. This is the preferred programming path for the Arty A7-100T because the script checks for an `xc7a100t` target before programming.

```sh
make list-targets HW_PORT=3124
```

Lists hardware targets through the XSCT helper script.

```sh
make program HW_PORT=3124
```

Programs the older RTL-only bitstream `build/status_led.bit` through XSCT.

### Testing

```sh
make test-hw IFACE=eno1 HW_PORT=3124
```

Runs the hardware pytest suite in `tests/hw/` against an already-programmed board. Raw Ethernet tests generally require `sudo` or `CAP_NET_RAW`.

```sh
sudo make install-test-board IFACE=eno1 HW_PORT=3124
```

Builds the HLS bitstream, programs the board, then runs the hardware pytest suite. Use this for end-to-end board verification after meaningful design changes.

Manual Ethernet probing is also available:

```sh
sudo scripts/send_broadcast_eth.py eno1 --listen --message ping
sudo scripts/send_broadcast_eth.py eno1 --broadcast --listen --message ping
```

The board also announces its fixed IP/MAC mapping at a regular interval with a
broadcast gratuitous ARP reply every 5 seconds:

```sh
tcpdump -ni eno1 arp
ip neigh show dev eno1
```

This lets a normal host learn that `192.168.1.100` is at
`02:00:00:00:00:01`. `ping 192.168.1.100` still requires ICMP echo reply
support, which this hardware endpoint does not implement.

### Formatting And Hooks

```sh
make check-format
```

Runs `clang-format --dry-run --Werror` over HLS C++ and header files.

```sh
make format
```

Applies `clang-format` to HLS C++ and header files.

```sh
make install-hooks
```

Installs the repository pre-commit hook by symlinking `scripts/pre-commit.sh` into `.git/hooks/pre-commit`.

### Legacy RTL Flow

```sh
make bit
```

Builds the older handwritten RTL status LED example from `rtl/status_led.v` with `scripts/build_bitstream.tcl`. This is kept as a reference path; the main project flow is `make hls-bit`.

### Miscellaneous

```sh
make vpp-version
```

Prints the installed `v++` version. The main project does not use `v++`.

```sh
make clean
```

Removes generated build output and Vivado logs:

```text
.Xil/
build/
hls_ethernet/build/
vivado*.jou
vivado*.log
vivado_pid*.str
```
