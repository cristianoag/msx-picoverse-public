// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_store.c - Carnivore2 FlashROM write-back cache and EEPROM persistence.
// See c2_store.h.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "c2_store.h"

#define EEPROM_MAGIC 0x45453243u    // "C2EE"

uint16_t c2s_map[C2S_SECTORS];
uint16_t c2s_tag[C2S_CACHE_BLOCKS];
uint8_t c2s_cache[C2S_CACHE_BLOCKS * C2S_BLOCK_SIZE];

static uint16_t free_stack[C2S_SLOTS];
static uint16_t free_count;
static uint16_t dirty_q[C2S_SLOTS];     // FIFO of slots waiting to be committed
static uint16_t dirty_head, dirty_count;
static uint16_t slot_owner[C2S_SLOTS];
static uint16_t erased_count;           // sectors marked C2S_ERASED, flash not erased yet
static uint16_t erased_scan;            // next sector the commit step looks at

static uint8_t eeprom_shadow[256];
static bool eeprom_dirty;

// Single 4 KB buffer in SRAM: PSRAM and flash are both unreachable while the
// flash is being programmed.
static uint8_t commit_buf[C2S_SECTOR_SIZE];

typedef struct {
    uint32_t offset;        // absolute flash offset of the 4 KB sector
    const uint8_t *buf;
    bool erase;
    uint16_t page_mask;     // 256 byte pages to program
} commit_op_t;

static inline uint8_t *slot_ptr(uint16_t slot)
{
    return (uint8_t *)(PSRAM_BASE_ADDR + C2_PSRAM_OVL_BASE + ((uint32_t)slot << 12));
}

static inline const uint8_t *flash_sector_ptr(uint32_t sector)
{
    return (const uint8_t *)(XIP_BASE + C2_FLASH_IMAGE_OFFSET + (sector << 12));
}

static void __no_inline_not_in_flash_func(commit_cb)(void *param)
{
    const commit_op_t *op = (const commit_op_t *)param;
    uint32_t m0_timing = qmi_hw->m[0].timing;
    uint32_t m1_timing = qmi_hw->m[1].timing;
    uint32_t m1_rfmt = qmi_hw->m[1].rfmt;
    uint32_t m1_rcmd = qmi_hw->m[1].rcmd;
    uint32_t m1_wfmt = qmi_hw->m[1].wfmt;
    uint32_t m1_wcmd = qmi_hw->m[1].wcmd;
    uint32_t xip_ctrl = xip_ctrl_hw->ctrl;

    if (op->erase) flash_range_erase(op->offset, C2S_SECTOR_SIZE);
    for (uint32_t page = 0u; page < 16u; ++page)
    {
        if (op->page_mask & (1u << page))
            flash_range_program(op->offset + page * FLASH_PAGE_SIZE, op->buf + page * FLASH_PAGE_SIZE, FLASH_PAGE_SIZE);
    }

    // The SDK re-enters XIP with the boot time QMI settings; put back the fast
    // flash timing and the PSRAM window that the firmware configured.
    qmi_hw->m[0].timing = m0_timing;
    qmi_hw->m[1].timing = m1_timing;
    qmi_hw->m[1].rfmt = m1_rfmt;
    qmi_hw->m[1].rcmd = m1_rcmd;
    qmi_hw->m[1].wfmt = m1_wfmt;
    qmi_hw->m[1].wcmd = m1_wcmd;
    xip_ctrl_hw->ctrl = xip_ctrl;
}

static bool run_commit(const commit_op_t *op)
{
    return flash_safe_execute(commit_cb, (void *)op, 1000u) == PICO_OK;
}

void c2s_init(void)
{
    for (uint32_t i = 0u; i < C2S_SECTORS; ++i) c2s_map[i] = C2S_NO_SLOT;
    for (uint32_t i = 0u; i < C2S_CACHE_BLOCKS; ++i) c2s_tag[i] = C2S_TAG_INVALID;
    for (uint32_t i = 0u; i < C2S_SLOTS; ++i) free_stack[i] = (uint16_t)(C2S_SLOTS - 1u - i);
    free_count = C2S_SLOTS;
    dirty_head = dirty_count = 0u;
    erased_count = erased_scan = 0u;
    eeprom_dirty = false;
}

// ---------------------------------------------------------------------
// SRAM read cache
// ---------------------------------------------------------------------
uint8_t __not_in_flash_func(c2s_cache_fill)(uint32_t addr)
{
    uint32_t block = addr >> C2S_BLOCK_SHIFT;
    uint32_t index = block % C2S_CACHE_BLOCKS;

    memcpy(&c2s_cache[index << C2S_BLOCK_SHIFT],
           (const uint8_t *)(XIP_BASE + C2_FLASH_IMAGE_OFFSET + (block << C2S_BLOCK_SHIFT)),
           C2S_BLOCK_SIZE);
    c2s_tag[index] = (uint16_t)block;
    return c2s_cache[(index << C2S_BLOCK_SHIFT) | (addr & (C2S_BLOCK_SIZE - 1u))];
}

// The flash content of a sector changed (commit): drop its cached blocks.
static void cache_invalidate_sector(uint32_t sector)
{
    uint32_t first = sector << (12u - C2S_BLOCK_SHIFT);
    for (uint32_t block = first; block < first + (C2S_SECTOR_SIZE >> C2S_BLOCK_SHIFT); ++block)
    {
        uint32_t index = block % C2S_CACHE_BLOCKS;
        if (c2s_tag[index] == (uint16_t)block) c2s_tag[index] = C2S_TAG_INVALID;
    }
}

// ---------------------------------------------------------------------
// Sector cache
// ---------------------------------------------------------------------
static bool commit_slot(uint16_t slot)
{
    uint32_t sector = slot_owner[slot];
    const uint8_t *flash = flash_sector_ptr(sector);
    const uint8_t *cached = slot_ptr(slot);
    bool need_erase = false;
    uint16_t page_mask = 0u;

    memcpy(commit_buf, cached, C2S_SECTOR_SIZE);

    for (uint32_t i = 0u; i < C2S_SECTOR_SIZE; ++i)
    {
        uint8_t old = flash[i], now = commit_buf[i];
        if (old == now) continue;
        if ((uint8_t)(~old & now) != 0u) need_erase = true;
        page_mask |= (uint16_t)(1u << (i >> 8));
    }

    if (page_mask != 0u)
    {
        commit_op_t op = {
            .offset = C2_FLASH_IMAGE_OFFSET + (sector << 12),
            .buf = commit_buf,
            .erase = need_erase,
            .page_mask = page_mask,
        };
        if (need_erase)
        {
            // After an erase every page that is not blank has to be written back.
            op.page_mask = 0u;
            for (uint32_t page = 0u; page < 16u; ++page)
            {
                for (uint32_t i = 0u; i < FLASH_PAGE_SIZE; ++i)
                {
                    if (commit_buf[page * FLASH_PAGE_SIZE + i] != 0xFFu) { op.page_mask |= (uint16_t)(1u << page); break; }
                }
            }
        }
        if (!run_commit(&op)) return false;
    }

    cache_invalidate_sector(sector);
    c2s_map[sector] = C2S_NO_SLOT;
    free_stack[free_count++] = slot;
    return true;
}

static void commit_oldest(void)
{
    if (dirty_count == 0u) return;
    uint16_t slot = dirty_q[dirty_head];
    if (commit_slot(slot))
    {
        dirty_head = (uint16_t)((dirty_head + 1u) % C2S_SLOTS);
        dirty_count--;
    }
}

static uint16_t __not_in_flash_func(ensure_slot)(uint32_t sector, bool load)
{
    uint16_t slot = c2s_map[sector];
    if (slot < C2S_SLOTS) return slot;

    bool was_erased = (slot == C2S_ERASED);

    while (free_count == 0u) commit_oldest();

    slot = free_stack[--free_count];
    slot_owner[slot] = (uint16_t)sector;
    if (was_erased)
    {
        memset(slot_ptr(slot), 0xFF, C2S_SECTOR_SIZE);
        erased_count--;
    }
    else if (load)
    {
        memcpy(slot_ptr(slot), flash_sector_ptr(sector), C2S_SECTOR_SIZE);
    }
    c2s_map[sector] = slot;
    dirty_q[(dirty_head + dirty_count) % C2S_SLOTS] = slot;
    dirty_count++;
    return slot;
}

void __not_in_flash_func(c2s_flash_program)(uint32_t addr, uint8_t data)
{
    uint32_t sector = addr >> 12;
    uint8_t old = c2s_flash_read(addr);
    uint8_t now = old & data;

    if (now == old) return;
    uint16_t slot = ensure_slot(sector, true);
    slot_ptr(slot)[addr & 0xFFFu] = now;
}

// An erase is instant: sectors without a cache slot are only marked as erased
// (reads return 0xFF) and the flash is erased later by c2s_commit_step, so a
// full chip erase does not stall the MSX for minutes.
void __not_in_flash_func(c2s_flash_erase)(uint32_t start, uint32_t length)
{
    for (uint32_t addr = start; addr < start + length; addr += C2S_SECTOR_SIZE)
    {
        uint32_t sector = addr >> 12;
        uint16_t slot = c2s_map[sector];

        if (slot == C2S_NO_SLOT)
        {
            c2s_map[sector] = C2S_ERASED;
            erased_count++;
        }
        else if (slot < C2S_SLOTS)
        {
            memset(slot_ptr(slot), 0xFF, C2S_SECTOR_SIZE);
        }
    }
}

bool c2s_has_work(void)
{
    return dirty_count != 0u || erased_count != 0u || eeprom_dirty;
}

uint32_t c2s_free_slots(void)
{
    return free_count;
}

// ---------------------------------------------------------------------
// EEPROM
// ---------------------------------------------------------------------
void c2s_eeprom_load(uint8_t *data, uint32_t size)
{
    const uint32_t *flash = (const uint32_t *)(XIP_BASE + C2_EEPROM_FLASH_OFFSET);
    if (flash[0] == EEPROM_MAGIC)
        memcpy(data, (const uint8_t *)(flash + 1), size);
}

void c2s_eeprom_changed(const uint8_t *data, uint32_t size)
{
    uint32_t magic = EEPROM_MAGIC;
    memset(eeprom_shadow, 0xFF, sizeof(eeprom_shadow));
    memcpy(eeprom_shadow, &magic, sizeof(magic));
    memcpy(eeprom_shadow + sizeof(magic), data, size);
    eeprom_dirty = true;
}

static void commit_eeprom(void)
{
    memcpy(commit_buf, eeprom_shadow, sizeof(eeprom_shadow));
    commit_op_t op = {
        .offset = C2_EEPROM_FLASH_OFFSET,
        .buf = commit_buf,
        .erase = true,
        .page_mask = 1u,
    };
    if (run_commit(&op)) eeprom_dirty = false;
}

// Commit one pending sector erase. Sectors that are already blank in flash are
// released without touching the flash (up to a small scan budget per call).
static void commit_erased(void)
{
    for (uint32_t scanned = 0u; scanned < 64u && erased_count != 0u; ++scanned)
    {
        while (c2s_map[erased_scan] != C2S_ERASED) erased_scan = (uint16_t)((erased_scan + 1u) % C2S_SECTORS);
        uint32_t sector = erased_scan;

        const uint32_t *w = (const uint32_t *)flash_sector_ptr(sector);
        bool blank = true;
        for (uint32_t i = 0u; i < C2S_SECTOR_SIZE / 4u; ++i) if (w[i] != 0xFFFFFFFFu) { blank = false; break; }

        if (!blank)
        {
            commit_op_t op = {
                .offset = C2_FLASH_IMAGE_OFFSET + (sector << 12),
                .buf = commit_buf,
                .erase = true,
                .page_mask = 0u,
            };
            if (!run_commit(&op)) return;
        }
        cache_invalidate_sector(sector);
        c2s_map[sector] = C2S_NO_SLOT;
        erased_count--;
        if (!blank) return;     // one real flash erase per call
    }
}

bool c2s_commit_step(void)
{
    if (dirty_count != 0u) commit_oldest();
    else if (erased_count != 0u) commit_erased();
    else if (eeprom_dirty) commit_eeprom();
    return c2s_has_work();
}
