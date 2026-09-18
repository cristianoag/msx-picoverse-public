// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// loadrom.h - Simple ROM loader for MSX PICOVERSE project - v1.0
//
// This is  small test program that demonstrates how to load ROM images using the MSX PICOVERSE project. 
// You need to concatenate the ROM image to the  end of this program binary in order  to load it.
// The program will then act as a simple ROM cartridge that responds to memory read requests from the MSX.
// 
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef LOADROM_H
#define LOADROM_H

#define ROM_NAME_MAX    50   // Maximum size of the ROM name
#define ROM_RECORD_SIZE (ROM_NAME_MAX + 1 + (sizeof(uint32_t) * 2)) // Name + mapper + size + offset
#define CACHE_SIZE      262144     // 256KB cache size for ROM data
#define MAPPER_SIZE     1048576    // 1 MB memory mapper RAM in external PSRAM
#define MAPPER_PAGES    64         // 1 MB / 16 KB = 64 pages
#define MAPPER_PAGE_SIZE 16384     // 16 KB per mapper page
#define MEGARAM_SIZE    1048576    // 1 MB MegaRAM in external PSRAM
#define MEGARAM_BANKS   128        // 1 MB / 8 KB = 128 banks
#define MEGARAM_BANK_SIZE 8192     // 8 KB per MegaRAM bank
#define PSRAM_BASE_ADDR 0x11000000u // Cached CS1 QMI window for external PSRAM (write-through)
#define PSRAM_TOTAL_SIZE 0x800000u  // 8 MB total external PSRAM

// PSRAM region descriptor returned by the memory manager.
typedef struct {
    uint32_t offset;    // Offset from PSRAM base
    uint32_t size;      // Size of this region in bytes
    uint8_t *ptr;       // Memory-mapped pointer (PSRAM_BASE_ADDR + offset)
} psram_region_t;

// -----------------------
// User-defined pin assignments for the Raspberry Pi Pico
// -----------------------
// Address lines (A0-A15) as inputs from MSX
#define PIN_A0     0 
#define PIN_A1     1
#define PIN_A2     2
#define PIN_A3     3
#define PIN_A4     4
#define PIN_A5     5
#define PIN_A6     6
#define PIN_A7     7
#define PIN_A8     8
#define PIN_A9     9
#define PIN_A10    10
#define PIN_A11    11
#define PIN_A12    12
#define PIN_A13    13
#define PIN_A14    14
#define PIN_A15    15

// Data lines (D0-D7)
#define PIN_D0     16
#define PIN_D1     17
#define PIN_D2     18
#define PIN_D3     19
#define PIN_D4     20
#define PIN_D5     21
#define PIN_D6     22
#define PIN_D7     23

// Control signals
#define PIN_RD     24   // Read strobe from MSX
#define PIN_WR     25   // Write strobe from MSX
#define PIN_IORQ   26   // IO Request line from MSX
#define PIN_SLTSL  27   // Slot Select for this cartridge slot
#define PIN_WAIT    28  // WAIT line to MSX 
#define PIN_BUSSDIR 37  // Bus direction line 
#define PIN_PSRAM   47  // PSRAM select line

// I2S DAC pins
#define I2S_DATA_PIN  29   // I2S serial data
#define I2S_BCLK_PIN  30   // I2S bit clock
#define I2S_WSEL_PIN  31   // I2S word select (LRCLK)
#define I2S_MUTE_PIN  32   // I2S DAC mute control

// SCC emulation constants
#define SCC_SAMPLE_RATE 44100
#define SCC_CLOCK       3579545
#define SCC_FLAG        0x80u   // Bit flag in rom_type for SCC emulation
#define FLASHROM_FLAG   0x80u   // Non-SCC mapper flag for ASCII16-X FlashROM emulation
#define SCC_PLUS_FLAG   0x40u   // Bit flag in rom_type for SCC+ (enhanced) emulation
#define WIFI_FLAG       0x20u   // Bit flag in rom_type for Sunrise WiFi system ROM + memio UART
#define MSX_MUSIC_FLAG  0x20u   // Non-SYSTEM ROM flag for MSX-MUSIC/YM2413 emulation
#define DUAL_PSG_FLAG   0x10u   // Bit flag in rom_type for Carnivore2-style secondary PSG emulation
#define WIFI_ROM_SIZE   16384u  // ESP8266P system ROM size appended after the Sunrise ROM payload
#define FMPAC_BIOS_SIZE 65536u  // FM-PAC BIOS ROM size appended after the external ROM payload

// Dual PSG emulation constants
#define PSG_SAMPLE_RATE 44100
#define PSG_CLOCK       1789773
#define PSG_VOLUME_SHIFT 2
#define PSG_PORT_REG    0x10u
#define PSG_PORT_DATA   0x11u

// MSX-MUSIC / YM2413 emulation constants
#define MSX_MUSIC_SAMPLE_RATE 44100
#define MSX_MUSIC_CLOCK       3579545
#define MSX_MUSIC_VOLUME_SHIFT 2
#define MSX_MUSIC_PORT_REG    0x7Cu
#define MSX_MUSIC_PORT_DATA   0x7Du

// ESP-01 pins on the RP2350 WiFi hardware variant
// Per the Waveshare CORE2350B PCB schematic:
//   Pico GPIO38 -> net ESP01_UART_RX -> ESP-01 pin 7 (ESP RX)  => Pico UART TX
//   Pico GPIO39 <- net ESP01_UART_TX <- ESP-01 pin 2 (ESP TX)  => Pico UART RX
#define PIN_ESP_UART_TX 38
#define PIN_ESP_UART_RX 39

// This symbol marks the end of the main program in flash.
// The ROM data is concatenated immediately after this point.
extern unsigned char __flash_binary_end;

// Optionally copy the ROM into this SRAM buffer for faster access.
// Normal modes use the full 256KB as ROM cache.
// Sunrise+Mapper mode disables the ROM cache and uses external PSRAM for mapper RAM.
static uint8_t rom_sram[CACHE_SIZE];

static uint32_t active_rom_size = 0;


// The ROM is concatenated right after the main program binary.
// __flash_binary_end points to the end of the program in flash memory.
const uint8_t *rom = (const uint8_t *)&__flash_binary_end;

void dump_rom_sram(uint32_t size);
void dump_rom(uint32_t size);

#endif