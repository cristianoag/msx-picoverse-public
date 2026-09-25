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
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef SUNRISE_DSK_H
#define SUNRISE_DSK_H

#include <stdint.h>
#include "sunrise_ide.h"

#define SUNRISE_DSK_SECTOR_SIZE  512u
#define SUNRISE_DSK_MAX_EXTENTS  16u

// A contiguous run of image sectors and the FatFs (volume-relative) LBA where
// it starts on the microSD card. Used to write image sectors back in place.
typedef struct {
    uint32_t image_sector;
    uint32_t lba;
    uint32_t count;
} sunrise_dsk_extent_t;

// Set the pointer to the shared IDE context (call before launching core 1).
void sunrise_dsk_set_ide_ctx(sunrise_ide_t *ide);

// Attach the PSRAM-staged image. The extent table is referenced, not copied,
// so it must stay valid while the image runs. extent_count == 0 makes the
// disk read-only (writes are rejected with an ATA abort).
void sunrise_dsk_attach_image(uint8_t *image, uint32_t sector_count,
                              const sunrise_dsk_extent_t *extents, uint8_t extent_count);

// Disk-image task loop - runs on Core 1 and services IDE read/write requests.
void __not_in_flash_func(sunrise_dsk_task)(void);

#endif // SUNRISE_DSK_H
