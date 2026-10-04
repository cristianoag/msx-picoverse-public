// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// menu_input.c - BIOS keyboard input helpers for the MSX Explorer menu
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include "menu_input.h"
#include "menu.h"

unsigned char bios_chsns(void) __naked
{
    __asm
    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_CHSNS
    call    BIOS_CALSLT
    pop     ix
    jr      z, bios_chsns_no_key
    ld      l,a
    ld      h,#0
    ret

bios_chsns_no_key:
    xor     a
    ld      l,a
    ld      h,a
    ret
    __endasm;
}

// bios_chget_quiet - Like bios_chget, but waits with CHSNS first: CHGET draws
// the text cursor at the current position while it blocks for a key.
unsigned char bios_chget_quiet(void) __naked
{
    __asm
bios_chget_quiet_wait:
    call    _bios_chsns
    ld      a,l
    or      a
    jr      z, bios_chget_quiet_wait
    jp      _bios_chget
    __endasm;
}

unsigned char bios_chget(void) __naked
{
    __asm
    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_CHGET
    call    BIOS_CALSLT
    pop     ix
    ld      l,a
    ld      h,#0
    ret
    __endasm;
}
