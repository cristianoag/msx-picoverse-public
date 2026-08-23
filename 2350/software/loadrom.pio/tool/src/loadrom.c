// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// loadrom.c - Windows console application to create a loadrom UF2 file for the MSX PICOVERSE 2350
//
// This program creates a UF2 file to program the Raspberry Pi Pico with the MSX PICOVERSE 2350 loadROM firmware. The UF2 file is
// created with the combined PICO firmware binary file, the configuration area and the ROM file. The configuration area contains the
// information of the ROM file processed by the tool so the MSX can have the required information to load the ROM and execute.
// 
// The configuration record has the following structure:
//  game - Game name                            - 20 bytes (padded by 0x00)
//  mapp - Mapper code                          - 01 byte  (1 - Plain16, 2 - Plain32, 3 - KonamiSCC, 4 - Planar48, 5 - ASCII8, 6 - ASCII16, 7 - Konami, 8 - NEO8, 9 - NEO16, 10 - Sunrise, 11 - Sunrise+Mapper, 12 - ASCII16-X, 13 - Planar64, 14 - Manbow2, 15 - Sunrise SD, 16 - Sunrise SD+Mapper, 17 - Carnivore2 SD, 18 - Carnivore2 USB, 19 - MegaRAM SD, 20 - MegaRAM USB)
//  size - Size of the ROM in bytes             - 04 bytes 
//  offset - Offset of the game in the flash    - 04 bytes 
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License. 
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <ctype.h>
#include "uf2format.h"
#include "loadrom.h"
#include "nextor_sunrise.h"
#include "esp8266p_rom.h"
#include "fmpac_bios.h"
#include "opl4_fw.h"
#include "opl4_fw_22k.h"
#include "yrw801_rom.h"
#include "msxaudio_fw.h"
#include "msxaudio_bios.h"
#include "sha1.h"
#include "romdb.h"

#ifndef APP_VERSION
#define APP_VERSION "v1.0"
#endif

#define UF2FILENAME             "loadrom.uf2"          // this is the UF2 file to program the Raspberry Pi Pico
#define UF2_FLAG_FAMILYID_PRESENT 0x00002000UL           // Signals that fileSize stores the RP2350 family ID
#define RP2350_FAMILY_ID        0xE48BFF59UL             // RP2350 family identifier expected by UF2 bootloader

#define MAX_FILE_NAME_LENGTH    50              // Maximum length of a ROM name
#define MAX_ROM_SIZE            16*1024*1024    // Maximum size of a ROM file
#define FLASH_START             0x10000000      // Start of the flash memory on the Raspberry Pi Pico
#define MIN_ROM_SIZE            8192           // Minimum size of a ROM file
#define MAX_ANALYSIS_SIZE       131072         // 128KB for the mapper analysis
#define CONFIG_RECORD_SIZE      (MAX_FILE_NAME_LENGTH + 1 + sizeof(uint32_t) + sizeof(uint32_t))


uint32_t file_size(const char *filename);
uint8_t detect_rom_type(const char *filename, uint32_t size);
void write_padding(FILE *file, size_t current_size, size_t target_size, uint8_t padding_byte);
void create_uf2_file(const char *rom_filename, const uint8_t *embedded_rom, uint32_t rom_size,
                     const uint8_t *extra_embedded_rom, uint32_t extra_rom_size, uint8_t rom_type,
                     const char *rom_name, uint32_t base_offset, const char *uf2_filename);

static const char *MAPPER_DESCRIPTIONS[] = {
    "PLA-16", "PLA-32", "KonSCC", "PLN-48", "ASC-08",
    "ASC-16", "Konami", "NEO-8", "NEO-16", "SYSTEM", "SYSTEM", "ASC-16X", "PLN-64", "MANBW2",
    "SYSTEM", "SYSTEM", "SYSTEM", "SYSTEM", "SYSTEM", "SYSTEM"
};

static const char *rom_types[] = {
    "Unknown",
    "Plain16",
    "Plain32",
    "Konami SCC",
    "Planar48",
    "ASCII8",
    "ASCII16",
    "Konami",
    "NEO8",
    "NEO16",
    "Sunrise USB",
    "Sunrise USB+Mapper",
    "ASCII16-X",
    "Planar64",
    "Manbow2",
    "Sunrise SD",
    "Sunrise SD+Mapper",
    "Carnivore2 SD",
    "Carnivore2 USB",
    "MegaRAM SD",
    "MegaRAM USB"
};

#define ROM_TYPE_SUNRISE 10
#define ROM_TYPE_SUNRISE_MAPPER 11
#define ROM_TYPE_ASCII16X 12
#define ROM_TYPE_PLANAR64 13
#define ROM_TYPE_MANBOW2 14
#define ROM_TYPE_SUNRISE_SD 15
#define ROM_TYPE_SUNRISE_MAPPER_SD 16
#define ROM_TYPE_C2_SD 17
#define ROM_TYPE_C2_USB 18
#define ROM_TYPE_MEGARAM_SD 19
#define ROM_TYPE_MEGARAM_USB 20

#define ROM_TYPE_DUAL_PSG_FLAG 0x10
#define ROM_TYPE_MSX_MUSIC_FLAG 0x20
#define ROM_TYPE_WIFI_FLAG 0x20

#define MAPPER_DESCRIPTION_COUNT (sizeof(MAPPER_DESCRIPTIONS) / sizeof(MAPPER_DESCRIPTIONS[0]))

static bool equals_ignore_case(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static uint8_t mapper_number_from_description(const char *description) {
    for (size_t i = 0; i < MAPPER_DESCRIPTION_COUNT; ++i) {
        if (equals_ignore_case(description, MAPPER_DESCRIPTIONS[i])) {
            return (uint8_t)(i + 1);
        }
    }

    // Backward-compatible aliases for older tags and verbose planar names.
    if (equals_ignore_case(description, "PL-16")) {
        return 1;
    }
    if (equals_ignore_case(description, "PL-32")) {
        return 2;
    }
    if (equals_ignore_case(description, "PL-48") || equals_ignore_case(description, "PLN-32") || equals_ignore_case(description, "PLANAR32") || equals_ignore_case(description, "LINEAR") || equals_ignore_case(description, "LINEAR0") || equals_ignore_case(description, "PLANAR48")) {
        return 4;
    }
    if (equals_ignore_case(description, "PL-64") || equals_ignore_case(description, "PLANAR64")) {
        return ROM_TYPE_PLANAR64;
    }
    if (equals_ignore_case(description, "MANBOW2") || equals_ignore_case(description, "MBW-2")) {
        return ROM_TYPE_MANBOW2;
    }

    return 0;
}

// Return byte length of a file on disk, 0 on failure.
uint32_t file_size(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        return 0;
    }
    fseek(file, 0, SEEK_END);
    uint32_t size = ftell(file);
    fclose(file);
    return size;
}

// Return a textual description of the mapper type given its number.
const char* mapper_description(int number) {
    if (number <= 0 || (size_t)number > MAPPER_DESCRIPTION_COUNT) {
        return "Unknown";
    }
    return MAPPER_DESCRIPTIONS[number - 1];
}

// Attempt to guess the mapper type from the ROM contents.
// Returns the mapper byte expected by the firmware (0 signals unsupported/unknown).
// Code adapted from openMSX mapper detection routines.
uint8_t detect_rom_type(const char *filename, uint32_t size) {
    
    // Define the NEO8 signature
    const char neo8_signature[] = "ROM_NEO8";
    const char neo16_signature[] = "ROM_NE16";
    const char ascii16x_signature[] = "ASCII16X";

    // Scores for each mapper type (unit weights, matching openMSX guessRomType)
    unsigned int konami_score = 0;
    unsigned int konami_scc_score = 0;
    unsigned int ascii8_score = 0;
    unsigned int ascii16_score = 0;

    //size_t size = file_size(filename);
    if (size > MAX_ROM_SIZE || size < MIN_ROM_SIZE) {
        printf("Invalid ROM size\n");
        return 0; // unknown mapper
    }

    FILE *file = fopen(filename, "rb");
    if (!file) {
        printf("Failed to open ROM file\n");
        return 0; // unknown mapper
    }

    // openMSX-style: inspect the full ROM for mapper-write patterns.
    size_t read_size = size;
    // Dynamically allocate memory for the ROM
    uint8_t *rom = (uint8_t *)malloc(read_size);
    if (!rom) {
        printf("Failed to allocate memory for ROM\n");
        fclose(file);
        return 0; // unknown mapper
    }

    size_t read_bytes = fread(rom, 1, read_size, file);
    fclose(file);
    if (read_bytes != read_size) {
        printf("Failed to read ROM data from %s\n", filename);
        free(rom);
        return 0; // unknown mapper
    }

    // SHA1 database lookup (openMSX softwaredb) — checked before heuristics.
    {
        uint8_t sha1[20];
        sha1_from_buffer(rom, (uint32_t)read_size, sha1);
        uint8_t db_type = romdb_lookup(sha1);
        if (db_type) {
            free(rom);
            return db_type;
        }
    }
    
    // Check if the ROM has the signature "AB" at 0x0000 and 0x0001
    // Those are the cases for 16KB and 32KB ROMs
    if (rom[0] == 'A' && rom[1] == 'B' && size == 16384) {
        free(rom);
        return 1;     // Plain 16KB 
    }

    if (rom[0] == 'A' && rom[1] == 'B' && size <= 32768) {

        // Check if it is a normal 32KB ROM or Planar32/48-style layout.
        if (rom[0x4000] == 'A' && rom[0x4001] == 'B') {
            free(rom);
            return 4; // Planar32/48 style (AB at 0x4000)
        }
        
        free(rom);
        return 2;     // Plain 32KB 
    }

    // Check for the "AB" header at the start
    if (rom[0] == 'A' && rom[1] == 'B') {
        if (memcmp(&rom[16], ascii16x_signature, sizeof(ascii16x_signature) - 1) == 0) {
            free(rom);
            return ROM_TYPE_ASCII16X; // ASCII16-X mapper detected
        }
        // Check for the NEO8 signature at offset 16
        if (memcmp(&rom[16], neo8_signature, sizeof(neo8_signature) - 1) == 0) {
            free(rom);
            return 8; // NEO8 mapper detected
        } else if (memcmp(&rom[16], neo16_signature, sizeof(neo16_signature) - 1) == 0) {
            free(rom);
            return 9; // NEO16 mapper detected
        }
    }

    // Manbow 2 detection: 512KB ROM with "Manbow 2" string at offset 0x28000.
    // All known Manbow 2 dumps share this signature (game-over music track label).
    // Must be checked before the heuristic scoring which would classify it as Konami SCC.
    if (size == 524288u && rom[0] == 'A' && rom[1] == 'B' &&
        memcmp(&rom[0x28000], "Manbow 2", 8) == 0)
    {
        free(rom);
        return ROM_TYPE_MANBOW2;
    }

    // Check if the ROM has the signature "AB" at 0x4000 and 0x4001
    // That is the case for 48KB Planar mapping.
    if (rom[0x4000] == 'A' && rom[0x4001] == 'B' && size <= 49152) {
        free(rom);
        return 4; // Planar48
    }

    // 64KB planar ROMs may only expose AB at 0x4000.
    // Treat that as sufficient for Planar64 classification.
    if (size == 65536u) {
        bool ab4000 = (rom[0x4000] == 'A' && rom[0x4001] == 'B');

        if (ab4000) {
            free(rom);
            return ROM_TYPE_PLANAR64;
        }
    }

    // Heuristic analysis for larger ROMs
    if (size > 32768) {
        // Scan through the ROM data to detect ld (nnnn),a patterns.
        // Scoring matches openMSX guessRomType(): unit weights, no SCC
        // credit for 0x6000, and >= tie-breaking favoring later types.
        for (size_t i = 0; i < read_size - 3; i++) {
            if (rom[i] == 0x32) { // ld (nnnn),a
                uint16_t addr = rom[i + 1] | (rom[i + 2] << 8);
                switch (addr) {
                    case 0x4000:
                    case 0x8000:
                    case 0xA000:
                        konami_score++;
                        break;
                    case 0x5000:
                    case 0x9000:
                    case 0xB000:
                        konami_scc_score++;
                        break;
                    case 0x6800:
                    case 0x7800:
                        ascii8_score++;
                        break;
                    case 0x77FF:
                        ascii16_score++;
                        break;
                    case 0x6000:
                        konami_score++;
                        ascii8_score++;
                        ascii16_score++;
                        break;
                    case 0x7000:
                        konami_scc_score++;
                        ascii8_score++;
                        ascii16_score++;
                        break;
                }
            }
        }

        // openMSX quirk: subtract 1 from ASCII8 if non-zero.
        if (ascii8_score) ascii8_score--;

#if defined(DEBUG) || defined(_DEBUG)
        printf("DEBUG: ascii8_score = %u\n", ascii8_score);
        printf("DEBUG: ascii16_score = %u\n", ascii16_score);
        printf("DEBUG: konami_score = %u\n", konami_score);
        printf("DEBUG: konami_scc_score = %u\n\n", konami_scc_score);
#endif

        // Pick the winner using >= so that later types win ties.
        // Iteration order: KonamiSCC, Konami, ASCII8, ASCII16.
        // This means ASCII16 beats ASCII8 on equal scores, etc.
        {
            uint8_t best_type = 0; // GENERIC_8KB / unknown
            unsigned int best_score = 0;
            struct { unsigned int score; uint8_t type; } candidates[] = {
                { konami_scc_score, 3 },  // Konami SCC
                { konami_score,     7 },  // Konami
                { ascii8_score,     5 },  // ASCII8
                { ascii16_score,    6 },  // ASCII16
            };
            for (int c = 0; c < 4; c++) {
                if (candidates[c].score && candidates[c].score >= best_score) {
                    best_score = candidates[c].score;
                    best_type = candidates[c].type;
                }
            }
            if (best_type) {
                free(rom);
                return best_type;
            }
        }

        // Avoid false positives when no mapper writes were identified.
        // For 64KB ROMs with AB header(s), fallback to Planar64.
        if (konami_score == 0 && konami_scc_score == 0 && ascii8_score == 0 && ascii16_score == 0)
        {
            bool ab0 = (rom[0x0000] == 'A' && rom[0x0001] == 'B');
            bool ab4000 = (rom[0x4000] == 'A' && rom[0x4001] == 'B');

            if (size == 65536u && (ab0 || ab4000))
            {
                free(rom);
                return ROM_TYPE_PLANAR64;
            }

            // Some valid dumps contain no detectable ld(nn),a mapper writes.
            // For AB-header ROMs larger than 64KB, use raw constant density
            // as a secondary hint: 0x77FF favors ASCII16; otherwise ASCII8.
            if (size > 65536u && ab0 && ((size % 16384u) == 0u))
            {
                unsigned int raw_77ff = 0u;
                unsigned int raw_6800 = 0u;
                unsigned int raw_7800 = 0u;

                for (size_t i = 0; i + 1 < read_size; ++i)
                {
                    uint16_t raw = (uint16_t)(rom[i] | (rom[i + 1] << 8));
                    if (raw == 0x77FFu) ++raw_77ff;
                    else if (raw == 0x6800u) ++raw_6800;
                    else if (raw == 0x7800u) ++raw_7800;
                }

                free(rom);
                return (raw_77ff > (raw_6800 + raw_7800)) ? 6 : 5; // ASCII16 : ASCII8
            }

            free(rom);
            return 0; // unknown mapper
        }

        free(rom);
        return 0; // unknown mapper
    }
    
    free(rom);
    return 0;
}

// Print usage information
static void print_usage(const char *prog_name) {
    size_t i;
    bool first = true;

    printf("Usage: %s [-h] [-s1] [-m1] [-r1] [-s2] [-m2] [-r2] [-c1] [-c2] [-4] [-a] [--4mhz] [-w] [-d] [-f] [-scc] [-sccplus] [-o <filename>] [romfile]\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
    printf("  -s1, --sunrise-sd  Build UF2 with Sunrise IDE Nextor ROM (microSD card)\n");
    printf("  -m1, --mapper-sd   Build UF2 with Sunrise IDE Nextor ROM + 1MB PSRAM mapper (microSD card)\n");
    printf("  -r1, --megaram-sd  Build UF2 with Sunrise IDE Nextor ROM + 1MB PSRAM mapper + 1MB MegaRAM (microSD card)\n");
    printf("  -s2, --sunrise-usb Build UF2 with Sunrise IDE Nextor ROM (USB pendrive)\n");
    printf("  -m2, --mapper-usb  Build UF2 with Sunrise IDE Nextor ROM + 1MB PSRAM mapper (USB pendrive)\n");
    printf("  -r2, --megaram-usb Build UF2 with Sunrise IDE Nextor ROM + 1MB PSRAM mapper + 1MB MegaRAM (USB pendrive)\n");
    printf("  -c1, --carnivore2-sd  Build UF2 with Sunrise IDE Nextor ROM + 1MB PSRAM mapper + Carnivore2 RAM emulation (microSD card)\n");
    printf("  -c2, --carnivore2-usb Build UF2 with Sunrise IDE Nextor ROM + 1MB PSRAM mapper + Carnivore2 RAM emulation (USB pendrive)\n");
    printf("  -4, --opl4         Build UF2 with the standalone OPL4 / YMF278B / MoonSound cartridge firmware (2MB YRW801-M ROM + 2MB PCM sample RAM)\n");
    printf("      --opl4-limit   (with -4) Enable the adaptive PCM voice limiter: sheds PCM voices on extreme-polyphony songs to avoid audio underrun\n");
    printf("      --lowclock     (with -4) Build an OPL4 image that runs the RP2350 at 282 MHz instead of the default 300 MHz\n");
    printf("      --22khz        (with -4) Build an OPL4 image that renders at 22050 Hz instead of 44100 Hz: halves the CPU cost so\n");
    printf("                     very dense songs play without dropouts, at the cost of a duller high end\n");
    printf("  -a, --msx-audio    Build UF2 with the standalone MSX-AUDIO / Y8950 cartridge firmware (MSX-Audio BIOS + 256KB ADPCM sample RAM)\n");
    printf("      --4mhz         (with -a) Clock the emulated Y8950 at 4 MHz instead of the standard 3.579545 MHz\n");
    printf("  -w, --wifi         Enable ESP-01 WiFi support for Sunrise IDE Nextor modes (-s1/-m1/-s2/-m2 only)\n");
    printf("  -d, --dual-psg     Enable secondary PSG emulation on I/O ports 0x10/0x11\n");
    printf("  -f, -fmpac         Enable MSX-MUSIC/YM2413 emulation on I/O ports 0x7C/0x7D\n");
    printf("  -scc, --scc        Enable SCC sound emulation (Konami SCC mapper only)\n");
    printf("  -sccplus, --sccplus  Enable SCC+ (enhanced) sound emulation (Konami SCC mapper only)\n");
    printf("  -o <filename>, --output <filename>  Set UF2 output filename (default %s)\n", UF2FILENAME);
    printf("\n");
    printf("Mapper forcing: append tags (case-insensitive) before the ROM extension.\n");
    printf("Forceable tags: ");
    for (i = 0; i < MAPPER_DESCRIPTION_COUNT; ++i) {
        uint8_t mapper_id = (uint8_t)(i + 1u);
        if (mapper_id == 10u || mapper_id == 11u || mapper_id == 15u || mapper_id == 16u
            || mapper_id == 17u || mapper_id == 18u || mapper_id == 19u || mapper_id == 20u) {
            continue;
        }

        if (!first) {
            printf(", ");
        }
        printf("%s", MAPPER_DESCRIPTIONS[i]);
        first = false;
    }
    printf("\n");
    printf("Example: \"Knight Mare.PLA-32.ROM\" forces PLA-32; \"SYSTEM\" tags are ignored.\n");
}

// create_uf2_file - Create the UF2 file
// This function streams the firmware binary, a single configuration record, and the ROM payload into UF2 blocks.

void create_uf2_file(const char *rom_filename, const uint8_t *embedded_rom, uint32_t rom_size,
                     const uint8_t *extra_embedded_rom, uint32_t extra_rom_size, uint8_t rom_type,
                     const char *rom_name, uint32_t base_offset, const char *uf2_filename) {
    if (!rom_name || rom_size == 0) {
        printf("Invalid parameters provided for UF2 generation.\n");
        return;
    }

    if (!rom_filename && !embedded_rom) {
        printf("Invalid parameters provided for UF2 generation.\n");
        return;
    }

    const uint8_t *firmware_data = ___pico_loadrom_dist_loadrom_bin;
    const size_t firmware_size = sizeof(___pico_loadrom_dist_loadrom_bin);
    uint8_t config_record[CONFIG_RECORD_SIZE] = {0};

    size_t cursor = 0;
    memcpy(config_record + cursor, rom_name, MAX_FILE_NAME_LENGTH);
    cursor += MAX_FILE_NAME_LENGTH;
    memcpy(config_record + cursor, &rom_type, sizeof(rom_type));
    cursor += sizeof(rom_type);
    memcpy(config_record + cursor, &rom_size, sizeof(rom_size));
    cursor += sizeof(rom_size);
    memcpy(config_record + cursor, &base_offset, sizeof(base_offset));

    FILE *rom_file = NULL;
    if (rom_filename) {
        rom_file = fopen(rom_filename, "rb");
        if (!rom_file) {
            perror("Failed to open ROM file for UF2 creation");
            return;
        }
    }

    FILE *uf2_file = fopen(uf2_filename, "wb");
    if (!uf2_file) {
        perror("Failed to create UF2 file");
        if (rom_file) {
            fclose(rom_file);
        }
        return;
    }

    UF2_Block bl;
    memset(&bl, 0, sizeof(bl));
    bl.magicStart0 = UF2_MAGIC_START0;
    bl.magicStart1 = UF2_MAGIC_START1;
    bl.flags = UF2_FLAG_FAMILYID_PRESENT;
    bl.magicEnd = UF2_MAGIC_END;
    bl.targetAddr = FLASH_START;
    bl.payloadSize = 256;

    const size_t total_binary_size = firmware_size + CONFIG_RECORD_SIZE + (size_t)rom_size + (size_t)extra_rom_size;
    bl.numBlocks = (uint32_t)((total_binary_size + bl.payloadSize - 1) / bl.payloadSize);
    bl.fileSize = RP2350_FAMILY_ID;

    size_t firmware_offset = 0;
    size_t config_offset = 0;
    size_t rom_bytes_written = 0;
    size_t extra_rom_bytes_written = 0;
    size_t total_written = 0;
    uint32_t block_no = 0;
    bool success = false;

    while (total_written < total_binary_size) {
        memset(bl.data, 0, sizeof(bl.data));
        size_t chunk_filled = 0;

        while (chunk_filled < bl.payloadSize && total_written < total_binary_size) {
            size_t to_copy = 0;

            if (firmware_offset < firmware_size) {
                size_t remaining = firmware_size - firmware_offset;
                to_copy = remaining < (bl.payloadSize - chunk_filled) ? remaining : (bl.payloadSize - chunk_filled);
                memcpy(bl.data + chunk_filled, firmware_data + firmware_offset, to_copy);
                firmware_offset += to_copy;
            } else if (config_offset < CONFIG_RECORD_SIZE) {
                size_t remaining = CONFIG_RECORD_SIZE - config_offset;
                to_copy = remaining < (bl.payloadSize - chunk_filled) ? remaining : (bl.payloadSize - chunk_filled);
                memcpy(bl.data + chunk_filled, config_record + config_offset, to_copy);
                config_offset += to_copy;
            } else if (rom_bytes_written < (size_t)rom_size) {
                size_t remaining = (size_t)rom_size - rom_bytes_written;
                if (rom_file) {
                    size_t request = remaining < (bl.payloadSize - chunk_filled) ? remaining : (bl.payloadSize - chunk_filled);
                    size_t read_now = fread(bl.data + chunk_filled, 1, request, rom_file);
                    if (read_now != request) {
                        printf("Failed to read ROM data while building UF2 file.\n");
                        goto cleanup;
                    }
                    rom_bytes_written += read_now;
                    to_copy = read_now;
                } else {
                    size_t to_embed = remaining < (bl.payloadSize - chunk_filled) ? remaining : (bl.payloadSize - chunk_filled);
                    memcpy(bl.data + chunk_filled, embedded_rom + rom_bytes_written, to_embed);
                    rom_bytes_written += to_embed;
                    to_copy = to_embed;
                }
            } else {
                size_t remaining = (size_t)extra_rom_size - extra_rom_bytes_written;
                if (remaining == 0) {
                    break;
                }

                size_t to_embed = remaining < (bl.payloadSize - chunk_filled) ? remaining : (bl.payloadSize - chunk_filled);
                memcpy(bl.data + chunk_filled, extra_embedded_rom + extra_rom_bytes_written, to_embed);
                extra_rom_bytes_written += to_embed;
                to_copy = to_embed;
            }

            chunk_filled += to_copy;
            total_written += to_copy;
        }

        bl.blockNo = block_no++;
        if (fwrite(&bl, 1, sizeof(bl), uf2_file) != sizeof(bl)) {
            printf("Failed to write UF2 block %u.\n", block_no - 1);
            goto cleanup;
        }
        bl.targetAddr += bl.payloadSize;
    }

    success = true;

cleanup:
    if (rom_file) {
        fclose(rom_file);
    }
    fclose(uf2_file);

    if (success) {
        printf("\nSuccessfully wrote %u blocks to %s.\n", block_no, uf2_filename);
    }
}

// Main function
int main(int argc, char *argv[])
{
    printf("MSX PICOVERSE 2350 LoadROM UF2 Creator %s\n", APP_VERSION);
    printf("(c) 2026 The Retro Hacker\n\n");

    const char *output_filename = UF2FILENAME;
    const char *rom_filename = NULL;
    bool use_sunrise_sd = false;
    bool use_mapper_sd = false;
    bool use_sunrise_usb = false;
    bool use_mapper_usb = false;
    bool use_c2_sd = false;
    bool use_c2_usb = false;
    bool use_megaram_sd = false;
    bool use_megaram_usb = false;
    bool use_opl4 = false;
    bool opl4_limit = false;
    bool opl4_lowclock = false;
    bool opl4_22khz = false;
    bool use_msx_audio = false;
    bool msx_audio_4mhz = false;
    bool use_wifi = false;
    bool dual_psg = false;
    bool msx_music = false;
    bool scc_emulation = false;
    bool scc_plus = false;

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
            print_usage(argv[0]);
            return 0;
        } else if ((strcmp(argv[i], "-s1") == 0) || (strcmp(argv[i], "--sunrise-sd") == 0)) {
            use_sunrise_sd = true;
        } else if ((strcmp(argv[i], "-m1") == 0) || (strcmp(argv[i], "--mapper-sd") == 0)) {
            use_mapper_sd = true;
        } else if ((strcmp(argv[i], "-r1") == 0) || (strcmp(argv[i], "--megaram-sd") == 0)) {
            use_megaram_sd = true;
        } else if ((strcmp(argv[i], "-s2") == 0) || (strcmp(argv[i], "--sunrise-usb") == 0)) {
            use_sunrise_usb = true;
        } else if ((strcmp(argv[i], "-m2") == 0) || (strcmp(argv[i], "--mapper-usb") == 0)) {
            use_mapper_usb = true;
        } else if ((strcmp(argv[i], "-r2") == 0) || (strcmp(argv[i], "--megaram-usb") == 0)) {
            use_megaram_usb = true;
        } else if ((strcmp(argv[i], "-c1") == 0) || (strcmp(argv[i], "--carnivore2-sd") == 0)) {
            use_c2_sd = true;
        } else if ((strcmp(argv[i], "-c2") == 0) || (strcmp(argv[i], "--carnivore2-usb") == 0)) {
            use_c2_usb = true;
        } else if ((strcmp(argv[i], "-4") == 0) || (strcmp(argv[i], "--opl4") == 0)) {
            use_opl4 = true;
        } else if (strcmp(argv[i], "--opl4-limit") == 0) {
            opl4_limit = true;
        } else if (strcmp(argv[i], "--lowclock") == 0) {
            opl4_lowclock = true;
        } else if (strcmp(argv[i], "--22khz") == 0) {
            opl4_22khz = true;
        } else if ((strcmp(argv[i], "-a") == 0) || (strcmp(argv[i], "--msx-audio") == 0)) {
            use_msx_audio = true;
        } else if (strcmp(argv[i], "--4mhz") == 0) {
            msx_audio_4mhz = true;
        } else if ((strcmp(argv[i], "-w") == 0) || (strcmp(argv[i], "--wifi") == 0)) {
            use_wifi = true;
        } else if ((strcmp(argv[i], "-d") == 0) || (strcmp(argv[i], "--dual-psg") == 0)) {
            dual_psg = true;
        } else if ((strcmp(argv[i], "-f") == 0) || (strcmp(argv[i], "-fmpac") == 0) || (strcmp(argv[i], "--fmpac") == 0)) {
            msx_music = true;
        } else if ((strcmp(argv[i], "-scc") == 0) || (strcmp(argv[i], "--scc") == 0)) {
            scc_emulation = true;
        } else if ((strcmp(argv[i], "-sccplus") == 0) || (strcmp(argv[i], "--sccplus") == 0)) {
            scc_plus = true;
        } else if ((strcmp(argv[i], "-o") == 0) || (strcmp(argv[i], "--output") == 0)) {
            if (i + 1 >= argc) {
                printf("Option -o/--output requires a filename.\n");
                return 1;
            }
            output_filename = argv[++i];
        } else if (argv[i][0] == '-') {
            printf("Unknown option: %s\n", argv[i]);
            return 1;
        } else if (!rom_filename) {
            rom_filename = argv[i];
        } else {
            printf("Unexpected argument: %s\n", argv[i]);
            return 1;
        }
    }

    {
        int nextor_count = (use_sunrise_sd ? 1 : 0) + (use_mapper_sd ? 1 : 0)
                         + (use_sunrise_usb ? 1 : 0) + (use_mapper_usb ? 1 : 0)
                         + (use_c2_sd ? 1 : 0) + (use_c2_usb ? 1 : 0)
                         + (use_megaram_sd ? 1 : 0) + (use_megaram_usb ? 1 : 0);
        if (nextor_count > 1) {
            printf("Options -s1, -m1, -r1, -s2, -m2, -r2, -c1 and -c2 are mutually exclusive.\n");
            return 1;
        }
    }

    bool use_nextor = use_sunrise_sd || use_mapper_sd || use_sunrise_usb || use_mapper_usb
                   || use_c2_sd || use_c2_usb || use_megaram_sd || use_megaram_usb;

    if (opl4_limit && !use_opl4) {
        printf("Option --opl4-limit requires -4/--opl4.\n");
        return 1;
    }
    if (opl4_lowclock && !use_opl4) {
        printf("Option --lowclock requires -4/--opl4.\n");
        return 1;
    }
    if (opl4_22khz && !use_opl4) {
        printf("Option --22khz requires -4/--opl4.\n");
        return 1;
    }
    if (use_opl4 && use_msx_audio) {
        printf("Options -4/--opl4 and -a/--msx-audio are mutually exclusive.\n");
        return 1;
    }
    if (msx_audio_4mhz && !use_msx_audio) {
        printf("Option --4mhz requires -a/--msx-audio.\n");
        return 1;
    }

    // -------------------------------------------------------------------
    // Standalone MSX-AUDIO / Y8950 cartridge (-a / --msx-audio)
    // -------------------------------------------------------------------
    // Writes a firmware-only UF2 laid out as
    //   [msxaudio.bin][16-byte "PVAU" config header][48KB MSX-Audio BIOS ROM]
    // The firmware finds the config header at __flash_binary_end and the BIOS
    // ROM immediately after it. The emulated cartridge follows the openMSX
    // "Philips/MSXPro MSX-Audio" extension (Boosted_audio.xml): a Y8950 on
    // ports 0xC0/0xC1 with 256KB of ADPCM sample RAM, the MSX-Audio BIOS
    // mapped as a plain Normal0000 ROM, and the two 4KB work RAM blocks.
    if (use_msx_audio) {
        if (use_nextor || dual_psg || msx_music || scc_emulation || scc_plus
            || use_wifi || rom_filename) {
            printf("Option -a/--msx-audio is standalone and cannot be combined with other modes or a ROM file.\n");
            return 1;
        }

        const uint8_t *fw_data = ___pico_msxaudio_dist_msxaudio_bin;
        const size_t fw_size = sizeof(___pico_msxaudio_dist_msxaudio_bin);
        const uint8_t *bios_data = ___resources_MSXAUDIO_NMS1205_ROM;
        const size_t bios_size = (size_t)___resources_MSXAUDIO_NMS1205_ROM_len;

        uint8_t cfg_header[16];
        memset(cfg_header, 0, sizeof(cfg_header));
        cfg_header[0] = 'P'; cfg_header[1] = 'V'; cfg_header[2] = 'A'; cfg_header[3] = 'U';
        cfg_header[4] = (uint8_t)(msx_audio_4mhz ? 0x01u : 0x00u);
        const size_t cfg_size = sizeof(cfg_header);

        printf("Mode: MSX-AUDIO / Y8950 (dedicated cartridge)\n");
        printf("Firmware Size: %zu bytes\n", fw_size);
        printf("MSX-Audio BIOS ROM Size: %zu bytes\n", bios_size);
        printf("Y8950 ports: 0xC0 / 0xC1\n");
        printf("ADPCM sample RAM: 256 KB (external PSRAM)\n");
        printf("Y8950 clock: %s\n", msx_audio_4mhz ? "4 MHz" : "3.579545 MHz (standard)");
        printf("Sample rate: %u Hz (Y8950 native)\n", msx_audio_4mhz ? 55555u : 49716u);
        printf("UF2 Output: %s\n", output_filename);

        FILE *uf2_file = fopen(output_filename, "wb");
        if (!uf2_file) {
            perror("Failed to create UF2 file");
            return 1;
        }

        const size_t total_size = fw_size + cfg_size + bios_size;

        UF2_Block bl;
        memset(&bl, 0, sizeof(bl));
        bl.magicStart0 = UF2_MAGIC_START0;
        bl.magicStart1 = UF2_MAGIC_START1;
        bl.flags = UF2_FLAG_FAMILYID_PRESENT;
        bl.magicEnd = UF2_MAGIC_END;
        bl.targetAddr = FLASH_START;
        bl.payloadSize = 256;
        bl.numBlocks = (uint32_t)((total_size + bl.payloadSize - 1) / bl.payloadSize);
        bl.fileSize = RP2350_FAMILY_ID;

        size_t fw_off = 0;
        size_t cfg_off = 0;
        size_t bios_off = 0;
        size_t total_written = 0;
        uint32_t block_no = 0;
        while (total_written < total_size) {
            memset(bl.data, 0, sizeof(bl.data));
            size_t chunk_filled = 0;

            while (chunk_filled < bl.payloadSize && total_written < total_size) {
                size_t to_copy;
                size_t space = bl.payloadSize - chunk_filled;
                if (fw_off < fw_size) {
                    size_t remaining = fw_size - fw_off;
                    to_copy = remaining < space ? remaining : space;
                    memcpy(bl.data + chunk_filled, fw_data + fw_off, to_copy);
                    fw_off += to_copy;
                } else if (cfg_off < cfg_size) {
                    size_t remaining = cfg_size - cfg_off;
                    to_copy = remaining < space ? remaining : space;
                    memcpy(bl.data + chunk_filled, cfg_header + cfg_off, to_copy);
                    cfg_off += to_copy;
                } else {
                    size_t remaining = bios_size - bios_off;
                    to_copy = remaining < space ? remaining : space;
                    memcpy(bl.data + chunk_filled, bios_data + bios_off, to_copy);
                    bios_off += to_copy;
                }
                chunk_filled += to_copy;
                total_written += to_copy;
            }

            bl.blockNo = block_no++;
            if (fwrite(&bl, 1, sizeof(bl), uf2_file) != sizeof(bl)) {
                printf("Failed to write UF2 block %u.\n", block_no - 1);
                fclose(uf2_file);
                return 1;
            }
            bl.targetAddr += bl.payloadSize;
        }

        fclose(uf2_file);
        printf("\nSuccessfully wrote %u blocks to %s.\n", block_no, output_filename);
        return 0;
    }

    // -------------------------------------------------------------------
    // Standalone OPL4 / MoonSound cartridge (-4 / --opl4)
    // -------------------------------------------------------------------
    // Writes a firmware-only UF2 with the 2 MB YRW801-M wave ROM appended
    // directly after the firmware image. The firmware finds the ROM at
    // __flash_binary_end (offset 0 relative to the end of the binary).
    if (use_opl4) {
        if (use_nextor || dual_psg || msx_music || scc_emulation || scc_plus
            || use_wifi || rom_filename) {
            printf("Option -4/--opl4 is standalone and cannot be combined with other modes or a ROM file.\n");
            return 1;
        }

        const uint8_t *fw_data = opl4_22khz ? ___pico_opl4_dist_opl4_22k_bin : ___pico_opl4_dist_opl4_bin;
        const size_t fw_size = opl4_22khz ? sizeof(___pico_opl4_dist_opl4_22k_bin) : sizeof(___pico_opl4_dist_opl4_bin);
        const uint8_t *yrw_data = ___resources_YRW801_M___Yamaha___1993_rom;
        const size_t yrw_size = (size_t)___resources_YRW801_M___Yamaha___1993_rom_len;

        // 16-byte config header written between the firmware and the ROM. The
        // firmware reads it at __flash_binary_end: magic "PVO4" + flags byte.
        uint8_t cfg_header[16];
        memset(cfg_header, 0, sizeof(cfg_header));
        cfg_header[0] = 'P'; cfg_header[1] = 'V'; cfg_header[2] = 'O'; cfg_header[3] = '4';
        cfg_header[4] = (uint8_t)((opl4_limit ? 0x01u : 0x00u) |
                      (opl4_lowclock ? 0x02u : 0x00u));
        const size_t cfg_size = sizeof(cfg_header);

        printf("Mode: OPL4 / YMF278B / MoonSound (dedicated cartridge)\n");
        printf("Firmware Size: %zu bytes\n", fw_size);
        printf("YRW801-M ROM Size: %zu bytes\n", yrw_size);
        printf("OPL4 sample rate: %s\n", opl4_22khz ? "22050 Hz" : "44100 Hz");
        printf("OPL4 RP2350 clock: %s\n", opl4_lowclock ? "282 MHz" : "300 MHz");
        printf("Adaptive voice limiter: %s\n", opl4_limit ? "ON (reduced peak polyphony)" : "OFF (full fidelity)");
        printf("UF2 Output: %s\n", output_filename);

        FILE *uf2_file = fopen(output_filename, "wb");
        if (!uf2_file) {
            perror("Failed to create UF2 file");
            return 1;
        }

        const size_t total_size = fw_size + cfg_size + yrw_size;

        UF2_Block bl;
        memset(&bl, 0, sizeof(bl));
        bl.magicStart0 = UF2_MAGIC_START0;
        bl.magicStart1 = UF2_MAGIC_START1;
        bl.flags = UF2_FLAG_FAMILYID_PRESENT;
        bl.magicEnd = UF2_MAGIC_END;
        bl.targetAddr = FLASH_START;
        bl.payloadSize = 256;
        bl.numBlocks = (uint32_t)((total_size + bl.payloadSize - 1) / bl.payloadSize);
        bl.fileSize = RP2350_FAMILY_ID;

        size_t fw_off = 0;
        size_t cfg_off = 0;
        size_t yrw_off = 0;
        size_t total_written = 0;
        uint32_t block_no = 0;
        while (total_written < total_size) {
            memset(bl.data, 0, sizeof(bl.data));
            size_t chunk_filled = 0;

            while (chunk_filled < bl.payloadSize && total_written < total_size) {
                size_t to_copy;
                size_t space = bl.payloadSize - chunk_filled;
                if (fw_off < fw_size) {
                    size_t remaining = fw_size - fw_off;
                    to_copy = remaining < space ? remaining : space;
                    memcpy(bl.data + chunk_filled, fw_data + fw_off, to_copy);
                    fw_off += to_copy;
                } else if (cfg_off < cfg_size) {
                    size_t remaining = cfg_size - cfg_off;
                    to_copy = remaining < space ? remaining : space;
                    memcpy(bl.data + chunk_filled, cfg_header + cfg_off, to_copy);
                    cfg_off += to_copy;
                } else {
                    size_t remaining = yrw_size - yrw_off;
                    to_copy = remaining < space ? remaining : space;
                    memcpy(bl.data + chunk_filled, yrw_data + yrw_off, to_copy);
                    yrw_off += to_copy;
                }
                chunk_filled += to_copy;
                total_written += to_copy;
            }

            bl.blockNo = block_no++;
            if (fwrite(&bl, 1, sizeof(bl), uf2_file) != sizeof(bl)) {
                printf("Failed to write UF2 block %u.\n", block_no - 1);
                fclose(uf2_file);
                return 1;
            }
            bl.targetAddr += bl.payloadSize;
        }

        fclose(uf2_file);
        printf("\nSuccessfully wrote %u blocks to %s.\n", block_no, output_filename);
        return 0;
    }

    if (use_wifi && !(use_sunrise_sd || use_mapper_sd || use_sunrise_usb || use_mapper_usb)) {
        printf("Error: -w/--wifi is supported only with -s1, -m1, -s2 or -m2.\n");
        return 1;
    }

    if (scc_emulation && scc_plus) {
        printf("Error: -scc and -sccplus are mutually exclusive. Use only one.\n");
        return 1;
    }

    if ((dual_psg && msx_music) || (dual_psg && (scc_emulation || scc_plus)) || (msx_music && (scc_emulation || scc_plus))) {
        printf("Error: -scc, -sccplus, -d/--dual-psg and -f/-fmpac are mutually exclusive; only one audio emulation can be active.\n");
        return 1;
    }

    if (dual_psg && use_nextor) {
        printf("Error: -d/--dual-psg is supported only when loading an external ROM file.\n");
        return 1;
    }

    if (msx_music && use_nextor) {
        printf("Error: -f/-fmpac is supported only when loading a non-SYSTEM external ROM file.\n");
        return 1;
    }

    if (use_nextor) {
        if (rom_filename) {
            printf("The Sunrise IDE options do not accept an external ROM file.\n");
            return 1;
        }

        const uint8_t *sunrise_rom = ___resources_Nextor_2_1_4_SunriseIDE_MasterOnly_ROM;
        uint32_t sunrise_rom_size = (uint32_t)___resources_Nextor_2_1_4_SunriseIDE_MasterOnly_ROM_len;
        const uint8_t *wifi_rom = NULL;
        uint32_t wifi_rom_size = 0u;
        uint8_t rom_type;
        const char *sunrise_name;
        if (use_sunrise_sd) {
            rom_type = ROM_TYPE_SUNRISE_SD;
            sunrise_name = "Nextor Sunrise IDE (SD)";
        } else if (use_mapper_sd) {
            rom_type = ROM_TYPE_SUNRISE_MAPPER_SD;
            sunrise_name = "Nextor Sunrise+Mapper (SD)";
        } else if (use_mapper_usb) {
            rom_type = ROM_TYPE_SUNRISE_MAPPER;
            sunrise_name = "Nextor Sunrise+Mapper (USB)";
        } else if (use_c2_sd) {
            rom_type = ROM_TYPE_C2_SD;
            sunrise_name = "Nextor Sunrise+Mapper+C2 (SD)";
        } else if (use_c2_usb) {
            rom_type = ROM_TYPE_C2_USB;
            sunrise_name = "Nextor Sunrise+Mapper+C2 (USB)";
        } else if (use_megaram_sd) {
            rom_type = ROM_TYPE_MEGARAM_SD;
            sunrise_name = "Nextor Sunrise+Mapper+MegaRAM (SD)";
        } else if (use_megaram_usb) {
            rom_type = ROM_TYPE_MEGARAM_USB;
            sunrise_name = "Nextor Sunrise+Mapper+MegaRAM (USB)";
        } else {
            rom_type = ROM_TYPE_SUNRISE;
            sunrise_name = "Nextor Sunrise IDE (USB)";
        }

        if (use_c2_sd || use_c2_usb) {
            if (scc_emulation) {
                rom_type |= 0x80u;
                printf("SCC Emulation: Enabled for SROM-loaded Konami SCC ROMs\n");
            } else if (scc_plus) {
                rom_type |= 0x40u;
                printf("SCC+ Emulation: Enabled for SROM-loaded Konami SCC ROMs\n");
            }
        } else if (scc_emulation || scc_plus) {
            printf("Warning: -scc/-sccplus ignored for this Sunrise mode (supported with -c1/-c2 only)\n");
        }

        if (use_wifi) {
            rom_type |= ROM_TYPE_WIFI_FLAG;
            printf("WiFi Support: Enabled (ESP8266P system ROM + UART support)\n");
            wifi_rom = ______wifi_bios_ESP8266P_rom;
            wifi_rom_size = (uint32_t)______wifi_bios_ESP8266P_rom_len;
        }

        uint32_t base_offset = CONFIG_RECORD_SIZE;

        if (sunrise_rom_size == 0) {
            printf("Embedded Sunrise IDE Nextor ROM payload is empty.\n");
            return 1;
        }

        uint8_t base_rom_type = (uint8_t)(rom_type & ~(0x80u | 0x40u | ROM_TYPE_WIFI_FLAG));
        printf("ROM Type: %s [Embedded]\n", rom_types[base_rom_type]);
        printf("ROM Name: %s\n", sunrise_name);
        printf("ROM Size: %u bytes\n", sunrise_rom_size);
        printf("Pico Offset: 0x%08X\n", base_offset);
        printf("UF2 Output: %s\n", output_filename);

        create_uf2_file(NULL, sunrise_rom, sunrise_rom_size, wifi_rom, wifi_rom_size, rom_type, sunrise_name, base_offset, output_filename);
        return 0;
    }

    if (!rom_filename) {
        print_usage(argv[0]);
        return 1;
    }

    const char *base_name = rom_filename;
    const char *slash = strrchr(rom_filename, '/');
    const char *backslash = strrchr(rom_filename, '\\');
    if (slash || backslash) {
        const char *sep = slash;
        if (!sep || (backslash && backslash > sep)) {
            sep = backslash;
        }
        base_name = sep + 1;
    }

    const char *extension = strrchr(base_name, '.');
    if (!extension || !equals_ignore_case(extension, ".rom")) {
        printf("Invalid ROM file. Please provide a .rom file.\n");
        return 1;
    }

    bool mapper_forced_by_tag = false;
    uint8_t mapper_from_tag = 0;
    const char *name_end = extension;
    const char *last_period = NULL;
    for (const char *p = base_name; p < extension; ++p) {
        if (*p == '.') {
            last_period = p;
        }
    }

    if (last_period) {
        const char *token_start = last_period + 1;
        size_t token_length = (size_t)(extension - token_start);
        if (token_length > 0) {
            char mapper_token[32];
            if (token_length >= sizeof(mapper_token)) {
                token_length = sizeof(mapper_token) - 1;
            }
            memcpy(mapper_token, token_start, token_length);
            mapper_token[token_length] = '\0';

            uint8_t candidate = mapper_number_from_description(mapper_token);
            if (candidate == 10 || candidate == 11 || candidate == 15 || candidate == 16 || candidate == 17 || candidate == 18) {
                printf("Ignoring SYSTEM mapper tag in %s (cannot be forced)\n", base_name);
            } else if (candidate != 0) {
                mapper_forced_by_tag = true;
                mapper_from_tag = candidate;
                name_end = last_period;
            }
        }
    }

    uint32_t rom_size = file_size(rom_filename);
    if (rom_size == 0 || rom_size > MAX_ROM_SIZE) {
        printf("Failed to get the size of the ROM file or size not supported.\n");
        return 1;
    }

    uint8_t rom_type = 0;
    const char *mapper_label = NULL;
    if (mapper_forced_by_tag) {
        rom_type = mapper_from_tag;
        mapper_label = "[Forced via filename tag]";
    } else {
        rom_type = detect_rom_type(rom_filename, rom_size);
        if (rom_type == 0) {
            printf("Failed to detect the ROM type. Please check the ROM file.\n");
            return 1;
        }
        mapper_label = "[Auto-detected]";
    }
    printf("ROM Type: %s %s\n", rom_types[rom_type], mapper_label);

    if (scc_emulation) {
        if (rom_type == 3 || rom_type == ROM_TYPE_MANBOW2) {
            rom_type |= 0x80;
            printf("SCC Emulation: Enabled\n");
        } else {
            printf("Warning: -scc flag ignored (ROM type is not Konami SCC or Manbow2)\n");
        }
    }

    if (scc_plus) {
        if (rom_type == 3 || rom_type == ROM_TYPE_MANBOW2) {
            rom_type |= 0x40;
            printf("SCC+ Emulation: Enabled\n");
        } else {
            printf("Warning: -sccplus flag ignored (ROM type is not Konami SCC or Manbow2)\n");
        }
    }

    // For SCC-capable mappers, always print emulation status
    if (!scc_emulation && !scc_plus && (rom_type == 3 || rom_type == ROM_TYPE_MANBOW2)) {
        printf("SCC Emulation: Disabled (use -scc or -sccplus to enable)\n");
    }

    if (dual_psg && (rom_type == 3 || rom_type == ROM_TYPE_MANBOW2)) {
        printf("Error: Second PSG is not supported with Konami SCC ROMs.\n");
        return 1;
    }

    if (msx_music && (rom_type == 3 || rom_type == ROM_TYPE_MANBOW2)) {
        printf("Error: -f/-fmpac is not supported with Konami SCC or Manbow2 mapper ROMs.\n");
        return 1;
    }

    if (dual_psg) {
        rom_type |= ROM_TYPE_DUAL_PSG_FLAG;
        printf("Dual PSG Emulation: Enabled on I/O ports 0x10/0x11\n");
    }

    const uint8_t *fmpac_bios_rom = NULL;
    uint32_t fmpac_bios_size = 0u;
    if (msx_music) {
        rom_type |= ROM_TYPE_MSX_MUSIC_FLAG;
        fmpac_bios_rom = ___fmpac_FMPCCMFC_BIN;
        fmpac_bios_size = (uint32_t)___fmpac_FMPCCMFC_BIN_len;
        printf("MSX-MUSIC/FM-PAC Emulation: Enabled with BIOS and YM2413 ports 0x7C/0x7D\n");
    }

    char rom_name[MAX_FILE_NAME_LENGTH] = {0};
    size_t raw_length = (size_t)(name_end - base_name);
    if (raw_length >= MAX_FILE_NAME_LENGTH) {
        raw_length = MAX_FILE_NAME_LENGTH - 1;
    }
    memcpy(rom_name, base_name, raw_length);
    rom_name[raw_length] = '\0';

    uint32_t base_offset = CONFIG_RECORD_SIZE; // The ROM is placed right after the configuration record

    printf("ROM Name: %s\n", rom_name);
    printf("ROM Size: %u bytes\n", rom_size);
    printf("Pico Offset: 0x%08X\n", base_offset);
    printf("UF2 Output: %s\n", output_filename);

    create_uf2_file(rom_filename, NULL, rom_size, fmpac_bios_rom, fmpac_bios_size, rom_type, rom_name, base_offset, output_filename);
    return 0;
}