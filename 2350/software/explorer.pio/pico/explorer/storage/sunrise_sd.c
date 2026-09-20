// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// sunrise_sd.c - Sunrise IDE SD card backend for MSX PicoVerse
//
// Implements the microSD-backed block I/O backend for Sunrise IDE ATA
// emulation.  This file parallels the USB backend in sunrise_ide.c but
// replaces TinyUSB MSC operations with raw sector reads/writes via the
// no-OS-FatFS-SD-SDIO-SPI-RPi-Pico library (diskio.h).
//
// Architecture:
//   Core 0: PIO bus engine + Sunrise mapper/IDE register handling (same)
//   Core 1: SD card SPI initialisation + block read/write operations
//
// The volatile flags in sunrise_ide_t (usb_read_ready, usb_read_failed,
// usb_write_ready, usb_write_failed, usb_identify_pending) are reused
// as-is — they are generic IDE completion signals, not USB-specific in
// semantics despite their naming.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/sync.h"
#include "diskio.h"
#include "hw_config.h"
#include "sd_card.h"
#include "util.h"
#include "sunrise_sd.h"
#include "sunrise_ide.h"

// -----------------------------------------------------------------------
// SD card state (Core 1 only, except shared volatiles)
// -----------------------------------------------------------------------
static sunrise_ide_t *sd_ide_ctx = NULL;

static volatile bool sd_device_mounted = false;
static volatile uint32_t sd_block_count = 0;
static volatile uint8_t sd_selected_partition = 0;
static uint32_t sd_lba_base = 0;

// Pending request flags — same semantics as the USB backend.
// Set by Core 0 (via sunrise_ide_handle_read/write), cleared by Core 1.
// These are defined in sunrise_ide.c (non-static) so the same ATA front-end
// code works unchanged with either backend.
extern volatile bool     usb_device_mounted;
extern volatile bool     usb_read_requested;
extern volatile uint32_t usb_read_lba;
extern volatile bool     usb_write_requested;
extern volatile uint32_t usb_write_lba;
extern uint8_t           usb_write_buffer[512];

// Physical drive number for FatFS diskio (always 0 — single SD card)
#define SD_PDRV 0
#define SD_FAT16_MAX_SECTORS 0x800000u

static uint32_t read_le32(const uint8_t *ptr)
{
    return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) |
           ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
}

static uint16_t read_le16(const uint8_t *ptr)
{
    return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static bool is_fat16_partition_type(uint8_t type)
{
    return type == 0x04u || type == 0x06u || type == 0x0Eu;
}

static bool is_fat32_partition_type(uint8_t type)
{
    return type == 0x0Bu || type == 0x0Cu;
}

static bool is_exfat_partition_type(uint8_t type)
{
    return type == 0x07u;
}

static bool is_extended_partition_type(uint8_t type)
{
    return type == 0x05u || type == 0x0Fu || type == 0x85u;
}

static void copy_volume_label(char *dst, const uint8_t *src)
{
    uint8_t out = 0;
    for (uint8_t i = 0; i < 11u; i++) {
        uint8_t ch = src[i];
        if (ch < 32u) break;
        dst[out++] = (char)ch;
    }
    while (out && dst[out - 1u] == ' ') out--;
    dst[out] = '\0';
}

static bool label_is_blank_or_default(const char *label)
{
    return label[0] == '\0' || strcmp(label, "NO NAME") == 0 || strcmp(label, "NEXTOR 2.0") == 0;
}

static bool copy_fat_dir_label(char *dst, const uint8_t *sector)
{
    for (uint16_t off = 0; off < 512u; off += 32u) {
        const uint8_t *entry = sector + off;
        uint8_t first = entry[0];
        uint8_t attr = entry[11];
        if (first == 0x00u) break;
        if (first == 0xE5u || attr == 0x0Fu) continue;
        if ((attr & 0x08u) != 0) {
            copy_volume_label(dst, entry);
            return dst[0] != '\0';
        }
    }
    return false;
}

static void read_fat16_root_label(sunrise_sd_partition_t *part, const uint8_t *boot, uint8_t *scratch)
{
    uint16_t root_entries = read_le16(boot + 17);
    uint16_t reserved = read_le16(boot + 14);
    uint8_t fats = boot[16];
    uint16_t sectors_per_fat = read_le16(boot + 22);
    uint32_t root_lba = part->start_lba + reserved + ((uint32_t)fats * sectors_per_fat);
    uint32_t root_sectors = (((uint32_t)root_entries * 32u) + 511u) / 512u;

    for (uint32_t i = 0; i < root_sectors && i < 32u; i++) {
        if (disk_read_raw(SD_PDRV, scratch, root_lba + i, 1) != RES_OK) break;
        if (copy_fat_dir_label(part->label, scratch)) break;
    }
}

static uint32_t fat_first_data_lba(const sunrise_sd_partition_t *part, const uint8_t *boot)
{
    uint16_t root_entries = read_le16(boot + 17);
    uint16_t reserved = read_le16(boot + 14);
    uint8_t fats = boot[16];
    uint32_t sectors_per_fat = read_le16(boot + 22);
    uint32_t root_sectors = (((uint32_t)root_entries * 32u) + 511u) / 512u;
    if (sectors_per_fat == 0) sectors_per_fat = read_le32(boot + 36);
    return part->start_lba + reserved + ((uint32_t)fats * sectors_per_fat) + root_sectors;
}

static void read_fat32_root_label(sunrise_sd_partition_t *part, const uint8_t *boot, uint8_t *scratch)
{
    uint8_t sectors_per_cluster = boot[13];
    uint32_t root_cluster = read_le32(boot + 44);
    uint32_t first_data = fat_first_data_lba(part, boot);
    if (sectors_per_cluster == 0 || root_cluster < 2u) return;

    uint32_t root_lba = first_data + ((root_cluster - 2u) * sectors_per_cluster);
    for (uint8_t i = 0; i < sectors_per_cluster && i < 32u; i++) {
        if (disk_read_raw(SD_PDRV, scratch, root_lba + i, 1) != RES_OK) break;
        if (copy_fat_dir_label(part->label, scratch)) break;
    }
}

static bool copy_exfat_dir_label(char *dst, const uint8_t *sector)
{
    for (uint16_t off = 0; off < 512u; off += 32u) {
        const uint8_t *entry = sector + off;
        if (entry[0] == 0x00u) break;
        if (entry[0] == 0x83u) {
            uint8_t len = entry[1];
            uint8_t out = 0;
            if (len > 11u) len = 11u;
            for (uint8_t i = 0; i < len; i++) {
                uint16_t ch = read_le16(entry + 2u + ((uint16_t)i * 2u));
                dst[out++] = (char)((ch >= 32u && ch < 127u) ? ch : '?');
            }
            dst[out] = '\0';
            return out != 0;
        }
    }
    return false;
}

static void read_exfat_root_label(sunrise_sd_partition_t *part, const uint8_t *boot, uint8_t *scratch)
{
    uint32_t cluster_heap = read_le32(boot + 88);
    uint32_t root_cluster = read_le32(boot + 96);
    uint32_t sectors_per_cluster = 1u << boot[109];
    if (root_cluster < 2u) return;

    uint32_t root_lba = part->start_lba + cluster_heap + ((root_cluster - 2u) * sectors_per_cluster);
    for (uint32_t i = 0; i < sectors_per_cluster && i < 32u; i++) {
        if (disk_read_raw(SD_PDRV, scratch, root_lba + i, 1) != RES_OK) break;
        if (copy_exfat_dir_label(part->label, scratch)) break;
    }
}

static void read_partition_label(sunrise_sd_partition_t *part, const uint8_t *boot, uint8_t *scratch)
{
    if (part->type == SUNRISE_SD_FS_FAT16 && label_is_blank_or_default(part->label)) {
        part->label[0] = '\0';
        read_fat16_root_label(part, boot, scratch);
    } else if (part->type == SUNRISE_SD_FS_FAT32 && label_is_blank_or_default(part->label)) {
        part->label[0] = '\0';
        read_fat32_root_label(part, boot, scratch);
    } else if (part->type == SUNRISE_SD_FS_EXFAT) {
        read_exfat_root_label(part, boot, scratch);
    }
}

static bool partition_fits_card(const sunrise_sd_partition_t *part, uint32_t card_sectors)
{
    return part->sector_count > 0 &&
           part->start_lba < card_sectors &&
           part->sector_count <= (card_sectors - part->start_lba);
}

static bool decode_supported_partition(const uint8_t *entry, const uint8_t *boot, sunrise_sd_partition_t *part)
{
    part->label[0] = '\0';
    if (boot[510] != 0x55u || boot[511] != 0xAAu)
        return false;
    if (is_fat16_partition_type(entry[4]) && memcmp(boot + 54, "FAT16", 5) == 0) {
        part->type = SUNRISE_SD_FS_FAT16;
        copy_volume_label(part->label, boot + 43);
    } else if (is_fat32_partition_type(entry[4]) && memcmp(boot + 82, "FAT32", 5) == 0) {
        part->type = SUNRISE_SD_FS_FAT32;
        copy_volume_label(part->label, boot + 71);
    } else if (is_exfat_partition_type(entry[4]) && memcmp(boot + 3, "EXFAT   ", 8) == 0) {
        part->type = SUNRISE_SD_FS_EXFAT;
    }
    return part->type != 0;
}

uint8_t sunrise_sd_list_fat16_partitions(sunrise_sd_partition_t *out, uint8_t max_count)
{
    sunrise_sd_partition_t parts[4];
    uint8_t count = sunrise_sd_list_supported_partitions(parts, 4);
    uint8_t out_count = 0;
    for (uint8_t i = 0; i < count && out_count < max_count; i++) {
        if (parts[i].number <= 4u && parts[i].type == SUNRISE_SD_FS_FAT16 && parts[i].sector_count <= SD_FAT16_MAX_SECTORS) {
            out[out_count++] = parts[i];
        }
    }
    return out_count;
}

uint8_t sunrise_sd_list_supported_partitions(sunrise_sd_partition_t *out, uint8_t max_count)
{
    if (!out || max_count == 0)
        return 0;

    sd_card_t *sd = sd_get_by_num(0);
    uint32_t card_sectors = sd ? sd->get_num_sectors(sd) : 0;
    if (card_sectors == 0)
        return 0;

    uint8_t sector[512];
    if (disk_read_raw(SD_PDRV, sector, 0, 1) != RES_OK)
        return 0;
    if (sector[510] != 0x55u || sector[511] != 0xAAu)
        return 0;

    uint8_t boot[512];
    uint8_t count = 0;
    uint32_t extended_base = 0;
    for (uint8_t i = 0; i < 4 && count < max_count; i++)
    {
        const uint8_t *entry = sector + 446 + ((uint16_t)i * 16u);
        if (is_extended_partition_type(entry[4]) && extended_base == 0) {
            extended_base = read_le32(entry + 8);
            continue;
        }
        sunrise_sd_partition_t part = {
            .number = (uint8_t)(i + 1u),
            .type = 0,
            .start_lba = read_le32(entry + 8),
            .sector_count = read_le32(entry + 12),
        };
        if (!partition_fits_card(&part, card_sectors))
            continue;
        if (disk_read_raw(SD_PDRV, boot, part.start_lba, 1) != RES_OK)
            continue;
        if (decode_supported_partition(entry, boot, &part)) {
            read_partition_label(&part, boot, boot);
            out[count++] = part;
        }
    }

    uint8_t logical_number = 5u;
    uint32_t ebr_lba = extended_base;
    for (uint8_t guard = 0; ebr_lba != 0 && count < max_count && guard < 16u; guard++)
    {
        if (ebr_lba >= card_sectors || disk_read_raw(SD_PDRV, sector, ebr_lba, 1) != RES_OK)
            break;
        if (sector[510] != 0x55u || sector[511] != 0xAAu)
            break;

        const uint8_t *entry = sector + 446;
        sunrise_sd_partition_t part = {
            .number = logical_number,
            .type = 0,
            .start_lba = ebr_lba + read_le32(entry + 8),
            .sector_count = read_le32(entry + 12),
        };
        if (partition_fits_card(&part, card_sectors) && disk_read_raw(SD_PDRV, boot, part.start_lba, 1) == RES_OK) {
            if (decode_supported_partition(entry, boot, &part)) {
                read_partition_label(&part, boot, boot);
                out[count++] = part;
            }
        }
        logical_number++;

        const uint8_t *next = sector + 462;
        uint32_t next_rel = read_le32(next + 8);
        if (!is_extended_partition_type(next[4]) || next_rel == 0)
            break;
        ebr_lba = extended_base + next_rel;
    }
    return count;
}

// -----------------------------------------------------------------------
// ATA IDENTIFY DEVICE response builder (SD variant)
// -----------------------------------------------------------------------
static void build_identify_data_sd(uint8_t *buf)
{
    memset(buf, 0, 512);
    uint16_t *w = (uint16_t *)buf;

    // Word 0: General configuration — fixed, non-removable
    w[0] = 0x0040;

    // Words 1-3: Legacy CHS geometry
    uint32_t total = sd_block_count;
    uint16_t heads = 16;
    uint16_t spt = 63;
    uint32_t cyls_calc = total / (heads * spt);
    uint16_t cyls = (cyls_calc > 16383) ? 16383 : (uint16_t)cyls_calc;
    w[1] = cyls;
    w[3] = heads;
    w[6] = spt;

    // Words 10-19: Serial number (20 ASCII chars)
    static const char serial[20] = "PICOVERSE-SD0001    ";
    for (int i = 0; i < 10; i++)
        w[10 + i] = ((uint16_t)(uint8_t)serial[i * 2] << 8) | (uint8_t)serial[i * 2 + 1];

    // Words 23-26: Firmware revision (8 ASCII chars)
    static const char fwrev[8] = "1.00    ";
    for (int i = 0; i < 4; i++)
        w[23 + i] = ((uint16_t)(uint8_t)fwrev[i * 2] << 8) | (uint8_t)fwrev[i * 2 + 1];

    // Words 27-46: Model number (40 ASCII chars)
    char model[40];
    memset(model, ' ', 40);
    memcpy(model, "PicoVerse MicroSD", 17);
    for (int i = 0; i < 20; i++)
        w[27 + i] = ((uint16_t)(uint8_t)model[i * 2] << 8) | (uint8_t)model[i * 2 + 1];

    // Word 47: Max sectors per READ/WRITE MULTIPLE
    w[47] = 0x0001;

    // Word 49: Capabilities — LBA supported
    w[49] = 0x0200;

    // Word 53: Fields valid
    w[53] = 0x0001;

    // Words 54-56: Current CHS
    w[54] = cyls;
    w[55] = heads;
    w[56] = spt;

    // Words 57-58: Current capacity in sectors
    uint32_t chs_cap = (uint32_t)cyls * heads * spt;
    w[57] = (uint16_t)(chs_cap & 0xFFFF);
    w[58] = (uint16_t)(chs_cap >> 16);

    // Words 60-61: Total user addressable LBA sectors
    w[60] = (uint16_t)(total & 0xFFFF);
    w[61] = (uint16_t)(total >> 16);
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------
void sunrise_sd_set_ide_ctx(sunrise_ide_t *ide)
{
    sd_ide_ctx = ide;
}

void sunrise_sd_select_partition(uint8_t partition_number)
{
    sd_selected_partition = (partition_number >= 1u && partition_number <= 4u) ? partition_number : 0u;
}

void __not_in_flash_func(sunrise_sd_task)(void)
{
    // Initialise SD card via SPI
    DSTATUS stat = disk_initialize(SD_PDRV);
    if (stat == 0)
    {
        // Get physical sector count
        sd_card_t *sd = sd_get_by_num(0);
        uint32_t sector_count = sd ? sd->get_num_sectors(sd) : 0;
        if (sector_count != 0)
        {
            sunrise_sd_partition_t parts[4];
            uint8_t part_count = sunrise_sd_list_fat16_partitions(parts, 4);
            uint8_t chosen = 0xFFu;

            for (uint8_t i = 0; i < part_count; i++)
            {
                if (parts[i].number == sd_selected_partition)
                {
                    chosen = i;
                    break;
                }
            }
            if (chosen == 0xFFu && part_count)
                chosen = 0;

            if (chosen != 0xFFu)
            {
                sd_lba_base = parts[chosen].start_lba;
                sd_block_count = parts[chosen].sector_count;
            }
            else
            {
                sd_lba_base = 0;
                sd_block_count = (uint32_t)sector_count;
            }
            sd_device_mounted = true;

            // Extract real device info from the SD card's CID register.
            // CID fields: OEM ID (2 chars), Product Name (5 chars), Product Revision.
            char vendor[3] = {0};   // OEM ID: 2 ASCII chars
            char product[6] = {0};  // Product Name: 5 ASCII chars
            char revision[5] = {0}; // "X.Y" from PRV register
            if (sd) {
                ext_str(16, sd->state.CID, 119, 104, sizeof(vendor), vendor);
                ext_str(16, sd->state.CID, 103, 64, sizeof(product), product);
                uint8_t prv_major = (uint8_t)ext_bits16(sd->state.CID, 63, 60);
                uint8_t prv_minor = (uint8_t)ext_bits16(sd->state.CID, 59, 56);
                snprintf(revision, sizeof(revision), "%u.%u", prv_major, prv_minor);
            }

            // Populate IDENTIFY DEVICE fields so the ATA front-end
            // returns proper device info to Nextor during boot.
            sunrise_ide_set_device_info(sd_block_count, 512,
                                        vendor, product, revision);
            usb_device_mounted = true;  // Signal IDE front-end that device is ready
        }
    }

    while (true)
    {
        if (sd_ide_ctx == NULL)
        {
            service_system_audio();
            continue;
        }

        // --- Handle read request from Core 0 ---
        if (usb_read_requested && sd_device_mounted)
        {
            usb_read_requested = false;

            uint32_t lba = usb_read_lba;

            if (lba >= sd_block_count)
            {
                sd_ide_ctx->usb_read_failed = true;
            }
            else
            {
                DRESULT res = disk_read_raw(SD_PDRV, sd_ide_ctx->sector_buffer, sd_lba_base + lba, 1);
                if (res == RES_OK)
                {
                    __dmb();
                    sd_ide_ctx->buffer_index = 0;
                    sd_ide_ctx->buffer_length = 512;
                    sd_ide_ctx->data_latch_valid = false;
                    sd_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
                    sd_ide_ctx->state = IDE_STATE_READ_DATA;
                }
                else
                {
                    sd_ide_ctx->usb_read_failed = true;
                }
            }
        }

        // --- Handle write request from Core 0 ---
        if (usb_write_requested && sd_device_mounted)
        {
            usb_write_requested = false;

            uint32_t lba = usb_write_lba;

            if (lba >= sd_block_count)
            {
                sd_ide_ctx->usb_write_failed = true;
            }
            else
            {
                DRESULT res = disk_write_raw(SD_PDRV, usb_write_buffer, sd_lba_base + lba, 1);
                if (res == RES_OK)
                {
                    sd_ide_ctx->usb_write_ready = true;
                }
                else
                {
                    sd_ide_ctx->usb_write_failed = true;
                }
            }
        }

        // --- Propagate completion status to IDE state machine ---
        if (sd_ide_ctx->usb_read_failed)
        {
            sd_ide_ctx->usb_read_failed = false;
            sd_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_ERR;
            sd_ide_ctx->error = ATA_ERROR_ABRT;
            sd_ide_ctx->state = IDE_STATE_IDLE;
        }

        if (sd_ide_ctx->usb_write_ready)
        {
            sd_ide_ctx->usb_write_ready = false;
            // ide_advance_lba is static in sunrise_ide.c — replicate here
            {
                uint32_t cur = (uint32_t)sd_ide_ctx->sector
                             | ((uint32_t)sd_ide_ctx->cylinder_low << 8)
                             | ((uint32_t)sd_ide_ctx->cylinder_high << 16)
                             | ((uint32_t)(sd_ide_ctx->device_head & 0x0F) << 24);
                cur++;
                sd_ide_ctx->sector = (uint8_t)(cur & 0xFF);
                sd_ide_ctx->cylinder_low = (uint8_t)((cur >> 8) & 0xFF);
                sd_ide_ctx->cylinder_high = (uint8_t)((cur >> 16) & 0xFF);
                sd_ide_ctx->device_head = (sd_ide_ctx->device_head & 0xF0) | (uint8_t)((cur >> 24) & 0x0F);
            }

            if (sd_ide_ctx->sectors_remaining > 0)
            {
                sd_ide_ctx->buffer_index = 0;
                sd_ide_ctx->data_latch_valid = false;
                sd_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
                sd_ide_ctx->state = IDE_STATE_WRITE_DATA;
            }
            else
            {
                sd_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_DSC;
                sd_ide_ctx->state = IDE_STATE_IDLE;
            }
        }

        if (sd_ide_ctx->usb_write_failed)
        {
            sd_ide_ctx->usb_write_failed = false;
            sd_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_ERR;
            sd_ide_ctx->error = ATA_ERROR_ABRT;
            sd_ide_ctx->state = IDE_STATE_IDLE;
        }

        // --- Complete a pending IDENTIFY after SD mount ---
        if (sd_ide_ctx->usb_identify_pending && sd_device_mounted)
        {
            sd_ide_ctx->usb_identify_pending = false;
            build_identify_data_sd(sd_ide_ctx->sector_buffer);
            __dmb();
            sd_ide_ctx->buffer_index = 0;
            sd_ide_ctx->buffer_length = 512;
            sd_ide_ctx->data_latch_valid = false;
            sd_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
            sd_ide_ctx->error = 0;
            sd_ide_ctx->state = IDE_STATE_READ_DATA;
        }

        // Publish disk completions before yielding a bounded slice to audio.
        // The raw SD calls above remain synchronous and may still stall audio.
        service_system_audio();
    }
}
