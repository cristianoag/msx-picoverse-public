// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// sunrise_dsk.c - Sunrise IDE disk-image (.DSK) backend for MSX PicoVerse
//
// Parallels sunrise_sd.c, but the IDE device is a .DSK image staged in PSRAM
// instead of a microSD partition. A .DSK file is a raw FAT12 volume without a
// partition table, which Nextor mounts as a single unpartitioned device and,
// when the boot sector is an MSX-DOS 1 one, boots in MSX-DOS 1 mode.
//
// A .DSK made of several disks joined together gets two generated sectors in
// front of it (see sunrise_dsk_build_emulation_header): a partition table
// holding Nextor 2.1's persistent disk-emulation pointer and the emulation
// data listing each disk. Nextor then boots disk 1 in disk emulation mode and
// swaps disks when the user presses 1-9 / A-W during a disk access.
//
// Architecture:
//   Core 0: PIO bus engine + Sunrise mapper/IDE register handling (unchanged)
//   Core 1: this task - serves sector reads from PSRAM and writes each
//           written sector through to the image file on the microSD card.
//
// The write-through path uses disk_write() with the FatFs volume-relative
// LBAs computed at launch (see sunrise_dsk_attach_image), so it bypasses the
// FatFs layer entirely; Core 0 never touches FatFs once the image is running.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pico/stdlib.h"
#include "pico/sync.h"
#include "diskio.h"
#include "sunrise_dsk.h"
#include "sunrise_ide.h"

#define DSK_PDRV 0

static sunrise_ide_t *dsk_ide_ctx = NULL;
static uint8_t *dsk_image = NULL;
static uint32_t dsk_sector_count = 0;
static uint32_t dsk_file_base = 0;
static const sunrise_dsk_extent_t *dsk_extents = NULL;
static uint8_t dsk_extent_count = 0;
static bool dsk_writable = false;

// Shared request flags, defined in sunrise_ide.c (see sunrise_sd.c).
extern volatile bool     usb_device_mounted;
extern volatile bool     usb_read_requested;
extern volatile uint32_t usb_read_lba;
extern volatile bool     usb_write_requested;
extern volatile uint32_t usb_write_lba;
extern uint8_t           usb_write_buffer[512];

void sunrise_dsk_set_ide_ctx(sunrise_ide_t *ide)
{
    dsk_ide_ctx = ide;
}

void sunrise_dsk_attach_image(uint8_t *device, uint32_t device_sectors, uint32_t file_sector_base,
                              const sunrise_dsk_extent_t *extents, uint8_t extent_count)
{
    dsk_image = device;
    dsk_sector_count = device_sectors;
    dsk_file_base = (file_sector_base < device_sectors) ? file_sector_base : 0u;
    if (extents == NULL || extent_count > SUNRISE_DSK_MAX_EXTENTS)
        extent_count = 0;
    dsk_extents = extents;
    dsk_extent_count = extent_count;
    dsk_writable = dsk_extent_count != 0u;
}

static uint16_t dsk_read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void dsk_write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void dsk_write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Size in sectors of the disk starting at `disk`, given `remaining` sectors.
static uint16_t dsk_disk_size_at(const uint8_t *disk, uint32_t remaining)
{
    uint16_t bytes_per_sector = dsk_read_le16(disk + 0x0B);
    uint16_t total = dsk_read_le16(disk + 0x13);
    if (bytes_per_sector == SUNRISE_DSK_SECTOR_SIZE &&
        (total == SUNRISE_DSK_SECTORS_360K || total == SUNRISE_DSK_SECTORS_720K) && total <= remaining)
        return total;

    // MSX-DOS 1 identifies the format by the FAT media byte, not the BPB, so
    // many game disks carry a boot sector without a usable BPB.
    uint8_t media = disk[SUNRISE_DSK_SECTOR_SIZE];
    if (media == 0xF9u && remaining >= SUNRISE_DSK_SECTORS_720K)
        return SUNRISE_DSK_SECTORS_720K;
    if (media == 0xF8u)
        return SUNRISE_DSK_SECTORS_360K;

    return (remaining % SUNRISE_DSK_SECTORS_720K == 0u) ? SUNRISE_DSK_SECTORS_720K : SUNRISE_DSK_SECTORS_360K;
}

uint8_t sunrise_dsk_split_disks(const uint8_t *image, uint32_t image_sectors,
                                uint16_t *disk_sectors, uint8_t max_disks)
{
    if (image == NULL || disk_sectors == NULL || image_sectors == 0u ||
        image_sectors % SUNRISE_DSK_SECTORS_360K != 0u)
        return 0;

    uint8_t count = 0;
    uint32_t offset = 0;
    while (offset < image_sectors) {
        if (count >= max_disks)
            return 0;
        uint16_t size = dsk_disk_size_at(image + offset * SUNRISE_DSK_SECTOR_SIZE, image_sectors - offset);
        disk_sectors[count++] = size;
        offset += size;
    }
    return count;
}

void sunrise_dsk_build_emulation_header(uint8_t *header, const uint16_t *disk_sectors, uint8_t disk_count)
{
    memset(header, 0, SUNRISE_DSK_HEADER_SECTORS * SUNRISE_DSK_SECTOR_SIZE);

    // Sector 0: first partition table entry. Nextor 2.1 enters persistent disk
    // emulation mode when status bit 0 is set and the type is non-zero; the
    // CHS fields then hold the emulation data pointer (device 1, LUN 1 - the
    // Sunrise master - and sector 1). The LBA fields map disk 1 as a FAT12
    // partition, so a normal boot (0 key held) still mounts disk 1.
    uint8_t *entry = header + 0x1BE;
    entry[0] = 0x81u;  // active + "enter disk emulation mode"
    entry[1] = 1u;     // device holding the emulation data
    entry[2] = 1u;     // logical unit
    entry[3] = 0u;     // emulation data sector, bits 31-24
    entry[4] = 0x01u;  // partition type: FAT12
    entry[5] = 0u;     // emulation data sector, bits 23-16
    entry[6] = 0u;     // emulation data sector, bits 15-8
    entry[7] = 1u;     // emulation data sector, bits 7-0
    dsk_write_le32(entry + 8, SUNRISE_DSK_HEADER_SECTORS);
    dsk_write_le32(entry + 12, disk_count ? disk_sectors[0] : 0u);
    header[510] = 0x55u;
    header[511] = 0xAAu;

    // Sector 1: Nextor disk emulation data. Device 0 in every entry means "the
    // same device as the emulation data"; work area 0 lets Nextor allocate it.
    uint8_t *emu = header + SUNRISE_DSK_SECTOR_SIZE;
    memcpy(emu, "NEXTOR_EMU_DATA", 16);
    emu[16] = disk_count;
    emu[17] = 1u;
    uint32_t start = SUNRISE_DSK_HEADER_SECTORS;
    for (uint8_t i = 0; i < disk_count; i++) {
        uint8_t *slot = emu + 24 + (uint32_t)i * 8u;
        dsk_write_le32(slot + 2, start);
        dsk_write_le16(slot + 6, disk_sectors[i]);
        start += disk_sectors[i];
    }
}

static bool __not_in_flash_func(dsk_map_sector)(uint32_t sector, uint32_t *lba_out)
{
    for (uint8_t i = 0; i < dsk_extent_count; i++)
    {
        const sunrise_dsk_extent_t *ext = &dsk_extents[i];
        if (sector >= ext->image_sector && sector - ext->image_sector < ext->count)
        {
            *lba_out = ext->lba + (sector - ext->image_sector);
            return true;
        }
    }
    return false;
}

static void dsk_put_ata_string(uint16_t *w, const char *src, int word_count)
{
    char field[40];
    int len = (int)strlen(src);
    memset(field, ' ', sizeof(field));
    if (len > word_count * 2) len = word_count * 2;
    memcpy(field, src, (size_t)len);
    for (int i = 0; i < word_count; i++)
        w[i] = ((uint16_t)(uint8_t)field[i * 2] << 8) | (uint8_t)field[i * 2 + 1];
}

static void dsk_build_identify(uint8_t *buf)
{
    memset(buf, 0, 512);
    uint16_t *w = (uint16_t *)buf;
    uint32_t total = dsk_sector_count;
    uint16_t heads = 16;
    uint16_t spt = 63;
    uint32_t cyls_calc = total / (heads * spt);
    uint16_t cyls = (cyls_calc == 0) ? 1 : ((cyls_calc > 16383) ? 16383 : (uint16_t)cyls_calc);

    w[0] = 0x0040;
    w[1] = cyls;
    w[3] = heads;
    w[6] = spt;
    dsk_put_ata_string(&w[10], "PICOVERSE-DSK0001", 10);
    dsk_put_ata_string(&w[23], "1.00", 4);
    dsk_put_ata_string(&w[27], "PicoVerse DSK image", 20);
    w[47] = 0x0001;
    w[49] = 0x0200;
    w[53] = 0x0001;
    w[54] = cyls;
    w[55] = heads;
    w[56] = spt;
    uint32_t chs_cap = (uint32_t)cyls * heads * spt;
    w[57] = (uint16_t)(chs_cap & 0xFFFF);
    w[58] = (uint16_t)(chs_cap >> 16);
    w[60] = (uint16_t)(total & 0xFFFF);
    w[61] = (uint16_t)(total >> 16);
}

static void __not_in_flash_func(dsk_advance_lba)(sunrise_ide_t *ide)
{
    uint32_t cur = (uint32_t)ide->sector
                 | ((uint32_t)ide->cylinder_low << 8)
                 | ((uint32_t)ide->cylinder_high << 16)
                 | ((uint32_t)(ide->device_head & 0x0F) << 24);
    cur++;
    ide->sector = (uint8_t)(cur & 0xFF);
    ide->cylinder_low = (uint8_t)((cur >> 8) & 0xFF);
    ide->cylinder_high = (uint8_t)((cur >> 16) & 0xFF);
    ide->device_head = (ide->device_head & 0xF0) | (uint8_t)((cur >> 24) & 0x0F);
}

void __not_in_flash_func(sunrise_dsk_task)(void)
{
    // Core 1 takes over the card from Core 0, as sunrise_sd_task() does. A
    // card that fails to re-initialise leaves the image usable read-only.
    if (dsk_writable && (disk_initialize(DSK_PDRV) & STA_NOINIT))
        dsk_writable = false;

    if (dsk_image != NULL && dsk_sector_count != 0u)
    {
        // The IDENTIFY model name is built as "vendor product", but the vendor
        // field holds only 8 characters (SCSI INQUIRY), so the full name goes
        // in the 16-character product field instead.
        sunrise_ide_set_device_info(dsk_sector_count, SUNRISE_DSK_SECTOR_SIZE,
                                    "", "PICOVERSE DSK", "1.00");
        usb_device_mounted = true;
    }

    while (true)
    {
        sunrise_ide_t *ide = dsk_ide_ctx;
        if (ide == NULL || !usb_device_mounted)
        {
            service_system_audio();
            continue;
        }

        if (usb_read_requested)
        {
            usb_read_requested = false;
            uint32_t lba = usb_read_lba;
            if (lba >= dsk_sector_count)
            {
                ide->usb_read_failed = true;
            }
            else
            {
                memcpy(ide->sector_buffer, dsk_image + lba * SUNRISE_DSK_SECTOR_SIZE, SUNRISE_DSK_SECTOR_SIZE);
                __dmb();
                ide->buffer_index = 0;
                ide->buffer_length = SUNRISE_DSK_SECTOR_SIZE;
                ide->data_latch_valid = false;
                ide->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
                ide->state = IDE_STATE_READ_DATA;
            }
        }

        if (usb_write_requested)
        {
            usb_write_requested = false;
            uint32_t lba = usb_write_lba;
            uint32_t card_lba;
            if (lba >= dsk_sector_count)
            {
                ide->usb_write_failed = true;
            }
            else if (lba < dsk_file_base)
            {
                // Generated header (partition table / emulation data): kept in
                // PSRAM only. Holding 0 at boot clears the emulation pointer
                // this way, which lasts until the next launch from the menu.
                memcpy(dsk_image + lba * SUNRISE_DSK_SECTOR_SIZE, usb_write_buffer, SUNRISE_DSK_SECTOR_SIZE);
                ide->usb_write_ready = true;
            }
            else if (!dsk_writable || !dsk_map_sector(lba - dsk_file_base, &card_lba))
            {
                ide->usb_write_failed = true;
            }
            else if (disk_write(DSK_PDRV, usb_write_buffer, card_lba, 1) == RES_OK)
            {
                // Update the PSRAM copy only once the card holds the sector,
                // so a failed write never leaves the two images diverged.
                memcpy(dsk_image + lba * SUNRISE_DSK_SECTOR_SIZE, usb_write_buffer, SUNRISE_DSK_SECTOR_SIZE);
                ide->usb_write_ready = true;
            }
            else
            {
                ide->usb_write_failed = true;
            }
        }

        if (ide->usb_read_failed)
        {
            ide->usb_read_failed = false;
            ide->status = ATA_STATUS_DRDY | ATA_STATUS_ERR;
            ide->error = ATA_ERROR_ABRT;
            ide->state = IDE_STATE_IDLE;
        }

        if (ide->usb_write_ready)
        {
            ide->usb_write_ready = false;
            dsk_advance_lba(ide);
            if (ide->sectors_remaining > 0)
            {
                ide->buffer_index = 0;
                ide->data_latch_valid = false;
                ide->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
                ide->state = IDE_STATE_WRITE_DATA;
            }
            else
            {
                ide->status = ATA_STATUS_DRDY | ATA_STATUS_DSC;
                ide->state = IDE_STATE_IDLE;
            }
        }

        if (ide->usb_write_failed)
        {
            ide->usb_write_failed = false;
            ide->status = ATA_STATUS_DRDY | ATA_STATUS_ERR;
            ide->error = ATA_ERROR_ABRT;
            ide->state = IDE_STATE_IDLE;
        }

        if (ide->usb_identify_pending)
        {
            ide->usb_identify_pending = false;
            dsk_build_identify(ide->sector_buffer);
            __dmb();
            ide->buffer_index = 0;
            ide->buffer_length = SUNRISE_DSK_SECTOR_SIZE;
            ide->data_latch_valid = false;
            ide->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
            ide->error = 0;
            ide->state = IDE_STATE_READ_DATA;
        }

        service_system_audio();
    }
}
