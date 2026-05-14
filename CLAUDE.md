# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Board and Flow

Target hardware is a verified Digilent Arty A7-**100T** (`xc7a100t_0`, build part `xc7a100ticsg324-1L`, cable `Digilent/210319BE1CD4A`). Do not assume 35T. If hardware changes, re-probe with `make hw-server HW_PORT=3124` then `make probe-board HW_PORT=3124` in a second terminal.

The flow is Vitis HLS C++ → Verilog RTL → Vivado place/route → bitstream. There is no MicroBlaze, no soft CPU, no firmware ELF, and `v++` is not used. Do not convert this into a software/SoC project unless explicitly asked. New FPGA behavior should be implemented as HLS C++ under `hls_blink/src/` with a C testbench under `hls_blink/tb/`; reserve handwritten Verilog for thin top-level wrappers, clock/reset plumbing, or cases where HLS is demonstrably unsuitable (document the reason).

## Toolchain

Xilinx tools live at `/home/hbina085/Xilinx/2025.2`. The Makefile invokes them as:

- HLS synthesis: `Vitis/bin/vitis-run --mode hls --tcl ...` (prefer this; direct `vitis_hls` is awkward in this install)
- Vivado implementation/programming: `Vivado/bin/vivado -mode batch -source ...`
- JTAG hardware server: `Vitis/bin/hw_server`

## Common Commands

```sh
make hls                                    # HLS C-sim + C-synth for both HLS blocks
make hls-bit                                # Full bitstream: HLS then Vivado P&R → hls_blink/build/hls_blink.bit
make hw-server HW_PORT=3124                 # Start hw_server (run in its own terminal)
make probe-board HW_PORT=3124               # Verify xc7a100t target exists
make program-vivado HW_PORT=3124            # Program the FPGA (checks for xc7a100t first)
make clean                                  # Remove build/, hls_blink/build/, .Xil/, vivado logs
make bit                                    # Older RTL-only blink (rtl/blink.v) — reference only
PART=xc7a35ticsg324-1L make hls-bit         # Override part for an Arty A7-35T
```

`HW_PORT` defaults to 3121 in the Makefile but README/AGENTS examples use 3124 — pick whatever is free on the host. Running a single HLS testbench is done via the same `make hls` (both blocks are synthesized/simulated by `hls_blink/scripts/run_hls.tcl`); to isolate one, edit that script.

After programming, verify the L2 endpoint end-to-end with:

```sh
sudo scripts/send_broadcast_eth.py eno1 --listen --message ping       # listen + unicast
sudo scripts/verify_l2_endpoint.sh eno1                                # tcpdump-based PASS/FAIL
```

## Architecture

There are two HLS blocks and a Verilog wrapper that ties them to the Arty board:

- `hls_blink/src/blink_hls.cpp` — runs on the **100 MHz** system clock. Drives the 4 user LEDs from frame events: `led[0]` is an activity holdoff, `led[1..3]` are low bits of a frame counter.
- `hls_blink/src/ethernet_l2_endpoint_hls.cpp` — runs on the **25 MHz** Ethernet MII clock (`create_clock -period 40` in `run_hls.tcl`). A minimal custom Layer-2 endpoint with MAC `02:00:00:00:00:01` and EtherType `0x88B5`. Periodically broadcasts `ARTY_BEACON`, and replies `ARTY_ACK` to valid unicast or broadcast custom frames. Includes its own preamble/SFD, FCS (CRC-32 in `crc32_next_byte`), and IFG; frames are padded to the 60-byte minimum.
- `hls_blink/src/hls_top.v` — top wrapper. Generates the 25 MHz `eth_ref_clk` via `PLLE2_BASE` → `BUFG` → `ODDR`, ties `eth_rstn` to PLL `LOCKED`, instantiates both HLS modules, and **crosses the 25 MHz → 100 MHz clock domain** using 3-stage toggle synchronizers on `rx_accept_toggle`/`tx_frame_toggle` plus a 2-stage activity sync. `frame_event` into `blink_hls` is the XOR of adjacent toggle-sync stages.

The XDC at `constraints/arty_a7.xdc` defines `sys_clk_pin` (100 MHz) and `eth_rx_clk_pin` (40 ns / 25 MHz) and marks them asynchronous — any CDC must stay safe under that assumption.

The build runs HLS first (`run_hls.tcl` produces RTL under `hls_blink/build/hls/<project>/solution1/syn/verilog/`), then `build_hls_bitstream.tcl` reads those .v files + `hls_top.v` + the XDC and runs synth/opt/place/route/bitstream. Outputs land in `hls_blink/build/` (`hls_blink.bit`, `timing_summary.rpt`, `utilization.rpt`).

## Generated Files — Do Not Edit

`hls_blink/build/`, `build/`, `.Xil/`, `logs/`, `vivado*.jou`, `vivado*.log`, `clockInfo.txt`. Edit source TCL/C++/Verilog/XDC and rebuild instead. These are also gitignored.

## Other Notes

`rtl/blink.v` is the older handwritten Verilog example, kept as reference. `scripts/program_hw.tcl` (XSCT) works but `scripts/program_vivado_hw.tcl` (Hardware Manager) gives better diagnostics and verifies an `xc7a100t` device is present before flashing — prefer it.
