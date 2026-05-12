# Agent Notes

This folder is a command-line FPGA sample for a verified Digilent Arty A7-100T.

## Board

- Verified JTAG target: `xc7a100t_0`
- Verified part family name from Hardware Manager: `xc7a100t`
- Build part string: `xc7a100ticsg324-1L`
- Cable seen by Vivado: `Digilent/210319BE1CD4A`

Do not assume 35T. If the hardware changes, run:

```sh
make hw-server HW_PORT=3124
make probe-board HW_PORT=3124
```

Use a different `HW_PORT` when a port is already bound.

## Preferred Flow

The requested flow is Vitis HLS C++ to RTL. Do not convert this into a MicroBlaze/software project unless explicitly asked.

Primary commands:

```sh
make hls
make hls-bit
make program-vivado HW_PORT=3124
```

Primary source files:

```text
hls_blink/src/blink_hls.cpp
hls_blink/src/blink_hls_tb.cpp
hls_blink/src/hls_top.v
hls_blink/scripts/run_hls.tcl
hls_blink/scripts/build_hls_bitstream.tcl
constraints/arty_a7.xdc
```

## Tooling

Installed Xilinx tools are under:

```text
/home/hbina085/Xilinx/2025.2
```

Use:

```text
/home/hbina085/Xilinx/2025.2/Vitis/bin/vitis-run --mode hls --tcl ...
/home/hbina085/Xilinx/2025.2/Vivado/bin/vivado -mode batch -source ...
/home/hbina085/Xilinx/2025.2/Vitis/bin/hw_server
```

Direct `vitis_hls` execution is awkward in this install; prefer `vitis-run --mode hls --tcl`.

## Programming

Prefer `scripts/program_vivado_hw.tcl` for this board. It checks that an `xc7a100t` device exists before programming.

`scripts/program_hw.tcl` is an XSCT-based generic programmer and accepts `HW_PORT`, but Vivado Hardware Manager gives better diagnostics.

## Generated Files

Do not hand-edit generated files under:

```text
hls_blink/build/
build/
.Xil/
logs/
```

Edit source TCL/C++/Verilog/XDC instead, then rebuild.
