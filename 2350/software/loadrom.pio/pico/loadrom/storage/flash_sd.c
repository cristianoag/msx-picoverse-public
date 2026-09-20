// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// flash_sd.c - microSD backing store for emulated cartridge FlashROM
//
// Keeps the emulated ASCII16-X FlashROM array mirrored in a byte exact
// binary file on the microSD card so that erases and programs issued by
// the MSX survive a power cycle.
//
// Architecture:
//   Core 0: bus engine, applies flash writes to the in-memory array and
//           records the touched blocks with flash_sd_mark_dirty()
//   Core 1: mounts the card, restores or creates the image file, then
//           writes dirty blocks back once the MSX stops modifying them
//
// The file has no header: it is the flash contents and nothing else. It is
// matched to the running cartridge by name and size only, so replacing the
// cartridge ROM with a different image of the same size and name will keep
// using the previously saved file.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pico/stdlib.h"
#include "ff.h"
#include "diskio.h"
#include "hw_config.h"
#include "sd_card.h"
#include "flash_sd.h"

#define FLASH_SD_MAX_BLOCKS  2048u   // 8 MB / 4 KB
#define FLASH_SD_MAP_WORDS   (FLASH_SD_MAX_BLOCKS / 32u)

// Quiet period the MSX must leave the flash alone before a write-back runs.
// Programming a save game issues a long burst of byte-program commands, and
// coalescing them into one pass keeps the card writes down to a handful.
#define FLASH_SD_QUIET_US    250000u

// Physical drive number for FatFS diskio (single SD card)
#define FLASH_SD_PDRV 0

static uint8_t  *flash_image = NULL;
static uint32_t  flash_image_size = 0;
static const uint8_t *flash_rom_src = NULL;
static uint32_t  flash_rom_len = 0;
static char      flash_file_path[80];

static volatile bool flash_sd_init_done = false;
static volatile bool flash_sd_file_ok = false;

static volatile uint32_t dirty_map[FLASH_SD_MAP_WORDS];
static volatile bool     dirty_pending = false;
static volatile uint32_t dirty_stamp_us = 0;

static FIL     flash_fil;
static bool    flash_fil_open = false;
static uint8_t flash_chunk[FLASH_SD_BLOCK_SIZE];

// -----------------------------------------------------------------------
// Path construction
// -----------------------------------------------------------------------
// The cartridge name comes from the PC tool and is derived from the ROM
// file name, so it can contain characters FAT does not accept. Long file
// names are enabled in the FatFS configuration, so only the reserved
// characters need replacing.
static void build_flash_path(const char *rom_name)
{
    static const char reserved[] = "\\/:*?\"<>|";

    size_t out = 0;
    flash_file_path[out++] = '/';

    for (size_t i = 0; rom_name[i] != '\0' && i < 50u; i++)
    {
        if (out >= sizeof(flash_file_path) - 5u)
            break;

        char c = rom_name[i];
        if ((unsigned char)c < 0x20u || strchr(reserved, c) != NULL)
            c = '_';
        flash_file_path[out++] = c;
    }

    // Trailing dots and spaces are not valid in FAT names.
    while (out > 1u && (flash_file_path[out - 1u] == ' ' || flash_file_path[out - 1u] == '.'))
        out--;

    if (out == 1u)
        flash_file_path[out++] = 'F';

    memcpy(&flash_file_path[out], ".FLA", 5u);
}

// -----------------------------------------------------------------------
// Dirty block tracking
// -----------------------------------------------------------------------
// Core 0 sets bits, Core 1 clears them, so every access goes through the
// atomic builtins. A block is cleared before its contents are copied out,
// so a program landing during the copy simply sets the bit again and is
// written on the following pass.
static inline void __not_in_flash_func(dirty_set)(uint32_t block)
{
    __atomic_fetch_or(&dirty_map[block >> 5], 1u << (block & 31u), __ATOMIC_RELAXED);
}

static inline void dirty_clear(uint32_t block)
{
    __atomic_fetch_and(&dirty_map[block >> 5], ~(1u << (block & 31u)), __ATOMIC_RELAXED);
}

static inline uint32_t dirty_word(uint32_t index)
{
    return __atomic_load_n(&dirty_map[index], __ATOMIC_RELAXED);
}

void __not_in_flash_func(flash_sd_mark_dirty)(uint32_t offset, uint32_t length)
{
    if (length == 0u || flash_image_size == 0u || offset >= flash_image_size)
        return;

    if (offset + length > flash_image_size)
        length = flash_image_size - offset;

    uint32_t first = offset >> FLASH_SD_BLOCK_SHIFT;
    uint32_t last  = (offset + length - 1u) >> FLASH_SD_BLOCK_SHIFT;

    if (last >= FLASH_SD_MAX_BLOCKS)
        last = FLASH_SD_MAX_BLOCKS - 1u;

    for (uint32_t b = first; b <= last; b++)
        dirty_set(b);

    dirty_stamp_us = time_us_32();
    dirty_pending = true;
}

// -----------------------------------------------------------------------
// Public state
// -----------------------------------------------------------------------
void flash_sd_configure(const char *rom_name, uint8_t *image, uint32_t image_size,
                        const uint8_t *rom_src, uint32_t rom_len)
{
    flash_image = image;
    flash_image_size = image_size;
    flash_rom_src = rom_src;
    flash_rom_len = rom_len;
    memset((void *)dirty_map, 0, sizeof(dirty_map));
    dirty_pending = false;
    build_flash_path(rom_name);
}

bool flash_sd_ready(void)
{
    // Acquire: the caller must see every write Core 1 made to the flash
    // array while restoring or creating the image.
    return __atomic_load_n(&flash_sd_init_done, __ATOMIC_ACQUIRE);
}

bool flash_sd_backed(void)
{
    return flash_sd_file_ok;
}

const char *flash_sd_path(void)
{
    return flash_file_path;
}

// -----------------------------------------------------------------------
// Core 1: image restore / creation
// -----------------------------------------------------------------------
// Rebuild the array from the cartridge ROM embedded in the firmware. Used
// when a restore from the card fails after part of the image was replaced.
static void flash_sd_stage_rom(void)
{
    uint32_t n = (flash_rom_len < flash_image_size) ? flash_rom_len : flash_image_size;

    if (flash_rom_src != NULL && n > 0u)
        memcpy(flash_image, flash_rom_src, n);
    else
        n = 0u;

    if (n < flash_image_size)
        memset(flash_image + n, 0xFFu, flash_image_size - n);
}

static bool flash_sd_restore(void)
{
    UINT io = 0;

    for (uint32_t off = 0; off < flash_image_size; off += FLASH_SD_BLOCK_SIZE)
    {
        uint32_t n = flash_image_size - off;
        if (n > FLASH_SD_BLOCK_SIZE)
            n = FLASH_SD_BLOCK_SIZE;

        if (f_read(&flash_fil, flash_chunk, (UINT)n, &io) != FR_OK || io != n)
        {
            flash_sd_stage_rom();
            return false;
        }

        memcpy(flash_image + off, flash_chunk, n);
    }

    return true;
}

static bool flash_sd_create(void)
{
    UINT io = 0;

    for (uint32_t off = 0; off < flash_image_size; off += FLASH_SD_BLOCK_SIZE)
    {
        uint32_t n = flash_image_size - off;
        if (n > FLASH_SD_BLOCK_SIZE)
            n = FLASH_SD_BLOCK_SIZE;

        memcpy(flash_chunk, flash_image + off, n);

        if (f_write(&flash_fil, flash_chunk, (UINT)n, &io) != FR_OK || io != n)
            return false;
    }

    return f_sync(&flash_fil) == FR_OK;
}

static void flash_sd_open_image(void)
{
    if (flash_image == NULL || flash_image_size == 0u)
        return;

    if (disk_initialize(FLASH_SD_PDRV) != 0)
        return;

    sd_card_t *sd = sd_get_by_num(FLASH_SD_PDRV);
    if (sd == NULL)
        return;

    if (f_mount(&sd->state.fatfs, "", 1) != FR_OK)
        return;

    flash_sd_file_ok = true;

    // An existing image of the expected size is the saved cartridge flash.
    // Anything else (missing, truncated, grown) is discarded and the array
    // keeps the ROM embedded in the firmware.
    if (f_open(&flash_fil, flash_file_path, FA_READ | FA_WRITE) == FR_OK)
    {
        if (f_size(&flash_fil) == (FSIZE_t)flash_image_size && flash_sd_restore())
        {
            flash_fil_open = true;
            return;
        }
        f_close(&flash_fil);
    }

    // No usable image yet. Create the whole thing now, before the bus goes
    // live. Deferring this until the MSX programs the flash does not work:
    // the file has to be opened with FA_CREATE_ALWAYS, which truncates it to
    // zero straight away, and the rest is only written once the MSX has been
    // quiet for a while - a game that saves and is then switched off leaves
    // an empty or partial file on the card, which the next boot rejects for
    // having the wrong size.
    if (f_open(&flash_fil, flash_file_path, FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK)
    {
        flash_sd_file_ok = false;
        return;
    }

    if (!flash_sd_create())
    {
        f_close(&flash_fil);
        f_unlink(flash_file_path);
        flash_sd_file_ok = false;
        return;
    }

    flash_fil_open = true;
}

// -----------------------------------------------------------------------
// Core 1: write-back
// -----------------------------------------------------------------------
static void flash_sd_flush(void)
{
    static uint32_t inflight[FLASH_SD_MAP_WORDS];

    bool wrote = false;
    bool failed = false;

    memset(inflight, 0, sizeof(inflight));

    for (uint32_t w = 0; w < FLASH_SD_MAP_WORDS; w++)
    {
        uint32_t bits = dirty_word(w);
        while (bits != 0u)
        {
            uint32_t bit = (uint32_t)__builtin_ctz(bits);
            bits &= ~(1u << bit);

            uint32_t block = (w << 5) + bit;
            uint32_t off = block << FLASH_SD_BLOCK_SHIFT;
            if (off >= flash_image_size)
            {
                dirty_clear(block);
                continue;
            }

            uint32_t n = flash_image_size - off;
            if (n > FLASH_SD_BLOCK_SIZE)
                n = FLASH_SD_BLOCK_SIZE;

            // Clear before copying out: a program that lands while the
            // block is in flight sets the bit again and is written next
            // pass. A failed write puts the bit back so nothing is lost.
            dirty_clear(block);
            memcpy(flash_chunk, flash_image + off, n);

            UINT io = 0;
            if (f_lseek(&flash_fil, (FSIZE_t)off) != FR_OK ||
                f_write(&flash_fil, flash_chunk, (UINT)n, &io) != FR_OK || io != n)
            {
                dirty_set(block);
                failed = true;
                continue;
            }

            inflight[w] |= 1u << bit;
            wrote = true;
        }
    }

    if (wrote && f_sync(&flash_fil) != FR_OK)
    {
        // The data may still be sitting in FatFS or the card, so requeue
        // everything written in this pass for another attempt.
        for (uint32_t w = 0; w < FLASH_SD_MAP_WORDS; w++)
        {
            uint32_t bits = inflight[w];
            while (bits != 0u)
            {
                uint32_t bit = (uint32_t)__builtin_ctz(bits);
                bits &= ~(1u << bit);
                dirty_set((w << 5) + bit);
            }
        }
        failed = true;
    }

    if (failed)
    {
        dirty_stamp_us = time_us_32();
        dirty_pending = true;
    }
}

void __not_in_flash_func(flash_sd_task)(void)
{
    flash_sd_open_image();
    __atomic_store_n(&flash_sd_init_done, true, __ATOMIC_RELEASE);

    while (true)
    {
        if (!flash_fil_open || !dirty_pending)
        {
            tight_loop_contents();
            continue;
        }

        // Wait until the MSX has left the flash alone for a while so a
        // burst of byte programs turns into a single pass over the card.
        if ((time_us_32() - dirty_stamp_us) < FLASH_SD_QUIET_US)
        {
            tight_loop_contents();
            continue;
        }

        dirty_pending = false;
        flash_sd_flush();
    }
}
