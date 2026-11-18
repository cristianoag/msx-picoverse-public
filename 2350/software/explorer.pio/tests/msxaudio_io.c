#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MSXAUDIO_PORT_ADDR 0xC0u
#define MSXAUDIO_PORT_DATA 0xC1u

static bool msxaudio_ready = true;
#define __no_inline_not_in_flash_func(name) name

// BIOS subslot state and the devices it sits next to.
static bool msxaudio_bios_slot;
static uint8_t msxaudio_subslot_reg;
static uint8_t *msxaudio_bios_ram;
static const uint8_t *msxaudio_bios_rom;
typedef struct { int unused; } external_scc_game_t;
static unsigned game_reads, game_writes, sfg_reads, sfg_writes;
static bool external_scc_game_read(external_scc_game_t *g, uint16_t addr, uint8_t *data) {
    (void)g; (void)addr; game_reads++; *data = 0x77; return true;
}
static void external_scc_game_write(external_scc_game_t *g, uint16_t addr, uint8_t data) {
    (void)g; (void)addr; (void)data; game_writes++;
}
static bool sfg_read(const uint8_t *base, uint16_t addr, uint8_t *data) {
    (void)base; (void)addr; sfg_reads++; *data = 0x66; return true;
}
static void sfg_write(uint16_t addr, uint8_t data) { (void)addr; (void)data; sfg_writes++; }

// The chip is not under test: record what reaches it.
static struct { uint8_t offset, data; } chip_writes[16];
static unsigned chip_write_count;
static uint8_t chip_read_offset = 0xFFu;

static void msxaudio_queue_write(uint8_t offset, uint8_t data) {
    assert(chip_write_count < 16);
    chip_writes[chip_write_count].offset = offset;
    chip_writes[chip_write_count].data = data;
    chip_write_count++;
}

static uint8_t msxaudio_chip_read(uint8_t offset) {
    chip_read_offset = offset;
    return (uint8_t)(0x50u + offset);
}

// Scripted capture FIFOs. A write is "late" when it is captured after the read
// address was dequeued, the window between the two drains of msxaudio_service_io().
static struct { uint16_t addr; uint8_t data; bool late; } writes[8];
static unsigned write_count, write_next;
static uint16_t reads[4];
static unsigned read_count, read_next;
static bool read_dequeued;
static struct { bool in_window; uint8_t data; } responses[4];
static unsigned response_count;

static bool pio_try_get_io_write(uint16_t *addr, uint8_t *data) {
    if (write_next >= write_count)
        return false;
    if (writes[write_next].late && !read_dequeued)
        return false;
    *addr = writes[write_next].addr;
    *data = writes[write_next].data;
    write_next++;
    return true;
}

static bool pio_try_get_io_read(uint16_t *addr) {
    if (read_next >= read_count)
        return false;
    *addr = reads[read_next++];
    read_dequeued = true;
    return true;
}

static void io_read_respond(bool in_window, uint8_t data) {
    assert(response_count < 4);
    responses[response_count].in_window = in_window;
    responses[response_count].data = data;
    response_count++;
}

#include "msxaudio-production.h"

static void reset(void) {
    memset(chip_writes, 0, sizeof(chip_writes));
    memset(writes, 0, sizeof(writes));
    memset(reads, 0, sizeof(reads));
    memset(responses, 0, sizeof(responses));
    chip_write_count = write_count = write_next = read_count = read_next = response_count = 0;
    read_dequeued = false;
    chip_read_offset = 0xFFu;
    msxaudio_ready = true;
}

int main(void) {
    // Status/register-select (0xC0) and data (0xC1) reach the chip as offsets 0/1.
    reset();
    assert(msxaudio_handle_io_write(0xC0, 0x08) && msxaudio_handle_io_write(0xC1, 0x7F));
    assert(chip_write_count == 2 && chip_writes[0].offset == 0 && chip_writes[1].offset == 1);
    assert(!msxaudio_handle_io_write(0xC2, 1) && !msxaudio_handle_io_write(0x7C, 1) && chip_write_count == 2);
    uint8_t value = 0;
    assert(msxaudio_handle_io_read(0xC1, &value) && value == 0x51 && chip_read_offset == 1);
    assert(!msxaudio_handle_io_read(0xC4, &value) && !msxaudio_handle_io_read(0x98, &value));

    // A write that lands between the two drains must not change the port the
    // read is answered for: status read (0xC0), late write to the data port.
    reset();
    reads[0] = 0x00C0; read_count = 1;
    writes[0].addr = 0x00C1; writes[0].data = 0x12; writes[0].late = true; write_count = 1;
    msxaudio_service_io();
    assert(chip_write_count == 1 && chip_writes[0].offset == 1 && chip_writes[0].data == 0x12);
    assert(chip_read_offset == 0);
    assert(response_count == 1 && responses[0].in_window && responses[0].data == 0x50);

    // A read of a foreign port is released undriven, even after a late write.
    reset();
    reads[0] = 0x0099; read_count = 1;
    writes[0].addr = 0x00C0; writes[0].data = 0x77; writes[0].late = true; write_count = 1;
    msxaudio_service_io();
    assert(chip_write_count == 1 && chip_read_offset == 0xFF);
    assert(response_count == 1 && !responses[0].in_window);

    // Writes captured before the read are applied in order, then the read answers.
    reset();
    writes[0].addr = 0x00C0; writes[0].data = 0x07;
    writes[1].addr = 0x00C1; writes[1].data = 0x01;
    write_count = 2;
    reads[0] = 0x00C1; read_count = 1;
    msxaudio_service_io();
    assert(chip_write_count == 2 && chip_writes[0].offset == 0 && chip_writes[1].offset == 1);
    assert(chip_read_offset == 1 && response_count == 1 && responses[0].data == 0x51);

    // Not ready (chip missing): nothing is applied and every read is released.
    reset();
    msxaudio_ready = false;
    reads[0] = 0x00C0; read_count = 1;
    writes[0].addr = 0x00C1; writes[0].data = 0x01; write_count = 1;
    msxaudio_service_io();
    assert(chip_write_count == 0 && chip_read_offset == 0xFF);
    assert(response_count == 1 && !responses[0].in_window);

    // The NMS-1205 BIOS subslot: 48 KB ROM at its own addresses, two 4 KB work RAM
    // blocks mirrored 0x4000 higher, 0xC000-0xEFFF empty.
    static uint8_t rom[0xC000], ram[0x2000];
    for (unsigned i = 0; i < sizeof(rom); i++)
        rom[i] = (uint8_t)(i * 7u + (i >> 8));
    msxaudio_bios_rom = rom;
    msxaudio_bios_ram = ram;
    for (uint32_t addr = 0; addr < 0x10000u; addr++) {
        uint8_t d = 0;
        bool mapped = msxaudio_bios_read((uint16_t)addr, &d);
        uint32_t block = addr >> 12;
        if (block == 3 || block == 7 || block == 0xB || block == 0xF)
            assert(mapped);
        else if (block >= 0xC && block <= 0xE)
            assert(!mapped);
        else
            assert(mapped && d == rom[addr]);
    }
    uint8_t d = 0;
    msxaudio_bios_write(0x3010, 0xA5);
    assert(msxaudio_bios_read(0x3010, &d) && d == 0xA5 && msxaudio_bios_read(0x7010, &d) && d == 0xA5 && ram[0x10] == 0xA5);
    msxaudio_bios_write(0xF020, 0x5A);
    assert(msxaudio_bios_read(0xB020, &d) && d == 0x5A && ram[0x1020] == 0x5A);
    assert(msxaudio_bios_read(0x3020, &d) && d == 0 && msxaudio_bios_read(0xB010, &d) && d == 0);
    msxaudio_bios_write(0x4000, 0x00);
    msxaudio_bios_write(0xC000, 0x00);
    assert(msxaudio_bios_read(0x4000, &d) && d == rom[0x4000]);
    assert(rom[0x4000] == (uint8_t)(0x4000u * 7u + 0x40u));
    msxaudio_bios_ram = NULL; // the chip could not be built: the subslot is empty
    assert(!msxaudio_bios_read(0x3000, &d) && !msxaudio_bios_read(0x4000, &d));
    msxaudio_bios_write(0x3000, 1);
    msxaudio_bios_ram = ram;

    // The game ROM sits in subslot 0 and the BIOS in subslot 1 of an expanded slot.
    external_scc_game_t game;
    msxaudio_bios_slot = false;
    game_reads = game_writes = sfg_reads = sfg_writes = 0;
    assert(fm_game_read(&game, 0xFFFF, &d) && d == 0x77 && game_reads == 1); // not expanded: untouched
    fm_game_write(&game, 0xFFFF, 0x04);
    assert(game_writes == 1);
    assert(bios_slot_read(rom, 0x4000, &d) && d == 0x66 && sfg_reads == 1);
    bios_slot_write(0x4000, 1);
    assert(sfg_writes == 1);

    msxaudio_bios_slot = true;
    msxaudio_subslot_reg = 0;
    game_reads = game_writes = 0;
    assert(fm_game_read(&game, 0x4000, &d) && d == 0x77 && game_reads == 1);
    fm_game_write(&game, 0xFFFF, 0x04); // page 1 -> subslot 1
    assert(msxaudio_subslot_reg == 0x04 && game_writes == 0);
    assert(fm_game_read(&game, 0xFFFF, &d) && d == (uint8_t)~0x04);
    assert(fm_game_read(&game, 0x4000, &d) && d == rom[0x4000] && game_reads == 1);
    assert(fm_game_read(&game, 0x8000, &d) && d == 0x77 && game_reads == 2); // page 2 is still the game
    fm_game_write(&game, 0x7000, 0x33);
    assert(game_writes == 0 && ram[0x00] == 0x33); // BIOS work RAM, not the game's mapper
    fm_game_write(&game, 0x8000, 0x01);
    assert(game_writes == 1);
    fm_game_write(&game, 0xFFFF, 0x20); // page 2 -> subslot 2: nothing there
    assert(!fm_game_read(&game, 0x8000, &d) && game_reads == 2);
    fm_game_write(&game, 0x8000, 0x01);
    assert(game_writes == 1);
    assert(bios_slot_read(rom, 0x4000, &d) && d == rom[0x4000] && sfg_reads == 1);
    bios_slot_write(0x7000, 0x44);
    assert(sfg_writes == 1 && ram[0] == 0x44);

    printf("PASS: MSX-AUDIO I/O service answers its own port when a write lands between the drains; BIOS subslot map and expanded-slot routing\n");
    return 0;
}
