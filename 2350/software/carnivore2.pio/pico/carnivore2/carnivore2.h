// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// carnivore2.h - pin map and memory layout of the Carnivore2 firmware.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef CARNIVORE2_H
#define CARNIVORE2_H

#include <stdint.h>

#define C2_FW_VERSION       "2.50"      // Carnivore2 FPGA firmware level that is modelled


// RP2350 system clock. The other PicoVerse bus firmwares run at 210 MHz; the
// OPL4 firmware runs at 300 MHz with the QMI timings below. The cartridge read
// latency (the time /WAIT is held) drops in proportion, which is what keeps
// timing sensitive games (raster effects) stable. Build with
// -DC2_SYS_CLOCK_KHZ=210000 to go back to the conservative clock.
#ifndef C2_SYS_CLOCK_KHZ
#define C2_SYS_CLOCK_KHZ    300000u
#endif

// QMI timing for the flash (M0) and the PSRAM (M1) at the chosen clock.
#if C2_SYS_CLOCK_KHZ > 250000u
#define C2_QMI_M0_TIMING        0x40000203u
#define C2_PSRAM_RXDELAY        3u
#define C2_PSRAM_MAX_SELECT     37u
#define C2_PSRAM_MIN_DESELECT   7u
#define C2_PSRAM_CLKDIV         3u
#else
#define C2_QMI_M0_TIMING        0x40000202u
#define C2_PSRAM_RXDELAY        2u
#define C2_PSRAM_MAX_SELECT     26u
#define C2_PSRAM_MIN_DESELECT   5u
#define C2_PSRAM_CLKDIV         2u
#endif

// Address lines (A0-A15) and data lines (D0-D7)
#define PIN_A0     0
#define PIN_A15    15
#define PIN_D0     16
#define PIN_D7     23

// Control signals
#define PIN_RD     24
#define PIN_WR     25
#define PIN_IORQ   26
#define PIN_SLTSL  27
#define PIN_WAIT   28
#define PIN_BUSSDIR 37
#define PIN_PSRAM  47

// I2S DAC pins
#define I2S_DATA_PIN  29
#define I2S_BCLK_PIN  30
#define I2S_WSEL_PIN  31
#define I2S_MUTE_PIN  32

// Audio
#define C2_SAMPLE_RATE          44100
#define C2_AUDIO_BUFFER_SAMPLES 256
#define C2_SCC_CLOCK            3579545
#define C2_PSG_CLOCK            1789773
#define C2_OPLL_CLOCK           3579545

// External PSRAM (8 MB, QMI CS1):
//   [0, 2 MB)  Carnivore2 RAM chip: 1 MB MegaRAM/shadow RAM + 1 MB memory mapper
//   [2, 8 MB)  write-back cache of the emulated FlashROM sectors
#define PSRAM_BASE_ADDR     0x11000000u
#define PSRAM_TOTAL_SIZE    0x800000u
#define C2_PSRAM_RAM_BASE   0x000000u
#define C2_PSRAM_OVL_BASE   0x200000u

// RP2350 flash layout (16 MB):
//   [0, 4 MB)     firmware
//   [4, 12 MB)    8 MB FlashROM image of the Carnivore2 (Carnivore2.rom)
//   [12, 16MB-4K) unused
//   [16MB-4K, 16MB) configuration EEPROM
#define C2_FLASH_IMAGE_OFFSET   0x400000u
#define C2_FLASH_IMAGE_SIZE     0x800000u
#define C2_EEPROM_FLASH_OFFSET  (0x1000000u - 0x1000u)

#endif // CARNIVORE2_H
