// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// msxaudio.h - shared constants for the standalone MSX-AUDIO cartridge firmware
//
// Self-contained: this firmware does not include any header from the other
// PicoVerse firmwares, so the pin map and the PSRAM window constants are
// repeated here (they describe the same PicoVerse 2350 hardware).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef MSXAUDIO_H
#define MSXAUDIO_H

#include <stdint.h>

// -----------------------------------------------------------------------
// PicoVerse 2350 cartridge pin map
// -----------------------------------------------------------------------
#define PIN_A0        0     // A0..A15 = GPIO 0..15 (always inputs)
#define PIN_A15       15
#define PIN_D0        16    // D0..D7  = GPIO 16..23 (bidirectional, PIO driven)
#define PIN_D7        23
#define PIN_RD        24    // /RD     (active low)
#define PIN_WR        25    // /WR     (active low)
#define PIN_IORQ      26    // /IORQ   (active low)
#define PIN_SLTSL     27    // /SLTSL  (active low)
#define PIN_WAIT      28    // /WAIT   (active low, open-drain)
#define PIN_BUSSDIR   37    // /BUSDIR (active low, push-pull)
#define PIN_INT       40    // /INT    (active low, open-drain)
#define PIN_PSRAM     47    // external PSRAM chip select (QMI CS1)

// I2S DAC pins
#define I2S_DATA_PIN  29
#define I2S_BCLK_PIN  30
#define I2S_WSEL_PIN  31
#define I2S_MUTE_PIN  32

// -----------------------------------------------------------------------
// External PSRAM (QMI CS1)
// -----------------------------------------------------------------------
#define PSRAM_BASE_ADDR  0x11000000u   // cached, write-through CS1 XIP window
#define PSRAM_TOTAL_SIZE 0x800000u     // 8 MB

// -----------------------------------------------------------------------
// Yamaha Y8950 (MSX-AUDIO) parameters
// -----------------------------------------------------------------------
// The Y8950 in an MSX-AUDIO cartridge is normally fed the 3.579545 MHz MSX
// clock. ymfm generates at the chip's native rate, clock / (OPERATORS *
// PRESCALE) = clock / 72, so the I2S output simply runs at that same rate and
// no resampling is needed anywhere in the audio path.
//
// A few MSX-AUDIO implementations clock the Y8950 at 4 MHz instead, which runs
// the whole chip - pitch, envelopes, LFO, ADPCM playback and the FM timers -
// 4000000/3579545 = 1.1174x faster. That is selectable at programming time
// (loadrom -a --4mhz) and is implemented purely by raising the output sample
// rate, because every one of those rates is derived from the sample clock.
#define Y8950_CLOCK              3579545u
#define Y8950_CLOCK_4MHZ         4000000u
#define Y8950_CLOCKS_PER_SAMPLE  72u
#define MSXAUDIO_SAMPLE_RATE     (Y8950_CLOCK / Y8950_CLOCKS_PER_SAMPLE)       // 49715
#define MSXAUDIO_SAMPLE_RATE_4MHZ (Y8950_CLOCK_4MHZ / Y8950_CLOCKS_PER_SAMPLE) // 55555

// openMSX Boosted_audio.xml: <sampleram>256</sampleram>
#define MSXAUDIO_SAMPLE_RAM_SIZE (256u * 1024u)

// MSX-AUDIO host ports (openMSX: <io base="0xC0" num="2"/>)
#define MSXAUDIO_PORT_ADDR       0xC0u
#define MSXAUDIO_PORT_DATA       0xC1u

// -----------------------------------------------------------------------
// Flash payload appended by the loadrom tool: [16-byte config][48KB BIOS ROM]
// -----------------------------------------------------------------------
#define MSXAUDIO_CFG_HEADER_SIZE 16u
#define MSXAUDIO_CFG_MAGIC0      'P'
#define MSXAUDIO_CFG_MAGIC1      'V'
#define MSXAUDIO_CFG_MAGIC2      'A'
#define MSXAUDIO_CFG_MAGIC3      'U'

// Config header flag byte (offset 4)
#define MSXAUDIO_CFG_FLAG_CLOCK_4MHZ 0x01u

// MSX-AUDIO BIOS ROM image size (openMSX "MSX-Audio BIOS v1.3", 48KB)
#define MSXAUDIO_BIOS_ROM_SIZE   49152u

#endif // MSXAUDIO_H
