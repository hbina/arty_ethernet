if {[info exists ::env(HW_PORT)]} {
    set port $::env(HW_PORT)
} else {
    set port 3121
}

open_hw_manager
connect_hw_server -url localhost:$port

puts "HW servers:"
puts [get_hw_servers]

puts "HW targets before open:"
puts [get_hw_targets]

set targets [get_hw_targets]
if {[llength $targets] == 0} {
    error "No hardware targets found. Check board power, USB cable, JTAG mode, and cable permissions."
}

open_hw_target [lindex $targets 0]

puts "HW devices:"
foreach dev [get_hw_devices] {
    puts "DEVICE $dev"
    puts "  PART: [get_property PART $dev]"
    puts "  IDCODE: [get_property IDCODE $dev]"
}
