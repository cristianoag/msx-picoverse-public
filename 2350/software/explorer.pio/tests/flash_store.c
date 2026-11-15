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

#define __noinline
#define __not_in_flash_func(name) name
#define __no_inline_not_in_flash_func(name) name
#include "flash-store-types.h"
#define MAPPER_DETECT_MAX_ROM_SIZE PICO_FLASH_SIZE_BYTES
#include "mapper_detect.h"

#define FLASH_PAGE_SIZE   256u
#define FLASH_SECTOR_SIZE 4096u
#define FA_READ 1u
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
static uint16_t flash_free_kb, flash_largest_kb;
static uint16_t total_record_count, full_record_count;
static uint16_t filtered_indices[MAX_FLASH_RECORDS];
static uint16_t sd_path_offsets[MAX_FLASH_RECORDS];
static char sd_path_buffer[SD_PATH_MAX];
static FIL sd_source_file;
static volatile uint8_t ctrl_mapper_value;
static volatile uint8_t ctrl_ack_value;
static volatile uint8_t ctrl_cmd_state;
static uint8_t browse_source_mode;
static uint16_t match_index;
static char sd_current_path[SD_PATH_MAX];
static volatile bool refresh_requested;
static psram_region_t sd_rom_region;
static uint8_t psram_stage[64 * 1024];
static uint8_t menu_image[MENU_ROM_SIZE], page_buffer[DATA_BUFFER_SIZE];
static uint8_t *rom_sram = menu_image;
static uint8_t current_page, ctrl_status_value, ctrl_audio_selection, ctrl_wifi_support, ctrl_megaram;
static uint8_t ctrl_psg_emulation, ctrl_wavegame_rom, ctrl_sd_partition, sd_browse_partition;
static uint8_t ctrl_audio_volume, ctrl_vdp_frequency, ctrl_cpu_mode, esp_net_status, sd_activity_counter;
static uint8_t ctrl_sd_partition_info[CTRL_SD_PARTITION_INFO_SIZE];
static char pico_chip_id[CTRL_CHIP_ID_SIZE];
static bool fh_menu_window_active;
static uint16_t fh_catalog_count, fh_selected_index, fh_progress_percent, mp3_selected_index;
static uint8_t fh_page_index, fh_status, fh_result, mp3_play_mode;
static char fh_wifi_status_text[FH_STATUS_TEXT_SIZE];
static struct { unsigned pio, sm_read; } msx_bus = {1, 2};
static uint16_t pio_reads[16], pio_results[16];
static unsigned pio_count, pio_read_pos, pio_result_count;
static uint32_t test_now;

static unsigned erase_4k_ops, erase_64k_ops, program_pages, unlink_count, options_copied;
static unsigned image_program_calls, sd_read_calls, mapper_tag_calls;
static char last_unlink[SD_PATH_MAX];
// Ranges a flash operation may touch, set by each scenario.
static uint32_t allow_lo, allow_hi;
static bool allow_config;
static bool corrupt_next_image_program;

static bool psram_bring_up_once(void) { return true; }
static void quiesce_mp3_core1_before_sd_work(void) {}
static bool sd_mount_card(void) { return true; }
static void sd_activity_note(void) {}
static void cancel_refresh_work(void) {}
static void set_root_path(void) { strcpy(sd_current_path, "/"); }
static uint16_t query_filtered_index(void) { return 0; }
static uint8_t mapper_code_from_record_byte(uint8_t mapper) { return mapper & 0x1Fu; }
static uint8_t mapper_number_from_filename(const char *name) {
    mapper_tag_calls++;
    return strstr(name, ".NEO-16.ROM") ? 9 : 0;
}
static bool is_system_mapper(uint8_t mapper) { return mapper == 10 || mapper == 11 || (mapper >= 15 && mapper <= 21); }
static bool is_dsk_record(const ROMRecord *record) {
    return (record->Mapper & (FOLDER_FLAG | MP3_FLAG)) == 0 && mapper_code_from_record_byte(record->Mapper) == MAPPER_DSK;
}
static uint16_t mp3_get_elapsed_seconds(void) { return 0x1234; }
static uint16_t mp3_get_total_seconds(void) { return 0x5678; }
static uint8_t mp3_get_status(void) { return 0x40; }
static uint32_t time_us_32(void) { return test_now; }
static bool pio_sm_is_rx_fifo_empty(unsigned pio, unsigned sm) {
    assert(pio == msx_bus.pio && sm == msx_bus.sm_read);
    return pio_read_pos >= pio_count;
}
static uint32_t pio_sm_get(unsigned pio, unsigned sm) {
    assert(!pio_sm_is_rx_fifo_empty(pio, sm));
    return pio_reads[pio_read_pos++];
}
static void pio_sm_put_blocking(unsigned pio, unsigned sm, uint32_t token) {
    assert(pio == msx_bus.pio && sm == msx_bus.sm_read);
    assert(pio_result_count < pio_count);
    pio_results[pio_result_count++] = (uint16_t)token;
}
static const char *basename_from_path(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
static FRESULT f_open(FIL *file, const char *path, unsigned mode) {
    (void)path;
    assert(mode == FA_READ);
    *file = sd_source_file;
    file->pos = 0;
    return FR_OK;
}
static uint32_t f_size(const FIL *file) { return file->size; }

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
    sd_read_calls++;
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
        if (in_image) image_program_calls++;
        if (in_image && corrupt_next_image_program) {
            sim_flash[addr] ^= 0x80u;
            corrupt_next_image_program = false;
        }
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
    flash_job_show_details = false;
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
    flash_job_record = 0;
    sd_path_offsets[0] = 0;
    strcpy(sd_path_buffer, "/large.ROM");
    flash_job_file = (FIL){.data = data, .size = size, .pos = 0, .fail_at = fail_at};
    flash_job_file_open = true;
    flash_job_state = FLASH_JOB_WRITE;
    flash_job_resume_us = test_now = 0;
    ctrl_ack_value = 0;
    ctrl_mapper_value = 0;
    ctrl_cmd_state = 0x0E;
    allow_lo = flash_job_addr;
    allow_hi = flash_job_addr + flash_job_span;
    unsigned steps = 0;
    unsigned reads_before = sd_read_calls;
    unsigned programs_before = image_program_calls;
    unsigned tags_before = mapper_tag_calls;
    while (flash_job_state == FLASH_JOB_WRITE) {
        test_now = flash_job_resume_us;
        flash_job_background_work();
        if (flash_job_state == FLASH_JOB_IDLE) {
            allow_lo = allow_hi = 0;
            return false;
        }
        assert(++steps < 100000);
    }
    assert(flash_job_state == FLASH_JOB_DETECT && ctrl_mapper_value == FLASH_COPY_DETECTING);
    assert(mapper_tag_calls == tags_before);
    if (size >= 4u * 1024u * 1024u) {
        unsigned batches = (size + 2047u) / 2048u;
        assert(sd_read_calls - reads_before == batches);
        assert(image_program_calls - programs_before == batches);
        assert(steps * FLASH_JOB_YIELD_US <= (size / 4096u + size / 65536u + 32u) * 1000u);
        printf("PERF: %lu-byte copy: %u SD reads/program calls (was %lu); scheduled pauses %luus (was %luus)\n",
               (unsigned long)size, batches, (unsigned long)((size + 255u) / 256u),
               (unsigned long)(steps * FLASH_JOB_YIELD_US), (unsigned long)(steps * 3000u));
    }
    unsigned reads_after_copy = sd_read_calls;
    test_now = flash_job_resume_us;
    flash_job_background_work();
    assert(sd_read_calls == reads_after_copy);
    bool ok = flash_job_state == FLASH_JOB_COMMIT;
    if (ok) {
        assert(ctrl_mapper_value == 100);
        test_now = flash_job_resume_us;
        flash_job_background_work();
    }
    assert(flash_job_state == FLASH_JOB_IDLE);
    allow_lo = allow_hi = 0;
    return ok;
}

static void check_menu_reader(void) {
    bool mapped;
    memset(menu_image, 0x31, sizeof(menu_image));
    memset(page_buffer, 0x52, sizeof(page_buffer));
    fh_menu_window_active = false;
    ctrl_cmd_state = 0x10;
    ctrl_mapper_value = FLASH_COPY_DETECTING;
    ctrl_status_value = CTRL_MAGIC;
    total_record_count = 0x123;
    ctrl_ack_value = 0;
    assert(menu_read_byte(0x4100, &mapped) == 0x31 && mapped);
    assert(menu_read_byte(DATA_BASE_ADDR, &mapped) == 0x52 && mapped);
    assert(menu_read_byte(CTRL_CMD, &mapped) == 0x10 && mapped);
    assert(menu_read_byte(CTRL_MAPPER, &mapped) == FLASH_COPY_DETECTING && mapped);
    assert(menu_read_byte(CTRL_COUNT_L, &mapped) == 0x23 && mapped);
    assert(menu_read_byte(CTRL_COUNT_H, &mapped) == 1 && mapped);
    assert(menu_read_byte(CTRL_STATUS, &mapped) == CTRL_MAGIC && mapped);
    assert(menu_read_byte(MP3_CTRL_ELAPSED_L, &mapped) == 0x34 && mapped);
    assert(menu_read_byte(MP3_CTRL_TOTAL_H, &mapped) == 0x56 && mapped);
    flash_free_kb = 0x2345;
    assert(menu_read_byte(CTRL_FLASH_FREE_KB, &mapped) == 0x45 && mapped);
    ctrl_sd_partition_info[2] = 'V';
    assert(menu_read_byte(CTRL_SD_PARTITION_INFO_BASE + 2u, &mapped) == 'V' && mapped);
    assert(menu_read_byte(0x2000, &mapped) == 0xFF && !mapped);
    fh_menu_window_active = true;
    fh_catalog_count = 0x345;
    fh_progress_percent = 99;
    fh_result = 1;
    fh_wifi_status_text[0] = 'S';
    assert(menu_read_byte(CTRL_COUNT_L, &mapped) == 0x45 && mapped);
    assert(menu_read_byte(CTRL_MAPPER, &mapped) == 99 && mapped);
    assert(menu_read_byte(CTRL_AUDIO, &mapped) == 1 && mapped);
    assert(menu_read_byte(FH_STATUS_TEXT_BASE, &mapped) == 'S' && mapped);
    fh_menu_window_active = false;
}

static void prepare_sd_record(const char *name, const uint8_t *data, uint32_t size) {
    memset(records, 0, sizeof(records));
    snprintf(records[0].Name, sizeof(records[0].Name), "%s", name);
    records[0].Mapper = SOURCE_SD_FLAG | 8u;
    records[0].Size = size;
    total_record_count = full_record_count = 1;
    filtered_indices[0] = sd_path_offsets[0] = 0;
    strcpy(sd_path_buffer, "/large.ROM");
    sd_source_file = (FIL){.data = data, .size = size};
    ctrl_cmd_state = 0x10;
    browse_source_mode = SOURCE_MODE_SD;
    strcpy(sd_current_path, "/games");
    refresh_requested = false;
    process_copy_to_flash_request(true);
}

static void finish_copy_refresh(void) {
    assert(refresh_requested && flash_job_show_details && ctrl_cmd_state == 0x10);
    assert(browse_source_mode == SOURCE_MODE_FLASH && strcmp(sd_current_path, "/") == 0);
    ROMRecord sd_duplicate = records[0];
    memcpy(records, flash_records, sizeof(records));
    assert(flash_record_count < MAX_FLASH_RECORDS);
    records[flash_record_count] = sd_duplicate;
    records[flash_record_count].Offset = flash_job_entry.Offset;
    filtered_indices[0] = flash_record_count;
    for (uint16_t i = 0; i < flash_record_count; i++) {
        filtered_indices[i + 1u] = (uint16_t)(flash_record_count - i - 1u);
    }
    total_record_count = full_record_count = (uint16_t)(flash_record_count + 1u);
    resolve_flash_copy_match();
    assert(!flash_job_show_details && ctrl_ack_value == CTRL_MAGIC);
    assert(match_index > 0 && match_index < total_record_count);
    const ROMRecord *selected = &records[filtered_indices[match_index]];
    assert((selected->Mapper & SOURCE_SD_FLAG) == 0 && selected->Offset == flash_job_entry.Offset);
    assert(selected->Size == flash_job_entry.Size);
    ctrl_cmd_state = 0; // The background refresh clears the command only after selecting the entry.
    refresh_requested = false;
}

static void check_capacity(void) {
    uint32_t first = flash_store_align_up(sim_base() + FLASH_ROM_AREA_OFFSET, FLASH_SECTOR_SIZE);
    uint32_t capacity = PICO_FLASH_SIZE_BYTES - first;
    uint32_t mb = 1024u * 1024u;
    assert(capacity >= 10u * mb);
    memset(flash_records, 0, sizeof(flash_records));
    flash_record_count = 2;
    flash_store_overflow = false;
    flash_records[0].Offset = first - sim_base() + 2u * mb;
    flash_records[0].Size = mb;
    flash_records[1].Offset = first - sim_base() + 7u * mb;
    flash_records[1].Size = mb;
    flash_store_update_space();
    assert(flash_free_kb == (capacity - 2u * mb) / 1024u);
    uint32_t largest = capacity - 8u * mb;
    if (largest < 4u * mb) largest = 4u * mb;
    assert(flash_largest_kb == largest / 1024u);
    assert(flash_free_kb >= 8192u && flash_largest_kb < 8192u);
    uint32_t addr;
    assert(!flash_store_find_space(8u * mb, &addr));
    assert(flash_store_find_space(2u * mb, &addr) && addr == first);
    assert(!flash_store_find_space(UINT32_MAX, &addr));
    assert(!flash_store_find_space(0, &addr));

    // Unaligned and overlapping images consume the union of their sectors.
    flash_records[0].Offset = first - sim_base() + 4097u;
    flash_records[0].Size = 4097u;
    flash_records[1] = flash_records[0];
    flash_store_update_space();
    assert(flash_free_kb == (capacity - 8192u) / 1024u);
    assert(flash_largest_kb == (capacity - 12288u) / 1024u);
    assert(flash_store_find_space(4096u, &addr) && addr == first);
    assert(flash_store_find_space(8192u, &addr) && addr == first + 12288u);
    flash_store_overflow = true;
    flash_store_update_space();
    assert(flash_free_kb == UINT16_MAX && flash_largest_kb == 0);
    assert(!flash_store_find_space(4096u, &addr));
    flash_store_load_records();
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
    check_menu_reader();

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

    // An 8 MB ROM lands after the image, over stale data,
    // using 64 KB block erases where aligned.
    uint32_t big_size = 8u * 1024u * 1024u;
    uint8_t *big_rom = make_data(7, big_size);
    prepare_sd_record("Big One", big_rom, big_size);
    records[0].Mapper = SOURCE_SD_FLAG;
    assert(flash_job_start() && flash_job_entry.Size == big_size);
    assert(flash_job_entry.Mapper == 0);
    assert(flash_job_state == FLASH_JOB_START);
    flash_job_finish(false); // Preflight only; the copy below exercises writes.
    erase_64k_ops = 0;
    assert(copy_rom("Big One", 8, big_rom, big_size, 0));
    int big = find_loaded("Big One");
    assert(big >= 0 && sim_base() + (uint32_t)flash_records[big].Offset == flash_store_align_up(rom_end, FLASH_SECTOR_SIZE));
    assert(erase_64k_ops >= big_size / 65536u - 1u);
    reload();
    check_static_regions();
    check_all_data(99, "Gap Filler");
    check_data("Big One", 7);

    check_capacity();
    unsigned unchanged_pages = program_pages;
    unsigned unchanged_erases = erase_4k_ops + erase_64k_ops;
    uint16_t unchanged_slots = flash_slots_used;
    prepare_sd_record("Big One", big_rom, big_size);
    assert(flash_job_start() && flash_job_state == FLASH_JOB_MATCH);
    while (flash_job_written < big_size) assert(flash_job_match_step());
    flash_job_finish(true);
    assert(ctrl_ack_value == 0 && ctrl_cmd_state == 0x10 && refresh_requested);
    assert(records[0].Mapper == (SOURCE_SD_FLAG | 8u));
    finish_copy_refresh();
    assert(records[filtered_indices[match_index]].Offset == flash_records[big].Offset);
    assert(program_pages == unchanged_pages && erase_4k_ops + erase_64k_ops == unchanged_erases);
    assert(flash_slots_used == unchanged_slots);

    big_rom[big_size - 1u] ^= 1u;
    prepare_sd_record("Big One", big_rom, big_size);
    assert(flash_job_start() && flash_job_state == FLASH_JOB_MATCH);
    bool matches = true;
    while (matches && flash_job_written < big_size) matches = flash_job_match_step();
    assert(!matches && ctrl_mapper_value == FLASH_COPY_DUPLICATE);
    flash_job_finish(false);
    assert(ctrl_ack_value == 0 && !flash_job_show_details);
    assert(browse_source_mode == SOURCE_MODE_SD && strcmp(sd_current_path, "/games") == 0);
    big_rom[big_size - 1u] ^= 1u;
    assert(program_pages == unchanged_pages && erase_4k_ops + erase_64k_ops == unchanged_erases);

    prepare_sd_record("Too Big", big_rom, PICO_FLASH_SIZE_BYTES);
    assert(!flash_job_start() && ctrl_mapper_value == FLASH_COPY_TOO_LARGE);
    flash_job_finish(false);
    prepare_sd_record("New Large ROM", big_rom, big_size);
    flash_record_count = MAX_FLASH_RECORDS;
    assert(!flash_job_start() && ctrl_mapper_value == FLASH_COPY_NO_SPACE);
    flash_job_finish(false);
    flash_store_load_records();

    // Copying is allowed before choosing compatible mapper/audio settings on the flash screen.
    prepare_sd_record("Writable", big_rom, 5u * 1024u * 1024u);
    records[0].Mapper = SOURCE_SD_FLAG | MAPPER_ASCII16X_FR;
    assert(flash_job_start() && flash_job_entry.Mapper == MAPPER_ASCII16X_FR);
    flash_job_finish(false);

    uint32_t stored_offset = (uint32_t)flash_job_entry.Offset;
    flash_job_entry.Offset = UINT32_MAX;
    flash_job_show_details = true;
    resolve_flash_copy_match();
    assert(ctrl_ack_value == 0 && ctrl_mapper_value == FLASH_COPY_IO && match_index == 0xFFFF);
    flash_job_entry.Offset = stored_offset;

    uint32_t untyped_size = 1024u * 1024u + 37u;
    uint8_t *untyped_rom = make_data(17, untyped_size);
    untyped_rom[0] = 'A';
    untyped_rom[1] = 'B';
    memcpy(untyped_rom + 16, "ROM_NEO8", 8);
    pio_reads[0] = CTRL_CMD;
    pio_reads[1] = CTRL_MAPPER;
    pio_reads[2] = 0x4100;
    pio_reads[3] = DATA_BASE_ADDR;
    pio_count = 4;
    pio_read_pos = pio_result_count = 0;
    assert(copy_rom("Untyped", 0, untyped_rom, untyped_size, 0));
    int untyped = find_loaded("Untyped");
    assert(untyped >= 0 && flash_records[untyped].Mapper == MAPPER_DETECT_NEO8);
    assert(memcmp(sim_flash + sim_base() + flash_records[untyped].Offset, untyped_rom, untyped_size) == 0);
    for (uint32_t i = untyped_size; i < flash_store_align_up(untyped_size, FLASH_PAGE_SIZE); i++) {
        assert(sim_flash[sim_base() + flash_records[untyped].Offset + i] == 0xFFu);
    }
    assert(pio_result_count == 4);
    assert(pio_results[0] == (0xFF00u | 0x0Eu));
    assert(pio_results[1] == (0xFF00u | FLASH_COPY_DETECTING));
    assert(pio_results[2] == (0xFF00u | 0x31u));
    assert(pio_results[3] == (0xFF00u | 0x52u));
    memcpy(records, flash_records, sizeof(records));
    assert(flash_store_delete_entry((uint16_t)untyped));
    free(untyped_rom);

    uint8_t *unsupported_rom = calloc(MIN_ROM_SIZE, 1);
    assert(unsupported_rom);
    uint16_t before_count = flash_record_count, before_slots = flash_slots_used;
    assert(!copy_rom("Unsupported", 0, unsupported_rom, MIN_ROM_SIZE, 0));
    assert(flash_job_written == MIN_ROM_SIZE && ctrl_mapper_value == FLASH_COPY_MAPPER);
    assert(flash_record_count == before_count && flash_slots_used == before_slots);
    assert(find_loaded("Unsupported") < 0 && !flash_job_file_open);
    corrupt_next_image_program = true;
    assert(!copy_rom("Corrupted", 1, unsupported_rom, MIN_ROM_SIZE, 0));
    assert(!corrupt_next_image_program && ctrl_mapper_value == FLASH_COPY_IO);
    assert(flash_record_count == before_count && flash_slots_used == before_slots);
    assert(find_loaded("Corrupted") < 0 && !flash_job_file_open);
    free(unsupported_rom);

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
    printf("PASS: flash store batching/deferred flash detection/menu servicing, 8 MB import/reuse, capacity, failures and compaction "
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
