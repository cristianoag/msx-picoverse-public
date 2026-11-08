// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// sunrise_sd.h - Sunrise IDE SD card backend for MSX PicoVerse
//
// Provides a microSD-backed block I/O backend for the Sunrise IDE ATA
// emulation.  The public interface mirrors sunrise_ide.h's USB functions
// so that loadrom.c can launch either backend on Core 1.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef SUNRISE_SD_H
#define SUNRISE_SD_H

#include "sunrise_ide.h"

// Set the pointer to the shared IDE context (call before launching core 1).
void sunrise_sd_set_ide_ctx(sunrise_ide_t *ide);

// SD card task loop — runs on Core 1.
// Initialises the microSD card via SPI and services IDE read/write requests.
void __not_in_flash_func(sunrise_sd_task)(void);

#endif // SUNRISE_SD_H
