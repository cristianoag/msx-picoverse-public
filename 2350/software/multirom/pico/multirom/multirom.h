// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// multirom.h - This is the Raspberry Pico firmware that will be used to load ROMs into the MSX
//
// This firmware is responsible for loading the multirom menu and the ROMs selected by the user into the MSX. When flashed through the 
// multirom tool, it will be stored on the pico flash memory followed by the MSX MENU ROM (with the config) and all the ROMs processed by the 
// multirom tool. The sofware in this firmware will load the first 32KB ROM that contains the menu into the MSX and it will allow the user
// to select a ROM to be loaded into the MSX. The selected ROM will be loaded into the MSX and the MSX will be reseted to run the selected ROM.
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

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
#define PIN_WR     26   // Write strobe from MSX
#define PIN_SLTSL  27   // Slot Select for this cartridge slot
#define PIN_IORQ   28   // IO Request line from MSX
#define PIN_WAIT    46  // WAIT line to MSX 
#define PIN_BUSSDIR 47  // Bus direction line 

static inline void setup_gpio();
unsigned long __no_inline_not_in_flash_func(read_ulong)(const unsigned char *ptr);
int isEndOfData(const unsigned char *memory);

int __no_inline_not_in_flash_func(loadrom_msx_menu)(uint32_t offset);
void __no_inline_not_in_flash_func(loadrom_plain32)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_linear48)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_konamiscc)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_konami)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_ascii8)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_ascii16)(uint32_t offset, bool cache_enable);
void __no_inline_not_in_flash_func(loadrom_neo8)(uint32_t offset);
void __no_inline_not_in_flash_func(loadrom_neo16)(uint32_t offset);