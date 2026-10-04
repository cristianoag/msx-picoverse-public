# MSX PICOVERSE PROJECT
# (c) 2026 Cristiano Goncalves
# The Retro Hacker
#
# picoverse2350.tcl - openMSX support for PicoVerse 2350 Explorer UF2 images
#
# Lets openMSX run an Explorer UF2 produced by 2350/software/explorer.pio/tool
# as if the PicoVerse 2350 cartridge were plugged into a cartridge slot. The
# Raspberry Pi Pico 2 itself is not emulated; this script plays the role of
# the Explorer firmware (pico/explorer/explorer.c) at the level the MSX sees:
#
#  1. The UF2 is decoded back into the Pico flash image, from the menu ROM on:
#       [firmware][menu 32KB][config 16KB][hidden payloads][ROM images...]
#     and kept as an editable copy in the openMSX persistent folder, so the
#     flash entries copied, renamed or deleted from the menu survive restarts.
#  2. The PicoVerse_2350 extension maps a 32KB window at 0x4000-0xBFFF. The
#     script loads the Explorer menu ROM into it and serves the Pico control
#     area (0xB900-0xBFFF: page buffer, control registers, query buffer) like
#     the firmware's menu loop does. A write watchpoint catches every MSX
#     write to the window: commands are executed at once and the written byte
#     is replaced by what the Pico serves, so the MSX never sees RAM there.
#  3. A host folder plays the microSD card: folders, ROM, DSK, MP3 and WAV
#     files are listed like on the cartridge, .PVC options files and the
#     PICOVERSE.PVL last selection are written to it, and microSD ROMs can be
#     copied to flash (F), renamed (R) and deleted (D).
#  4. The selected entry is started with the matching openMSX hardware: game
#     ROMs with the openMSX mapper of the PicoVerse mapper code, plus the
#     audio profile chosen in the menu (external SCC/SCC+, MSX-MUSIC with the
#     FM-PAC BIOS of the UF2, Yamaha SFG-05/SFG-01 with the SFG BIOS of the
#     UF2, or a second PSG); the Nextor SYSTEM entries as a Sunrise IDE with
#     their own Nextor kernel, the 1MB memory mapper and MegaRAM; .DSK images
#     through the hidden Nextor 2.1.4 kernel of the UF2.
#
# A reset or a power cycle of the emulated MSX brings the menu back, and so
# does restarting openMSX with a PicoVerse entry still in the slot.
#
# Install with openmsx/install.ps1 or openmsx/install.sh. See
# docs/msx-picoverse-2350-openmsx.md in the repository for details.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version. See <https://www.gnu.org/licenses/>.

namespace eval picoverse2350 {

variable version "v2.57"

# Flash layout, must match pico/explorer/explorer.c and tool/src/explorer.c.
# Offsets are relative to the menu ROM (the firmware's flash_rom pointer).
variable flash_base     0x10000000
variable flash_size     0x1000000      ;# 16MB flash on the PicoVerse 2350
variable families       {0xE48BFF59 0xE48BFF5A 0xE48BFF5B}  ;# RP2350 ARM-S/RISC-V/ARM-NS
variable menu_size      0x8000
variable config_offset  0x8000
variable config_size    0x4000
variable record_size    80             ;# name(71) + mapper(1) + size(4) + offset(4)
variable name_max       71
variable config_slots   204
variable max_flash      128
variable max_records    1024
variable fmpac_offset   0x12000        ;# 64KB FM-PAC BIOS
variable sfg_offset     0x22000        ;# SFG-05 BIOS (32KB) + SFG-01 BIOS (32KB)
variable dsk_offset     0x32000        ;# 128KB Nextor 2.1.4 kernel for .DSK images
variable store_offset   0x52000        ;# first byte after the hidden payloads
variable sector         0x1000

# Pico control area served in the cartridge window (msx/src/menu.h)
variable data_base      0xB900
variable data_size      0x680          ;# 0xB900-0xBF7F page buffer
variable monitor_addr   0xBF7F         ;# ROM select register
variable part_base      0xBF80         ;# SD partition info / status text
variable chip_base      0xBFAF
variable vdp_addr       0xBFA0
variable query_base     0xBFC0
variable mp3_base       0xBFE0
variable ctrl_base      0xBFF0
variable magic          0xA5
variable files_per_page 19

variable source_flag 0x80
variable folder_flag 0x40
variable mp3_flag    0x20
variable system_mappers {10 11 15 16 17 18 19 20 21}
variable mapper_dsk  23

# Filename tags, indexed by mapper code - 1 (MAPPER_DESCRIPTIONS)
variable tags {PLA-16 PLA-32 KonSCC PLN-48 ASC-08 ASC-16 Konami NEO-8 NEO-16 SYSTEM
               SYSTEM ASC16X PLN-64 MANBW2 SYSTEM SYSTEM SYSTEM SYSTEM SYSTEM
               SYSTEM SYSTEM ASC16X-FR}

# PicoVerse mapper code -> {openMSX romtype, fixed image size, bank size}.
# Planar ROMs are padded with 0xFF to the window the firmware serves.
variable romtypes [dict create \
	1  {Page12    32768 0}     \
	2  {Page12    32768 0}     \
	3  {KonamiSCC 0     8192}  \
	4  {Page012   49152 0}     \
	5  {ASCII8    0     8192}  \
	6  {ASCII16   0     16384} \
	7  {Konami    0     8192}  \
	8  {NEO-8     0     8192}  \
	9  {NEO-16    0     16384} \
	12 {ASCII16-X 0     16384} \
	13 {Page0123  65536 0}     \
	14 {Manbow2   0     8192}  \
	22 {ASCII16-X 0     16384} \
]

# Loaded UF2 / flash image
variable uf2_file   ""
variable image_file ""      ;# editable flash image (from the menu ROM on)
variable menu_pos   0       ;# offset of the menu ROM in the Pico flash
variable menu_rom   ""
variable flash_recs [list]  ;# {name mapper size offset slot}
variable slots_used 0
variable overflow   0
variable dsk_support 0

# Cartridge / openMSX state
variable dbg        "PicoVerse 2350 Explorer"   ;# RAM device of the extension
variable ext_config "PicoVerse_2350"
variable boot_ext   "PicoVerse2350_Boot"
variable slot       ""
variable slot_ps    0
variable slot_ss    "X"
variable hd_image   ""
variable sd_folder  ""
variable state      "idle"  ;# idle | menu | rom
variable current    ""      ;# {ext <instance>} or {cart <file>}
variable wp_id      ""
variable pending    0
variable own_boot   0
variable running    ""      ;# name of the running entry
variable notified   ""
variable machine_id ""
variable uf2_kinds  [dict create]   ;# .uf2 file -> 1 when it is an RP2350 image
variable watch_interval 0.25
variable romdb      ""

# Emulated Pico menu state (names follow the firmware)
variable mirror     ""      ;# what the Pico serves at 0x4000-0xBFFF
variable recs       [list]  ;# {name mapper size path flash_index}
variable filtered   [list]
variable browse     0       ;# SOURCE_MODE_ALL / FLASH / SD
variable sd_path    "/"
variable query      [lrepeat 32 0]
variable rename_buf [lrepeat 71 0]
variable ctrl       [dict create]
variable match      0xFFFF
variable part_info  ""
variable mp3_status 0
variable mp3_index  0
variable mp3_mode   0
variable disk_act   0
variable last_source 0
variable last_path  ""
variable last_dir   "/"
variable last_index 0xFFFF
variable restore_pending 0
variable rename_pending  ""
variable job        ""      ;# copy to flash job
variable fh_active  0       ;# File Hunter window active

user_setting create string picoverse2350_uf2 \
"PicoVerse 2350 Explorer UF2 loaded when the PicoVerse 2350 extension is
inserted. Updated automatically every time a UF2 is inserted." ""

user_setting create string picoverse2350_slot \
"Cartridge slot (a, b, ...) used for the PicoVerse 2350 cartridge." a

user_setting create string picoverse2350_sd \
"Folder used as the microSD card of the PicoVerse 2350 Explorer. Leave empty
to use picoverse2350/sd in the openMSX persistent folder." ""

user_setting create string picoverse2350_hd \
"Hard disk image used as the microSD/USB drive by the Nextor entries, read
every time a Nextor entry boots. Leave empty to use picoverse2350/hd.dsk in
the openMSX persistent folder." ""

# ---------------------------------------------------------------------------
# Binary and file helpers
# ---------------------------------------------------------------------------

proc read_binary {filename} {
	set fh [open $filename r]
	fconfigure $fh -translation binary
	set data [read $fh]
	close $fh
	return $data
}

proc write_binary {filename data} {
	set fh [open $filename w]
	fconfigure $fh -translation binary
	puts -nonewline $fh $data
	close $fh
}

proc read_range {filename offset length} {
	set fh [open $filename r]
	fconfigure $fh -translation binary
	seek $fh $offset
	set data [read $fh $length]
	close $fh
	return $data
}

# Write data at an offset of a file; a gap after the end is filled with 0xFF,
# like erased flash.
proc patch_file {filename offset data} {
	set fh [open $filename r+]
	fconfigure $fh -translation binary
	seek $fh 0 end
	set end [tell $fh]
	if {$offset > $end} {
		puts -nonewline $fh [string repeat "\xFF" [expr {$offset - $end}]]
	}
	seek $fh $offset
	puts -nonewline $fh $data
	close $fh
}

proc u8 {data pos} {
	binary scan $data "@${pos}cu" value
	return $value
}

proc u32 {data pos} {
	binary scan $data "@${pos}iu" value
	return $value
}

proc bytes_to_string {list} {
	set text ""
	foreach b $list {
		if {$b == 0} break
		append text [format %c $b]
	}
	return $text
}

proc align_up {value align} {
	expr {($value + $align - 1) / $align * $align}
}

proc persistent_dir {} {
	return [file join [file dirname $::env(OPENMSX_USER_DATA)] persistent picoverse2350]
}

proc clean_name {name} {
	set name [regsub -all {[^A-Za-z0-9 ._()+-]} $name _]
	set name [string trim $name " ."]
	if {$name eq ""} {set name "rom"}
	return $name
}

proc xml_escape {text} {
	string map {& &amp; < &lt; > &gt; \" &quot; ' &apos;} $text
}

proc notify {text level} {
	catch {message "PicoVerse 2350: $text" $level}
}

# ---------------------------------------------------------------------------
# UF2 and flash image
# ---------------------------------------------------------------------------

# Family ID of the first block of a UF2 file ("" when the file has none).
proc uf2_family {filename} {
	if {[catch {set block [read_range $filename 0 512]}] || [string length $block] < 512} {
		return ""
	}
	binary scan $block "iuiuiu" m0 m1 flags
	if {$m0 != 0x0A324655 || $m1 != 0x9E5D5157} {return ""}
	if {!($flags & 0x2000)} {return ""}
	return [format 0x%08X [u32 $block 28]]
}

proc is_rp2350_uf2 {filename} {
	variable families
	variable uf2_kinds
	set key [list $filename [file mtime $filename]]
	if {![dict exists $uf2_kinds $key]} {
		dict set uf2_kinds $key [expr {[uf2_family $filename] in $families}]
	}
	dict get $uf2_kinds $key
}

# Decode a UF2 file into a flat flash image (starting at the flash base).
proc uf2_to_flash {filename} {
	variable flash_base
	variable flash_size
	variable families

	set data [read_binary $filename]
	set len [string length $data]
	if {$len == 0 || $len % 512 != 0} {
		error "$filename is not a UF2 file (size is not a multiple of 512 bytes)"
	}
	set blocks [list]
	for {set o 0} {$o < $len} {incr o 512} {
		binary scan $data "@${o}iuiuiuiuiu" m0 m1 flags addr psize
		binary scan $data "@[expr {$o + 28}]iu" family
		binary scan $data "@[expr {$o + 508}]iu" mend
		if {$m0 != 0x0A324655 || $m1 != 0x9E5D5157 || $mend != 0x0AB16F30} {
			error "$filename is not a valid UF2 file (bad magic in block [expr {$o / 512}])"
		}
		if {$flags & 0x1} continue
		if {($flags & 0x2000) && [format 0x%08X $family] ni $families} continue
		if {$psize == 0 || $psize > 476} {
			error "$filename has an invalid payload size in block [expr {$o / 512}]"
		}
		if {$addr < $flash_base || $addr >= $flash_base + $flash_size} continue
		lappend blocks [list [expr {$addr - $flash_base}] \
			[string range $data [expr {$o + 32}] [expr {$o + 31 + $psize}]]]
	}
	if {[llength $blocks] == 0} {
		error "$filename does not contain RP2350 flash data (is it a PicoVerse 2040 UF2?)"
	}
	set image ""
	set pos 0
	foreach block [lsort -integer -index 0 $blocks] {
		lassign $block addr payload
		if {$addr < $pos} {
			set payload [string range $payload [expr {$pos - $addr}] end]
			set addr $pos
		}
		if {$addr > $pos} {
			append image [string repeat "\xFF" [expr {$addr - $pos}]]
		}
		append image $payload
		set pos [expr {$addr + [string length $payload]}]
	}
	return $image
}

# Check whether the Explorer menu ROM + config area start at 'pos'.
proc is_menu_at {flash len pos} {
	variable menu_size
	variable config_offset
	variable config_size
	variable record_size
	variable name_max

	if {$pos + $config_offset + $config_size > $len} {return 0}
	if {[string range $flash $pos [expr {$pos + 1}]] ne "AB"} {return 0}
	binary scan $flash "@[expr {$pos + 2}]su" init
	if {$init < 0x4000 || $init > 0xBFFF} {return 0}
	set rec [expr {$pos + $config_offset}]
	if {[string range $flash $rec [expr {$rec + $record_size - 1}]] eq [string repeat "\xFF" $record_size]} {
		return 1
	}
	set first [u8 $flash $rec]
	if {$first < 0x20 || $first > 0x7E} {return 0}
	set mapper [expr {[u8 $flash [expr {$rec + $name_max}]] & 0x1F}]
	if {$mapper < 1 || $mapper > 23} {return 0}
	set size   [u32 $flash [expr {$rec + $name_max + 1}]]
	set offset [u32 $flash [expr {$rec + $name_max + 5}]]
	expr {$size > 0 && $offset >= $config_offset + $config_size && $pos + $offset + $size <= $len}
}

# The menu ROM follows the firmware: on the next 4KB boundary since v2.57,
# right after it before. Look for the "AB" header followed by a config area.
proc locate_menu {flash} {
	set len [string length $flash]
	for {set pos 0x1000} {$pos < $len} {incr pos 0x1000} {
		if {[is_menu_at $flash $len $pos]} {return $pos}
	}
	# Search a separate copy: string searches convert the value to a text
	# representation, which would slow down every later binary scan of $flash.
	set head [string range $flash 0 [expr {($len < 0x400000 ? $len : 0x400000) - 1}]]
	set pos 0
	while {[set pos [string first "AB" $head $pos]] >= 0} {
		if {[is_menu_at $flash $len $pos]} {return $pos}
		incr pos
	}
	error "no PicoVerse 2350 Explorer menu found in the UF2 image"
}

proc load_flash_records {} {
	variable image_file
	variable config_offset
	variable config_size
	variable config_slots
	variable record_size
	variable name_max
	variable max_flash
	variable flash_recs
	variable slots_used
	variable overflow

	set cfg [read_range $image_file $config_offset $config_size]
	set empty [string repeat "\xFF" $record_size]
	set flash_recs [list]
	set overflow 0
	for {set s 0} {$s < $config_slots} {incr s} {
		set o [expr {$s * $record_size}]
		if {[string range $cfg $o [expr {$o + $record_size - 1}]] eq $empty} break
		if {[u8 $cfg $o] == 0} continue          ;# deleted entry
		if {[llength $flash_recs] >= $max_flash} {
			set overflow 1
			continue
		}
		set name [string range $cfg $o [expr {$o + $name_max - 1}]]
		set nul [string first "\x00" $name]
		if {$nul >= 0} {set name [string range $name 0 [expr {$nul - 1}]]}
		lappend flash_recs [list [string trimright $name] \
			[u8 $cfg [expr {$o + $name_max}]] \
			[u32 $cfg [expr {$o + $name_max + 1}]] \
			[u32 $cfg [expr {$o + $name_max + 5}]] $s]
	}
	set slots_used $s
}

proc load_uf2 {filename} {
	variable uf2_file
	variable image_file
	variable menu_pos
	variable menu_rom
	variable menu_size
	variable dsk_offset
	variable dsk_support

	set given $filename
	set filename [file normalize $filename]
	if {![file isfile $filename]} {
		set msg "file not found: $filename"
		# Tcl treats backslashes as escapes: C:\temp\x.uf2 arrives as "C:<TAB>empx.uf2".
		if {![string match {*[/\\]*} $given] || [regexp {[\x00-\x1f]} $given]} {
			append msg "\nIn the openMSX console backslashes are escape characters:\
				use forward slashes (C:/temp/explorer.uf2) or braces ({C:\\temp\\explorer.uf2})."
		}
		error $msg
	}
	set fam [uf2_family $filename]
	if {$fam eq "0xE48BFF56"} {
		error "[file tail $filename] is a PicoVerse 2040 (RP2040) UF2, use 'picoverse2040 insert'"
	}

	# The editable flash copy is tied to this UF2 build: a rebuilt UF2 starts
	# from a fresh copy, like flashing the cartridge again.
	set key [format %x_%x [file size $filename] [file mtime $filename]]
	set base [clean_name [file rootname [file tail $filename]]]
	set dir [file join [persistent_dir] flash]
	file mkdir $dir
	set img [file join $dir "${base}_$key.img"]
	set meta "$img.pos"
	if {![file isfile $img] || ![file isfile $meta]} {
		set flash [uf2_to_flash $filename]
		set pos [locate_menu $flash]
		write_binary $img [string range $flash $pos end]
		write_binary $meta $pos
		foreach old [glob -nocomplain -directory $dir "${base}_*.img"] {
			if {$old ne $img && [regexp {_[0-9a-f]+_[0-9a-f]+\.img$} $old]} {
				catch {file delete $old "$old.pos"}
			}
		}
	}
	set uf2_file $filename
	set image_file $img
	set menu_pos [string trim [read_binary $meta]]
	set menu_rom [read_range $img 0 $menu_size]
	set dsk_support [expr {[read_range $img $dsk_offset 2] eq "AB"}]
	load_flash_records
}

# ---------------------------------------------------------------------------
# microSD card (a host folder)
# ---------------------------------------------------------------------------

proc sd_root {} {
	variable sd_folder
	if {$sd_folder ne ""} {return $sd_folder}
	if {$::picoverse2350_sd ne ""} {return [file normalize $::picoverse2350_sd]}
	set dir [file join [persistent_dir] sd]
	file mkdir $dir
	return $dir
}

proc sd_available {} {
	file isdirectory [sd_root]
}

# microSD path ("/GAMES/X.ROM") -> host path
proc sd_host {path} {
	set parts [lsearch -all -inline -not -exact [split $path /] ""]
	if {[llength $parts] == 0} {return [sd_root]}
	file join [sd_root] {*}$parts
}

proc sd_join {dir name} {
	expr {$dir eq "/" ? "/$name" : "$dir/$name"}
}

proc sd_note {} {
	variable disk_act
	set disk_act [expr {($disk_act + 1) & 0xFF}]
}

# Length of the displayed part of a file name and the mapper of its tag:
# "Game.Konami.ROM" -> {4 7}.
proc tag_info {filename} {
	variable tags
	set dot [string last "." $filename]
	set stem [expr {$dot >= 0 ? [string range $filename 0 [expr {$dot - 1}]] : $filename}]
	set n [string length $stem]
	set i 0
	foreach tag $tags {
		incr i
		if {$tag eq "SYSTEM"} continue
		set tl [string length $tag]
		if {$n > $tl + 1 && [string index $stem [expr {$n - $tl - 1}]] eq "." &&
		    [string equal -nocase [string range $stem [expr {$n - $tl}] end] $tag]} {
			return [list [expr {$n - $tl - 1}] $i]
		}
	}
	return [list $n 0]
}

proc display_name {filename} {
	string range $filename 0 [expr {[lindex [tag_info $filename] 0] - 1}]
}

# .PVC options path of a microSD ROM/DSK: the extension is replaced, except for
# .DSK, which keeps it.
proc pvc_from_rom {path is_dsk} {
	set slash [string last "/" $path]
	set dot [string last "." $path]
	if {$dot < 0 || $dot < $slash || $is_dsk} {
		return "$path.PVC"
	}
	return "[string range $path 0 [expr {$dot - 1}]].PVC"
}

# ---------------------------------------------------------------------------
# Record list (flash entries + microSD folder), filter and page buffer
# ---------------------------------------------------------------------------

proc code_of {mapper} {
	expr {$mapper & 0x1F}
}

proc is_system_code {code} {
	variable system_mappers
	expr {$code in $system_mappers}
}

proc rec_is_folder {rec} {
	variable folder_flag
	expr {[lindex $rec 1] & $folder_flag}
}

proc rec_is_sd {rec} {
	variable source_flag
	expr {[lindex $rec 1] & $source_flag}
}

proc rec_is_mp3 {rec} {
	variable mp3_flag
	expr {[lindex $rec 1] & $mp3_flag}
}

proc rec_is_dsk {rec} {
	variable mapper_dsk
	expr {![rec_is_folder $rec] && ![rec_is_mp3 $rec] && [code_of [lindex $rec 1]] == $mapper_dsk}
}

proc refresh {} {
	variable recs
	variable browse
	variable sd_path
	variable flash_recs
	variable folder_flag
	variable source_flag
	variable mp3_flag
	variable mapper_dsk
	variable max_records
	variable dsk_support
	variable name_max
	variable magic
	variable query
	variable current_page
	variable fh_active

	set fh_active 0
	set status $magic
	set folders [list]
	set files [list]
	if {$sd_path ne "/"} {
		lappend folders [list ".." [expr {$folder_flag | $source_flag}] 0 "" -1]
	}
	if {$browse != 1} {
		set dir [sd_host $sd_path]
		if {![file isdirectory $dir]} {
			if {$browse == 2} {set status 0x5D}   ;# CTRL_STATUS_SD_MISSING
		} else {
			sd_note
			set names [list]
			foreach name [glob -nocomplain -types d -tails -directory $dir *] {
				if {$name in {. .. {System Volume Information}}} continue
				lappend names $name
			}
			foreach name [lsort $names] {
				lappend folders [list [string range $name 0 [expr {$name_max - 1}]] \
					[expr {$folder_flag | $source_flag}] 0 "" -1]
			}
			foreach name [glob -nocomplain -types f -tails -directory $dir *] {
				set ext [string toupper [file extension $name]]
				set path [sd_join $sd_path $name]
				set size [file size [file join $dir $name]]
				set display [string range [display_name $name] 0 [expr {$name_max - 1}]]
				switch -- $ext {
					.MP3 - .WAV {
						if {$size == 0} continue
						set m [expr {$mp3_flag | $source_flag | ($ext eq ".WAV" ? 1 : 0)}]
					}
					.DSK {
						if {!$dsk_support || $size == 0 || $size % 368640 != 0 || $size > 0x400000 - 1024} continue
						set m [expr {$mapper_dsk | $source_flag}]
					}
					.ROM {
						if {$size < 8192 || $size > 0x400000} continue
						set m [expr {[lindex [tag_info $name] 1] | $source_flag}]
					}
					default continue
				}
				lappend files [list $display $m $size $path -1]
			}
		}
	}
	if {$sd_path eq "/"} {
		set i 0
		foreach f $flash_recs {
			lassign $f name mapper size
			lappend files [list $name $mapper $size "" $i]
			incr i
		}
	}
	# SYSTEM entries first, then the rest sorted by name.
	set system [list]
	set others [list]
	foreach r $files {
		if {[is_system_code [code_of [lindex $r 1]]] && !([lindex $r 1] & ($folder_flag | $mp3_flag))} {
			lappend system $r
		} else {
			lappend others $r
		}
	}
	set recs [concat $folders $system [lsort -index 0 $others]]
	set recs [lrange $recs 0 [expr {$max_records - 1}]]
	ctrl_set status $status
	set_part_info 1 1 [browse_label]
	set query [lrepeat 32 0]
	apply_filter
	resolve_last_selection_match
	resolve_rename_match
	set current_page 0
	build_page 0
}

proc source_matches {rec} {
	variable browse
	switch -- $browse {
		1 {expr {![rec_is_folder $rec] && ![rec_is_sd $rec]}}
		2 {expr {[rec_is_folder $rec] || [rec_is_sd $rec]}}
		default {return 1}
	}
}

proc query_text {} {
	variable query
	bytes_to_string [lrange $query 0 30]
}

proc query_index {} {
	variable query
	expr {[lindex $query 0] | ([lindex $query 1] << 8)}
}

proc apply_filter {} {
	variable recs
	variable filtered
	set q [query_text]
	set filtered [list]
	set i 0
	foreach r $recs {
		if {[source_matches $r] && ($q eq "" || [string first [string toupper $q] [string toupper [lindex $r 0]]] >= 0)} {
			lappend filtered $i
		}
		incr i
	}
	ctrl_set count [llength $filtered]
}

proc find_first {} {
	variable recs
	variable filtered
	variable match
	set match 0xFFFF
	set q [string toupper [query_text]]
	if {$q eq ""} return
	set i 0
	foreach idx $filtered {
		if {[string first $q [string toupper [lindex $recs $idx 0]]] >= 0} {
			set match $i
			return
		}
		incr i
	}
}

# Record behind a filtered index, or "" when out of range.
proc filtered_rec {index} {
	variable recs
	variable filtered
	if {$index >= [llength $filtered]} {return ""}
	lindex $recs [lindex $filtered $index]
}

proc build_page {page} {
	variable recs
	variable filtered
	variable data_base
	variable data_size
	variable files_per_page

	set total [llength $filtered]
	set start [expr {$page * $files_per_page}]
	set count [expr {$start < $total ? $total - $start : 0}]
	if {$count > $files_per_page} {set count $files_per_page}
	set table_size [expr {$count * 4}]
	set pool_offset [expr {24 + $table_size}]
	set max_pool [expr {$data_size - $pool_offset - $count * 13}]
	set max_name [expr {$count ? $max_pool / $count : 0}]

	set pool ""
	set name_offsets [list]
	for {set i 0} {$i < $count} {incr i} {
		set name [string trimright [lindex $recs [lindex $filtered [expr {$start + $i}]] 0]]
		if {$max_name && [string length $name] + 1 > $max_name} {
			set name [string range $name 0 [expr {$max_name - 2}]]
		}
		lappend name_offsets [string length $pool]
		append pool $name "\x00"
	}
	set payload_offset [expr {$pool_offset + [string length $pool]}]
	set table ""
	set payload ""
	for {set i 0} {$i < $count} {incr i} {
		lassign [lindex $recs [lindex $filtered [expr {$start + $i}]]] name mapper size
		append table [binary format ss [expr {$payload_offset + $i * 13}] 13]
		append payload [binary format "ccsccccci" 1 2 [lindex $name_offsets $i] 2 1 $mapper 3 4 $size]
	}
	set buf "PVEX[binary format ccsssssssss 1 0 24 $total $page $count 24 $table_size $pool_offset [string length $pool] $payload_offset]"
	append buf $table $pool $payload
	append buf [string repeat "\xFF" [expr {$data_size - [string length $buf]}]]
	put $data_base [string range $buf 0 [expr {$data_size - 1}]]
	ctrl_set page $page
}

# ---------------------------------------------------------------------------
# Served cartridge window
# ---------------------------------------------------------------------------

# Write bytes the Pico serves at a cartridge address (0x4000-0xBFFF).
proc put {addr data} {
	variable mirror
	variable dbg
	set off [expr {$addr - 0x4000}]
	set mirror [string replace $mirror $off [expr {$off + [string length $data] - 1}] $data]
	catch {debug write_block $dbg $off $data}
}

proc ctrl_reset {} {
	variable ctrl
	set ctrl [dict create count 0 page 0 status 0xA5 cmd 0 mapper 0 \
		ack 0 audio 0 wifi 0 psg 1 wavegame 0 sd_part 0 browse_part 1 volume 100 \
		vdp 0 net 0 cpu 0]
}

proc ctrl_set {key value} {
	variable ctrl
	dict set ctrl $key $value
}

proc ctrl_get {key} {
	variable ctrl
	dict get $ctrl $key
}

# Refresh the control registers in the window (CTRL_*, CTRL_VDP_FREQ..
# CTRL_DISK_ACT and the MP3 registers).
proc sync_ctrl {} {
	variable ctrl
	variable match
	variable ctrl_base
	variable vdp_addr
	variable mp3_base
	variable disk_act
	variable mp3_status
	variable mp3_index
	variable mp3_mode
	dict with ctrl {
		set block [binary format c16 [list \
			[expr {$count & 0xFF}] [expr {$count >> 8}] $page $status $cmd \
			[expr {$match & 0xFF}] [expr {($match >> 8) & 0xFF}] $mapper $ack $audio \
			$wifi $psg $wavegame $sd_part $browse_part $volume]]
		set misc [binary format c4 [list $vdp $net $cpu $disk_act]]
	}
	put $ctrl_base $block
	put $vdp_addr $misc
	put $mp3_base [binary format c9 [list 0xFF $mp3_status [expr {$mp3_index & 0xFF}] \
		[expr {$mp3_index >> 8}] 0 0 0 0 $mp3_mode]]
}

# SD partition info / status text at 0xBF80: count, mask, label.
proc set_part_info {count mask label} {
	variable part_base
	set label [string range $label 0 28]
	put $part_base [binary format cca30 $count $mask $label]
}

proc browse_label {} {
	set name [string toupper [file tail [sd_root]]]
	if {$name eq ""} {set name "MICROSD"}
	return $name
}

proc hd_label {} {
	string toupper [file rootname [file tail [hd_path]]]
}

# The window content when the menu starts: menu ROM + chip id and SD label.
proc init_mirror {} {
	variable mirror
	variable menu_rom
	variable chip_base
	set mirror $menu_rom
	put $chip_base [binary format a17 "OPENMSX EMULATED"]
	set_part_info 1 1 [browse_label]
}

# Write the whole served window to the cartridge.
proc serve_window {} {
	variable mirror
	variable dbg
	sync_ctrl
	catch {debug write_block $dbg 0 $mirror}
}

# ---------------------------------------------------------------------------
# Mapper detection (pico/explorer/mapper_detect.h: SHA1 database, then the
# openMSX-derived header and opcode heuristics)
# ---------------------------------------------------------------------------

proc load_romdb {} {
	variable romdb
	if {$romdb ne ""} return
	set romdb [dict create]
	set file ""
	catch {set file [data_file extensions/PicoVerse_2350/romdb.txt]}
	if {$file eq "" || ![file isfile $file]} {
		set file [file join $::env(OPENMSX_USER_DATA) extensions PicoVerse_2350 romdb.txt]
	}
	if {[catch {set text [read_binary $file]}]} {
		dict set romdb missing 0
		return
	}
	foreach line [split $text "\n"] {
		set line [string trim $line]
		if {$line eq "" || [string index $line 0] eq "#"} continue
		dict set romdb [lindex $line 0] [lindex $line 1]
	}
}

proc detect_rom {host} {
	variable romdb
	set size [file size $host]
	if {$size < 8192 || $size > 15 * 1024 * 1024} {return 0}
	load_romdb
	if {![catch {sha1sum $host} sha] && [dict exists $romdb [string tolower $sha]]} {
		return [dict get $romdb [string tolower $sha]]
	}

	set data [read_binary $host]
	set n [string length $data]
	set konami 0; set konami_scc 0; set ascii8 0; set ascii16 0
	set p -1
	while {[set p [string first "\x32" $data [incr p]]] >= 0 && $p + 2 < $n} {
		scan [string range $data [expr {$p + 1}] [expr {$p + 2}]] %c%c lo hi
		switch -- [expr {$lo | ($hi << 8)}] {
			16384 - 32768 - 40960 {incr konami}
			20480 - 36864 - 45056 {incr konami_scc}
			26624 - 30720 {incr ascii8}
			30719 {incr ascii16}
			24576 {incr konami; incr ascii8; incr ascii16}
			28672 {incr konami_scc; incr ascii8; incr ascii16}
		}
	}
	set raw_77ff [regexp -all {\xff\x77} $data]
	set raw_6800 [regexp -all {\x00\x68} $data]
	set raw_7800 [regexp -all {\x00\x78} $data]
	if {$ascii8} {incr ascii8 -1}

	set ab0 [expr {[string range $data 0 1] eq "AB"}]
	set ab4000 [expr {[string range $data 16384 16385] eq "AB"}]
	if {$ab0 && $size == 16384} {return 1}
	if {$ab0 && $size <= 32768} {return [expr {$ab4000 ? 4 : 2}]}
	if {$ab0} {
		switch -- [string range $data 16 23] {
			ASCII16X {return 12}
			ROM_NEO8 {return 8}
			ROM_NE16 {return 9}
		}
	}
	if {$size == 524288 && $ab0 && [string range $data 163840 163847] eq "Manbow 2"} {return 14}
	if {$ab4000 && $size <= 49152} {return 4}
	if {$size == 65536 && $ab4000} {return 13}
	if {$size > 32768} {
		set best 0
		set best_score 0
		foreach {score type} [list $konami_scc 3 $konami 7 $ascii8 5 $ascii16 6] {
			if {$score && $score >= $best_score} {
				set best_score $score
				set best $type
			}
		}
		if {$best} {return $best}
		if {$konami == 0 && $konami_scc == 0 && $ascii8 == 0 && $ascii16 == 0} {
			if {$size == 65536 && ($ab0 || $ab4000)} {return 13}
			if {$size > 65536 && $ab0 && $size % 16384 == 0} {
				return [expr {$raw_77ff > $raw_6800 + $raw_7800 ? 6 : 5}]
			}
		}
	}
	return 0
}

# ---------------------------------------------------------------------------
# Options (.PVC), last selection (PICOVERSE.PVL)
# ---------------------------------------------------------------------------

proc pvc_path {rec} {
	lassign $rec name mapper size path
	if {[rec_is_folder $rec] || [rec_is_mp3 $rec]} {return ""}
	if {[code_of $mapper] == 21 || ![rec_is_sd $rec]} {return "/$name.flash.PVC"}
	if {$path eq ""} {return ""}
	pvc_from_rom $path [rec_is_dsk $rec]
}

proc sunrise_partition_info {selected} {
	ctrl_set sd_part 1
	set_part_info 1 1 [hd_label]
}

proc load_options {} {
	variable recs
	variable filtered
	foreach {k v} {ack 0 mapper 0 wavegame 0 audio 0 psg 1 wifi 0 sd_part 0 volume 100 vdp 0 cpu 0} {
		ctrl_set $k $v
	}
	set_part_info 0 0 ""
	if {![sd_available]} return
	set idx [query_index]
	if {$idx >= [llength $filtered]} return
	set ri [lindex $filtered $idx]
	set rec [lindex $recs $ri]
	set sunrise [expr {[code_of [lindex $rec 1]] in {15 16 17 19}}]
	if {$sunrise} {sunrise_partition_info 0}
	set p [pvc_path $rec]
	if {$p eq "" || ![file isfile [sd_host $p]]} {
		if {$sunrise} {ctrl_set ack 1}
		return
	}
	sd_note
	set data [string range [read_binary [sd_host $p]] 0 11]
	set n [string length $data]
	if {$n < 6 || [string range $data 0 3] ne "PVC1"} return
	binary scan $data cu* b
	ctrl_set audio [lindex $b 4]
	ctrl_set psg [expr {[lindex $b 5] ? 1 : 0}]
	if {$n >= 7} {
		set m [lindex $b 6]
		set flags [expr {[lindex $rec 1] & 0xE0}]
		if {$m != 0 && ![is_system_code $m] && $m <= 22 && ![rec_is_dsk $rec] && !($flags & 0x60)} {
			lset recs $ri 1 [expr {$flags | $m}]
			ctrl_set mapper $m
		}
	}
	if {$sunrise && $n >= 8} {sunrise_partition_info [lindex $b 7]}
	if {$n >= 9}  {ctrl_set volume [expr {[lindex $b 8] <= 200 ? [lindex $b 8] : 100}]}
	if {$n >= 10} {ctrl_set vdp [expr {[lindex $b 9] <= 2 ? [lindex $b 9] : 0}]}
	if {$n >= 11} {ctrl_set cpu [expr {[lindex $b 10] <= 2 ? [lindex $b 10] : 0}]}
	if {$n >= 12 && [rec_is_dsk $rec]} {ctrl_set wifi [expr {[lindex $b 11] ? 1 : 0}]}
	ctrl_set ack 1
}

proc save_options {} {
	variable query
	ctrl_set ack 0
	if {![sd_available]} return
	set rec [filtered_rec [query_index]]
	if {$rec eq ""} return
	set p [pvc_path $rec]
	if {$p eq ""} return
	lassign [lrange $query 2 9] audio psg mapper part volume vdp cpu wifi
	if {$volume > 200} {set volume 100}
	if {$vdp > 2} {set vdp 0}
	if {$cpu > 2} {set cpu 0}
	set psg [expr {$psg ? 1 : 0}]
	set wifi [expr {$wifi ? 1 : 0}]
	if {[catch {write_binary [sd_host $p] "PVC1[binary format c8 [list $audio $psg $mapper $part $volume $vdp $cpu $wifi]]"}]} {
		return
	}
	sd_note
	foreach {k v} [list audio $audio psg $psg mapper $mapper sd_part $part volume $volume vdp $vdp cpu $cpu] {
		ctrl_set $k $v
	}
	ctrl_set ack 1
}

proc quick_run {} {
	variable recs
	variable filtered
	foreach {k v} {ack 0 mapper 0 audio 0 psg 1 wifi 0 wavegame 0 volume 100 vdp 0 cpu 0} {
		ctrl_set $k $v
	}
	set idx [query_index]
	if {$idx >= [llength $filtered]} return
	set ri [lindex $filtered $idx]
	if {[code_of [lindex $recs $ri 1]] in {3 14}} {ctrl_set audio 1}
	load_options
	set loaded [ctrl_get ack]
	set rec [lindex $recs $ri]
	set code [code_of [lindex $rec 1]]
	if {$code == 0} {
		detect_mapper_cmd
		set code [ctrl_get mapper]
	} else {
		ctrl_set mapper $code
	}
	if {!$loaded} {
		ctrl_set audio [expr {$code in {3 14} ? 1 : 0}]
		ctrl_set psg 1
	}
	if {![rec_is_dsk $rec]} {ctrl_set wifi 0}
	ctrl_set ack 1
}

proc set_last_dir {} {
	variable last_source
	variable last_path
	variable last_dir
	set last_dir "/"
	if {$last_source != 2} return
	set slash [string last "/" $last_path]
	if {$slash > 0} {set last_dir [string range $last_path 0 [expr {$slash - 1}]]}
}

proc save_last_selection {} {
	variable last_source
	variable last_path
	variable last_index
	ctrl_set ack 0
	set idx [query_index]
	set rec [filtered_rec $idx]
	if {$rec eq "" || [rec_is_folder $rec]} return
	set last_index $idx
	if {[rec_is_sd $rec]} {
		set source 2
		set path [lindex $rec 3]
		if {$path eq ""} return
	} else {
		set source 1
		set path [string trimright [lindex $rec 0]]
	}
	if {$source == $last_source && $path eq $last_path} {
		ctrl_set ack 1
		return
	}
	if {![sd_available]} return
	if {[catch {write_binary [sd_host /PICOVERSE.PVL] "PVLS[binary format c4 [list 1 $source 0 0]]$path\x00"}]} return
	sd_note
	set last_source $source
	set last_path $path
	set_last_dir
	ctrl_set ack 1
}

proc load_last_selection {} {
	variable last_source
	variable last_path
	variable last_dir
	variable restore_pending
	ctrl_set ack 0
	ctrl_set mapper 0
	set last_source 0
	set last_path ""
	set last_dir "/"
	set restore_pending 0
	set file [sd_host /PICOVERSE.PVL]
	if {![sd_available] || ![file isfile $file]} return
	sd_note
	set data [read_binary $file]
	if {[string length $data] < 10 || [string range $data 0 4] ne "PVLS\x01"} return
	set source [u8 $data 5]
	if {$source != 1 && $source != 2} return
	set path [string range $data 8 end]
	set nul [string first "\x00" $path]
	if {$nul >= 0} {set path [string range $path 0 [expr {$nul - 1}]]}
	if {$source == 2 && ![file isfile [sd_host $path]]} return
	set last_source $source
	set last_path $path
	set_last_dir
	set restore_pending 1
	ctrl_set mapper $source
	ctrl_set ack 1
}

proc resolve_last_selection_match {} {
	variable restore_pending
	variable last_source
	variable last_path
	variable filtered
	variable recs
	variable match
	variable magic
	if {!$restore_pending} return
	set restore_pending 0
	set match 0xFFFF
	set i 0
	foreach idx $filtered {
		set rec [lindex $recs $idx]
		if {![rec_is_folder $rec]} {
			if {$last_source == 2} {
				if {[rec_is_sd $rec] && [lindex $rec 3] ne "" && [string equal -nocase [lindex $rec 3] $last_path]} {
					set match $i
					break
				}
			} elseif {![rec_is_sd $rec] && [string trimright [lindex $rec 0]] eq $last_path} {
				set match $i
				break
			}
		}
		incr i
	}
	ctrl_set ack $magic
}

proc resolve_rename_match {} {
	variable rename_pending
	variable filtered
	variable recs
	variable match
	variable magic
	if {$rename_pending eq ""} return
	lassign $rename_pending sd size name
	set rename_pending ""
	set match 0xFFFF
	set i 0
	foreach idx $filtered {
		set rec [lindex $recs $idx]
		if {![rec_is_folder $rec] && ([rec_is_sd $rec] != 0) == $sd && [lindex $rec 2] == $size &&
		    [lindex $rec 0] eq $name} {
			set match $i
			break
		}
		incr i
	}
	ctrl_set ack $magic
}

# ---------------------------------------------------------------------------
# Flash ROM store (copy to flash, delete, rename of flash entries)
# ---------------------------------------------------------------------------

proc flash_record_bytes {name mapper size offset} {
	return "[binary format A71 $name][binary format cii $mapper $size $offset]"
}

proc flash_tombstone {slot} {
	variable image_file
	variable config_offset
	variable record_size
	patch_file $image_file [expr {$config_offset + $slot * $record_size}] "\x00"
}

# Rewrite the config area with the live entries (all 204 slots were used).
proc flash_compact {} {
	variable image_file
	variable config_offset
	variable config_size
	variable flash_recs
	variable overflow
	if {$overflow} {return 0}
	set cfg ""
	foreach f $flash_recs {
		lassign $f name mapper size offset
		append cfg [flash_record_bytes $name $mapper $size $offset]
	}
	append cfg [string repeat "\xFF" [expr {$config_size - [string length $cfg]}]]
	patch_file $image_file $config_offset $cfg
	load_flash_records
	return 1
}

proc flash_append {name mapper size offset} {
	variable image_file
	variable config_offset
	variable config_slots
	variable record_size
	variable slots_used
	if {$slots_used >= $config_slots && ![flash_compact]} {return 0}
	patch_file $image_file [expr {$config_offset + $slots_used * $record_size}] \
		[flash_record_bytes $name $mapper $size $offset]
	load_flash_records
	return 1
}

# Lowest 4KB aligned gap after the hidden payloads that shares no sector with
# a live entry (flash_store_find_space), or -1.
proc flash_find_space {size} {
	variable flash_recs
	variable store_offset
	variable sector
	variable flash_size
	variable menu_pos
	set span [align_up $size $sector]
	set used [list]
	foreach f $flash_recs {
		lassign $f name mapper fsize offset
		if {$fsize == 0} continue
		lappend used [list [expr {$offset / $sector * $sector}] [align_up [expr {$offset + $fsize}] $sector]]
	}
	set cursor $store_offset
	foreach u [lsort -integer -index 0 $used] {
		lassign $u s e
		if {$cursor + $span <= $s} break
		if {$e > $cursor} {set cursor $e}
	}
	if {$cursor + $span > $flash_size - $menu_pos} {return -1}
	return $cursor
}

proc flash_name_taken {name {except -1}} {
	variable flash_recs
	set i 0
	foreach f $flash_recs {
		if {$i != $except && [string equal -nocase [lindex $f 0] $name]} {return 1}
		incr i
	}
	return 0
}

proc rename_sidecar {from to} {
	set hfrom [sd_host $from]
	set hto [sd_host $to]
	if {$from eq $to || ![file isfile $hfrom]} return
	if {[file exists $hto] && ![string equal -nocase $hfrom $hto]} {catch {file delete $hto}}
	catch {file rename -force $hfrom $hto}
}

proc delete_entry {} {
	variable flash_recs
	ctrl_set ack 0
	set rec [filtered_rec [query_index]]
	if {$rec eq "" || [rec_is_folder $rec]} return
	if {![rec_is_sd $rec]} {
		set f [lindex $flash_recs [lindex $rec 4]]
		if {$f eq ""} return
		flash_tombstone [lindex $f 4]
		load_flash_records
		catch {file delete [sd_host "/[lindex $f 0].flash.PVC"]}
	} else {
		set path [lindex $rec 3]
		if {$path eq "" || [catch {file delete [sd_host $path]}]} return
		set p [pvc_path $rec]
		if {$p ne ""} {catch {file delete [sd_host $p]}}
		sd_note
	}
	refresh
	ctrl_set ack 1
}

proc copy_to_flash {} {
	variable job
	variable flash_recs
	variable max_flash
	variable overflow
	variable slots_used
	variable config_slots
	variable mapper_dsk
	ctrl_set ack 0
	ctrl_set mapper 0
	if {$job ne ""} {ctrl_set cmd 0; return}
	set rec [filtered_rec [query_index]]
	lassign $rec name mapper size path
	if {$rec eq "" || ![rec_is_sd $rec] || [rec_is_folder $rec] || [rec_is_mp3 $rec] || $path eq "" ||
	    $size < 8192 || $size > 0x400000 || [llength $flash_recs] >= $max_flash || $overflow ||
	    [flash_name_taken $name]} {
		ctrl_set cmd 0
		return
	}
	set host [sd_host $path]
	set code [code_of $mapper]
	if {$code == 0} {set code [lindex [tag_info [file tail $host]] 1]}
	if {$code == 0} {set code [detect_rom $host]}
	if {$code == 0 || $code == $mapper_dsk || [is_system_code $code] ||
	    ($slots_used >= $config_slots && ![flash_compact])} {
		ctrl_set cmd 0
		return
	}
	set addr [flash_find_space $size]
	if {$addr < 0 || [catch {set data [read_binary $host]}]} {
		ctrl_set cmd 0
		return
	}
	sd_note
	# Erasing and programming take a while on the cartridge (about 200KB/s);
	# report the percentage in CTRL_MAPPER like the firmware does.
	set steps [expr {$size / 20480 > 15 ? $size / 20480 : 15}]
	set job [dict create name $name mapper $code size $size addr $addr data $data \
		pvc [pvc_path $rec] step 0 steps $steps]
	after time 0.1 picoverse2350::copy_step
}

proc copy_step {} {
	variable job
	variable state
	variable image_file
	if {$job eq ""} return
	if {$state ne "menu"} {
		set job ""
		return
	}
	dict incr job step
	dict with job {
		if {$step < $steps} {
			ctrl_set mapper [expr {$step * 100 / $steps}]
			sd_note
			sync_ctrl
			after time 0.1 picoverse2350::copy_step
			return
		}
	}
	set j $job
	set job ""
	dict with j {
		patch_file $image_file $addr $data
		set ok [flash_append $name $mapper $size $addr]
		if {$ok && $pvc ne "" && [file isfile [sd_host $pvc]]} {
			catch {file copy -force [sd_host $pvc] [sd_host "/$name.flash.PVC"]}
		}
	}
	ctrl_set mapper 100
	refresh
	ctrl_set ack $ok
	ctrl_set cmd 0
	sync_ctrl
}

proc clean_rename {list} {
	set name [string range [bytes_to_string $list] 0 69]
	set name [string trimleft $name " "]
	set name [string trimright $name " ."]
	if {$name eq "" || [regexp {[^\x20-\x7e]|[\\/:*?"<>|]} $name]} {return ""}
	return $name
}

proc sd_rename {rec name} {
	set old [lindex $rec 3]
	if {$old eq ""} {return 0}
	set slash [string last "/" $old]
	set base [string range $old [expr {$slash + 1}] end]
	set suffix [string range $base [lindex [tag_info $base] 0] end]
	set new "[string range $old 0 $slash]$name$suffix"
	if {$new eq $old} {return 1}
	set hold [sd_host $old]
	set hnew [sd_host $new]
	if {[file exists $hnew] && ![string equal -nocase $hold $hnew]} {return 0}
	# A case-only rename needs a detour on case-insensitive file systems.
	if {[string equal -nocase $hold $hnew]} {
		set tmp "$hold.pvtmp"
		if {[catch {file rename $hold $tmp; file rename $tmp $hnew}]} {return 0}
	} elseif {[catch {file rename $hold $hnew}]} {
		return 0
	}
	sd_note
	set is_dsk [rec_is_dsk $rec]
	rename_sidecar [pvc_from_rom $old $is_dsk] [pvc_from_rom $new $is_dsk]
	return 1
}

proc flash_rename {rec name} {
	variable job
	variable overflow
	variable flash_recs
	variable slots_used
	variable config_slots
	if {$job ne "" || $overflow} {return 0}
	set f [lindex $flash_recs [lindex $rec 4]]
	if {$f eq ""} {return 0}
	lassign $f old mapper size offset
	if {$old eq $name} {return 1}
	if {[flash_name_taken $name [lindex $rec 4]]} {return 0}
	if {$slots_used >= $config_slots && ![flash_compact]} {return 0}
	# The entry may have moved to another slot during compaction.
	foreach f $flash_recs {
		if {[lindex $f 0] eq $old && [lindex $f 2] == $size && [lindex $f 3] == $offset} break
	}
	set old_slot [lindex $f 4]
	if {![flash_append $name $mapper $size $offset]} {return 0}
	flash_tombstone $old_slot
	load_flash_records
	rename_sidecar "/$old.flash.PVC" "/$name.flash.PVC"
	return 1
}

proc rename_entry {} {
	variable rename_buf
	variable rename_pending
	ctrl_set ack 0
	set name [clean_rename $rename_buf]
	set rec [filtered_rec [query_index]]
	if {$name eq "" || $rec eq "" || [rec_is_folder $rec] || [rec_is_mp3 $rec]} return
	set sd [expr {[rec_is_sd $rec] != 0}]
	if {$sd ? ![sd_rename $rec $name] : ![flash_rename $rec $name]} return
	set rename_pending [list $sd [lindex $rec 2] $name]
	ctrl_set ack 1
	refresh
}

# ---------------------------------------------------------------------------
# Menu commands and the MSX write handler (handle_menu_write_explorer)
# ---------------------------------------------------------------------------

proc detect_mapper_cmd {} {
	variable recs
	variable filtered
	ctrl_set mapper 0
	set idx [query_index]
	if {$idx >= [llength $filtered]} return
	set ri [lindex $filtered $idx]
	set rec [lindex $recs $ri]
	set flags [expr {[lindex $rec 1] & 0xE0}]
	if {![rec_is_sd $rec] || [rec_is_folder $rec] || [rec_is_mp3 $rec] || [lindex $rec 3] eq ""} {
		ctrl_set mapper [code_of [lindex $rec 1]]
		return
	}
	set host [sd_host [lindex $rec 3]]
	sd_note
	set m [lindex [tag_info [file tail $host]] 1]
	if {$m == 0} {set m [detect_rom $host]}
	ctrl_set mapper $m
	lset recs $ri 1 [expr {$flags | $m}]
}

proc set_mapper_cmd {} {
	variable recs
	variable filtered
	variable query
	ctrl_set ack 0
	set idx [query_index]
	set m [lindex $query 2]
	if {$idx >= [llength $filtered] || $m == 0 || [is_system_code $m] || $m > 22} return
	set ri [lindex $filtered $idx]
	set rec [lindex $recs $ri]
	set flags [expr {[lindex $rec 1] & 0xE0}]
	if {($flags & 0x60) || [rec_is_dsk $rec]} return
	lset recs $ri 1 [expr {$flags | $m}]
	ctrl_set ack 1
}

proc enter_dir {} {
	variable query
	variable sd_path
	variable magic
	if {[lindex $query 2] == $magic} {
		set rec [filtered_rec [query_index]]
		if {$rec eq "" || ![rec_is_folder $rec]} return
		set folder [lindex $rec 0]
	} else {
		set folder [query_text]
	}
	if {$folder eq "" || $folder eq ".."} {
		set slash [string last "/" $sd_path]
		set sd_path [expr {$slash > 0 ? [string range $sd_path 0 [expr {$slash - 1}]] : "/"}]
	} else {
		set sd_path [sd_join $sd_path $folder]
	}
}

proc set_source {} {
	variable query
	variable browse
	variable sd_path
	variable restore_pending
	variable last_source
	variable last_dir
	set mode [lindex $query 0]
	if {$mode > 2} {set mode 0}
	set browse $mode
	set sd_path "/"
	if {$restore_pending && $mode == $last_source && [file isdirectory [sd_host $last_dir]]} {
		set sd_path $last_dir
	}
	refresh
}

proc command {cmd} {
	variable query
	variable browse
	variable sd_path
	ctrl_set cmd $cmd
	switch -- $cmd {
		1 {
			lset query 31 0
			apply_filter
			build_page 0
		}
		2  {find_first}
		3  {enter_dir; refresh}
		4  {detect_mapper_cmd}
		5  {set_mapper_cmd}
		6  {set_source}
		7  {load_options}
		8  {save_options}
		9  {quick_run}
		10 {
			set browse 2
			set sd_path "/"
			refresh
			ctrl_set ack 1
		}
		11 {delete_entry}
		12 {load_last_selection}
		13 {save_last_selection}
		14 {copy_to_flash}
		15 {rename_entry}
		64 - 66 - 65 {fh_offline}
		67 {ctrl_set net 0}
	}
	if {$cmd != 14} {ctrl_set cmd 0}
}

# File Hunter needs the WiFi module: answer like the firmware does when the
# network is offline, with a single "Offline" message record.
proc fh_offline {} {
	variable fh_active
	variable data_base
	variable data_size
	variable part_base
	set fh_active 1
	set rec "[binary format a71 Offline][binary format cs 0x80 0]"
	put $data_base "$rec[string repeat \xFF [expr {$data_size - [string length $rec]}]]"
	put $part_base [binary format a64 "WiFi not emulated"]
	ctrl_set count 1
	ctrl_set audio 0
}

proc handle_write {addr value} {
	variable query
	variable rename_buf
	variable query_base
	variable data_base
	variable mp3_status
	variable mp3_index
	variable mp3_mode
	variable monitor_addr

	if {$addr >= $query_base && $addr < $query_base + 32} {
		lset query [expr {$addr - $query_base}] $value
		return
	}
	if {$addr >= $data_base && $addr < $data_base + 71} {
		lset rename_buf [expr {$addr - $data_base}] $value
		return
	}
	switch -- [format %04X $addr] {
		BFF4 {command $value}
		BFF2 {
			variable fh_active
			if {$fh_active} {
				ctrl_set page $value
			} elseif {$value != [ctrl_get page]} {
				build_page $value
			}
		}
		BFF9 {ctrl_set audio $value}
		BFFA {ctrl_set wifi [expr {$value ? 1 : 0}]}
		BFFB {ctrl_set psg [expr {$value ? 1 : 0}]}
		BFFD {
			ctrl_set sd_part [expr {$value >= 1 && $value <= 4 ? $value : 0}]
			sunrise_partition_info $value
		}
		BFFF {ctrl_set volume [expr {$value <= 200 ? $value : 100}]}
		BFE0 {
			# MP3/WAV playback is not emulated: report an error to the player.
			switch -- $value {
				1 - 2 {set mp3_status 0x04}
				3 {set mp3_status 0}
			}
		}
		BFE2 {set mp3_index [expr {($mp3_index & 0xFF00) | $value}]}
		BFE3 {set mp3_index [expr {($mp3_index & 0xFF) | ($value << 8)}]}
		BFE8 {
			if {$value <= 2} {set mp3_mode $value}
		}
		BF7F {on_select $value}
		default return
	}
	sync_ctrl
}

proc on_write {} {
	variable state
	variable mirror
	variable dbg
	if {$state ni {menu placeholder}} return
	set addr $::wp_last_address
	# Only the menu talks to the Pico; the BIOS RAM probe at boot does not.
	if {$state eq "menu" && [reg pc] >= 0x4000 && [catch {handle_write $addr $::wp_last_value} err]} {
		notify "internal error: $err" error
	}
	# The MSX does not see RAM: put back what the Pico serves.
	set off [expr {$addr - 0x4000}]
	catch {debug write $dbg $off [scan [string index $mirror $off] %c]}
}

# Watchpoint condition: the write reaches the PicoVerse cartridge.
proc in_cart {} {
	variable slot_ps
	variable slot_ss
	set page [expr {$::wp_last_address >> 14}]
	set ps [expr {([debug read "ioports" 0xA8] >> (2 * $page)) & 3}]
	if {$ps != $slot_ps} {return 0}
	if {$slot_ss eq "X"} {return 1}
	set ss_reg [debug read "slotted memory" [expr {0x40000 * $ps + 0xFFFF}]]
	expr {((($ss_reg ^ 255) >> (2 * $page)) & 3) == $slot_ss}
}

proc on_select {value} {
	variable pending
	variable last_index
	if {$value == 0xFE || $pending} return
	set idx $value
	# The register is 8-bit: use the full index of the entry the menu just
	# stored as last selection.
	if {$last_index != 0xFFFF && ($last_index & 0xFF) == $value} {set idx $last_index}
	set rec [filtered_rec $idx]
	if {$rec eq "" || [rec_is_folder $rec] || [rec_is_mp3 $rec]} return
	set opts [dict create]
	foreach k {audio psg wifi sd_part volume vdp cpu} {dict set opts $k [ctrl_get $k]}
	set pending 1
	after realtime 0 [list picoverse2350::boot_record $rec $opts]
}

# ---------------------------------------------------------------------------
# Starting an entry
# ---------------------------------------------------------------------------

# resolve_audio_mode() of the firmware
proc audio_mode {code profile} {
	set system [is_system_code $code]
	if {$system} {
		if {$profile == 4 && $code != 21} {return msx_music}
		if {$code in {19 20 21}} {
			switch -- $profile {
				10 {return megaram_sccp}
				9  {return megaram_scc}
			}
			return none
		}
	}
	switch -- $profile {
		6 {return sccp_ext}
		5 {return scc_ext}
		7 - 11 {return sfg05}
		8 - 12 {return sfg01}
	}
	if {$system} {
		return [expr {$profile == 3 ? "dual_psg" : "none"}]
	}
	if {$code == 9} {return none}
	if {$profile == 4} {return msx_music}
	if {$code in {3 14}} {
		switch -- $profile {
			2 {return sccp}
			1 {return scc}
		}
		return none
	}
	expr {$profile == 3 ? "dual_psg" : "none"}
}

proc cache_dir {} {
	file join [persistent_dir] cache
}

# Write a cache file unless an identical one already exists. openMSX may keep
# files open while they are in use, so a busy file is written under a new name.
proc cache_file {name data} {
	set dir [cache_dir]
	file mkdir $dir
	set path [file join $dir $name]
	set root [file rootname $path]
	set ext [file extension $path]
	for {set n 1} {$n < 100} {incr n} {
		if {[file exists $path] && [file size $path] == [string length $data]} {
			if {[read_binary $path] eq $data} {return $path}
		}
		if {![catch {write_binary $path $data}]} {return $path}
		set path "${root}_$n$ext"
	}
	error "unable to write cache file $path"
}

proc is_cached {file} {
	set root "[cache_dir]/"
	string equal -nocase -length [string length $root] $root $file
}

proc default_hd_image {} {
	file join [persistent_dir] hd.dsk
}

proc hd_path {} {
	variable hd_image
	if {$hd_image ne ""} {return $hd_image}
	if {$::picoverse2350_hd ne ""} {return [file normalize $::picoverse2350_hd]}
	return [default_hd_image]
}

proc rom_bytes {rec} {
	variable image_file
	variable flash_recs
	if {[rec_is_sd $rec]} {
		return [read_binary [sd_host [lindex $rec 3]]]
	}
	lassign [lindex $flash_recs [lindex $rec 4]] name mapper size offset
	read_range $image_file $offset $size
}

proc pad_rom {data code} {
	variable romtypes
	lassign [dict get $romtypes $code] romtype fixed bank
	if {$fixed > 0} {
		set data [string range $data 0 [expr {$fixed - 1}]]
		set pad [expr {$fixed - [string length $data]}]
	} else {
		set pad [expr {($bank - [string length $data] % $bank) % $bank}]
	}
	if {$pad > 0} {append data [string repeat "\xFF" $pad]}
	return $data
}

proc ide_xml {rom_file hd} {
	return "<SunriseIDE id=\"PicoVerse 2350 Sunrise IDE\">
          <mem base=\"0x0000\" size=\"0x10000\"/>
          <rom><filename>[xml_escape $rom_file]</filename></rom>
          <master>
            <type>IDEHD</type>
            <filename>[xml_escape $hd]</filename>
            <size>100</size>
            <name>PicoVerse microSD</name>
          </master>
        </SunriseIDE>"
}

# Devices of an audio profile: {subslot xml} pairs and I/O-only devices.
proc audio_devices {mode var_slots var_io} {
	variable image_file
	variable fmpac_offset
	variable sfg_offset
	upvar $var_slots slots $var_io io
	switch -- $mode {
		scc_ext - megaram_scc {
			dict set slots 2 "<ROM id=\"PicoVerse 2350 SCC\">
          <mem base=\"0x4000\" size=\"0x8000\"/>
          <sound><volume>11500</volume></sound>
          <mappertype>SCC</mappertype>
          <rom/>
        </ROM>"
		}
		sccp_ext - megaram_sccp {
			dict set slots 2 "<SCCplus id=\"PicoVerse 2350 SCC+\">
          <mem base=\"0x4000\" size=\"0x8000\"/>
          <subtype>expanded</subtype>
          <sound><volume>13000</volume></sound>
        </SCCplus>"
		}
		sfg05 - sfg01 {
			set second [expr {$mode eq "sfg01"}]
			set bios [cache_file "$mode.rom" [read_range $image_file [expr {$sfg_offset + $second * 0x8000}] 0x8000]]
			dict set slots 2 "<YamahaSFG id=\"PicoVerse 2350 [string toupper [string range $mode 0 2]]-[string range $mode 3 end]\">
          <variant>[expr {$second ? "YM2151" : "YM2164"}]</variant>
          <mem base=\"0x0000\" size=\"0x10000\"/>
          <sound><volume>30000</volume></sound>
          <rom><filename>[xml_escape $bios]</filename></rom>
        </YamahaSFG>"
		}
		msx_music {
			set bios [cache_file "fmpac.rom" [read_range $image_file $fmpac_offset 0x10000]]
			set sub [expr {[dict exists $slots 3] ? 2 : 3}]
			dict set slots $sub "<FMPAC id=\"PicoVerse 2350 FM-PAC\">
          <io base=\"0x7C\" num=\"2\" type=\"O\"/>
          <mem base=\"0x4000\" size=\"0x4000\"/>
          <sound><volume>13000</volume></sound>
          <rom><filename>[xml_escape $bios]</filename></rom>
          <sramname>picoverse2350_fmpac.pac</sramname>
        </FMPAC>"
		}
		dual_psg {
			append io "<PSG id=\"PicoVerse 2350 PSG 2\">
      <io base=\"0x10\" num=\"4\" type=\"IO\"/>
      <sound><volume>21000</volume></sound>
    </PSG>"
		}
	}
}

# Write the openMSX extension that starts an entry and return its name: an
# expanded slot when several devices share the cartridge, like on the
# PicoVerse (subslot 0 = ROM/Nextor, 1 = mapper, 2 = SCC/SFG, 3 = FM-PAC or
# MegaRAM). The file lives in the persistent folder, so openMSX doesn't list
# it as an extension.
proc write_boot_extension {title slots io} {
	variable boot_ext
	if {[dict size $slots] == 1 && [dict exists $slots 0]} {
		set body "    <primary slot=\"any\">
      <secondary slot=\"any\">
        [dict get $slots 0]
      </secondary>
    </primary>"
	} elseif {[dict size $slots] > 0} {
		set body "    <primary slot=\"any\">"
		foreach sub {0 1 2 3} {
			if {[dict exists $slots $sub]} {
				append body "\n      <secondary slot=\"$sub\">\n        [dict get $slots $sub]\n      </secondary>"
			} else {
				append body "\n      <secondary slot=\"$sub\"/>"
			}
		}
		append body "\n    </primary>"
	} else {
		set body ""
	}
	set xml "<?xml version=\"1.0\" ?>
<!DOCTYPE msxconfig SYSTEM 'msxconfig2.dtd'>
<!-- Generated by picoverse2350.tcl, do not edit: it is rewritten on use. -->
<msxconfig>
  <info>
    <name>[xml_escape $title]</name>
    <manufacturer>The Retro Hacker</manufacturer>
    <code/>
    <release_year>2026</release_year>
    <description>Used internally by picoverse2350.tcl to start a PicoVerse 2350 entry.</description>
    <type>multi-ROM cartridge</type>
  </info>
  <devices>
$body
    $io
  </devices>
</msxconfig>
"
	set dir [file join [persistent_dir] extensions]
	file mkdir $dir
	write_binary [file join $dir $boot_ext.xml] $xml
	return "../../persistent/picoverse2350/extensions/$boot_ext"
}

proc boot_record {rec opts} {
	variable pending
	variable state
	set pending 0
	if {[catch {start_record $rec $opts} err]} {
		notify "[lindex $rec 0]: $err" error
		set state "rom"
		restore_menu
	}
}

proc start_record {rec opts} {
	variable pending
	variable slot
	variable current
	variable state
	variable own_boot
	variable running
	variable romtypes
	variable image_file
	variable dsk_offset
	variable mapper_dsk

	set pending 0
	lassign $rec name mapper size path
	set code [code_of $mapper]
	if {$code == 0 && [rec_is_sd $rec]} {set code [detect_rom [sd_host $path]]}
	set audio [audio_mode $code [dict get $opts audio]]
	set slots [dict create]
	set io ""
	set what "ROM"

	if {$code in {17 18}} {
		notify "$name: the Carnivore2 RAM registers are not emulated, starting Nextor with the 1MB mapper" warning
	}
	if {$code == $mapper_dsk} {
		if {$audio ni {scc_ext sccp_ext msx_music dual_psg sfg05 sfg01}} {set audio none}
		set kernel [cache_file "nextor_dsk.rom" [read_range $image_file $dsk_offset 0x20000]]
		dict set slots 0 [ide_xml $kernel [sd_host $path]]
		if {[dict get $opts wifi]} {
			dict set slots 1 [mapper_xml]
		}
		set what "DSK"
	} elseif {[is_system_code $code]} {
		if {$code == 21} {
			dict set slots 0 [megaram_xml]
		} else {
			set kernel [cache_file "[clean_name $name].rom" [rom_bytes $rec]]
			set hd [hd_path]
			file mkdir [file dirname $hd]
			dict set slots 0 [ide_xml $kernel $hd]
			if {$code in {11 16 17 18 19 20}} {dict set slots 1 [mapper_xml]}
			if {$code in {19 20}} {dict set slots 3 [megaram_xml]}
		}
		set what "Nextor"
	} else {
		if {![dict exists $romtypes $code]} {
			set romtype ""
			set data [rom_bytes $rec]
		} else {
			set romtype [lindex [dict get $romtypes $code] 0]
			set data [pad_rom [rom_bytes $rec] $code]
		}
		set file [cache_file "[clean_name $name].rom" $data]
		set mt [expr {$romtype ne "" ? "<mappertype>$romtype</mappertype>" : ""}]
		dict set slots 0 "<ROM id=\"PicoVerse 2350 ROM\">
          $mt
          <rom><filename>[xml_escape $file]</filename></rom>
          <mem base=\"0x0000\" size=\"0x10000\"/>
          <sound><volume>9000</volume></sound>
        </ROM>"
		set what [expr {$romtype ne "" ? $romtype : "auto"}]
	}
	audio_devices $audio slots io

	remove_watchpoint
	remove_current
	if {$what ni {DSK Nextor} && [dict size $slots] == 1 && $io eq ""} {
		cart$slot insert $file {*}[expr {$romtype ne "" ? [list -romtype $romtype] : ""}]
		set current [list cart $file]
	} else {
		set current [list ext [ext$slot [write_boot_extension "PicoVerse 2350: $name" $slots $io]]]
	}
	set state "rom"
	set running $name
	set note [expr {$audio ne "none" ? " + $audio" : ""}]
	notify "$name ($what$note)" info
	set own_boot [expr {$::power ? 1 : 0}]
	reset
}

proc mapper_xml {} {
	return "<MemoryMapper id=\"PicoVerse 2350 1MB Mapper\">
          <mem base=\"0x0000\" size=\"0x10000\"/>
          <size>1024</size>
        </MemoryMapper>"
}

proc megaram_xml {} {
	return "<MegaRam id=\"PicoVerse 2350 MegaRAM\">
          <io base=\"0x8E\" num=\"1\"/>
          <mem base=\"0x0000\" size=\"0x10000\"/>
          <size>1024</size>
        </MegaRam>"
}

# ---------------------------------------------------------------------------
# Cartridge slot handling
# ---------------------------------------------------------------------------

proc resolve_slot {} {
	variable slot
	variable slot_ps
	variable slot_ss
	set info [machine_info external_slot slot$slot]
	set slot_ps [lindex $info 0]
	set slot_ss [lindex $info 1]
}

proc remove_current {} {
	variable current
	variable slot
	lassign $current kind what
	switch -- $kind {
		cart {catch {cart$slot eject}}
		ext  {catch {remove_extension $what}}
	}
	set current ""
}

proc remove_watchpoint {} {
	variable wp_id
	if {$wp_id ne ""} {
		catch {debug remove_watchpoint $wp_id}
		set wp_id ""
	}
}

proc install_watchpoint {} {
	variable wp_id
	remove_watchpoint
	set wp_id [debug set_watchpoint write_mem {0x4000 0xBFFF} \
		{[picoverse2350::in_cart]} picoverse2350::on_write]
}

proc is_menu_ext {name} {
	variable ext_config
	expr {$name eq $ext_config || [string match "$ext_config (*)" $name]}
}

proc is_boot_ext {name} {
	variable boot_ext
	string match "${boot_ext}*" [file tail $name]
}

# Reset the emulated Pico, as when the cartridge is powered on.
proc reset_pico {} {
	variable browse
	variable sd_path
	variable query
	variable rename_buf
	variable match
	variable mp3_status
	variable mp3_index
	variable mp3_mode
	variable job
	variable restore_pending
	variable rename_pending
	variable last_index
	variable last_source
	variable last_path
	set browse 0
	set sd_path "/"
	set query [lrepeat 32 0]
	set rename_buf [lrepeat 71 0]
	set match 0xFFFF
	set mp3_status 0
	set mp3_index 0
	set mp3_mode 0
	set job ""
	set restore_pending 0
	set rename_pending ""
	set last_index 0xFFFF
	set last_source 0
	set last_path ""
	ctrl_reset
	init_mirror
	refresh
}

# Make sure the PicoVerse_2350 extension is in the slot and serve the menu.
proc insert_menu {} {
	variable slot
	variable current
	variable state
	variable pending
	variable ext_config
	remove_watchpoint
	set inserted [lindex [machine_info external_slot slot$slot] 2]
	if {![is_menu_ext $inserted]} {
		remove_current
		set inserted [ext$slot $ext_config]
	}
	set current [list ext $inserted]
	resolve_slot
	reset_pico
	serve_window
	install_watchpoint
	set state "menu"
	set pending 0
}

# Without a UF2 the extension shows how to select one.
proc insert_placeholder {} {
	variable mirror
	variable state
	set msg "\x0CPicoVerse 2350 Explorer\r\n\r\nNo UF2 selected. Choose the\r\n.uf2 as ROM image of this\r\nslot, or type in the openMSX\r\nconsole (F10):\r\n\r\npicoverse2350 insert <uf2>\r\n\x00"
	set code "AB\x10\x40[string repeat \x00 12]\x21\x21\x40\x7E\xB7\x28\x06\xCD\xA2\x00\x23\x18\xF6\xFB\x76\x18\xFC$msg"
	set mirror "$code[string repeat \xFF [expr {0x8000 - [string length $code]}]]"
	serve_window
	install_watchpoint
	set state "placeholder"
}

# Powering the MSX off brings back the menu.
proc on_power_change {args} {
	variable state
	variable own_boot
	if {!$::power} {
		set own_boot 0
		if {$state eq "rom"} {
			after realtime 0 picoverse2350::restore_menu
		}
	}
}

# Any other reset of the MSX brings back the menu; only the reset issued by
# boot_record keeps the selected entry. A power cycle clears the window RAM,
# so it is written again.
proc on_boot {} {
	variable state
	variable own_boot
	variable dbg
	after boot picoverse2350::on_boot
	if {$own_boot} {
		set own_boot 0
		return
	}
	if {[replaying]} return
	if {$state eq "rom"} {
		after realtime 0 picoverse2350::restore_menu
	} elseif {$state in {menu placeholder}} {
		serve_window
	}
}

proc replaying {} {
	expr {![catch {dict get [reverse status] status} status] && $status eq "replaying"}
}

proc restore_menu {} {
	variable state
	variable uf2_file
	if {$state ne "rom"} return
	if {$uf2_file eq "" && [catch {
		if {$::picoverse2350_uf2 eq ""} {error "no UF2 selected, use 'picoverse2350 insert <file.uf2>'"}
		load_uf2 $::picoverse2350_uf2
	} err]} {
		notify $err error
		return
	}
	if {[catch {insert_menu} err]} {
		notify $err error
		return
	}
	if {$::power} reset
}

# Track the cartridge across machine switches and savestate loads.
proc on_machine_switch {} {
	variable state
	variable current
	variable wp_id
	variable slot
	variable pending
	variable own_boot
	variable machine_id
	variable uf2_file
	variable job

	after machine_switch picoverse2350::on_machine_switch
	set wp_id ""
	set pending 0
	set own_boot 0
	set job ""
	set state "idle"
	set current ""
	catch {set machine_id [machine]}
	if {[catch {set slots [machine_info external_slot]}]} return
	foreach s $slots {
		set inserted [lindex [machine_info external_slot $s] 2]
		set letter [string range $s 4 end]
		if {[is_menu_ext $inserted] && $uf2_file ne ""} {
			set slot $letter
			if {[catch {insert_menu}]} return
			return
		}
		if {[is_boot_ext $inserted] || [is_cached $inserted]} {
			set slot $letter
			catch resolve_slot
			set current [list [expr {[is_boot_ext $inserted] ? "ext" : "cart"}] $inserted]
			set state "rom"
			return
		}
	}
}

# ---------------------------------------------------------------------------
# User command
# ---------------------------------------------------------------------------

proc parse_options {arglist} {
	variable slot
	variable hd_image
	variable sd_folder
	set file ""
	set new_slot $::picoverse2350_slot
	set new_hd ""
	set new_sd ""
	for {set i 0} {$i < [llength $arglist]} {incr i} {
		set arg [lindex $arglist $i]
		switch -- $arg {
			-slot {set new_slot [string tolower [lindex $arglist [incr i]]]}
			-hd   {set new_hd [lindex $arglist [incr i]]}
			-sd   {set new_sd [lindex $arglist [incr i]]}
			default {
				if {$file ne ""} {error "unexpected argument: $arg"}
				set file $arg
			}
		}
	}
	if {[string length $new_slot] != 1 || [info commands ::cart$new_slot] eq ""} {
		error "invalid cartridge slot '$new_slot' (available: [string map {slot {}} [machine_info external_slot]])"
	}
	if {$new_sd ne "" && ![file isdirectory $new_sd]} {
		error "microSD folder not found: $new_sd"
	}
	set slot $new_slot
	set hd_image [expr {$new_hd ne "" ? [file normalize $new_hd] : ""}]
	set sd_folder [expr {$new_sd ne "" ? [file normalize $new_sd] : ""}]
	return $file
}

proc cmd_insert {arglist} {
	variable uf2_file
	variable state
	variable slot
	variable flash_recs
	set old_slot $slot
	set file [parse_options $arglist]
	if {$file eq ""} {
		set file [expr {$uf2_file ne "" ? $uf2_file : $::picoverse2350_uf2}]
		if {$file eq ""} {error "no UF2 file given"}
	}
	if {$state ne "idle" && $old_slot ne "" && $old_slot ne $slot} {
		set new_slot $slot
		set slot $old_slot
		remove_watchpoint
		remove_current
		set slot $new_slot
		set state "idle"
	}
	load_uf2 $file
	insert_menu
	set ::picoverse2350_uf2 $uf2_file
	reset
	return "PicoVerse 2350 inserted in slot $slot: [file tail $uf2_file] ([llength $flash_recs] flash entries, microSD: [sd_root])"
}

proc mapper_name {mapper} {
	variable tags
	set code [code_of $mapper]
	switch -- $code {
		10 {return "Nextor USB"}
		11 {return "Nextor USB+Mapper"}
		15 {return "Nextor SD"}
		16 {return "Nextor SD+Mapper"}
		17 - 18 {return "Carnivore2"}
		19 - 20 {return "Nextor+MegaRAM"}
		21 {return "MegaRAM"}
		23 {return "DSK"}
	}
	if {$code >= 1 && $code <= [llength $tags]} {return [lindex $tags [expr {$code - 1}]]}
	return "?"
}

proc cmd_list {} {
	variable flash_recs
	if {$flash_recs eq ""} {return "No flash entries (no UF2 loaded?)."}
	set result ""
	set i 0
	foreach f $flash_recs {
		lassign $f name mapper size
		append result [format "%3d  %-60s %-17s %8d\n" [incr i] $name [mapper_name $mapper] $size]
	}
	return $result
}

proc cmd_info {} {
	variable uf2_file
	variable image_file
	variable state
	variable slot
	variable flash_recs
	variable running
	variable version
	if {$uf2_file eq ""} {return "No PicoVerse 2350 UF2 loaded (script $version)."}
	set result "UF2 file    : $uf2_file\n"
	append result "Flash image : $image_file\n"
	append result "Slot        : $slot\n"
	append result "Flash       : [llength $flash_recs] entries\n"
	append result "microSD     : [sd_root]\n"
	append result "HD image    : [hd_path]\n"
	append result "State       : $state"
	if {$state eq "rom"} {append result " ($running)"}
	return $result
}

proc cmd_eject {} {
	variable state
	remove_watchpoint
	remove_current
	set state "idle"
	return "PicoVerse 2350 removed."
}

# Discard the flash changes (copies, renames, deletes): start again from the UF2.
proc cmd_reflash {} {
	variable uf2_file
	variable image_file
	if {$uf2_file eq ""} {error "no UF2 loaded, use 'picoverse2350 insert <file.uf2>'"}
	remove_watchpoint
	catch {file delete $image_file "$image_file.pos"}
	load_uf2 $uf2_file
	insert_menu
	reset
	return "Flash restored from [file tail $uf2_file]."
}

set_help_text picoverse2350 \
"Run a PicoVerse 2350 Explorer UF2 image in openMSX (script $version).

picoverse2350 insert <file.uf2> \[-slot <a|b>\] \[-sd <folder>\] \[-hd <image>\]
    Insert the PicoVerse 2350 cartridge built into the UF2 and reset the MSX.
    -slot  cartridge slot to use (default: setting picoverse2350_slot)
    -sd    folder used as microSD card until the next insert (default:
           setting picoverse2350_sd, or picoverse2350/sd)
    -hd    hard disk image used by the Nextor entries until the next insert
           (default: setting picoverse2350_hd, or picoverse2350/hd.dsk)
picoverse2350 insert
    Re-insert the last UF2 (setting picoverse2350_uf2).
picoverse2350 menu
    Return to the menu (same as resetting the MSX).
picoverse2350 list
    List the flash entries.
picoverse2350 info
    Show the current status.
picoverse2350 reflash
    Discard the flash changes made from the menu (copied, renamed and deleted
    entries) and start again from the UF2.
picoverse2350 eject
    Remove the PicoVerse 2350 cartridge.

Resetting the MSX, turning the power off and on, or restarting openMSX with
an entry still in the slot brings back the menu.

Instead of this command you can also insert the 'PicoVerse 2350 Explorer'
extension (it loads the UF2 in setting picoverse2350_uf2, the last UF2 used),
or select the .uf2 file as ROM image of a cartridge slot.

In this console backslashes are escape characters: write Windows paths with
forward slashes (C:/temp/explorer.uf2) or between braces ({C:\\temp\\explorer.uf2})."

proc picoverse2350 {args} {
	variable uf2_file
	set sub [lindex $args 0]
	set rest [lrange $args 1 end]
	switch -- $sub {
		insert  {return [cmd_insert $rest]}
		eject   {return [cmd_eject]}
		list    {return [cmd_list]}
		reflash {return [cmd_reflash]}
		info    -
		""      {return [cmd_info]}
		menu {
			if {$uf2_file eq ""} {error "no UF2 loaded, use 'picoverse2350 insert <file.uf2>'"}
			insert_menu
			reset
			return "PicoVerse 2350 menu restored."
		}
		default {
			if {[file isfile $sub]} {return [cmd_insert $args]}
			error "unknown subcommand '$sub', see 'help picoverse2350'"
		}
	}
}

proc tab_completion {args} {
	set sub [lindex $args 1]
	set prev [lindex $args end-1]
	if {[llength $args] == 2} {
		return [list insert eject menu list info reflash]
	}
	if {$sub eq "insert"} {
		if {$prev eq "-slot"} {
			return [string map {slot {}} [machine_info external_slot]]
		}
		return [concat [list -slot -sd -hd] [utils::file_completion {*}$args]]
	}
	return [list]
}

set_tabcompletion_proc picoverse2350 [namespace code tab_completion]

# ---------------------------------------------------------------------------
# Slot watcher: openMSX has no Tcl event for cartridge insertion, so the
# external slots are polled to catch the PicoVerse extension or a .uf2 file
# inserted from the GUI, the command line or the console.
# ---------------------------------------------------------------------------

proc take_over_uf2 {letter file} {
	catch {cart$letter eject}
	if {[catch {cmd_insert [list $file -slot $letter]} result]} {
		notify $result error
	} else {
		notify $result info
	}
}

# The extension was inserted by the user, or the slot still holds media of an
# earlier session (restored setup): serve the menu of the last UF2.
proc take_over {letter kind inserted} {
	variable uf2_file
	variable notified
	variable slot
	variable current
	set file [expr {$uf2_file ne "" ? $uf2_file : $::picoverse2350_uf2}]
	if {$file eq "" || ![file isfile $file]} {
		if {$kind eq "menu"} {
			set slot $letter
			set current [list ext $inserted]
			catch {resolve_slot; insert_placeholder; reset}
		}
		if {$notified ne $inserted} {
			set notified $inserted
			notify "no UF2 selected: choose the .uf2 file as ROM image in Media > Cartridge Slot, or type 'picoverse2350 insert <file.uf2>' in the console" warning
		}
		return
	}
	set notified ""
	switch -- $kind {
		ext  {catch {remove_extension $inserted}}
		cart {catch {cart$letter eject}}
	}
	if {[catch {cmd_insert [list $file -slot $letter]} result]} {
		notify $result error
	} else {
		notify $result info
	}
}

proc check_slots {} {
	variable state
	variable slot
	variable machine_id
	set settled [expr {[machine] eq $machine_id}]
	foreach s [machine_info external_slot] {
		set inserted [lindex [machine_info external_slot $s] 2]
		if {$inserted eq ""} continue
		set letter [string range $s 4 end]
		if {[string match -nocase *.uf2 $inserted] && [file isfile $inserted] && [is_rp2350_uf2 $inserted]} {
			take_over_uf2 $letter $inserted
			return
		}
		if {[is_menu_ext $inserted]} {
			if {$state ni {menu placeholder} || $letter ne $slot} {
				take_over $letter menu $inserted
				return
			}
		} elseif {$state eq "idle" && $settled} {
			if {[is_boot_ext $inserted]} {
				take_over $letter ext $inserted
				return
			}
			if {[is_cached $inserted]} {
				take_over $letter cart $inserted
				return
			}
		}
	}
}

proc watch_slots {} {
	variable watch_interval
	after realtime $watch_interval picoverse2350::watch_slots
	catch {check_slots}
}

trace add variable ::power write [namespace code on_power_change]
ctrl_reset
catch {set machine_id [machine]}
after machine_switch picoverse2350::on_machine_switch
after boot picoverse2350::on_boot
after realtime 0 picoverse2350::watch_slots

namespace export picoverse2350

} ;# namespace picoverse2350

namespace import picoverse2350::*
