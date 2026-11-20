# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Board and Flow

Target hardware is a verified Digilent Arty A7-**100T** (`xc7a100t_0`, build part `xc7a100ticsg324-1L`, cable `Digilent/210319BE1CD4A`). Do not assume 35T. If hardware changes, re-probe with `python3 build.py hw-server --hw-port 3124` then `python3 build.py probe --hw-port 3124` in a second terminal.

The flow is bitlogic Rust → generated Verilog → Vivado place/route → bitstream. There is no MicroBlaze, no soft CPU, no firmware ELF, and `v++` is not used. Do not convert this into a software/SoC project unless explicitly asked. New FPGA behavior belongs in `bitlogic_ethernet/src/` as `#[logic]` functions, with tests under `bitlogic_ethernet/tests/`; reserve handwritten Verilog (`bitlogic_ethernet/rtl/arty_top.v`) for clocks, resets and clock-domain crossings.

The earlier Vitis HLS C++ design (`hls_ethernet/`), the RHDL experiment and the handwritten `rtl/status_led.v` example were removed on 2026-11-20. Do not reintroduce them; they remain in git history.

## Toolchain

Xilinx tools on this machine live at `/mnt/extra_pool/Xilinx/2025.2`. `build.py` defaults to `~/Xilinx/2025.2`, so pass `--xilinx-root` or set `XILINX_ROOT`. The Python CLI invokes:

- RTL generation: `cargo run --release --locked --manifest-path bitlogic_ethernet/Cargo.toml --bin generate`
- Vivado implementation/programming: `Vivado/bin/vivado -mode batch -source ...`
- JTAG hardware server: `Vitis/bin/hw_server`

The design depends on the sibling `../bitlogic` checkout.

## Common Commands

```sh
python3 build.py build                       # Generate RTL, then Vivado P&R → bitlogic_ethernet/build/bitlogic_ethernet.bit
python3 build.py hw-server --hw-port 3124    # Start hw_server (run in its own terminal)
python3 build.py probe --hw-port 3124        # Verify xc7a100t target exists
python3 build.py program --hw-port 3124      # Program the FPGA (checks for xc7a100t first)
python3 build.py test                        # Board-independent Python tests
python3 build.py clean                       # Remove build/, bitlogic_ethernet/build/, .Xil/, vivado logs
python3 build.py build --part xc7a35ticsg324-1L   # Override part for an Arty A7-35T
cargo test --release --locked --manifest-path bitlogic_ethernet/Cargo.toml   # Rust + xsim tests
```

`--hw-port` defaults to 3124 in the Python CLI. Pick a different free port if needed.

After programming, verify the endpoint end-to-end with:

```sh
sudo scripts/send_broadcast_eth.py eno1 --listen --message ping       # listen + unicast
sudo scripts/verify_l2_endpoint.sh eno1                                # tcpdump-based PASS/FAIL
sudo python3 build.py test --hardware --iface eno1                     # hardware pytest suite
```

## Architecture

See `bitlogic_ethernet/README.md` for the full description. In short:

- `src/rx.rs`, `src/proto.rs`, `src/tx.rs` — the Ethernet units on the **25 MHz** MII clock: RX assembly and FCS check, protocol handling (ARP, ICMP echo, peer cache, timers, beacons), TX framing with CRC-32 and IFG.
- `src/status.rs` — LED status on the **100 MHz** system clock.
- `src/core.rs` — a `bitlogic::system!` declaration that wires units, RAMs and the template ROM, and generates both the `eth_core` Verilog and the Rust cycle model.
- `rtl/arty_top.v` — handwritten top: PLL, 25 MHz PHY reference clock, resets, and the 25 MHz → 100 MHz toggle synchronizers.

The endpoint uses MAC `02:00:00:00:00:01` and IP `192.168.1.100`. The XDC at `constraints/arty_a7.xdc` defines `sys_clk_pin` (100 MHz) and `eth_rx_clk_pin` (40 ns / 25 MHz) and marks them asynchronous; any CDC must stay safe under that assumption.

## Generated Files — Do Not Edit

`bitlogic_ethernet/build/`, `bitlogic_ethernet/target/`, `build/`, `.Xil/`, `logs/`, `vivado*.jou`, `vivado*.log`, `clockInfo.txt`. Edit the Rust, Verilog, Tcl or XDC sources and rebuild instead.

## Other Notes

`scripts/program_hw.tcl` (XSCT) works but `scripts/program_vivado_hw.tcl` (Hardware Manager) gives better diagnostics and verifies an `xc7a100t` device is present before flashing — prefer it.
