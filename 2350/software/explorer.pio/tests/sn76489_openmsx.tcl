proc assert {expression} {
    if {![uplevel 1 [list expr $expression]]} {error "Assertion failed: $expression"}
}
proc user_setting {action type name description value} {set ::$name $value}
rename after native_after
proc after {args} {return test-event}
proc machine {} {return test-machine}
proc set_help_text {args} {}
proc set_tabcompletion_proc {args} {}
proc notify {args} {}
lassign $argv script work
source $script
set ::picoverse2350_sd $work
set ::picoverse2350::sd_folder $work
set ::picoverse2350::recs [list [list Song 0x81 32768 /Song.ROM]]
set ::picoverse2350::filtered {0}

namespace eval picoverse2350 {
    for {set code 1} {$code <= 23} {incr code} {
        assert {[expr {[audio_mode $code 13] eq "sn_franky"}] ==
            [expr {$code <= 18 || $code == 22 || $code == 23}]}
        assert {[audio_mode $code 14] eq "none"}
    }
    set slots [dict create 0 ROM]
    set io ""
    audio_devices sn_franky slots io
    assert {[dict size $slots] == 1 && [string match "*<Franky*" $io]}
    assert {[string match "*0x48*" $io] && [string match "*0x88*" $io]}
    foreach profile {13 14} {
        set query [lrepeat 32 0]
        lset query 2 $profile
        lset query 3 1
        lset query 4 1
        lset query 6 130
        save_options
        assert {[ctrl_get ack] == 1}
        set data [read_binary [file join $sd_folder Song.PVC]]
        assert {[string length $data] == 13 && [string range $data 0 3] eq "PVC1"}
        ctrl_set audio 0
        load_options
        assert {[ctrl_get ack] == 1 && [ctrl_get audio] == $profile}
        assert {[ctrl_get psg] == 1 && [ctrl_get volume] == 130}
        if {$profile == 14} {
            assert {[audio_mode 1 [ctrl_get audio]] eq "none"}
            set slots [dict create 0 ROM]
            set io ""
            audio_devices [audio_mode 1 [ctrl_get audio]] slots io
            assert {[dict size $slots] == 1 && $io eq ""}
        }
    }
    file delete [file join $sd_folder Song.PVC]
}
puts "PASS: openMSX Franky matrix/XML and PVC round trips; removed profile 14 creates no audio or mapper"
