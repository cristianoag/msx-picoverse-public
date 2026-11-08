// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_core.c - Carnivore2 (RBSC) cartridge model. See c2_core.h.
//
// The decoding rules follow mcscc.vhd (firmware 2.50) signal by signal; the VHDL
// quirks that existing Carnivore2 software can depend on (for example memory
// mapped FD/FE writes landing in MAP_FC, or the bank 3 32 KB match using
// B3AdrD(0)) are reproduced on purpose.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "c2_core.h"

#define HW (c->hw)

#define hw_has(fn) ((fn) != 0)

// ---------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------
C2_FUNC void c2_init(c2_t *c, const c2_hw_t *hw)
{
    memset(c, 0, sizeof(*c));
    c->hw = hw;
    c->slt_save[1] = 0x55u;

    c->cardmdr = 0x30u;
    c->conffl = 0x02u;
    c->mconf = c->a_mconf = 0xFFu;
    c->slm_cfg = c->a_slm_cfg = 0xE4u;
    c->bank[0].mult = c->a_bank[0].mult = 0x85u;
    c->bank[0].mask = c->a_bank[0].mask = 0xF8u;
    c->bank[0].addr = c->a_bank[0].addr = 0x50u;
    c->bank[0].maskr = c->a_bank[0].maskr = 0x03u;
    c->bank[0].adrd = c->a_bank[0].adrd = 0x40u;
    c->map[0] = 3u; c->map[1] = 2u; c->map[2] = 1u; c->map[3] = 0u;
    c->fm_7ff6_b4 = 1u;
    c->scc_bank2 = 2u;
    c->scc_bank3 = 3u;
    c->flash_array = true;
    c->fast_dirty = true;
    c->kc = 1u;
    c->mapper_port_read = true;
    c->lvl = 0x1Bu;
    c->lvl1 = 0x1Bu;

    if (hw_has(hw->flash_ctrl)) hw->flash_ctrl(hw->ctx, true, false);
    if (hw_has(hw->levels)) hw->levels(hw->ctx, c->lvl, c->lvl1);
    if (hw_has(hw->beeper)) hw->beeper(hw->ctx, c->kc);
    if (hw_has(hw->scc_plus)) hw->scc_plus(hw->ctx, false);
}

// ---------------------------------------------------------------------
// Configuration shadow -> active copy ("delayed reconfiguration")
// ---------------------------------------------------------------------
static C2_FUNC void c2_apply(c2_t *c)
{
    c->addrfr = c->a_addrfr;
    memcpy(c->bank, c->a_bank, sizeof(c->bank));
    c->mconf = c->a_mconf;
    c->nsreg = c->a_nsreg;
    c->slm_cfg = c->a_slm_cfg;
    c->scart_cfg = c->a_scart_cfg;
    c->scart_slt = c->a_scart_slt;
    c->scart_stbl = c->a_scart_stbl;
    c->reload_pending = false;
    c->fast_dirty = true;
}

static C2_FUNC inline void c2_shadow_written(c2_t *c)
{
    if ((c->cardmdr & C2_MDR_DELAYED) == 0u) c2_apply(c);
    else c->reload_pending = true;
}

C2_FUNC void c2_fetch_zero(c2_t *c)
{
    if ((c->cardmdr & C2_MDR_DELAYED) != 0u && (c->cardmdr & C2_MDR_RELOAD_RD) == 0u)
        c2_apply(c);
}

// ---------------------------------------------------------------------
// Expanded slot decode (Sltsl_C/D/M/F)
// ---------------------------------------------------------------------
C2_FUNC uint8_t c2_devices(const c2_t *c, uint16_t addr)
{
    uint8_t m = c->mconf;
    uint8_t sub = (uint8_t)((c->exp_slt >> ((addr >> 14) * 2u)) & 3u);
    uint8_t dev = 0u;

    if (m & 0x80u)
    {
        if ((m & 1u) && sub == (c->slm_cfg & 3u)) dev |= C2_DEV_MAIN;
        if ((m & 2u) && sub == ((c->slm_cfg >> 2) & 3u)) dev |= C2_DEV_IDE;
        if ((m & 4u) && sub == ((c->slm_cfg >> 4) & 3u)) dev |= C2_DEV_MAPPER;
        if ((m & 8u) && sub == ((c->slm_cfg >> 6) & 3u)) dev |= C2_DEV_FM;
    }
    else
    {
        if (m & 1u) dev |= C2_DEV_MAIN;
        if ((m & 3u) == 2u) dev |= C2_DEV_IDE;
        if ((m & 7u) == 4u) dev |= C2_DEV_MAPPER;
        if ((m & 15u) == 8u) dev |= C2_DEV_FM;
    }
    return dev;
}

// ---------------------------------------------------------------------
// Cartridge bank windows (R1..R4)
// ---------------------------------------------------------------------
typedef struct {
    bool flash_cs;
    bool ram_cs;
    bool write_en;
    uint32_t addr;      // 23 bit flash address (RAM uses the low 21 bits)
} c2_bank_res_t;

static C2_FUNC inline bool nsc(const c2_t *c) { return (c->bank[3].mult & 7u) == 1u; }
static C2_FUNC inline bool nsc_sccp(const c2_t *c) { return nsc(c) && c->bank[3].maskr == 1u; }

// Size code (7..4) of the window of bank b for address a, or 8 when it does not hit.
static C2_FUNC uint8_t c2_bank_hit(const c2_t *c, uint8_t b, uint16_t a)
{
    const c2_bank_t *k = &c->bank[b];
    uint8_t mult = k->mult;
    uint8_t adrd = k->adrd;
    uint8_t size = mult & 7u;
    uint8_t m6 = mult & 0x40u;

    if (b == 3u && nsc(c)) size = c->bank[2].mult & 7u;
    if (mult & 0x08u) return 8u;

    switch (size)
    {
    case 7u: return 7u;
    case 6u:
        if ((b == 2u ? (adrd & 1u) : ((adrd >> 7) & 1u)) == (uint8_t)(a >> 15)) return 6u;
        return 8u;
    case 5u:
        if (((adrd >> 6) & 3u) == (uint8_t)(a >> 14)) return 5u;
        return 8u;
    case 4u:
        if ((((adrd >> 7) & 1u) == (uint8_t)(a >> 15) || m6 == 0u) &&
            ((adrd >> 5) & 3u) == (uint8_t)((a >> 13) & 3u)) return 4u;
        return 8u;
    default:
        return 8u;
    }
}

static C2_FUNC void c2_bank_decode(const c2_t *c, uint16_t a, c2_bank_res_t *r)
{
    bool have_addr = false;
    bool nsccp = nsc_sccp(c);
    bool dec1ffe = (a & 0x1FFEu) == 0x1FFEu;
    uint8_t ma = c->scc_mode_a, mb = c->scc_mode_b;

    r->flash_cs = r->ram_cs = r->write_en = false;
    r->addr = 0u;

    for (uint8_t b = 0u; b < 4u; ++b)
    {
        uint8_t size = c2_bank_hit(c, b, a);
        if (size == 8u) continue;

        const c2_bank_t *k = &c->bank[b];
        bool is_ram = (k->mult & 0x20u) != 0u;
        if (is_ram) r->ram_cs = true; else r->flash_cs = true;

        if (k->mult & 0x10u)
        {
            bool allow;
            switch (b)
            {
            case 0:  allow = !nsccp || (mb & 0x10u) || (ma & 0x10u) || (mb & 0x01u); break;
            case 1:  allow = !nsccp || (mb & 0x10u) || (mb & 0x02u) || ((ma & 0x10u) && !dec1ffe); break;
            case 2:  allow = !nsccp || (mb & 0x10u) || ((mb & 0x04u) && (mb & 0x20u)); break;
            default: allow = !nsccp || ((mb & 0x10u) && !dec1ffe); break;
            }
            if (allow) r->write_en = true;
        }

        if (!have_addr)
        {
            const c2_bank_t *m = (b == 3u && nsc(c)) ? &c->bank[2] : k;
            uint32_t n = (uint32_t)(m->maskr & k->reg);
            uint32_t maddr;
            switch (size)
            {
            case 7u: maddr = ((n & 0x7Fu) << 16) | a; break;
            case 6u: maddr = (n << 15) | (a & 0x7FFFu); break;
            case 5u: maddr = (n << 14) | (a & 0x3FFFu); break;
            default: maddr = (n << 13) | (a & 0x1FFFu); break;
            }
            maddr &= 0x7FFFFFu;
            r->addr = ((((uint32_t)c->addrfr + (maddr >> 16)) & 0x7Fu) << 16) | (maddr & 0xFFFFu);
            have_addr = true;
        }
    }
    if (r->flash_cs) r->ram_cs = false;
}

// ---------------------------------------------------------------------
// Chip access helpers
// ---------------------------------------------------------------------
static C2_FUNC inline bool chip_read(const c2_t *c, bool flash, uint32_t addr, uint8_t *data)
{
    if (flash)
    {
        if (!hw_has(HW->flash_read)) return false;
        *data = HW->flash_read(HW->ctx, addr & (C2_FLASH_SIZE - 1u));
    }
    else
    {
        if (!hw_has(HW->ram_read)) return false;
        *data = HW->ram_read(HW->ctx, addr & C2_RAM_MASK);
    }
    return true;
}

static C2_FUNC inline void chip_write(const c2_t *c, bool flash, uint32_t addr, uint8_t data)
{
    if (flash)
    {
        if (hw_has(HW->flash_write)) HW->flash_write(HW->ctx, addr & (C2_FLASH_SIZE - 1u), data);
    }
    else if (hw_has(HW->ram_write))
    {
        HW->ram_write(HW->ctx, addr & C2_RAM_MASK, data);
    }
}

// ---------------------------------------------------------------------
// Register window (DecMDR)
// ---------------------------------------------------------------------
static C2_FUNC inline bool dec_mdr(const c2_t *c, uint16_t a)
{
    return (c->cardmdr & C2_MDR_DISABLE) == 0u &&
           (a & 0x3FC0u) == 0x0F80u &&
           (uint8_t)(a >> 14) == ((c->cardmdr >> 5) & 3u);
}

static C2_FUNC inline uint32_t direct_addr(const c2_t *c)
{
    return ((uint32_t)(c->addrm2 & 0x7Fu) << 16) | ((uint32_t)c->addrm1 << 8) | c->addrm0;
}

static C2_FUNC bool mdr_read(const c2_t *c, uint8_t idx, uint8_t *v)
{
    if (idx >= 0x06u && idx <= 0x1Du)
    {
        const c2_bank_t *b = &c->a_bank[(idx - 6u) / 6u];
        switch ((idx - 6u) % 6u)
        {
        case 0: *v = b->mask; break;
        case 1: *v = b->addr; break;
        case 2: *v = b->reg; break;
        case 3: *v = b->mult; break;
        case 4: *v = b->maskr; break;
        default: *v = b->adrd; break;
        }
        return true;
    }

    switch (idx)
    {
    case 0x00: case 0x1F: *v = c->cardmdr; return true;
    case 0x01: *v = c->addrm0; return true;
    case 0x02: *v = c->addrm1; return true;
    case 0x03: *v = (uint8_t)(c->addrm2 & 0x7Fu); return true;
    case 0x05: *v = (uint8_t)(c->a_addrfr & 0x7Fu); return true;
    case 0x1E: *v = c->a_mconf; return true;
    case 0x20: *v = (uint8_t)(c->conffl & 7u); return true;
    case 0x21: *v = c->a_nsreg; return true;
    case 0x22: *v = c->lvl; return true;
    case 0x23:
        *v = (uint8_t)((c->eecs << 3) | (c->eeck << 2) | (c->eedi << 1) |
                       (hw_has(HW->eeprom_do) ? (HW->eeprom_do(HW->ctx) & 1u) : 1u));
        return true;
    case 0x24: *v = c->lvl1; return true;
    case 0x28: *v = c->a_slm_cfg; return true;
    case 0x29: *v = c->a_scart_cfg; return true;
    case 0x2A: *v = c->a_scart_slt; return true;
    case 0x2B: *v = c->a_scart_stbl; return true;
    case 0x2C: *v = 0x32u; return true;
    case 0x2D: *v = 0x35u; return true;
    case 0x2E: *v = 0x30u; return true;
    case 0x2F: *v = (uint8_t)(c->scrt_mrr & 7u); return true;
    case 0x30: *v = (uint8_t)(c->psgalt & 3u); return true;
    case 0x31: *v = c->slt_save[1]; return true;
    case 0x32: *v = c->slt_save[2]; return true;
    case 0x33: *v = c->slt_save[3]; return true;
    case 0x34: *v = c->a8; return true;
    case 0x35: *v = (uint8_t)(0xF0u | (c->pfxn & 3u)); return true;
    default: return false;
    }
}

static C2_FUNC void mdr_write(c2_t *c, uint8_t idx, uint8_t d)
{
    c->fast_dirty = true;
    if (idx >= 0x06u && idx <= 0x1Du)
    {
        c2_bank_t *b = &c->a_bank[(idx - 6u) / 6u];
        switch ((idx - 6u) % 6u)
        {
        case 0: b->mask = d; break;
        case 1: b->addr = d; break;
        case 2: b->reg = d; break;
        case 3: b->mult = d; break;
        case 4: b->maskr = d; break;
        default: b->adrd = d; break;
        }
        c2_shadow_written(c);
        return;
    }

    switch (idx)
    {
    case 0x00: case 0x1F:
        c->cardmdr = d;
        if ((d & C2_MDR_DELAYED) == 0u) c2_apply(c);
        break;
    case 0x01: c->addrm0 = d; break;
    case 0x02: c->addrm1 = d; break;
    case 0x03: c->addrm2 = (uint8_t)(d & 0x7Fu); break;
    case 0x05: c->a_addrfr = (uint8_t)(d & 0x7Fu); c2_shadow_written(c); break;
    case 0x1E:
        if ((d & 0x80u) || (d & 0x0Fu) != 0x0Fu) { c->a_mconf = d; c2_shadow_written(c); }
        break;
    case 0x20:
        c->conffl = (uint8_t)(d & 7u);
        if (hw_has(HW->flash_ctrl)) HW->flash_ctrl(HW->ctx, (c->conffl & 2u) != 0u, (c->conffl & 1u) != 0u);
        break;
    case 0x21: c->a_nsreg = d; c2_shadow_written(c); break;
    case 0x22:
        c->lvl = d;
        if (hw_has(HW->levels)) HW->levels(HW->ctx, c->lvl, c->lvl1);
        break;
    case 0x23:
        c->eecs = (d >> 3) & 1u; c->eeck = (d >> 2) & 1u; c->eedi = (d >> 1) & 1u;
        if (hw_has(HW->eeprom_pins)) HW->eeprom_pins(HW->ctx, c->eecs, c->eeck, c->eedi);
        break;
    case 0x24:
        c->lvl1 = d;
        if (hw_has(HW->levels)) HW->levels(HW->ctx, c->lvl, c->lvl1);
        break;
    case 0x28: c->a_slm_cfg = d; c2_shadow_written(c); break;
    case 0x29: c->a_scart_cfg = d; c2_shadow_written(c); break;
    case 0x2A: c->a_scart_slt = d; c2_shadow_written(c); break;
    case 0x2B: c->a_scart_stbl = d; c2_shadow_written(c); break;
    case 0x2F: c->scrt_mrr = (uint8_t)(d & 7u); break;
    case 0x30: c->psgalt = (uint8_t)(d & 3u); break;
    case 0x35: c->pfxn = (uint8_t)(d & 3u); break;
    default: break;
    }
}

// ---------------------------------------------------------------------
// SCC / SCC+ decode (DecSccA / DecSccB)
// ---------------------------------------------------------------------
static C2_FUNC inline bool dec_scc_a(const c2_t *c, uint16_t a)
{
    return (a >> 11) == 0x13u && (c->scc_mode_b & 0x20u) == 0u && (c->scc_bank2 & 0x3Fu) == 0x3Fu;
}

static C2_FUNC inline bool dec_scc_b(const c2_t *c, uint16_t a)
{
    return (a >> 11) == 0x17u && (c->scc_mode_b & 0x20u) != 0u && (c->scc_bank3 & 0x80u) != 0u;
}

static C2_FUNC bool scc_wave_oe(const c2_t *c, uint16_t a)
{
    if ((c->cardmdr & C2_MDR_SCC_ENABLE) == 0u || (c->scc_mode_b & 0x10u)) return false;
    if (dec_scc_a(c, a)) return (a & 0x80u) == 0u;
    if (dec_scc_b(c, a)) return (a & 0x80u) == 0u || ((a >> 5) & 3u) == 0u;
    return false;
}

static C2_FUNC void scc_write(c2_t *c, uint16_t a, uint8_t d)
{
    uint8_t ma = c->scc_mode_a, mb = c->scc_mode_b;
    bool dec1ffe = (a & 0x1FFEu) == 0x1FFEu;

    if ((c->cardmdr & C2_MDR_SCC_ENABLE) == 0u) return;

    // Bank / mode registers
    if ((a >> 11) == 0x12u && (mb & 0x10u) == 0u) { c->scc_bank2 = d; c->fast_dirty = true; }
    if ((a >> 11) == 0x16u && (ma & 0x40u) == 0u && (ma & 0x10u) == 0u && (mb & 0x10u) == 0u) { c->scc_bank3 = d; c->fast_dirty = true; }
    if ((a >> 13) == 3u && dec1ffe && (mb & 0x30u) == 0u) { c->scc_mode_a = d; c->fast_dirty = true; }
    if ((a >> 13) == 5u && dec1ffe && (ma & 0x40u) == 0u && (ma & 0x10u) == 0u)
    {
        c->scc_mode_b = d;
        c->fast_dirty = true;
        if (hw_has(HW->scc_plus) && ((d ^ mb) & 0x20u)) HW->scc_plus(HW->ctx, (d & 0x20u) != 0u);
    }

    if (mb & 0x10u) return;

    bool da = dec_scc_a(c, a), db = dec_scc_b(c, a);
    if (!da && !db) return;

    if ((da && (a & 0x80u) == 0u) || db)
    {
        if (hw_has(HW->scc_wave_write)) HW->scc_wave_write(HW->ctx, (uint8_t)a, d);
        if (da && ((a >> 5) & 7u) == 3u && hw_has(HW->scc_wave_write))
            HW->scc_wave_write(HW->ctx, (uint8_t)(0x80u | (a & 0x1Fu)), d);
    }
    if ((da && ((a >> 5) & 7u) == 4u) || (db && ((a >> 5) & 7u) == 5u))
    {
        if (hw_has(HW->scc_reg_write)) HW->scc_reg_write(HW->ctx, (uint8_t)(a & 0x0Fu), d);
    }
    if (((a >> 6) & 3u) == 3u && (da || (db && (a & 0x20u) == 0u)))
    {
        if (hw_has(HW->scc_mode_write)) HW->scc_mode_write(HW->ctx, d);
    }
}

// ---------------------------------------------------------------------
// IDE (Sltsl_D)
// ---------------------------------------------------------------------
static C2_FUNC inline bool ide_reg_window(const c2_t *c, uint16_t a)
{
    return (c->creg & 1u) && (a >> 10) == 0x1Fu && ((a >> 8) & 3u) != 3u;
}

static C2_FUNC inline uint32_t ide_rom_addr(const c2_t *c, uint16_t a)
{
    uint32_t seg = ((uint32_t)((c->creg >> 5) & 1u) << 2) | ((uint32_t)((c->creg >> 6) & 1u) << 1) |
                   (uint32_t)((c->creg >> 7) & 1u);
    return C2_FLASH_IDE_ROM + (seg << 14) + (a & 0x3FFFu);
}

// ---------------------------------------------------------------------
// FM-PAC slot (Sltsl_F)
// ---------------------------------------------------------------------
static C2_FUNC inline bool fm_ram8k(const c2_t *c) { return c->fm_5ffe == 0x4Du && c->fm_5fff == 0x69u; }

static C2_FUNC inline uint32_t fm_rom_addr(const c2_t *c, uint16_t a)
{
    return C2_FLASH_FM_ROM + ((uint32_t)(c->fm_7ff7 & 3u) << 14) + (a & 0x3FFFu);
}

// ---------------------------------------------------------------------
// Memory mapper (Sltsl_M)
// ---------------------------------------------------------------------
static C2_FUNC inline bool dec_p(const c2_t *c, uint16_t a)
{
    uint8_t page = (uint8_t)(a >> 14);
    return (c->port3c & 0x80u) &&
           ((page == 2u && (c->port3c & 0x08u) == 0u) || (page == 1u && (c->port3c & 0x08u)));
}

static C2_FUNC inline uint32_t mapper_addr(const c2_t *c, uint16_t a)
{
    return C2_RAM_MAPPER | ((uint32_t)(c->map[a >> 14] & 0x3Fu) << 14) | (a & 0x3FFFu);
}

// ---------------------------------------------------------------------
// Memory read
// ---------------------------------------------------------------------
C2_FUNC bool c2_mem_read(c2_t *c, uint16_t a, uint8_t *data)
{
    uint8_t dev = c2_devices(c, a);
    uint8_t v;

    c->last_page = (uint8_t)(a >> 14);

    if (a == 0xFFFFu && (c->mconf & 0x80u))
    {
        *data = (uint8_t)~c->exp_slt;
        return true;
    }

    if (dev & C2_DEV_MAIN)
    {
        bool decmdr = dec_mdr(c, a);

        if (decmdr && (c->cardmdr & C2_MDR_NO_READBACK) == 0u)
        {
            uint8_t idx = (uint8_t)(a & 0x3Fu);
            if (idx == 4u) return chip_read(c, true, direct_addr(c), data);
            if (mdr_read(c, idx, &v)) { *data = v; return true; }
        }

        if (scc_wave_oe(c, a) && hw_has(HW->scc_wave_read))
        {
            *data = HW->scc_wave_read(HW->ctx, (uint8_t)a);
            return true;
        }

        c2_bank_res_t r;
        bool ok = false;
        c2_bank_decode(c, a, &r);
        if (r.flash_cs || r.ram_cs) ok = chip_read(c, r.flash_cs, r.addr, data);

        if ((c->cardmdr & C2_MDR_DELAYED) && (c->cardmdr & C2_MDR_RELOAD_RD) && (a & 0xFFF0u) == 0x4000u)
            c2_apply(c);
        return ok;
    }

    if (dev & C2_DEV_IDE)
    {
        if (ide_reg_window(c, a))
        {
            if (hw_has(HW->ide_read)) return HW->ide_read(HW->ctx, a, data);
            return false;
        }
        if ((a >> 14) == 1u)
            return chip_read(c, (c->cardmdr & C2_MDR_SHADOW) == 0u, ide_rom_addr(c, a), data);
        return false;
    }

    if (dev & C2_DEV_MAPPER)
    {
        if (dec_p(c, a))
        {
            uint8_t low = (uint8_t)a;
            if (low >= 0xFCu) { *data = (uint8_t)(0xC0u | (c->map[low & 3u] & 0x3Fu)); return true; }
            if (low == 0x3Cu) { *data = c->port3c; return true; }
        }
        return chip_read(c, false, mapper_addr(c, a), data);
    }

    if (dev & C2_DEV_FM)
    {
        bool ram8k = fm_ram8k(c);
        bool shadow = (c->cardmdr & C2_MDR_SHADOW) != 0u;

        if (a == 0x7FF6u) { *data = (uint8_t)((c->fm_7ff6_b4 << 4) | c->fm_7ff6_b0); return true; }
        if (a == 0x7FF7u) { *data = (uint8_t)(c->fm_7ff7 & 3u); return true; }
        if (ram8k && a == 0x5FFEu) { *data = c->fm_5ffe; return true; }
        if (ram8k && a == 0x5FFFu) { *data = c->fm_5fff; return true; }

        if ((a >> 14) != 1u) return false;
        if (!ram8k) return chip_read(c, !shadow, fm_rom_addr(c, a), data);
        if ((a >> 13) == 2u || shadow)
            return chip_read(c, false, C2_RAM_FM_SRAM | (a & 0x1FFFu), data);
        return false;
    }

    return false;
}

// ---------------------------------------------------------------------
// Memory write
// ---------------------------------------------------------------------
C2_FUNC void c2_mem_write(c2_t *c, uint16_t a, uint8_t d)
{
    c->last_page = (uint8_t)(a >> 14);

    if (a == 0xFFFFu && (c->mconf & 0x80u))
    {
        c->exp_slt = d;
        c->fast_dirty = true;
        c->slt_save[c->a8 >> 6] = d;
    }

    uint8_t dev = c2_devices(c, a);

    if (dev & C2_DEV_MAIN)
    {
        if (dec_mdr(c, a))
        {
            uint8_t idx = (uint8_t)(a & 0x3Fu);
            if (idx == 4u) chip_write(c, true, direct_addr(c), d);
            mdr_write(c, idx, d);
        }
        else
        {
            // Memory cycle on the chip bus uses the configuration valid at the
            // start of the cycle, the register latches update afterwards.
            c2_bank_res_t r;
            c2_bank_decode(c, a, &r);
            if (r.write_en && (r.flash_cs || r.ram_cs)) chip_write(c, r.flash_cs, r.addr, d);

            bool nsccp = nsc_sccp(c);
            uint8_t ma = c->scc_mode_a, mb = c->scc_mode_b;
            for (uint8_t b = 0u; b < 4u; ++b)
            {
                c2_bank_t *k = &c->bank[b];
                if ((k->mult & 0x80u) == 0u) continue;

                bool match = true;
                for (uint8_t bit = 0u; bit < 8u; ++bit)
                {
                    if ((k->mask >> bit) & 1u)
                        match &= (((a >> (8u + bit)) & 1u) == ((k->addr >> bit) & 1u));
                }
                if (!match) continue;

                if (nsccp)
                {
                    bool ok;
                    switch (b)
                    {
                    case 0:  ok = !(ma & 0x40u) && !(ma & 0x10u) && !(mb & 0x10u) && !(mb & 0x01u); break;
                    case 1:  ok = !(ma & 0x40u) && !(ma & 0x10u) && !(mb & 0x10u) && !(mb & 0x02u); break;
                    case 2:  ok = !(mb & 0x10u) && (!(mb & 0x04u) || !(mb & 0x20u)); break;
                    default: ok = !(ma & 0x40u) && !(ma & 0x10u) && !(mb & 0x10u); break;
                    }
                    if (!ok) continue;
                }
                if (k->reg != d) c->fast_dirty = true;
                k->reg = d;
                c->a_bank[b].reg = d;
            }
            scc_write(c, a, d);
        }
        return;
    }

    if (dev & C2_DEV_IDE)
    {
        if (a == 0x4104u) { c->creg = d; c->fast_dirty = true; }
        if (hw_has(HW->ide_write) && (a == 0x4104u || ide_reg_window(c, a))) HW->ide_write(HW->ctx, a, d);
        return;
    }

    if (dev & C2_DEV_MAPPER)
    {
        if (dec_p(c, a))
        {
            c->fast_dirty = true;
            switch ((uint8_t)a)
            {
            case 0x3Cu:
                c->port3c = (uint8_t)((d & 0x7Fu) | (((d & 0xA0u) != 0u) ? 0x80u : 0u));
                return;
            case 0xFCu: c->map[0] = (uint8_t)(d & 0x7Fu); return;
            case 0xFDu: c->map[0] = (uint8_t)(d & 0x7Fu); return;   // VHDL quirk: FD lands in MAP_FC
            case 0xFEu: c->map[0] = (uint8_t)(d & 0x7Fu); return;   // VHDL quirk: FE lands in MAP_FC
            case 0xFFu: c->map[3] = (uint8_t)(d & 0x7Fu); return;
            default: break;
            }
        }
        if ((c->port3c & (1u << (a >> 14))) == 0u) chip_write(c, false, mapper_addr(c, a), d);
        return;
    }

    if (dev & C2_DEV_FM)
    {
        bool shadow = (c->cardmdr & C2_MDR_SHADOW) != 0u;
        bool ram8k = fm_ram8k(c);

        if (ram8k && ((a >> 13) == 2u || (shadow && (a >> 14) == 1u)))
            chip_write(c, false, C2_RAM_FM_SRAM | (a & 0x1FFFu), d);

        if (a == 0x7FF6u) { c->fm_7ff6_b0 = d & 1u; c->fm_7ff6_b4 = (d >> 4) & 1u; }
        else if (a == 0x7FF7u) { c->fm_7ff7 = d & 3u; c->fast_dirty = true; }
        else if (a == 0x5FFEu) { c->fm_5ffe = d; c->fast_dirty = true; }
        else if (a == 0x5FFFu) { c->fm_5fff = d; c->fast_dirty = true; }
        else if ((a & 0xFFFEu) == 0x7FF4u && c->fm_7ff6_b0)
        {
            if (hw_has(HW->opll_write)) HW->opll_write(HW->ctx, (uint8_t)(a & 1u), d);
        }
    }
}

// ---------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------
C2_FUNC bool c2_io_read(c2_t *c, uint8_t port, uint8_t *data)
{
    if (port >= 0xFCu && (c->port3c & 0x20u) == 0u && (c->mconf & 0x40u) && c->mapper_port_read)
    {
        *data = (uint8_t)(0xC0u | (c->map[port & 3u] & 0x3Fu));
        return true;
    }

    if (port == (uint8_t)(0xF0u | (c->pfxn & 3u)) && c->pf0_rv != 0u)
    {
        *data = (c->pf0_rv == 1u) ? 0x32u : (uint8_t)(0x30u | ((c->a8 >> (c->last_page * 2u)) & 3u));
        return true;
    }
    return false;
}

C2_FUNC void c2_io_write(c2_t *c, uint8_t port, uint8_t d)
{
    if (port == (uint8_t)(0xF0u | (c->pfxn & 3u)))
    {
        c->fast_dirty = true;
        switch (d)
        {
        case 'C': c->pf0_rv = 1u; break;
        case 'S': c->pf0_rv = 2u; break;
        case 'R': c->cardmdr &= (uint8_t)~C2_MDR_DISABLE; break;
        case 'H': c->cardmdr |= C2_MDR_DISABLE; break;
        case '0': c->cardmdr &= (uint8_t)~C2_MDR_BASE_MASK; break;
        case '1': c->cardmdr = (uint8_t)((c->cardmdr & ~C2_MDR_BASE_MASK) | 0x20u); break;
        case '2': c->cardmdr = (uint8_t)((c->cardmdr & ~C2_MDR_BASE_MASK) | 0x40u); break;
        case '3': c->cardmdr |= C2_MDR_BASE_MASK; break;
        case 'A': c->mconf = (uint8_t)((c->mconf & 0x70u) | 0x01u); break;
        case 'M': c->mconf = (uint8_t)(0x80u | (c->mconf & 0x70u) | 0x0Fu); break;
        default: c->pf0_rv = 0u; break;
        }
    }

    if (port == 0xA8u) c->a8 = d;

    {
        uint8_t psg_base = (c->psgalt & 1u) ? 0x10u : 0xA0u;
        if ((port == psg_base || port == (uint8_t)(psg_base + 1u)) && hw_has(HW->psg_write))
            HW->psg_write(HW->ctx, (uint8_t)(port & 1u), d);
    }

    if (port == 0xAAu || port == 0xABu)
    {
        uint8_t kc = c->kc;
        if (port == 0xAAu) kc = d >> 7;
        else if (((d >> 1) & 7u) == 7u && (d & 0x80u) == 0u) kc = d & 1u;
        if (kc != c->kc)
        {
            c->kc = kc;
            if (hw_has(HW->beeper)) HW->beeper(HW->ctx, kc);
        }
    }

    if ((port & 0xFEu) == 0x7Cu && (c->fm_7ff6_b0 || (c->mconf & 0x20u)) && hw_has(HW->opll_write))
        HW->opll_write(HW->ctx, (uint8_t)(port & 1u), d);

    if ((c->port3c & 0x20u) == 0u)
    {
        if (port == 0x3Cu || port >= 0xFCu) c->fast_dirty = true;
        if (port == 0x3Cu && (c->mconf & 0x10u)) c->port3c = (uint8_t)((c->port3c & 0x7Fu) | (d & 0x80u));
        if (port >= 0xFCu) c->map[port & 3u] = (uint8_t)(d & 0x7Fu);
    }
}

// ---------------------------------------------------------------------
// Per-page read decode cache
// ---------------------------------------------------------------------
// Every decode rule is constant over an aligned 8 KB page except for a few
// special addresses (register window, SCC windows, 0xFFFF, FM-PAC and IDE
// registers). Pages that contain one of those stay on the slow path; all other
// pages reduce to "read the FlashROM / RAM chip at base + offset".
C2_FUNC void c2_fast_rebuild(c2_t *c)
{
    for (uint8_t p = 0u; p < 8u; ++p)
    {
        uint16_t a = (uint16_t)(p << 13);
        uint8_t dev = c2_devices(c, a);
        c2_fast_t *f = &c->fast[p];

        f->kind = C2_FAST_SLOW;
        f->base = 0u;

        if (p == 7u && (c->mconf & 0x80u)) continue;            // 0xFFFF is the sub-slot register
        if (dev == C2_DEV_NONE) { f->kind = C2_FAST_OPEN; continue; }
        if (dev & (dev - 1u)) continue;                          // overlapping devices

        switch (dev)
        {
        case C2_DEV_MAIN:
        {
            c2_bank_res_t r;

            if ((c->cardmdr & C2_MDR_DISABLE) == 0u && p == (uint8_t)(((c->cardmdr >> 5) & 3u) << 1)) break;
            if ((c->cardmdr & C2_MDR_SCC_ENABLE) != 0u &&
                ((p == 4u && dec_scc_a(c, 0x9800u)) || (p == 5u && dec_scc_b(c, 0xB800u)))) break;
            if ((c->cardmdr & C2_MDR_DELAYED) && (c->cardmdr & C2_MDR_RELOAD_RD) && p == 2u) break;

            c2_bank_decode(c, a, &r);
            if (r.flash_cs) { f->kind = C2_FAST_FLASH; f->base = r.addr & (C2_FLASH_SIZE - 1u); }
            else if (r.ram_cs) { f->kind = C2_FAST_RAM; f->base = r.addr & C2_RAM_MASK; }
            else f->kind = C2_FAST_OPEN;
            break;
        }
        case C2_DEV_IDE:
            if (p < 2u || p > 3u) { f->kind = C2_FAST_OPEN; break; }
            if (p == 3u && (c->creg & 1u)) break;                // IDE registers
            if (c->cardmdr & C2_MDR_SHADOW) { f->kind = C2_FAST_RAM; f->base = ide_rom_addr(c, a) & C2_RAM_MASK; }
            else { f->kind = C2_FAST_FLASH; f->base = ide_rom_addr(c, a); }
            break;
        case C2_DEV_MAPPER:
            if (c->port3c & 0x80u) break;                        // memory mapped mapper registers
            f->kind = C2_FAST_RAM;
            f->base = mapper_addr(c, a) & C2_RAM_MASK;
            break;
        default:    // C2_DEV_FM
            if (p < 2u || p > 3u) { f->kind = C2_FAST_OPEN; break; }
            if (p == 3u || fm_ram8k(c)) break;                   // 0x7FF6/0x7FF7 and the SRAM keys
            if (c->cardmdr & C2_MDR_SHADOW) { f->kind = C2_FAST_RAM; f->base = fm_rom_addr(c, a) & C2_RAM_MASK; }
            else { f->kind = C2_FAST_FLASH; f->base = fm_rom_addr(c, a); }
            break;
        }
        if (f->kind == C2_FAST_FLASH && !c->flash_array) f->kind = C2_FAST_SLOW;
    }
    c->fast_dirty = false;
}