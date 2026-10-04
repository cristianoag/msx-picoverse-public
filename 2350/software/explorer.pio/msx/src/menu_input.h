// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// menu_input.h - Public interface for BIOS keyboard input helpers
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef MENU_INPUT_H
#define MENU_INPUT_H

unsigned char bios_chsns(void);
unsigned char bios_chget(void);
unsigned char bios_chget_quiet(void);

#endif
