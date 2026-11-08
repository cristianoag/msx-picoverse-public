// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// sunrise_ide.c - Sunrise IDE emulation for MSX PicoVerse
//
// Emulates the Sunrise MSX IDE interface hardware by intercepting memory-mapped
// reads/writes in the 0x4000-0x7FFF range and translating ATA commands into
// USB Mass Storage Class operations via TinyUSB on the RP2040 USB-C port.
//
// Architecture:
//   Core 0: PIO bus engine + Sunrise mapper/IDE register handling
//   Core 1: TinyUSB USB host stack + MSC block read/write operations
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/sync.h"
#include "bsp/board.h"
#include "tusb.h"
#include "class/msc/msc_host.h"
#include "sunrise_ide.h"

// -----------------------------------------------------------------------
// USB MSC state (Core 1 only, except where noted volatile/shared)
// -----------------------------------------------------------------------
static sunrise_ide_t *usb_ide_ctx = NULL;

static scsi_inquiry_resp_t inquiry_resp;

static uint8_t current_dev_addr = 0;
static uint8_t current_lun = 0;
volatile bool usb_device_mounted = false;
static volatile uint32_t usb_block_count = 0;
static volatile uint32_t usb_block_size = 0;

// Block I/O buffers — aligned for DMA.
// usb_read_buffer is 4096 bytes to accommodate devices with 4K native sectors.
// Each USB read fetches one native block; we extract the correct 512-byte slice.
CFG_TUH_MEM_SECTION TU_ATTR_ALIGNED(4) static uint8_t usb_read_buffer[4096];
TU_ATTR_ALIGNED(4) uint8_t usb_write_buffer[512];

// Pending request flags (set by Core 0/IDE handler, cleared by Core 1)
volatile bool usb_read_requested = false;
volatile uint32_t usb_read_lba = 0;
volatile bool usb_write_requested = false;
volatile uint32_t usb_write_lba = 0;

static volatile bool usb_read_in_progress = false;
static volatile bool usb_write_in_progress = false;

// Timestamp (in microseconds) when the current USB transfer started.
// Used for timeout detection — slow or stalled USB devices must not
// keep the IDE in BSY state indefinitely.
static volatile uint64_t usb_transfer_start_us = 0;

// Maximum time (microseconds) to wait for a single USB sector transfer.
// USB Full Speed (12 Mbps): a 512-byte bulk transfer takes ~0.5 ms minimum.
// Slow devices may NAK for many frames during flash wear-levelling.
// Allow a generous 3 seconds to accommodate the slowest devices.
#define USB_TRANSFER_TIMEOUT_US  3000000u

// -----------------------------------------------------------------------
// ATA IDENTIFY DEVICE response builder
// -----------------------------------------------------------------------

// Write a fixed-length ATA string field into IDENTIFY words.
// ATA spec: first char in high byte (bits 15:8), second char in low byte (bits 7:0).
// src must be exactly len bytes (space-padded, not null-terminated).
static void ata_string_to_words(uint16_t *w, const char *src, int word_count)
{
    for (int i = 0; i < word_count; i++)
        w[i] = ((uint16_t)(uint8_t)src[i * 2] << 8) | (uint8_t)src[i * 2 + 1];
}

static void build_identify_data(uint8_t *buf)
{
    memset(buf, 0, 512);
    uint16_t *w = (uint16_t *)buf;

    // Word 0: General configuration
    w[0] = 0x0040;  // Fixed device, non-removable

    // Words 1-3: Legacy CHS geometry (fake values for LBA device)
    // The ATA interface presents 512-byte sectors to the MSX.
    // For devices with larger native sectors, multiply the block count.
    uint32_t total = usb_block_count;
    if (usb_block_size > 512)
        total *= (usb_block_size / 512);
    uint16_t heads = 16;
    uint16_t spt = 63;
    uint32_t cyls_calc = total / (heads * spt);
    uint16_t cyls = (cyls_calc > 16383) ? 16383 : (uint16_t)cyls_calc;
    w[1] = cyls;
    w[3] = heads;
    w[6] = spt;

    // Words 10-19: Serial number (20 ASCII chars, space-padded)
    char serial[20];
    memset(serial, ' ', 20);
    memcpy(serial, "PICOVERSE00000001", 17);
    ata_string_to_words(&w[10], serial, 10);

    // Words 23-26: Firmware revision (8 ASCII chars)
    // Use USB device's SCSI product revision if available
    char fwrev[8];
    memset(fwrev, ' ', 8);
    memcpy(fwrev, inquiry_resp.product_rev, 4);
    ata_string_to_words(&w[23], fwrev, 4);

    // Words 27-46: Model number (40 ASCII chars)
    // Use USB device's SCSI vendor + product identification
    char model[40];
    memset(model, ' ', 40);
    int pos = 0;
    for (int i = 0; i < 8; i++)
    {
        uint8_t c = (uint8_t)inquiry_resp.vendor_id[i];
        if (c >= 0x20 && c < 0x7F) model[pos++] = (char)c;
        else break;
    }
    // Trim trailing spaces from vendor
    while (pos > 0 && model[pos - 1] == ' ') pos--;
    if (pos > 0) model[pos++] = ' ';
    for (int i = 0; i < 16; i++)
    {
        uint8_t c = (uint8_t)inquiry_resp.product_id[i];
        if (c >= 0x20 && c < 0x7F) model[pos++] = (char)c;
        else break;
    }
    ata_string_to_words(&w[27], model, 20);

    // Word 47: Max sectors per READ/WRITE MULTIPLE (not used, but set to 1)
    w[47] = 0x0001;

    // Word 49: Capabilities — LBA supported
    w[49] = 0x0200;  // LBA supported

    // Word 53: Fields valid
    w[53] = 0x0001;

    // Words 54-56: Current CHS (same as legacy)
    w[54] = cyls;
    w[55] = heads;
    w[56] = spt;

    // Words 57-58: Current capacity in sectors (CHS)
    uint32_t chs_cap = (uint32_t)cyls * heads * spt;
    w[57] = (uint16_t)(chs_cap & 0xFFFF);
    w[58] = (uint16_t)(chs_cap >> 16);

    // Words 60-61: Total number of user addressable LBA sectors
    w[60] = (uint16_t)(total & 0xFFFF);
    w[61] = (uint16_t)(total >> 16);
}

// -----------------------------------------------------------------------
// IDE context initialisation
// -----------------------------------------------------------------------
// Set ATA registers to the power-on / post-reset / post-DEVDIAG state
// for a PATA master device (ATA/ATAPI-6 §9.2, §9.12).
static void ide_set_device_signature(sunrise_ide_t *ide)
{
    ide->error        = ATA_DIAG_NO_ERROR;  // diagnostic "no error"
    ide->sector_count = 0x01;               // PATA device signature
    ide->sector       = 0x01;
    ide->cylinder_low = 0x00;               // PATA: 0x0000
    ide->cylinder_high = 0x00;
    ide->device_head  = 0x00;               // master selected
    ide->status       = ATA_STATUS_DRDY | ATA_STATUS_DSC;
    ide->state        = IDE_STATE_IDLE;
    ide->usb_identify_pending = false;      // cancel any waiting IDENTIFY
}

void sunrise_ide_init(sunrise_ide_t *ide)
{
    memset((void *)ide, 0, sizeof(sunrise_ide_t));
    ide->segment = 0;
    ide->ide_enabled = false;
    ide->data_latch_valid = false;
    ide->buffer_index = 0;
    ide->buffer_length = 0;
    ide->sectors_remaining = 0;
    ide_set_device_signature(ide);
}

// -----------------------------------------------------------------------
// Compute LBA from ATA registers (LBA mode)
// -----------------------------------------------------------------------
static inline uint32_t ide_get_lba(const sunrise_ide_t *ide)
{
    return (uint32_t)ide->sector
         | ((uint32_t)ide->cylinder_low << 8)
         | ((uint32_t)ide->cylinder_high << 16)
         | ((uint32_t)(ide->device_head & ATA_DEV_HEAD_HEAD_MASK) << 24);
}

// -----------------------------------------------------------------------
// Execute ATA command (called on write to command register 0x7E07)
// -----------------------------------------------------------------------
static void __not_in_flash_func(ide_execute_command)(sunrise_ide_t *ide, uint8_t cmd)
{
    // Only respond to master device (bit 4 = 0)
    if (ide->device_head & ATA_DEV_HEAD_DEV)
    {
        ide->status = ATA_STATUS_ERR;
        ide->error = ATA_ERROR_ABRT;
        ide->state = IDE_STATE_IDLE;
        return;
    }

    switch (cmd)
    {
    case ATA_CMD_IDENTIFY:
    {
        if (!usb_device_mounted)
        {
            // USB not enumerated yet — stay busy so the driver keeps
            // polling WAIT_DRQ (up to 5 s).  Core 1 will complete the
            // IDENTIFY once the USB device mounts.
            ide->status = ATA_STATUS_BSY;
            ide->error = 0;
            ide->state = IDE_STATE_BUSY;
            ide->usb_identify_pending = true;
            return;
        }
        build_identify_data(ide->sector_buffer);
        ide->buffer_index = 0;
        ide->buffer_length = 512;
        ide->data_latch_valid = false;
        ide->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
        ide->error = 0;
        ide->state = IDE_STATE_READ_DATA;
        break;
    }

    case ATA_CMD_READ_SECTORS:
    {
        if (!usb_device_mounted)
        {
            ide->status = ATA_STATUS_ERR;
            ide->error = ATA_ERROR_ABRT;
            ide->state = IDE_STATE_IDLE;
            return;
        }
        // ATA spec: sector_count==0 means 256 sectors
        uint16_t count = ide->sector_count ? ide->sector_count : 256;
        ide->sectors_remaining = count;
        ide->buffer_index = 0;
        ide->buffer_length = 0;
        ide->data_latch_valid = false;

        // Request first sector from USB
        uint32_t lba = ide_get_lba(ide);
        usb_read_lba = lba;
        ide->status = ATA_STATUS_BSY;
        ide->error = 0;
        ide->state = IDE_STATE_BUSY;
        usb_read_requested = true;
        break;
    }

    case ATA_CMD_WRITE_SECTORS:
    {
        if (!usb_device_mounted)
        {
            ide->status = ATA_STATUS_ERR;
            ide->error = ATA_ERROR_ABRT;
            ide->state = IDE_STATE_IDLE;
            return;
        }
        // ATA spec: sector_count==0 means 256 sectors
        uint16_t count = ide->sector_count ? ide->sector_count : 256;
        ide->sectors_remaining = count;
        ide->buffer_index = 0;
        ide->buffer_length = 512;
        ide->data_latch_valid = false;

        // Set DRQ immediately — MSX should start writing data
        ide->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
        ide->error = 0;
        ide->state = IDE_STATE_WRITE_DATA;
        break;
    }

    case ATA_CMD_SET_FEATURES:
    case ATA_CMD_INIT_PARAMS:
    case ATA_CMD_RECALIBRATE:
    {
        // Accept but do nothing
        ide->status = ATA_STATUS_DRDY | ATA_STATUS_DSC;
        ide->error = 0;
        ide->state = IDE_STATE_IDLE;
        break;
    }

    case ATA_CMD_DEVICE_DIAG:
    {
        // EXECUTE DEVICE DIAGNOSTIC (0x90)
        // After this command, the error register must contain the
        // diagnostic code: 0x01 = no error.  The Nextor Sunrise IDE
        // driver (CHKDIAG) reads IDE_ERROR and expects bits[6:0]==1
        // for success; any other value is mapped to an error string.
        ide_set_device_signature(ide);
        break;
    }

    case ATA_CMD_DEVICE_RESET:
    {
        // DEVICE RESET (0x08) — intended for ATAPI devices only;
        // for our ATA device, treat it the same as a post-reset state.
        ide_set_device_signature(ide);
        break;
    }

    default:
    {
        // Unknown command — abort
        ide->status = ATA_STATUS_DRDY | ATA_STATUS_ERR;
        ide->error = ATA_ERROR_ABRT;
        ide->state = IDE_STATE_IDLE;
        break;
    }
    }
}

// -----------------------------------------------------------------------
// Advance LBA registers to next sector (after a sector read/write)
// -----------------------------------------------------------------------
static void __not_in_flash_func(ide_advance_lba)(sunrise_ide_t *ide)
{
    uint32_t lba = ide_get_lba(ide) + 1;
    ide->sector = (uint8_t)(lba & 0xFF);
    ide->cylinder_low = (uint8_t)((lba >> 8) & 0xFF);
    ide->cylinder_high = (uint8_t)((lba >> 16) & 0xFF);
    ide->device_head = (ide->device_head & 0xF0) | (uint8_t)((lba >> 24) & 0x0F);
}

// -----------------------------------------------------------------------
// Handle write to Sunrise address space (0x4000-0x7FFF)
// Returns true if the write was consumed (not regular ROM write)
// -----------------------------------------------------------------------
bool __not_in_flash_func(sunrise_ide_handle_write)(sunrise_ide_t *ide, uint16_t addr, uint8_t data)
{
    // --- Segment/IDE control register at 0x4104 ---
    // The Sunrise IDE chgbnk routine bit-reverses the bank number before
    // writing: bank bit0→reg bit7, bit1→reg bit6, bit2→reg bit5.
    // The real hardware reverses them back:
    //   IDEROMADDR <= cReg(5) & cReg(6) & cReg(7) & Addr(13..0)
    // We must do the same reversal to recover the actual page number.
    if (addr == SUNRISE_CTRL_REG_ADDR)
    {
        uint8_t raw = (data >> 5) & 0x07;  // bits [7:5] as 3-bit value
        // Reverse 3 bits: {bit2,bit1,bit0} → {bit0,bit1,bit2}
        ide->segment = (uint8_t)(((raw & 4) >> 2) | (raw & 2) | ((raw & 1) << 2));
        ide->ide_enabled = (data & 0x01) != 0;
        return true;
    }

    // If IDE is not enabled, no IDE registers are mapped
    if (!ide->ide_enabled)
        return false;

    // --- IDE data register write (16-bit with latch): 0x7C00-0x7DFF ---
    if (addr >= IDE_DATA_BASE && addr <= IDE_DATA_END)
    {
        if (ide->state != IDE_STATE_WRITE_DATA)
            return true;  // consume but ignore if not in write state

        if ((addr & 1) == 0)
        {
            // Low byte — latch it
            ide->data_latch = data;
            ide->data_latch_valid = true;
        }
        else
        {
            // High byte — store the word (low byte was latched)
            uint8_t lo = ide->data_latch_valid ? ide->data_latch : 0x00;
            ide->data_latch_valid = false;

            if (ide->buffer_index < 512)
            {
                ide->sector_buffer[ide->buffer_index++] = lo;
            }
            if (ide->buffer_index < 512)
            {
                ide->sector_buffer[ide->buffer_index++] = data;
            }

            // Check if we've received a full sector
            if (ide->buffer_index >= 512)
            {
                ide->sectors_remaining--;
                ide->buffer_index = 0;

                // Copy data to USB write buffer and request write
                memcpy(usb_write_buffer, ide->sector_buffer, 512);
                usb_write_lba = ide_get_lba(ide);
                ide->status = ATA_STATUS_BSY;
                ide->state = IDE_STATE_BUSY;
                usb_write_requested = true;
            }
        }
        return true;
    }

    // --- IDE registers write: 0x7E00-0x7EFF ---
    if (addr >= IDE_REG_BASE && addr <= IDE_REG_END)
    {
        uint8_t reg = addr & 0x0F;
        switch (reg)
        {
        case IDE_REG_FEATURE:       ide->feature = data; break;
        case IDE_REG_SECTOR_COUNT:  ide->sector_count = data; break;
        case IDE_REG_SECTOR:        ide->sector = data; break;
        case IDE_REG_CYLINDER_LOW:  ide->cylinder_low = data; break;
        case IDE_REG_CYLINDER_HIGH: ide->cylinder_high = data; break;
        case IDE_REG_DEVICE_HEAD:   ide->device_head = data; break;
        case IDE_REG_COMMAND:
            ide_execute_command(ide, data);
            break;
        case IDE_REG_DEVICE_CTRL:
            // Bit 2 = SRST (software reset)
            if (data & 0x04)
            {
                // SRST asserted — device goes busy
                ide->status = ATA_STATUS_BSY;
                ide->state = IDE_STATE_IDLE;
            }
            else
            {
                // SRST deasserted (or nIEN-only write) —
                // set the post-reset device signature so that
                // GETDEVTYPE finds the PATA signature (cyl=0x0000)
                // and CHKDIAG sees error=0x01 (no error).
                ide_set_device_signature(ide);
            }
            break;
        default:
            break;
        }
        return true;
    }

    return false;  // Not an IDE address
}

// -----------------------------------------------------------------------
// Handle read from Sunrise address space (0x4000-0x7FFF)
// Returns true if *data_out should be returned instead of ROM data
// -----------------------------------------------------------------------
bool __not_in_flash_func(sunrise_ide_handle_read)(sunrise_ide_t *ide, uint16_t addr, uint8_t *data_out)
{
    // Reads to 0x4104 return normal ROM contents (not the control register)
    // IDE registers only visible when ide_enabled is true
    if (!ide->ide_enabled)
        return false;

    // --- IDE data register read (16-bit with latch): 0x7C00-0x7DFF ---
    if (addr >= IDE_DATA_BASE && addr <= IDE_DATA_END)
    {
        if (ide->state != IDE_STATE_READ_DATA)
        {
            *data_out = 0xFF;
            return true;
        }

        if ((addr & 1) == 0)
        {
            // Low byte read — fetch a word from the buffer, latch high byte
            uint8_t lo = 0xFF, hi = 0xFF;
            if (ide->buffer_index < ide->buffer_length)
                lo = ide->sector_buffer[ide->buffer_index++];
            if (ide->buffer_index < ide->buffer_length)
                hi = ide->sector_buffer[ide->buffer_index];
            // Don't increment high byte index yet — it will be read next

            ide->data_latch = hi;
            ide->data_latch_valid = true;
            *data_out = lo;
        }
        else
        {
            // High byte read — return latched value and advance
            *data_out = ide->data_latch_valid ? ide->data_latch : 0xFF;
            ide->data_latch_valid = false;
            if (ide->buffer_index < ide->buffer_length)
                ide->buffer_index++;

            // Check if we've read the entire sector
            if (ide->buffer_index >= ide->buffer_length)
            {
                ide->sectors_remaining--;
                if (ide->sectors_remaining > 0)
                {
                    // Request next sector
                    ide_advance_lba(ide);
                    ide->buffer_index = 0;
                    ide->buffer_length = 0;
                    ide->data_latch_valid = false;
                    usb_read_lba = ide_get_lba(ide);
                    ide->status = ATA_STATUS_BSY;
                    ide->state = IDE_STATE_BUSY;
                    usb_read_requested = true;
                }
                else
                {
                    // All sectors transferred
                    ide->status = ATA_STATUS_DRDY | ATA_STATUS_DSC;
                    ide->state = IDE_STATE_IDLE;
                    ide->data_latch_valid = false;
                }
            }
        }
        return true;
    }

    // --- IDE registers read: 0x7E00-0x7EFF ---
    if (addr >= IDE_REG_BASE && addr <= IDE_REG_END)
    {
        uint8_t reg = addr & 0x0F;
        switch (reg)
        {
        case IDE_REG_DATA:
            *data_out = 0xFF;  // Should use 0x7C00 instead
            break;
        case IDE_REG_ERROR:
            *data_out = ide->error;
            break;
        case IDE_REG_SECTOR_COUNT:
            *data_out = ide->sector_count;
            break;
        case IDE_REG_SECTOR:
            *data_out = ide->sector;
            break;
        case IDE_REG_CYLINDER_LOW:
            *data_out = ide->cylinder_low;
            break;
        case IDE_REG_CYLINDER_HIGH:
            *data_out = ide->cylinder_high;
            break;
        case IDE_REG_DEVICE_HEAD:
            *data_out = ide->device_head;
            break;
        case IDE_REG_STATUS:
            // Reading status clears the error flag (convention)
            *data_out = ide->status;
            break;
        case IDE_REG_ALT_STATUS:
            *data_out = ide->status;
            break;
        default:
            *data_out = 0xFF;
            break;
        }
        return true;
    }

    return false;  // Not an IDE address — return ROM data
}

// -----------------------------------------------------------------------
// USB MSC callbacks (called from TinyUSB on Core 1)
// -----------------------------------------------------------------------

static bool read_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data);
static bool write_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data);

static bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
{
    if (cb_data->csw->status != 0)
        return false;

    usb_block_count = tuh_msc_get_block_count(dev_addr, cb_data->cbw->lun);
    usb_block_size = tuh_msc_get_block_size(dev_addr, cb_data->cbw->lun);

    usb_device_mounted = true;
    return true;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    current_dev_addr = dev_addr;
    current_lun = 0;
    usb_device_mounted = false;
    tuh_msc_inquiry(dev_addr, 0, &inquiry_resp, inquiry_complete_cb, 0);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    (void)dev_addr;
    current_dev_addr = 0;
    current_lun = 0;
    usb_device_mounted = false;
    usb_block_count = 0;
    usb_block_size = 0;
}

static bool read_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
{
    (void)dev_addr;
    usb_read_in_progress = false;

    if (usb_ide_ctx == NULL)
        return false;

    if (cb_data == NULL || cb_data->csw == NULL || cb_data->csw->status != 0)
    {
        usb_ide_ctx->usb_read_failed = true;
        return false;
    }

    // For devices with native sectors > 512 bytes (e.g. 4K), byte_offset
    // is the offset within the native block where our 512-byte ATA sector
    // lives.  It was passed via the user_arg parameter of tuh_msc_read10().
    uint32_t byte_offset = (uint32_t)cb_data->user_arg;

    // Copy the correct 512-byte slice to the IDE sector buffer.
    memcpy(usb_ide_ctx->sector_buffer, &usb_read_buffer[byte_offset], 512);

    __dmb();
    usb_ide_ctx->usb_read_ready = true;
    return true;
}

static bool write_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
{
    (void)dev_addr;
    usb_write_in_progress = false;

    if (usb_ide_ctx == NULL)
        return false;

    if (cb_data == NULL || cb_data->csw == NULL || cb_data->csw->status != 0)
    {
        usb_ide_ctx->usb_write_failed = true;
        return false;
    }

    usb_ide_ctx->usb_write_ready = true;
    return true;
}

// -----------------------------------------------------------------------
// USB task loop (runs on Core 1)
// -----------------------------------------------------------------------
void sunrise_usb_set_ide_ctx(sunrise_ide_t *ide)
{
    usb_ide_ctx = ide;
}

void sunrise_ide_set_device_info(uint32_t block_count, uint32_t block_size,
                                const char *vendor, const char *product,
                                const char *revision)
{
    usb_block_count = block_count;
    usb_block_size = block_size;
    memset(&inquiry_resp, 0, sizeof(inquiry_resp));
    if (vendor) {
        size_t len = strlen(vendor);
        if (len > sizeof(inquiry_resp.vendor_id)) len = sizeof(inquiry_resp.vendor_id);
        memcpy(inquiry_resp.vendor_id, vendor, len);
    }
    if (product) {
        size_t len = strlen(product);
        if (len > sizeof(inquiry_resp.product_id)) len = sizeof(inquiry_resp.product_id);
        memcpy(inquiry_resp.product_id, product, len);
    }
    if (revision) {
        size_t len = strlen(revision);
        if (len > sizeof(inquiry_resp.product_rev)) len = sizeof(inquiry_resp.product_rev);
        memcpy(inquiry_resp.product_rev, revision, len);
    }
}

void __not_in_flash_func(sunrise_usb_task)(void)
{
    // Initialize TinyUSB host stack
    tusb_init();
    tuh_init(0);

    while (true)
    {
        tuh_task();
        service_scc_audio();

        if (usb_ide_ctx == NULL)
            continue;

        // --- Timeout watchdog for in-progress USB transfers ---
        // Slow or stalled USB devices must not leave IDE in permanent BSY.
        // If a transfer exceeds the timeout, treat it as a failure so the
        // MSX driver sees ERR and can retry or report the fault.
        if ((usb_read_in_progress || usb_write_in_progress) && usb_transfer_start_us != 0)
        {
            uint64_t elapsed = time_us_64() - usb_transfer_start_us;
            if (elapsed > USB_TRANSFER_TIMEOUT_US)
            {
                if (usb_read_in_progress)
                {
                    usb_read_in_progress = false;
                    usb_ide_ctx->usb_read_failed = true;
                }
                if (usb_write_in_progress)
                {
                    usb_write_in_progress = false;
                    usb_ide_ctx->usb_write_failed = true;
                }
                usb_transfer_start_us = 0;
            }
        }

        // --- Handle read request from Core 0 ---
        if (usb_read_requested && !usb_read_in_progress && usb_device_mounted)
        {
            usb_read_requested = false;
            usb_read_in_progress = true;
            usb_transfer_start_us = time_us_64();

            uint32_t lba = usb_read_lba;

            // Convert LBA if the USB device has non-512 byte sectors (e.g. 4K).
            // The ATA interface presents a 512-byte sector view to the MSX.
            // For devices with larger native sectors, we read the native sector
            // that contains the requested 512-byte LBA and extract the right
            // 512-byte slice.
            uint32_t native_lba = lba;
            uint32_t byte_offset = 0;
            if (usb_block_size > 512)
            {
                uint32_t sectors_per_block = usb_block_size / 512;
                native_lba = lba / sectors_per_block;
                byte_offset = (lba % sectors_per_block) * 512;
            }

            if (native_lba >= usb_block_count || usb_block_size == 0)
            {
                usb_read_in_progress = false;
                usb_transfer_start_us = 0;
                usb_ide_ctx->usb_read_failed = true;
            }
            else if (!tuh_msc_read10(current_dev_addr, current_lun, usb_read_buffer,
                                      native_lba, 1, read_complete_cb, (uintptr_t)byte_offset))
            {
                usb_read_in_progress = false;
                usb_transfer_start_us = 0;
                usb_ide_ctx->usb_read_failed = true;
            }
        }

        // --- Handle write request from Core 0 ---
        if (usb_write_requested && !usb_write_in_progress && usb_device_mounted)
        {
            usb_write_requested = false;
            usb_write_in_progress = true;
            usb_transfer_start_us = time_us_64();

            uint32_t lba = usb_write_lba;

            // For devices with native block size > 512 bytes, a single
            // 512-byte ATA write cannot be directly mapped to a native
            // block write (would require read-modify-write).  This is
            // extremely rare for USB flash drives; report an error if
            // it ever occurs rather than corrupting data.
            if (usb_block_size != 512)
            {
                usb_write_in_progress = false;
                usb_transfer_start_us = 0;
                usb_ide_ctx->usb_write_failed = true;
            }
            else if (lba >= usb_block_count)
            {
                usb_write_in_progress = false;
                usb_transfer_start_us = 0;
                usb_ide_ctx->usb_write_failed = true;
            }
            else if (!tuh_msc_write10(current_dev_addr, current_lun, usb_write_buffer,
                                       lba, 1, write_complete_cb, 0))
            {
                usb_write_in_progress = false;
                usb_transfer_start_us = 0;
                usb_ide_ctx->usb_write_failed = true;
            }
        }

        // --- Propagate USB completion status to IDE state machine ---
        // Memory barrier ensures sector_buffer writes from the callback
        // are fully committed before we transition the IDE state.
        if (usb_ide_ctx->usb_read_ready)
        {
            usb_ide_ctx->usb_read_ready = false;
            usb_transfer_start_us = 0;
            __dmb();
            usb_ide_ctx->buffer_index = 0;
            usb_ide_ctx->buffer_length = 512;
            usb_ide_ctx->data_latch_valid = false;
            usb_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
            usb_ide_ctx->state = IDE_STATE_READ_DATA;
        }

        if (usb_ide_ctx->usb_read_failed)
        {
            usb_ide_ctx->usb_read_failed = false;
            usb_transfer_start_us = 0;
            usb_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_ERR;
            usb_ide_ctx->error = ATA_ERROR_ABRT;
            usb_ide_ctx->state = IDE_STATE_IDLE;
        }

        if (usb_ide_ctx->usb_write_ready)
        {
            usb_ide_ctx->usb_write_ready = false;
            usb_transfer_start_us = 0;
            ide_advance_lba(usb_ide_ctx);

            if (usb_ide_ctx->sectors_remaining > 0)
            {
                // More sectors to write — set DRQ for next sector
                usb_ide_ctx->buffer_index = 0;
                usb_ide_ctx->data_latch_valid = false;
                usb_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
                usb_ide_ctx->state = IDE_STATE_WRITE_DATA;
            }
            else
            {
                // All sectors written
                usb_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_DSC;
                usb_ide_ctx->state = IDE_STATE_IDLE;
            }
        }

        if (usb_ide_ctx->usb_write_failed)
        {
            usb_ide_ctx->usb_write_failed = false;
            usb_transfer_start_us = 0;
            usb_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_ERR;
            usb_ide_ctx->error = ATA_ERROR_ABRT;
            usb_ide_ctx->state = IDE_STATE_IDLE;
        }

        // --- Complete a pending IDENTIFY after USB mount ---
        if (usb_ide_ctx->usb_identify_pending && usb_device_mounted)
        {
            usb_ide_ctx->usb_identify_pending = false;
            build_identify_data(usb_ide_ctx->sector_buffer);
            __dmb();
            usb_ide_ctx->buffer_index = 0;
            usb_ide_ctx->buffer_length = 512;
            usb_ide_ctx->data_latch_valid = false;
            usb_ide_ctx->status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
            usb_ide_ctx->error = 0;
            usb_ide_ctx->state = IDE_STATE_READ_DATA;
        }
    }
}
