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

#define MAX_MEM_SIZE        (131072+29)    // Maximum memory size
#define ROM_NAME_MAX        20          // Maximum ROM name length
#define SIZE_CONFIG_RECORD  29          // Size of the configuration record in the ROM

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

// This symbol marks the end of the main program in flash.
// The ROM data is concatenated immediately after this point.
extern unsigned char __flash_binary_end;

// Optionally copy the ROM into this SRAM buffer for faster access
static uint8_t rom_sram[MAX_MEM_SIZE];

// The ROM is concatenated right after the main program binary.
// __flash_binary_end points to the end of the program in flash memory.
const uint8_t *rom = (const uint8_t *)&__flash_binary_end;

void dump_rom_sram(uint32_t size);
void dump_rom(uint32_t size);

#endif