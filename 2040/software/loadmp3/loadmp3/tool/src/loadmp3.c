// MSX PICOVERSE PROJECT
// (c) 2024 Cristiano Goncalves
// The Retro Hacker
//
// loadmp3.c - Windows console application to create a loadmp3 UF2 file for the MSX PICOVERSE 2040 audio
//
// This program creates a UF2 file to program the Raspberry Pi Pico with the MSX PICOVERSE 2040 loadMP3 firmware. The UF2 file is
// created with the combined PICO firmware binary file, the configuration area and the MP3 file. The configuration area contains the
// information about the MP3 file that will be played on the MSX.
// 
// The configuration record has the following structure:
//  mp3  - MP3 Name                             - 20 bytes (padded by 0x00)
//  size - Size of the MP3 in bytes             - 04 bytes 
//  offset - Offset of the MP3 in the flash     - 04 bytes 
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License. 
// https://creativecommons.org/licenses/by-nc-sa/4.0/
//

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>
#include "uf2format.h"

#define CONFIG_FILE     "loadmp3.cfg"          // this is the 28 bytes file with the information about the MP3 to load
#define COMBINED_FILE   "loadmp3.cmb"          // this is the final binary file with the firmware, configuration and ROM
#define PICOFIRMWARE    "loadmp3.bin"          // this is the Raspberry PI Pico firmware binary file
#define UF2FILENAME     "loadmp3.uf2"          // this is the UF2 file to program the Raspberry Pi Pico

#define MAX_FILE_NAME_LENGTH    20             // Maximum length of a ROM name
#define MIN_MP3_SIZE            8192           // Minimum size of a ROM file
#define MAX_MP3_SIZE            15*1024*1024    // Maximum size of a ROM file
#define FLASH_START             0x10000000     // Start of the flash memory on the Raspberry Pi Pico

uint32_t file_size(const char *filename);
void write_padding(FILE *file, size_t current_size, size_t target_size, uint8_t padding_byte);
void create_uf2_file(const char *combined_filename, const char *uf2_filename);

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

// create_uf2_file - Create the UF2 file
// This function will create the UF2 file with the firmware, menu and ROM files
// Parameters:
// combined_filename - Name of the combined file
// uf2_filename - Name of the UF2 file
void create_uf2_file(const char *combined_filename, const char *uf2_filename) {
    
    // Get the size of the combined file
    uint32_t sz = file_size(combined_filename);

    // Open the combined file
    FILE *combined_file = fopen(combined_filename, "rb");
    if (!combined_file) {
        perror("Failed to open the combined file");
        return;
    }

    // Create/Open for writing the UF2 file
    FILE *uf2_file = fopen(uf2_filename, "wb");
    if (!uf2_file) {
        perror("Failed to create UF2 file");
        fclose(combined_file);
        return;
    }

    UF2_Block bl;
    memset(&bl, 0, sizeof(bl));

    bl.magicStart0 = UF2_MAGIC_START0;
    bl.magicStart1 = UF2_MAGIC_START1;
    bl.flags = 0x00002000;
    bl.magicEnd = UF2_MAGIC_END;
    bl.targetAddr = FLASH_START;
    bl.numBlocks = (sz + 255) / 256;
    bl.payloadSize = 256;
    bl.fileSize = 0xe48bff56;
    
    int numbl = 0;
    while (fread(bl.data, 1, bl.payloadSize, combined_file)) {
        bl.blockNo = numbl++;
        fwrite(&bl, 1, sizeof(bl), uf2_file);
        bl.targetAddr += bl.payloadSize;
        // clear for next iteration, in case we get a short read
        memset(bl.data, 0, sizeof(bl.data));
    }
    fclose(uf2_file);
    fclose(combined_file);
    printf("\nSuccessfully wrote %d blocks to %s.\n", numbl, uf2_filename);

}

// Main function
int main(int argc, char *argv[])
{
    printf("MSX PICOVERSE 2040 LoadMP3 UF2 Creator v1.0\n");
    printf("(c) 2025 The Retro Hacker\n\n");

    if (argc < 2) {
        printf("Usage: loadmp3 <mp3file>\n");
        return 1;
    }

    // Open the PICO firmware binary file
    FILE *input_file = fopen(PICOFIRMWARE, "rb");
    if (!input_file) {
        perror("Failed to open PICO firmware binary file");
        return 1;
    }

    // Create the final output file
    FILE *output_file = fopen(COMBINED_FILE, "wb");
    if (!output_file) {
        perror("Failed to create final output file");
        fclose(input_file);
        return 1;
    }

    // Copy the PICO firmware binary file to the final output file
    uint8_t buffer[1024];
    size_t bytes_read;
    size_t total_bytes_written = 0;
    size_t current_size = 0; // Current size of the output file
    uint32_t base_offset = 0x1d; // Base offset for the ROM file = 29B (one config record)

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), input_file)) > 0) {
        fwrite(buffer, 1, bytes_read, output_file);
    }
    fclose(input_file);

    // Open the MP3 file
    FILE *rom_file = fopen(argv[1], "rb");
    if (!rom_file) {
        perror("Failed to open ROM file");
        fclose(output_file);
        return 1;
    }

    // Write the MP3 name to the configuration file
    if ((strstr(argv[1], ".MP3") != NULL) || (strstr(argv[1], ".mp3") != NULL)) // Check if it is a .ROM file
    {
        // Get the size of the MP3 file
        uint32_t rom_size = file_size(argv[1]);
        if (rom_size == 0 || rom_size > MAX_MP3_SIZE) {
            printf("Failed to get the size of the MP3 file or size not supported.\n");
            fclose(rom_file);
            fclose(output_file);
            return 1;
        }

        // Write the MP3 name to the configuration file
        char rom_name[MAX_FILE_NAME_LENGTH] = {0};
        uint32_t fl_offset = base_offset;

        // Extract the first part of the file name (up to the first '.MP3' or '.mp3')
        char *dot_position = strstr(argv[1], ".MP3");
        if (dot_position == NULL) {
            dot_position = strstr(argv[1], ".mp3");
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

        printf("MP3 Name: %s\n", rom_name);
        // Write the file name (20 bytes)
        fwrite(rom_name, 1, MAX_FILE_NAME_LENGTH, output_file);

        printf("MP3 Size: %u bytes\n", rom_size);
        // Write the file size (4 bytes)
        fwrite(&rom_size, 4, 1, output_file);

        printf("Pico Flash Offset: 0x%08X\n", fl_offset);
        // Write the flash offset (4 bytes)
        fwrite(&fl_offset, 4, 1, output_file);

        // Write the MP3 file to the final output file
        while ((bytes_read = fread(buffer, 1, sizeof(buffer), rom_file)) > 0) {
            fwrite(buffer, 1, bytes_read, output_file);
        }
        fclose(rom_file);
        fclose(output_file);

        //create the uf2 file
        create_uf2_file(COMBINED_FILE, UF2FILENAME);

    } else
    {
        perror("Invalid MP3 file");
        fclose(rom_file);
        fclose(output_file);
        return 1;
    }
}