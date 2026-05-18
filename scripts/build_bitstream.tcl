set script_dir [file dirname [file normalize [info script]]]
set proj_dir [file normalize [file join $script_dir ..]]

if {[info exists ::env(PART)]} {
    set part $::env(PART)
} else {
    set part "xc7a35ticsg324-1L"
}

set build_dir [file join $proj_dir build]
file mkdir $build_dir

read_verilog [file join $proj_dir rtl status_led.v]
read_xdc [file join $proj_dir constraints arty_a7.xdc]

synth_design -top status_led -part $part
opt_design
place_design
route_design

report_timing_summary -file [file join $build_dir timing_summary.rpt]
report_utilization -file [file join $build_dir utilization.rpt]
write_checkpoint -force [file join $build_dir status_led_routed.dcp]
write_bitstream -force [file join $build_dir status_led.bit]

puts "Wrote [file join $build_dir status_led.bit] for $part"
