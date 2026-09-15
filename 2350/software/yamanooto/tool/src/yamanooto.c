// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// yamanooto.c - Windows console application to create a Yamanooto UF2 file for the MSX PICOVERSE 2350
//
// This program creates a UF2 file to program the Raspberry Pi Pico with the MSX PICOVERSE 2350
// Yamanooto firmware. The UF2 file combines the Pico firmware binary, a small configuration record
// and the Yamanooto flash-ROM image supplied by the user.
//
// The Yamanooto is a Konami-SCC compatible 8 MB flash cartridge with SCC/SCC+ audio and a
// secondary (dual) PSG. Any Konami-SCC / Konami-4 compatible ROM image (up to 8 MB) can be used.
//
// The configuration record has the following structure:
//   game   - Game name                         - 50 bytes (padded with 0x00)
//   mapp   - Cartridge type (fixed, 1)          - 01 byte
//   size   - Size of the ROM in bytes           - 04 bytes
//   offset - Reserved (0)                        - 04 bytes
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "uf2format.h"
#include <yamanooto.h>
#include <fmpac_bios.h>

#ifndef APP_VERSION
#define APP_VERSION "v1.18"
#endif

#define UF2FILENAME               "yamanooto.uf2"        // default UF2 output file
#define UF2_FLAG_FAMILYID_PRESENT 0x00002000UL           // Signals that fileSize stores the RP2350 family ID
#define RP2350_FAMILY_ID          0xE48BFF59UL           // RP2350 family identifier expected by the bootloader

#define MAX_FILE_NAME_LENGTH      50                     // Maximum length of a ROM name
#define MAX_ROM_SIZE              (8 * 1024 * 1024)      // Yamanooto flash size: 8 MB
#define MIN_ROM_SIZE              8192                    // Minimum sensible ROM size
#define FLASH_START               0x10000000             // Start of flash on the Raspberry Pi Pico
#define YAMANOOTO_ROM_TYPE        1                       // Fixed cartridge type (informational)
#define YAMANOOTO_MSX_MUSIC_FLAG  0x20u                   // 'type' byte flag: expanded slot + FM-PAC (YM2413)
#define CONFIG_RECORD_SIZE        (MAX_FILE_NAME_LENGTH + 1 + sizeof(uint32_t) + sizeof(uint32_t))

static uint32_t file_size(const char *filename)
{
    FILE *file = fopen(filename, "rb");
    if (!file)
        return 0;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fclose(file);
    return (size < 0) ? 0u : (uint32_t)size;
}

// Extract the base filename (without directory or extension) for the ROM name.
static void derive_rom_name(const char *path, char *out, size_t out_size)
{
    const char *base = path;
    for (const char *p = path; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }

    size_t len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot != base)
        len = (size_t)(dot - base);

    if (len >= out_size)
        len = out_size - 1;

    memcpy(out, base, len);
    out[len] = '\0';
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options] <rom_file>\n\n", prog);
    printf("Creates a Yamanooto flash cartridge UF2 for the MSX PICOVERSE 2350.\n\n");
    printf("Options:\n");
    printf("  -o, --output <file>   Output UF2 filename (default: %s)\n", UF2FILENAME);
    printf("  -f, --fmpac           Enable the FM-PAC BIOS: present the cartridge as an\n");
    printf("                        expanded slot with the MSX-MUSIC (YM2413) BIOS in\n");
    printf("                        sub-slot 3. \n");
    printf("  -h, --help            Show this help message\n\n");
    printf("The ROM image must be a Konami-SCC / Konami-4 compatible image up to 8 MB.\n");
    printf("By default the cartridge is a plain slot with SCC/SCC+, FMPAC I/O and dual PSG only.\n");
    printf("Use --fmpac to add FMPAC BIOS; the firmware then selects SCC or FM on the fly.\n");
}

// Build the UF2 file: firmware + config record + ROM image + FM-PAC BIOS.
static int create_uf2_file(const char *rom_filename, uint32_t rom_size,
                           const char *rom_name, const char *uf2_filename,
                           uint8_t type_flags)
{
    const uint8_t *firmware_data = ___pico_yamanooto_dist_yamanooto_bin;
    const size_t firmware_size = sizeof(___pico_yamanooto_dist_yamanooto_bin);

    // The FM-PAC BIOS is always appended right after the ROM payload; the
    // firmware finds it at (rom + config + rom_size).
    const uint8_t *extra_data = ___fmpac_FMPCCMFC_BIN;
    const size_t extra_size = (size_t)___fmpac_FMPCCMFC_BIN_len;

    uint8_t config_record[CONFIG_RECORD_SIZE];
    memset(config_record, 0, sizeof(config_record));

    uint8_t rom_type = (uint8_t)(YAMANOOTO_ROM_TYPE | type_flags);
    uint32_t base_offset = 0;

    size_t cursor = 0;
    memcpy(config_record + cursor, rom_name, strnlen(rom_name, MAX_FILE_NAME_LENGTH));
    cursor += MAX_FILE_NAME_LENGTH;
    memcpy(config_record + cursor, &rom_type, sizeof(rom_type));
    cursor += sizeof(rom_type);
    memcpy(config_record + cursor, &rom_size, sizeof(rom_size));
    cursor += sizeof(rom_size);
    memcpy(config_record + cursor, &base_offset, sizeof(base_offset));

    FILE *rom_file = fopen(rom_filename, "rb");
    if (!rom_file)
    {
        perror("Failed to open ROM file for UF2 creation");
        return 1;
    }

    FILE *uf2_file = fopen(uf2_filename, "wb");
    if (!uf2_file)
    {
        perror("Failed to create UF2 file");
        fclose(rom_file);
        return 1;
    }

    UF2_Block bl;
    memset(&bl, 0, sizeof(bl));
    bl.magicStart0 = UF2_MAGIC_START0;
    bl.magicStart1 = UF2_MAGIC_START1;
    bl.flags = UF2_FLAG_FAMILYID_PRESENT;
    bl.magicEnd = UF2_MAGIC_END;
    bl.targetAddr = FLASH_START;
    bl.payloadSize = 256;

    const size_t total_binary_size = firmware_size + CONFIG_RECORD_SIZE + (size_t)rom_size + extra_size;
    bl.numBlocks = (uint32_t)((total_binary_size + bl.payloadSize - 1) / bl.payloadSize);
    bl.fileSize = RP2350_FAMILY_ID;

    size_t firmware_offset = 0;
    size_t config_offset = 0;
    size_t rom_bytes_written = 0;
    size_t extra_bytes_written = 0;
    size_t total_written = 0;
    uint32_t block_no = 0;
    bool success = false;

    while (total_written < total_binary_size)
    {
        memset(bl.data, 0, sizeof(bl.data));
        size_t chunk_filled = 0;

        while (chunk_filled < bl.payloadSize && total_written < total_binary_size)
        {
            size_t space = bl.payloadSize - chunk_filled;
            size_t to_copy = 0;

            if (firmware_offset < firmware_size)
            {
                size_t remaining = firmware_size - firmware_offset;
                to_copy = remaining < space ? remaining : space;
                memcpy(bl.data + chunk_filled, firmware_data + firmware_offset, to_copy);
                firmware_offset += to_copy;
            }
            else if (config_offset < CONFIG_RECORD_SIZE)
            {
                size_t remaining = CONFIG_RECORD_SIZE - config_offset;
                to_copy = remaining < space ? remaining : space;
                memcpy(bl.data + chunk_filled, config_record + config_offset, to_copy);
                config_offset += to_copy;
            }
            else if (rom_bytes_written < (size_t)rom_size)
            {
                size_t remaining = (size_t)rom_size - rom_bytes_written;
                size_t request = remaining < space ? remaining : space;
                size_t read_now = fread(bl.data + chunk_filled, 1, request, rom_file);
                if (read_now != request)
                {
                    printf("Failed to read ROM data while building UF2 file.\n");
                    goto cleanup;
                }
                rom_bytes_written += read_now;
                to_copy = read_now;
            }
            else
            {
                size_t remaining = extra_size - extra_bytes_written;
                if (remaining == 0)
                    break;
                to_copy = remaining < space ? remaining : space;
                memcpy(bl.data + chunk_filled, extra_data + extra_bytes_written, to_copy);
                extra_bytes_written += to_copy;
            }

            chunk_filled += to_copy;
            total_written += to_copy;
        }

        bl.blockNo = block_no++;
        if (fwrite(&bl, 1, sizeof(bl), uf2_file) != sizeof(bl))
        {
            printf("Failed to write UF2 block %u.\n", block_no - 1);
            goto cleanup;
        }
        bl.targetAddr += bl.payloadSize;
    }

    success = true;

cleanup:
    fclose(rom_file);
    fclose(uf2_file);

    if (success)
    {
        printf("\nSuccessfully wrote %u blocks to %s.\n", block_no, uf2_filename);
        return 0;
    }
    return 1;
}

int main(int argc, char *argv[])
{
    printf("MSX PICOVERSE 2350 Yamanooto UF2 Creator %s\n", APP_VERSION);
    printf("(c) 2026 The Retro Hacker\n\n");

    const char *output_filename = UF2FILENAME;
    const char *rom_filename = NULL;
    uint8_t type_flags = 0;

    for (int i = 1; i < argc; ++i)
    {
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0))
        {
            print_usage(argv[0]);
            return 0;
        }
        else if ((strcmp(argv[i], "-o") == 0) || (strcmp(argv[i], "--output") == 0))
        {
            if (i + 1 >= argc)
            {
                printf("Option -o/--output requires a filename.\n");
                return 1;
            }
            output_filename = argv[++i];
        }
        else if ((strcmp(argv[i], "-f") == 0) || (strcmp(argv[i], "--fmpac") == 0))
        {
            type_flags |= YAMANOOTO_MSX_MUSIC_FLAG;
        }
        else if (argv[i][0] == '-')
        {
            printf("Unknown option: %s\n", argv[i]);
            return 1;
        }
        else if (!rom_filename)
        {
            rom_filename = argv[i];
        }
        else
        {
            printf("Unexpected argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (!rom_filename)
    {
        print_usage(argv[0]);
        return 1;
    }

    uint32_t rom_size = file_size(rom_filename);
    if (rom_size == 0)
    {
        printf("Failed to open or read ROM file: %s\n", rom_filename);
        return 1;
    }
    if (rom_size < MIN_ROM_SIZE)
    {
        printf("ROM file is too small (%u bytes); minimum is %u bytes.\n", rom_size, MIN_ROM_SIZE);
        return 1;
    }
    if (rom_size > MAX_ROM_SIZE)
    {
        printf("ROM file is too large (%u bytes); the Yamanooto flash is %u bytes.\n",
               rom_size, MAX_ROM_SIZE);
        return 1;
    }

    char rom_name[MAX_FILE_NAME_LENGTH];
    derive_rom_name(rom_filename, rom_name, sizeof(rom_name));

    printf("Mode: Yamanooto flash cartridge (Konami-SCC / SCC+ / dual PSG)\n");
    printf("ROM File: %s\n", rom_filename);
    printf("ROM Name: %s\n", rom_name);
    printf("ROM Size: %u bytes\n", rom_size);
    if (type_flags & YAMANOOTO_MSX_MUSIC_FLAG)
    {
        printf("Audio: SCC/SCC+ + dual PSG + MSX-MUSIC (FM-PAC BIOS embedded; auto SCC/FM select)\n");
        printf("Slot: expanded (game sub-slot 0 + FM-PAC sub-slot 3)\n");
    }
    else
    {
        printf("Audio: SCC/SCC+ + dual PSG + MSX-MUSIC raw I/O (no FM-PAC BIOS)\n");
        printf("Slot: plain (non-expanded), like real Yamanooto hardware\n");
    }
    printf("UF2 Output: %s\n", output_filename);

    return create_uf2_file(rom_filename, rom_size, rom_name, output_filename, type_flags);
}
