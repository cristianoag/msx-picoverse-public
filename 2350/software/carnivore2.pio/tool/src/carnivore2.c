// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// carnivore2.c - Windows console application to create the Carnivore2 UF2 for the MSX PICOVERSE 2350.
//
// The UF2 holds the Carnivore2 firmware image at the start of the RP2350 flash. With -f the 8 MB
// Carnivore2 FlashROM image (Carnivore2.rom from the RBSC repository, or a c2backup dump) is
// placed at flash offset 4 MB, which is where the firmware reads the emulated FlashROM from.
// Without -f the UF2 only updates the firmware and keeps the FlashROM contents already stored in
// the cartridge (uploaded ROMs, boot menu settings).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "uf2format.h"
#include "carnivore2.h"

#ifndef APP_VERSION
#define APP_VERSION "v1.1"
#endif

#define UF2FILENAME               "carnivore2.uf2"
#define UF2_FLAG_FAMILYID_PRESENT 0x00002000UL
#define RP2350_FAMILY_ID          0xE48BFF59UL
#define FLASH_START               0x10000000UL
#define FLASH_IMAGE_OFFSET        0x00400000UL      // must match C2_FLASH_IMAGE_OFFSET in the firmware
#define FLASH_IMAGE_SIZE          0x00800000UL
#define PAYLOAD_SIZE              256u

static void usage(const char *name)
{
    printf("Creates a Carnivore2 UF2 for the MSX PICOVERSE 2350.\n\n");
    printf("Usage: %s [options]\n\n", name);
    printf("  -f, --flash <file>    Carnivore2 FlashROM image (up to 8 MB, padded with 0xFF) to install in the\n");
    printf("                        cartridge. Omit it to update only the firmware and keep the stored FlashROM.\n");
    printf("  -o, --output <file>   Output UF2 filename (default: %s)\n", UF2FILENAME);
    printf("  -h, --help            Show this help\n");
}

typedef struct {
    FILE *out;
    UF2_Block block;
    uint32_t block_no;
    uint32_t num_blocks;
} uf2_writer_t;

static bool emit(uf2_writer_t *w, uint32_t address, const uint8_t *data, size_t length)
{
    w->block.targetAddr = address;
    w->block.payloadSize = PAYLOAD_SIZE;
    w->block.blockNo = w->block_no++;
    memset(w->block.data, 0, sizeof(w->block.data));
    memcpy(w->block.data, data, length);
    return fwrite(&w->block, 1, sizeof(w->block), w->out) == sizeof(w->block);
}

int main(int argc, char **argv)
{
    const char *flash_name = NULL;
    const char *output = UF2FILENAME;

    printf("MSX PICOVERSE 2350 Carnivore2 UF2 Creator %s\n", APP_VERSION);

    for (int i = 1; i < argc; ++i)
    {
        if ((!strcmp(argv[i], "-f") || !strcmp(argv[i], "--flash")) && i + 1 < argc) flash_name = argv[++i];
        else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) output = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { printf("Unknown or incomplete option: %s\n\n", argv[i]); usage(argv[0]); return 1; }
    }

    const uint8_t *firmware = ___pico_carnivore2_dist_carnivore2_bin;
    size_t firmware_size = sizeof(___pico_carnivore2_dist_carnivore2_bin);
    if (firmware_size >= FLASH_IMAGE_OFFSET)
    {
        printf("The firmware (%zu bytes) overlaps the FlashROM area.\n", firmware_size);
        return 1;
    }

    FILE *flash_file = NULL;
    size_t flash_size = 0;
    if (flash_name)
    {
        flash_file = fopen(flash_name, "rb");
        if (!flash_file) { perror("Failed to open the FlashROM image"); return 1; }
        fseek(flash_file, 0, SEEK_END);
        long size = ftell(flash_file);
        fseek(flash_file, 0, SEEK_SET);
        if (size <= 0 || (unsigned long)size > FLASH_IMAGE_SIZE)
        {
            printf("The FlashROM image must be between 1 byte and 8 MB (got %ld bytes).\n", size);
            fclose(flash_file);
            return 1;
        }
        flash_size = (size_t)size;
    }

    FILE *out = fopen(output, "wb");
    if (!out) { perror("Failed to create the UF2 file"); if (flash_file) fclose(flash_file); return 1; }

    uf2_writer_t w;
    memset(&w, 0, sizeof(w));
    w.out = out;
    w.block.magicStart0 = UF2_MAGIC_START0;
    w.block.magicStart1 = UF2_MAGIC_START1;
    w.block.magicEnd = UF2_MAGIC_END;
    w.block.flags = UF2_FLAG_FAMILYID_PRESENT;
    w.block.fileSize = RP2350_FAMILY_ID;

    size_t firmware_blocks = (firmware_size + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;
    size_t flash_blocks = flash_file ? FLASH_IMAGE_SIZE / PAYLOAD_SIZE : 0;
    w.block.numBlocks = (uint32_t)(firmware_blocks + flash_blocks);

    bool ok = true;
    for (size_t b = 0; ok && b < firmware_blocks; ++b)
    {
        size_t offset = b * PAYLOAD_SIZE;
        size_t length = firmware_size - offset < PAYLOAD_SIZE ? firmware_size - offset : PAYLOAD_SIZE;
        ok = emit(&w, (uint32_t)(FLASH_START + offset), firmware + offset, length);
    }

    if (ok && flash_file)
    {
        uint8_t chunk[PAYLOAD_SIZE];
        for (size_t b = 0; ok && b < flash_blocks; ++b)
        {
            size_t offset = b * PAYLOAD_SIZE;
            memset(chunk, 0xFF, sizeof(chunk));
            if (offset < flash_size)
            {
                size_t want = flash_size - offset < PAYLOAD_SIZE ? flash_size - offset : PAYLOAD_SIZE;
                if (fread(chunk, 1, want, flash_file) != want)
                {
                    printf("Failed to read the FlashROM image.\n");
                    ok = false;
                    break;
                }
            }
            ok = emit(&w, (uint32_t)(FLASH_START + FLASH_IMAGE_OFFSET + offset), chunk, PAYLOAD_SIZE);
        }
    }

    fclose(out);
    if (flash_file) fclose(flash_file);

    if (!ok) { printf("Failed to write %s.\n", output); return 1; }

    printf("Firmware: %zu bytes\n", firmware_size);
    if (flash_file) printf("FlashROM: %zu bytes installed at flash offset 0x%lX\n", flash_size, FLASH_IMAGE_OFFSET);
    else printf("FlashROM: not included (the FlashROM stored in the cartridge is kept)\n");
    printf("\nSuccessfully wrote %u blocks to %s.\n", w.block_no, output);
    return 0;
}
