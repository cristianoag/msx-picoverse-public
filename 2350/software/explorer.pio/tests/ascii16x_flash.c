#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __not_in_flash_func(name) name

static uint32_t fake_us;
static uint32_t time_us_32(void) { return fake_us; }

#include "ascii16x-production.h"

#define BANKS 16u
#define ROM_BYTES (BANKS * 16384u)

static uint8_t rom[ROM_BYTES];
static uint8_t array[ROM_BYTES];
static uint8_t store[8 * 16384];
static ascii16x_flash_state_t device;

static void command(uint16_t addr, uint8_t data) {
    handle_ascii16x_flash_write(addr, data, &device);
}

static uint8_t read_at(uint16_t addr) {
    uint8_t data;
    assert(fr_read_byte(&device, addr, &data));
    return data;
}

// Every cartridge access is on the bus at 4000h-BFFFh; the command addresses
// below are word 555h/2AAh (A-1 is a don't-care) inside that window.
static void unlock(void) {
    command(0x4AAA, 0xAA);
    command(0x4555, 0x55);
}

static void open_device(bool writable) {
    for (unsigned bank = 0; bank < BANKS; bank++)
        memset(rom + bank * 16384u, (int)bank + 1, 16384u);
    memcpy(array, rom, sizeof(array));
    memset(&device, 0, sizeof(device));
    device.array = writable ? array : NULL;
    device.array_size = ROM_BYTES;
    flash_build_cfi(&device);
    bcache_init(&device.cache, 16384u, 2, writable ? array : rom, ROM_BYTES, store, 8);
    bcache_prefill(&device.cache);
    device.cache.page_slot[0] = bcache_find(&device.cache, 0);
    device.cache.page_slot[1] = device.cache.page_slot[0];
    flash_update_gates(&device);
}

static void run_erase(void) {
    unsigned steps = 0;
    while (device.erase_active) {
        flash_service_erase(&device);
        flash_update_gates(&device);
        assert(++steps < 8192u);
    }
}

static void check_device(bool writable) {
    open_device(writable);

    // Banks switch like the plain mapper: 6000h selects the bank at 4000h.
    assert(read_at(0x4000) == 1);
    command(0x6000, 5);
    assert(read_at(0x4000) == 6 && read_at(0x7FFF) == 6);
    command(0x7000, 9);
    assert(read_at(0x8000) == 10);

    // Autoselect: manufacturer 01h and device ID 7Eh, then reset to read mode.
    unlock();
    command(0x4AAA, 0x90);
    assert(read_at(0x4000) == 0x01 && read_at(0x4002) == 0x7E);
    command(0x4000, 0xF0);
    assert(read_at(0x4000) == 6);

    // CFI query: 'Q' 'R' 'Y' at word 10h-12h and the device size 2^n.
    command(0x40AA, 0x98);
    assert(read_at(0x4020) == 'Q' && read_at(0x4022) == 'R' && read_at(0x4024) == 'Y');
    assert(read_at(0x404E) == 18); // 2^18 = 256 KB
    command(0x4000, 0xF0);
    assert(read_at(0x4000) == 6);

    // Byte program, as the game's own flash probe does it: program a byte and read
    // it back. Both forms show the AND-ed value; only the writable one keeps it
    // beyond the bank cache (and syncs it to the card).
    unlock();
    command(0x4AAA, 0xA0);
    command(0x4100, 0x04);
    assert(read_at(0x4100) == (6 & 0x04));
    assert(!writable || array[5 * 16384u + 0x100] == (6 & 0x04));

    // Sector erase of the bank-5 sector: busy status first (DQ7 low, DQ6 toggling),
    // then FFh, which is what the game's polling loop waits for. The ROM image
    // itself is never touched.
    unlock();
    command(0x4AAA, 0x80);
    unlock();
    command(0x4000, 0x30);
    assert(device.erase_active && device.idle_work);
    uint8_t first = read_at(0x4000), second = read_at(0x4000);
    assert((first & 0x80) == 0 && (first ^ second) == 0x40);
    command(0x4AAA, 0x55); // ignored while the erase runs
    run_erase();
    assert(!device.erase_active);
    assert(read_at(0x4000) == 0xFF);
    assert(!writable || array[5 * 16384u] == 0xFF);

    // Neon Horizon's probe: program 45h at 4000h and read it back (the byte is
    // erased by now, so the AND leaves it intact).
    unlock();
    command(0x4AAA, 0xA0);
    command(0x4000, 0x45);
    assert(read_at(0x4000) == 0x45);
    for (unsigned bank = 0; bank < BANKS; bank++) // the ROM image itself is never touched
        assert(rom[bank * 16384u] == bank + 1);

    // Chip erase finishes too.
    unlock();
    command(0x4AAA, 0x80);
    unlock();
    command(0x4AAA, 0x10);
    assert(device.erase_active);
    run_erase();
    assert(read_at(0x4000) == 0xFF);
    for (unsigned bank = 0; bank < BANKS; bank++)
        assert(rom[bank * 16384u] == bank + 1);

    // Outside 4000h-BFFFh the cartridge stays released.
    uint8_t data;
    assert(!fr_read_byte(&device, 0x3FFF, &data) && !fr_read_byte(&device, 0xC000, &data));
}

int main(void) {
    check_device(true);
    check_device(false);
    puts("PASS: FlashROM device answers autoselect/CFI and bank switching; program and erase read back in both forms, the presented one never touching the ROM image");
    return 0;
}
