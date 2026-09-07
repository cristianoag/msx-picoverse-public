// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// sd_activity.h - microSD activity counter shared by the Explorer firmware
//
// The counter is bumped on every microSD transfer (directory scans, ROM loads,
// configuration files, downloads and MP3/WAV streaming) and is published to the
// MSX menu through the CTRL_DISK_ACT register. The menu blinks the keyboard
// CAPS LED while the value keeps changing, so the card behaves like a disk
// drive with its own activity LED.
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef SD_ACTIVITY_H
#define SD_ACTIVITY_H

#include <stdint.h>

// Written from both cores (Core 0 browses, Core 1 streams audio) and read by
// the bus service loop, so it is a plain volatile byte that only ever wraps.
extern volatile uint8_t sd_activity_counter;

static inline void sd_activity_note(void)
{
    sd_activity_counter++;
}

#endif // SD_ACTIVITY_H
