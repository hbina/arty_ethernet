# Arty A7-100T bitlogic designs

A test bed for [bitlogic](https://github.com/hbina/bitlogic): FPGA designs for
a Digilent Arty A7-100T, written in Rust with bitlogic, which generates
Verilog. Vivado implements that Verilog from the command line, and the
bitstream is programmed over USB-JTAG. There is no soft CPU, firmware or
`v++`; each design is pure hardware.

The verified board is:

```text
Cable:  Digilent/210319BE1CD4A
Device: xc7a100t_0
Part:   xc7a100t
Build:  xc7a100ticsg324-1L
```

The designs need Rust/Cargo and the sibling `../bitlogic` checkout.

## Projects

| Project | What it is |
|---|---|
| [`ethernet`](projects/ethernet/README.md) | Layer-2 Ethernet endpoint (ARP, ICMP echo, beacons, ARP peer cache) and IEX DEEP feed handler with an on-chip order book and BBO output. Verified on the board. |

An earlier Vitis HLS C++ implementation (`hls_ethernet/`) and a handwritten
Verilog LED example (`rtl/status_led.v`) were removed on 2026-11-20. They
remain in git history.

## Layout

```text
Cargo.toml              Cargo workspace: every crate under projects/ (and crates/, once it exists)
build.py                build, program and test CLI (stdlib only)
scripts/                shared Vivado and JTAG Tcl: the bitstream flow, programming, probing
tests/                  tests of build.py itself
projects/<name>/        one design per directory, a Cargo crate:
    Cargo.toml          [package.metadata.arty] top = "<Vivado top module>"
    src/bin/generate.rs writes the generated Verilog into the directory it is given
    rtl/*.v             handwritten Verilog (clocks, resets, CDC, the top module)
    constraints/*.xdc   pins and clocks this design uses
    scripts/            project scripts; post_route.tcl, if present, runs after routing
    tests/*.rs          Rust tests (cycle model, xsim)
    tests/hw/           pytest board tests; tests using the board get the `hardware` marker
build/<name>/           generated RTL, Vivado project, reports, routed.dcp, <name>.bit
target/                 the workspace's Cargo output
```

Do not hand-edit generated output; edit the Rust, Verilog, Tcl or XDC sources
and rebuild.

### Adding a project

1. `cargo new --lib projects/<name>` (it joins the workspace through the
   `projects/*` glob) and add `bitlogic.workspace = true` under
   `[dependencies]`.
2. Add `[package.metadata.arty] top = "<module>"` to its `Cargo.toml`.
3. Add a `generate` bin that writes the design's generated Verilog files into
   the directory given as its only argument (see
   `projects/ethernet/src/bin/generate.rs`).
4. Put the handwritten top-level Verilog in `rtl/` and the pins it uses in
   `constraints/*.xdc`.
5. `python3 build.py build <name>`.

Code that a second project needs goes into a shared crate under `crates/`;
add `"crates/*"` to the workspace members when the first one is created.

## Toolchain

Set `XILINX_ROOT` to your Xilinx/Vivado 2025.2 installation directory (default
`~/Xilinx/2025.2`), or pass `--xilinx-root`. The CLI uses it to locate Vivado,
`hw_server`, XSCT, and related tools.

Defaults:

```text
--part xc7a100ticsg324-1L
--hw-port 3124
--iface eno1
--bitfile build/<project>/<project>.bit
```

Pass options after the subcommand. Commands that act on a project take its
name; while there is only one project, it can be left out.

```sh
python3 build.py --help
python3 build.py projects                       # list the projects
python3 build.py build ethernet --part xc7a35ticsg324-1L
python3 build.py test ethernet --hardware --iface enp3s0
```

## Python CLI

### Build

```sh
python3 build.py build <project>
python3 build.py build <project> --dry-run
```

Runs the project's `generate` bin into `build/<project>/rtl/` (emptied first),
then `scripts/build_bitstream.tcl`, which reads every `.v` there and in the
project's `rtl/`, every `.xdc` in its `constraints/`, synthesizes the top
module named in its `Cargo.toml`, and writes reports, `routed.dcp` and
`build/<project>/<project>.bit`.

`--dry-run` previews any command without executing it. Invoke the script by
its path from any directory; relative paths passed as options are resolved
from your current directory. Builds do not program the board.

### Board Discovery And Programming

```sh
python3 build.py hw-server --hw-port 3124
```

Starts Xilinx `hw_server`. Run this in its own terminal when probing or
programming manually. Use a different `--hw-port` if the port is busy.

```sh
python3 build.py probe --hw-port 3124
```

Uses Vivado Hardware Manager to inspect the JTAG chain. For the verified
board, expect an `xc7a100t` target such as `xc7a100t_0`.

```sh
python3 build.py program <project> --hw-port 3124
```

Programs `build/<project>/<project>.bit` (or `--bitfile`) with
`scripts/program_vivado_hw.tcl`, which checks for an `xc7a100t` target first.

```sh
python3 build.py program <project> --method xsct
python3 build.py list-targets --hw-port 3124
```

Program through the older XSCT script, or list hardware targets through XSCT.

### Testing

```sh
cargo test --release --locked                   # every project
cargo test --release --locked -p <crate>        # one project
```

Runs the Rust tests: unit functions, the generated cycle models, and Vivado
xsim checks of the generated Verilog (`BITLOGIC_ETHERNET_SKIP_HDL=1` skips the
ethernet project's xsim tests).

```sh
python3 build.py test <project>
```

Runs the CLI tests and the project's board-independent pytest tests. Install
`pytest` in the Python environment first.

```sh
python3 build.py test <project> --hardware --iface eno1 --hw-port 3124
sudo python3 build.py test <project> --hardware --program --iface eno1
```

Runs the project's hardware pytest suite against an already-programmed board,
or builds and programs it first. Raw Ethernet tests generally require `sudo`
or `CAP_NET_RAW`.

### Cleaning

```sh
python3 build.py clean              # all of build/
python3 build.py clean <project>    # build/<project>/ only
```

Also removes `.Xil/` and the Vivado logs and journals in the repository root.
