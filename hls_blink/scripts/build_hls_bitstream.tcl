set script_dir [file dirname [file normalize [info script]]]
set flow_dir [file normalize [file join $script_dir ..]]
set repo_dir [file normalize [file join $flow_dir ..]]

if {[info exists ::env(PART)]} {
    set part $::env(PART)
} else {
    set part "xc7a35ticsg324-1L"
}

set hls_rtl [file join $flow_dir build hls blink_hls_project solution1 syn verilog blink_hls.v]
if {![file exists $hls_rtl]} {
    error "HLS RTL not found: $hls_rtl. Run make hls first."
}

set build_dir [file join $flow_dir build vivado]
file mkdir $build_dir

create_project -force hls_blink_vivado $build_dir -part $part
set_property target_language Verilog [current_project]

read_verilog $hls_rtl
read_verilog [file join $flow_dir src mii_rx_monitor.v]
read_verilog [file join $flow_dir src hls_top.v]
read_xdc [file join $repo_dir constraints arty_a7.xdc]

synth_design -top hls_top -part $part
opt_design
place_design
route_design

report_timing_summary -file [file join $flow_dir build timing_summary.rpt]
report_utilization -file [file join $flow_dir build utilization.rpt]
write_bitstream -force [file join $flow_dir build hls_blink.bit]

puts "Wrote [file join $flow_dir build hls_blink.bit] for $part"
