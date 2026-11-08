// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_flash.c - M29W640GB command interface model. See c2_flash.h.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "c2_flash.h"

#define FLASH_SIZE      0x800000u
#define BOOT_AREA       0x010000u   // 8 boot blocks of 8 KB
#define BOOT_BLOCK      0x002000u
#define MAIN_BLOCK      0x010000u

C2_FUNC void c2_flash_block_bounds(uint32_t addr, uint32_t *start, uint32_t *length)
{
    if (addr < BOOT_AREA)
    {
        *start = addr & ~(BOOT_BLOCK - 1u);
        *length = BOOT_BLOCK;
    }
    else
    {
        *start = addr & ~(MAIN_BLOCK - 1u);
        *length = MAIN_BLOCK;
    }
}

C2_FUNC void c2_flash_init(c2_flash_t *f, const c2_flash_backend_t *be)
{
    memset(f, 0, sizeof(*f));
    f->be = be;
}

C2_FUNC void c2_flash_control(c2_flash_t *f, bool reset_n, bool vpp_wp)
{
    if (!reset_n && !f->in_reset)
    {
        f->state = C2F_READ;
        f->mode = C2F_MODE_ARRAY;
        f->bypass = false;
    }
    // VPP/WP# does not protect the boot blocks on the Carnivore2: C2MAN rewrites
    // them with ConfFl at its power-on value.
    (void)vpp_wp;
}

// CFI query data, indexed by word address (byte address / 2).
static C2_FUNC uint8_t cfi_byte(uint32_t word)
{
    static const uint8_t table[0x50 - 0x10] = {
        /* 10 */ 0x51, 0x52, 0x59, 0x02, 0x00, 0x40, 0x00, 0x00,
        /* 18 */ 0x00, 0x00, 0x00, 0x27, 0x36, 0xB5, 0xC5, 0x04,
        /* 20 */ 0x00, 0x0A, 0x00, 0x04, 0x00, 0x04, 0x00, 0x17,
        /* 28 */ 0x02, 0x00, 0x00, 0x00, 0x02, 0x07, 0x00, 0x20,
        /* 30 */ 0x00, 0x7E, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        /* 38 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 40 */ 0x50, 0x52, 0x49, 0x31, 0x33, 0x0C, 0x02, 0x01,
        /* 48 */ 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
    };
    if (word < 0x10u || word >= 0x50u) return 0u;
    return table[word - 0x10u];
}

static C2_FUNC uint8_t autoselect_byte(uint32_t addr)
{
    switch (addr & 0xFFu)
    {
    case 0x00: return 0x20u;    // manufacturer: ST/Numonyx
    case 0x02: return 0x7Eu;    // device code 1
    case 0x04: return 0x00u;    // block protection verify: unprotected
    case 0x06: return 0x80u;    // extended memory block verify
    case 0x1C: return 0x10u;    // device code 2 (M29W640G)
    case 0x1E: return 0x00u;    // device code 3 (bottom boot)
    default:   return 0x00u;
    }
}

C2_FUNC uint8_t c2_flash_read(c2_flash_t *f, uint32_t addr)
{
    if (f->in_reset) return 0xFFu;
    switch (f->mode)
    {
    case C2F_MODE_AUTOSELECT: return autoselect_byte(addr);
    case C2F_MODE_CFI:        return (addr & 1u) ? 0u : cfi_byte(addr >> 1);
    default:                  return f->be->read(f->be->ctx, addr & (FLASH_SIZE - 1u));
    }
}

static C2_FUNC void program_byte(c2_flash_t *f, uint32_t addr, uint8_t data)
{
    addr &= FLASH_SIZE - 1u;
    f->be->program(f->be->ctx, addr, data);
    f->programmed_bytes++;
}

static C2_FUNC void erase_block(c2_flash_t *f, uint32_t addr)
{
    uint32_t start, length;
    c2_flash_block_bounds(addr & (FLASH_SIZE - 1u), &start, &length);
    f->be->erase(f->be->ctx, start, length);
    f->erased_blocks++;
}

static C2_FUNC void erase_chip(c2_flash_t *f)
{
    uint32_t addr = 0u;
    while (addr < FLASH_SIZE)
    {
        uint32_t start, length;
        c2_flash_block_bounds(addr, &start, &length);
        erase_block(f, start);
        addr = start + length;
    }
}

C2_FUNC void c2_flash_write(c2_flash_t *f, uint32_t addr, uint8_t data)
{
    uint32_t cmd = addr & 0xFFFu;

    if (f->in_reset) return;

    if (data == 0xF0u && f->state != C2F_PROGRAM && f->state != C2F_MULTI &&
        f->state != C2F_BYPASS_PROGRAM)
    {
        f->state = C2F_READ;
        f->mode = C2F_MODE_ARRAY;
        return;
    }

    switch (f->state)
    {
    case C2F_PROGRAM:
        program_byte(f, addr, data);
        f->state = C2F_READ;
        f->mode = C2F_MODE_ARRAY;
        return;

    case C2F_BYPASS_PROGRAM:
        program_byte(f, addr, data);
        f->state = C2F_READ;
        return;

    case C2F_MULTI:
        program_byte(f, addr, data);
        if (--f->count == 0u) { f->state = C2F_READ; f->mode = C2F_MODE_ARRAY; }
        return;

    case C2F_BYPASS_EXIT:
        f->bypass = false;
        f->state = C2F_READ;
        return;

    case C2F_UNLOCK1:
        f->state = (cmd == 0x555u && data == 0x55u) ? C2F_UNLOCK2 : C2F_READ;
        return;

    case C2F_UNLOCK2:
        f->state = C2F_READ;
        if (cmd != 0xAAAu) return;
        switch (data)
        {
        case 0x90u: f->mode = C2F_MODE_AUTOSELECT; break;
        case 0xA0u: f->state = C2F_PROGRAM; break;
        case 0x80u: f->state = C2F_ERASE1; break;
        case 0x20u: f->bypass = true; f->mode = C2F_MODE_ARRAY; break;
        default: break;
        }
        return;

    case C2F_ERASE1:
        f->state = (cmd == 0xAAAu && data == 0xAAu) ? C2F_ERASE2 : C2F_READ;
        return;

    case C2F_ERASE2:
        f->state = (cmd == 0x555u && data == 0x55u) ? C2F_ERASE3 : C2F_READ;
        return;

    case C2F_ERASE3:
        f->state = C2F_READ;
        if (cmd == 0xAAAu && data == 0x10u) erase_chip(f);
        else if (data == 0x30u) { erase_block(f, addr); f->state = C2F_ERASE_MORE; }
        return;

    case C2F_ERASE_MORE:
        if (data == 0x30u) { erase_block(f, addr); return; }
        f->state = C2F_READ;
        break;

    default:
        break;
    }

    // C2F_READ and fall-through from the states above: a new command cycle.
    if (f->bypass)
    {
        if (data == 0xA0u) f->state = C2F_BYPASS_PROGRAM;
        else if (data == 0x90u) f->state = C2F_BYPASS_EXIT;
        return;
    }

    if (data == 0xAAu && cmd == 0xAAAu) f->state = C2F_UNLOCK1;
    else if (data == 0x50u) { f->state = C2F_MULTI; f->count = 2u; }
    else if (data == 0x56u) { f->state = C2F_MULTI; f->count = 4u; }
    else if (data == 0x98u && cmd == 0x0AAu) f->mode = C2F_MODE_CFI;
}
