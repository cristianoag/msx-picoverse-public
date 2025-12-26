// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// multirom.c - This is the Raspberry Pico firmware that will be used to load ROMs into the MSX
//
// This firmware is responsible for loading the multirom menu and the ROMs selected by the user into the MSX. When flashed through the 
// multirom tool, it will be stored on the pico flash memory followed by the MSX MENU ROM (with the config) and all the ROMs processed by the 
// multirom tool. The sofware in this firmware will load the first 32KB ROM that contains the menu into the MSX and it will allow the user
// to select a ROM to be loaded into the MSX. The selected ROM will be loaded into the MSX and the MSX will be reseted to run the selected ROM.
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hw_config.h"
#include "multirom.h"
#include "nextor.h"

// config area and buffer for the ROM data
#define ROM_NAME_MAX    50          // Maximum size of the ROM name
#define MAX_ROM_RECORDS 128         // Maximum ROM files supported
#define ROM_RECORD_SIZE (ROM_NAME_MAX + 1 + (sizeof(uint32_t) * 2)) // Name + mapper + size + offset
#define MONITOR_ADDR    (0x8000 + (ROM_RECORD_SIZE * MAX_ROM_RECORDS) + 1) // Monitor ROM address within image (currently 0x9D81)
#define CACHE_SIZE      262144     // 256KB cache size for ROM data

// This symbol marks the end of the main program in flash.
// Custom data starts right after it
extern unsigned char __flash_binary_end;

// SRAM buffer to cache ROM data
static uint8_t rom_sram[CACHE_SIZE];
static uint32_t active_rom_size = 0;

//pointer to the custom data
const uint8_t *rom = (const uint8_t *)&__flash_binary_end;

uint8_t ctr_val = 0xFF;    
BYTE const pdrv = 0;  // Physical drive number
DSTATUS ds = 1; // Disk status (1 = not initialized)

// Structure to represent a ROM record
// The ROM record will contain the name of the ROM, the mapper code, the size of the ROM and the offset in the flash memory
// Name: ROM_NAME_MAX bytes
// Mapper: 1 byte
// Size: 4 bytes
// Offset: 4 bytes
typedef struct {
    char Name[ROM_NAME_MAX];
    unsigned char Mapper;
    unsigned long Size;
    unsigned long Offset;
} ROMRecord;

ROMRecord records[MAX_ROM_RECORDS]; // Array to store the ROM records

// Initialize GPIO pins
static inline void setup_gpio()
{

    for (int i = 0; i <= 23; i++) {
        gpio_init(i);
        gpio_set_input_hysteresis_enabled(i, true);
    }

    for (int i = 0; i <= 15; i++) {
        gpio_set_dir(i, GPIO_IN);
    }

    // Initialize control pins as input
    gpio_init(PIN_RD); gpio_set_dir(PIN_RD, GPIO_IN);    
    gpio_init(PIN_WR); gpio_set_dir(PIN_WR, GPIO_IN); 
    gpio_init(PIN_IORQ); gpio_set_dir(PIN_IORQ, GPIO_IN); 
    gpio_init(PIN_SLTSL); gpio_set_dir(PIN_SLTSL, GPIO_IN); 
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN); 
}

// read_ulong - Read a 4-byte value from the memory area
// This function will read a 4-byte value from the memory area pointed by ptr and return the value as an unsigned long
// Parameters:
//   ptr - Pointer to the memory area to read the value from
// Returns:
//   The 4-byte value as an unsigned long 
unsigned long __no_inline_not_in_flash_func(read_ulong)(const unsigned char *ptr) {
    return (unsigned long)ptr[0] |
           ((unsigned long)ptr[1] << 8) |
           ((unsigned long)ptr[2] << 16) |
           ((unsigned long)ptr[3] << 24);
}

// isEndOfData - Check if the memory area is the end of the data
// This function will check if the memory area pointed by memory is the end of the data. The end of the data is defined by all bytes being 0xFF.
// Parameters:
//   memory - Pointer to the memory area to check
// Returns:
//   1 if the memory area is the end of the data, 0 otherwise
int __no_inline_not_in_flash_func(isEndOfData)(const unsigned char *memory) {
    for (int i = 0; i < ROM_RECORD_SIZE; i++) {
        if (memory[i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}

//load the MSX Menu ROM into the MSX
int __no_inline_not_in_flash_func(loadrom_msx_menu)(uint32_t offset)
{
    //setup the rom_sram buffer for the 32KB ROM
    gpio_init(PIN_WAIT); // Init wait signal pin
    gpio_set_dir(PIN_WAIT, GPIO_OUT); // Set the WAIT signal as output
    gpio_put(PIN_WAIT, 0); // Wait until we are ready to read the ROM
    memset(rom_sram, 0, 32768); // Clear the SRAM buffer
    memcpy(rom_sram, rom + offset, 32768); //for 32KB ROMs we start at 0x4000
    gpio_put(PIN_WAIT, 1); // Lets go!

    int record_count = 0; // Record count
    const uint8_t *record_ptr = rom + offset + 0x4000; // Pointer to the ROM records
    for (int i = 0; i < MAX_ROM_RECORDS; i++)      // Read the ROMs from the configuration area
    {
        if (isEndOfData(record_ptr)) {
            break; // Stop if end of data is reached
        }
        memcpy(records[record_count].Name, record_ptr, ROM_NAME_MAX); // Copy the ROM name
        record_ptr += ROM_NAME_MAX; // Move the pointer to the next field
        records[record_count].Mapper = *record_ptr++; // Read the mapper code
        records[record_count].Size = read_ulong(record_ptr); // Read the ROM size
        record_ptr += sizeof(unsigned long); // Move the pointer to the next field
        records[record_count].Offset = read_ulong(record_ptr); // Read the ROM offset
        record_ptr += sizeof(unsigned long); // Move the pointer to the next record
        record_count++; // Increment the record count
    }

    uint8_t rom_index = 0;
    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
    bool rom_selected = false; // ROM selected flag
    while (true)  // Loop until a ROM is selected
    {
        // Check control signals
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)

        uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
        //uint16_t addr = sio_hw->gpio_in & 0x00FFFF;
        if (sltsl) 
        {
            bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low, not used)

            if (wr && addr == MONITOR_ADDR) // Monitor ROM address to select the ROM
            {   
                    rom_index = (gpio_get_all() >> 16) & 0xFF;
                    while (!(gpio_get(PIN_WR))) { // Wait until the write cycle completes (WR goes high){
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
                    rom_selected = true;    // ROM selected
            }

            if (addr >= 0x4000 && addr <= 0xBFFF) // Check if the address is within the ROM range
            {   
                if (rd)
                {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    uint32_t rom_addr = offset + (addr - 0x4000); // Calculate flash address
                    gpio_put_masked(0xFF0000, rom_sram[rom_addr] << 16); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD))) { // Wait until the read cycle completes (RD goes high)
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
                }
            } 
        }

        if (rd && addr == 0x0000 && rom_selected)   // lets return the rom_index and load the selected ROM
        {
            return rom_index;
        }
    }
}

// loadrom_plain32 - Load a simple 32KB (or less) ROM into the MSX directly from the pico flash
// 32KB ROMS have two pages of 16Kb each in the following areas:
// 0x4000-0x7FFF and 0x8000-0xBFFF
// AB is on 0x0000, 0x0001
// 16KB ROMS have only one page in the 0x4000-0x7FFF area
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_plain32)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base = rom + offset;

     if (cache_enable)
    {
        gpio_init(PIN_WAIT);
        gpio_set_dir(PIN_WAIT, GPIO_OUT);
        gpio_put(PIN_WAIT, 0);
        memset(rom_sram, 0, 32768);
        memcpy(rom_sram, rom_base, 32768);
        gpio_put(PIN_WAIT, 1);
        rom_base = rom_sram;
    }

    gpio_set_dir_in_masked(0xFF << 16);
    while (true)
    {
        if (!gpio_get(PIN_SLTSL))
        {
            uint16_t addr = gpio_get_all() & 0x00FFFF;
            if ((addr >= 0x4000) && (addr <= 0xBFFF) && !gpio_get(PIN_RD))
            {
                uint8_t data = rom_base[addr - 0x4000];
                gpio_set_dir_out_masked(0xFF << 16);
                gpio_put_masked(0xFF0000, (uint32_t)data << 16);
                while (!gpio_get(PIN_RD))
                    tight_loop_contents();
                gpio_set_dir_in_masked(0xFF << 16);
            }
        }
    }
}

// loadrom_linear48 - Load a simple 48KB Linear0 ROM into the MSX directly from the pico flash
// Those ROMs have three pages of 16Kb each in the following areas:
// 0x0000-0x3FFF, 0x4000-0x7FFF and 0x8000-0xBFFF
// AB is on 0x4000, 0x4001
void __no_inline_not_in_flash_func(loadrom_linear48)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base = rom + offset;

    if (cache_enable)
    {
        gpio_init(PIN_WAIT);
        gpio_set_dir(PIN_WAIT, GPIO_OUT);
        gpio_put(PIN_WAIT, 0);
        memset(rom_sram, 0, 49152);
        memcpy(rom_sram, rom_base, 49152);
        gpio_put(PIN_WAIT, 1);
        rom_base = rom_sram;
    }
    
    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
    while (true) 
    {
        if (!gpio_get(PIN_SLTSL))
        {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
            if ((addr >= 0x0000) && (addr <= 0xBFFF) && !gpio_get(PIN_RD)) // Check if the address is within the ROM range
            {
                uint8_t data = rom_base[addr];
                gpio_set_dir_out_masked(0xFF << 16);
                gpio_put_masked(0xFF0000, (uint32_t)data << 16);
                while (!gpio_get(PIN_RD))
                    tight_loop_contents();
                gpio_set_dir_in_masked(0xFF << 16);
            }
        }
    }
}

// loadrom_konamiscc - Load a any Konami SCC ROM into the MSX directly from the pico flash
// The KonamiSCC ROMs are divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments 
// into the MSX's address space. Since the size of the mapper is 8Kb, the memory banks are:
// Bank 1: 4000h - 5FFFh , Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
// And the address to change banks are:
// Bank 1: 5000h - 57FFh (5000h used), Bank 2: 7000h - 77FFh (7000h used), Bank 3: 9000h - 97FFh (9000h used), Bank 4: B000h - B7FFh (B000h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_konamiscc)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3}; // Initial banks 0-3 mapped
    uint32_t cached_length = 0;

    if (cache_enable)
    {
        gpio_init(PIN_WAIT);
        gpio_set_dir(PIN_WAIT, GPIO_OUT);
        gpio_put(PIN_WAIT, 0);

        uint32_t bytes_to_cache = active_rom_size;
        if (bytes_to_cache == 0 || bytes_to_cache > sizeof(rom_sram))
        {
            bytes_to_cache = sizeof(rom_sram);
        }

        memset(rom_sram, 0, bytes_to_cache);
        memcpy(rom_sram, rom + offset, bytes_to_cache);
        gpio_put(PIN_WAIT, 1);
        cached_length = bytes_to_cache;
    }

    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
    while (true) 
    {
        // Check control signals
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl)
        {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
            if (addr >= 0x4000 && addr <= 0xBFFF)  // Check if the address is within the ROM range
            {
                if (rd) 
                {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    uint32_t const rom_offset = offset + (bank_registers[(addr - 0x4000) >> 13] * 0x2000u) + (addr & 0x1FFFu); // Calculate the ROM offset

                    uint8_t data;
                    uint32_t const relative_offset = (rom_offset >= offset) ? (rom_offset - offset) : cached_length;
                    if (cache_enable && relative_offset < cached_length)
                    {
                        data = rom_sram[relative_offset];
                    }
                    else
                    {
                        data = rom[rom_offset];
                    }

                    gpio_put_masked(0xFF0000, (uint32_t)data << 16); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD)))  // Wait until the read cycle completes (RD goes high)
                    {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode after cycle completes
                } else if (wr) 
                {
                    // Handle writes to bank switching addresses
                    if ((addr >= 0x5000)  && (addr <= 0x57FF)) { 
                        bank_registers[0] = (gpio_get_all() >> 16) & 0xFF; // Read the data bus and store in bank register
                    } else if ((addr >= 0x7000) && (addr <= 0x77FF)) {
                        bank_registers[1] = (gpio_get_all() >> 16) & 0xFF;
                    } else if ((addr >= 0x9000) && (addr <= 0x97FF)) {
                        bank_registers[2] = (gpio_get_all() >> 16) & 0xFF;
                    } else if ((addr >= 0xB000) && (addr <= 0xB7FF)) {
                        bank_registers[3] = (gpio_get_all() >> 16) & 0xFF;
                    }

                    while (!(gpio_get(PIN_WR)))
                    {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}

// loadrom_konami - Load a Konami (without SCC) ROM into the MSX directly from the pico flash
// The Konami (without SCC) ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 8Kb, the memory banks are:
//  Bank 1: 4000h - 5FFFh, Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
// And the addresses to change banks are:
//	Bank 1: <none>, Bank 2: 6000h - 67FFh (6000h used), Bank 3: 8000h - 87FFh (8000h used), Bank 4: A000h - A7FFh (A000h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_konami)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3}; // Initial banks 0-3 mapped
    uint32_t cached_length = 0;

    if (cache_enable)
    {
        gpio_init(PIN_WAIT);
        gpio_set_dir(PIN_WAIT, GPIO_OUT);
        gpio_put(PIN_WAIT, 0);

        uint32_t bytes_to_cache = active_rom_size;
        if (bytes_to_cache == 0 || bytes_to_cache > sizeof(rom_sram))
        {
            bytes_to_cache = sizeof(rom_sram);
        }

        memset(rom_sram, 0, bytes_to_cache);
        memcpy(rom_sram, rom + offset, bytes_to_cache);
        gpio_put(PIN_WAIT, 1);
        cached_length = bytes_to_cache;
    }

    gpio_set_dir_in_masked(0xFF << 16);
    while (true) 
    {
        // Check control signals
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl)
        {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
            if (addr >= 0x4000 && addr <= 0xBFFF) 
            {
                if (rd) 
                {
                    gpio_set_dir_out_masked(0xFF << 16);
                    uint32_t const rom_offset = offset + (bank_registers[(addr - 0x4000) >> 13] * 0x2000u) + (addr & 0x1FFFu); // Calculate the ROM offset

                    uint8_t data;
                    uint32_t const relative_offset = (rom_offset >= offset) ? (rom_offset - offset) : cached_length;
                    if (cache_enable && relative_offset < cached_length)
                    {
                        data = rom_sram[relative_offset];
                    }
                    else
                    {
                        data = rom[rom_offset];
                    }

                    gpio_put_masked(0xFF0000, (uint32_t)data << 16);
                    while (!(gpio_get(PIN_RD))) 
                    {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);

                }else if (wr) {
                    // Handle writes to bank switching addresses
                    if ((addr >= 0x6000) && (addr <= 0x67FF)) {
                        bank_registers[1] = (gpio_get_all() >> 16) & 0xFF;
                    } else if ((addr >= 0x8000) && (addr <= 0x87FF)) {
                        bank_registers[2] = (gpio_get_all() >> 16) & 0xFF;
                    } else if ((addr >= 0xA000) && (addr <= 0xA7FF)) {
                        bank_registers[3] = (gpio_get_all() >> 16) & 0xFF;
                    }

                    while (!(gpio_get(PIN_WR))) 
                    {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}

// loadrom_ascii8 - Load an ASCII8 ROM into the MSX directly from the pico flash
// The ASCII8 ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 8Kb, the memory banks are:
// Bank 1: 4000h - 5FFFh , Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
// And the address to change banks are:
// Bank 1: 6000h - 67FFh (6000h used), Bank 2: 6800h - 6FFFh (6800h used), Bank 3: 7000h - 77FFh (7000h used), Bank 4: 7800h - 7FFFh (7800h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_ascii8)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3}; // Initial banks 0-3 mapped
    const uint8_t *rom_base = rom + offset;
    uint32_t cached_length = 0;

    if (cache_enable)
    {
        gpio_init(PIN_WAIT);
        gpio_set_dir(PIN_WAIT, GPIO_OUT);
        gpio_put(PIN_WAIT, 0);

        uint32_t bytes_to_cache = active_rom_size;
        if (bytes_to_cache == 0 || bytes_to_cache > sizeof(rom_sram))
        {
            bytes_to_cache = sizeof(rom_sram);
        }

        memset(rom_sram, 0, bytes_to_cache);
        memcpy(rom_sram, rom_base, bytes_to_cache);
        gpio_put(PIN_WAIT, 1);
        cached_length = bytes_to_cache;
    }

    gpio_set_dir_in_masked(0xFF << 16);
    while (true) 
    {
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl) {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
            if (addr >= 0x4000 && addr <= 0xBFFF) 
            {
                if (rd) 
                {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    uint8_t const bank = bank_registers[(addr - 0x4000) >> 13];
                    uint32_t const rom_offset = offset + (bank * 0x2000u) + (addr & 0x1FFFu); // Calculate the ROM offset

                    uint8_t data;
                    uint32_t const relative_offset = (rom_offset >= offset) ? (rom_offset - offset) : cached_length;
                    if (cache_enable && relative_offset < cached_length)
                    {
                        data = rom_sram[relative_offset];
                    }
                    else
                    {
                        data = rom[rom_offset];
                    }

                    gpio_put_masked(0xFF0000, (uint32_t)data << 16); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD)))  { tight_loop_contents(); } // Wait until the read cycle completes (RD goes high)        }
                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode after the read cycle
                } else if (wr)  // Handle writes to bank switching addresses
                { 
                    if ((addr >= 0x6000) && (addr <= 0x67FF)) { 
                        bank_registers[0] = (gpio_get_all() >> 16) & 0xFF; // Read the data bus and store in bank register
                    } else if ((addr >= 0x6800) && (addr <= 0x6FFF)) {
                        bank_registers[1] = (gpio_get_all() >> 16) & 0xFF;
                    } else if ((addr >= 0x7000) && (addr <= 0x77FF)) {
                        bank_registers[2] = (gpio_get_all() >> 16) & 0xFF;
                    } else if ((addr >= 0x7800) && (addr <= 0x7FFF)) {
                        bank_registers[3] = (gpio_get_all() >> 16) & 0xFF;
                    }

                    while (!(gpio_get(PIN_WR))) 
                    {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}

// loadrom_ascii16 - Load an ASCII16 ROM into the MSX directly from the pico flash
// The ASCII16 ROM is divided into 16KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 16Kb, the memory banks are:
// Bank 1: 4000h - 7FFFh , Bank 2: 8000h - BFFFh
// And the address to change banks are:
// Bank 1: 6000h - 67FFh (6000h used), Bank 2: 7000h - 77FFh (7000h and 77FFh used)
void __no_inline_not_in_flash_func(loadrom_ascii16)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[2] = {0, 1}; // Initial banks 0 and 1 mapped
    uint32_t cached_length = 0;

    if (cache_enable)
    {
        gpio_init(PIN_WAIT);
        gpio_set_dir(PIN_WAIT, GPIO_OUT);
        gpio_put(PIN_WAIT, 0);

        uint32_t bytes_to_cache = active_rom_size;
        if (bytes_to_cache == 0 || bytes_to_cache > sizeof(rom_sram))
        {
            bytes_to_cache = sizeof(rom_sram);
        }

        memset(rom_sram, 0, bytes_to_cache);
        memcpy(rom_sram, rom + offset, bytes_to_cache);
        gpio_put(PIN_WAIT, 1);
        cached_length = bytes_to_cache;
    }

    gpio_set_dir_in_masked(0xFF << 16);
    while (true) {
        // Check control signals
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl) {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
            if (addr >= 0x4000 && addr <= 0xBFFF)  
            {
                if (rd) {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    uint8_t const bank = (addr >> 15) & 1;
                    uint32_t const rom_offset = offset + ((uint32_t)bank_registers[bank] << 14) + (addr & 0x3FFF);

                    uint8_t data;
                    uint32_t const relative_offset = (rom_offset >= offset) ? (rom_offset - offset) : cached_length;
                    if (cache_enable && relative_offset < cached_length)
                    {
                        data = rom_sram[relative_offset];
                    }
                    else
                    {
                        data = rom[rom_offset];
                    }

                    gpio_put_masked(0xFF0000, (uint32_t)data << 16); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD)))  // Wait for the read cycle to complete
                    {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode after the read cycle
                }
                else if (wr) 
                {
                    // Update bank registers based on the specific switching addresses
                    if ((addr >= 0x6000) && (addr <= 0x67FF)) {
                        bank_registers[0] = (gpio_get_all() >> 16) & 0xFF;
                    } else if (addr >= 0x7000 && addr <= 0x77FF) {
                        bank_registers[1] = (gpio_get_all() >> 16) & 0xFF;
                    }
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                }
                
            }
        }
    }
}


void __no_inline_not_in_flash_func(loadrom_nextor_sd_io)(uint32_t offset)
{
    //static uint8_t rom_sram[131072]; // Buffer for the ROM data (128KB) - TEST

    //overclock to test performance gains
    //qmi_hw->m[0].timing = 0x40000201; // Set the QMI timing for the MSX bus
    //set_sys_clock_khz(150000, true);     // Set system clock to 285Mhz

    //runs the IO code in the second core
    multicore_launch_core1(nextor_sd_io);    // Launch core 1

    //Test copying to RAM to check performance gains
    gpio_init(PIN_WAIT); // Init wait signal pin
    gpio_set_dir(PIN_WAIT, GPIO_OUT); // Set the WAIT signal as output
    gpio_put(PIN_WAIT, 0); // Wait until we are ready to read the ROM
    memset(rom_sram, 0, 131072); // Clear the SRAM buffer
    memcpy(rom_sram, rom + offset, 131072); //for 32KB ROMs we start at 0x4000
    gpio_put(PIN_WAIT, 1); // Lets go!

    uint8_t bank_registers[2] = {0, 1}; // Initial banks 0 and 1 mapped

    gpio_set_dir_in_masked(0xFF << 16);
    while (true) {
        // Check control signals
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl) {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
            if (addr >= 0x4000 && addr <= 0xBFFF) 
            {
                if (rd) {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    //uint32_t rom_offset = offset + (bank_registers[(addr >> 15) & 1] << 14) + (addr & 0x3FFF);
                    //gpio_put_masked(0xFF0000, rom[rom_offset] << 16); // Write the data to the data bus
                    //Sram - Tests
                    uint32_t rom_offset = (bank_registers[(addr >> 15) & 1] << 14) + (addr & 0x3FFF);
                    gpio_put_masked(0xFF0000, rom_sram[rom_offset] << 16); // Write the data to the data bus

                    while (!(gpio_get(PIN_RD)))  // Wait for the read cycle to complete
                    {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode after the read cycle
                }
                else if (wr) 
                {
                    // Update bank registers based on the specific switching addresses
                    if ((addr >= 0x6000) && (addr <= 0x67FF)) {
                        bank_registers[0] = (gpio_get_all() >> 16) & 0xFF;
                    } else if (addr >= 0x7000 && addr <= 0x77FF) {
                        bank_registers[1] = (gpio_get_all() >> 16) & 0xFF;
                    }
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}

// loadrom_neo8 - Load an NEO8 ROM into the MSX directly from the pico flash
// The NEO8 ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Size of a segment: 8 KB
// Segment switching addresses:
// Bank 0: 0000h~1FFFh, Bank 1: 2000h~3FFFh, Bank 2: 4000h~5FFFh, Bank 3: 6000h~7FFFh, Bank 4: 8000h~9FFFh, Bank 5: A000h~BFFFh
// Switching address: 
// 5000h (mirror at 1000h, 9000h and D000h), 
// 5800h (mirror at 1800h, 9800h and D800h), 
// 6000h (mirror at 2000h, A000h and E000h), 
// 6800h (mirror at 2800h, A800h and E800h), 
// 7000h (mirror at 3000h, B000h and F000h), 
// 7800h (mirror at 3800h, B800h and F800h)
void __no_inline_not_in_flash_func(loadrom_neo8)(uint32_t offset)
{
    uint16_t bank_registers[6] = {0}; // 16-bit bank registers initialized to zero (12-bit segment, 4 MSB reserved)

    gpio_set_dir_in_masked(0xFF << 16);    // Configure GPIO pins for input mode
    while (true)
    {
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl)
        {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read address bus

            if (addr <= 0xBFFF)
            {
                if (rd)
                {
                    // Handle read access
                    gpio_set_dir_out_masked(0xFF << 16); // Data bus output mode
                    uint8_t bank_index = addr >> 13;     // Determine bank index (0-5)

                    if (bank_index < 6)
                    {
                        uint32_t segment = bank_registers[bank_index] & 0x0FFF; // 12-bit segment number
                        uint32_t rom_offset = offset + (segment << 13) + (addr & 0x1FFF); // Calculate ROM offset
                        gpio_put_masked(0xFF0000, rom[rom_offset] << 16); // Place data on data bus
                    }
                    else
                    {
                        gpio_put_masked(0xFF0000, 0xFF << 16); // Invalid page handling (Page 3)
                    }

                    while (!(gpio_get(PIN_RD))) // Wait for read cycle to complete
                    {
                        tight_loop_contents();
                    }

                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode
                }
                else if (wr)
                {
                    // Handle write access
                    uint16_t base_addr = addr & 0xF800; // Mask to identify base address
                    uint8_t bank_index = 6;             // Initialize to invalid bank

                    // Determine bank index based on base address
                    switch (base_addr)
                    {
                        case 0x5000:
                        case 0x1000:
                        case 0x9000:
                        case 0xD000:
                            bank_index = 0;
                            break;
                        case 0x5800:
                        case 0x1800:
                        case 0x9800:
                        case 0xD800:
                            bank_index = 1;
                            break;
                        case 0x6000:
                        case 0x2000:
                        case 0xA000:
                        case 0xE000:
                            bank_index = 2;
                            break;
                        case 0x6800:
                        case 0x2800:
                        case 0xA800:
                        case 0xE800:
                            bank_index = 3;
                            break;
                        case 0x7000:
                        case 0x3000:
                        case 0xB000:
                        case 0xF000:
                            bank_index = 4;
                            break;
                        case 0x7800:
                        case 0x3800:
                        case 0xB800:
                        case 0xF800:
                            bank_index = 5;
                            break;
                    }

                    if (bank_index < 6)
                    {
                        uint8_t data = (gpio_get_all() >> 16) & 0xFF;
                        if (addr & 0x01)
                        {
                            // Write to MSB
                            bank_registers[bank_index] = (bank_registers[bank_index] & 0x00FF) | (data << 8);
                        }
                        else
                        {
                            // Write to LSB
                            bank_registers[bank_index] = (bank_registers[bank_index] & 0xFF00) | data;
                        }

                        // Ensure reserved MSB bits are zero
                        bank_registers[bank_index] &= 0x0FFF;
                    }

                    while (!(gpio_get(PIN_WR))) // Wait for write cycle to complete
                    {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}

// loadrom_neo16 - Load an NEO16 ROM into the MSX directly from the pico flash
// The NEO16 ROM is divided into 16KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Size of a segment: 16 KB
// Segment switching addresses:
// Bank 0: 0000h~3FFFh, Bank 1: 4000h~7FFFh, Bank 2: 8000h~BFFFh
// Switching address:
// 5000h (mirror at 1000h, 9000h and D000h),
// 6000h (mirror at 2000h, A000h and E000h),
// 7000h (mirror at 3000h, B000h and F000h)
void __no_inline_not_in_flash_func(loadrom_neo16)(uint32_t offset)
{
    // 16-bit bank registers initialized to zero (12-bit segment, 4 MSB reserved)
    uint16_t bank_registers[3] = {0};

    // Configure GPIO pins for input mode
    gpio_set_dir_in_masked(0xFF << 16);
    while (true)
    {
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl)
        {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read address bus
            if (addr <= 0xBFFF)
            {
                if (rd)
                {
                    // Handle read access
                    gpio_set_dir_out_masked(0xFF << 16); // Data bus output mode
                    uint8_t bank_index = addr >> 14;     // Determine bank index (0-2)

                    if (bank_index < 3)
                    {
                        uint32_t segment = bank_registers[bank_index] & 0x0FFF; // 12-bit segment number
                        uint32_t rom_offset = offset + (segment << 14) + (addr & 0x3FFF); // Calculate ROM offset
                        gpio_put_masked(0xFF0000, rom[rom_offset] << 16); // Place data on data bus
                    }
                    else
                    {
                        gpio_put_masked(0xFF0000, 0xFF << 16); // Invalid page handling
                    }

                    while (!(gpio_get(PIN_RD))) // Wait for read cycle to complete
                    {
                        tight_loop_contents();
                    }

                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode
                }
                else if (wr)
                {
                    // Handle write access
                    uint16_t base_addr = addr & 0xF800; // Mask to identify base address
                    uint8_t bank_index = 3;             // Initialize to invalid bank

                    // Determine bank index based on base address
                    switch (base_addr)
                    {
                        case 0x5000:
                        case 0x1000:
                        case 0x9000:
                        case 0xD000:
                            bank_index = 0;
                            break;
                        case 0x6000:
                        case 0x2000:
                        case 0xA000:
                        case 0xE000:
                            bank_index = 1;
                            break;
                        case 0x7000:
                        case 0x3000:
                        case 0xB000:
                        case 0xF000:
                            bank_index = 2;
                            break;
                    }

                    if (bank_index < 3)
                    {
                        uint8_t data = (gpio_get_all() >> 16) & 0xFF; // Read 8-bit data from bus
                        if (addr & 0x01)
                        {
                            // Write to MSB
                            bank_registers[bank_index] = (bank_registers[bank_index] & 0x00FF) | (data << 8);
                        }
                        else
                        {
                            // Write to LSB
                            bank_registers[bank_index] = (bank_registers[bank_index] & 0xFF00) | data;
                        }

                        // Ensure reserved MSB bits are zero
                        bank_registers[bank_index] &= 0x0FFF;
                    }

                    while (!(gpio_get(PIN_WR))) // Wait for write cycle to complete
                    {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}


// Main function running on core 0
int __no_inline_not_in_flash_func(main)()
{
    qmi_hw->m[0].timing = 0x40000202; // Set the QMI timing for the MSX bus
    set_sys_clock_khz(250000, true);     // Set system clock to 250Mhz

    stdio_init_all();     // Initialize stdio
    setup_gpio();     // Initialize GPIO

    int rom_index = loadrom_msx_menu(0x0000); //load the first 32KB ROM into the MSX (The MSX PICOVERSE MENU)

    // Load the selected ROM into the MSX according to the mapper
    switch (records[rom_index].Mapper) {
       
        case 1:
        case 2:
            loadrom_plain32(records[rom_index].Offset, true);
            break;
        case 3:
            loadrom_konamiscc(records[rom_index].Offset, true);
            break;
        case 4:
            loadrom_linear48(records[rom_index].Offset, true);
            break;
        case 5:
            loadrom_ascii8(records[rom_index].Offset, true); 
            break;
        case 6:
            loadrom_ascii16(records[rom_index].Offset, true); 
            break;
        case 7:
            loadrom_konami(records[rom_index].Offset, true); 
            break;
        case 8:
            loadrom_neo8(records[rom_index].Offset); 
            break;
        case 9:
            loadrom_neo16(records[rom_index].Offset); 
            break;
        case 10:
            loadrom_nextor_sd_io(records[rom_index].Offset);
            break;
        default:
            printf("Debug: Unsupported ROM mapper: %d\n", records[rom_index].Mapper);
            break;
    }
    
}
