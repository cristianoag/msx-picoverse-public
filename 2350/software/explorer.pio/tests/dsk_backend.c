// Host tests for the Sunrise IDE .DSK backend (pico/explorer/storage/sunrise_dsk.c).
// The production file is compiled as-is against stub Pico/FatFs headers; each
// service_system_audio() call ends one iteration of the Core 1 loop.
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define __not_in_flash_func(name) name
#include "diskio.h"
#include "sunrise_ide.h"

volatile bool usb_device_mounted;
volatile bool usb_read_requested;
volatile uint32_t usb_read_lba;
volatile bool usb_write_requested;
volatile uint32_t usb_write_lba;
uint8_t usb_write_buffer[512];

static jmp_buf loop_return;
static int init_status;
static int write_result;
static unsigned init_calls, write_calls;
static uint32_t last_write_lba;
static uint32_t info_blocks, info_block_size;

void service_system_audio(void) { longjmp(loop_return, 1); }

DSTATUS disk_initialize(BYTE pdrv)
{
    assert(pdrv == 0);
    init_calls++;
    return (DSTATUS)init_status;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    assert(pdrv == 0 && buff == usb_write_buffer && count == 1);
    write_calls++;
    last_write_lba = sector;
    return (DRESULT)write_result;
}

void sunrise_ide_set_device_info(uint32_t block_count, uint32_t block_size,
                                const char *vendor, const char *product, const char *revision)
{
    // sunrise_ide.c copies these into 8/16/4-byte SCSI INQUIRY fields and
    // builds the IDENTIFY model from "vendor product": nothing may be cut.
    assert(vendor && strlen(vendor) <= 8);
    assert(product && strlen(product) <= 16 && strcmp(product, "PICOVERSE DSK") == 0);
    assert(revision && strlen(revision) <= 4);
    info_blocks = block_count;
    info_block_size = block_size;
}

#include "sunrise_dsk.c"

#define SECTORS 1440u
static uint8_t image[SECTORS * 512u];
static sunrise_ide_t ide;

// Two fragments: image sectors 0-63 at card LBA 1000, 64-1439 at LBA 5000.
static const sunrise_dsk_extent_t extents[] = {
    { 0u, 1000u, 64u },
    { 64u, 5000u, SECTORS - 64u },
};

static void reset_state(void)
{
    usb_device_mounted = usb_read_requested = usb_write_requested = false;
    usb_read_lba = usb_write_lba = 0;
    init_status = 0;
    write_result = RES_OK;
    init_calls = write_calls = 0;
    last_write_lba = 0;
    info_blocks = info_block_size = 0;
    memset(&ide, 0, sizeof(ide));
    for (uint32_t i = 0; i < sizeof(image); i++)
        image[i] = (uint8_t)(i / 512u);
    dsk_ide_ctx = NULL;
    dsk_image = NULL;
    dsk_sector_count = 0;
    dsk_file_base = 0;
    dsk_extents = NULL;
    dsk_extent_count = 0;
    dsk_writable = false;
}

// Each call runs the task prologue (idempotent for these tests) and exactly one
// loop iteration, which ends at service_system_audio().
static void run_iteration(void)
{
    if (setjmp(loop_return) == 0)
        sunrise_dsk_task();
}

static void start_task(void)
{
    run_iteration();
}

static void set_lba(uint32_t lba)
{
    ide.sector = (uint8_t)lba;
    ide.cylinder_low = (uint8_t)(lba >> 8);
    ide.cylinder_high = (uint8_t)(lba >> 16);
    ide.device_head = (uint8_t)(0x40u | ((lba >> 24) & 0x0Fu));
}

static uint32_t get_lba(void)
{
    return (uint32_t)ide.sector | ((uint32_t)ide.cylinder_low << 8) |
           ((uint32_t)ide.cylinder_high << 16) | ((uint32_t)(ide.device_head & 0x0Fu) << 24);
}

static void test_mount_and_read(void)
{
    reset_state();
    sunrise_dsk_set_ide_ctx(&ide);
    sunrise_dsk_attach_image(image, SECTORS, 0, extents, 2);
    start_task();
    assert(init_calls == 1 && usb_device_mounted);
    assert(info_blocks == SECTORS && info_block_size == 512u);

    usb_read_lba = 700;
    usb_read_requested = true;
    run_iteration();
    assert(!usb_read_requested);
    assert(ide.state == IDE_STATE_READ_DATA && ide.buffer_length == 512u && ide.buffer_index == 0);
    assert(ide.status == (ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ));
    assert(memcmp(ide.sector_buffer, image + 700u * 512u, 512u) == 0);

    usb_read_lba = SECTORS;
    usb_read_requested = true;
    run_iteration();
    assert(ide.state == IDE_STATE_IDLE && (ide.status & ATA_STATUS_ERR) && ide.error == ATA_ERROR_ABRT);
    puts("PASS: mount publishes image size; reads come from the PSRAM copy; out-of-range read aborts");
}

static void test_write_through(void)
{
    reset_state();
    sunrise_dsk_set_ide_ctx(&ide);
    sunrise_dsk_attach_image(image, SECTORS, 0, extents, 2);
    start_task();

    // Multi-sector write crossing the extent boundary (63 -> 64).
    set_lba(63);
    ide.sectors_remaining = 1; // the IDE front-end already decremented for sector 63
    memset(usb_write_buffer, 0xA5, sizeof(usb_write_buffer));
    usb_write_lba = 63;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == 1 && last_write_lba == 1000u + 63u);
    assert(image[63u * 512u] == 0xA5 && image[63u * 512u + 511u] == 0xA5);
    assert(get_lba() == 64u && ide.state == IDE_STATE_WRITE_DATA);
    assert(ide.status == (ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ));

    ide.sectors_remaining = 0;
    memset(usb_write_buffer, 0x5A, sizeof(usb_write_buffer));
    usb_write_lba = 64;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == 2 && last_write_lba == 5000u);
    assert(image[64u * 512u] == 0x5A);
    assert(get_lba() == 65u && ide.state == IDE_STATE_IDLE);
    assert(ide.status == (ATA_STATUS_DRDY | ATA_STATUS_DSC));

    // A failed card write must leave the PSRAM copy untouched.
    write_result = RES_ERROR;
    memset(usb_write_buffer, 0x11, sizeof(usb_write_buffer));
    usb_write_lba = 100;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == 3 && last_write_lba == 5000u + 36u);
    assert(image[100u * 512u] == 100u);
    assert(ide.state == IDE_STATE_IDLE && (ide.status & ATA_STATUS_ERR));
    puts("PASS: writes map through both extents, update PSRAM only after the card write, advance LBA");
}

static void test_read_only(void)
{
    reset_state();
    sunrise_dsk_set_ide_ctx(&ide);
    sunrise_dsk_attach_image(image, SECTORS, 0, NULL, 0);
    start_task();
    assert(init_calls == 0 && usb_device_mounted);

    memset(usb_write_buffer, 0xEE, sizeof(usb_write_buffer));
    usb_write_lba = 10;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == 0 && image[10u * 512u] == 10u);
    assert(ide.state == IDE_STATE_IDLE && (ide.status & ATA_STATUS_ERR) && ide.error == ATA_ERROR_ABRT);

    // A card that fails to re-initialise on Core 1 also leaves the image read-only.
    reset_state();
    init_status = STA_NOINIT;
    sunrise_dsk_set_ide_ctx(&ide);
    sunrise_dsk_attach_image(image, SECTORS, 0, extents, 2);
    start_task();
    assert(init_calls == 1 && usb_device_mounted);
    usb_write_lba = 10;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == 0 && (ide.status & ATA_STATUS_ERR));

    // Extent tables past the fixed limit are rejected rather than trusted.
    reset_state();
    sunrise_dsk_attach_image(image, SECTORS, 0, extents, SUNRISE_DSK_MAX_EXTENTS + 1u);
    assert(dsk_extent_count == 0 && !dsk_writable);
    puts("PASS: read-only images, failed card init and oversized extent tables reject writes");
}

static void test_identify_pending(void)
{
    reset_state();
    sunrise_dsk_set_ide_ctx(&ide);
    sunrise_dsk_attach_image(image, SECTORS, 0, NULL, 0);
    start_task();
    ide.usb_identify_pending = true;
    run_iteration();
    const uint16_t *w = (const uint16_t *)ide.sector_buffer;
    assert(!ide.usb_identify_pending && ide.state == IDE_STATE_READ_DATA);
    assert(w[0] == 0x0040u && w[1] == 1u && w[49] == 0x0200u);
    assert(((uint32_t)w[61] << 16 | w[60]) == SECTORS);
    assert(w[27] == (('P' << 8) | 'i'));
    puts("PASS: pending IDENTIFY completes with the image sector count");
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// Stamps a boot sector with a BPB total sector count and a FAT media byte.
static void stamp_disk(uint8_t *disk, uint16_t bpb_total, uint8_t media)
{
    disk[0x0B] = 0x00; disk[0x0C] = 0x02;               // 512 bytes per sector
    disk[0x13] = (uint8_t)bpb_total; disk[0x14] = (uint8_t)(bpb_total >> 8);
    disk[512] = media;                                   // first FAT byte
}

#define MULTI_FILE_SECTORS (1440u + 720u + 1440u)
static uint8_t multi[(SUNRISE_DSK_HEADER_SECTORS + MULTI_FILE_SECTORS) * 512u];

static void test_split_disks(void)
{
    uint16_t sizes[SUNRISE_DSK_MAX_DISKS];
    static uint8_t buf[MULTI_FILE_SECTORS * 512u];

    // BPB decides: 720KB + 360KB + 720KB.
    memset(buf, 0, sizeof(buf));
    stamp_disk(buf, 1440, 0xF9);
    stamp_disk(buf + 1440u * 512u, 720, 0xF8);
    stamp_disk(buf + 2160u * 512u, 1440, 0xF9);
    assert(sunrise_dsk_split_disks(buf, MULTI_FILE_SECTORS, sizes, SUNRISE_DSK_MAX_DISKS) == 3);
    assert(sizes[0] == 1440 && sizes[1] == 720 && sizes[2] == 1440);

    // No BPB: the FAT media byte decides (F8h = 360KB, F9h = 720KB).
    memset(buf, 0, sizeof(buf));
    buf[512] = 0xF8;
    buf[720u * 512u + 512u] = 0xF9;
    buf[2160u * 512u + 512u] = 0xF8;
    buf[2880u * 512u + 512u] = 0xF8;
    assert(sunrise_dsk_split_disks(buf, MULTI_FILE_SECTORS, sizes, SUNRISE_DSK_MAX_DISKS) == 4);
    assert(sizes[0] == 720 && sizes[1] == 1440 && sizes[2] == 720 && sizes[3] == 720);

    // Neither: split evenly, as 720KB disks when what is left allows it.
    memset(buf, 0, sizeof(buf));
    assert(sunrise_dsk_split_disks(buf, 2880, sizes, SUNRISE_DSK_MAX_DISKS) == 2);
    assert(sizes[0] == 1440 && sizes[1] == 1440);
    assert(sunrise_dsk_split_disks(buf, 2160, sizes, SUNRISE_DSK_MAX_DISKS) == 2);
    assert(sizes[0] == 720 && sizes[1] == 1440);

    // A BPB claiming more than is left is ignored.
    memset(buf, 0, sizeof(buf));
    stamp_disk(buf, 1440, 0xF9);
    stamp_disk(buf + 1440u * 512u, 1440, 0x00);
    assert(sunrise_dsk_split_disks(buf, 2160, sizes, SUNRISE_DSK_MAX_DISKS) == 2);
    assert(sizes[0] == 1440 && sizes[1] == 720);

    // Single disks, bad sizes and too many disks.
    memset(buf, 0, sizeof(buf));
    assert(sunrise_dsk_split_disks(buf, 720, sizes, SUNRISE_DSK_MAX_DISKS) == 1 && sizes[0] == 720);
    assert(sunrise_dsk_split_disks(buf, 1441, sizes, SUNRISE_DSK_MAX_DISKS) == 0);
    assert(sunrise_dsk_split_disks(buf, 0, sizes, SUNRISE_DSK_MAX_DISKS) == 0);
    assert(sunrise_dsk_split_disks(buf, 2880, sizes, 1) == 0);
    puts("PASS: joined images split by BPB, then FAT media byte, then evenly; invalid sets rejected");
}

static void test_emulation_header(void)
{
    const uint16_t sizes[] = { 1440, 720, 1440 };
    uint8_t header[SUNRISE_DSK_HEADER_SECTORS * 512u];
    memset(header, 0xEE, sizeof(header));
    sunrise_dsk_build_emulation_header(header, sizes, 3);

    // Nextor 2.1 persistent emulation pointer (first partition table entry).
    const uint8_t *entry = header + 0x1BE;
    assert(entry[0] & 0x01);                            // enter disk emulation mode
    assert(entry[4] != 0);                              // partition type must be non-zero
    assert(entry[1] == 1 && entry[2] == 1);             // device 1, LUN 1
    uint32_t emu_sector = (uint32_t)entry[3] << 24 | (uint32_t)entry[5] << 16 | (uint32_t)entry[6] << 8 | entry[7];
    assert(emu_sector == 1);
    // The same entry maps disk 1 for a normal (non-emulation) boot.
    assert(rd32(entry + 8) == SUNRISE_DSK_HEADER_SECTORS && rd32(entry + 12) == 1440);
    assert(header[510] == 0x55 && header[511] == 0xAA);
    for (unsigned i = 0x1CE; i < 510; i++) assert(header[i] == 0);   // other entries empty

    const uint8_t *emu = header + 512u * emu_sector;
    assert(memcmp(emu, "NEXTOR_EMU_DATA", 16) == 0);
    assert(emu[16] == 3 && emu[17] == 1);               // 3 disks, boot disk 1
    assert(rd16(emu + 18) == 0 && rd32(emu + 20) == 0); // Nextor allocates the work area
    uint32_t expected_start = SUNRISE_DSK_HEADER_SECTORS;
    for (unsigned i = 0; i < 3; i++) {
        const uint8_t *slot = emu + 24 + i * 8;
        assert(slot[0] == 0);                           // same device as the emulation data
        assert(rd32(slot + 2) == expected_start && rd16(slot + 6) == sizes[i]);
        expected_start += sizes[i];
    }
    for (unsigned i = 24 + 3 * 8; i < 512; i++) assert(emu[i] == 0);
    puts("PASS: header carries the Nextor 2.1 persistent pointer, maps disk 1, and lists each disk's start/size");
}

static void test_multi_disk_device(void)
{
    const uint16_t sizes[] = { 1440, 720, 1440 };
    // File sectors 0-99 at LBA 1000, the rest at LBA 9000.
    static const sunrise_dsk_extent_t multi_extents[] = {
        { 0u, 1000u, 100u },
        { 100u, 9000u, MULTI_FILE_SECTORS - 100u },
    };
    const uint32_t device_sectors = SUNRISE_DSK_HEADER_SECTORS + MULTI_FILE_SECTORS;

    reset_state();
    for (uint32_t i = 0; i < sizeof(multi); i++)
        multi[i] = (uint8_t)(i / 512u);
    sunrise_dsk_build_emulation_header(multi, sizes, 3);
    sunrise_dsk_set_ide_ctx(&ide);
    sunrise_dsk_attach_image(multi, device_sectors, SUNRISE_DSK_HEADER_SECTORS, multi_extents, 2);
    start_task();
    assert(info_blocks == device_sectors);

    // Device sector 0 is the generated partition table; disk 2 starts at 2 + 1440.
    usb_read_lba = 0;
    usb_read_requested = true;
    run_iteration();
    assert(ide.sector_buffer[0x1BE] == 0x81 && ide.sector_buffer[511] == 0xAA);
    usb_read_lba = SUNRISE_DSK_HEADER_SECTORS + 1440u;
    usb_read_requested = true;
    run_iteration();
    assert(memcmp(ide.sector_buffer, multi + (SUNRISE_DSK_HEADER_SECTORS + 1440u) * 512u, 512u) == 0);

    // A write to disk 2 lands on file sector 1440, i.e. card LBA 9000 + 1340.
    ide.sectors_remaining = 0;
    set_lba(SUNRISE_DSK_HEADER_SECTORS + 1440u);
    memset(usb_write_buffer, 0x77, sizeof(usb_write_buffer));
    usb_write_lba = SUNRISE_DSK_HEADER_SECTORS + 1440u;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == 1 && last_write_lba == 9000u + 1340u);
    assert(multi[(SUNRISE_DSK_HEADER_SECTORS + 1440u) * 512u] == 0x77);
    assert(ide.state == IDE_STATE_IDLE && !(ide.status & ATA_STATUS_ERR));

    // The first file sector maps to the first extent.
    ide.sectors_remaining = 0;
    usb_write_lba = SUNRISE_DSK_HEADER_SECTORS;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == 2 && last_write_lba == 1000u);

    // Header writes (Nextor clearing the pointer when 0 is held at boot) stay
    // in PSRAM and never reach the card, even for a read-only image.
    sunrise_dsk_attach_image(multi, device_sectors, SUNRISE_DSK_HEADER_SECTORS, NULL, 0);
    ide.sectors_remaining = 0;
    memset(usb_write_buffer, 0x00, sizeof(usb_write_buffer));
    usb_write_lba = 0;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == 2 && multi[0x1BE] == 0x00);
    assert(ide.state == IDE_STATE_IDLE && !(ide.status & ATA_STATUS_ERR));
    // ...while file writes on that read-only image are still rejected.
    usb_write_lba = SUNRISE_DSK_HEADER_SECTORS + 5u;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == 2 && (ide.status & ATA_STATUS_ERR));

    // A base at or past the device end is ignored rather than trusted.
    sunrise_dsk_attach_image(multi, device_sectors, device_sectors, NULL, 0);
    assert(dsk_file_base == 0);
    puts("PASS: multi-disk device serves the header, maps disk sectors to the file, keeps header writes in PSRAM");
}

int main(void)
{
    test_mount_and_read();
    test_write_through();
    test_read_only();
    test_identify_pending();
    test_split_disks();
    test_emulation_header();
    test_multi_disk_device();
    puts("PASS: Sunrise .DSK backend");
    return 0;
}
