#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "emu2212.h"

#define MEGARAM_BANKS 128u
#define MEGARAM_BANK_SIZE 8192u

typedef struct { uint8_t *ptr; uint32_t size; } psram_region_t;
typedef struct { uint8_t segment; } sunrise_ide_t;

static uint8_t megaram_ram[MEGARAM_BANKS * MEGARAM_BANK_SIZE];
static psram_region_t megaram_region = { megaram_ram, sizeof(megaram_ram) };
static SCC scc_instance;

static struct { uint16_t addr; uint8_t data; } writes[32];
static unsigned write_head, write_tail, mapper_count, ide_count;
static uint32_t mapper_offset;

static bool pio_try_get_write(uint16_t *addr, uint8_t *data) {
    if (write_tail == write_head) return false;
    *addr = writes[write_tail].addr;
    *data = writes[write_tail++].data;
    return true;
}
static void enqueue(uint16_t addr, uint8_t data) {
    assert(write_head < 32);
    writes[write_head].addr = addr;
    writes[write_head++].data = data;
}
static void sunrise_ide_handle_write(sunrise_ide_t *ide, uint16_t addr, uint8_t data) {
    (void)addr;
    ide->segment = data;
    ide_count++;
}
static void mapper_write_byte(uint32_t offset, uint8_t data) {
    (void)data;
    mapper_offset = offset;
    mapper_count++;
}

#include "megaram-production.h"

static void reset_state(uint32_t type) {
    memset(megaram_ram, 0xFF, sizeof(megaram_ram));
    memset(&scc_instance, 0, sizeof(scc_instance));
    scc_instance.clk = 3579545;
    scc_instance.rate = 44100;
    SCC_set_quality(&scc_instance, 1);
    scc_instance.type = type;
    SCC_reset(&scc_instance);
    write_head = write_tail = 0;
}

static uint32_t ram_offset(uint8_t bank, uint16_t addr) {
    return ((uint32_t)bank << 13) | (addr & 0x1FFFu);
}

static void test_scc_follows_page_bank(void) {
    uint8_t banks[4] = {0, 1, 2, 3};
    reset_state(SCC_STANDARD);
    megaram_ram[ram_offset(5, 0x9800)] = 0x5A;

    // Konami SCC enable: bank 3Fh in the 9000h page exposes the SCC registers.
    megaram_handle_write(0x9000, 0x3F, banks, false, true);
    assert(scc_instance.active && banks[megaram_page_from_addr(0x9000)] == 0x3F);
    megaram_handle_write(0x9800, 0x21, banks, false, true);
    assert(megaram_handle_read(0x9800, banks, false, true) == 0x21);
    assert(megaram_ram[ram_offset(0x3F, 0x9800)] == 0xFF);
    assert(banks[megaram_page_from_addr(0x9800)] == 0x3F);

    // Switching the page away through 8000h (not 9000h) must hide the SCC.
    megaram_handle_write(0x8000, 5, banks, false, true);
    assert(!scc_instance.active && banks[megaram_page_from_addr(0x8000)] == 5);
    assert(megaram_handle_read(0x9800, banks, false, true) == 0x5A);

    // Any bank write in the page re-enables it; other pages never do.
    megaram_handle_write(0x9E00, 0x3F, banks, false, true);
    assert(scc_instance.active);
    megaram_handle_write(0x7000, 0x3F, banks, false, true);
    megaram_handle_write(0xB000, 0x3F, banks, false, true);
    assert(scc_instance.active && megaram_handle_read(0x9800, banks, false, true) == 0x21);
    megaram_handle_write(0x9000, 0x3E, banks, false, true);
    assert(!scc_instance.active);
    printf("PASS: MegaRAM SCC overlay follows the current bank of the 9000h page\n");
}

static void test_write_mode_is_plain_ram(void) {
    uint8_t banks[4] = {0, 1, 2, 3};
    reset_state(SCC_STANDARD);
    megaram_handle_write(0x9000, 0x3F, banks, false, true);
    megaram_handle_write(0x9810, 0x44, banks, false, true);

    // RAM write mode (IN 8Eh): writes and reads reach RAM, never the SCC.
    megaram_handle_write(0x9810, 0x99, banks, true, true);
    assert(megaram_ram[ram_offset(0x3F, 0x9810)] == 0x99);
    assert(megaram_handle_read(0x9810, banks, true, true) == 0x99);
    megaram_handle_write(0x9000, 0x07, banks, true, true);
    assert(scc_instance.active && banks[megaram_page_from_addr(0x9000)] == 0x3F);
    assert(megaram_ram[ram_offset(0x3F, 0x9000)] == 0x07);

    // Back in bank-switch mode the SCC (still selected) reappears unchanged.
    assert(megaram_handle_read(0x9810, banks, false, true) == 0x44);

    // Without a MegaRAM SCC profile the slot is pure MegaRAM.
    megaram_handle_write(0x9000, 0x3F, banks, false, false);
    megaram_ram[ram_offset(0x3F, 0x9800)] = 0x12;
    assert(megaram_handle_read(0x9800, banks, false, false) == 0x12);
    printf("PASS: MegaRAM write mode hides the SCC; no-SCC profile is plain MegaRAM\n");
}

static void test_scc_plus(void) {
    uint8_t banks[4] = {0, 1, 2, 3};
    reset_state(SCC_ENHANCED);
    megaram_ram[ram_offset(3, 0xB800)] = 0x77;
    megaram_handle_write(0xBFFE, 0x20, banks, false, true);
    assert(scc_instance.base_adr == 0xB000);
    megaram_handle_write(0xB000, 0x80, banks, false, true);
    assert(scc_instance.active && scc_instance.mode == 1);
    megaram_handle_write(0xB800, 0x33, banks, false, true);
    assert(megaram_handle_read(0xB800, banks, false, true) == 0x33);
    megaram_handle_write(0x9000, 0x3F, banks, false, true);
    assert(scc_instance.active);
    megaram_handle_write(0xA000, 3, banks, false, true);
    assert(!scc_instance.active);
    assert(megaram_handle_read(0xB800, banks, false, true) == 0x77);
    printf("PASS: MegaRAM SCC+ overlay follows the current bank of the B000h page\n");
}

static void test_standalone_drain(void) {
    uint8_t banks[4] = {0, 1, 2, 3};
    reset_state(SCC_STANDARD);
    enqueue(0x9000, 0x3F);
    enqueue(0x9800, 0x10);
    enqueue(0x3000, 0x3F);
    enqueue(0xC000, 0x3F);
    megaram_drain_writes(banks, false, true);
    assert(write_tail == write_head && scc_instance.active);
    assert(megaram_handle_read(0x9800, banks, false, true) == 0x10);
    enqueue(0x8123, 9);
    megaram_drain_writes(banks, false, true);
    assert(!scc_instance.active && banks[megaram_page_from_addr(0x8123)] == 9);
    printf("PASS: standalone MegaRAM drain applies every captured bank/SCC write\n");
}

static void test_nextor_drain(void) {
    sunrise_ide_t ide = {0};
    sunrise_megaram_bus_t bus = {
        .ide = &ide, .mapper_reg = {3, 2, 1, 0}, .megaram_bank_reg = {0, 1, 2, 3},
        .subslot_reg = 1u << 4, .megaram_write_enabled = false, .megaram_scc_audio = true,
    };
    reset_state(SCC_STANDARD);
    // Page 2 in the 1MB mapper subslot: SCC-looking writes are mapper RAM.
    enqueue(0x9000, 0x3F);
    enqueue(0x9800, 0x55);
    sunrise_megaram_drain_writes(&bus);
    assert(mapper_count == 2 && mapper_offset == 1u * 16384u + 0x1800u && !scc_instance.active);
    // ENASLT page 2 to the MegaRAM subslot: the SCC belongs to MegaRAM.
    enqueue(0xFFFF, 3u << 4);
    enqueue(0x9000, 0x3F);
    enqueue(0x9800, 0x66);
    sunrise_megaram_drain_writes(&bus);
    assert(bus.subslot_reg == (3u << 4) && scc_instance.active && mapper_count == 2);
    assert(megaram_handle_read(0x9800, bus.megaram_bank_reg, false, true) == 0x66);
    // Nextor segment writes in page 1 still reach the IDE.
    enqueue(0xFFFF, 3u << 4);
    enqueue(0x7FFF, 4);
    sunrise_megaram_drain_writes(&bus);
    assert(ide_count == 1 && ide.segment == 4);
    printf("PASS: Nextor MegaRAM drain keeps SCC in the MegaRAM subslot only\n");
}

int main(void) {
    test_scc_follows_page_bank();
    test_write_mode_is_plain_ram();
    test_scc_plus();
    test_standalone_drain();
    test_nextor_drain();
    printf("PASS: MegaRAM SCC/SCC+\n");
    return 0;
}
