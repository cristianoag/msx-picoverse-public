#pragma once

#include "pvaudio/pva.h"

typedef struct {
    const uint8_t *rom;
    uint8_t subslot;
    uint8_t bank;
} picoamp_bus_t;

static inline void PVA_RAM_FUNC(picoamp_bus_write)(picoamp_bus_t *state, uint16_t addr, uint8_t data)
{
    if (addr == 0xFFFFu)
        state->subslot = data;
    else if ((addr >> 14) == 1u)
    {
        uint8_t subslot = (state->subslot >> 2) & 3u;
        if (subslot == 3u)
            pva_mem_write(addr, data);
        else if (subslot == 0u && (addr & 0xF800u) == 0x6000u)
            state->bank = data & 3u;
    }
}

static inline uint8_t PVA_RAM_FUNC(picoamp_bus_read)(const picoamp_bus_t *state, uint16_t addr)
{
    if (addr == 0xFFFFu)
        return (uint8_t)~state->subslot;
    if ((addr >> 14) != 1u)
        return 0xFFu;
    uint8_t subslot = (state->subslot >> 2) & 3u;
    if (subslot == 0u)
        return state->rom[((uint32_t)state->bank << 14) | (addr & 0x3FFFu)];
    if (subslot == 3u)
        return pva_mem_read(addr);
    return 0xFFu;
}
