#include <msx_fusion.h>
#include <stdio.h>
#include "bios.h"

// Define maximum files per page and screen properties
#define FILES_PER_PAGE 19   // Maximum files per page on the menu
#define MAX_FILE_NAME_LENGTH 50     // Maximum size of the ROM name
#define ROM_RECORD_SIZE (MAX_FILE_NAME_LENGTH + 1 + sizeof(unsigned long) + sizeof(unsigned long))  // Size of the ROM record in bytes
#define MAX_ROM_RECORDS 128 // Maximum ROM files supported
#define MEMORY_START 0x8000 // Start of the memory area to read the ROM records
#define ROM_SELECT_REGISTER 0x9D81 // Memory-mapped register that selects the ROM to load
#define JIFFY 0xFC9E

// Structure to represent a ROM record
// The ROM record will contain the name of the ROM, the mapper code, the size of the ROM and the offset in the flash memory
// Name: MAX_FILE_NAME_LENGTH bytes
// Mapper: 1 byte
// Size: 4 bytes
// Offset: 4 bytes
typedef struct {
    char Name[MAX_FILE_NAME_LENGTH + 1];
    unsigned char Mapper;
    unsigned long Size;
    unsigned long Offset;
} ROMRecord;


// Control Variables
int currentPage;    // Current page
int totalPages;     // Total pages
int currentIndex;   // Current file index
unsigned char totalFiles;     // Total files
unsigned long totalSize;
ROMRecord records[MAX_ROM_RECORDS]; // Array to store the ROM records

// Declare the functions
unsigned long read_ulong(const unsigned char *ptr);
int isEndOfData(const unsigned char *memory);
void readROMData(ROMRecord *records, unsigned char *recordCount, unsigned long *sizeTotal);
int putchar (int character);
void invert_chars(unsigned char startChar, unsigned char endChar);
void print_str_normal(const char *str);
void print_str_inverted(const char *str);
void print_str_inverted_padded(const char *str, unsigned char width);
char* mapper_description(int number);
void charMap(); //debug
void displayMenu();
void navigateMenu();
void configMenu();
void helpMenu();
void loadGame(int index);
void main();



