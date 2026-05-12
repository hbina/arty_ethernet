if {[info exists ::env(HW_PORT)]} {
    set port $::env(HW_PORT)
} else {
    set port 3121
}

if {[llength $argv] > 0} {
    set bitfile [lindex $argv 0]
} else {
    set bitfile [file normalize "hls_blink/build/hls_blink.bit"]
}

if {![file exists $bitfile]} {
    error "Bitstream not found: $bitfile"
}

open_hw_manager
connect_hw_server -url localhost:$port
set targets [get_hw_targets]
if {[llength $targets] == 0} {
    error "No hardware targets found."
}

open_hw_target [lindex $targets 0]

set devices [get_hw_devices xc7a100t*]
if {[llength $devices] == 0} {
    error "No xc7a100t device found. Found devices: [get_hw_devices]"
}

set dev [lindex $devices 0]
current_hw_device $dev
refresh_hw_device $dev
set_property PROGRAM.FILE $bitfile $dev
program_hw_devices $dev

puts "Programmed $dev with $bitfile"
