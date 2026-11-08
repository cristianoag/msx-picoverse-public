// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// sunrise_ide.h - Sunrise IDE emulation for MSX PicoVerse (USB host via TinyUSB)
//
// Emulates the Sunrise MSX IDE interface by translating ATA register commands
// into USB Mass Storage Class operations on the Pico's USB-C port.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef SUNRISE_IDE_H
#define SUNRISE_IDE_H

#include <stdbool.h>
#include <stdint.h>

// -----------------------------------------------------------------------
// Sunrise IDE address map constants
// -----------------------------------------------------------------------
// Segment/IDE control register: write-only at 0x4104
//   bits[7:5] = FlashROM segment number (BIT-REVERSED: bit7=seg0, bit6=seg1, bit5=seg2)
//   bit[0]    = IDE registers enable (0=disabled, 1=enabled)
//   Note: The Nextor chgbnk routine reverses the bank number bits before
//   writing, so we must reverse bits[7:5] back to get the actual page.
#define SUNRISE_CTRL_REG_ADDR   0x4104u

// IDE data register (16-bit, low/high byte latch): 0x7C00-0x7DFF
#define IDE_DATA_BASE           0x7C00u
#define IDE_DATA_END            0x7DFFu

// IDE registers (8-bit): 0x7E00-0x7E0F (mirrored up to 0x7EFF)
#define IDE_REG_BASE            0x7E00u
#define IDE_REG_END             0x7EFFu

// Individual IDE register offsets (relative to 0x7E00, mirrored every 16)
#define IDE_REG_DATA            0x00  // Data Register (use 0x7C00 instead)
#define IDE_REG_ERROR           0x01  // r: Error Register
#define IDE_REG_FEATURE         0x01  // w: Feature Register
#define IDE_REG_SECTOR_COUNT    0x02  // r/w: Sector Count
#define IDE_REG_SECTOR          0x03  // r/w: Sector Number (LBA bits 0-7)
#define IDE_REG_CYLINDER_LOW    0x04  // r/w: Cylinder Low (LBA bits 8-15)
#define IDE_REG_CYLINDER_HIGH   0x05  // r/w: Cylinder High (LBA bits 16-23)
#define IDE_REG_DEVICE_HEAD     0x06  // r/w: Device/Head (LBA bits 24-27 + flags)
#define IDE_REG_STATUS          0x07  // r: Status Register
#define IDE_REG_COMMAND         0x07  // w: Command Register
#define IDE_REG_ALT_STATUS      0x0E  // r: Alternate Status Register
#define IDE_REG_DEVICE_CTRL     0x0E  // w: Device Control Register

// ATA status register bits
#define ATA_STATUS_ERR          0x01  // Error occurred
#define ATA_STATUS_DRQ          0x08  // Data Request
#define ATA_STATUS_DSC          0x10  // Device Seek Complete
#define ATA_STATUS_DRDY         0x40  // Device Ready
#define ATA_STATUS_BSY          0x80  // Busy

// ATA error register bits
#define ATA_ERROR_ABRT          0x04  // Aborted command

// ATA commands (subset used by Nextor Sunrise IDE driver)
#define ATA_CMD_DEVICE_RESET    0x08
#define ATA_CMD_RECALIBRATE     0x10
#define ATA_CMD_READ_SECTORS    0x20
#define ATA_CMD_WRITE_SECTORS   0x30
#define ATA_CMD_DEVICE_DIAG     0x90  // EXECUTE DEVICE DIAGNOSTIC
#define ATA_CMD_INIT_PARAMS     0x91
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_SET_FEATURES    0xEF

// ATA diagnostic codes (returned in error register after DEVDIAG/reset)
#define ATA_DIAG_NO_ERROR       0x01  // No error detected

// Device/Head register bit masks
#define ATA_DEV_HEAD_DEV        0x10  // Device select (0=master, 1=slave)
#define ATA_DEV_HEAD_LBA        0x40  // LBA mode
#define ATA_DEV_HEAD_HEAD_MASK  0x0F  // Head number / LBA bits 24-27

// -----------------------------------------------------------------------
// IDE state machine
// -----------------------------------------------------------------------
typedef enum {
    IDE_STATE_IDLE,         // Waiting for command
    IDE_STATE_READ_DATA,    // Data ready for MSX to read (DRQ set)
    IDE_STATE_WRITE_DATA,   // Waiting for MSX to write data (DRQ set)
    IDE_STATE_BUSY,         // USB operation in progress
} ide_state_t;

// -----------------------------------------------------------------------
// Sunrise IDE context (shared between cores)
// -----------------------------------------------------------------------
typedef struct {
    // Segment/IDE control
    volatile uint8_t  segment;           // Current FlashROM segment (0-7)
    volatile bool     ide_enabled;       // IDE registers visible in address space

    // Shadow ATA registers (written by MSX)
    volatile uint8_t  feature;           // Feature register
    volatile uint8_t  sector_count;      // Sector Count register
    volatile uint8_t  sector;            // Sector Number / LBA[7:0]
    volatile uint8_t  cylinder_low;      // Cylinder Low / LBA[15:8]
    volatile uint8_t  cylinder_high;     // Cylinder High / LBA[23:16]
    volatile uint8_t  device_head;       // Device/Head / LBA[27:24]

    // Status/Error registers (read by MSX)
    volatile uint8_t  status;            // Current status
    volatile uint8_t  error;             // Current error

    // Data register latch (16-bit with low/high byte split)
    volatile uint8_t  data_latch;        // Temporary byte for low/high byte mechanism
    volatile bool     data_latch_valid;  // Low byte has been written/read

    // Sector data buffer (512 bytes per sector)
    uint8_t           sector_buffer[512];
    volatile uint16_t buffer_index;      // Current position in sector_buffer
    volatile uint16_t buffer_length;     // How many bytes are valid
    volatile uint16_t sectors_remaining; // Sectors left in multi-sector xfer (0=256 in ATA)

    // IDE state machine
    volatile ide_state_t state;

    // USB block read/write status (set by core 1, consumed by core 0)
    volatile bool     usb_read_ready;    // A block read completed successfully
    volatile bool     usb_read_failed;   // A block read failed
    volatile bool     usb_write_ready;   // A block write completed successfully
    volatile bool     usb_write_failed;  // A block write failed
    volatile bool     usb_identify_pending; // IDENTIFY waiting for USB mount
} sunrise_ide_t;

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// Initialize the Sunrise IDE context to power-on defaults.
void sunrise_ide_init(sunrise_ide_t *ide);

// Handle a write to an address in 0x4000-0x7FFF.
// Returns true if the write was consumed by the Sunrise controller (not ROM data).
bool __not_in_flash_func(sunrise_ide_handle_write)(sunrise_ide_t *ide, uint16_t addr, uint8_t data);

// Handle a read from an address in 0x4000-0x7FFF.
// Returns true if the read should return *data_out instead of ROM data.
bool __not_in_flash_func(sunrise_ide_handle_read)(sunrise_ide_t *ide, uint16_t addr, uint8_t *data_out);

// USB host task loop — runs on Core 1.
// Initialises TinyUSB, polls for MSC devices, and services IDE read/write requests.
void __not_in_flash_func(sunrise_usb_task)(void);

// Set the pointer to the shared IDE context for the USB task (call before launching core 1).
void sunrise_usb_set_ide_ctx(sunrise_ide_t *ide);

// Populate IDENTIFY DEVICE fields for non-USB backends (e.g. SD card).
// Sets the block count, block size, and SCSI-style vendor/product/revision
// strings used by build_identify_data().  Call before setting usb_device_mounted.
void sunrise_ide_set_device_info(uint32_t block_count, uint32_t block_size,
                                const char *vendor, const char *product,
                                const char *revision);

// Feed one SCC audio buffer when SCC/I2S is enabled for loader modes that
// use Core 1 for storage backends instead of a dedicated audio worker.
void __not_in_flash_func(service_scc_audio)(void);

#endif // SUNRISE_IDE_H
