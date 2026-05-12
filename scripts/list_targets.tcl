if {[info exists ::env(HW_PORT)]} {
    set port $::env(HW_PORT)
} else {
    set port 3121
}

connect -host localhost -port $port
targets
