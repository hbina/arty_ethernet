# Dump the device grid and the placement of the routed design as
# tab-separated records, for scripts/floorplan.py.
#   vivado -mode batch -source dump_floorplan.tcl -tclargs routed.dcp out.tsv
set dcp [lindex $argv 0]
set out [lindex $argv 1]
open_checkpoint $dcp
set f [open $out w]

puts $f "part\t[get_property PART [current_design]]"

# Tiles that hold sites (interconnect and filler tiles are left out).
foreach t [get_tiles -filter {NUM_SITES > 0}] {
    puts $f "tile\t$t\t[get_property TYPE $t]\t[get_property ROW $t]\t[get_property COLUMN $t]"
}
foreach s [get_sites] {
    puts $f "site\t$s\t[get_property SITE_TYPE $s]\t[get_tiles -of $s]\t[get_property CLOCK_REGION $s]"
}
foreach r [get_clock_regions] {
    set tl [get_tiles [get_property TOP_LEFT_TILE $r]]
    set br [get_tiles [get_property BOTTOM_RIGHT_TILE $r]]
    puts $f "region\t$r\t[get_property ROW $tl]\t[get_property COLUMN $tl]\t[get_property ROW $br]\t[get_property COLUMN $br]"
}
foreach p [get_package_pins] {
    set site [get_sites -quiet -of $p]
    puts $f "pin\t$p\t[get_property BANK $p]\t$site"
}
puts $f "grid\t[llength [get_tiles]]\t[expr {[tcl::mathfunc::max {*}[get_property ROW [get_tiles]]] + 1}]\t[expr {[tcl::mathfunc::max {*}[get_property COLUMN [get_tiles]]] + 1}]"
foreach p [get_ports] {
    puts $f "port\t$p\t[get_property PACKAGE_PIN $p]\t[get_property DIRECTION $p]"
}
foreach c [get_cells -hier -filter {IS_PRIMITIVE && LOC != ""}] {
    puts $f "cell\t$c\t[get_property LOC $c]\t[get_property BEL $c]\t[get_property REF_NAME $c]"
}
close $f
