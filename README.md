# Arty A7-100T bitlogic Ethernet Endpoint

This repository contains a command-line FPGA project for a Digilent Arty A7-100T. The design is written in Rust with [bitlogic](https://github.com/hbina/bitlogic), which generates Verilog. Vivado implements that Verilog, and the bitstream is programmed over USB-JTAG.

The verified board is:

```text
Cable:  Digilent/210319BE1CD4A
Device: xc7a100t_0
Part:   xc7a100t
Build:  xc7a100ticsg324-1L
```

The project is a pure hardware design. It does not use MicroBlaze, a soft CPU, firmware, lwIP, DHCP, TCP, or `v++`.

## Design Overview

The design lives under `bitlogic_ethernet/`. Its behavior is written as
bitlogic pure next-state functions in Rust. The core's units, RAMs and
template ROM are wired by a `bitlogic::system!` declaration that generates
both the Verilog and the Rust cycle model the tests run. Clocks, resets and
clock-domain crossings are handwritten Verilog in
`bitlogic_ethernet/rtl/arty_top.v`. See
[bitlogic_ethernet/README.md](bitlogic_ethernet/README.md) for the block
diagram, verification and implementation results.

- `bitlogic_ethernet/` contains the design, its generator and its Rust tests.
- `constraints/arty_a7.xdc` contains board constraints.
- `tests/hw/` contains hardware-in-the-loop pytest tests.

The endpoint uses MAC `02:00:00:00:00:01` and IP `192.168.1.100`. It:

- replies to valid ARP requests for `192.168.1.100`;
- announces its IP/MAC mapping with a broadcast gratuitous ARP reply every 5 seconds;
- sends a broadcast diagnostics beacon on EtherType `0x88B5` every second;
- checks the FCS of received frames;
- answers ICMP echo requests (`ping 192.168.1.100`);
- learns ARP peers into a 16-entry cache;
- handles the IEX DEEP market-data feed (UDP multicast `233.215.21.4:10378`):
  keeps a price-level order book per symbol in block RAM and sends the best
  bid and offer (EtherType `0x88B6`) after each atomic book update. Verified
  in simulation and on the board (2026-11-21) against
  `iex_data_broadcast.pcap`: replayed at full 100 Mb/s line rate, all 99,474 BBO
  records match a reference book (`sudo scripts/board_bringup.sh eno1`).

The design requires Rust/Cargo and the sibling `../bitlogic` checkout.

An earlier Vitis HLS C++ implementation (`hls_ethernet/`) and a handwritten
Verilog LED example (`rtl/status_led.v`) were removed on 2026-11-20. They
remain in git history.

Generated outputs are written under `bitlogic_ethernet/build/`, `bitlogic_ethernet/target/`, `.Xil/`, and Vivado log/journal files. Do not hand-edit generated output; edit the Rust, Verilog, Tcl or XDC sources and rebuild.

## Toolchain

Set `XILINX_ROOT` to your Xilinx/Vivado 2025.2 installation directory (default `~/Xilinx/2025.2`). The Python CLI uses it to locate Vivado, `hw_server`, XSCT, and related tools.

For example:

```sh
python3 build.py build --xilinx-root /opt/Xilinx/2025.2
```

Important defaults:

```text
--part xc7a100ticsg324-1L
--hw-port 3124
--iface eno1
--bitfile bitlogic_ethernet/build/bitlogic_ethernet.bit
```

Use Python 3 and pass options after the subcommand. Run `python3 build.py --help` or
`python3 build.py build --help` for details. Override defaults when needed, for example:

```sh
python3 build.py build --part xc7a35ticsg324-1L
python3 build.py test --hardware --iface enp3s0
python3 build.py program --hw-port 3125
```

## Python CLI

### Build

```sh
python3 build.py build
python3 build.py build --dry-run
```

Generates the core's Verilog from Rust into `bitlogic_ethernet/build/rtl/`,
then runs Vivado implementation through
`bitlogic_ethernet/scripts/build_bitstream.tcl`. The output bitstream is:

```text
bitlogic_ethernet/build/bitlogic_ethernet.bit
```

`--dry-run` previews any command without executing it. Invoke the script by its path from any directory; relative paths
passed as options are resolved from your current directory. Builds do not program
the board. Board programming requires the explicit `program` command or
`test --hardware --program`.

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
python3 build.py program --method xsct
python3 build.py list-targets --hw-port 3124
```

Program through the older XSCT script, or list hardware targets through XSCT.

### Testing

```sh
cargo test --release --locked --manifest-path bitlogic_ethernet/Cargo.toml
```

Runs the design's Rust tests: unit functions, the generated cycle model, and
Vivado xsim checks of the generated Verilog (set
`BITLOGIC_ETHERNET_SKIP_HDL=1` to skip the xsim tests).

```sh
python3 build.py test
```

Runs the board-independent CLI and Ethernet helper tests. Install `pytest` in the
Python environment first. The CLI itself uses only the Python standard library.

```sh
python3 build.py test --hardware --iface eno1 --hw-port 3124
```

Runs the hardware pytest suite in `tests/hw/` against an already-programmed board. Raw Ethernet tests generally require `sudo` or `CAP_NET_RAW`.

```sh
sudo python3 build.py test --hardware --program --iface eno1 --hw-port 3124
```

Builds the bitstream, programs the board, then runs the hardware pytest suite. Use this for end-to-end board verification after meaningful design changes.

Manual Ethernet probing is also available:

```sh
sudo scripts/send_broadcast_eth.py eno1 --listen --message ping
sudo scripts/send_broadcast_eth.py eno1 --broadcast --listen --message ping
sudo scripts/verify_l2_endpoint.sh eno1
```

The board's gratuitous ARP lets a normal host learn that `192.168.1.100` is at
`02:00:00:00:00:01`:

```sh
tcpdump -ni eno1 arp
ip neigh show dev eno1
ping 192.168.1.100
```

### Cleaning

```sh
python3 build.py clean
```

Removes generated build output and Vivado logs:

```text
.Xil/
build/
bitlogic_ethernet/build/
vivado*.jou
vivado*.log
vivado_pid*.str
```
