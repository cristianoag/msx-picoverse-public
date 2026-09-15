// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// explorer.c - Windows console application to create the explorer firmware for the MSX PICOVERSE 2350
//
// This program creates a UF2 file to program the Raspberry Pi Pico with the MSX PICOVERSE 2350 Explorer firmware. The UF2 file is
// created with the combined PICO firmware binary file, the MSX MENU ROM file, the configuration file and the ROM files. The
// configuration file contains the information of each ROM file processed by the tool and it is stored in flash immediately after
// the full 32KB MENU ROM so the Pico firmware can read it.
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
#include <explorer.h>
#include <menu.h>
#include <nextor.h>
#include <wifibios.h>
#include <esp8266p_rom.h>
#include <fmpac_bios.h>
#include <sfg_bios.h>
#include "mapper_detect.h"

#ifndef APP_VERSION
#define APP_VERSION "v1.00"
#endif

#define UF2FILENAME             "explorer.uf2"  // UF2 produced by this tool
#define MENU_COPY_SIZE          (32 * 1024)     // Full menu ROM copied verbatim before config payload
#define CONFIG_AREA_SIZE        (16 * 1024)     // Size of the configuration area stored after the menu ROM
#define TARGET_FILE_SIZE        (MENU_COPY_SIZE + CONFIG_AREA_SIZE) // Size of menu ROM + configuration area
#define WIFI_CONFIG_ROM_SIZE    (8 * 1024)      // Hidden WiFi configuration ROM payload stored after config area
#define WIFI_BIOS_ROM_SIZE      (16 * 1024)     // Hidden ESP8266P UNAPI BIOS payload used by Sunrise WiFi support
#define FMPAC_BIOS_ROM_SIZE     (64 * 1024)     // Hidden FM-PAC BIOS payload used by Explorer MSX-MUSIC support
#define SFG_BIOS_ROM_SIZE       (64 * 1024)     // Hidden Yamaha SFG ROM payload used by Explorer YM2151/SFG support
#define MAX_FILE_NAME_LENGTH    71              // Maximum length of a ROM name on the 80-column detail screen
#define FLASH_START             0x10000000      // Start of the flash memory on the Raspberry Pi Pico
#define MAX_ROM_FILES           128             // Maximum number of ROM files
#define MAX_TOTAL_ROM_SIZE      (12U * 1024U * 1024U) // Cap combined visible ROM payload to 12 MB
#define MAX_ROM_SIZE            15*1024*1024    // Maximum size of a ROM file
#define MIN_ROM_SIZE            8192            // Minimum size of a ROM file
#define MAX_ANALYSIS_SIZE       131072          // 128KB for the mapper analysis
#define CONFIG_RECORD_SIZE      (MAX_FILE_NAME_LENGTH + 1 + sizeof(uint32_t) + sizeof(uint32_t))
#define MAX_UF2_FILENAME_LENGTH 512

#define ROM_TYPE_SUNRISE           10
#define ROM_TYPE_SUNRISE_MAPPER    11
#define ROM_TYPE_SUNRISE_SD        15
#define ROM_TYPE_SUNRISE_MAPPER_SD 16
#define ROM_TYPE_C2_SD             17
#define ROM_TYPE_C2_USB            18
#define ROM_TYPE_MEGARAM_SD        19
#define ROM_TYPE_MEGARAM_USB       20
#define ROM_TYPE_MEGARAM           21

static const char *MAPPER_DESCRIPTIONS[] = {
    "PLA-16", "PLA-32", "KonSCC", "PLN-48", "ASC-08",
    "ASC-16", "Konami", "NEO-8", "NEO-16", "SYSTEM",
    "SYSTEM", "ASC16X", "PLN-64", "MANBW2"
};

#define MAPPER_DESCRIPTION_COUNT (sizeof(MAPPER_DESCRIPTIONS) / sizeof(MAPPER_DESCRIPTIONS[0]))

static bool equals_ignore_case(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static uint8_t mapper_number_from_description(const char *description) {
    for (size_t i = 0; i < MAPPER_DESCRIPTION_COUNT; ++i) {
        const char *tag = MAPPER_DESCRIPTIONS[i];
        if (!tag || tag[0] == '\0') continue;
        // Skip the SYSTEM placeholders so users can't force reserved slots.
        if (equals_ignore_case(tag, "SYSTEM")) continue;
        if (equals_ignore_case(description, tag)) {
            return (uint8_t)(i + 1);
        }
    }
    return 0;
}

#if TARGET_FILE_SIZE < MENU_COPY_SIZE // Sanity check
#error "TARGET_FILE_SIZE must be larger than MENU_COPY_SIZE"
#endif

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

// Build modes supported by the tool.
typedef enum {
    BUILD_MODE_STANDARD = 0,    // Standard Explorer build scanning for .ROM files
} BuildMode;

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
    switch (number) {
        case ROM_TYPE_SUNRISE:
        case ROM_TYPE_SUNRISE_MAPPER:
        case ROM_TYPE_SUNRISE_SD:
        case ROM_TYPE_SUNRISE_MAPPER_SD:
        case ROM_TYPE_C2_SD:
        case ROM_TYPE_C2_USB:
        case ROM_TYPE_MEGARAM_SD:
        case ROM_TYPE_MEGARAM_USB:
        case ROM_TYPE_MEGARAM:
            return "SYSTEM";
        default:
            break;
    }

    if (number <= 0 || (size_t)number > MAPPER_DESCRIPTION_COUNT) {
        return "Unknown";
    }
    return MAPPER_DESCRIPTIONS[number - 1];
}

// Attempt to guess the mapper type from the ROM contents using the shared
// SHA1 + heuristic detector (mapper_detect.h). Loads the full file into RAM
// so the openMSX softwaredb hash matches the canonical algorithm.
// Returns the mapper byte expected by the firmware (0 signals unsupported/unknown).
uint8_t detect_rom_type(const char *filename, uint32_t size) {
    if (size > MAX_ROM_SIZE || size < MIN_ROM_SIZE) {
        printf("Invalid ROM size\n");
        return 0;
    }

    FILE *file = fopen(filename, "rb");
    if (!file) {
        printf("Failed to open ROM file\n");
        return 0;
    }

    uint8_t *rom = (uint8_t *)malloc(size);
    if (!rom) {
        printf("Failed to allocate memory for ROM\n");
        fclose(file);
        return 0;
    }

    size_t read_bytes = fread(rom, 1, size, file);
    fclose(file);
    if (read_bytes != size) {
        free(rom);
        return 0;
    }

    uint8_t mapper = mapper_detect_buffer(rom, size);
    free(rom);
    return mapper;
}

// Print usage information
static void print_usage(const char *prog_name) {

    printf("Usage: %s [-h] [-a] [-r] [-s1] [-m1] [-c1] [-r1] [-s2] [-m2] [-c2] [-r2] [-o <filename>]\n", prog_name);
    printf("  without options, the tool scans the current directory for .ROM files to include in the Explorer image\n");
    printf("Options:\n");
    printf("  -h   Show this help message\n");
    printf("  -a, --allnextor  Include all embedded Nextor system ROM options\n");
    printf("  -r, --megaram    Include standalone 1MB MegaRAM without Nextor or memory mapper\n");
    printf("  -s1, --sunrise-sd  Include Sunrise IDE Nextor ROM (microSD card)\n");
    printf("  -m1, --mapper-sd   Include Sunrise IDE Nextor ROM + 1MB mapper (microSD card)\n");
    printf("  -c1, --carnivore2-sd  Include Sunrise IDE Nextor ROM + 1MB mapper + Carnivore2 RAM (microSD card)\n");
    printf("  -r1, --megaram-sd  Include Sunrise IDE Nextor ROM + 1MB mapper + 1MB MegaRAM (microSD card)\n");
    printf("  -s2, --sunrise-usb Include Sunrise IDE Nextor ROM (USB pendrive)\n");
    printf("  -m2, --mapper-usb  Include Sunrise IDE Nextor ROM + 1MB mapper (USB pendrive)\n");
    printf("  -c2, --carnivore2-usb Include Sunrise IDE Nextor ROM + 1MB mapper + Carnivore2 RAM (USB pendrive)\n");
    printf("  -r2, --megaram-usb Include Sunrise IDE Nextor ROM + 1MB mapper + 1MB MegaRAM (USB pendrive)\n");
    printf("  Options -s1, -m1, -c1, -r1, -s2, -m2, -c2, -r2 can be combined to add multiple Nextor entries\n");
    printf("  -o <filename>, --output <filename>  Set UF2 output filename (default %s)\n", UF2FILENAME);
    printf("\n");
    printf("  append a mapper tag before the extension to force detection (case-insensitive)\n");
    printf("  e.g., \"Knight Mare.PL-32.ROM\" forces PL-32; \"SYSTEM\" tags are ignored\n\n");
    printf("  here are the mapper descriptions you can use to force a specific mapper type:\n");
    for (size_t i = 0; i < MAPPER_DESCRIPTION_COUNT; ++i) {
        const char *tag = MAPPER_DESCRIPTIONS[i];
        if (!tag || tag[0] == '\0') continue;
        if (equals_ignore_case(tag, "SYSTEM")) continue;
        printf("  %s", tag);
    }
    printf("\n");
    printf("UF2 output file: %s\n", UF2FILENAME);
}

// Serialize the fully joined binary image into UF2 blocks so the Pico can be programmed via USB MSC.
void create_uf2_file(const uint8_t *data, size_t size, const char *uf2_filename) {

    if (!data || size == 0) {
        printf("No data provided to create UF2 file\n");
        return;
    }

#ifdef DEBUG // Dump the payload to a binary file for debugging
    const char *dump_filename = "explorer_payload.bin";
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
    printf("MSX PICOVERSE 2350 Explorer UF2 Creator %s\n", APP_VERSION);
    printf("(c) 2026 The Retro Hacker\n\n");

    bool show_help = false;
    bool use_sunrise_sd = false;
    bool use_mapper_sd = false;
    bool use_c2_sd = false;
    bool use_megaram_sd = false;
    bool use_megaram = false;
    bool use_sunrise_usb = false;
    bool use_mapper_usb = false;
    bool use_c2_usb = false;
    bool use_megaram_usb = false;
    const char *bad_option = NULL;
    const char *missing_output_option = NULL;
    char uf2_output_filename[MAX_UF2_FILENAME_LENGTH];

    strncpy(uf2_output_filename, UF2FILENAME, sizeof(uf2_output_filename));
    uf2_output_filename[sizeof(uf2_output_filename) - 1] = '\0';

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
            show_help = true;
        } else if ((strcmp(argv[i], "-a") == 0) || (strcmp(argv[i], "--allnextor") == 0)) {
            use_sunrise_sd = true;
            use_mapper_sd = true;
            use_c2_sd = true;
            use_megaram_sd = true;
            use_sunrise_usb = true;
            use_mapper_usb = true;
            use_c2_usb = true;
            use_megaram_usb = true;
        } else if ((strcmp(argv[i], "-s1") == 0) || (strcmp(argv[i], "--sunrise-sd") == 0)) {
            use_sunrise_sd = true;
        } else if ((strcmp(argv[i], "-m1") == 0) || (strcmp(argv[i], "--mapper-sd") == 0)) {
            use_mapper_sd = true;
        } else if ((strcmp(argv[i], "-c1") == 0) || (strcmp(argv[i], "--carnivore2-sd") == 0)) {
            use_c2_sd = true;
        } else if ((strcmp(argv[i], "-r") == 0) || (strcmp(argv[i], "--megaram") == 0)) {
            use_megaram = true;
        } else if ((strcmp(argv[i], "-r1") == 0) || (strcmp(argv[i], "--megaram-sd") == 0)) {
            use_megaram_sd = true;
        } else if ((strcmp(argv[i], "-s2") == 0) || (strcmp(argv[i], "--sunrise-usb") == 0)) {
            use_sunrise_usb = true;
        } else if ((strcmp(argv[i], "-m2") == 0) || (strcmp(argv[i], "--mapper-usb") == 0)) {
            use_mapper_usb = true;
        } else if ((strcmp(argv[i], "-c2") == 0) || (strcmp(argv[i], "--carnivore2-usb") == 0)) {
            use_c2_usb = true;
        } else if ((strcmp(argv[i], "-r2") == 0) || (strcmp(argv[i], "--megaram-usb") == 0)) {
            use_megaram_usb = true;
        } else if ((strcmp(argv[i], "-o") == 0) || (strcmp(argv[i], "--output") == 0)) {
            if (i + 1 >= argc) {
                missing_output_option = argv[i];
                break;
            }
            ++i;
            strncpy(uf2_output_filename, argv[i], sizeof(uf2_output_filename));
            uf2_output_filename[sizeof(uf2_output_filename) - 1] = '\0';
        } else {
            bad_option = argv[i];
            break;
        }
    }

    if (missing_output_option) {
        printf("Option %s requires a filename argument\n\n", missing_output_option);
        print_usage(argv[0] ? argv[0] : "explorer");
        return 1;
    }

    // Handle bad options
    if (bad_option) {
        printf("Unknown option: %s\n\n", bad_option);
        print_usage(argv[0] ? argv[0] : "explorer");
        return 1;
    }

    // Show help and exit
    if (show_help) {
        print_usage(argv[0] ? argv[0] : "explorer");
        return 0;
    }

    bool include_nextor = use_sunrise_sd || use_mapper_sd || use_c2_sd || use_megaram_sd ||
                          use_sunrise_usb || use_mapper_usb || use_c2_usb || use_megaram_usb;
    struct {
        bool enabled;
        uint8_t mapper;
        const char *name;
    } nextor_entries[] = {
        { use_sunrise_sd,  ROM_TYPE_SUNRISE_SD,        "Nextor Sunrise IDE (SD)" },
        { use_mapper_sd,   ROM_TYPE_SUNRISE_MAPPER_SD, "Nextor Sunrise IDE + 1MB Mapper (SD)" },
        { use_c2_sd,       ROM_TYPE_C2_SD,             "Nextor Sunrise IDE + 1MB Mapper + C2 RAM (SD)" },
        { use_megaram_sd,  ROM_TYPE_MEGARAM_SD,        "Nextor Sunrise IDE + 1MB Mapper + 1MB MegaRAM (SD)" },
        { use_sunrise_usb, ROM_TYPE_SUNRISE,           "Nextor Sunrise IDE (USB)" },
        { use_mapper_usb,  ROM_TYPE_SUNRISE_MAPPER,    "Nextor Sunrise + 1MB Mapper (USB)" },
        { use_c2_usb,      ROM_TYPE_C2_USB,            "Nextor Sunrise + 1MB Mapper + C2 RAM (USB)" },
        { use_megaram_usb, ROM_TYPE_MEGARAM_USB,       "Nextor Sunrise + 1MB Mapper + 1MB MegaRAM (USB)" },
    };

    // Standard Explorer build mode
    printf("Scanning current directory for .ROM files...\n\n");
    DIR *dir;
    struct dirent *entry;
    FileInfo files[MAX_ROM_FILES]; // Array to track discovered ROM files
    int file_count = 0;
    int file_index = 1;
    uint32_t base_offset = TARGET_FILE_SIZE + WIFI_CONFIG_ROM_SIZE + WIFI_BIOS_ROM_SIZE + FMPAC_BIOS_ROM_SIZE + SFG_BIOS_ROM_SIZE; // Visible ROMs start after hidden payloads
    size_t total_rom_size = 0;
    size_t config_offset = 0;
    uint8_t *config_buffer = (uint8_t *)malloc(CONFIG_AREA_SIZE); // Configuration area buffer
    if (!config_buffer) {
        printf("Failed to allocate configuration buffer\n");
        return 1;
    }
    memset(config_buffer, 0xFF, CONFIG_AREA_SIZE); // Initialize config area to 0xFF

    // Include embedded Nextor ROM entries, one per selected option.
    if (include_nextor) {
        uint32_t nextor_size = sizeof(___resources_Nextor_2_1_4_SunriseIDE_MasterOnly_ROM);
        for (int ne = 0; ne < (int)(sizeof(nextor_entries) / sizeof(nextor_entries[0])); ++ne) {
            char nextor_rom_name[MAX_FILE_NAME_LENGTH] = {0};
            uint32_t nextor_offset;

            if (!nextor_entries[ne].enabled) {
                continue;
            }

            if (config_offset + CONFIG_RECORD_SIZE > CONFIG_AREA_SIZE) {
                printf("Configuration area capacity exceeded\n");
                free(config_buffer);
                return 1;
            }

            strncpy(nextor_rom_name, nextor_entries[ne].name, MAX_FILE_NAME_LENGTH);
            memcpy(config_buffer + config_offset, nextor_rom_name, MAX_FILE_NAME_LENGTH);
            config_offset += MAX_FILE_NAME_LENGTH;
            config_buffer[config_offset++] = nextor_entries[ne].mapper;
            memcpy(config_buffer + config_offset, &nextor_size, sizeof(nextor_size));
            config_offset += sizeof(nextor_size);
            nextor_offset = base_offset;
            memcpy(config_buffer + config_offset, &nextor_offset, sizeof(nextor_offset));
            config_offset += sizeof(nextor_offset);

            printf("File %02d: Name = %-60s, Size = %07u bytes, Flash Offset = 0x%08X, Mapper = %s\n",
                   file_index, nextor_entries[ne].name, nextor_size, nextor_offset,
                   mapper_description(nextor_entries[ne].mapper));

            total_rom_size += nextor_size;
            if (total_rom_size > MAX_TOTAL_ROM_SIZE) {
                printf("Total ROM data exceeds maximum supported size of %u bytes.\n", (unsigned)MAX_TOTAL_ROM_SIZE);
                free(config_buffer);
                return 1;
            }

            file_index++;
            base_offset += nextor_size;
        }
    }

    if (use_megaram) {
        char megaram_name[MAX_FILE_NAME_LENGTH] = {0};
        uint32_t megaram_size = 0;
        uint32_t megaram_offset = base_offset;

        if (config_offset + CONFIG_RECORD_SIZE > CONFIG_AREA_SIZE) {
            printf("Configuration area capacity exceeded\n");
            free(config_buffer);
            return 1;
        }

        strncpy(megaram_name, "Brazilian MegaRAM (1MB)", MAX_FILE_NAME_LENGTH);
        memcpy(config_buffer + config_offset, megaram_name, MAX_FILE_NAME_LENGTH);
        config_offset += MAX_FILE_NAME_LENGTH;
        config_buffer[config_offset++] = ROM_TYPE_MEGARAM;
        memcpy(config_buffer + config_offset, &megaram_size, sizeof(megaram_size));
        config_offset += sizeof(megaram_size);
        memcpy(config_buffer + config_offset, &megaram_offset, sizeof(megaram_offset));
        config_offset += sizeof(megaram_offset);

        printf("File %02d: Name = %-60s, Size = %07u bytes, Flash Offset = 0x%08X, Mapper = %s\n",
               file_index, megaram_name, megaram_size, megaram_offset,
               mapper_description(ROM_TYPE_MEGARAM));

        file_index++;
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
                    if (candidate == 10) {
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
        strncpy(rom_name, entry->d_name, name_length);
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
         printf("File %02d: Name = %-60s, Size = %07u bytes, Flash Offset = 0x%08X, Mapper = %s%s\n",
             file_index, rom_name, rom_size, fl_offset, mapper_description(mapper_byte),
             mapper_forced ? " (forced)" : "");

        strncpy(files[file_count].file_name, entry->d_name, sizeof(files[file_count].file_name));
        files[file_count].file_name[sizeof(files[file_count].file_name) - 1] = '\0';
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
        if (include_nextor && use_megaram) {
            printf("No external ROM files found; generating image with embedded Nextor and MegaRAM entries only.\n");
        } else if (include_nextor) {
            printf("No external ROM files found; generating image with embedded Nextor entries only.\n");
        } else if (use_megaram) {
            printf("No external ROM files found; generating image with standalone MegaRAM entry only.\n");
        } else {
            printf("No ROM files found in the current directory.\n\n");
            print_usage(argv[0] ? argv[0] : "explorer");
            free(config_buffer);
            return 1;
        }
    }

    // Prepare the final combined binary image
    const size_t firmware_size = sizeof(___pico_explorer_build_explorer_bin);
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
    const size_t nextor_rom_size = sizeof(___resources_Nextor_2_1_4_SunriseIDE_MasterOnly_ROM);
    if (include_nextor && nextor_rom_size == 0) {
        printf("Embedded Nextor ROM payload is empty\n");
        free(config_buffer);
        return 1;
    }

    const size_t wifi_config_rom_size = sizeof(______wifi_config_ESP8266_config_rom);
    if (wifi_config_rom_size > WIFI_CONFIG_ROM_SIZE) {
        printf("Embedded WiFi configuration ROM must fit in %u bytes (found %zu)\n",
               (unsigned)WIFI_CONFIG_ROM_SIZE, wifi_config_rom_size);
        free(config_buffer);
        return 1;
    }

    const size_t wifi_bios_rom_size = sizeof(______wifi_bios_ESP8266P_rom);
    if (wifi_bios_rom_size != WIFI_BIOS_ROM_SIZE) {
        printf("Embedded ESP8266P WiFi BIOS must be %u bytes (found %zu)\n",
               (unsigned)WIFI_BIOS_ROM_SIZE, wifi_bios_rom_size);
        free(config_buffer);
        return 1;
    }

    const size_t fmpac_bios_rom_size = sizeof(______loadrom_pio_fmpac_FMPCCMFC_BIN);
    if (fmpac_bios_rom_size != FMPAC_BIOS_ROM_SIZE) {
        printf("Embedded FM-PAC BIOS must be %u bytes (found %zu)\n",
               (unsigned)FMPAC_BIOS_ROM_SIZE, fmpac_bios_rom_size);
        free(config_buffer);
        return 1;
    }

    const size_t sfg_bios_rom_size = sizeof(___resources_SFG_64K_ROM);
    if (sfg_bios_rom_size != SFG_BIOS_ROM_SIZE) {
        printf("Embedded SFG BIOS must be %u bytes (found %zu)\n",
               (unsigned)SFG_BIOS_ROM_SIZE, sfg_bios_rom_size);
        free(config_buffer);
        return 1;
    }

    // Final flash image layout: [firmware][menu ROM][config area][WiFi config ROM][ESP8266P BIOS][FM-PAC BIOS][SFG BIOS][Nextor ROM + scanned ROM payloads]
    const size_t total_size = firmware_size + MENU_COPY_SIZE + CONFIG_AREA_SIZE + WIFI_CONFIG_ROM_SIZE + WIFI_BIOS_ROM_SIZE + FMPAC_BIOS_ROM_SIZE + SFG_BIOS_ROM_SIZE + total_rom_size;
    uint8_t *combined_buffer = (uint8_t *)malloc(total_size);
    if (!combined_buffer) {
        printf("Failed to allocate combined buffer\n");
        free(config_buffer);
        return 1;
    }
    memset(combined_buffer, 0xFF, total_size);

    size_t offset = 0;
    // Copy the embedded Pico firmware blob.
    memcpy(combined_buffer + offset, ___pico_explorer_build_explorer_bin, firmware_size);
    offset += firmware_size;

    // Copy the full MSX menu ROM.
    memcpy(combined_buffer + offset, ___msx_dist_menu_rom, MENU_COPY_SIZE);
    offset += MENU_COPY_SIZE;

    // Drop in the generated configuration block (ROM metadata + offsets) after the menu ROM.
    memcpy(combined_buffer + offset, config_buffer, CONFIG_AREA_SIZE);
    offset += CONFIG_AREA_SIZE;

    // Hidden WiFi configuration payload used by File Hunter F4 setup.
    memcpy(combined_buffer + offset, ______wifi_config_ESP8266_config_rom, wifi_config_rom_size);
    offset += WIFI_CONFIG_ROM_SIZE;

    // Hidden ESP8266P UNAPI BIOS exposed when Sunrise WiFi Support is enabled.
    memcpy(combined_buffer + offset, ______wifi_bios_ESP8266P_rom, wifi_bios_rom_size);
    offset += WIFI_BIOS_ROM_SIZE;

    // Hidden FM-PAC BIOS exposed when an Explorer ROM is launched with MSX-MUSIC.
    memcpy(combined_buffer + offset, ______loadrom_pio_fmpac_FMPCCMFC_BIN, fmpac_bios_rom_size);
    offset += FMPAC_BIOS_ROM_SIZE;

    // Hidden Yamaha SFG BIOS exposed when an Explorer ROM is launched with YM2151/SFG.
    memcpy(combined_buffer + offset, ___resources_SFG_64K_ROM, sfg_bios_rom_size);
    offset += SFG_BIOS_ROM_SIZE;

#ifdef DEBUG
    {
        const char *rom_dump_filename = "explorer.rom";
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
        for (int ne = 0; ne < (int)(sizeof(nextor_entries) / sizeof(nextor_entries[0])); ++ne) {
            if (!nextor_entries[ne].enabled) {
                continue;
            }
            memcpy(combined_buffer + offset, ___resources_Nextor_2_1_4_SunriseIDE_MasterOnly_ROM, nextor_rom_size);
            offset += nextor_rom_size;
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
