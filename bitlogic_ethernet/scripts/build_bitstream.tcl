# Implement the bitlogic Ethernet endpoint for the Arty A7.
# Expects generated Verilog in bitlogic_ethernet/build/rtl (python3 build.py
# build --design bitlogic runs the generator first).
set script_dir [file dirname [file normalize [info script]]]
set flow_dir [file normalize [file join $script_dir ..]]
set repo_dir [file normalize [file join $flow_dir ..]]

if {[info exists ::env(PART)]} {
    set part $::env(PART)
} else {
    set part "xc7a100ticsg324-1L"
}

set generated_dir [file join $flow_dir build rtl]
set generated [list eth_core.v rx_unit.v proto_unit.v tx_unit.v status_unit.v template_rom.v bitlogic_sdp_ram.v]
foreach name $generated {
    if {![file exists [file join $generated_dir $name]]} {
        error "Generated RTL not found: [file join $generated_dir $name]. Run python3 build.py build --design bitlogic."
    }
}

set build_dir [file join $flow_dir build vivado]
file mkdir $build_dir

create_project -force bitlogic_ethernet_vivado $build_dir -part $part
set_property target_language Verilog [current_project]

foreach name $generated {
    read_verilog [file join $generated_dir $name]
}
foreach name [list arty_top.v] {
    read_verilog [file join $flow_dir rtl $name]
}
read_xdc [file join $repo_dir constraints arty_a7.xdc]

synth_design -top arty_top -part $part
opt_design
place_design
route_design
write_checkpoint -force [file join $flow_dir build bitlogic_ethernet_routed.dcp]

report_timing_summary -file [file join $flow_dir build timing_summary.rpt]
report_utilization -file [file join $flow_dir build utilization.rpt]
report_cdc -file [file join $flow_dir build cdc.rpt]
write_bitstream -force [file join $flow_dir build bitlogic_ethernet.bit]

puts "Wrote [file join $flow_dir build bitlogic_ethernet.bit] for $part"
