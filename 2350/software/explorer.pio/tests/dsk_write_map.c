// Host tests for dsk_build_write_map() in pico/explorer/explorer.c.
// The production function runs against the real FatFs (ff15) on a RAM disk, so
// the cluster-chain -> card LBA math is checked against actual FAT16 and exFAT
// layouts, including fragmented and read-only images.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __not_in_flash_func(name) name
#include "ff.h"
#include "diskio.h"
#include "sunrise_dsk.h"

#define DISK_SECTORS (48u * 1024u * 1024u / 512u)
#define IMAGE_SIZE   (720u * 1024u)

static uint8_t *ram_disk;

DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    assert(sector + count <= DISK_SECTORS);
    memcpy(buff, ram_disk + sector * 512u, count * 512u);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    assert(sector + count <= DISK_SECTORS);
    memcpy(ram_disk + sector * 512u, buff, count * 512u);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = DISK_SECTORS; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (1u << 21) | (1u << 16); }

// Globals the production function reads from explorer.c.
#define SD_PATH_BUFFER_SIZE 128
static char sd_path_buffer[SD_PATH_BUFFER_SIZE];
static uint16_t sd_path_offsets[1];
static uint16_t full_record_count = 1;

#include "dsk-map-production.h"

static FATFS fs;
static uint8_t work[4096];

static uint8_t pattern(uint32_t sector, uint32_t offset) { return (uint8_t)(sector * 7u + offset * 13u + 1u); }

static void format(BYTE fmt, DWORD au)
{
    memset(ram_disk, 0, DISK_SECTORS * 512u);
    MKFS_PARM opt = { fmt | FM_SFD, 0, 0, 0, au };
    assert(f_mkfs("", &opt, work, sizeof(work)) == FR_OK);
    assert(f_mount(&fs, "", 1) == FR_OK);
}

// Writes the image in `chunk` sized pieces. When `fragment` is set, a filler
// file grabs a cluster between pieces so the image's cluster chain is split.
static void write_image(const char *path, uint32_t chunk, bool fragment)
{
    FIL img, filler;
    static uint8_t buf[64u * 1024u];
    assert(chunk <= sizeof(buf) && chunk % 512u == 0);
    assert(f_open(&img, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    if (fragment)
        assert(f_open(&filler, "FILLER.BIN", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    for (uint32_t pos = 0; pos < IMAGE_SIZE; pos += chunk) {
        for (uint32_t i = 0; i < chunk; i++)
            buf[i] = pattern((pos + i) / 512u, (pos + i) % 512u);
        UINT bw;
        assert(f_write(&img, buf, chunk, &bw) == FR_OK && bw == chunk);
        assert(f_sync(&img) == FR_OK);
        if (fragment) {
            assert(f_write(&filler, buf, 512, &bw) == FR_OK && bw == 512);
            assert(f_sync(&filler) == FR_OK);
        }
    }
    assert(f_close(&img) == FR_OK);
    if (fragment)
        assert(f_close(&filler) == FR_OK);
}

static uint8_t build_map(const char *path, sunrise_dsk_extent_t *extents)
{
    strcpy(sd_path_buffer, path);
    sd_path_offsets[0] = 0;
    return dsk_build_write_map(0, IMAGE_SIZE, extents);
}

static uint32_t map_sector(const sunrise_dsk_extent_t *extents, uint8_t count, uint32_t sector)
{
    for (uint8_t i = 0; i < count; i++)
        if (sector >= extents[i].image_sector && sector - extents[i].image_sector < extents[i].count)
            return extents[i].lba + (sector - extents[i].image_sector);
    assert(!"image sector not covered by the write map");
    return 0;
}

static void verify_map(const char *path, const sunrise_dsk_extent_t *extents, uint8_t count)
{
    uint32_t next = 0;
    for (uint8_t i = 0; i < count; i++) {
        assert(extents[i].image_sector == next && extents[i].count > 0);
        next += extents[i].count;
    }
    assert(next >= IMAGE_SIZE / 512u);

    // Every image sector maps to the card sector holding its data.
    for (uint32_t s = 0; s < IMAGE_SIZE / 512u; s++) {
        const uint8_t *card = ram_disk + (size_t)map_sector(extents, count, s) * 512u;
        for (uint32_t o = 0; o < 512u; o++)
            assert(card[o] == pattern(s, o));
    }

    // A write through the map is what FatFs then reads back from the file.
    const uint32_t target = IMAGE_SIZE / 512u - 3u;
    uint8_t sector[512];
    memset(sector, 0xC3, sizeof(sector));
    assert(disk_write(0, sector, map_sector(extents, count, target), 1) == RES_OK);
    FIL fil;
    UINT br;
    uint8_t back[512];
    assert(f_open(&fil, path, FA_READ) == FR_OK);
    assert(f_lseek(&fil, (FSIZE_t)target * 512u) == FR_OK);
    assert(f_read(&fil, back, sizeof(back), &br) == FR_OK && br == sizeof(back));
    assert(memcmp(back, sector, sizeof(back)) == 0);
    assert(f_close(&fil) == FR_OK);
}

static void run_case(const char *label, BYTE fmt, DWORD au, uint32_t chunk, bool fragment,
                     uint8_t min_extents, uint8_t max_extents)
{
    sunrise_dsk_extent_t extents[SUNRISE_DSK_MAX_EXTENTS];
    format(fmt, au);
    write_image("GAME.DSK", chunk, fragment);
    static uint8_t *before;
    if (!before)
        before = malloc(DISK_SECTORS * 512u);
    assert(before);
    memcpy(before, ram_disk, DISK_SECTORS * 512u);
    uint8_t count = build_map("/GAME.DSK", extents);
    // The write-mode open used to probe writability must not touch the card.
    assert(memcmp(before, ram_disk, DISK_SECTORS * 512u) == 0);
    printf("%s: %u extent(s)\n", label, (unsigned)count);
    if (max_extents == 0) {
        assert(count == 0);
    } else {
        assert(count >= min_extents && count <= max_extents);
        verify_map("/GAME.DSK", extents, count);
    }
    assert(f_unmount("") == FR_OK);
}

static void test_read_only(void)
{
    sunrise_dsk_extent_t extents[SUNRISE_DSK_MAX_EXTENTS];
    format(FM_FAT, 2048);
    write_image("GAME.DSK", 64u * 1024u, false);
    assert(f_chmod("GAME.DSK", AM_RDO, AM_RDO) == FR_OK);
    assert(build_map("/GAME.DSK", extents) == 0);
    assert(build_map("/MISSING.DSK", extents) == 0);
    sd_path_offsets[0] = 0xFFFF;
    assert(dsk_build_write_map(0, IMAGE_SIZE, extents) == 0);
    assert(f_unmount("") == FR_OK);
    puts("PASS: read-only, missing and path-less images map no extents (read-only boot)");
}

int main(void)
{
    ram_disk = malloc(DISK_SECTORS * 512u);
    assert(ram_disk);
    run_case("FAT16 contiguous", FM_FAT, 2048, 64u * 1024u, false, 1, 1);
    run_case("FAT16 fragmented", FM_FAT, 2048, 64u * 1024u, true, 2, SUNRISE_DSK_MAX_EXTENTS);
    run_case("FAT16 over-fragmented", FM_FAT, 2048, 8u * 1024u, true, 0, 0);
    run_case("FAT32 contiguous", FM_FAT32, 512, 64u * 1024u, false, 1, 1);
    run_case("FAT32 fragmented", FM_FAT32, 512, 64u * 1024u, true, 2, SUNRISE_DSK_MAX_EXTENTS);
    run_case("exFAT contiguous", FM_EXFAT, 4096, 64u * 1024u, false, 1, 1);
    run_case("exFAT fragmented", FM_EXFAT, 4096, 64u * 1024u, true, 2, SUNRISE_DSK_MAX_EXTENTS);
    puts("PASS: FAT16/FAT32/exFAT link maps address the image's card sectors; writes read back via FatFs");
    test_read_only();
    free(ram_disk);
    puts("PASS: DSK write map");
    return 0;
}
