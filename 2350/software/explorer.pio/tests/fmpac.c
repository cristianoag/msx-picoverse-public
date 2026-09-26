#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#define FMPAC_BIOS_SIZE 65536u
#define MSX_MUSIC_PORT_REG 0x7Cu
#define MSX_MUSIC_PORT_DATA 0x7Du
#define atrace_note_opll_write(...) ((void)0)
#define atrace_note_fmpac_ctl(...) ((void)0)

typedef struct { uint8_t segment; } sunrise_ide_t;
typedef struct { uint8_t Mapper; } ROMRecord;
static uint8_t cur_wifi_enabled, current_wavegame_rom;
static uint16_t fm_writes[16];
static unsigned fm_count, mapper_count, megaram_count, bank_count, ide_count;
static uint32_t mapper_offset;
static uint8_t mapper_data;
static struct { uint16_t addr; uint8_t data; } writes[16];
static unsigned write_head, write_tail;

static uint8_t record_mapper_code(uint8_t mapper) { return mapper & 0x1Fu; }
static int record_is_folder(const ROMRecord *record) { return (record->Mapper & 0x40u) != 0; }
static void msx_music_write_io(uint8_t port, uint8_t data) {
    assert(fm_count < 16);
    fm_writes[fm_count++] = ((uint16_t)port << 8) | data;
}
static bool pio_try_get_write(uint16_t *addr, uint8_t *data) {
    if (write_tail == write_head) return false;
    *addr = writes[write_tail].addr;
    *data = writes[write_tail++].data;
    return true;
}

// MSX I/O bus captor. The standalone FM-PAC loop drains this on Core 0: the
// hardware FIFO is only eight entries deep, so it has to be emptied by the loop
// that polls continuously rather than by the audio core.
static struct { uint16_t addr; uint8_t data; } io_writes[16];
static unsigned io_write_head, io_write_tail;
static unsigned io_reads_pending, io_read_responses;
static unsigned io_psg_count;
static struct { int pio_read; int sm_io_read; } msx_io_bus = { .pio_read = 0x5A5A, .sm_io_read = 0x5A };

static bool pio_try_get_io_write(uint16_t *addr, uint8_t *data) {
    if (io_write_tail == io_write_head) return false;
    *addr = io_writes[io_write_tail].addr;
    *data = io_writes[io_write_tail++].data;
    return true;
}
static bool pio_try_get_io_read(uint16_t *addr) {
    if (io_reads_pending == 0u) return false;
    io_reads_pending--;
    *addr = 0x00a2u;
    return true;
}
static void enqueue_io(uint16_t addr, uint8_t data) {
    assert(io_write_head < 16);
    io_writes[io_write_head].addr = addr;
    io_writes[io_write_head++].data = data;
}
static bool main_psg_handle_io_write(uint8_t port, uint8_t data) {
    (void)data;
    if (port == 0xA0u || port == 0xA1u) { io_psg_count++; return true; }
    return false;
}
static void enqueue(uint16_t addr, uint8_t data) {
    assert(write_head < 16);
    writes[write_head].addr = addr;
    writes[write_head++].data = data;
}
static void wifi_handle_mem_write(uint16_t addr, uint8_t data) { (void)addr; (void)data; }
static void sunrise_ide_handle_write(sunrise_ide_t *ide, uint16_t addr, uint8_t data) {
    assert(addr >= 0x4000u && addr <= 0x7FFFu);
    ide->segment = data;
    ide_count++;
}
static void mapper_write_byte(uint32_t offset, uint8_t data) {
    mapper_offset = offset;
    mapper_data = data;
    mapper_count++;
}
static void megaram_write_byte(uint16_t addr, uint8_t data, uint8_t *banks) {
    (void)data; (void)banks;
    assert(addr >= 0x4000u && addr <= 0xBFFFu);
    megaram_count++;
}
static void megaram_bank_switch_write(uint16_t addr, uint8_t data, uint8_t *banks) {
    (void)data; (void)banks;
    assert(addr >= 0x4000u && addr <= 0xBFFFu);
    bank_count++;
}

typedef struct { uint8_t *ram_ptr; uint32_t ram_size, max_written; } c2_state_t;
#define SYSTEM_AUDIO_PROFILE_MSX_MUSIC 1
static int system_audio_profile = SYSTEM_AUDIO_PROFILE_MSX_MUSIC, scc_instance;
static unsigned c2_count, scc_count, sfg_count;
static bool c2_addr_is_regwin(c2_state_t *c2, uint16_t addr) { (void)c2; return addr == 0x4F80; }
static void c2_reg_write(c2_state_t *c2, uint16_t addr, uint8_t data) {
    (void)c2; (void)addr; (void)data; c2_count++;
}
static void SCC_write(int *scc, uint16_t addr, uint8_t data) {
    (void)scc; (void)addr; (void)data; scc_count++;
}
static void sfg_write(uint16_t addr, uint8_t data) { (void)addr; (void)data; sfg_count++; }
static void c2_bank_switch_write(c2_state_t *c2, uint16_t addr, uint8_t data) {
    (void)c2; (void)addr; (void)data;
}
static bool c2_decode_addr(c2_state_t *c2, uint16_t addr, uint8_t *bank, uint32_t *linear) {
    (void)c2; (void)addr; (void)bank; (void)linear; return false;
}
static bool c2_bank_is_ram(c2_state_t *c2, uint8_t bank) { (void)c2; (void)bank; return false; }
static bool c2_bank_is_we(c2_state_t *c2, uint8_t bank) { (void)c2; (void)bank; return false; }
static void c2_flash_cmd_write(c2_state_t *c2, uint16_t addr, uint8_t data) {
    (void)c2; (void)addr; (void)data;
}

#define debug_trace(...) ((void)0)
static uint8_t game_rom[512 * 1024], flash_rom[256 * 1024];
static struct { int pio; unsigned sm_read; } msx_bus;
static jmp_buf game_read_done;
static unsigned late_write_case, read_captures;
static uint32_t game_read_token;
static void fmpac_wait_for_expanded_bootstrap(void) {}
static void msx_pio_bus_init(void) {}
static void start_msx_music_audio_output(void) {}
static void tight_loop_contents(void) {}
static void prepare_rom_source(uint32_t offset, bool cache, uint32_t limit,
                               const uint8_t **base, uint32_t *length) {
    (void)offset; (void)cache; (void)limit;
    *base = game_rom;
    *length = sizeof(game_rom);
}
static bool pio_sm_is_rx_fifo_empty(int pio, unsigned sm) { (void)pio; (void)sm; return false; }
static uint32_t pio_sm_get(int pio, unsigned sm) {
    (void)pio; (void)sm;
    assert(++read_captures == 1);
    // Model a write captor publishing the preceding bus write after the
    // first empty-FIFO check but before the read address is consumed.
    if (late_write_case == 0) {
        enqueue(0xFFFF, 3 << 2);
        return 0x4018;
    }
    if (late_write_case == 1) {
        enqueue(0x6000, 3);
        return 0x4000;
    }
    enqueue(0xFFFF, 0x55);
    return 0xFFFF;
}
static uint32_t pio_build_token(bool mapped, uint8_t data) { return (mapped ? 0x100u : 0) | data; }
static void pio_sm_put_blocking(int pio, unsigned sm, uint32_t token) {
    (void)sm;
    // I/O read responses are counted; only a memory read response ends the
    // game loop under test.
    if (pio == msx_io_bus.pio_read) { io_read_responses++; return; }
    game_read_token = token;
    longjmp(game_read_done, 1);
}
static uint8_t read_rom_byte(const uint8_t *base, uint32_t offset) {
    assert(offset < sizeof(game_rom));
    return base[offset];
}

#include "fmpac-production.h"

static void test_game_read_ordering(void) {
    for (unsigned bank = 0; bank < 64; bank++)
        memset(game_rom + bank * 8192, bank, 8192);
    memcpy(flash_rom + FMPAC_BIOS_FLASH_OFFSET + 0x18, "PAC2OPLL", 8);
    const uint32_t expected[] = {0x150, 0x103, 0x1AA};
    for (late_write_case = 0; late_write_case < 3; late_write_case++) {
        write_head = write_tail = read_captures = 0;
        if (setjmp(game_read_done) == 0)
            loadrom_fmpac(0, true, 5);
        assert(game_read_token == expected[late_write_case]);
        assert(write_head == write_tail);
    }
    puts("PASS: production game loop commits late subslot/bank writes before BIOS, ROM and FFFF reads");
}

// The standalone FM-PAC loop must drain the MSX I/O captor on Core 0. That FIFO
// is eight entries deep and a Z80 OUT burst fills it in roughly 35 us, so
// leaving it to the audio core - which could only drain it once per rendered
// sample, and not at all while swapping buffers - silently lost bursts of FM
// register writes. It is why the same game sounded correct under a Nextor
// SYSTEM ROM, whose bus loop always serviced I/O on Core 0, and wrong when
// launched directly from Explorer.
static void test_game_services_io_bus(void) {
    write_head = write_tail = read_captures = 0;
    io_write_head = io_write_tail = 0;
    io_reads_pending = io_read_responses = 0;
    fm_count = 0;
    io_psg_count = 0;
    late_write_case = 0;

    // A YM2413 register write (address + data) and a PSG write arriving on the
    // I/O bus, plus an I/O read the cartridge has to answer.
    enqueue_io(0x007C, 0x30);
    enqueue_io(0x007D, 0x1F);
    enqueue_io(0x00A0, 0x07);
    io_reads_pending = 1;

    if (setjmp(game_read_done) == 0)
        loadrom_fmpac(0, true, 5);

    assert(io_write_head == io_write_tail);
    assert(fm_count == 2 && fm_writes[0] == 0x7C30 && fm_writes[1] == 0x7D1F);
    assert(io_psg_count == 1);
    assert(io_read_responses == 1);
    puts("PASS: standalone game loop services the MSX I/O captor on Core 0 (FM, PSG and reads)");
}

static void test_profiles(void) {
    const uint8_t variants[] = {10, 11, 15, 16, 17, 18, 19, 20};
    for (unsigned i = 0; i < sizeof(variants); i++) {
        ROMRecord record = {variants[i]};
        assert(resolve_audio_mode(record.Mapper, AUDIO_PROFILE_MSX_MUSIC) == AUDIO_MODE_MSX_MUSIC);
        assert(audio_profile_is_supported(&record, AUDIO_PROFILE_MSX_MUSIC));
        record.Mapper |= 0x80u;
        assert(audio_profile_is_supported(&record, AUDIO_PROFILE_MSX_MUSIC));
        cur_wifi_enabled = 1;
        assert(!audio_profile_is_supported(&record, AUDIO_PROFILE_MSX_MUSIC));
        cur_wifi_enabled = 0;
        assert(resolve_audio_mode(variants[i], AUDIO_PROFILE_NONE) == AUDIO_MODE_NONE);
    }
    ROMRecord record = {21};
    assert(!audio_profile_is_supported(&record, AUDIO_PROFILE_MSX_MUSIC));
    assert(resolve_audio_mode(21, AUDIO_PROFILE_MSX_MUSIC) == AUDIO_MODE_NONE);
    assert(resolve_audio_mode(19, AUDIO_PROFILE_MEGARAM_SCC) == AUDIO_MODE_MEGARAM_SCC);
    assert(resolve_audio_mode(20, AUDIO_PROFILE_MEGARAM_SCC_PLUS) == AUDIO_MODE_MEGARAM_SCC_PLUS);
    assert(resolve_audio_mode(11, AUDIO_PROFILE_SCC_EXTERNAL) == AUDIO_MODE_SCC_EXTERNAL);
    assert(resolve_audio_mode(17, AUDIO_PROFILE_YM2151_SFG05) == AUDIO_MODE_YM2151_SFG05);
    record.Mapper = 1;
    assert(audio_profile_is_supported(&record, AUDIO_PROFILE_MSX_MUSIC));
    assert(resolve_audio_mode(1, AUDIO_PROFILE_MSX_MUSIC) == AUDIO_MODE_MSX_MUSIC);
}

static void test_fmpac(void) {
    static uint8_t bios[FMPAC_BIOS_SIZE];
    for (unsigned bank = 0; bank < 4; bank++) memset(bios + bank * 16384, bank + 1, 16384);
    fmpac_state_t pac = {.control = 0x10};
    for (unsigned bank = 0; bank < 4; bank++) {
        fmpac_handle_write(&pac, 0x7FF7, bank | 0xFC);
        assert(fmpac_handle_read(&pac, bios, 0x7FF7) == bank);
        assert(fmpac_handle_read(&pac, bios, 0x4000) == bank + 1);
        assert(fmpac_handle_read(&pac, bios, 0x7FFF) == bank + 1);
    }
    fmpac_handle_write(&pac, 0x4000, 0xA5);
    assert(fmpac_handle_read(&pac, bios, 0x4000) == 4);
    fmpac_handle_write(&pac, 0x5FFE, 0x4D);
    fmpac_handle_write(&pac, 0x5FFF, 0x69);
    fmpac_handle_write(&pac, 0x4000, 0xA5);
    fmpac_handle_write(&pac, 0x5FFD, 0x5A);
    assert(fmpac_handle_read(&pac, bios, 0x4000) == 0xA5);
    assert(fmpac_handle_read(&pac, bios, 0x5FFD) == 0x5A);
    assert(fmpac_handle_read(&pac, bios, 0x6000) == 4);
    fmpac_handle_write(&pac, 0x5FFF, 0);
    assert(fmpac_handle_read(&pac, bios, 0x4000) == 4);
    fmpac_handle_write(&pac, 0x7FF6, 0x11);
    assert(fmpac_handle_read(&pac, bios, 0x7FF6) == 0x11);
    assert(fmpac_handle_read(&pac, bios, 0x3FFF) == 0xFF);
    assert(fmpac_handle_read(&pac, bios, 0x8000) == 0xFF);
    fmpac_handle_write(&pac, 0x7FF4, 0x20);
    fmpac_handle_write(&pac, 0x7FF5, 0x15);
    assert(fm_count == 2 && fm_writes[0] == 0x7C20 && fm_writes[1] == 0x7D15);
}

static void test_bus(void) {
    sunrise_ide_t ide = {0};
    sunrise_fmpac_bus_t bus = {
        .ide = &ide, .mapper_reg = {3, 2, 1, 0},
        .nextor_subslot = 0, .mapper_subslot = 1, .fmpac_subslot = 2,
        .mapper_enable = true, .megaram_enable = true
    };
    enqueue(0xFFFF, 0);
    enqueue(0x7FFF, 7);
    enqueue(0xFFFF, 2 << 2);
    enqueue(0x7FF7, 3);
    sunrise_fmpac_drain_writes(&bus);
    assert(ide_count == 1 && ide.segment == 7 && system_fmpac.page == 3);
    assert(bus.subslot_reg == (2 << 2));

    write_head = write_tail = 0;
    enqueue(0xFFFF, 1 << 2);
    enqueue(0x4123, 0xA5);
    enqueue(0xFFFF, 3 << 2);
    enqueue(0x6000, 4);
    sunrise_fmpac_drain_writes(&bus);
    assert(mapper_count == 1 && mapper_offset == 2 * 16384 + 0x123 && mapper_data == 0xA5);
    assert(bank_count == 1 && megaram_count == 0);
    bus.megaram_write_enabled = true;
    write_head = write_tail = 0;
    enqueue(0x6000, 0x55);
    sunrise_fmpac_drain_writes(&bus);
    assert(megaram_count == 1);
    bus.mapper_enable = bus.megaram_enable = false;
    write_head = write_tail = 0;
    enqueue(0x6000, 0x56);
    enqueue(0xFFFF, 1 << 2);
    enqueue(0x4123, 0x56);
    sunrise_fmpac_drain_writes(&bus);
    assert(megaram_count == 1 && mapper_count == 1);
}

static void test_c2_bus(void) {
    c2_state_t c2 = {0};
    sunrise_ide_t ide = {0};
    uint8_t mapper[4] = {3, 2, 1, 0}, subslot = 0;
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, false, 0x4F80, 1);
    assert(c2_count == 1);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, false, 0xFFFF, 1 << 2);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, false, 0x7FFF, 5);
    assert(ide.segment == 5);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, false, 0xFFFF, 2 << 2);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, false, 0x4567, 0x42);
    assert(mapper_offset == 2 * 16384 + 0x567 && mapper_data == 0x42);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, false, 0xFFFF, 3 << 2);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, false, 0x7FF7, 1);
    assert(system_fmpac.page == 1);
    unsigned before = fm_count;
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, false, 0x7FF4, 0x30);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, false, 0x7FF5, 0x25);
    assert(fm_count == before + 2 && fm_writes[before] == 0x7C30 && fm_writes[before + 1] == 0x7D25);
    system_audio_profile = 0;
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, false, 0x7FF5, 0x26);
    assert(fm_count == before + 2);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, true, false, 0x7FF5, 0x26);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, false, true, 0x7FF5, 0x26);
    assert(scc_count == 1 && sfg_count == 1 && fm_count == before + 2);
    // External SCC lives only in subslot 3: C2 RAM/flash (subslot 0) must not alias it.
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, true, false, 0xFFFF, 0);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, true, false, 0x9000, 0x3F);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, true, false, 0x9800, 0x11);
    c2_handle_memory_write(&c2, &ide, mapper, &subslot, true, false, 0xBFFE, 0x20);
    assert(scc_count == 1);
}

static void test_sunrise_scc_bus(void) {
    sunrise_ide_t ide = {0};
    for (int wifi = 0; wifi <= 1; wifi++) {
        uint8_t base = wifi ? 1u : 0u;
        sunrise_scc_bus_t bus = {
            .ide = &ide, .mapper_reg = {3, 2, 1, 0},
            .subslot_reg = (uint8_t)((base + 1u) << 4),
            .nextor_subslot = base, .mapper_subslot = (uint8_t)(base + 1u),
            .scc_subslot = (uint8_t)(base + 2u),
            .wifi_enable = wifi != 0, .mapper_enable = true,
        };
        unsigned scc0 = scc_count, map0 = mapper_count, ide0 = ide_count;
        write_head = write_tail = 0;
        // RAM subslot: SCC-looking writes are plain mapper RAM writes.
        enqueue(0x9000, 0x3F);
        enqueue(0x9800, 0x12);
        sunrise_scc_drain_writes(&bus);
        assert(scc_count == scc0 && mapper_count == map0 + 2);
        assert(mapper_offset == 1 * 16384 + 0x1800 && mapper_data == 0x12);
        // ENASLT to the SCC subslot, then SCC writes reach only the SCC.
        enqueue(0xFFFF, (uint8_t)(bus.scc_subslot << 4));
        enqueue(0x9000, 0x3F);
        enqueue(0x9800, 0x34);
        sunrise_scc_drain_writes(&bus);
        assert(scc_count == scc0 + 2 && mapper_count == map0 + 2);
        // Nextor in page 1 still receives its segment writes.
        enqueue(0xFFFF, (uint8_t)(bus.nextor_subslot << 2));
        enqueue(0x7FFF, 6);
        sunrise_scc_drain_writes(&bus);
        assert(ide_count == ide0 + 1 && ide.segment == 6 && scc_count == scc0 + 2);
        bus.mapper_enable = false;
        enqueue(0xFFFF, (uint8_t)(bus.mapper_subslot << 4));
        enqueue(0x8000, 1);
        sunrise_scc_drain_writes(&bus);
        assert(mapper_count == map0 + 2 && scc_count == scc0 + 2);
    }
}

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void test_bios_image(const char *bios_path, const char *uf2_path, uint32_t firmware_size) {
    uint8_t bios[FMPAC_BIOS_ROM_SIZE], block[512], records[8 * ROM_RECORD_SIZE];
    FILE *file = fopen(bios_path, "rb");
    assert(file);
    assert(fread(bios, 1, sizeof(bios), file) == sizeof(bios));
    assert(fgetc(file) == EOF && !ferror(file));
    assert(fclose(file) == 0);

    fmpac_state_t pac = {.control = 0x10};
    for (unsigned bank = 0; bank < 4; bank++) {
        fmpac_handle_write(&pac, 0x7FF7, bank);
        assert(fmpac_handle_read(&pac, bios, 0x4000) == 'A');
        assert(fmpac_handle_read(&pac, bios, 0x4001) == 'B');
        for (unsigned i = 0; i < 8; i++)
            assert(fmpac_handle_read(&pac, bios, 0x4018 + i) == (uint8_t)"PAC2OPLL"[i]);
        for (unsigned addr = 0x4000; addr < 0x8000; addr++) {
            if (addr != 0x7FF6 && addr != 0x7FF7)
                assert(fmpac_handle_read(&pac, bios, addr) == bios[bank * 16384 + addr - 0x4000]);
        }
    }

    file = fopen(uf2_path, "rb");
    assert(file);
    const uint32_t bios_start = firmware_size + FMPAC_BIOS_FLASH_OFFSET;
    const uint32_t records_start = firmware_size + MENU_ROM_SIZE;
    uint32_t bios_bytes = 0, record_bytes = 0, blocks = 0, total_blocks = 0;
    size_t count;
    while ((count = fread(block, 1, sizeof(block), file)) != 0) {
        assert(count == sizeof(block));
        assert(read_le32(block) == 0x0A324655 && read_le32(block + 4) == 0x9E5D5157);
        assert(read_le32(block + 508) == 0x0AB16F30);
        assert(read_le32(block + 16) == 256 && read_le32(block + 20) == blocks);
        assert(read_le32(block + 12) == 0x10000000u + blocks * 256);
        total_blocks = read_le32(block + 24);
        for (unsigned i = 0; i < 256; i++) {
            uint32_t offset = blocks * 256 + i;
            if (offset >= bios_start && offset < bios_start + sizeof(bios)) {
                assert(block[32 + i] == bios[offset - bios_start]);
                bios_bytes++;
            }
            if (offset >= records_start && offset < records_start + sizeof(records)) {
                records[offset - records_start] = block[32 + i];
                record_bytes++;
            }
        }
        blocks++;
    }
    assert(!ferror(file) && fclose(file) == 0);
    assert(blocks == total_blocks && bios_bytes == sizeof(bios) && record_bytes == sizeof(records));

    const uint8_t variants[] = {15, 16, 17, 19, 10, 11, 18, 20};
    uint32_t nextor_offset = NEXTOR_DSK_FLASH_OFFSET + NEXTOR_DSK_ROM_SIZE;
    for (unsigned i = 0; i < sizeof(variants); i++) {
        const uint8_t *record = records + i * ROM_RECORD_SIZE;
        assert(record[ROM_NAME_MAX] == variants[i]);
        assert(read_le32(record + ROM_NAME_MAX + 1) == 128 * 1024);
        assert(read_le32(record + ROM_NAME_MAX + 5) == nextor_offset);
        nextor_offset += 128 * 1024;
    }
    puts("PASS: real BIOS AB/PAC2OPLL discovery and all bank bytes; UF2 BIOS payload and 8 Nextor records");
}

int main(int argc, char **argv) {
    assert(argc == 4);
    char *end;
    unsigned long firmware_size = strtoul(argv[3], &end, 10);
    assert(*end == '\0' && firmware_size > 0 && firmware_size < 16 * 1024 * 1024);
    test_profiles();
    test_fmpac();
    test_bus();
    test_c2_bus();
    test_sunrise_scc_bus();
    test_game_read_ordering();
    test_game_services_io_bus();
    test_bios_image(argv[1], argv[2], (uint32_t)firmware_size);
    puts("PASS: 8 Nextor profiles, WiFi exclusion, FM-PAC banks/SRAM/FM writes, Sunrise/C2 bus dispatch");
    return 0;
}
