// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// emuy8950.h - C interface to the ymfm Yamaha Y8950 (MSX-AUDIO) core
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef EMUY8950_H
#define EMUY8950_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Register the MSX /INT line driver. The Y8950 timers raise and clear the
// interrupt; the callback drives the open-drain /INT pin.
void Y8950_SetIrqCallback(void (*cb)(bool asserted));

// Reset the chip and attach the ADPCM sample RAM (openMSX <sampleram>256</>).
// The buffer is zeroed. sample_ram_size must be a power of two.
void Y8950_Reset(uint8_t *sample_ram, uint32_t sample_ram_size);

// Host port access. offset 0 = 0xC0 (register select / status),
//                   offset 1 = 0xC1 (data).
void    Y8950_Write(uint32_t offset, uint8_t data);
uint8_t Y8950_Read(uint32_t offset);

// Generate exactly one output sample at the chip's native rate
// (clock / 72 = 49716 Hz) and advance the FM timers by one sample period.
// The Y8950 is a mono device; the value is already clamped to int16 range.
int32_t Y8950_Clock(void);

#ifdef __cplusplus
}
#endif

#endif // EMUY8950_H
