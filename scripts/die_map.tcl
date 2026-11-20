# Dump a routed checkpoint's device floorplan and cell placement as
# tab-separated records for scripts/die_map.py.
#
# usage: vivado -mode batch -source scripts/die_map.tcl -tclargs <checkpoint.dcp> <dump.tsv>
#
# Records (one per line, fields separated by tabs):
#   V  <vivado version>
#   D  <part> <top module>
#   T  <grid x> <grid y> <tile type>                 every tile on the die
#   R  <clock region> <x0> <y0> <x1> <y1>             grid bounds of each clock region
#   C  <grid x> <grid y> <primitive> <cell path>      every placed primitive cell
#   P  <grid x> <grid y> <port> <package pin>         every placed top-level port
if {[llength $argv] != 2} {
    error "usage: die_map.tcl <checkpoint.dcp> <dump.tsv>"
}
lassign $argv checkpoint dump

open_checkpoint $checkpoint
set f [open $dump w]
fconfigure $f -translation lf
puts $f "V\t[version -short]"
puts $f "D\t[get_property PART [current_design]]\t[get_property TOP [current_design]]"

foreach t [get_tiles] {
    puts $f "T\t[get_property GRID_POINT_X $t]\t[get_property GRID_POINT_Y $t]\t[get_property TYPE $t]"
}
foreach r [get_clock_regions] {
    set tl [get_tiles [get_property TOP_LEFT_TILE $r]]
    set br [get_tiles [get_property BOTTOM_RIGHT_TILE $r]]
    puts $f "R\t[get_property NAME $r]\t[get_property GRID_POINT_X $tl]\t[get_property GRID_POINT_Y $tl]\t[get_property GRID_POINT_X $br]\t[get_property GRID_POINT_Y $br]"
}
foreach c [get_cells -hierarchical -filter {IS_PRIMITIVE && STATUS != UNPLACED}] {
    set site [get_sites -quiet -of_objects $c]
    if {$site eq ""} { continue }
    set t [get_tiles -of_objects $site]
    puts $f "C\t[get_property GRID_POINT_X $t]\t[get_property GRID_POINT_Y $t]\t[get_property REF_NAME $c]\t$c"
}
foreach p [get_ports] {
    set site [get_sites -quiet -of_objects $p]
    if {$site eq ""} { continue }
    set t [get_tiles -of_objects $site]
    puts $f "P\t[get_property GRID_POINT_X $t]\t[get_property GRID_POINT_Y $t]\t$p\t[get_property PACKAGE_PIN $p]"
}
close $f
puts "Wrote $dump"
