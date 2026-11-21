if {[llength $argv] != 1} {
    error "usage: xsct program_hw.tcl <bitfile>"
}
set bitfile [lindex $argv 0]

if {![file exists $bitfile]} {
    error "Bitstream not found: $bitfile"
}

if {[info exists ::env(HW_PORT)]} {
    set port $::env(HW_PORT)
} else {
    set port 3121
}

connect -host localhost -port $port
targets
targets -set -filter {name =~ "xc7a*"}
fpga -file $bitfile
puts "Programmed $bitfile"
