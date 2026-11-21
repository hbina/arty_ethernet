# Implement one project for the Arty A7 (python3 build.py build <project>
# generates the RTL first and calls this).
#
#   vivado -mode batch -source scripts/build_bitstream.tcl \
#       -tclargs <project-dir> <out-dir> <top-module> <name>
#
# Reads every .v in <out-dir>/rtl (generated) and <project-dir>/rtl
# (handwritten), every .xdc in <project-dir>/constraints, and sources
# <project-dir>/scripts/post_route.tcl after routing if it exists (with
# $out_dir set), for project-specific reports. Writes the routed checkpoint,
# the standard reports and <out-dir>/<name>.bit.
if {[llength $argv] != 4} {
    error "usage: build_bitstream.tcl <project-dir> <out-dir> <top-module> <name>"
}
lassign $argv project_dir out_dir top name
set project_dir [file normalize $project_dir]
set out_dir [file normalize $out_dir]

if {[info exists ::env(PART)]} {
    set part $::env(PART)
} else {
    set part "xc7a100ticsg324-1L"
}

set generated [lsort [glob -nocomplain -directory [file join $out_dir rtl] *.v]]
if {[llength $generated] == 0} {
    error "No generated RTL in [file join $out_dir rtl]. Run python3 build.py build."
}
set handwritten [lsort [glob -nocomplain -directory [file join $project_dir rtl] *.v]]
set constraints [lsort [glob -nocomplain -directory [file join $project_dir constraints] *.xdc]]
if {[llength $constraints] == 0} {
    error "No constraints in [file join $project_dir constraints]"
}

set build_dir [file join $out_dir vivado]
file mkdir $build_dir

create_project -force ${name}_vivado $build_dir -part $part
set_property target_language Verilog [current_project]

foreach path [concat $generated $handwritten] {
    read_verilog $path
}
foreach path $constraints {
    read_xdc $path
}

synth_design -top $top -part $part
opt_design
place_design
route_design
# Routed design, for floorplan and post-route inspection.
write_checkpoint -force [file join $out_dir routed.dcp]

report_timing_summary -file [file join $out_dir timing_summary.rpt]
report_utilization -file [file join $out_dir utilization.rpt]
report_cdc -file [file join $out_dir cdc.rpt]

set hook [file join $project_dir scripts post_route.tcl]
if {[file exists $hook]} {
    source $hook
}

set bitfile [file join $out_dir $name.bit]
write_bitstream -force $bitfile
puts "Wrote $bitfile for $part"
