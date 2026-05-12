set script_dir [file dirname [file normalize [info script]]]
set proj_dir [file normalize [file join $script_dir ..]]

if {[llength $argv] > 0} {
    set bitfile [lindex $argv 0]
} else {
    set bitfile [file join $proj_dir build blink.bit]
}

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
