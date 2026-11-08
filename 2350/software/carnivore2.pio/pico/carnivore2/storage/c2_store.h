// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_store.h - Carnivore2 FlashROM and configuration EEPROM storage.
//
// The 8 MB FlashROM image lives in RP2350 flash (read through XIP). Program and
// erase cycles from the MSX are applied to a write-back cache of 4 KB sectors
// held in PSRAM, so they complete instantly like on the real chip. Dirty
// sectors are committed to the RP2350 flash when the MSX is quiet (see
// c2s_commit_step), which is what makes uploaded ROMs and settings survive
// power cycles.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef C2_STORE_H
#define C2_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "../carnivore2.h"

#define C2S_SECTOR_SIZE     4096u
#define C2S_SECTORS         (C2_FLASH_IMAGE_SIZE / C2S_SECTOR_SIZE)
#define C2S_SLOTS           ((PSRAM_TOTAL_SIZE - C2_PSRAM_OVL_BASE) / C2S_SECTOR_SIZE)
#define C2S_NO_SLOT         0xFFFFu     // sector is read from flash
#define C2S_ERASED          0xFFFEu     // sector reads as 0xFF, the flash erase is still pending

// SRAM read cache of the FlashROM image. PSRAM would not be faster than the
// flash (both sit behind the same QMI/XIP cache and the PSRAM is clocked
// slower), so the working set of the running ROM is copied into on-chip SRAM
// in 512 byte blocks (direct mapped, any size that is a multiple of 512 bytes)
// the first time it is read.
#ifndef C2S_CACHE_KB
#define C2S_CACHE_KB        192u
#endif
#define C2S_BLOCK_SHIFT     9u
#define C2S_BLOCK_SIZE      (1u << C2S_BLOCK_SHIFT)
#define C2S_CACHE_BLOCKS    ((C2S_CACHE_KB * 1024u) >> C2S_BLOCK_SHIFT)
#define C2S_TAG_INVALID     0xFFFFu

extern uint16_t c2s_map[C2S_SECTORS];
extern uint16_t c2s_tag[C2S_CACHE_BLOCKS];
extern uint8_t c2s_cache[C2S_CACHE_BLOCKS * C2S_BLOCK_SIZE];

// Initialise the cache and load the configuration EEPROM. Call once the PSRAM is up.
void c2s_init(void);

// Copies the block holding addr from flash into the SRAM cache and returns the byte.
uint8_t c2s_cache_fill(uint32_t addr);

// FlashROM array access (bus path, runs from RAM).
static inline uint8_t __not_in_flash_func(c2s_flash_read)(uint32_t addr)
{
    uint16_t slot = c2s_map[addr >> 12];
    if (slot == C2S_NO_SLOT)
    {
        uint32_t block = addr >> C2S_BLOCK_SHIFT;
        uint32_t index = block % C2S_CACHE_BLOCKS;
        if (c2s_tag[index] == (uint16_t)block)
            return c2s_cache[(index << C2S_BLOCK_SHIFT) | (addr & (C2S_BLOCK_SIZE - 1u))];
        return c2s_cache_fill(addr);
    }
    if (slot == C2S_ERASED)
        return 0xFFu;
    return *(const volatile uint8_t *)(PSRAM_BASE_ADDR + C2_PSRAM_OVL_BASE + ((uint32_t)slot << 12) + (addr & 0xFFFu));
}

void c2s_flash_program(uint32_t addr, uint8_t data);
void c2s_flash_erase(uint32_t start, uint32_t length);

// RAM chip
static inline uint8_t __not_in_flash_func(c2s_ram_read)(uint32_t addr)
{
    return *(const volatile uint8_t *)(PSRAM_BASE_ADDR + C2_PSRAM_RAM_BASE + addr);
}

static inline void __not_in_flash_func(c2s_ram_write)(uint32_t addr, uint8_t data)
{
    *(volatile uint8_t *)(PSRAM_BASE_ADDR + C2_PSRAM_RAM_BASE + addr) = data;
}

// Configuration EEPROM persistence
void c2s_eeprom_load(uint8_t *data, uint32_t size);
void c2s_eeprom_changed(const uint8_t *data, uint32_t size);

// Write-back of dirty FlashROM sectors and of the EEPROM, one unit per call.
// The caller invokes it only while the MSX is quiet (a commit stalls the bus).
// Returns true when more work remains.
bool c2s_has_work(void);
uint32_t c2s_free_slots(void);
bool c2s_commit_step(void);

#endif // C2_STORE_H
