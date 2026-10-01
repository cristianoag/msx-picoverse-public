; MSX PICOVERSE PROJECT
; (c) 2026 Cristiano Goncalves
; The Retro Hacker
;
; placeholder.asm - ROM of the "PicoVerse 2040 MultiROM" openMSX extension
;
; The picoverse2040.tcl script replaces this ROM with the PicoVerse menu of the
; selected UF2 as soon as the extension is inserted. The ROM only runs when no
; UF2 was selected yet (or when the script is not installed) and explains how
; to select one.
;
; Build: make (in the openmsx folder), requires SDCC (sdasz80, sdldz80, hex2bin)
;
; This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
; License". https://creativecommons.org/licenses/by-nc-sa/4.0/

        .module placeholder

CHPUT   = 0x00A2
INITXT  = 0x006C

        .area   _HEADER (ABS)
        .org    0x4000

        .db     0x41, 0x42              ; "AB"
        .dw     init                    ; INIT
        .dw     0x0000                  ; STATEMENT
        .dw     0x0000                  ; DEVICE
        .dw     0x0000                  ; TEXT
        .ds     6                       ; reserved

init:
        call    INITXT                  ; SCREEN 0, clear screen
        ld      hl, #message
next:
        ld      a, (hl)
        or      a
        jr      z, done
        call    CHPUT
        inc     hl
        jr      next
done:
        jr      done

message:
        .ascii  "PicoVerse 2040 MultiROM"
        .db     13, 10, 13, 10
        .ascii  "No UF2 image selected yet."
        .db     13, 10, 13, 10
        .ascii  "In openMSX select the UF2 at"
        .db     13, 10
        .ascii  "Media > Cartridge Slot >"
        .db     13, 10
        .ascii  "ROM image (All files filter)"
        .db     13, 10, 13, 10
        .ascii  "or type in the console (F10):"
        .db     13, 10
        .ascii  "picoverse2040 insert"
        .db     13, 10
        .ascii  "  C:/path/multirom.uf2"
        .db     13, 10, 13, 10
        .ascii  "Needs picoverse2040.tcl in"
        .db     13, 10
        .ascii  "openMSX share/scripts."
        .db     0
