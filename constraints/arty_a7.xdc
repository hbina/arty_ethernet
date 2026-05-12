## Arty A7-35T / Arty A7-100T Rev D/E
## Clock signal
set_property -dict { PACKAGE_PIN E3 IOSTANDARD LVCMOS33 } [get_ports { CLK100MHZ }]
create_clock -add -name sys_clk_pin -period 10.000 -waveform {0.000 5.000} [get_ports { CLK100MHZ }]

## User LEDs LD4-LD7
set_property -dict { PACKAGE_PIN H5  IOSTANDARD LVCMOS33 } [get_ports { led[0] }]
set_property -dict { PACKAGE_PIN J5  IOSTANDARD LVCMOS33 } [get_ports { led[1] }]
set_property -dict { PACKAGE_PIN T9  IOSTANDARD LVCMOS33 } [get_ports { led[2] }]
set_property -dict { PACKAGE_PIN T10 IOSTANDARD LVCMOS33 } [get_ports { led[3] }]

## SMSC/TI 10/100 Ethernet PHY MII
set_property -dict { PACKAGE_PIN G18 IOSTANDARD LVCMOS33 } [get_ports { eth_ref_clk }]
set_property -dict { PACKAGE_PIN C16 IOSTANDARD LVCMOS33 } [get_ports { eth_rstn }]
set_property -dict { PACKAGE_PIN F15 IOSTANDARD LVCMOS33 } [get_ports { eth_rx_clk }]
set_property -dict { PACKAGE_PIN G16 IOSTANDARD LVCMOS33 } [get_ports { eth_rx_dv }]
set_property -dict { PACKAGE_PIN D18 IOSTANDARD LVCMOS33 } [get_ports { eth_rxd[0] }]
set_property -dict { PACKAGE_PIN E17 IOSTANDARD LVCMOS33 } [get_ports { eth_rxd[1] }]
set_property -dict { PACKAGE_PIN E18 IOSTANDARD LVCMOS33 } [get_ports { eth_rxd[2] }]
set_property -dict { PACKAGE_PIN G17 IOSTANDARD LVCMOS33 } [get_ports { eth_rxd[3] }]
set_property -dict { PACKAGE_PIN C17 IOSTANDARD LVCMOS33 } [get_ports { eth_rxerr }]
set_property -dict { PACKAGE_PIN H15 IOSTANDARD LVCMOS33 } [get_ports { eth_tx_en }]
set_property -dict { PACKAGE_PIN H14 IOSTANDARD LVCMOS33 } [get_ports { eth_txd[0] }]
set_property -dict { PACKAGE_PIN J14 IOSTANDARD LVCMOS33 } [get_ports { eth_txd[1] }]
set_property -dict { PACKAGE_PIN J13 IOSTANDARD LVCMOS33 } [get_ports { eth_txd[2] }]
set_property -dict { PACKAGE_PIN H17 IOSTANDARD LVCMOS33 } [get_ports { eth_txd[3] }]

create_clock -add -name eth_rx_clk_pin -period 40.000 -waveform {0.000 20.000} [get_ports { eth_rx_clk }]
set_clock_groups -asynchronous -group [get_clocks sys_clk_pin] -group [get_clocks eth_rx_clk_pin]
