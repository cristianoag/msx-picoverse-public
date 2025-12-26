// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// loadrom.c - Windows console application to create a loadrom UF2 file for the MSX PICOVERSE 2040
//
// This program creates a UF2 file to program the Raspberry Pi Pico with the MSX PICOVERSE 2040 loadROM firmware. The UF2 file is
// created with the combined PICO firmware binary file, the configuration area and the ROM file. The configuration area contains the
// information of the ROM file processed by the tool so the MSX can have the required information to load the ROM and execute.
// 
// The configuration record has the following structure:
//  game - Game name                            - 20 bytes (padded by 0x00)
//  mapp - Mapper code                          - 01 byte  (1 - Plain16, 2 - Plain32, 3 - KonamiSCC, 4 - Linear0, 5 - ASCII8, 6 - ASCII16, 7 - Konami)
//  size - Size of the ROM in bytes             - 04 bytes 
//  offset - Offset of the game in the flash    - 04 bytes 
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License. 
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>
#include "uf2format.h"
#include "loadrom.h"

#define UF2FILENAME     "loadrom.uf2"          // this is the UF2 file to program the Raspberry Pi Pico

#define MAX_FILE_NAME_LENGTH    20             // Maximum length of a ROM name
#define MIN_ROM_SIZE            8192           // Minimum size of a ROM file
#define MAX_ROM_SIZE            10*1024*1024    // Maximum size of a ROM file
#define MAX_ANALYSIS_SIZE       131072         // 128KB for the mapper analysis
#define FLASH_START             0x10000000     // Start of the flash memory on the Raspberry Pi Pico

uint32_t file_size(const char *filename);
uint8_t detect_rom_type(const char *filename, uint32_t size);
void write_padding(FILE *file, size_t current_size, size_t target_size, uint8_t padding_byte);
void create_uf2_file(const uint8_t *combined_data, size_t combined_size, const char *uf2_filename);

const char *rom_types[] = {
    "Unknown ROM type", // Default for invalid indices
    "Plain16",          // Index 1
    "Plain32",          // Index 2
    "KonamiS",          // Index 3
    "Linear0",          // Index 4
    "ASCII8",           // Index 5
    "ASCII16",          // Index 6
    "Konami",           // Index 7
    "NEO8",             // Index 8
    "NEO16"             // Index 9
};

// Function to get the size of a file
// Parameters:
// filename - Name of the file
// Returns:
// Size of the file in bytes
uint32_t file_size(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        printf("Failed to open file %s\n", filename);
        return 0;
    }
    fseek(file, 0, SEEK_END);
    uint32_t size = ftell(file);
    fseek(file, 0L, SEEK_SET);
    fclose(file);
    return size;
}

// detect_rom_type - Detect the ROM type using a heuristic approach
// Parameters:
// filename - Name of the ROM file
// size - Size of the ROM file
// Returns:
// ROM type: 0 - Unknown, 1 - 16KB ROM, 2 - 32KB ROM, 3 - Konami SCC ROM, 4 - 48KB Linear0 ROM, 5 - ASCII8 ROM, 6 - ASCII16 ROM, 7 - Konami (without SCC) ROM
uint8_t detect_rom_type(const char *filename, uint32_t size) {
    
    // Define the NEO8 signature
    const char neo8_signature[] = "ROM_NEO8";
    const char neo16_signature[] = "ROM_NE16";

    // Initialize weighted scores for different mapper types
    int konami_score = 0;
    int konami_scc_score = 0;
    int ascii8_score = 0;
    int ascii16_score = 0;

    // Define weights for specific addresses
    const int KONAMI_WEIGHT = 2;
    const int KONAMI_SCC_WEIGHT = 2;
    const int ASCII8_WEIGHT_HIGH = 3;
    const int ASCII8_WEIGHT_LOW = 1;
    const int ASCII16_WEIGHT = 2;

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

    // Determine the size to read (max 128KB or the actual size if smaller)
    size_t read_size = (size > MAX_ANALYSIS_SIZE) ? MAX_ANALYSIS_SIZE : size;
    //size_t read_size = size; 
    // Dynamically allocate memory for the ROM
    uint8_t *rom = (uint8_t *)malloc(read_size);
    if (!rom) {
        printf("Failed to allocate memory for ROM\n");
        fclose(file);
        return 0; // unknown mapper
    }

    fread(rom, 1, read_size, file);
    fclose(file);
    
    // Check if the ROM has the signature "AB" at 0x0000 and 0x0001
    // Those are the cases for 16KB and 32KB ROMs
    if (rom[0] == 'A' && rom[1] == 'B' && size == 16384) {
        free(rom);
        return 1;     // Plain 16KB 
    }

    if (rom[0] == 'A' && rom[1] == 'B' && size <= 32768) {

        //check if it is a normal 32KB ROM or linear0 32KB ROM
        if (rom[0x4000] == 'A' && rom[0x4001] == 'B') {
            free(rom);
            return 4; // Linear0 32KB
        }
        
        free(rom);
        return 2;     // Plain 32KB 
    }

    // Check for the "AB" header at the start
    if (rom[0] == 'A' && rom[1] == 'B') {
        // Check for the NEO8 signature at offset 16
        if (memcmp(&rom[16], neo8_signature, sizeof(neo8_signature) - 1) == 0) {
            return 8; // NEO8 mapper detected
        } else if (memcmp(&rom[16], neo16_signature, sizeof(neo16_signature) - 1) == 0) {
            return 9; // NEO16 mapper detected
        }
    }

    // Check if the ROM has the signature "AB" at 0x4000 and 0x4001
    // That is the case for 48KB ROMs with Linear page 0 config
    if (rom[0x4000] == 'A' && rom[0x4001] == 'B' && size == 49152) {
        free(rom);
        return 4; // Linear0 48KB
    }

    // Heuristic analysis for larger ROMs
    if (size > 32768) {
        // Scan through the ROM data to detect patterns
        for (size_t i = 0; i < read_size - 3; i++) {
            if (rom[i] == 0x32) { // Check for 'ld (nnnn),a' instruction
                uint16_t addr = rom[i + 1] | (rom[i + 2] << 8);
                switch (addr) {
                    case 0x4000:
                    case 0x8000:
                    case 0xA000:
                        konami_score += KONAMI_WEIGHT;
                        break;
                    case 0x5000:
                    case 0x9000:
                    case 0xB000:
                        konami_scc_score += KONAMI_SCC_WEIGHT;
                        break;
                    case 0x6800:
                    case 0x7800:
                        ascii8_score += ASCII8_WEIGHT_HIGH;
                        break;
                    case 0x77FF:
                        ascii16_score += ASCII16_WEIGHT;
                        break;
                    case 0x6000:
                        konami_score += KONAMI_WEIGHT;
                        konami_scc_score += KONAMI_SCC_WEIGHT;
                        ascii8_score += ASCII8_WEIGHT_LOW;
                        ascii16_score += ASCII16_WEIGHT;
                        break;
                    case 0x7000:
                        konami_scc_score += KONAMI_SCC_WEIGHT;
                        ascii8_score += ASCII8_WEIGHT_LOW;
                        ascii16_score += ASCII16_WEIGHT;
                        break;
                    // Add more cases as needed
                }
            }
        }
         
        
        
        printf ("DEBUG: ascii8_score = %d\n", ascii8_score);
        printf ("DEBUG: ascii16_score = %d\n", ascii16_score);
        printf ("DEBUG: konami_score = %d\n", konami_score);
        printf ("DEBUG: konami_scc_score = %d\n\n", konami_scc_score);
        
        
        if (ascii8_score==1) ascii8_score--;

        // Determine the ROM type based on the highest weighted score
        if (konami_scc_score > konami_score && konami_scc_score > ascii8_score && konami_scc_score > ascii16_score) {
            free(rom);
            return 3; // Konami SCC
        }
        if (konami_score > konami_scc_score && konami_score > ascii8_score && konami_score > ascii16_score) {
            free(rom);
            return 7; // Konami
        }
        if (ascii8_score > konami_score && ascii8_score > konami_scc_score && ascii8_score > ascii16_score) {
            free(rom);
            return 5; // ASCII8
        }
        if (ascii16_score > konami_score && ascii16_score > konami_scc_score && ascii16_score > ascii8_score) {
            free(rom);
            return 6; // ASCII16
        }

        if (ascii16_score == konami_scc_score)
        {
            free(rom);
            return 6; // Konami SCC
        }

        free(rom);
        return 0; // unknown mapper
    }
   
}

// create_uf2_file - Create the UF2 file
// This function will create the UF2 file with the firmware, menu and ROM files

void create_uf2_file(const uint8_t *combined_data, size_t combined_size, const char *uf2_filename) {
    if (!combined_data || combined_size == 0) {
        printf("Invalid combined data provided for UF2 generation.\n");
        return;
    }

    // Create/Open for writing the UF2 file
    FILE *uf2_file = fopen(uf2_filename, "wb");
    if (!uf2_file) {
        perror("Failed to create UF2 file");
        return;
    }

    UF2_Block bl;
    memset(&bl, 0, sizeof(bl));

    bl.magicStart0 = UF2_MAGIC_START0;
    bl.magicStart1 = UF2_MAGIC_START1;
    bl.flags = 0x00002000;
    bl.magicEnd = UF2_MAGIC_END;
    bl.targetAddr = FLASH_START;
    bl.numBlocks = (uint32_t)((combined_size + 255) / 256);
    bl.payloadSize = 256;
    bl.fileSize = 0xe48bff56;
    
    int numbl = 0;
    size_t offset = 0;
        while (offset < combined_size) {
        size_t chunk = combined_size - offset;
        if (chunk > bl.payloadSize) {
            chunk = bl.payloadSize;
        }

        memset(bl.data, 0, sizeof(bl.data));
        memcpy(bl.data, combined_data + offset, chunk);

        bl.blockNo = numbl++;
        fwrite(&bl, 1, sizeof(bl), uf2_file);
        bl.targetAddr += bl.payloadSize;
        offset += chunk;
    }
    fclose(uf2_file);
    printf("\nSuccessfully wrote %d blocks to %s.\n", numbl, uf2_filename);

}

// Main function
int main(int argc, char *argv[])
{
    printf("MSX PICOVERSE 2040 LoadROM UF2 Creator v1.0\n");
    printf("(c) 2025 The Retro Hacker\n\n");

    if (argc < 2) {
        printf("Usage: loadrom <romfile> [forced_mapper]\n");
        printf("Possible forced mapper values:\n");
        printf("  1: Plain16\n");
        printf("  2: Plain32\n");
        printf("  3: Konami SCC\n");
        printf("  4: Linear0\n");
        printf("  5: ASCII8\n");
        printf("  6: ASCII16\n");
        printf("  7: Konami\n");
        printf("  8: NEO8\n");
        printf("  9: NEO16\n");
        return 1;
    }

    const uint8_t *firmware_data = ___pico_loadrom_dist_loadrom_bin;
    const size_t firmware_size = sizeof(___pico_loadrom_dist_loadrom_bin);
    const size_t config_size = MAX_FILE_NAME_LENGTH + 1 + 4 + 4; // 20 + 1 + 4 + 4 = 29 bytes
    uint32_t base_offset = 0x1d; // Base offset for the ROM file = 29B (one config record)

    // Open the ROM file
    FILE *rom_file = fopen(argv[1], "rb");
    if (!rom_file) {
        printf("Failed to open ROM file");
        return 1;
    }

    // Process only .rom/.ROM files
    if ((strstr(argv[1], ".ROM") != NULL) || (strstr(argv[1], ".rom") != NULL))
    {
        // Get the size of the ROM file
        uint32_t rom_size = file_size(argv[1]);
        if (rom_size == 0 || rom_size > MAX_ROM_SIZE) {
            printf("Failed to get the size of the ROM file or size not supported.\n");
            fclose(rom_file);
            return 1;
        }

        // Detect the ROM type
        uint8_t rom_type = 0;
        // Check if a forced mapper value was provided as a second parameter
        if (argc >= 3) {
            int forced_mapper = atoi(argv[2]);
            // Validate forced value (adjust valid range as needed)
            if (forced_mapper < 1 || forced_mapper > 9) {
                printf("Forced mapper must be between 1 and 9.\n");
                fclose(rom_file);
                return 1;
            }
            rom_type = forced_mapper;
            printf("ROM type: %s [Forced]\n", rom_types[rom_type]);
        }
        else {
            rom_type = detect_rom_type(argv[1], rom_size);
            if (rom_type == 0) {
                printf("Failed to detect the ROM type. Please check the ROM file.\n");
                fclose(rom_file);
                return 1;
            }
            printf("ROM Type: %s [Auto-detected]\n", rom_types[rom_type]);
        }

        // Write the ROM name to the configuration file
        char rom_name[MAX_FILE_NAME_LENGTH] = {0};

        // Extract the first part of the file name (up to the first '.ROM' or '.rom')
        char *dot_position = strstr(argv[1], ".ROM");
        if (dot_position == NULL) {
            dot_position = strstr(argv[1], ".rom");
        }
        if (dot_position != NULL) {
            size_t name_length = dot_position - argv[1];
            if (name_length > MAX_FILE_NAME_LENGTH) {
                name_length = MAX_FILE_NAME_LENGTH;
            }
            strncpy(rom_name, argv[1], name_length);
        } else {
            strncpy(rom_name, argv[1], MAX_FILE_NAME_LENGTH);
        }
        printf("ROM Name: %s\n", rom_name);

        size_t combined_size = firmware_size + config_size + rom_size;
        uint8_t *combined_data = (uint8_t *)malloc(combined_size);
        if (!combined_data) {
            printf("Failed to allocate memory for combined UF2 image.\n");
            fclose(rom_file);
            return 1;
        }

        uint8_t *cursor = combined_data;
        memcpy(cursor, firmware_data, firmware_size);
        cursor += firmware_size;

        memset(cursor, 0, config_size);
        // Write the configuration record to the output file
        memcpy(cursor, rom_name, MAX_FILE_NAME_LENGTH);
        cursor += MAX_FILE_NAME_LENGTH;
        memcpy(cursor, &rom_type, sizeof(rom_type));
        cursor += sizeof(rom_type);
        memcpy(cursor, &rom_size, sizeof(rom_size));
        cursor += sizeof(rom_size);
        memcpy(cursor, &base_offset, sizeof(base_offset));
        cursor += sizeof(base_offset);

        printf("ROM Size: %u bytes\n", rom_size);
        printf("Pico Offset: 0x%08X\n", base_offset);

        size_t bytes_remaining = rom_size;
        while (bytes_remaining > 0) {
            size_t to_read = bytes_remaining < 1024 ? bytes_remaining : 1024;
            size_t read_now = fread(cursor, 1, to_read, rom_file);
            if (read_now == 0) {
                printf("Failed to read ROM file contents.\n");
                free(combined_data);
                fclose(rom_file);
                return 1;
            }
            cursor += read_now;
            bytes_remaining -= read_now;
        }
        fclose(rom_file);

        // Create the UF2 file
        create_uf2_file(combined_data, combined_size, UF2FILENAME);
        free(combined_data);

    } else {
        perror("Invalid ROM file");
        fclose(rom_file);
        return 1;
    }
}