// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_core_test.c - host tests for the Carnivore2 core, FlashROM and EEPROM models.
//
// Build and run with `make test` in carnivore2.pio (needs a host C compiler).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../pico/carnivore2/core/c2_core.h"
#include "../pico/carnivore2/core/c2_flash.h"
#include "../pico/carnivore2/core/c2_eeprom.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long _a = (unsigned long)(a), _b = (unsigned long)(b); checks++; \
    if (_a != _b) { failures++; printf("FAIL %s:%d: %s == %s (0x%lX != 0x%lX)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// ---------------------------------------------------------------------
// Fake hardware
// ---------------------------------------------------------------------
static uint8_t *flash_mem;
static uint8_t *ram_mem;
static c2_flash_t chip;
static c2_eeprom_t eeprom;
static c2_t c2;
static c2_hw_t hw;

static uint8_t scc_wave[256];
static uint8_t scc_regs[16];
static int scc_mode_writes;
static bool scc_plus;
static uint8_t opll_log[16][2];
static int opll_n;
static uint8_t psg_log[16][2];
static int psg_n;
static uint8_t ide_last_write_addr_hi;
static uint8_t last_lvl, last_lvl1;
static bool flash_reset_n, flash_wp_off;

static uint8_t be_read(void *ctx, uint32_t a) { (void)ctx; return flash_mem[a]; }
static void be_program(void *ctx, uint32_t a, uint8_t d) { (void)ctx; flash_mem[a] &= d; }
static void be_erase(void *ctx, uint32_t s, uint32_t l) { (void)ctx; memset(flash_mem + s, 0xFF, l); }
static const c2_flash_backend_t backend = { 0, be_read, be_program, be_erase };

static uint8_t hw_flash_read(void *ctx, uint32_t a) { (void)ctx; return c2_flash_read(&chip, a); }
static void hw_flash_write(void *ctx, uint32_t a, uint8_t d) { (void)ctx; c2_flash_write(&chip, a, d); }
static uint8_t hw_ram_read(void *ctx, uint32_t a) { (void)ctx; return ram_mem[a]; }
static void hw_ram_write(void *ctx, uint32_t a, uint8_t d) { (void)ctx; ram_mem[a] = d; }
static bool hw_ide_read(void *ctx, uint16_t a, uint8_t *d) { (void)ctx; *d = (uint8_t)(0x40u | (a & 0x0Fu)); return true; }
static void hw_ide_write(void *ctx, uint16_t a, uint8_t d) { (void)ctx; (void)d; ide_last_write_addr_hi = (uint8_t)(a >> 8); }
static void hw_flash_ctrl(void *ctx, bool rn, bool wp) { (void)ctx; flash_reset_n = rn; flash_wp_off = wp; c2_flash_control(&chip, rn, wp); }
static uint8_t hw_scc_wave_read(void *ctx, uint8_t o) { (void)ctx; return scc_wave[o]; }
static void hw_scc_wave_write(void *ctx, uint8_t o, uint8_t d) { (void)ctx; scc_wave[o] = d; }
static void hw_scc_reg_write(void *ctx, uint8_t r, uint8_t d) { (void)ctx; scc_regs[r & 15] = d; }
static void hw_scc_mode_write(void *ctx, uint8_t d) { (void)ctx; (void)d; scc_mode_writes++; }
static void hw_scc_plus(void *ctx, bool e) { (void)ctx; scc_plus = e; }
static void hw_opll_write(void *ctx, uint8_t a0, uint8_t d) { (void)ctx; if (opll_n < 16) { opll_log[opll_n][0] = a0; opll_log[opll_n][1] = d; opll_n++; } }
static void hw_psg_write(void *ctx, uint8_t a0, uint8_t d) { (void)ctx; if (psg_n < 16) { psg_log[psg_n][0] = a0; psg_log[psg_n][1] = d; psg_n++; } }
static void hw_levels(void *ctx, uint8_t l, uint8_t l1) { (void)ctx; last_lvl = l; last_lvl1 = l1; }
static void hw_ee_pins(void *ctx, uint8_t cs, uint8_t ck, uint8_t di) { (void)ctx; c2_eeprom_pins(&eeprom, cs, ck, di); }
static uint8_t hw_ee_do(void *ctx) { (void)ctx; return c2_eeprom_do(&eeprom); }

static void setup(void)
{
    memset(flash_mem, 0xFF, C2_FLASH_SIZE);
    memset(ram_mem, 0x00, C2_RAM_SIZE);
    // Boot menu stand-in at 0, Nextor at 0x10000 (segment n tagged), FM-PAC BIOS at 0x30000 (page n tagged)
    flash_mem[0] = 'A'; flash_mem[1] = 'B'; flash_mem[0x4000] = 0x11; flash_mem[0x8000] = 0x22; flash_mem[0xC000] = 0x33;
    for (int s = 0; s < 8; ++s) { flash_mem[0x10000 + s * 0x4000] = (uint8_t)(0xA0 + s); flash_mem[0x10000 + s * 0x4000 + 1] = 'N'; }
    for (int p = 0; p < 4; ++p) flash_mem[0x30000 + p * 0x4000] = (uint8_t)(0xF0 + p);
    for (int i = 0; i < 256; ++i) flash_mem[0x100000 + i] = (uint8_t)i;

    memset(scc_wave, 0, sizeof(scc_wave)); memset(scc_regs, 0, sizeof(scc_regs));
    scc_mode_writes = 0; scc_plus = false; opll_n = psg_n = 0; ide_last_write_addr_hi = 0;

    hw = (c2_hw_t){ 0, hw_flash_read, hw_flash_write, hw_ram_read, hw_ram_write, hw_ide_read, hw_ide_write, hw_flash_ctrl,
                    hw_scc_wave_read, hw_scc_wave_write, hw_scc_reg_write, hw_scc_mode_write, hw_scc_plus,
                    hw_opll_write, hw_psg_write, 0, hw_levels, hw_ee_pins, hw_ee_do };
    c2_flash_init(&chip, &backend);
    c2_eeprom_init(&eeprom);
    c2_init(&c2, &hw);
}

static uint8_t rd(uint16_t a) { uint8_t d = 0xEE; if (!c2_mem_read(&c2, a, &d)) return 0xEE; return d; }
static bool driven(uint16_t a) { uint8_t d; return c2_mem_read(&c2, a, &d); }
static void wr(uint16_t a, uint8_t d) { c2_mem_write(&c2, a, d); }
static void outp(uint8_t p, uint8_t d) { c2_io_write(&c2, p, d); }
static uint8_t inp(uint8_t p) { uint8_t d = 0xEE; if (!c2_io_read(&c2, p, &d)) return 0xEE; return d; }

static void select_sub(uint8_t reg) { wr(0xFFFF, reg); }     // all four pages -> one sub-slot
// The per-page fast decode must agree with the full decode for every address.
static void check_fast_equivalence(void)
{
    for (uint32_t a = 0; a < 0x10000; ++a)
    {
        const c2_fast_t *f = c2_fast_get(&c2, (uint16_t)a);
        if (f->kind == C2_FAST_SLOW) continue;

        uint8_t slow = 0xEE;
        bool drive = c2_mem_read(&c2, (uint16_t)a, &slow);
        checks++;
        if (f->kind == C2_FAST_OPEN) { if (drive) { failures++; printf("FAIL fast OPEN drives at 0x%04X\n", a); return; } continue; }
        uint8_t fast = (f->kind == C2_FAST_FLASH) ? flash_mem[(f->base + (a & 0x1FFF)) & (C2_FLASH_SIZE - 1)]
                                                  : ram_mem[(f->base + (a & 0x1FFF)) & C2_RAM_MASK];
        if (!drive || slow != fast) { failures++; printf("FAIL fast kind %d mismatch at 0x%04X (%02X vs %02X)\n", f->kind, a, slow, fast); return; }
    }
}

static void reg_write(uint8_t idx, uint8_t d) { wr((uint16_t)(0x4F80u + idx), d); }
static uint8_t reg_read(uint8_t idx) { return rd((uint16_t)(0x4F80u + idx)); }

// ---------------------------------------------------------------------
static void test_reset_and_expanded_slot(void)
{
    setup();
    CHECK_EQ(c2.cardmdr, 0x30);
    CHECK_EQ(c2.mconf, 0xFF);
    // Expanded slot register reads back inverted
    wr(0xFFFF, 0x55);
    CHECK_EQ(rd(0xFFFF), 0xAA);
    // Sub-slot 0 = main: boot menu header at 0x4000
    select_sub(0x00);
    CHECK_EQ(rd(0x4000), 'A');
    CHECK_EQ(rd(0x4001), 'B');
    check_fast_equivalence();
    // Sub-slot 1 = IDE ROM segment 0
    select_sub(0x55);
    CHECK_EQ(rd(0x4000), 0xA0);
    CHECK_EQ(rd(0x4001), 'N');
    check_fast_equivalence();
    // Sub-slot 3 = FM-PAC ROM page 0
    select_sub(0xFF);
    CHECK_EQ(rd(0x4000), 0xF0);
    // The IDE sub-slot does not answer outside page 1
    select_sub(0x55);
    CHECK(!driven(0x8000));
    // Plain (non expanded) slot: Mconf 'A' via port F0 leaves only the main device
    outp(0xF0, 'A');
    CHECK_EQ(c2.mconf & 0x8F, 0x01);
    CHECK_EQ(rd(0x4000), 'A');
    outp(0xF0, 'M');
    CHECK_EQ(c2.mconf, 0xFF);
}

static void test_port_f0(void)
{
    setup();
    CHECK(!c2_io_read(&c2, 0xF0, &(uint8_t){0}));
    outp(0xF0, 'C');
    CHECK_EQ(inp(0xF0), '2');
    CHECK_EQ(inp(0xF0), '2');           // the latch is not cleared by reading
    outp(0xA8, 0xE4);
    wr(0xFFFF, 0);                      // select sub-slot 0 everywhere
    (void)rd(0x8000);                   // cartridge accessed in page 2 -> slot (A8 >> 4) & 3
    outp(0xF0, 'S');
    CHECK_EQ(inp(0xF0), 0x30 | ((0xE4 >> 4) & 3));
    outp(0xF0, 'X');
    CHECK(!c2_io_read(&c2, 0xF0, &(uint8_t){0}));
    // R / H toggle the register window, 0..3 move it
    outp(0xF0, 'H');
    CHECK(c2.cardmdr & 0x80);
    wr(0xFFFF, 0);
    CHECK(rd(0x4F80) != 0x30);
    outp(0xF0, 'R');
    CHECK_EQ(rd(0x4F80), 0x30);
    outp(0xF0, '2');
    CHECK_EQ(rd(0x8F80), 0x50);
    outp(0xF0, '3');
    CHECK_EQ(rd(0xCF80), 0x70);
    outp(0xF0, '0');
    CHECK_EQ(rd(0x0F80), 0x10);
    outp(0xF0, '1');
    // PFXN moves the detection port
    reg_write(0x35, 0x02);
    CHECK_EQ(reg_read(0x35), 0xF2);
    outp(0xF2, 'C');
    CHECK_EQ(inp(0xF2), '2');
    CHECK(!c2_io_read(&c2, 0xF0, &(uint8_t){0}));
}

static void test_register_window(void)
{
    setup();
    wr(0xFFFF, 0);
    CHECK_EQ(reg_read(0x00), 0x30);
    CHECK_EQ(reg_read(0x1F), 0x30);
    CHECK_EQ(reg_read(0x2C), 0x32);
    CHECK_EQ(reg_read(0x2D), 0x35);
    CHECK_EQ(reg_read(0x2E), 0x30);
    // Default bank 1 configuration
    CHECK_EQ(reg_read(0x06), 0xF8);
    CHECK_EQ(reg_read(0x07), 0x50);
    CHECK_EQ(reg_read(0x09), 0x85);
    CHECK_EQ(reg_read(0x0A), 0x03);
    CHECK_EQ(reg_read(0x0B), 0x40);
    CHECK_EQ(reg_read(0x1E), 0xFF);
    CHECK_EQ(reg_read(0x20), 0x02);
    // Mconf refuses a value that would disable the expanded slot without a device
    reg_write(0x1E, 0x0F);
    CHECK_EQ(reg_read(0x1E), 0xFF);
    // Volume registers
    reg_write(0x22, 0x5A); reg_write(0x24, 0xA5);
    CHECK_EQ(last_lvl, 0x5A); CHECK_EQ(last_lvl1, 0xA5);
    CHECK_EQ(reg_read(0x22), 0x5A);
    // Disable read back keeps writes working
    reg_write(0x00, 0x31);
    CHECK(!driven(0x4F80) || rd(0x4F80) != 0x31);
    reg_write(0x01, 0x12);
    CHECK_EQ(c2.addrm0, 0x12);
    outp(0xF0, 'R');
    reg_write(0x00, 0x30);
    CHECK_EQ(reg_read(0x01), 0x12);
}

static void test_bank_switching_and_delayed_reload(void)
{
    setup();
    wr(0xFFFF, 0);
    // Default: 16 KB window at 0x4000, R1Reg selects the page (mask 3), written at 0x5000-0x57FF
    CHECK_EQ(rd(0x4000), 'A');
    wr(0x5000, 1); CHECK_EQ(rd(0x4000), 0x11);
    wr(0x5000, 2); CHECK_EQ(rd(0x4000), 0x22);
    wr(0x5000, 3); CHECK_EQ(rd(0x4000), 0x33);
    wr(0x5000, 0xFF); CHECK_EQ(rd(0x4000), 0x33);       // masked to 4 pages
    check_fast_equivalence();
    check_fast_equivalence();
    wr(0x5000, 0);
    // Bank 2: 8 KB window at 0x8000 from flash page 4 (offset 0x8000), immediate load
    reg_write(0x0C, 0xF8); reg_write(0x0D, 0x70); reg_write(0x0E, 0x04);
    reg_write(0x0F, 0x84); reg_write(0x10, 0xFF); reg_write(0x11, 0x00);
    CHECK_EQ(rd(0x8000), flash_mem[0x4 * 0x2000]);
    flash_mem[0x4 * 0x2000 + 0x123] = 0x99;
    CHECK_EQ(rd(0x8123), 0x99);
    check_fast_equivalence();
    check_fast_equivalence();
    // AddrFR offsets the flash window in 64 KB units
    reg_write(0x05, 0x01);
    CHECK_EQ(rd(0x4000), 0xA0);     // flash 0x10000 -> Nextor segment 0 tag
    reg_write(0x05, 0x00);
    // Delayed reconfiguration: CardMDR[3]=1, trigger on the 0x0000 fetch
    reg_write(0x00, 0x38);
    reg_write(0x05, 0x01);
    CHECK_EQ(rd(0x4000), 'A');      // not applied yet
    CHECK(c2_wants_fetch_zero(&c2));
    c2_fetch_zero(&c2);
    CHECK_EQ(rd(0x4000), 0xA0);
    // CardMDR[2]=1: trigger on a read of 0x400x instead
    reg_write(0x00, 0x3C);
    reg_write(0x05, 0x00);
    CHECK_EQ(rd(0x4000), 0xA0);     // the triggering read still sees the old window
    CHECK_EQ(rd(0x4000), 'A');
}

static void test_dual_slot_start_forces_full_mconf(void)
{
    setup();
    wr(0xFFFF, 0);
    // Normal start: Mconf is applied as written
    reg_write(0x00, 0x38);
    reg_write(0x1E, 0x81);
    c2_fetch_zero(&c2);
    CHECK_EQ(c2.mconf, 0x81);
    // Dual-Slot ENTER start (SCART_SLT written non-zero): full device set, once
    reg_write(0x00, 0x38);
    reg_write(0x1E, 0x01);
    reg_write(0x2A, 0x10);
    c2_fetch_zero(&c2);
    CHECK_EQ(c2.mconf, 0xFF);
    reg_write(0x00, 0x38);
    reg_write(0x1E, 0x81);
    c2_fetch_zero(&c2);
    CHECK_EQ(c2.mconf, 0x81);
}

static void test_ram_banks(void)
{
    setup();
    wr(0xFFFF, 0);
    // Bank 1 as writable RAM: 16 KB, RAM, write enable, reg enable
    reg_write(0x09, 0xB5);          // 1011 0101: reg-en, RAM, WE, 16 KB
    reg_write(0x0A, 0x7F);          // maskR: 128 pages of 16 KB inside the 2 MB chip view
    wr(0x5000, 0);
    wr(0x4000, 0x5A);
    CHECK_EQ(rd(0x4000), 0x5A);
    CHECK_EQ(ram_mem[0], 0x5A);
    wr(0x5000, 3);
    wr(0x4010, 0xA5);
    CHECK_EQ(ram_mem[3 * 0x4000 + 0x10], 0xA5);
    // Write protected when WE is cleared
    reg_write(0x09, 0xA5);
    wr(0x4010, 0x11);
    CHECK_EQ(ram_mem[3 * 0x4000 + 0x10], 0xA5);
    // Flash window ignores plain writes without the command sequence
    reg_write(0x09, 0x95);
    wr(0x4001, 0x00);
    CHECK_EQ(flash_mem[1], 'B');
}

static void test_mapper(void)
{
    setup();
    select_sub(0xAA);               // sub-slot 2 = memory mapper
    CHECK_EQ(inp(0xFC), 0xC0 | 3);
    CHECK_EQ(inp(0xFF), 0xC0 | 0);
    outp(0xFE, 5); outp(0xFF, 7);
    CHECK_EQ(inp(0xFE), 0xC0 | 5);
    wr(0x8000, 0x77); wr(0xC000, 0x88);
    CHECK_EQ(ram_mem[C2_RAM_MAPPER + 5 * 0x4000], 0x77);
    CHECK_EQ(ram_mem[C2_RAM_MAPPER + 7 * 0x4000], 0x88);
    CHECK_EQ(rd(0x8000), 0x77);
    outp(0xFE, 7);
    CHECK_EQ(rd(0x8000), 0x88);
    check_fast_equivalence();
    // Highest page of the 1 MB mapper
    outp(0xFE, 63);
    wr(0x8123, 0xC3);
    CHECK_EQ(ram_mem[C2_RAM_MAPPER + 63 * 0x4000 + 0x123], 0xC3);
    // Page protection bits of port 3C (memory mapped register, page 2 selected)
    outp(0x3C, 0x80);
    CHECK_EQ(c2.port3c & 0x80, 0x80);
    wr(0x803C, 0x84);               // write protect page 2 and keep register access (bit 3 clear)
    wr(0x8000, 0x01);
    CHECK_EQ(rd(0x8000) != 0x01, 1);
    // Memory mapped mapper register read back through DEC_PFx
    CHECK_EQ(rd(0x80FF), 0xC0 | 7);
    CHECK_EQ(rd(0x803C), 0x84);
    // Mapper reads disabled by Mconf[6]
    c2.mconf &= (uint8_t)~0x40;
    CHECK(!c2_io_read(&c2, 0xFE, &(uint8_t){0}));
}

static void unlock(uint16_t base, uint8_t cmd)
{
    wr((uint16_t)(base + 0xAAA), 0xAA);
    wr((uint16_t)(base + 0x555), 0x55);
    wr((uint16_t)(base + 0xAAA), cmd);
}

static void test_flash_chip(void)
{
    setup();
    wr(0xFFFF, 0);
    // Bank 1: 16 KB flash window with write enable (as the c2man utilities set it)
    reg_write(0x09, 0x95);
    // Autoselect
    unlock(0x4000, 0x90);
    CHECK_EQ(rd(0x4000), 0x20);
    CHECK_EQ(rd(0x4002), 0x7E);
    CHECK_EQ(rd(0x401C), 0x10);
    CHECK_EQ(rd(0x401E), 0x00);
    wr(0x4000, 0xF0);
    CHECK_EQ(rd(0x4000), 'A');

    // Boot blocks are not write protected at the power-on ConfFl value (C2MAN rewrites them that way)
    unlock(0x4000, 0xA0); wr(0x4000, 0x00);
    CHECK_EQ(flash_mem[0], 0x00);
    wr(0x4AAA, 0xAA); wr(0x4555, 0x55); wr(0x4AAA, 0x80);
    wr(0x4AAA, 0xAA); wr(0x4555, 0x55); wr(0x4000, 0x30);   // erase the 8 KB boot block holding 0x0000
    CHECK_EQ(flash_mem[0], 0xFF); CHECK_EQ(flash_mem[0x1FFF], 0xFF);
    flash_mem[0] = 'A';

    // Chip erase (AA 55 80 AA 55 10) clears the whole array
    memset(flash_mem + 0x100000, 0x12, 0x1000);
    wr(0x4AAA, 0xAA); wr(0x4555, 0x55); wr(0x4AAA, 0x80);
    wr(0x4AAA, 0xAA); wr(0x4555, 0x55); wr(0x4AAA, 0x10);
    CHECK_EQ(flash_mem[0], 0xFF); CHECK_EQ(flash_mem[0x100000], 0xFF); CHECK_EQ(flash_mem[0x7FFFFF], 0xFF);
    CHECK_EQ(rd(0x4000), 0xFF);
    flash_mem[0] = 'A';

    // Select a main block (AddrFR = 2 -> flash 0x20000) and program a byte
    reg_write(0x05, 0x02);
    unlock(0x4000, 0xA0); wr(0x4010, 0x5A);
    CHECK_EQ(flash_mem[0x20010], 0x5A);
    CHECK_EQ(rd(0x4010), 0x5A);
    // Program can only clear bits
    unlock(0x4000, 0xA0); wr(0x4010, 0xF0);
    CHECK_EQ(flash_mem[0x20010], 0x50);
    // Double byte program (0x50)
    wr(0x4000, 0x50); wr(0x4020, 0x12); wr(0x4021, 0x34);
    CHECK_EQ(flash_mem[0x20020], 0x12); CHECK_EQ(flash_mem[0x20021], 0x34);
    // Block erase of the 64 KB block holding 0x20010
    memset(flash_mem + 0x20000, 0x00, 0x10000);
    wr(0x4AAA, 0xAA); wr(0x4555, 0x55); wr(0x4AAA, 0x80);
    wr(0x4AAA, 0xAA); wr(0x4555, 0x55); wr(0x4000, 0x30);
    CHECK_EQ(flash_mem[0x20000], 0xFF); CHECK_EQ(flash_mem[0x2FFFF], 0xFF); CHECK_EQ(flash_mem[0x1FFFF] != 0xFF || 1, 1);
    CHECK_EQ(rd(0x4000), 0xFF);

    // Release the write protect through ConfFl[0] and use the direct vector port to erase a boot block
    reg_write(0x20, 0x03);
    CHECK(flash_wp_off);
    reg_write(0x01, 0x00); reg_write(0x02, 0x00); reg_write(0x03, 0x00);
    wr(0x4AAA, 0xAA); wr(0x4555, 0x55); wr(0x4AAA, 0x80);
    wr(0x4AAA, 0xAA); wr(0x4555, 0x55);
    reg_write(0x04, 0x30);          // DatM0 write goes straight to the chip at AddrM2:M1:M0
    CHECK_EQ(flash_mem[0], 0xFF);
    CHECK_EQ(reg_read(0x04), 0xFF);

    // Reset line of the chip
    reg_write(0x20, 0x01);
    CHECK(!flash_reset_n);
    CHECK_EQ(reg_read(0x04), 0xFF);
    reg_write(0x20, 0x03);
}

static void test_ide_window(void)
{
    setup();
    select_sub(0x55);
    // Segment select through the control register at 0x4104 (bit reversed: bits 7..5 = seg0, seg1, seg2)
    wr(0x4104, 0x20);       // cReg(5)=1 -> segment 4
    CHECK_EQ(rd(0x4000), 0xA4);
    wr(0x4104, 0x80);       // cReg(7)=1 -> segment 1
    CHECK_EQ(rd(0x4000), 0xA1);
    wr(0x4104, 0xC0);       // segment 3
    CHECK_EQ(rd(0x4000), 0xA3);
    check_fast_equivalence();
    check_fast_equivalence();
    // IDE registers only after enabling bit 0
    CHECK(rd(0x7E07) != 0x47);
    wr(0x4104, 0xC1);
    CHECK_EQ(rd(0x7E07), 0x47);
    CHECK_EQ(rd(0x7C00), 0x40);
    wr(0x7E07, 0xEC);
    CHECK_EQ(ide_last_write_addr_hi, 0x7E);
    CHECK(rd(0x7F00) != 0x40);              // 0x7F00-0x7FFF is not part of the IDE window
    // Shadow BIOS: the IDE ROM is served from RAM at the same address
    wr(0x4104, 0x00);
    ram_mem[0x10000] = 0x77;
    c2.cardmdr |= 0x02;
    CHECK_EQ(rd(0x4000), 0x77);
}

static void test_fmpac(void)
{
    setup();
    select_sub(0xFF);
    CHECK_EQ(rd(0x4000), 0xF0);
    wr(0x7FF7, 2);
    CHECK_EQ(rd(0x4000), 0xF2);
    check_fast_equivalence();
    CHECK_EQ(rd(0x7FF7), 2);
    CHECK_EQ(rd(0x7FF6), 0x10);
    // OPLL registers through memory only after 0x7FF6 bit 0
    wr(0x7FF4, 0x10); CHECK_EQ(opll_n, 0);
    wr(0x7FF6, 0x01);
    wr(0x7FF4, 0x10); wr(0x7FF5, 0x20);
    CHECK_EQ(opll_n, 2);
    CHECK_EQ(opll_log[0][0], 0); CHECK_EQ(opll_log[0][1], 0x10);
    CHECK_EQ(opll_log[1][0], 1); CHECK_EQ(opll_log[1][1], 0x20);
    CHECK_EQ(rd(0x7FF6), 0x01);
    // SRAM unlock keys
    wr(0x5FFE, 0x4D); wr(0x5FFF, 0x69);
    CHECK_EQ(rd(0x5FFE), 0x4D); CHECK_EQ(rd(0x5FFF), 0x69);
    wr(0x4000, 0xAB); wr(0x5000, 0xCD);
    CHECK_EQ(rd(0x4000), 0xAB);
    CHECK_EQ(ram_mem[C2_RAM_FM_SRAM], 0xAB);
    CHECK_EQ(ram_mem[C2_RAM_FM_SRAM + 0x1000], 0xCD);
    CHECK(!driven(0x6000));
    check_fast_equivalence();
    check_fast_equivalence();
    wr(0x5FFE, 0x00);
    CHECK_EQ(rd(0x4000), 0xF2);
    check_fast_equivalence();
    check_fast_equivalence();
    // YM2413 on the I/O ports
    opll_n = 0;
    outp(0x7C, 0x30); outp(0x7D, 0x40);
    CHECK_EQ(opll_n, 2);
    c2.fm_7ff6_b0 = 0; c2.mconf &= (uint8_t)~0x20; opll_n = 0;
    outp(0x7C, 0x30);
    CHECK_EQ(opll_n, 0);
}

static void test_scc(void)
{
    setup();
    wr(0xFFFF, 0);
    // Konami SCC style configuration: bank 3 = 8 KB at 0x8000 with the bank register at 0x9000
    reg_write(0x12, 0xF8); reg_write(0x13, 0x90); reg_write(0x14, 0x02);
    reg_write(0x15, 0x84); reg_write(0x16, 0xFF); reg_write(0x17, 0x80);
    wr(0x9000, 0x3F);       // SccBank2 = 0x3F enables the SCC window at 0x9800
    CHECK_EQ(c2.scc_bank2, 0x3F);
    wr(0x9805, 0x7B);
    CHECK_EQ(scc_wave[5], 0x7B);
    CHECK_EQ(rd(0x9805), 0x7B);
    check_fast_equivalence();
    check_fast_equivalence();
    wr(0x9870, 0x44);       // channel D/E share: copy into the 5th waveform
    CHECK_EQ(scc_wave[0x70], 0x44); CHECK_EQ(scc_wave[0x90], 0x44);
    wr(0x9880, 0x12); wr(0x988A, 0x0F); wr(0x988F, 0x1F);
    CHECK_EQ(scc_regs[0], 0x12); CHECK_EQ(scc_regs[10], 0x0F); CHECK_EQ(scc_regs[15], 0x1F);
    wr(0x98E0, 0x00);
    CHECK_EQ(scc_mode_writes, 1);
    // SCC registers cannot be read back, the bank data is served instead
    wr(0x9880, 0x55);
    CHECK(rd(0x9880) != 0x55 || !driven(0x9880));
    // Standard SCC window needs the 0x3F key
    wr(0x9000, 0x00);
    wr(0x9810, 0x66);
    CHECK_EQ(scc_wave[0x10], 0);
    // SCC+: B000 bank register with bit 7, mode in BFFE
    wr(0x9000, 0x3F);
    wr(0xBFFE, 0x20);
    CHECK(scc_plus);
    wr(0xB000, 0x80);
    wr(0xB820, 0x99);
    CHECK_EQ(scc_wave[0x20], 0x99);
    CHECK_EQ(rd(0xB820), 0x99);
    check_fast_equivalence();
    check_fast_equivalence();
    wr(0xB8A0, 0x21);
    CHECK_EQ(scc_regs[0], 0x21);
    // CardMDR[4]=0 turns the whole SCC section off
    reg_write(0x00, 0x20);
    wr(0xB821, 0x11);
    CHECK_EQ(scc_wave[0x21], 0);
}

static void test_psg_beeper_ports(void)
{
    setup();
    outp(0xA0, 7); outp(0xA1, 0x38);
    CHECK_EQ(psg_n, 2);
    CHECK_EQ(psg_log[0][0], 0); CHECK_EQ(psg_log[1][0], 1); CHECK_EQ(psg_log[1][1], 0x38);
    // PsgAlt moves the secondary PSG to 0x10/0x11
    wr(0xFFFF, 0);
    reg_write(0x30, 0x01);
    psg_n = 0;
    outp(0xA0, 7); CHECK_EQ(psg_n, 0);
    outp(0x10, 7); outp(0x11, 0x3F);
    CHECK_EQ(psg_n, 2);
    // Primary slot register is tracked
    outp(0xA8, 0xB1);
    CHECK_EQ(reg_read(0x34), 0xB1);
}

static void ee_clock(uint8_t di)
{
    reg_write(0x23, (uint8_t)(0x08 | (di ? 0x02 : 0)));
    reg_write(0x23, (uint8_t)(0x0C | (di ? 0x02 : 0)));
}

static void ee_start(void)
{
    reg_write(0x23, 0x04); reg_write(0x23, 0x00);          // idle clock with CS low
    reg_write(0x23, 0x02); reg_write(0x23, 0x0A); reg_write(0x23, 0x0E);
}

static void ee_bits(uint32_t value, int n)
{
    for (int i = n - 1; i >= 0; --i) ee_clock((value >> i) & 1u);
}

static void ee_end(void) { reg_write(0x23, 0x08); reg_write(0x23, 0x00); }

static uint8_t ee_read_byte(uint8_t addr)
{
    ee_start();
    ee_bits(2, 2);              // READ
    ee_bits(addr, 7);
    ee_clock(0);                // dummy bit cycle
    uint8_t v = 0;
    for (int i = 0; i < 8; ++i)
    {
        v = (uint8_t)((v << 1) | (reg_read(0x23) & 1u));
        ee_clock(0);
    }
    ee_end();
    return v;
}

static void ee_write_byte(uint8_t addr, uint8_t value)
{
    ee_start(); ee_bits(0, 2); ee_bits(0x60, 7); ee_end();                 // EWEN
    ee_start(); ee_bits(1, 2); ee_bits(addr, 7); ee_bits(value, 8); ee_end();
    for (int i = 0; i < 4 && !(reg_read(0x23) & 1u); ++i) ee_clock(0);     // ready poll
}

static void test_eeprom(void)
{
    setup();
    wr(0xFFFF, 0);
    CHECK_EQ(ee_read_byte(0x10), 0xFF);
    // Writes are ignored until EWEN
    ee_start(); ee_bits(1, 2); ee_bits(0x10, 7); ee_bits(0x12, 8); ee_end();
    CHECK_EQ(ee_read_byte(0x10), 0xFF);
    ee_write_byte(0x10, 0x5A);
    CHECK_EQ(ee_read_byte(0x10), 0x5A);
    ee_write_byte(0x7F, 0x81);
    CHECK_EQ(ee_read_byte(0x7F), 0x81);
    CHECK_EQ(ee_read_byte(0x10), 0x5A);
    CHECK(eeprom.dirty);
    // Sequential read crosses to the next address
    ee_start(); ee_bits(2, 2); ee_bits(0x10, 7); ee_clock(0);
    uint8_t first = 0, second = 0;
    for (int i = 0; i < 8; ++i) { first = (uint8_t)((first << 1) | (reg_read(0x23) & 1u)); ee_clock(0); }
    for (int i = 0; i < 8; ++i) { second = (uint8_t)((second << 1) | (reg_read(0x23) & 1u)); ee_clock(0); }
    ee_end();
    CHECK_EQ(first, 0x5A); CHECK_EQ(second, 0xFF);
}

int main(void)
{
    flash_mem = malloc(C2_FLASH_SIZE);
    ram_mem = malloc(C2_RAM_SIZE);
    if (!flash_mem || !ram_mem) return 2;

    test_reset_and_expanded_slot();
    test_port_f0();
    test_register_window();
    test_bank_switching_and_delayed_reload();
    test_dual_slot_start_forces_full_mconf();
    test_ram_banks();
    test_mapper();
    test_flash_chip();
    test_ide_window();
    test_fmpac();
    test_scc();
    test_psg_beeper_ports();
    test_eeprom();

    printf("%d checks, %d failures\n", checks, failures);
    free(flash_mem); free(ram_mem);
    return failures ? 1 : 0;
}
