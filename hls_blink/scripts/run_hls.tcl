set script_dir [file dirname [file normalize [info script]]]
set flow_dir [file normalize [file join $script_dir ..]]

if {[info exists ::env(PART)]} {
    set part $::env(PART)
} else {
    set part "xc7a35ticsg324-1L"
}

set out_dir [file join $flow_dir build hls]
file mkdir $out_dir
cd $out_dir

open_project -reset blink_hls_project
set_top blink_hls
add_files [file join $flow_dir src blink_hls.cpp]
add_files -tb [file join $flow_dir src blink_hls_tb.cpp]

open_solution -reset solution1 -flow_target vivado
set_part $part
create_clock -period 10 -name default

csim_design
csynth_design
export_design -format ip_catalog

puts "HLS RTL: [file join $out_dir blink_hls_project solution1 syn verilog blink_hls.v]"
puts "HLS IP:  [file join $out_dir blink_hls_project solution1 impl ip]"
exit
