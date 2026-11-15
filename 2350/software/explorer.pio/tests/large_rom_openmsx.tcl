proc assert {expression} {
    if {![uplevel 1 [list expr $expression]]} {error "Assertion failed: $expression"}
}

proc user_setting {action type name description value} {
    set ::$name $value
}
rename after native_after
proc after {args} {return test-event}
proc machine {} {return test-machine}
proc set_help_text {args} {}
proc set_tabcompletion_proc {args} {}
proc notify {message severity} {lappend ::notifications [list $message $severity]}
proc read_u16 {data offset} {
    binary scan [string range $data $offset [expr {$offset + 1}]] cu2 bytes
    return [expr {[lindex $bytes 0] | ([lindex $bytes 1] << 8)}]
}
set notifications [list]

lassign $argv script work
source $script
set sd [file join $work sd]
file mkdir $sd
set ::picoverse2350_sd $sd
set ::picoverse2350::sd_folder $sd
set ::picoverse2350::menu_pos 0x100000
set ::picoverse2350::image_file [file join $work flash.bin]
set ::picoverse2350::menu_rom [string repeat "\x00" 32768]
set ::picoverse2350::state menu

proc select_name {name} {
    for {set i 0} {$i < [llength $::picoverse2350::filtered]} {incr i} {
        if {[lindex [picoverse2350::filtered_rec $i] 0] eq $name} {
            set ::picoverse2350::query [lreplace $::picoverse2350::query 0 1 [expr {$i & 255}] [expr {$i >> 8}]]
            return [picoverse2350::filtered_rec $i]
        }
    }
    error "Entry not found: $name"
}

proc finish_copy {} {
    set steps 0
    while {$::picoverse2350::job ne ""} {
        picoverse2350::copy_step
        incr steps
        assert {$steps < 2000}
    }
}

namespace eval picoverse2350 {
    write_binary $image_file [string repeat "\xFF" [expr {$flash_size - $menu_pos}]]
    load_flash_records
    set fixture_count 21
    for {set i 0} {$i < $fixture_count} {incr i} {
        set offset [expr {$store_offset + $i * 8192}]
        patch_file $image_file $offset [string repeat Z 8192]
        assert {[flash_append [format "Fixture%02d" $i] 1 8192 $offset]}
    }
    set data [string repeat "0123456789ABCDEF" [expr {0x800000 / 16}]]
    file mkdir [file join $sd_folder games]
    write_binary [file join $sd_folder games Large.NEO-8.ROM] $data
    reset_pico
    set browse 2
    set sd_path "/games"
    refresh
    lassign [flash_capacity] initial_free initial_max
    assert {$initial_free == ($flash_size - $menu_pos - $store_offset) / 1024 - $fixture_count * 8}
    assert {$initial_max == $initial_free}
    set rec [select_name Large]
    assert {[lindex $rec 2] == 0x800000 && [rec_is_sd $rec]}
    set pvc [pvc_path $rec]
    write_binary [sd_host $pvc] "PVC1\x04\x01\x08"
    ctrl_set audio 4
    ctrl_set psg 1
    command 16
    assert {[ctrl_get cmd] == 16 && $job ne ""}
    finish_copy
    assert {[ctrl_get cmd] == 0 && [ctrl_get ack] == $magic && [ctrl_get mapper] == 100}
    assert {$browse == 1 && $sd_path eq "/" && $match == $fixture_count}
    assert {$pending == 0 && $last_source == 0 && ![file exists [sd_host /PICOVERSE.PVL]]}
    set flash_rec [filtered_rec $match]
    assert {![rec_is_sd $flash_rec] && [lindex $flash_rec 0] eq "Large" && [lindex $flash_rec 3] eq ""}
    assert {[llength $flash_recs] == $fixture_count + 1 && [read_binary [sd_host /Large.flash.PVC]] eq "PVC1\x04\x01\x08"}
    select_name Large
    command 7
    assert {[ctrl_get ack] == 1 && [ctrl_get audio] == 4 && [ctrl_get psg] == 1}
    set query [lreplace $query 2 9 5 0 8 0 130 2 1 0]
    command 8
    assert {[ctrl_get ack] == 1 && $pending == 0 && $last_source == 0}
    set flash_options [read_binary [sd_host /Large.flash.PVC]]
    assert {$flash_options eq "PVC1[binary format c9 {5 0 8 0 130 2 1 0 0}]"}
    assert {[read_binary [sd_host $pvc]] eq "PVC1\x04\x01\x08"}
    lassign [flash_capacity] free largest
    assert {$free == $initial_free - 8192 && $largest == $free}
    sync_ctrl
    assert {[read_u16 $mirror [expr {$flash_info_base - 0x4000}]] == $free}
    assert {[read_u16 $mirror [expr {$flash_info_base + 2 - 0x4000}]] == $largest}

    # The opened flash screen remains independent of changes to the SD original.
    set changed [string replace $data end end X]
    write_binary [sd_host [lindex $rec 3]] $changed
    assert {[rom_bytes $flash_rec] eq $data}
    set browse 2
    set sd_path "/games"
    refresh
    select_name Large
    command 16
    assert {$job eq "" && [ctrl_get ack] == 0 && [ctrl_get mapper] == 0x82}
    assert {[llength $flash_recs] == $fixture_count + 1 && $browse == 2 && $sd_path eq "/games" && $pending == 0}
    write_binary [sd_host [lindex $rec 3]] $data
    set old_slots $slots_used
    command 16
    assert {$job eq "" && [ctrl_get ack] == $magic && $slots_used == $old_slots}
    assert {$browse == 1 && $match == $fixture_count && $pending == 0}
    assert {[read_binary [sd_host /Large.flash.PVC]] eq $flash_options}
    select_name Large
    command 7
    assert {[ctrl_get audio] == 5 && [ctrl_get volume] == 130 && [ctrl_get vdp] == 2}
    assert {$pending == 0 && $last_source == 0}
    command 13
    assert {$last_source == 1 && $last_path eq "Large"}
    on_select $match
    assert {$pending == 1}
    set pending 0

    # A second 8 MB ROM needs cleanup, then succeeds after an F1 deletion.
    write_binary [file join $sd_folder Second.NEO-8.ROM] $data
    set browse 2
    set sd_path "/"
    refresh
    select_name Second
    command 16
    assert {$job eq "" && [ctrl_get ack] == 0 && [ctrl_get mapper] == 0x80}
    set browse 1
    refresh
    select_name Large
    command 11
    assert {[ctrl_get ack] == 1 && [llength $flash_recs] == $fixture_count}
    assert {[flash_capacity] eq [list $initial_free $initial_max]}
    set browse 2
    refresh
    select_name Second
    command 16
    finish_copy
    assert {[ctrl_get ack] == $magic && [llength $flash_recs] == $fixture_count + 1 && $browse == 1}
    load_flash_records
    assert {[lindex [filtered_rec $match] 0] eq "Second" && ![rec_is_sd [filtered_rec $match]]}

    set untyped [string replace [string repeat Z 1048576] 0 1 AB]
    set untyped [string replace $untyped 16 23 ROM_NEO8]
    write_binary [file join $sd_folder Untyped.ROM] $untyped
    set browse 2
    refresh
    set untyped_rec [select_name Untyped]
    assert {[code_of [lindex $untyped_rec 1]] == 0}
    set romdb [dict create missing 0]
    set ::expected_copied_data $untyped
    set ::flash_detect_calls 0
    rename detect_rom detect_rom_actual
    proc detect_rom {host} {
        assert {[ctrl_get mapper] == 0x7F}
        assert {[read_binary $host] eq $::expected_copied_data}
        set ::detector_tempfile $host
        incr ::flash_detect_calls
        return [detect_rom_actual $host]
    }
    set slots_before $slots_used
    command 16
    assert {$::flash_detect_calls == 0 && [dict get $job mapper] == 0}
    while {[dict get $job phase] eq "copy"} {copy_step}
    assert {$slots_used == $slots_before && [ctrl_get mapper] == 0x7F && $::flash_detect_calls == 0}
    # Detection must use flash even if the original SD image changes meanwhile.
    write_binary [sd_host [lindex $untyped_rec 3]] [string replace $untyped 16 23 ROM_NE16]
    copy_step
    assert {$::flash_detect_calls == 1 && ![file exists $::detector_tempfile]}
    assert {[ctrl_get ack] == $magic && $browse == 1 && $pending == 0}
    assert {[code_of [lindex [filtered_rec $match] 1]] == 8 && [rom_bytes [filtered_rec $match]] eq $untyped}
    rename detect_rom ""
    rename detect_rom_actual detect_rom

    write_binary [file join $sd_folder Unsupported.ROM] [string repeat "\x00" 8192]
    set browse 2
    refresh
    select_name Unsupported
    set slots_before $slots_used
    command 16
    assert {$job ne "" && [dict get $job mapper] == 0}
    finish_copy
    assert {[ctrl_get ack] == 0 && [ctrl_get mapper] == 0x83 && $slots_used == $slots_before}
    assert {$browse == 2 && $pending == 0 && $job eq ""}

    # Total free capacity is not interchangeable with a contiguous gap.
    set saved $flash_recs
    set flash_recs [list [list A 8 0x100000 [expr {$store_offset + 0x200000}] 0] \
                         [list B 8 0x100000 [expr {$store_offset + 0x700000}] 1]]
    lassign [flash_capacity] free largest
    assert {$free >= 8192 && $largest < 8192 && [flash_find_space 0x800000] == -1}
    assert {[flash_find_space 0x200000] == $store_offset}
    set overflow 1
    assert {[flash_capacity] eq [list 65535 0] && [flash_find_space 4096] == -1}
    set overflow 0
    set flash_recs $saved
}
puts "PASS: openMSX copy-to-flash selection across pages, editable flash options, no automatic run, reuse and cleanup/retry"

namespace eval picoverse2350 {
    set browse 1
    set sd_path "/"
    set hd_image [file join $work nextor.dsk]
    foreach mapper {15 16 17 19} {
        set recs [list [list "NextorTest" $mapper 131072 ""]]
        set filtered {0}
        set query [lrepeat 32 0]
        set p [sd_host /NextorTest.flash.PVC]
        if {[file exists $p]} {file delete $p}
        command 7
        assert {[ctrl_get ack] == 1 && [ctrl_get sd_part] == 1}
        binary scan [string range $mirror [expr {$part_base - 0x4000}] [expr {$part_base - 0x4000 + 1}]] cu2 choices
        assert {$choices eq {2 17}}
        handle_write 0xBFFD 5
        assert {[ctrl_get sd_part] == 5}
        assert {[string range $mirror [expr {$part_base - 0x4000 + 2}] [expr {$part_base - 0x4000 + 11}]] eq "WHOLE DISK"}
        set query [lreplace $query 2 9 4 1 $mapper 5 130 0 0 0]
        command 8
        assert {[ctrl_get ack] == 1 && [read_binary $p] eq "PVC1[binary format c9 [list 4 1 $mapper 5 130 0 0 0 0]]"}
        ctrl_set sd_part 1
        command 7
        assert {[ctrl_get sd_part] == 5 && [ctrl_get volume] == 130}
        handle_write 0xBFFD 1
        assert {[ctrl_get sd_part] == 1}
    }
}
puts "PASS: openMSX Whole Disk choice, control writes and PVC persistence across Nextor SD profiles"
