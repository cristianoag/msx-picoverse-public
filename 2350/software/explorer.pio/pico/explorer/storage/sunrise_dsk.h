// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// sunrise_dsk.h - Sunrise IDE disk-image (.DSK) backend for MSX PicoVerse
//
// Exposes a .DSK floppy image, staged in PSRAM, as the Sunrise IDE device so
// the embedded Nextor kernel boots it as an unpartitioned FAT12 volume.
// Reads are served from the PSRAM copy; writes update the PSRAM copy and are
// written through to the image file on the microSD card.
//
// A .DSK holding several disks joined back to back is exposed instead as
// [partition table][emulation data][disk 1][disk 2]...: the partition table
// carries Nextor 2.1's persistent disk-emulation pointer, so Nextor boots
// disk 1 in disk emulation mode and the number keys swap disks.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef SUNRISE_DSK_H
#define SUNRISE_DSK_H

#include <stdint.h>
#include "sunrise_ide.h"

#define SUNRISE_DSK_SECTOR_SIZE  512u
#define SUNRISE_DSK_MAX_EXTENTS  16u
// Nextor disk emulation mode accepts up to 32 images (keys 1-9, then A-W).
#define SUNRISE_DSK_MAX_DISKS    32u
// Generated sectors placed before a multi-disk image: the partition table
// (sector 0) and the Nextor emulation data (sector 1).
#define SUNRISE_DSK_HEADER_SECTORS 2u
// Standard MSX floppy sizes in sectors: 360KB (1DD) and 720KB (2DD).
#define SUNRISE_DSK_SECTORS_360K 720u
#define SUNRISE_DSK_SECTORS_720K 1440u

// A contiguous run of image sectors and the FatFs (volume-relative) LBA where
// it starts on the microSD card. Used to write image sectors back in place.
typedef struct {
    uint32_t image_sector;
    uint32_t lba;
    uint32_t count;
} sunrise_dsk_extent_t;

// Set the pointer to the shared IDE context (call before launching core 1).
void sunrise_dsk_set_ide_ctx(sunrise_ide_t *ide);

// Attach the PSRAM-staged device. `device` holds all `device_sectors`; the
// .DSK file starts `file_sector_base` sectors in (0 for a single disk,
// SUNRISE_DSK_HEADER_SECTORS for a multi-disk image). Writes below the base
// only update PSRAM. The extent table maps file sectors to the card; it is
// referenced, not copied, so it must stay valid while the image runs.
// extent_count == 0 makes the file read-only (writes to it are rejected
// with an ATA abort).
void sunrise_dsk_attach_image(uint8_t *device, uint32_t device_sectors, uint32_t file_sector_base,
                              const sunrise_dsk_extent_t *extents, uint8_t extent_count);

// Split a joined image of `image_sectors` into its disks, writing each disk's
// size (720 or 1440 sectors) to disk_sectors. Each disk's size comes from its
// boot sector (BPB), else its FAT media byte (F8h = 360KB, F9h = 720KB), else
// an even split of what is left. Returns the disk count, or 0 when the image
// is not a whole number of 360KB/720KB disks or has more than max_disks.
uint8_t sunrise_dsk_split_disks(const uint8_t *image, uint32_t image_sectors,
                                uint16_t *disk_sectors, uint8_t max_disks);

// Build the SUNRISE_DSK_HEADER_SECTORS generated sectors for a multi-disk
// image: sector 0 is a partition table whose first entry holds Nextor 2.1's
// persistent disk-emulation pointer (and also maps disk 1, so a normal boot
// still mounts it); sector 1 is the Nextor emulation data listing each disk.
void sunrise_dsk_build_emulation_header(uint8_t *header, const uint16_t *disk_sectors, uint8_t disk_count);

// Disk-image task loop - runs on Core 1 and services IDE read/write requests.
void __not_in_flash_func(sunrise_dsk_task)(void);

#endif // SUNRISE_DSK_H
