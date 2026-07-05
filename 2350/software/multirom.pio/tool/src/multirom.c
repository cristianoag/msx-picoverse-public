// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// multirom.c - Windows console application to create a multirom binary file for the MSX PICOVERSE 2350
//
// This program creates a UF2 file to program the Raspberry Pi Pico with the MSX PICOVERSE 2350 MultiROM firmware. The UF2 file is
// created with the combined PICO firmware binary file, the MSX MENU ROM file, the configuration file and the ROM files. The 
// configuration file contains the information of each ROM file processed by the tool and it is incorporated into the MENU ROM file 
// so the MSX can read it.
// 
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/
//

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "uf2format.h"
#include "multirom.h"
#include "menu.h"
#include "nextor_sunrise.h"
#include "esp8266p_rom.h"
#include "sha1.h"
#include "romdb.h"

#ifndef APP_VERSION
#define APP_VERSION "v1.00"
#endif

#define UF2FILENAME             "multirom.uf2"  // UF2 produced by this tool
#define MENU_COPY_SIZE          (16 * 1024)     // Portion of menu ROM copied verbatim before config payload
#define MAX_FILE_NAME_LENGTH    50              // Maximum length of a ROM name
#define TARGET_FILE_SIZE        32768           // Size of the combined MSX MENU ROM and the configuration file
#define FLASH_START             0x10000000      // Start of the flash memory on the Raspberry Pi Pico
#define MAX_ROM_FILES           128             // Maximum number of ROM files
#define MAX_TOTAL_ROM_SIZE      (14U * 1024U * 1024U) // Cap combined ROM payload to 14 MB
#define MAX_ROM_SIZE            15*1024*1024    // Maximum size of a ROM file
#define MIN_ROM_SIZE            8192            // Minimum size of a ROM file
#define MAX_ANALYSIS_SIZE       131072          // 128KB for the mapper analysis
#define CONFIG_RECORD_SIZE      (MAX_FILE_NAME_LENGTH + 1 + sizeof(uint32_t) + sizeof(uint32_t))
#define MAX_UF2_FILENAME_LENGTH 512

static const char *MAPPER_DESCRIPTIONS[] = {
    "PLA-16", "PLA-32", "KonSCC", "PLN-48", "ASC-08",
    "ASC-16", "Konami", "NEO-8", "NEO-16", "SYSTEM", "SYSTEM", "ASC16X", "PLN-64", "MANBW2",
    "SYSTEM", "SYSTEM", "SYSTEM", "SYSTEM"
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
    "Carnivore2 USB"
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

#define ROM_TYPE_WIFI_FLAG 0x20u
#define ROM_TYPE_SCC_FLAG  0x80u
#define ROM_TYPE_SCCP_FLAG 0x40u
#define WIFI_ROM_SIZE      16384u

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

#if TARGET_FILE_SIZE < MENU_COPY_SIZE // Sanity check
#error "TARGET_FILE_SIZE must be larger than MENU_COPY_SIZE"
#endif

#define CONFIG_AREA_SIZE        (TARGET_FILE_SIZE - MENU_COPY_SIZE) // Size of the configuration area in the MENU ROM

// Tracks the ROMs discovered on disk so they can be appended later in scan order.
typedef struct {
    char file_name[256];    // File name
    uint32_t file_size;     // File size
} FileInfo;

// Forward declarations
void create_uf2_file(const uint8_t *data, size_t size, const char *uf2_filename);
uint32_t file_size(const char *filename);
uint8_t detect_rom_type(const char *filename, uint32_t size);
static void print_usage(const char *prog_name);

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

static bool rom_has_ab_at(const uint8_t *rom, uint32_t size, uint32_t offset) {
    return (offset + 1u < size) && rom[offset] == 'A' && rom[offset + 1u] == 'B';
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

    size_t bytes_read = fread(rom, 1, read_size, file);
    fclose(file);
    if (bytes_read != read_size) {
        printf("Failed to read ROM file\n");
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
    
    bool ab0 = rom_has_ab_at(rom, size, 0x0000u);
    bool ab4000 = rom_has_ab_at(rom, size, 0x4000u);

    // Check if the ROM has the signature "AB" at 0x0000 and 0x0001
    // Those are the cases for 16KB and 32KB ROMs
    if (ab0 && size <= 16384u) {
        free(rom);
        return 1;     // Plain 8KB/16KB
    }

    if (ab0 && size <= 32768u) {

        // Check if it is a normal 32KB ROM or Planar32/48-style layout.
        if (ab4000) {
            free(rom);
            return 4; // Planar32/48 style (AB at 0x4000)
        }
        
        free(rom);
        return 2;     // Plain 32KB 
    }

    // Check for the "AB" header at the start
    if (ab0) {
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
    if (size == 524288u && ab0 &&
        memcmp(&rom[0x28000], "Manbow 2", 8) == 0)
    {
        free(rom);
        return ROM_TYPE_MANBOW2;
    }

    // Check if the ROM has the signature "AB" at 0x4000 and 0x4001
    // That is the case for 48KB Planar mapping.
    if (ab4000 && size <= 49152) {
        free(rom);
        return 4; // Planar48
    }

    // 64KB planar ROMs may only expose AB at 0x4000.
    if (size == 65536u) {
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

        // Pick the winner using >= so that later types win ties.
        {
            uint8_t best_type = 0;
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

        // Fallbacks when no mapper writes were identified.
        if (konami_score == 0 && konami_scc_score == 0 && ascii8_score == 0 && ascii16_score == 0)
        {
            if (size == 65536u && (ab0 || ab4000))
            {
                free(rom);
                return ROM_TYPE_PLANAR64;
            }

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

    printf("Usage: %s [-h] [-s1] [-m1] [-s2] [-m2] [-c1] [-c2] [-scc] [-sccplus] [-w] [-o <filename>]\n", prog_name);
    printf("  without options, the tool scans the current directory for .ROM files to include in the MultiROM image\n");
    printf("Options:\n");
    printf("  -h   Show this help message\n");
    printf("  -s1, --sunrise-sd  Include Sunrise IDE Nextor ROM (microSD card)\n");
    printf("  -m1, --mapper-sd   Include Sunrise IDE Nextor ROM + 1MB mapper (microSD card)\n");
    printf("  -s2, --sunrise-usb Include Sunrise IDE Nextor ROM (USB pendrive)\n");
    printf("  -m2, --mapper-usb  Include Sunrise IDE Nextor ROM + 1MB mapper (USB pendrive)\n");
    printf("  -c1, --carnivore2-sd  Include Sunrise IDE Nextor ROM + 1MB mapper + Carnivore2 RAM emulation (microSD card)\n");
    printf("  -c2, --carnivore2-usb Include Sunrise IDE Nextor ROM + 1MB mapper + Carnivore2 RAM emulation (USB pendrive)\n");
    printf("  Options -s1, -m1, -s2, -m2, -c1, -c2 can be combined to add multiple Nextor entries\n");
    printf("  -scc, --scc        Enable SCC sound emulation (Konami SCC / Manbow2 / Carnivore2 ROMs)\n");
    printf("  -sccplus, --sccplus  Enable SCC+ (enhanced) sound emulation (Konami SCC / Manbow2 / Carnivore2 ROMs)\n");
    printf("  -w, --wifi         Add ESP-01 WiFi BIOS sub-slot to -s1/-m1/-s2/-m2 Nextor entries\n");
    printf("  -o <filename>, --output <filename>  Set UF2 output filename (default %s)\n", UF2FILENAME);
    printf("\n");
    printf("Mapper forcing: append tags (case-insensitive) before the ROM extension.\n");
    printf("Forceable tags: ");
    for (i = 0; i < MAPPER_DESCRIPTION_COUNT; ++i) {
        uint8_t mapper_id = (uint8_t)(i + 1u);
        if (mapper_id == 10u || mapper_id == 11u || mapper_id == 15u || mapper_id == 16u
            || mapper_id == 17u || mapper_id == 18u) {
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
    printf("UF2 output file: %s\n", UF2FILENAME);
}

// Serialize the fully joined binary image into UF2 blocks so the Pico can be programmed via USB MSC.
void create_uf2_file(const uint8_t *data, size_t size, const char *uf2_filename) {

    if (!data || size == 0) {
        printf("No data provided to create UF2 file\n");
        return;
    }

#ifdef DEBUG // Dump the payload to a binary file for debugging
    const char *dump_filename = "multirom_payload.bin";
    FILE *dump_file = fopen(dump_filename, "wb");
    if (!dump_file) {
        printf("DEBUG: Failed to open %s for payload dump\n", dump_filename);
    } else {
        size_t written = fwrite(data, 1, size, dump_file);
        if (written != size) {
            printf("DEBUG: Payload dump truncated (%zu of %zu bytes)\n", written, size);
        } else {
            printf("DEBUG: Wrote %zu bytes to %s\n", size, dump_filename);
        }
        fclose(dump_file);
    }
#endif

    // Create and open the UF2 file for writing
    FILE *uf2_file = fopen(uf2_filename, "wb");
    if (!uf2_file) {
        printf("Failed to create UF2 file");
        return;
    }

    // Prepare the UF2 block template
    UF2_Block bl;
    memset(&bl, 0, sizeof(bl));

    bl.magicStart0 = UF2_MAGIC_START0; // 0x0A324655 "UF2\n"
    bl.magicStart1 = UF2_MAGIC_START1; // 0x9E5D5157
    bl.flags = 0x00002000;            // UF2_FLAG_FAMILYID_PRESENT
    bl.magicEnd = UF2_MAGIC_END;      // 0x0AB16F30
    bl.targetAddr = FLASH_START;      // Start of the flash memory on the Raspberry Pi Pico
    bl.payloadSize = 256;             // Payload size
    bl.numBlocks = (uint32_t)((size + bl.payloadSize - 1) / bl.payloadSize);
    //bl.fileSize = 0xe48bff56;         // Size of the file
    bl.fileSize = 0xe48bff59;         // Size of the file

    size_t offset = 0;
    uint32_t block_no = 0;

    // Write the UF2 blocks
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > bl.payloadSize) {
            chunk = bl.payloadSize;
        }

        memcpy(bl.data, data + offset, chunk);
        if (chunk < bl.payloadSize) {
            memset(bl.data + chunk, 0, bl.payloadSize - chunk);
        }

        bl.blockNo = block_no++;
        fwrite(&bl, 1, sizeof(bl), uf2_file);
        bl.targetAddr += bl.payloadSize;
        offset += chunk;
    }

    fclose(uf2_file);
    printf("\nSuccessfully wrote %u blocks to %s.\n", block_no, uf2_filename);
}

// Main function
int main(int argc, char *argv[])
{
    printf("MSX PICOVERSE 2350 MultiROM UF2 Creator %s\n", APP_VERSION);
    printf("(c) 2025 The Retro Hacker\n\n");

    bool show_help = false;
    bool use_sunrise_sd = false;
    bool use_mapper_sd = false;
    bool use_sunrise_usb = false;
    bool use_mapper_usb = false;
    bool use_c2_sd = false;
    bool use_c2_usb = false;
    bool scc_emulation = false;
    bool scc_plus = false;
    bool use_wifi = false;
    const char *bad_option = NULL;
    const char *missing_output_option = NULL;
    char uf2_output_filename[MAX_UF2_FILENAME_LENGTH];

    snprintf(uf2_output_filename, sizeof(uf2_output_filename), "%s", UF2FILENAME);

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
            show_help = true;
        } else if ((strcmp(argv[i], "-s1") == 0) || (strcmp(argv[i], "--sunrise-sd") == 0)) {
            use_sunrise_sd = true;
        } else if ((strcmp(argv[i], "-m1") == 0) || (strcmp(argv[i], "--mapper-sd") == 0)) {
            use_mapper_sd = true;
        } else if ((strcmp(argv[i], "-s2") == 0) || (strcmp(argv[i], "--sunrise-usb") == 0)) {
            use_sunrise_usb = true;
        } else if ((strcmp(argv[i], "-m2") == 0) || (strcmp(argv[i], "--mapper-usb") == 0)) {
            use_mapper_usb = true;
        } else if ((strcmp(argv[i], "-c1") == 0) || (strcmp(argv[i], "--carnivore2-sd") == 0)) {
            use_c2_sd = true;
        } else if ((strcmp(argv[i], "-c2") == 0) || (strcmp(argv[i], "--carnivore2-usb") == 0)) {
            use_c2_usb = true;
        } else if ((strcmp(argv[i], "-scc") == 0) || (strcmp(argv[i], "--scc") == 0)) {
            scc_emulation = true;
        } else if ((strcmp(argv[i], "-sccplus") == 0) || (strcmp(argv[i], "--sccplus") == 0)) {
            scc_plus = true;
        } else if ((strcmp(argv[i], "-w") == 0) || (strcmp(argv[i], "--wifi") == 0)) {
            use_wifi = true;
        } else if ((strcmp(argv[i], "-o") == 0) || (strcmp(argv[i], "--output") == 0)) {
            if (i + 1 >= argc) {
                missing_output_option = argv[i];
                break;
            }
            ++i;
            snprintf(uf2_output_filename, sizeof(uf2_output_filename), "%s", argv[i]);
        } else {
            bad_option = argv[i];
            break;
        }
    }

    if (missing_output_option) {
        printf("Option %s requires a filename argument\n\n", missing_output_option);
        print_usage(argv[0] ? argv[0] : "multirom");
        return 1;
    }

    // Handle bad options
    if (bad_option) {
        printf("Unknown option: %s\n\n", bad_option);
        print_usage(argv[0] ? argv[0] : "multirom");
        return 1;
    }

    // Show help and exit
    if (show_help) {
        print_usage(argv[0] ? argv[0] : "multirom");
        return 0;
    }

    if (scc_emulation && scc_plus) {
        printf("Error: -scc and -sccplus are mutually exclusive. Use only one.\n");
        return 1;
    }

    if (use_wifi && !(use_sunrise_sd || use_mapper_sd || use_sunrise_usb || use_mapper_usb)) {
        printf("Error: -w/--wifi requires at least one of -s1, -m1, -s2, -m2.\n");
        return 1;
    }

    bool include_nextor = use_sunrise_sd || use_mapper_sd || use_sunrise_usb || use_mapper_usb
                       || use_c2_sd || use_c2_usb;

    // Standard MultiROM build mode
    printf("Scanning current directory for .ROM files...\n\n");
    DIR *dir;
    struct dirent *entry;
    FileInfo files[MAX_ROM_FILES]; // Array to track discovered ROM files
    int file_count = 0;
    int file_index = 1;
    uint32_t base_offset = TARGET_FILE_SIZE; // Start appending ROMs after the MENU + config area
    size_t total_rom_size = 0;
    size_t config_offset = 0;
    int nextor_entry_count = 0;
    bool nextor_entry_wifi[6] = { false, false, false, false, false, false };
    uint8_t *config_buffer = (uint8_t *)malloc(CONFIG_AREA_SIZE); // Configuration area buffer
    if (!config_buffer) {
        printf("Failed to allocate configuration buffer\n");
        return 1;
    }
    memset(config_buffer, 0xFF, CONFIG_AREA_SIZE); // Initialize config area to 0xFF

    // Include embedded Sunrise IDE Nextor ROM entries (one per flag)
    if (include_nextor) {
        struct { bool enabled; uint8_t mapper; const char *name; } nextor_entries[] = {
            { use_sunrise_sd,  ROM_TYPE_SUNRISE_SD,        "Nextor Sunrise IDE (SD)"     },
            { use_mapper_sd,   ROM_TYPE_SUNRISE_MAPPER_SD, "Nextor Sunrise + 1MB Mapper (SD)"  },
            { use_sunrise_usb, ROM_TYPE_SUNRISE,           "Nextor Sunrise IDE (USB)"    },
            { use_mapper_usb,  ROM_TYPE_SUNRISE_MAPPER,    "Nextor Sunrise + 1MB Mapper (USB)" },
            { use_c2_sd,       ROM_TYPE_C2_SD,             "Nextor Sunrise + 1MB Mapper + C2 (SD)"  },
            { use_c2_usb,      ROM_TYPE_C2_USB,            "Nextor Sunrise + 1MB Mapper + C2 (USB)" },
        };

        uint32_t nextor_size = sizeof(___nextor_kernel_Nextor_2_1_4_SunriseIDE_MasterOnly_ROM);
        uint32_t wifi_rom_size = (uint32_t)______wifi_bios_ESP8266P_rom_len;
        if (use_wifi && wifi_rom_size != WIFI_ROM_SIZE) {
            printf("Error: embedded ESP8266P WiFi ROM size %u != expected %u\n",
                   wifi_rom_size, (unsigned)WIFI_ROM_SIZE);
            free(config_buffer);
            return 1;
        }

        for (int ne = 0; ne < (int)(sizeof(nextor_entries)/sizeof(nextor_entries[0])); ne++) {
            if (!nextor_entries[ne].enabled) continue;

            uint8_t mapper_byte = nextor_entries[ne].mapper;
            bool entry_wifi_eligible = (mapper_byte == ROM_TYPE_SUNRISE
                                        || mapper_byte == ROM_TYPE_SUNRISE_MAPPER
                                        || mapper_byte == ROM_TYPE_SUNRISE_SD
                                        || mapper_byte == ROM_TYPE_SUNRISE_MAPPER_SD);
            bool entry_wifi = use_wifi && entry_wifi_eligible;
            // Apply SCC flags only to Carnivore2 modes (matches loadrom tool behaviour).
            if (mapper_byte == ROM_TYPE_C2_SD || mapper_byte == ROM_TYPE_C2_USB) {
                if (scc_emulation) mapper_byte |= ROM_TYPE_SCC_FLAG;
                else if (scc_plus) mapper_byte |= ROM_TYPE_SCCP_FLAG;
            }
            if (entry_wifi) {
                mapper_byte |= ROM_TYPE_WIFI_FLAG;
            }

            char nextor_rom_name[MAX_FILE_NAME_LENGTH] = {0};
            snprintf(nextor_rom_name, MAX_FILE_NAME_LENGTH, "%s", nextor_entries[ne].name);
            memcpy(config_buffer + config_offset, nextor_rom_name, MAX_FILE_NAME_LENGTH);
            config_offset += MAX_FILE_NAME_LENGTH;
            config_buffer[config_offset++] = mapper_byte;
            // Size field stores Sunrise ROM size only; the firmware reads the
            // appended WiFi ROM at `rom + offset + available_length`.
            memcpy(config_buffer + config_offset, &nextor_size, sizeof(nextor_size));
            config_offset += sizeof(nextor_size);
            uint32_t nextor_offset = base_offset;
            memcpy(config_buffer + config_offset, &nextor_offset, sizeof(nextor_offset));
            config_offset += sizeof(nextor_offset);
            {
                const char *scc_label = "";
                if (mapper_byte & ROM_TYPE_SCC_FLAG) scc_label = " +SCC";
                else if (mapper_byte & ROM_TYPE_SCCP_FLAG) scc_label = " +SCC+";
                const char *wifi_label = entry_wifi ? " +WiFi" : "";
                printf("File %02d: Name = %-50s, Size = %07u bytes, Flash Offset = 0x%08X, Mapper = %s%s%s\n",
                       file_index, nextor_entries[ne].name, nextor_size, nextor_offset,
                       mapper_description(nextor_entries[ne].mapper), scc_label, wifi_label);
            }
            uint32_t entry_total = nextor_size + (entry_wifi ? wifi_rom_size : 0u);
            total_rom_size += entry_total;
            if (total_rom_size > MAX_TOTAL_ROM_SIZE) {
                printf("Total ROM data exceeds maximum supported size of %u bytes.\n", (unsigned)MAX_TOTAL_ROM_SIZE);
                free(config_buffer);
                return 1;
            }

            nextor_entry_wifi[nextor_entry_count] = entry_wifi;
            nextor_entry_count++;
            file_index++;
            base_offset += entry_total;
        }
    }

    // Scan the current directory for .ROM files
    dir = opendir(".");
    if (!dir) {
        printf("Failed to open directory!\n");
        free(config_buffer);
        return 1;
    }

    // Process each directory entry
    while ((entry = readdir(dir)) != NULL) {
        // Check for .ROM or .rom extension
        if ((strstr(entry->d_name, ".ROM") == NULL) && (strstr(entry->d_name, ".rom") == NULL)) {
            continue;
        }

        // Check maximum number of ROM files
        if (file_count >= MAX_ROM_FILES) {
            printf("Maximum number of ROM files (%d) reached\n", MAX_ROM_FILES);
            break;
        }

        // Extract ROM name (without extension) and check for forced mapper tags
        char rom_name[MAX_FILE_NAME_LENGTH] = {0};
        uint32_t rom_size = 0;
        uint32_t fl_offset = base_offset;
        bool mapper_forced = false;
        uint8_t forced_mapper_byte = 0;

        char *dot_position = strstr(entry->d_name, ".ROM");
        if (dot_position == NULL) {
            dot_position = strstr(entry->d_name, ".rom");
        }

        size_t name_length;
        if (dot_position != NULL) {
            name_length = (size_t)(dot_position - entry->d_name);

            char *last_period = NULL;
            for (char *p = entry->d_name; p < dot_position; ++p) {
                if (*p == '.') {
                    last_period = p;
                }
            }

            if (last_period) {
                const char *token_start = last_period + 1;
                size_t token_length = (size_t)(dot_position - token_start);
                if (token_length > 0) {
                    char mapper_token[32];
                    if (token_length >= sizeof(mapper_token)) {
                        token_length = sizeof(mapper_token) - 1;
                    }
                    memcpy(mapper_token, token_start, token_length);
                    mapper_token[token_length] = '\0';

                    uint8_t candidate = mapper_number_from_description(mapper_token);
                    if (candidate == 10 || candidate == 11 || candidate == 15 || candidate == 16) {
                        printf("Ignoring SYSTEM mapper tag in %s (cannot be forced)\n", entry->d_name);
                    } else if (candidate != 0) {
                        mapper_forced = true;
                        forced_mapper_byte = candidate;
                        name_length = (size_t)(last_period - entry->d_name);
                    }
                }
            }
        } else {
            name_length = strnlen(entry->d_name, MAX_FILE_NAME_LENGTH);
        }

        if (name_length > MAX_FILE_NAME_LENGTH) {
            name_length = MAX_FILE_NAME_LENGTH;
        }
        snprintf(rom_name, name_length + 1, "%s", entry->d_name);
        if (name_length < MAX_FILE_NAME_LENGTH) {
            rom_name[name_length] = '\0';
        } else {
            rom_name[MAX_FILE_NAME_LENGTH - 1] = '\0';
        }

        rom_size = file_size(entry->d_name);
        if (rom_size == 0) {
            printf("Skipping %s (unable to determine size)\n", entry->d_name);
            continue;
        }

        // Detect mapper type
        if (rom_size > MAX_ROM_SIZE || rom_size < MIN_ROM_SIZE) {
            printf("Skipping %s (invalid ROM size)\n", entry->d_name);
            continue;
        }

        uint8_t mapper_byte = mapper_forced ? forced_mapper_byte : detect_rom_type(entry->d_name, rom_size);
        if (mapper_byte == 0) {
            printf("Skipping %s (unsupported mapper)\n", entry->d_name);
            continue;
        }

        // Apply SCC flags for compatible mappers
        if (scc_emulation && (mapper_byte == 3 || mapper_byte == ROM_TYPE_MANBOW2)) {
            mapper_byte |= 0x80;
        }
        if (scc_plus && (mapper_byte == 3 || mapper_byte == ROM_TYPE_MANBOW2)) {
            mapper_byte |= 0x40;
        }

        // Check if ROM fits in the remaining space
        if (config_offset + CONFIG_RECORD_SIZE > CONFIG_AREA_SIZE) {
            printf("Configuration area capacity exceeded\n");
            closedir(dir);
            free(config_buffer);
            return 1;
        }

        // Write ROM metadata to configuration buffer
        memcpy(config_buffer + config_offset, rom_name, MAX_FILE_NAME_LENGTH);
        config_offset += MAX_FILE_NAME_LENGTH;
        config_buffer[config_offset++] = mapper_byte;
        memcpy(config_buffer + config_offset, &rom_size, sizeof(rom_size));
        config_offset += sizeof(rom_size);
        memcpy(config_buffer + config_offset, &fl_offset, sizeof(fl_offset));
        config_offset += sizeof(fl_offset);

        // Print ROM information
        {
            const char *scc_label = "";
            if (mapper_byte & 0x80) scc_label = " +SCC";
            else if (mapper_byte & 0x40) scc_label = " +SCC+";
            printf("File %02d: Name = %-50s, Size = %07u bytes, Flash Offset = 0x%08X, Mapper = %s%s%s\n",
                file_index, rom_name, rom_size, fl_offset, mapper_description(mapper_byte & 0x3F),
                scc_label, mapper_forced ? " (forced)" : "");
        }

        snprintf(files[file_count].file_name, sizeof(files[file_count].file_name), "%s", entry->d_name);
        files[file_count].file_size = rom_size;
        file_count++;
        file_index++;
        base_offset += rom_size;
        total_rom_size += rom_size;

        if (total_rom_size > MAX_TOTAL_ROM_SIZE) {
            printf("Total ROM data exceeds maximum supported size of %u bytes.\n", (unsigned)MAX_TOTAL_ROM_SIZE);
            closedir(dir);
            free(config_buffer);
            return 1;
        }
    }

    closedir(dir); // Close the directory

    // Handle case of no ROM files found
    if (file_count == 0) {
        if (include_nextor) {
            printf("No external ROM files found; generating image with embedded Nextor only.\n");
        } else {
            printf("No ROM files found in the current directory.\n\n");
            print_usage(argv[0] ? argv[0] : "multirom");
            free(config_buffer);
            return 1;
        }
    }

    // Prepare the final combined binary image
    const size_t firmware_size = sizeof(___pico_multirom_build_multirom_bin);
    if (firmware_size == 0) {
        printf("Embedded firmware payload is empty\n");
        free(config_buffer);
        return 1;
    }

    // Sanity check embedded menu ROM size
    const size_t menu_rom_size = sizeof(___msx_dist_menu_rom);
    if (menu_rom_size < MENU_COPY_SIZE) {
        printf("Embedded menu ROM is smaller than the expected %d bytes\n", MENU_COPY_SIZE);
        free(config_buffer);
        return 1;
    }

    // Sanity check embedded Nextor ROM size
    const size_t nextor_rom_size = sizeof(___nextor_kernel_Nextor_2_1_4_SunriseIDE_MasterOnly_ROM);
    if (include_nextor && nextor_rom_size == 0) {
        printf("Embedded Nextor ROM payload is empty\n");
        free(config_buffer);
        return 1;
    }

    // Final flash image layout: [firmware][config area][menu slice][Nextor ROM + scanned ROM payloads]
    const size_t total_size = firmware_size + CONFIG_AREA_SIZE + MENU_COPY_SIZE + total_rom_size;
    uint8_t *combined_buffer = (uint8_t *)malloc(total_size);
    if (!combined_buffer) {
        printf("Failed to allocate combined buffer\n");
        free(config_buffer);
        return 1;
    }

    size_t offset = 0;
    // Copy the embedded Pico firmware blob.
    memcpy(combined_buffer + offset, ___pico_multirom_build_multirom_bin, firmware_size);
    offset += firmware_size;

    // Copy the portion of the MSX menu ROM that precedes the dynamic configuration structure.
    memcpy(combined_buffer + offset, ___msx_dist_menu_rom, MENU_COPY_SIZE);
    offset += MENU_COPY_SIZE;

    // Drop in the generated configuration block (ROM metadata + offsets).
    memcpy(combined_buffer + offset, config_buffer, CONFIG_AREA_SIZE);
    offset += CONFIG_AREA_SIZE;

#ifdef DEBUG
    {
        const char *rom_dump_filename = "multirom.rom";
        FILE *rom_dump = fopen(rom_dump_filename, "wb");
        if (!rom_dump) {
            printf("DEBUG: Failed to open %s for menu dump\n", rom_dump_filename);
        } else {
            size_t rom_bytes_written = fwrite(combined_buffer + firmware_size, 1,
                                             MENU_COPY_SIZE + CONFIG_AREA_SIZE, rom_dump);
            if (rom_bytes_written != (MENU_COPY_SIZE + CONFIG_AREA_SIZE)) {
                printf("DEBUG: Menu dump truncated (%zu of %zu bytes)\n",
                       rom_bytes_written, (size_t)(MENU_COPY_SIZE + CONFIG_AREA_SIZE));
            } else {
                printf("DEBUG: Wrote %zu bytes to %s\n",
                       rom_bytes_written, rom_dump_filename);
            }
            fclose(rom_dump);
        }
    }
#endif

    if (include_nextor) {
        for (int ne = 0; ne < nextor_entry_count; ne++) {
            memcpy(combined_buffer + offset, ___nextor_kernel_Nextor_2_1_4_SunriseIDE_MasterOnly_ROM, nextor_rom_size);
            offset += nextor_rom_size;
            if (nextor_entry_wifi[ne]) {
                memcpy(combined_buffer + offset, ______wifi_bios_ESP8266P_rom, ______wifi_bios_ESP8266P_rom_len);
                offset += ______wifi_bios_ESP8266P_rom_len;
            }
        }
    }

    uint8_t io_buffer[4096];
    // Append every scanned ROM in discovery order right after the Nextor payload.
    for (int i = 0; i < file_count; i++) {
        FILE *rom_file = fopen(files[i].file_name, "rb");
        if (!rom_file) {
            printf("Failed to open ROM file %s\n", files[i].file_name);
            free(combined_buffer);
            free(config_buffer);
            return 1;
        }

        size_t bytes_read;
        while ((bytes_read = fread(io_buffer, 1, sizeof(io_buffer), rom_file)) > 0) {
            memcpy(combined_buffer + offset, io_buffer, bytes_read);
            offset += bytes_read;
        }

        fclose(rom_file);
    }

    // Final sanity check
    if (offset != total_size) {
        printf("Warning: combined buffer size mismatch (expected %zu, got %zu)\n", total_size, offset);
    }

    create_uf2_file(combined_buffer, offset, uf2_output_filename); // Create the UF2 file

    // Clean up and exit
    free(combined_buffer);
    free(config_buffer);
    return 0;
}