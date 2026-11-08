// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_flash.h - M29W640GB (8 MB, bottom boot) command interface model.
//
// The Carnivore2 FlashROM is a byte-mode M29W640GB. This module implements the
// bus level behaviour software expects from it: read/reset, autoselect, CFI
// query, byte program, double/quadruple byte program, unlock bypass, block and
// chip erase. Operations complete immediately (data polling and toggle bits
// already show the final state), which is what the Carnivore2 utilities poll
// for. The storage itself is provided through c2_flash_backend_t.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef C2_FLASH_H
#define C2_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "c2_config.h"

typedef struct {
    void *ctx;
    uint8_t (*read)(void *ctx, uint32_t addr);
    void    (*program)(void *ctx, uint32_t addr, uint8_t data);   // stored value = old & data
    void    (*erase)(void *ctx, uint32_t start, uint32_t length); // fill with 0xFF
} c2_flash_backend_t;

typedef enum {
    C2F_READ = 0,
    C2F_UNLOCK1,        // 0xAA written at 0xAAA
    C2F_UNLOCK2,        // 0x55 written at 0x555
    C2F_PROGRAM,        // next write is the byte to program
    C2F_MULTI,          // double/quad byte program, "count" bytes left
    C2F_ERASE1,         // 0x80 accepted, expects 0xAA
    C2F_ERASE2,         // expects 0x55
    C2F_ERASE3,         // expects 0x30 (block) or 0x10 (chip)
    C2F_ERASE_MORE,     // further 0x30 accepted after a block erase
    C2F_BYPASS_PROGRAM, // unlock bypass: next write is data
    C2F_BYPASS_EXIT     // unlock bypass reset second cycle
} c2_flash_state_t;

typedef enum { C2F_MODE_ARRAY = 0, C2F_MODE_AUTOSELECT, C2F_MODE_CFI } c2_flash_mode_t;

typedef struct {
    const c2_flash_backend_t *be;
    c2_flash_state_t state;
    c2_flash_mode_t mode;
    bool bypass;
    bool in_reset;      // RP# low: chip does not respond
    uint8_t count;
    uint32_t erased_blocks;   // statistics for tests / debug
    uint32_t programmed_bytes;
} c2_flash_t;

C2_FUNC void c2_flash_init(c2_flash_t *f, const c2_flash_backend_t *be);
C2_FUNC uint8_t c2_flash_read(c2_flash_t *f, uint32_t addr);
C2_FUNC void c2_flash_write(c2_flash_t *f, uint32_t addr, uint8_t data);
C2_FUNC void c2_flash_control(c2_flash_t *f, bool reset_n, bool vpp_wp);

// Boot block geometry helper (8 x 8 KB followed by 127 x 64 KB).
C2_FUNC void c2_flash_block_bounds(uint32_t addr, uint32_t *start, uint32_t *length);

#endif // C2_FLASH_H
