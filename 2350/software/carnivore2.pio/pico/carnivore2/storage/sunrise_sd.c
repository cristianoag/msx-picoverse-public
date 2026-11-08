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

void __not_in_flash_func(sunrise_sd_task)(void)
{
    // Initialise SD card via SPI
    DSTATUS stat = disk_initialize(SD_PDRV);
    if (stat == 0)
    {
        // Get sector count
        DWORD sector_count = 0;
        if (disk_ioctl(SD_PDRV, GET_SECTOR_COUNT, &sector_count) == RES_OK)
        {
            sd_block_count = (uint32_t)sector_count;
            sd_device_mounted = true;

            // Extract real device info from the SD card's CID register.
            // CID fields: OEM ID (2 chars), Product Name (5 chars), Product Revision.
            sd_card_t *sd = sd_get_by_num(0);
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
        service_scc_audio();

        if (sd_ide_ctx == NULL)
            continue;

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
                DRESULT res = disk_read(SD_PDRV, sd_ide_ctx->sector_buffer, lba, 1);
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
                DRESULT res = disk_write(SD_PDRV, usb_write_buffer, lba, 1);
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
    }
}
