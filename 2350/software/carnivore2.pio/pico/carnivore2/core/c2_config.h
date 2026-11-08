// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_config.h - build configuration of the portable Carnivore2 core.
//
// On the RP2350 the bus path must never execute from flash (XIP is unavailable
// while the flash is being programmed), so the CMake build defines
// C2_RAM_FUNCS and every core function is placed in the SDK's RAM section.
// The host tests leave it undefined.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef C2_CONFIG_H
#define C2_CONFIG_H

#ifdef C2_RAM_FUNCS
#define C2_FUNC __attribute__((section(".time_critical.c2core")))
#else
#define C2_FUNC
#endif

#endif // C2_CONFIG_H
