// Host tests for the Explorer flash ROM store in pico/explorer/explorer.c:
// F copies a microSD ROM into flash, D deletes a flash entry.
// The production functions run against a simulated 16 MB NOR flash (erase sets
// 0xFF, programming can only clear bits) loaded from a real UF2 built by the
// packaged Explorer tool, so offsets are checked against the tool's layout.
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash-store-types.h"

#define FLASH_PAGE_SIZE   256u
#define FLASH_SECTOR_SIZE 4096u
#define XIP_BASE          ((uintptr_t)sim_flash)

typedef unsigned int UINT;
typedef enum { FR_OK = 0, FR_DISK_ERR } FRESULT;
typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    uint32_t fail_at; // Reads at or past this offset fail (0 = never)
} FIL;
typedef struct {
    uint32_t offset;
    uint32_t size;
    uint8_t *ptr;
} psram_region_t;

static uint8_t *sim_flash;
static uint8_t *orig_flash;
static const uint8_t *flash_rom;

// Globals the production functions share with the rest of explorer.c.
ROMRecord records[MAX_FLASH_RECORDS];
static ROMRecord flash_records[MAX_FLASH_RECORDS];
static uint16_t flash_record_count;
static uint8_t flash_record_slot[MAX_FLASH_RECORDS];
static uint16_t flash_slots_used;
static volatile uint8_t ctrl_mapper_value;
static volatile uint8_t ctrl_ack_value;
static volatile uint8_t ctrl_cmd_state;
static volatile bool refresh_requested;
static psram_region_t sd_rom_region;
static uint8_t psram_stage[64 * 1024];

static unsigned erase_4k_ops, erase_64k_ops, program_pages, unlink_count, options_copied;
static char last_unlink[SD_PATH_MAX];
// Ranges a flash operation may touch, set by each scenario.
static uint32_t allow_lo, allow_hi;
static bool allow_config;

static bool psram_bring_up_once(void) { return true; }
static void quiesce_mp3_core1_before_sd_work(void) {}
static bool sd_mount_card(void) { return true; }
static void sd_activity_note(void) {}

static bool build_pvc_options_path(uint16_t record_index, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "/%s.flash.PVC", records[record_index].Name);
    return written > 0 && (size_t)written < out_size;
}

static FRESULT f_unlink(const char *path) {
    unlink_count++;
    snprintf(last_unlink, sizeof(last_unlink), "%s", path);
    return FR_OK;
}

static FRESULT f_read(FIL *file, void *buffer, UINT count, UINT *read) {
    if (file->fail_at && file->pos + count > file->fail_at) {
        *read = 0;
        return FR_DISK_ERR;
    }
    UINT left = file->size - file->pos;
    UINT n = count < left ? count : left;
    memcpy(buffer, file->data + file->pos, n);
    file->pos += n;
    *read = n;
    return FR_OK;
}

static FRESULT f_close(FIL *file) {
    (void)file;
    return FR_OK;
}

static void flash_job_copy_options(void) { options_copied++; }

static unsigned sidecar_renames;
static char sidecar_old[ROM_NAME_MAX + 1];
static char sidecar_new[ROM_NAME_MAX + 1];
static bool sidecars_allowed = true;
static bool flash_rename_sidecars_allowed(const ROMRecord *rec, const char *old_name, const char *new_name, bool *flashrom) {
    (void)rec;
    (void)old_name;
    (void)new_name;
    *flashrom = false;
    return sidecars_allowed;
}
static void rename_flash_sidecars(const char *old_name, const char *new_name, bool flashrom) {
    (void)flashrom;
    sidecar_renames++;
    snprintf(sidecar_old, sizeof(sidecar_old), "%s", old_name);
    snprintf(sidecar_new, sizeof(sidecar_new), "%s", new_name);
}

unsigned long read_ulong(const unsigned char *ptr) {
    return (unsigned long)ptr[0] | ((unsigned long)ptr[1] << 8) |
           ((unsigned long)ptr[2] << 16) | ((unsigned long)ptr[3] << 24);
}

int isEndOfData(const unsigned char *memory) {
    for (size_t i = 0; i < ROM_RECORD_SIZE; i++) {
        if (memory[i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}

static uint32_t sim_base(void) { return (uint32_t)(flash_rom - sim_flash); }

static bool ranges_overlap(uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len) {
    return a < b + b_len && b < a + a_len;
}

// NOR flash model with guards: every erase/program must stay inside the range
// the scenario allows (the image being written, or the config area itself),
// and an erase must never hit a sector holding a live entry.
static void flash_store_op(uint32_t addr, const uint8_t *data, uint32_t len) {
    assert(addr + len <= PICO_FLASH_SIZE_BYTES);
    uint32_t cfg = sim_base() + MENU_ROM_SIZE;
    bool in_image = addr >= allow_lo && addr + len <= allow_hi;
    bool in_config = allow_config ? (addr >= cfg && addr + len <= cfg + CONFIG_AREA_SIZE)
                                  : (addr >= (cfg & ~(FLASH_PAGE_SIZE - 1u)) && addr + len <= cfg + CONFIG_AREA_SIZE + FLASH_PAGE_SIZE);
    if (data) {
        assert(addr % FLASH_PAGE_SIZE == 0 && len % FLASH_PAGE_SIZE == 0);
        assert(in_image || in_config);
        for (uint32_t i = 0; i < len; i++) {
            sim_flash[addr + i] &= data[i];
        }
        program_pages += len / FLASH_PAGE_SIZE;
        return;
    }
    assert(addr % FLASH_SECTOR_SIZE == 0 && len % FLASH_SECTOR_SIZE == 0);
    assert(in_image || (allow_config && in_config));
    if (in_image) {
        for (uint16_t i = 0; i < flash_record_count; i++) {
            uint32_t start = sim_base() + (uint32_t)flash_records[i].Offset;
            assert(!ranges_overlap(addr, len, start, (uint32_t)flash_records[i].Size));
        }
    }
    memset(sim_flash + addr, 0xFF, len);
    if (len == 65536u) {
        erase_64k_ops++;
    } else {
        erase_4k_ops += len / FLASH_SECTOR_SIZE;
    }
}

#include "flash-store-production.h"

static uint8_t pattern(uint32_t seed, uint32_t i) {
    uint32_t x = seed * 2654435761u ^ (i * 40503u);
    x ^= x >> 13;
    return (uint8_t)(x ^ (i >> 8));
}

static uint8_t *make_data(uint32_t seed, uint32_t size) {
    uint8_t *data = malloc(size);
    assert(data);
    for (uint32_t i = 0; i < size; i++) {
        data[i] = pattern(seed, i);
    }
    return data;
}

static const struct {
    const char *file;
    const char *name;
    uint8_t mapper;
    uint32_t size;
    uint32_t seed;
} fixtures[] = {
    {"Alpha.PLA-32.ROM", "Alpha", 2, 32768u, 1},
    {"Bravo.ASC-16.ROM", "Bravo", 6, 131072u, 2},
    {"Charlie.ASC-08.ROM", "Charlie", 5, 65536u, 3},
};
#define FIXTURE_COUNT (sizeof(fixtures) / sizeof(fixtures[0]))

static void make_roms(const char *dir) {
    for (size_t i = 0; i < FIXTURE_COUNT; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, fixtures[i].file);
        uint8_t *data = make_data(fixtures[i].seed, fixtures[i].size);
        FILE *file = fopen(path, "wb");
        assert(file);
        assert(fwrite(data, 1, fixtures[i].size, file) == fixtures[i].size);
        assert(fclose(file) == 0);
        free(data);
    }
}

static uint32_t read_le32(const uint8_t *ptr) {
    return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
}

static uint32_t load_uf2(const char *path) {
    uint8_t block[512];
    uint32_t end = 0;
    FILE *file = fopen(path, "rb");
    assert(file);
    while (fread(block, 1, sizeof(block), file) == sizeof(block)) {
        assert(read_le32(block) == 0x0A324655 && read_le32(block + 508) == 0x0AB16F30);
        uint32_t addr = read_le32(block + 12) - 0x10000000u;
        uint32_t size = read_le32(block + 16);
        assert(size <= 476 && addr + size <= PICO_FLASH_SIZE_BYTES);
        memcpy(sim_flash + addr, block + 32, size);
        if (addr + size > end) {
            end = addr + size;
        }
    }
    assert(!ferror(file) && fclose(file) == 0);
    return end;
}

static int find_loaded(const char *name) {
    for (uint16_t i = 0; i < flash_record_count; i++) {
        if (strcmp(flash_records[i].Name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void check_data(const char *name, uint32_t seed) {
    int i = find_loaded(name);
    assert(i >= 0);
    const uint8_t *image = flash_rom + flash_records[i].Offset;
    for (uint32_t pos = 0; pos < flash_records[i].Size; pos++) {
        assert(image[pos] == pattern(seed, pos));
    }
}

// Firmware, the pad after it, the menu ROM and the hidden payloads never
// change; the config area is checked through the records it yields.
static void check_static_regions(void) {
    uint32_t cfg = sim_base() + MENU_ROM_SIZE;
    uint32_t rom_area = sim_base() + FLASH_ROM_AREA_OFFSET;
    assert(memcmp(sim_flash, orig_flash, cfg) == 0);
    assert(memcmp(sim_flash + cfg + CONFIG_AREA_SIZE, orig_flash + cfg + CONFIG_AREA_SIZE,
                  rom_area - cfg - CONFIG_AREA_SIZE) == 0);
}

static void check_all_data(uint32_t extra_seed, const char *extra_name) {
    for (size_t i = 0; i < FIXTURE_COUNT; i++) {
        if (find_loaded(fixtures[i].name) >= 0) {
            check_data(fixtures[i].name, fixtures[i].seed);
        }
    }
    if (extra_name) {
        check_data(extra_name, extra_seed);
    }
}

// Runs the copy job the way core1_bg_work() does once flash_job_start() has
// picked the destination, then commits the entry.
static bool copy_rom(const char *name, uint8_t mapper, const uint8_t *data, uint32_t size, uint32_t fail_at) {
    memset(&flash_job_entry, 0, sizeof(flash_job_entry));
    strncpy(flash_job_entry.Name, name, ROM_NAME_MAX - 1);
    flash_job_entry.Mapper = mapper;
    flash_job_entry.Size = size;
    if (!flash_store_find_space(size, &flash_job_addr)) {
        return false;
    }
    flash_job_entry.Offset = flash_job_addr - flash_store_base();
    flash_job_span = flash_store_align_up(size, FLASH_SECTOR_SIZE);
    flash_job_erased = 0;
    flash_job_written = 0;
    flash_job_file = (FIL){.data = data, .size = size, .pos = 0, .fail_at = fail_at};
    flash_job_file_open = true;
    flash_job_state = FLASH_JOB_WRITE;
    allow_lo = flash_job_addr;
    allow_hi = flash_job_addr + flash_job_span;
    unsigned steps = 0;
    while (flash_job_state == FLASH_JOB_WRITE) {
        if (!flash_job_write_step()) {
            flash_job_finish(false);
            allow_lo = allow_hi = 0;
            return false;
        }
        assert(++steps < 100000);
    }
    assert(flash_job_state == FLASH_JOB_COMMIT && ctrl_mapper_value == 100);
    bool ok = flash_job_commit();
    flash_job_finish(ok);
    allow_lo = allow_hi = 0;
    return ok;
}

static void reload(void) {
    ROMRecord before[MAX_FLASH_RECORDS];
    uint16_t count = flash_record_count;
    memcpy(before, flash_records, sizeof(before));
    flash_store_load_records();
    // The in-RAM list kept by the production code matches a fresh read.
    assert(flash_record_count == count);
    for (uint16_t i = 0; i < count; i++) {
        assert(strcmp(before[i].Name, flash_records[i].Name) == 0);
        assert(before[i].Mapper == flash_records[i].Mapper);
        assert(before[i].Size == flash_records[i].Size && before[i].Offset == flash_records[i].Offset);
    }
}

static void check(const char *uf2, uint32_t firmware_size) {
    sim_flash = malloc(PICO_FLASH_SIZE_BYTES);
    orig_flash = malloc(PICO_FLASH_SIZE_BYTES);
    assert(sim_flash && orig_flash);
    memset(sim_flash, 0xA5, PICO_FLASH_SIZE_BYTES); // Stale data past the image
    uint32_t image_end = load_uf2(uf2);
    memcpy(orig_flash, sim_flash, PICO_FLASH_SIZE_BYTES);
    // The tool starts the menu ROM on the first 4 KB boundary after the firmware.
    flash_rom = sim_flash + flash_store_align_up(firmware_size, FLASH_SECTOR_SIZE);
    assert(sim_base() % FLASH_SECTOR_SIZE == 0 && (sim_base() + MENU_ROM_SIZE) % FLASH_SECTOR_SIZE == 0);
    for (uint32_t pad = firmware_size; pad < sim_base(); pad++) {
        assert(sim_flash[pad] == 0xFF);
    }
    sd_rom_region = (psram_region_t){.offset = 0, .size = sizeof(psram_stage), .ptr = psram_stage};

    // Tool image: Nextor (-s1), standalone MegaRAM (-r, no data), three ROMs.
    flash_store_load_records();
    assert(flash_record_count == 5 && flash_slots_used == 5);
    uint32_t max_end = 0;
    for (uint16_t i = 0; i < flash_record_count; i++) {
        assert(flash_record_slot[i] == i);
        uint32_t end = (uint32_t)(flash_records[i].Offset + flash_records[i].Size);
        if (end > max_end) {
            max_end = end;
        }
    }
    uint32_t rom_end = sim_base() + max_end;
    assert(flash_store_align_up(rom_end, FLASH_PAGE_SIZE) == image_end); // UF2 pads its last 256-byte block
    for (size_t i = 0; i < FIXTURE_COUNT; i++) {
        int index = find_loaded(fixtures[i].name);
        assert(index >= 0 && flash_records[index].Mapper == fixtures[i].mapper);
        assert(flash_records[index].Size == fixtures[i].size);
    }
    check_all_data(0, NULL);

    // Free space starts at the first sector after the tool's last ROM.
    uint32_t addr = 0;
    assert(flash_store_find_space(65536u, &addr));
    assert(addr == flash_store_align_up(rom_end, FLASH_SECTOR_SIZE));
    assert(!flash_store_find_space(PICO_FLASH_SIZE_BYTES - FLASH_ROM_AREA_OFFSET, &addr));

    // D on the first tool ROM: tombstone only, nothing erased, .PVC removed.
    int victim = -1;
    for (uint16_t i = 0; i < flash_record_count; i++) {
        if (flash_records[i].Size >= MIN_ROM_SIZE && flash_records[i].Mapper < 10 &&
            (victim < 0 || flash_records[i].Offset < flash_records[victim].Offset)) {
            victim = i;
        }
    }
    assert(victim >= 0);
    ROMRecord deleted = flash_records[victim];
    uint8_t victim_slot = flash_record_slot[victim];
    memcpy(records, flash_records, sizeof(records));
    unsigned erases = erase_4k_ops + erase_64k_ops;
    assert(flash_store_delete_entry((uint16_t)victim));
    assert(erase_4k_ops + erase_64k_ops == erases);
    assert(unlink_count == 1);
    char expected_pvc[SD_PATH_MAX];
    snprintf(expected_pvc, sizeof(expected_pvc), "/%s.flash.PVC", deleted.Name);
    assert(strcmp(last_unlink, expected_pvc) == 0);
    assert(flash_record_count == 4 && find_loaded(deleted.Name) < 0);
    const uint8_t *slot_ptr = flash_rom + MENU_ROM_SIZE + victim_slot * ROM_RECORD_SIZE;
    const uint8_t *orig_slot = orig_flash + (slot_ptr - sim_flash);
    assert(slot_ptr[0] == FLASH_RECORD_DELETED && memcmp(slot_ptr + 1, orig_slot + 1, ROM_RECORD_SIZE - 1) == 0);
    reload();
    assert(flash_slots_used == 5);
    check_static_regions();
    check_all_data(0, NULL);

    // F of a ROM that fits in the freed gap reuses it without touching the
    // sectors shared with its neighbours.
    uint32_t gap_size = (uint32_t)deleted.Size - 8192u;
    uint8_t *gap_rom = make_data(99, gap_size);
    assert(copy_rom("Gap Filler", 6, gap_rom, gap_size, 0));
    assert(ctrl_ack_value == 1 && refresh_requested && options_copied == 1);
    int gap = find_loaded("Gap Filler");
    assert(gap >= 0 && flash_record_slot[gap] == 5 && flash_slots_used == 6);
    assert(flash_records[gap].Offset >= deleted.Offset);
    assert(flash_records[gap].Offset + flash_store_align_up(gap_size, FLASH_SECTOR_SIZE) <= deleted.Offset + deleted.Size);
    assert(sim_base() + (uint32_t)flash_records[gap].Offset == flash_store_align_up(sim_base() + (uint32_t)deleted.Offset, FLASH_SECTOR_SIZE));
    reload();
    check_static_regions();
    check_all_data(99, "Gap Filler");

    // R on a flash entry: the renamed entry goes into the next slot and the old
    // slot is tombstoned; the image is not touched and nothing is erased.
    int before = find_loaded("Gap Filler");
    ROMRecord original = flash_records[before];
    uint16_t slots_before = flash_slots_used;
    uint16_t count_before = flash_record_count;
    unsigned erases_before = erase_4k_ops + erase_64k_ops;
    memcpy(records, flash_records, sizeof(records));
    assert(flash_store_rename_entry((uint16_t)before, "Gap Renamed"));
    assert(erase_4k_ops + erase_64k_ops == erases_before);
    assert(flash_record_count == count_before && flash_slots_used == slots_before + 1u);
    int renamed = find_loaded("Gap Renamed");
    assert(renamed >= 0 && find_loaded("Gap Filler") < 0);
    assert(flash_records[renamed].Offset == original.Offset && flash_records[renamed].Size == original.Size);
    assert(flash_records[renamed].Mapper == original.Mapper && flash_record_slot[renamed] == slots_before);
    assert(sidecar_renames == 1 && strcmp(sidecar_old, "Gap Filler") == 0 && strcmp(sidecar_new, "Gap Renamed") == 0);
    reload();
    check_data("Gap Renamed", 99);
    // A name another entry uses is refused, whatever its case (the sidecars on
    // the card are case-insensitive); the unchanged name programs nothing.
    int other = renamed == 0 ? 1 : 0;
    char taken[ROM_NAME_MAX + 1];
    snprintf(taken, sizeof(taken), "%s", flash_records[other].Name);
    unsigned pages_before = program_pages;
    memcpy(records, flash_records, sizeof(records));
    assert(!flash_store_rename_entry((uint16_t)renamed, taken));
    for (char *p = taken; *p; p++) {
        *p = (char)(isupper((unsigned char)*p) ? tolower((unsigned char)*p) : toupper((unsigned char)*p));
    }
    assert(!flash_store_rename_entry((uint16_t)renamed, taken));
    assert(flash_store_rename_entry((uint16_t)renamed, "Gap Renamed"));
    // A FlashROM save already owned by another ROM blocks the rename.
    sidecars_allowed = false;
    assert(!flash_store_rename_entry((uint16_t)renamed, "Owner Of A Save"));
    sidecars_allowed = true;
    assert(program_pages == pages_before && sidecar_renames == 1);
    // A case-only rename of the entry itself is allowed.
    assert(flash_store_rename_entry((uint16_t)renamed, "GAP RENAMED"));
    reload();
    memcpy(records, flash_records, sizeof(records));
    assert(flash_store_rename_entry((uint16_t)find_loaded("GAP RENAMED"), "Gap Filler"));
    reload();
    check_static_regions();
    check_all_data(99, "Gap Filler");

    // A 4 MB ROM (the microSD maximum) lands after the image, over stale data,
    // using 64 KB block erases where aligned.
    uint32_t big_size = 4u * 1024u * 1024u;
    uint8_t *big_rom = make_data(7, big_size);
    erase_64k_ops = 0;
    assert(copy_rom("Big One", 14, big_rom, big_size, 0));
    int big = find_loaded("Big One");
    assert(big >= 0 && sim_base() + (uint32_t)flash_records[big].Offset == flash_store_align_up(rom_end, FLASH_SECTOR_SIZE));
    assert(erase_64k_ops >= big_size / 65536u - 1u);
    reload();
    check_static_regions();
    check_all_data(99, "Gap Filler");
    check_data("Big One", 7);

    // A failed read leaves no entry, consumes no slot and reports the failure.
    uint16_t count = flash_record_count;
    uint16_t slots = flash_slots_used;
    ctrl_cmd_state = 0x0E;
    assert(!copy_rom("Broken", 6, gap_rom, gap_size, 20000u));
    assert(ctrl_ack_value == 0 && ctrl_cmd_state == 0 && flash_job_state == FLASH_JOB_IDLE && !flash_job_file_open);
    assert(flash_record_count == count && flash_slots_used == slots);
    reload();
    assert(find_loaded("Broken") < 0);

    // Churn through every config slot with copy + delete, then compact.
    uint8_t *small_rom = make_data(5, MIN_ROM_SIZE);
    while (flash_slots_used < FLASH_CONFIG_SLOTS) {
        assert(copy_rom("Churn", 1, small_rom, MIN_ROM_SIZE, 0));
        memcpy(records, flash_records, sizeof(records));
        assert(flash_store_delete_entry((uint16_t)find_loaded("Churn")));
    }
    reload();
    assert(flash_slots_used == FLASH_CONFIG_SLOTS && flash_record_count == count);
    allow_config = true;
    assert(flash_store_compact());
    allow_config = false;
    assert(flash_slots_used == flash_record_count);
    for (uint16_t i = 0; i < flash_record_count; i++) {
        assert(flash_record_slot[i] == i);
    }
    reload();
    assert(flash_slots_used == flash_record_count);
    const uint8_t *tail = flash_rom + MENU_ROM_SIZE + flash_record_count * ROM_RECORD_SIZE;
    for (const uint8_t *p = tail; p < flash_rom + MENU_ROM_SIZE + CONFIG_AREA_SIZE; p++) {
        assert(*p == 0xFF);
    }
    check_static_regions();
    check_all_data(99, "Gap Filler");
    check_data("Big One", 7);
    assert(copy_rom("After Compact", 6, gap_rom, gap_size, 0));
    assert(flash_record_slot[find_loaded("After Compact")] == flash_record_count - 1);
    reload();
    check_data("After Compact", 99);

    // R with every config slot used compacts the area first.
    while (flash_slots_used < FLASH_CONFIG_SLOTS) {
        assert(copy_rom("Churn", 1, small_rom, MIN_ROM_SIZE, 0));
        memcpy(records, flash_records, sizeof(records));
        assert(flash_store_delete_entry((uint16_t)find_loaded("Churn")));
    }
    allow_config = true;
    memcpy(records, flash_records, sizeof(records));
    assert(flash_store_rename_entry((uint16_t)find_loaded("Big One"), "Big Two"));
    allow_config = false;
    assert(flash_slots_used == flash_record_count + 1u && find_loaded("Big One") < 0);
    reload();
    check_static_regions();
    check_data("Big Two", 7);

    // More live entries than the 128-entry list (the tool can write 138): the
    // unloaded ones are protected by refusing copies and compaction until
    // enough entries are deleted.
    uint16_t live = flash_record_count;
    uint16_t slots_start = flash_slots_used;
    uint16_t extra = (uint16_t)(MAX_FLASH_RECORDS + 2u - live);
    for (uint16_t i = 0; i < extra; i++) {
        uint8_t entry[ROM_RECORD_SIZE];
        ROMRecord ram = {.Mapper = 21, .Size = 0, .Offset = FLASH_ROM_AREA_OFFSET};
        snprintf(ram.Name, sizeof(ram.Name), "MegaRAM %u", (unsigned)i);
        flash_store_serialize(&ram, entry);
        memcpy(sim_flash + sim_base() + MENU_ROM_SIZE + (flash_slots_used + i) * ROM_RECORD_SIZE, entry, sizeof(entry));
    }
    flash_store_load_records();
    assert(flash_record_count == MAX_FLASH_RECORDS && flash_store_overflow);
    assert(flash_slots_used == slots_start + extra);
    assert(!flash_job_commit());
    allow_config = true;
    assert(!flash_store_compact());
    allow_config = false;
    memcpy(records, flash_records, sizeof(records));
    assert(!flash_store_rename_entry((uint16_t)find_loaded("Gap Filler"), "Not While Overflowing"));
    for (unsigned round = 0; round < 2; round++) {
        char victim_name[ROM_NAME_MAX];
        snprintf(victim_name, sizeof(victim_name), "MegaRAM %u", round);
        int index = find_loaded(victim_name);
        assert(index >= 0);
        memcpy(records, flash_records, sizeof(records));
        assert(flash_store_delete_entry((uint16_t)index));
        assert(flash_record_count == MAX_FLASH_RECORDS);
        assert(flash_store_overflow == (round == 0));
    }
    assert(find_loaded("MegaRAM 0") < 0 && find_loaded("MegaRAM 1") < 0);
    char last_name[ROM_NAME_MAX];
    snprintf(last_name, sizeof(last_name), "MegaRAM %u", (unsigned)(extra - 1u));
    assert(find_loaded(last_name) >= 0);
    check_static_regions();
    check_all_data(99, "Gap Filler");
    check_data("Big Two", 7);

    // Never more than MAX_FLASH_RECORDS live entries.
    assert(!flash_job_commit());

    free(small_rom);
    free(big_rom);
    free(gap_rom);
    free(orig_flash);
    free(sim_flash);
    printf("PASS: flash store load/delete/copy/rename/gap reuse/4 MB image/read failure/compaction "
           "(%u sector erases, %u block erases, %u pages programmed)\n",
           erase_4k_ops, erase_64k_ops, program_pages);
}

int main(int argc, char **argv) {
    // Used only by flash_job_start(), which needs the live menu state.
    (void)flash_job_index;
    (void)flash_job_record;
    if (argc == 3 && strcmp(argv[1], "make-roms") == 0) {
        make_roms(argv[2]);
        return 0;
    }
    assert(argc == 4 && strcmp(argv[1], "check") == 0);
    char *end;
    unsigned long firmware_size = strtoul(argv[3], &end, 10);
    assert(*end == '\0' && firmware_size > 0 && firmware_size < PICO_FLASH_SIZE_BYTES);
    check(argv[2], (uint32_t)firmware_size);
    return 0;
}
