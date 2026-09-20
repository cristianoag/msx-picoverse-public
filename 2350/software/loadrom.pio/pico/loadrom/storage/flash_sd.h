// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// flash_sd.h - microSD backing store for emulated cartridge FlashROM
//
// ASCII16-X cartridges expose real FlashROM memory that the MSX can erase
// and reprogram to keep save games, high scores or user created levels.
// This module keeps the emulated flash array mirrored in a plain binary
// file on the microSD card so those writes survive a power cycle.
//
// The file is a byte exact image of the cartridge flash: it can be copied
// off the card and inspected or used as a ROM image directly.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef FLASH_SD_H
#define FLASH_SD_H

#include <stdbool.h>
#include <stdint.h>

// Persistence granularity. Modified bytes are tracked per block and whole
// blocks are written back to the card.
#define FLASH_SD_BLOCK_SHIFT 12u
#define FLASH_SD_BLOCK_SIZE  (1u << FLASH_SD_BLOCK_SHIFT)

// Configure the backing store. Call on Core 0 before launching Core 1.
//   rom_name   cartridge name from the configuration record (NUL terminated)
//   image      flash array, already filled with the embedded cartridge ROM
//   image_size flash array size in bytes
//   rom_src    embedded cartridge ROM, used to rebuild the array if a
//              restore from the card fails part way through
//   rom_len    embedded cartridge ROM size in bytes
void flash_sd_configure(const char *rom_name, uint8_t *image, uint32_t image_size,
                        const uint8_t *rom_src, uint32_t rom_len);

// Core 1 entry point. Mounts the card, restores the image file (or creates
// it from the embedded ROM) and then services write-back requests forever.
void flash_sd_task(void);

// True once the initial restore/create pass has finished, successfully or
// not. Core 0 must wait for this before serving the MSX bus.
bool flash_sd_ready(void);

// True when the microSD card is mounted and the flash image file is open,
// so writes are being persisted.
bool flash_sd_backed(void);

// Path of the image file on the card, for diagnostics.
const char *flash_sd_path(void);

// Record a modified byte range so Core 1 writes it back to the card.
// Safe to call from Core 0 at any time, including from the bus loop, and
// before the image file exists: anything recorded while Core 1 is still
// creating the file is written back on the first pass afterwards.
void flash_sd_mark_dirty(uint32_t offset, uint32_t length);

#endif // FLASH_SD_H
