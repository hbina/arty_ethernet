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

Set `XILINX_ROOT` to your Xilinx/Vivado/Vitis 2025.2 installation directory. The Python CLI uses it to locate Vivado, Vitis HLS, `hw_server`, XSCT, and related tools.

For example:

```sh
python3 build.py hls --xilinx-root /opt/Xilinx/2025.2
```

Important defaults:

```text
--part xc7a100ticsg324-1L
--hw-port 3124
--iface eno1
--bitfile hls_ethernet/build/hls_ethernet.bit
```

Use Python 3 and pass options after the subcommand. Run `python3 build.py --help` or
`python3 build.py build --help` for details. Override defaults when needed, for example:

```sh
python3 build.py build --part xc7a35ticsg324-1L
python3 build.py test --hardware --iface enp3s0
python3 build.py program --hw-port 3125
```

## Python CLI

### Main HLS Flow

```sh
python3 build.py build --dry-run
python3 build.py build --skip-hls
```

`--dry-run` previews any command without executing it. `--skip-hls` reuses the
existing generated HLS RTL. Invoke the script by its path from any directory; relative paths
passed as options are resolved from your current directory. Builds do not program
the board. Board programming requires the explicit `program` command or
`test --hardware --program`.

```sh
python3 build.py hls
```

Runs Vitis HLS C simulation and C synthesis for the HLS blocks using `hls_ethernet/scripts/run_hls.tcl`. Generated HLS RTL and IP output are placed under `hls_ethernet/build/hls/`.

```sh
python3 build.py build
```

Runs `python3 build.py hls`, then runs Vivado implementation through `hls_ethernet/scripts/build_hls_bitstream.tcl`. The main output bitstream is:

```text
hls_ethernet/build/hls_ethernet.bit
```

```sh
python3 build.py program --method xsct
```

Programs `hls_ethernet/build/hls_ethernet.bit` through the older XSCT programming script. Prefer `python3 build.py program` for this board because it verifies the target more clearly.

### Board Discovery And Programming

```sh
python3 build.py hw-server --hw-port 3124
```

Starts Xilinx `hw_server`. Run this in its own terminal when probing or programming manually. Use a different `--hw-port` if the port is already busy.

```sh
python3 build.py probe --hw-port 3124
```

Uses Vivado Hardware Manager to inspect the JTAG chain. For the verified board, expect an `xc7a100t` target such as `xc7a100t_0`.

```sh
python3 build.py program --hw-port 3124
```

Programs the selected bitstream with `scripts/program_vivado_hw.tcl`. This is the preferred programming path for the Arty A7-100T because the script checks for an `xc7a100t` target before programming.

```sh
python3 build.py list-targets --hw-port 3124
```

Lists hardware targets through the XSCT helper script.

```sh
python3 build.py program --method xsct --bitfile build/status_led.bit --hw-port 3124
```

Programs the older RTL-only bitstream `build/status_led.bit` through XSCT.

### Testing

```sh
python3 build.py test
```

Runs the board-independent CLI and Ethernet helper tests. Install `pytest` in the
Python environment first. The CLI itself uses only the Python standard library.
Formatting commands additionally require `clang-format` on `PATH`.

```sh
python3 build.py test --hardware --iface eno1 --hw-port 3124
```

Runs the hardware pytest suite in `tests/hw/` against an already-programmed board. Raw Ethernet tests generally require `sudo` or `CAP_NET_RAW`.

```sh
sudo python3 build.py test --hardware --program --iface eno1 --hw-port 3124
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
python3 build.py format --check
```

Runs `clang-format --dry-run --Werror` over HLS C++ and header files.

```sh
python3 build.py format
```

Applies `clang-format` to HLS C++ and header files.

```sh
python3 build.py install-hooks
```

Installs the repository pre-commit hook by symlinking `scripts/pre-commit.sh` into `.git/hooks/pre-commit`.

### Legacy RTL Flow

```sh
python3 build.py build --design rtl
```

Builds the older handwritten RTL status LED example from `rtl/status_led.v` with `scripts/build_bitstream.tcl`. This is kept as a reference path; the main project flow is `python3 build.py build`.

### Miscellaneous

```sh
python3 build.py vpp-version
```

Prints the installed `v++` version. The main project does not use `v++`.

```sh
python3 build.py clean
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
