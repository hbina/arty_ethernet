XILINX_ROOT ?= /home/$(USER)/Xilinx/2025.2
VIVADO ?= $(XILINX_ROOT)/Vivado/bin/vivado
VITIS_LOADER ?= $(XILINX_ROOT)/Vitis/bin/loader
VITIS_HLS ?= $(VITIS_LOADER) -exec vitis_hls
VITIS_RUN ?= $(XILINX_ROOT)/Vitis/bin/vitis-run
VPP ?= $(XILINX_ROOT)/Vitis/bin/v++
HW_SERVER ?= $(XILINX_ROOT)/Vitis/bin/hw_server
XSCT ?= $(XILINX_ROOT)/Vitis/bin/xsct
HW_PORT ?= 3124
IFACE ?= eno1
BITFILE ?= hls_ethernet/build/hls_ethernet.bit
PYTHON ?= python3
PYTEST ?= $(PYTHON) -m pytest

# Arty A7-100T default. For Arty A7-35T use:
#   make hls-bit PART=xc7a35ticsg324-1L
PART ?= xc7a100ticsg324-1L

.PHONY: bit program list-targets hw-server vpp-version clean probe-board program-vivado
.PHONY: hls hls-bit hls-program
.PHONY: test-hw install-test-board
.PHONY: format check-format install-hooks

FORMAT_FILES := $(shell find hls_ethernet/src hls_ethernet/tb -name "*.cpp" -o -name "*.h" -o -name "*.hpp")

format:
	clang-format -i $(FORMAT_FILES)

check-format:
	clang-format --dry-run --Werror $(FORMAT_FILES)

install-hooks:
	ln -sf ../../scripts/pre-commit.sh .git/hooks/pre-commit

bit:
	PART=$(PART) $(VIVADO) -mode batch -source scripts/build_bitstream.tcl

hw-server:
	$(HW_SERVER) -s tcp::$(HW_PORT)

list-targets:
	HW_PORT=$(HW_PORT) $(XSCT) scripts/list_targets.tcl

probe-board:
	HW_PORT=$(HW_PORT) $(VIVADO) -mode batch -source scripts/probe_vivado_hw.tcl

program:
	HW_PORT=$(HW_PORT) $(XSCT) scripts/program_hw.tcl build/status_led.bit

program-vivado:
	HW_PORT=$(HW_PORT) $(VIVADO) -mode batch -source scripts/program_vivado_hw.tcl -tclargs $(BITFILE)

vpp-version:
	$(VPP) --version

hls:
	PART=$(PART) $(VITIS_RUN) --mode hls --tcl hls_ethernet/scripts/run_hls.tcl

hls-bit: hls
	PART=$(PART) $(VIVADO) -mode batch -source hls_ethernet/scripts/build_hls_bitstream.tcl

hls-program:
	HW_PORT=$(HW_PORT) $(XSCT) scripts/program_hw.tcl hls_ethernet/build/hls_ethernet.bit

test-hw:
	$(PYTEST) tests/hw --iface $(IFACE) --hw-port $(HW_PORT)

install-test-board:
	$(PYTEST) tests/hw --iface $(IFACE) --program --hw-port $(HW_PORT)

clean:
	rm -rf .Xil build hls_ethernet/build vivado*.jou vivado*.log vivado_pid*.str
