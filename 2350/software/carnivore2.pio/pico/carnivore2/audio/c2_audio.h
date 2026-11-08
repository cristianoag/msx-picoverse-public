// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_audio.h - Carnivore2 sound section: SCC/SCC+, FM-PAC (YM2413), PSG mirror
// and PPI beeper, mixed with the CardMDR+0x22 / +0x24 volume registers and
// delivered through the I2S DAC.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef C2_AUDIO_H
#define C2_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

// Register events posted by the bus core (core 0) and applied by the audio core (core 1).
enum {
    C2A_OPLL = 1,       // a = A0, d = data
    C2A_PSG,            // a = A0, d = data
    C2A_SCC_WAVE,       // a = offset, d = data
    C2A_SCC_REG,        // a = register (A3..A0 of the SCC register block), d = data
    C2A_SCC_MODE,       // d = deformation register
    C2A_SCC_PLUS,       // d = 1 for SCC+ (enhanced) mode
    C2A_BEEPER,         // d = level
    C2A_LEVELS          // a = CardMDR+0x22, d = CardMDR+0x24
};

void c2a_init(void);
void c2a_start(void);                       // I2S output + DMA
void c2a_post(uint8_t kind, uint8_t a, uint8_t d);
uint8_t c2a_scc_wave_read(uint8_t offset);  // wave RAM shadow kept on core 0

// Fill one audio buffer if the DMA pool has a free one. Called from the core 1
// storage loop (the symbol name is shared with the Sunrise IDE back-ends).
void service_scc_audio(void);

#endif // C2_AUDIO_H
