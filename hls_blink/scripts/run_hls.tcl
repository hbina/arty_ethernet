set script_dir [file dirname [file normalize [info script]]]
set flow_dir [file normalize [file join $script_dir ..]]

if {[info exists ::env(PART)]} {
    set part $::env(PART)
} else {
    set part "xc7a100ticsg324-1L"
}

set out_dir [file join $flow_dir build hls]
file mkdir $out_dir
cd $out_dir

open_project -reset blink_hls_project
set_top blink_hls
add_files [file join $flow_dir src blink_hls.cpp]
add_files -tb [file join $flow_dir tb blink_hls_tb.cpp]

open_solution -reset solution1 -flow_target vivado
set_part $part
create_clock -period 10 -name default

csim_design
csynth_design
export_design -format ip_catalog

puts "HLS RTL: [file join $out_dir blink_hls_project solution1 syn verilog blink_hls.v]"
puts "HLS IP:  [file join $out_dir blink_hls_project solution1 impl ip]"

open_project -reset ethernet_l2_endpoint_hls_project
set_top ethernet_l2_endpoint_hls
add_files [file join $flow_dir src ethernet_l2_endpoint_hls.cpp]
add_files -tb [file join $flow_dir tb ethernet_l2_endpoint_hls_test_frame.cpp]
add_files -tb [file join $flow_dir tb ethernet_l2_endpoint_hls_tb.cpp]

open_solution -reset solution1 -flow_target vivado
set_part $part
create_clock -period 40 -name default

csim_design
csynth_design
export_design -format ip_catalog

puts "HLS RTL: [file join $out_dir ethernet_l2_endpoint_hls_project solution1 syn verilog ethernet_l2_endpoint_hls.v]"
puts "HLS IP:  [file join $out_dir ethernet_l2_endpoint_hls_project solution1 impl ip]"
exit
