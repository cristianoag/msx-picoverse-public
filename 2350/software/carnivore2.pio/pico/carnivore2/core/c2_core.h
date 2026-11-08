// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_core.h - Carnivore2 (RBSC) cartridge model for PicoVerse 2350.
//
// This module is a software model of the Carnivore2 FPGA design (mcscc.vhd,
// firmware 2.50): expanded slot logic, configuration registers, the four
// programmable bank windows, 1 MB memory mapper, IDE (Sunrise) window, FM-PAC
// slot, SCC/SCC+ decoder, PSG/beeper ports, EEPROM port and the port #F0
// detection protocol. It is hardware independent: the flash chip, the RAM
// chip, the IDE controller and the sound chips are reached through the
// c2_hw_t callbacks so the same code runs on the RP2350 and in the host tests.
//
// Only /SLTSL based decoding is modelled. The PicoVerse cartridge edge has no
// /MERQ, /M1 or /RFSH lines, so the "slave slot" modes of the original design
// (SCART_cfg registers) are stored and read back but do not change decoding.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef C2_CORE_H
#define C2_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "c2_config.h"

#define C2_FLASH_SIZE   0x800000u     // 8 MB flash chip (M29W640GB)
#define C2_RAM_SIZE     0x200000u     // 2 MB RAM chip: 1 MB MegaRAM/shadow + 1 MB mapper
#define C2_RAM_MASK     (C2_RAM_SIZE - 1u)

// Flash chip layout used by the original design (see mcscc.vhd pFlAdr)
#define C2_FLASH_IDE_ROM    0x010000u // 128 KB Nextor/IDE BIOS (segments of 16 KB)
#define C2_FLASH_FM_ROM     0x030000u // 64 KB FM-PAC BIOS (4 pages of 16 KB)
#define C2_RAM_FM_SRAM      0x0FE000u // 8 KB FM-PAC SRAM inside the RAM chip
#define C2_RAM_MAPPER       0x100000u // 1 MB memory mapper inside the RAM chip

// CardMDR bits
#define C2_MDR_DISABLE      0x80u
#define C2_MDR_BASE_MASK    0x60u
#define C2_MDR_SCC_ENABLE   0x10u
#define C2_MDR_DELAYED      0x08u
#define C2_MDR_RELOAD_RD    0x04u
#define C2_MDR_SHADOW       0x02u
#define C2_MDR_NO_READBACK  0x01u

// Bank configuration indexes inside c2_bank_t.r[]
enum { C2_B_MASK = 0, C2_B_ADDR, C2_B_REG, C2_B_MULT, C2_B_MASKR, C2_B_ADRD };

typedef struct {
    uint8_t mask;   // RxMask  - which of A15..A8 take part in the bank register match
    uint8_t addr;   // RxAddr  - value of A15..A8 selecting the bank register
    uint8_t reg;    // RxReg   - current bank register value
    uint8_t mult;   // RxMult  - [7]=reg write enable [6]=A15 ignore (8K) [5]=RAM [4]=write enable [3]=disable [2:0]=size
    uint8_t maskr;  // BxMaskR - mask applied to RxReg
    uint8_t adrd;   // BxAdrD  - address of the data window
} c2_bank_t;

// Read decode of one 8 KB page of the slot, valid until the configuration changes.
//   C2_FAST_SLOW  - needs the full decode (c2_mem_read)
//   C2_FAST_OPEN  - the cartridge does not drive the bus
//   C2_FAST_FLASH - plain FlashROM array read at base + (addr & 0x1FFF)
//   C2_FAST_RAM   - plain RAM chip read at base + (addr & 0x1FFF)
enum { C2_FAST_SLOW = 0, C2_FAST_OPEN, C2_FAST_FLASH, C2_FAST_RAM };

typedef struct {
    uint32_t base;
    uint8_t kind;
} c2_fast_t;

// Hardware callbacks. Every pointer may be NULL in tests that do not use it.
typedef struct c2_hw {
    void *ctx;
    uint8_t (*flash_read)(void *ctx, uint32_t addr);               // 23 bit address
    void    (*flash_write)(void *ctx, uint32_t addr, uint8_t data); // bus write cycle into the flash chip
    uint8_t (*ram_read)(void *ctx, uint32_t addr);                 // 21 bit address
    void    (*ram_write)(void *ctx, uint32_t addr, uint8_t data);
    bool    (*ide_read)(void *ctx, uint16_t addr, uint8_t *data);  // true when an IDE register answered
    void    (*ide_write)(void *ctx, uint16_t addr, uint8_t data);
    void    (*flash_ctrl)(void *ctx, bool reset_n, bool vpp_wp);   // ConfFl[1] / ConfFl[0]
    uint8_t (*scc_wave_read)(void *ctx, uint8_t offset);
    void    (*scc_wave_write)(void *ctx, uint8_t offset, uint8_t data);
    void    (*scc_reg_write)(void *ctx, uint8_t reg, uint8_t data); // reg = A3..A0 of the register block
    void    (*scc_mode_write)(void *ctx, uint8_t data);             // deformation register
    void    (*scc_plus)(void *ctx, bool enhanced);
    void    (*opll_write)(void *ctx, uint8_t a0, uint8_t data);
    void    (*psg_write)(void *ctx, uint8_t a0, uint8_t data);
    void    (*beeper)(void *ctx, uint8_t level);
    void    (*levels)(void *ctx, uint8_t lvl, uint8_t lvl1);
    void    (*eeprom_pins)(void *ctx, uint8_t cs, uint8_t ck, uint8_t di);
    uint8_t (*eeprom_do)(void *ctx);
} c2_hw_t;

typedef struct c2 {
    const c2_hw_t *hw;

    // Control registers (active copy)
    uint8_t cardmdr, addrm0, addrm1, addrm2, addrfr;
    uint8_t mconf, nsreg, slm_cfg, scart_cfg, scart_slt, scart_stbl;
    c2_bank_t bank[4];

    // Shadow copy written by the MSX, loaded into the active copy by the
    // "delayed reconfiguration" trigger (or immediately when CardMDR[3]=0)
    uint8_t a_addrfr, a_mconf, a_nsreg, a_slm_cfg, a_scart_cfg, a_scart_slt, a_scart_stbl;
    c2_bank_t a_bank[4];

    uint8_t conffl, lvl, lvl1, scrt_mrr, psgalt, pfxn;
    uint8_t eecs, eeck, eedi;

    // Port #F0 detection protocol
    uint8_t pf0_rv;
    uint8_t last_page;          // 16 KB page of the last cartridge access (slot reported by port #F0 S)
    uint8_t a8;                 // last value written to the primary slot register (port A8)
    uint8_t slt_save[4];        // 0xFFFF values written while each primary slot was selected

    // Expanded slot, mapper and the other slot-resident devices
    uint8_t exp_slt;
    uint8_t map[4];             // FC..FF (7 bits)
    uint8_t port3c;
    uint8_t creg;               // Sunrise IDE control register (0x4104)
    uint8_t fm_7ff6_b0, fm_7ff6_b4, fm_7ff7, fm_5ffe, fm_5fff;
    uint8_t scc_bank2, scc_bank3, scc_mode_a, scc_mode_b;
    uint8_t kc;
    bool    mapper_port_read;   // MAPpEn jumper

    // Set when the active configuration must be reloaded by a bus event
    bool    reload_pending;

    // Per 8 KB page read decode cache (see c2_fast_get)
    c2_fast_t fast[8];
    bool    fast_dirty;
    bool    flash_array;        // FlashROM chip is in array read mode (fast path allowed)
} c2_t;

// Power-on / hardware reset state.
C2_FUNC void c2_init(c2_t *c, const c2_hw_t *hw);

// Memory cycle in the cartridge slot (/SLTSL active). The read returns true
// when the cartridge drives the data bus.
C2_FUNC bool c2_mem_read(c2_t *c, uint16_t addr, uint8_t *data);
C2_FUNC void c2_mem_write(c2_t *c, uint16_t addr, uint8_t data);

// I/O cycles. The read returns true when the cartridge drives the data bus.
C2_FUNC bool c2_io_read(c2_t *c, uint8_t port, uint8_t *data);
C2_FUNC void c2_io_write(c2_t *c, uint8_t port, uint8_t data);

// Opcode fetch from address 0x0000 (reset / JP 0 / RST 0): loads the delayed
// configuration when CardMDR[3]=1 and CardMDR[2]=0.
C2_FUNC void c2_fetch_zero(c2_t *c);

// True while a delayed configuration is waiting for the address 0 trigger.
static inline bool c2_wants_fetch_zero(const c2_t *c)
{
    return c->reload_pending && (c->cardmdr & C2_MDR_RELOAD_RD) == 0u;
}

// Address of the register window (0x0F80/0x4F80/0x8F80/0xCF80) or 0 when disabled.
static inline uint16_t c2_regwin_base(const c2_t *c)
{
    if (c->cardmdr & C2_MDR_DISABLE) return 0u;
    return (uint16_t)((((c->cardmdr >> 5) & 3u) << 14) | 0x0F80u);
}

// Sub-slot device selects (Sltsl_C/D/M/F of the original design) for an address.
enum { C2_DEV_NONE = 0, C2_DEV_MAIN = 1, C2_DEV_IDE = 2, C2_DEV_MAPPER = 4, C2_DEV_FM = 8 };
C2_FUNC uint8_t c2_devices(const c2_t *c, uint16_t addr);

// The FlashROM chip leaves/enters array read mode (autoselect, CFI, reset): flash pages need the slow path meanwhile.
static inline void c2_set_flash_array(c2_t *c, bool array)
{
    if (c->flash_array != array) { c->flash_array = array; c->fast_dirty = true; }
}

// Rebuild the per-page read decode cache after a configuration change.
C2_FUNC void c2_fast_rebuild(c2_t *c);

// Fast read decode for the bus loop. Entries marked C2_FAST_SLOW must go
// through c2_mem_read. Writes and register changes mark the cache dirty.
static inline const c2_fast_t *c2_fast_get(c2_t *c, uint16_t addr)
{
    if (c->fast_dirty) c2_fast_rebuild(c);
    return &c->fast[addr >> 13];
}

#endif // C2_CORE_H
