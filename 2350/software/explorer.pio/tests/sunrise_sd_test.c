// Production SD backend and extracted menu/options functions, with sparse card
// sectors and FatFs mocks. Each audio yield ends one Core 1 service iteration.
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define __not_in_flash_func(name) name
#include "diskio.h"
#include "sd_card.h"
#include "sunrise_sd.h"
#include "sunrise-sd-types.h"

volatile bool usb_device_mounted, usb_read_requested, usb_write_requested;
volatile uint32_t usb_read_lba, usb_write_lba;
uint8_t usb_write_buffer[512];

static jmp_buf loop_return;
static sd_card_t card;
static uint32_t card_sectors, info_blocks, info_block_size;
static uint32_t window_base, window_count, last_read_lba, last_write_lba;
static uint32_t failed_read_lba;
static unsigned read_calls, write_calls;
static DSTATUS init_status;
static DRESULT write_result;
static bool card_present;
static sunrise_ide_t ide;
static struct { uint32_t lba; uint8_t data[512]; } sectors[12];
static unsigned sector_entries;

static uint8_t *card_sector(uint32_t lba)
{
    for (unsigned i = 0; i < sector_entries; i++)
        if (sectors[i].lba == lba) return sectors[i].data;
    assert(sector_entries < sizeof(sectors) / sizeof(sectors[0]));
    sectors[sector_entries].lba = lba;
    memset(sectors[sector_entries].data, 0, 512);
    return sectors[sector_entries++].data;
}

sd_card_t *sd_get_by_num(unsigned num) { assert(num == 0); return card_present ? &card : NULL; }
static uint32_t get_num_sectors(sd_card_t *sd) { assert(sd == &card); return card_sectors; }
DSTATUS disk_initialize(BYTE pdrv) { assert(pdrv == 0); return init_status; }
void disk_set_partition_window(LBA_t base, LBA_t count) { window_base = base; window_count = count; }

DRESULT disk_read_raw(BYTE pdrv, BYTE *buff, LBA_t lba, UINT count)
{
    assert(pdrv == 0 && count == 1 && lba < card_sectors);
    read_calls++;
    last_read_lba = lba;
    if (lba == failed_read_lba) return RES_ERROR;
    memcpy(buff, card_sector(lba), 512);
    return RES_OK;
}

DRESULT disk_write_raw(BYTE pdrv, const BYTE *buff, LBA_t lba, UINT count)
{
    assert(pdrv == 0 && count == 1 && lba < card_sectors);
    write_calls++;
    last_write_lba = lba;
    if (write_result != RES_OK) return write_result;
    memcpy(card_sector(lba), buff, 512);
    return RES_OK;
}

void ext_str(size_t len, const uint8_t *cid, unsigned high, unsigned low, size_t size, char *out)
{
    assert(len == 16 && cid == card.state.CID);
    const char *value = high == 119 && low == 104 ? "PV" : "SD001";
    assert(strlen(value) < size);
    strcpy(out, value);
}

uint32_t ext_bits16(const uint8_t *cid, unsigned high, unsigned low)
{
    assert(cid == card.state.CID && high >= low);
    return low == 60 ? 1u : 0u;
}

void sunrise_ide_set_device_info(uint32_t blocks, uint32_t size,
                                const char *vendor, const char *product, const char *revision)
{
    assert(strcmp(vendor, "PV") == 0 && strcmp(product, "SD001") == 0 && strcmp(revision, "1.0") == 0);
    info_blocks = blocks;
    info_block_size = size;
}

void service_system_audio(void) { longjmp(loop_return, 1); }

#include "sunrise_sd.c"

typedef struct { uint8_t Mapper; } ROMRecord;
typedef struct { unsigned unused; } FIL;
typedef unsigned FRESULT;
#define FR_OK 0u
#define FR_NO_FILE 1u
#define FA_READ 1u
#define FA_WRITE 2u
#define FA_CREATE_ALWAYS 4u

static uint8_t ctrl_ack_value, ctrl_mapper_value, ctrl_wavegame_rom, ctrl_audio_selection;
static uint8_t ctrl_psg_emulation, ctrl_wifi_support, ctrl_sd_partition;
static uint8_t ctrl_audio_volume, ctrl_vdp_frequency, ctrl_cpu_mode;
static uint8_t ctrl_sd_partition_info[CTRL_SD_PARTITION_INFO_SIZE], sd_partition_mask;
static uint8_t sd_browse_partition, sd_mounted_partition, saved_browse_partition;
static uint8_t browse_source_mode;
static bool sd_mounted, has_filesystem, fh_menu_window_active, refresh_requested;
// State the firmware's unmount and partition-cycle code also clears.
static bool fh_download_dir_loaded, move_src_valid;
static sd_card_t *sd_card;
static ROMRecord records[1];
static uint16_t total_record_count, filtered_indices[1];
static uint8_t filter_query[32], pvc[PVC_OPTIONS_SIZE];
static unsigned pvc_size;
static bool pvc_exists, pvc_writable;
static FRESULT pvc_close_result;

static bool sd_mount_partition(const sunrise_sd_partition_t *parts, uint8_t count, uint8_t number);
static void sd_unmount_card(void);
static bool sd_prepare_card(void)
{
    sd_card = sd_get_by_num(0);
    return sd_card && disk_initialize(0) == 0;
}
static bool sd_mount_card(void)
{
    if (sd_mounted) return true;
    if (!has_filesystem || !sd_prepare_card()) return false;
    sunrise_sd_partition_t parts[SD_BROWSE_PARTITION_MAX];
    uint8_t count = sunrise_sd_list_supported_partitions(parts, SD_BROWSE_PARTITION_MAX);
    return count && sd_mount_partition(parts, count, sd_browse_partition);
}
static void quiesce_mp3_core1_before_sd_work(void) {}
static uint16_t query_filtered_index(void) { return 0; }
static bool wavegame_assets_detected_for_record(uint16_t index) { assert(index == 0); return false; }
static bool build_pvc_options_path(uint16_t index, char *path, size_t size)
{
    assert(index == 0 && size > strlen("/Nextor.flash.PVC"));
    strcpy(path, "/Nextor.flash.PVC");
    return true;
}
static FRESULT f_open(FIL *fil, const char *path, unsigned mode)
{
    (void)fil;
    assert(strcmp(path, "/Nextor.flash.PVC") == 0);
    if (mode & FA_CREATE_ALWAYS) {
        if (!pvc_writable) return FR_NO_FILE;
        pvc_exists = true;
        pvc_size = 0;
    }
    return pvc_exists ? FR_OK : FR_NO_FILE;
}
static FRESULT f_read(FIL *fil, void *buff, UINT count, UINT *read)
{
    (void)fil;
    *read = pvc_size < count ? pvc_size : count;
    memcpy(buff, pvc, *read);
    return FR_OK;
}
static FRESULT f_write(FIL *fil, const void *buff, UINT count, UINT *written)
{
    (void)fil;
    assert(count <= sizeof(pvc));
    memcpy(pvc, buff, count);
    pvc_size = count;
    *written = count;
    return FR_OK;
}
static FRESULT f_close(FIL *fil) { (void)fil; return pvc_close_result; }
static FRESULT f_mount(FATFS *fs, const char *path, unsigned immediate)
{
    assert(path[0] == '\0' && (fs ? immediate == 1 : immediate == 0));
    return FR_OK;
}
static void sd_activity_note(void) {}
static uint32_t pvc_options_fattime(uint16_t index) { assert(index == 0); return 0; }
static void fatfs_set_fattime_override(uint32_t value) { assert(value == 0); }
static void cancel_refresh_work(void) { refresh_requested = false; }
static void sd_save_browse_partition(uint8_t number) { saved_browse_partition = number; }
static void set_root_path(void) {}

#include "sunrise-sd-options.h"

static void write_le32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8u * i));
}

static void add_partition(unsigned number, uint8_t type, uint32_t start, uint32_t count, const char *label)
{
    uint8_t *mbr = card_sector(0);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    uint8_t *entry = mbr + 446 + 16u * (number - 1u);
    entry[4] = type;
    write_le32(entry + 8, start);
    write_le32(entry + 12, count);
    uint8_t *boot = card_sector(start);
    boot[510] = 0x55;
    boot[511] = 0xAA;
    unsigned label_offset = type == 0x0Cu ? 71u : 43u;
    memcpy(boot + (type == 0x0Cu ? 82u : 54u), type == 0x0Cu ? "FAT32" : "FAT16", 5);
    memset(boot + label_offset, ' ', 11);
    memcpy(boot + label_offset, label, strlen(label));
}

static void reset_state(void)
{
    usb_device_mounted = usb_read_requested = usb_write_requested = false;
    usb_read_lba = usb_write_lba = 0;
    sd_ide_ctx = NULL;
    sd_device_mounted = false;
    sd_block_count = sd_lba_base = sd_selected_partition = 0;
    memset(&ide, 0, sizeof(ide));
    memset(sectors, 0, sizeof(sectors));
    sector_entries = 0;
    card.get_num_sectors = get_num_sectors;
    card_present = true;
    card_sectors = 0x08000000u;
    info_blocks = info_block_size = window_base = window_count = 0;
    read_calls = write_calls = 0;
    failed_read_lba = UINT32_MAX;
    write_result = RES_OK;
    init_status = 0;
    sd_card = &card;
    sd_mounted = false;
    has_filesystem = true;
    sd_browse_partition = 1;
    sd_mounted_partition = saved_browse_partition = 0;
    pvc_exists = false;
    pvc_writable = true;
    pvc_close_result = FR_OK;
    pvc_size = 0;
    total_record_count = 1;
    filtered_indices[0] = 0;
    records[0].Mapper = MAPPER_SUNRISE_SD;
    ctrl_sd_partition = 0;
    memset(filter_query, 0, sizeof(filter_query));
    add_partition(1, 0x06, 2048u, 4096u, "GAMES");
    add_partition(2, 0x0C, 10000u, 8192u, "BROWSE");
    add_partition(3, 0x06, 32768u, 8192u, "NEXTOR");
    uint8_t *extended = card_sector(0) + 446 + 48;
    extended[4] = 0x0F;
    write_le32(extended + 8, 50000u);
    write_le32(extended + 12, 8192u);
    uint8_t *ebr = card_sector(50000u);
    ebr[510] = 0x55;
    ebr[511] = 0xAA;
    ebr[446 + 4] = 0x06;
    write_le32(ebr + 446 + 8, 32u);
    write_le32(ebr + 446 + 12, 4096u);
    memcpy(card_sector(50032u), card_sector(2048u), 512);
}

static void run_iteration(void)
{
    if (setjmp(loop_return) == 0) sunrise_sd_task();
}

static void attach(uint8_t selection)
{
    sunrise_sd_select_partition(selection);
    sunrise_sd_set_ide_ctx(&ide);
    run_iteration();
    assert(usb_device_mounted && info_block_size == 512u);
}

static void set_lba(uint32_t lba)
{
    ide.sector = (uint8_t)lba;
    ide.cylinder_low = (uint8_t)(lba >> 8);
    ide.cylinder_high = (uint8_t)(lba >> 16);
    ide.device_head = (uint8_t)(ATA_DEV_HEAD_LBA | ((lba >> 24) & ATA_DEV_HEAD_HEAD_MASK));
}

static uint32_t get_lba(void)
{
    return (uint32_t)ide.sector | ((uint32_t)ide.cylinder_low << 8) |
           ((uint32_t)ide.cylinder_high << 16) |
           ((uint32_t)(ide.device_head & ATA_DEV_HEAD_HEAD_MASK) << 24);
}

static void test_options_and_cycle(void)
{
    reset_state();
    refresh_sunrise_partition_info(0);
    assert(ctrl_sd_partition == 1 && ctrl_sd_partition_info[0] == 3 && ctrl_sd_partition_info[1] == 0x15);
    assert(strcmp((char *)ctrl_sd_partition_info + 2, "GAMES") == 0);
    write_partition_control(SD_PART_WHOLE_DISK);
    assert(ctrl_sd_partition == SD_PART_WHOLE_DISK && strcmp((char *)ctrl_sd_partition_info + 2, "WHOLE DISK") == 0);
    refresh_sunrise_partition_info(3);
    assert(ctrl_sd_partition == 3 && strcmp((char *)ctrl_sd_partition_info + 2, "NEXTOR") == 0);
    refresh_sunrise_partition_info(4);
    assert(ctrl_sd_partition == 1);

    for (unsigned mask = 0; mask < 32; mask++) {
        sd_partition_mask = (uint8_t)mask;
        for (unsigned current = 0; current <= 6; current++) {
            unsigned first = 0, last = 0, above = 0, below = 0;
            for (unsigned p = 1; p <= SD_PART_WHOLE_DISK; p++) {
                if (!(mask & (1u << (p - 1u)))) continue;
                if (!first) first = p;
                last = p;
                if (p > current && !above) above = p;
                if (p < current) below = p;
            }
            unsigned right = above ? above : first;
            unsigned left = below ? below : last;
            assert(next_sd_partition((uint8_t)current, 1) == (right ? right : current));
            assert(next_sd_partition((uint8_t)current, -1) == (left ? left : current));
        }
    }
    memset(card_sector(0) + 446, 0, 48);
    refresh_sunrise_partition_info(0);
    assert(ctrl_sd_partition == SD_PART_WHOLE_DISK && ctrl_sd_partition_info[0] == 1 && ctrl_sd_partition_info[1] == 0x10);
    card_present = false;
    refresh_sunrise_partition_info(SD_PART_WHOLE_DISK);
    assert(ctrl_sd_partition == 0 && ctrl_sd_partition_info[0] == 0 && ctrl_sd_partition_info[1] == 0);
    puts("PASS: primary/default and Whole Disk labels; exhaustive left/right cycling; logical-only and missing cards");
}

static void test_persistence_and_blank_card(void)
{
    reset_state();
    const uint8_t mappers[] = { MAPPER_SUNRISE_SD, MAPPER_SUNRISE_MAPPER_SD, MAPPER_C2_SD, MAPPER_MEGARAM_SD };
    for (unsigned i = 0; i < sizeof(mappers); i++) {
        records[0].Mapper = mappers[i];
        filter_query[2] = 4;
        filter_query[3] = 1;
        filter_query[4] = mappers[i];
        filter_query[5] = SD_PART_WHOLE_DISK;
        filter_query[6] = 130;
        process_save_options_request();
        uint8_t expected[] = { 'P', 'V', 'C', '1', 4, 1, mappers[i], SD_PART_WHOLE_DISK, 130, 0, 0, 0 };
        assert(ctrl_ack_value == 1 && pvc_size == sizeof(expected) && memcmp(pvc, expected, sizeof(expected)) == 0);
        ctrl_sd_partition = 1;
        process_load_options_request();
        assert(ctrl_ack_value == 1 && ctrl_sd_partition == SD_PART_WHOLE_DISK && ctrl_audio_volume == 130);
    }
    pvc[7] = 3;
    process_load_options_request();
    assert(ctrl_sd_partition == 3);
    pvc[7] = 0;
    process_load_options_request();
    assert(ctrl_sd_partition == 1);
    pvc[7] = 255;
    process_load_options_request();
    assert(ctrl_sd_partition == 1);
    pvc_size = PVC_OPTIONS_LEGACY_SIZE;
    process_load_options_request();
    assert(ctrl_sd_partition == 1);
    filter_query[5] = SD_PART_WHOLE_DISK;
    pvc_close_result = FR_NO_FILE;
    process_save_options_request();
    assert(ctrl_ack_value == 0);

    sd_unmount_card();
    has_filesystem = false;
    memset(card_sector(0), 0, 512);
    process_load_options_request();
    assert(ctrl_ack_value == 1 && ctrl_sd_partition == SD_PART_WHOLE_DISK && ctrl_sd_partition_info[0] == 1);
    assert(strcmp((char *)ctrl_sd_partition_info + 2, "WHOLE DISK") == 0);
    process_save_options_request();
    assert(ctrl_ack_value == 0);
    card_present = false;
    process_load_options_request();
    assert(ctrl_ack_value == 0 && ctrl_sd_partition_info[0] == 0);
    card_present = true;
    card_sectors = 0;
    process_load_options_request();
    assert(ctrl_ack_value == 0 && ctrl_sd_partition_info[0] == 0);
    puts("PASS: exact PVC bytes and reload across SD profiles; old defaults; failed saves; blank and zero-capacity cards");

    reset_state();
    records[0].Mapper = MAPPER_PICOAMP;
    filter_query[4] = MAPPER_PICOAMP;
    filter_query[6] = 200;
    filter_query[9] = 1;
    process_save_options_request();
    assert(ctrl_ack_value == 1 && pvc[8] == 200 && pvc[11] == 1);
    process_load_options_request();
    assert(ctrl_ack_value == 1 && ctrl_wifi_support == 1 && ctrl_audio_volume == 200);
    pvc[6] = 6;
    process_load_options_request();
    assert(records[0].Mapper == MAPPER_PICOAMP);
    pvc_size = PVC_OPTIONS_LEGACY_SIZE;
    process_load_options_request();
    assert(ctrl_wifi_support == 0);
    puts("PASS: PicoAmp saved WiFi/200% volume, legacy defaults and fixed system mapper");
}

static void test_raw_and_partition_io(void)
{
    reset_state();
    assert(sd_mount_card() && window_base == 2048u);
    read_calls = 0;
    attach(SD_PART_WHOLE_DISK);
    assert(read_calls == 0 && sd_lba_base == 0 && info_blocks == card_sectors);
    usb_read_requested = true;
    usb_read_lba = 0;
    run_iteration();
    assert(last_read_lba == 0 && memcmp(ide.sector_buffer, card_sector(0), 512) == 0);
    assert(ide.state == IDE_STATE_READ_DATA && ide.buffer_length == 512 && ide.buffer_index == 0);
    assert(ide.status == (ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ));
    memcpy(usb_write_buffer, card_sector(0), 512);
    usb_write_buffer[446] = 0x80;
    set_lba(0);
    ide.sectors_remaining = 1;
    usb_write_lba = 0;
    usb_write_requested = true;
    run_iteration();
    assert(last_write_lba == 0 && card_sector(0)[446] == 0x80 && get_lba() == 1);
    assert(ide.state == IDE_STATE_WRITE_DATA && (ide.status & ATA_STATUS_DRQ));
    ide.sectors_remaining = 0;
    usb_write_lba = 1;
    usb_write_requested = true;
    run_iteration();
    assert(last_write_lba == 1 && get_lba() == 2 && ide.state == IDE_STATE_IDLE);
    usb_read_lba = 0;
    usb_read_requested = true;
    run_iteration();
    assert(last_read_lba == 0 && memcmp(ide.sector_buffer, usb_write_buffer, 512) == 0);
    usb_read_lba = card_sectors - 1u;
    usb_read_requested = true;
    run_iteration();
    assert(last_read_lba == card_sectors - 1u);
    ide.usb_identify_pending = true;
    run_iteration();
    assert(!ide.usb_identify_pending && read_le32(ide.sector_buffer + 120) == card_sectors);
    memset(usb_write_buffer, 0xA5, sizeof(usb_write_buffer));
    set_lba(card_sectors - 1u);
    usb_write_lba = card_sectors - 1u;
    usb_write_requested = true;
    run_iteration();
    assert(last_write_lba == card_sectors - 1u && memcmp(card_sector(last_write_lba), usb_write_buffer, 512) == 0);
    assert(get_lba() == card_sectors && ide.state == IDE_STATE_IDLE && ide.status == (ATA_STATUS_DRDY | ATA_STATUS_DSC));

    unsigned reads = read_calls, writes = write_calls;
    usb_read_lba = usb_write_lba = card_sectors;
    usb_read_requested = usb_write_requested = true;
    run_iteration();
    assert(read_calls == reads && write_calls == writes && ide.error == ATA_ERROR_ABRT && ide.state == IDE_STATE_IDLE);
    failed_read_lba = 40960u;
    usb_read_lba = failed_read_lba;
    usb_read_requested = true;
    run_iteration();
    assert(ide.status == (ATA_STATUS_DRDY | ATA_STATUS_ERR) && ide.error == ATA_ERROR_ABRT);
    write_result = RES_WRPRT;
    usb_write_lba = 40960u;
    usb_write_requested = true;
    run_iteration();
    assert(ide.status == (ATA_STATUS_DRDY | ATA_STATUS_ERR) && ide.error == ATA_ERROR_ABRT);

    reset_state();
    attach(3);
    assert(sd_lba_base == 32768u && info_blocks == 8192u);
    usb_read_lba = 0;
    usb_read_requested = true;
    run_iteration();
    assert(last_read_lba == 32768u && memcmp(ide.sector_buffer, card_sector(32768u), 512) == 0);
    set_lba(8190u);
    ide.sectors_remaining = 1;
    usb_write_lba = 8190u;
    usb_write_requested = true;
    run_iteration();
    assert(last_write_lba == 40958u && get_lba() == 8191u && ide.state == IDE_STATE_WRITE_DATA && (ide.status & ATA_STATUS_DRQ));
    ide.sectors_remaining = 0;
    usb_write_lba = 8191u;
    usb_write_requested = true;
    run_iteration();
    assert(last_write_lba == 40959u && get_lba() == 8192u && ide.state == IDE_STATE_IDLE);
    writes = write_calls;
    usb_write_lba = 8192u;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == writes && ide.error == ATA_ERROR_ABRT);
    reset_state();
    attach(0);
    assert(sd_lba_base == 2048u && info_blocks == 4096u);
    puts("PASS: full-card capacity/IDENTIFY, raw MBR and final sector, bounds/errors, unchanged partition offsets and write handshakes");
}

static void test_browsing_independence(void)
{
    reset_state();
    sunrise_sd_partition_t parts[SD_BROWSE_PARTITION_MAX];
    uint8_t count = sunrise_sd_list_supported_partitions(parts, SD_BROWSE_PARTITION_MAX);
    assert(count == 4 && parts[0].number == 1 && parts[1].number == 2 && parts[2].number == 3 && parts[3].number == 5);
    assert(sd_partition_supported(parts, count, 5));
    assert(parts[1].type == SUNRISE_SD_FS_FAT32);
    sd_browse_partition = 3;
    assert(sd_mount_card() && window_base == 32768u);
    write_partition_control(SD_PART_WHOLE_DISK);
    assert(sd_browse_partition == 3 && sd_mounted_partition == 3 && window_base == 32768u);
    attach(SD_PART_WHOLE_DISK);
    process_cycle_sd_partition_request();
    assert(ctrl_ack_value == 1 && refresh_requested && !fh_menu_window_active);
    assert(sd_browse_partition == 5 && saved_browse_partition == 5 && sd_mounted_partition == 5 && window_base == 50032u);
    assert(ctrl_sd_partition == SD_PART_WHOLE_DISK && sd_lba_base == 0 && sd_block_count == card_sectors);
    process_cycle_sd_partition_request();
    assert(sd_browse_partition == 1 && window_base == 2048u);
    process_cycle_sd_partition_request();
    assert(sd_browse_partition == 2 && sd_mounted_partition == 2 && window_base == 10000u && window_count == 8192u);
    assert(ctrl_sd_partition == SD_PART_WHOLE_DISK && info_blocks == card_sectors);
    puts("PASS: F2 partition enumeration, cycling, saved browsing selection and FatFs windows stay independent of Whole Disk");
}

int main(void)
{
    test_options_and_cycle();
    test_persistence_and_blank_card();
    test_raw_and_partition_io();
    test_browsing_independence();
    return 0;
}
