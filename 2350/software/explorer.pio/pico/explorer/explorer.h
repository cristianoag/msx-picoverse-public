// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// explorer.h - PicoVerse 2350 Explorer firmware definitions
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdint.h>
#include <stdbool.h>

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
#define ADDR_PINS   0    // Address bus (A0-A15)

// Data lines (D0-D7)
#define PIN_D0     16
#define PIN_D1     17
#define PIN_D2     18
#define PIN_D3     19
#define PIN_D4     20
#define PIN_D5     21
#define PIN_D6     22
#define PIN_D7     23
#define DATA_PINS   16   // Data bus (D0-D7)

// Control signals
#define PIN_RD     24   // Read strobe from MSX
#define PIN_WR     25   // Write strobe from MSX
#define PIN_IORQ   26   // IO Request line from MSX
#define PIN_SLTSL  27   // Slot Select for this cartridge slot
#define PIN_WAIT    28  // WAIT line to MSX 
#define PIN_INT     40  // MSX maskable interrupt line (/INT), open-drain
#define PIN_BUSSDIR 37  // Bus direction line 
#define PIN_ESP_UART_TX 38 // ESP-01 UART TX
#define PIN_ESP_UART_RX 39 // ESP-01 UART RX
#define PIN_PSRAM   47  // PSRAM select line

// External PSRAM (8MB on QMI CS1)
#define PSRAM_BASE_ADDR  0x11000000u // Cached CS1 QMI window for external PSRAM (write-through)
#define PSRAM_TOTAL_SIZE 0x800000u   // 8 MB total external PSRAM
#define MAPPER_SIZE      1048576u    // 1 MB memory mapper RAM in external PSRAM
#define MAPPER_PAGES     64u         // 1 MB / 16 KB = 64 pages
#define MAPPER_PAGE_SIZE 16384u      // 16 KB per mapper page
#define MEGARAM_SIZE     1048576u    // 1 MB MegaRAM in external PSRAM
#define MEGARAM_BANKS    128u        // 1 MB / 8 KB = 128 banks
#define MEGARAM_BANK_SIZE 8192u      // 8 KB per MegaRAM bank

typedef struct {
    uint32_t offset;    // Offset within PSRAM
    uint32_t size;      // Size of the region in bytes
    uint8_t *ptr;       // Memory-mapped pointer (PSRAM_BASE_ADDR + offset)
} psram_region_t;

// I2S DAC pins
#define I2S_DATA_PIN  29   // I2S serial data
#define I2S_BCLK_PIN  30   // I2S bit clock
#define I2S_WSEL_PIN  31   // I2S word select (LRCLK)
#define I2S_MUTE_PIN  32   // I2S DAC mute control

// SCC emulation constants
#define SCC_SAMPLE_RATE 44100
#define SCC_CLOCK       3579545

static inline void setup_gpio();
unsigned long __no_inline_not_in_flash_func(read_ulong)(const unsigned char *ptr);
int isEndOfData(const unsigned char *memory);

int __no_inline_not_in_flash_func(loadrom_msx_menu)(uint32_t offset);
void __no_inline_not_in_flash_func(loadrom_plain32)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_linear48)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_konamiscc)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_konamiscc_scc)(uint32_t offset, bool cache_enable, uint32_t scc_type);
void __no_inline_not_in_flash_func(loadrom_konami)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_ascii8)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_ascii16)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise_mapper)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_c2_usb)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_neo8)(uint32_t offset);
void __no_inline_not_in_flash_func(loadrom_neo16)(uint32_t offset);
void __no_inline_not_in_flash_func(loadrom_ascii16x)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_ascii16x_flash)(uint32_t offset, bool cache_enable,
                                                           const char *rom_name);
void __no_inline_not_in_flash_func(loadrom_planar64)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_manbow2)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_manbow2_scc)(uint32_t offset, bool cache_enable, uint32_t scc_type);
void __no_inline_not_in_flash_func(loadrom_sunrise_sd)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise_mapper_sd)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_c2_sd)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise_megaram_sd)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_sunrise_megaram_usb)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_megaram)(uint32_t offset, bool cache_enable);