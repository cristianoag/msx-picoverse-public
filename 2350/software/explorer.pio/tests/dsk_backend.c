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
    (void)vendor; (void)product; (void)revision;
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
    sunrise_dsk_attach_image(image, SECTORS, extents, 2);
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
    sunrise_dsk_attach_image(image, SECTORS, extents, 2);
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
    sunrise_dsk_attach_image(image, SECTORS, NULL, 0);
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
    sunrise_dsk_attach_image(image, SECTORS, extents, 2);
    start_task();
    assert(init_calls == 1 && usb_device_mounted);
    usb_write_lba = 10;
    usb_write_requested = true;
    run_iteration();
    assert(write_calls == 0 && (ide.status & ATA_STATUS_ERR));

    // Extent tables past the fixed limit are rejected rather than trusted.
    reset_state();
    sunrise_dsk_attach_image(image, SECTORS, extents, SUNRISE_DSK_MAX_EXTENTS + 1u);
    assert(dsk_extent_count == 0 && !dsk_writable);
    puts("PASS: read-only images, failed card init and oversized extent tables reject writes");
}

static void test_identify_pending(void)
{
    reset_state();
    sunrise_dsk_set_ide_ctx(&ide);
    sunrise_dsk_attach_image(image, SECTORS, NULL, 0);
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

int main(void)
{
    test_mount_and_read();
    test_write_through();
    test_read_only();
    test_identify_pending();
    puts("PASS: Sunrise .DSK backend");
    return 0;
}
