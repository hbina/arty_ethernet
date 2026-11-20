# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Board and Flow

Target hardware is a verified Digilent Arty A7-**100T** (`xc7a100t_0`, build part `xc7a100ticsg324-1L`, cable `Digilent/210319BE1CD4A`). Do not assume 35T. If hardware changes, re-probe with `python3 build.py hw-server --hw-port 3124` then `python3 build.py probe --hw-port 3124` in a second terminal.

The flow is Vitis HLS C++ → Verilog RTL → Vivado place/route → bitstream. There is no MicroBlaze, no soft CPU, no firmware ELF, and `v++` is not used. Do not convert this into a software/SoC project unless explicitly asked. New FPGA behavior should be implemented as HLS C++ under `hls_ethernet/src/` with a C testbench under `hls_ethernet/tb/`; reserve handwritten Verilog for thin top-level wrappers, clock/reset plumbing, or cases where HLS is demonstrably unsuitable (document the reason).

## Toolchain

Xilinx tools live at `/home/hbina085/Xilinx/2025.2`. The Python CLI invokes them as:

- HLS synthesis: `Vitis/bin/vitis-run --mode hls --tcl ...` (prefer this; direct `vitis_hls` is awkward in this install)
- Vivado implementation/programming: `Vivado/bin/vivado -mode batch -source ...`
- JTAG hardware server: `Vitis/bin/hw_server`

## Common Commands

```sh
python3 build.py hls                                    # HLS C-sim + C-synth for both HLS blocks
python3 build.py build                                # Full bitstream: HLS then Vivado P&R → hls_ethernet/build/hls_ethernet.bit
python3 build.py hw-server --hw-port 3124                 # Start hw_server (run in its own terminal)
python3 build.py probe --hw-port 3124               # Verify xc7a100t target exists
python3 build.py program --hw-port 3124            # Program the FPGA (checks for xc7a100t first)
python3 build.py clean                                  # Remove build/, hls_ethernet/build/, .Xil/, vivado logs
python3 build.py build --design rtl                                    # Older RTL-only status_led (rtl/status_led.v) — reference only
python3 build.py build --part xc7a35ticsg324-1L         # Override part for an Arty A7-35T
```

`--hw-port` defaults to 3124 in the Python CLI. Pick a different free port if needed. Running a single HLS testbench is done via the same `python3 build.py hls` (both blocks are synthesized/simulated by `hls_ethernet/scripts/run_hls.tcl`); to isolate one, edit that script.

After programming, verify the L2 endpoint end-to-end with:

```sh
sudo scripts/send_broadcast_eth.py eno1 --listen --message ping       # listen + unicast
sudo scripts/verify_l2_endpoint.sh eno1                                # tcpdump-based PASS/FAIL
```

## Architecture

There are two HLS blocks and a Verilog wrapper that ties them to the Arty board:

- `hls_ethernet/src/ethernet_status_hls.cpp` — runs on the **100 MHz** system clock. Drives the 4 user LEDs from frame events: `led[0]` is an activity holdoff, `led[1..3]` are low bits of a frame counter.
- `hls_ethernet/src/ethernet_l2_endpoint_hls.cpp` — runs on the **25 MHz** Ethernet MII clock (`create_clock -period 40` in `run_hls.tcl`). A minimal custom Layer-2 endpoint with MAC `02:00:00:00:00:01`, diagnostics beacons on EtherType `0x88B5`, and ARP replies for `192.168.1.100`. Custom probes and IPv4/UDP packets are consumed for diagnostics counters but do not generate ACK replies. Includes its own preamble/SFD, FCS (CRC-32 in `crc32_next_byte`), and IFG; frames are padded to the 60-byte minimum.
- `hls_ethernet/src/hls_top.v` — top wrapper. Generates the 25 MHz `eth_ref_clk` via `PLLE2_BASE` → `BUFG` → `ODDR`, ties `eth_rstn` to PLL `LOCKED`, instantiates both HLS modules, and **crosses the 25 MHz → 100 MHz clock domain** using 3-stage toggle synchronizers on `rx_accept_toggle`/`tx_frame_toggle` plus a 2-stage activity sync. `frame_event` into `ethernet_status_hls` is the XOR of adjacent toggle-sync stages.

The XDC at `constraints/arty_a7.xdc` defines `sys_clk_pin` (100 MHz) and `eth_rx_clk_pin` (40 ns / 25 MHz) and marks them asynchronous — any CDC must stay safe under that assumption.

The build runs HLS first (`run_hls.tcl` produces RTL under `hls_ethernet/build/hls/<project>/solution1/syn/verilog/`), then `build_hls_bitstream.tcl` reads those .v files + `hls_top.v` + the XDC and runs synth/opt/place/route/bitstream. Outputs land in `hls_ethernet/build/` (`hls_ethernet.bit`, `timing_summary.rpt`, `utilization.rpt`).

## bitlogic Implementation

`bitlogic_ethernet/` is a parallel implementation of the same endpoint, requested explicitly by the user, using the sibling `../bitlogic` Rust DSL. It extends the HLS behavior with receive FCS checking and ICMP echo replies. Behavior lives in pure `#[logic]` step functions (`src/rx.rs`, `proto.rs`, `tx.rs`, `status.rs`) that call shared `#[logic]` helpers in `src/ops.rs` (bitlogic inlines each call); handwritten Verilog in `bitlogic_ethernet/rtl/` owns RAMs, unit wiring (`eth_core.v`), clocks, resets and CDC (`arty_top.v`). `src/sim.rs` is a cycle-accurate model of `eth_core.v`; keep the two in sync — `tests/hdl.rs` checks them cycle-for-cycle in xsim. Build with `python3 build.py build --design bitlogic`; test with `cargo test --release --locked --manifest-path bitlogic_ethernet/Cargo.toml`. Generated Verilog goes to `bitlogic_ethernet/build/rtl/` (do not edit).

## Generated Files — Do Not Edit

`hls_ethernet/build/`, `build/`, `.Xil/`, `logs/`, `vivado*.jou`, `vivado*.log`, `clockInfo.txt`. Edit source TCL/C++/Verilog/XDC and rebuild instead. These are also gitignored.

## Other Notes

`rtl/status_led.v` is the older handwritten Verilog example, kept as reference. `scripts/program_hw.tcl` (XSCT) works but `scripts/program_vivado_hw.tcl` (Hardware Manager) gives better diagnostics and verifies an `xc7a100t` device is present before flashing — prefer it.
