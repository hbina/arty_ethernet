# Sourced by scripts/build_bitstream.tcl after routing ($out_dir is set).
# Worst register-to-register paths (pad timing excluded): the logic depth.
report_timing -group [get_clocks eth_rx_clk_pin] -from [all_registers] -to [all_registers] -max_paths 10 -nworst 1 -file [file join $out_dir internal_paths.rpt]
