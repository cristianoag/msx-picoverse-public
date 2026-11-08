// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_eeprom.c - Microwire (93C46 style, x8) EEPROM model. See c2_eeprom.h.
//
// Bit timing follows what the Carnivore2 software drives: DI is sampled on the
// rising edge of CK while CS is high, DO shows 0 after the last address bit and
// then the data bits MSB first, and a programming cycle starts when CS falls.
// Programming completes instantly so the ready poll sees DO = 1.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "c2_eeprom.h"

C2_FUNC void c2_eeprom_init(c2_eeprom_t *e)
{
    memset(e, 0, sizeof(*e));
    memset(e->data, 0xFF, sizeof(e->data));
    e->do_pin = 1u;
}

C2_FUNC uint8_t c2_eeprom_do(const c2_eeprom_t *e)
{
    return e->do_pin;
}

static C2_FUNC void set_data(c2_eeprom_t *e, uint8_t addr, uint8_t value)
{
    addr &= C2_EEPROM_SIZE - 1u;
    if (e->data[addr] != value) { e->data[addr] = value; e->dirty = true; }
}

static C2_FUNC void finish_frame(c2_eeprom_t *e)
{
    if (e->state == EE_WRITE_DONE && e->write_enabled)
    {
        if (e->pending_all) for (uint8_t i = 0u; i < C2_EEPROM_SIZE; ++i) set_data(e, i, (uint8_t)e->shift);
        else set_data(e, e->addr, (uint8_t)e->shift);
    }
    else if (e->state == EE_IGNORE && e->write_enabled)
    {
        if (e->op == 3u) set_data(e, e->addr, 0xFFu);
        else if (e->pending_erase_all) for (uint8_t i = 0u; i < C2_EEPROM_SIZE; ++i) set_data(e, i, 0xFFu);
    }
    e->pending_all = e->pending_erase_all = false;
    e->state = EE_IDLE;
    e->do_pin = 1u;
}

static C2_FUNC void decode_command(c2_eeprom_t *e)
{
    uint8_t opcode = (uint8_t)((e->shift >> 7) & 3u);
    uint8_t addr = (uint8_t)(e->shift & 0x7Fu);

    e->addr = addr;
    switch (opcode)
    {
    case 2u:    // READ
        e->op = 2u;
        e->state = EE_READ;
        e->out_bit = 8u;     // first clock after the address loads D7, DO is the dummy 0 until then
        e->do_pin = 0u;
        break;
    case 1u:    // WRITE
        e->op = 1u;
        e->state = EE_WRITE_DATA;
        e->nbits = 0u;
        e->shift = 0u;
        break;
    case 3u:    // ERASE
        e->op = 3u;
        e->state = EE_IGNORE;
        break;
    default:    // 00: EWDS / WRAL / ERAL / EWEN selected by A6..A5
        e->op = 0u;
        switch (addr >> 5)
        {
        case 3u: e->write_enabled = true; e->state = EE_IGNORE; break;
        case 0u: e->write_enabled = false; e->state = EE_IGNORE; break;
        case 2u: e->pending_erase_all = true; e->state = EE_IGNORE; break;
        default: e->pending_all = true; e->state = EE_WRITE_DATA; e->nbits = 0u; e->shift = 0u; break;
        }
        break;
    }
}

static C2_FUNC void clock_rising(c2_eeprom_t *e, uint8_t di)
{
    switch (e->state)
    {
    case EE_IDLE:
        if (di)
        {
            e->state = EE_CMD;
            e->nbits = 0u;
            e->shift = 0u;
        }
        break;

    case EE_CMD:
        e->shift = (uint16_t)((e->shift << 1) | (di & 1u));
        if (++e->nbits == 9u) decode_command(e);
        break;

    case EE_WRITE_DATA:
        e->shift = (uint16_t)((e->shift << 1) | (di & 1u));
        if (++e->nbits == 8u) e->state = EE_WRITE_DONE;
        break;

    case EE_READ:
        if (e->out_bit == 0u)
        {
            e->addr = (uint8_t)((e->addr + 1u) & (C2_EEPROM_SIZE - 1u));   // sequential read
            e->out_bit = 8u;
        }
        e->do_pin = (e->data[e->addr] >> (e->out_bit - 1u)) & 1u;
        e->out_bit--;
        break;

    default:
        break;
    }
}

C2_FUNC void c2_eeprom_pins(c2_eeprom_t *e, uint8_t cs, uint8_t ck, uint8_t di)
{
    uint8_t old_cs = e->cs, old_ck = e->ck;

    e->cs = cs & 1u;
    e->ck = ck & 1u;

    if (old_cs && !e->cs)
    {
        finish_frame(e);
        return;
    }
    if (!e->cs)
    {
        e->state = EE_IDLE;
        e->do_pin = 1u;
        return;
    }
    if (!old_cs) e->do_pin = 1u;    // ready/busy status after CS rises
    if (!old_ck && e->ck) clock_rising(e, di);
}
