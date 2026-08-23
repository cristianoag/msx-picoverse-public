// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// menu.h - MSX Explorer menu definitions for PicoVerse 2350
// 
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <msx_fusion.h>
#include <stdio.h>
#include <stdint.h>
#include "bios.h"

#ifndef EXPLORER_VERSION
#define EXPLORER_VERSION "v1.00"
#endif

// Define maximum files per page and screen properties
#define FILES_PER_PAGE 19   // Maximum files per page on the menu
#define MAX_FILE_NAME_LENGTH 71     // Maximum size of the ROM name on the 80-column detail screen
#define ROM_RECORD_SIZE (MAX_FILE_NAME_LENGTH + 1 + sizeof(unsigned long) + sizeof(unsigned long))  // Size of the ROM record in bytes
#define MAX_ROM_RECORDS 65535 // Maximum ROM files supported (counter limit)
#define SD_ROM_MAX_SIZE_KB 4096 // 4 MB PSRAM region capacity for microSD-loaded ROMs
#define MEMORY_START 0xB900 // Start of the memory area to read the ROM records
#define ROM_SELECT_REGISTER 0xBF7F // Memory-mapped register that selects the ROM to load
#define JIFFY 0xFC9E
#define SOURCE_SD_FLAG 0x80
#define FOLDER_FLAG 0x40
#define MP3_FLAG 0x20
#define OVERRIDE_FLAG 0x10
#define AUDIO_TYPE_MASK 0x0F
#define AUDIO_TYPE_WAV 0x01
#define NAME_COL_WIDTH 22
#define NAME_COL_WIDTH_80 60
#define MENU_FORCE_40_COLUMNS 0
#define MENU_KEY_F4_CONFIG 0x04
#define CTRL_BASE_ADDR 0xBFF0
#define CTRL_COUNT_L (CTRL_BASE_ADDR + 0)
#define CTRL_COUNT_H (CTRL_BASE_ADDR + 1)
#define CTRL_PAGE    (CTRL_BASE_ADDR + 2)
#define CTRL_STATUS  (CTRL_BASE_ADDR + 3)
#define CTRL_CMD     (CTRL_BASE_ADDR + 4)
#define CTRL_MATCH_L (CTRL_BASE_ADDR + 5)
#define CTRL_MATCH_H (CTRL_BASE_ADDR + 6)
#define CTRL_MAPPER  (CTRL_BASE_ADDR + 7)
#define CTRL_ACK     (CTRL_BASE_ADDR + 8)
#define CTRL_AUDIO   (CTRL_BASE_ADDR + 9)
#define CTRL_WIFI_SUPPORT (CTRL_BASE_ADDR + 10)
#define CTRL_PSG_EMULATION (CTRL_BASE_ADDR + 11)
#define CTRL_WAVEGAME_ROM (CTRL_BASE_ADDR + 12)
#define CTRL_SD_PARTITION (CTRL_BASE_ADDR + 13)
#define CTRL_SD_BROWSE_PARTITION (CTRL_BASE_ADDR + 14)
#define CTRL_AUDIO_VOLUME (CTRL_BASE_ADDR + 15)
#define CTRL_MAGIC   0xA5
#define CTRL_STATUS_SD_MISSING 0x5D
#define CTRL_QUERY_BASE 0xBFC0
#define CTRL_QUERY_SIZE 32
#define AUDIO_PROFILE_NONE 0
#define AUDIO_PROFILE_SCC 1
#define AUDIO_PROFILE_SCC_PLUS 2
#define AUDIO_PROFILE_DUAL_PSG 3
#define AUDIO_PROFILE_MSX_MUSIC 4
#define AUDIO_PROFILE_SCC_EXTERNAL 5
#define AUDIO_PROFILE_SCC_PLUS_EXTERNAL 6
#define AUDIO_PROFILE_YM2151_SFG05 7
#define AUDIO_PROFILE_YM2151_SFG01 8
#define AUDIO_PROFILE_MEGARAM_SCC 9
#define AUDIO_PROFILE_MEGARAM_SCC_PLUS 10
#define AUDIO_PROFILE_YM2151_SFG05_4MHZ 11
#define AUDIO_PROFILE_YM2151_SFG01_4MHZ 12
#define AUDIO_VOLUME_DEFAULT 100
#define AUDIO_VOLUME_MAX 200
#define AUDIO_VOLUME_STEP 10
#define VDP_FREQ_DEFAULT 0
#define VDP_FREQ_60HZ 1
#define VDP_FREQ_50HZ 2
#define CPU_MODE_DEFAULT 0
#define CPU_MODE_TURBO 1
#define CPU_MODE_R800 2
// Per-ROM VDP frequency load read-back byte (Pico -> MSX). Sits in the free gap
// between the SD partition info buffer (0xBF80..0xBF9F) and the chip-id buffer
// (0xBFAF..0xBFBF). The save channel reuses query buffer byte 7.
#define CTRL_VDP_FREQ 0xBFA0
// ESP-01 link state maintained by the Pico (0 = offline, 1 = online). The Pico
// probes the module, so the menu only has to read this byte.
#define CTRL_NET_STATUS 0xBFA1
#define NET_STATUS_ONLINE 1
// Per-ROM CPU mode load read-back byte (Pico -> MSX), next free byte after
// CTRL_NET_STATUS. The save channel reuses query buffer byte 8.
#define CTRL_CPU_MODE 0xBFA2
#define MP3_CTRL_BASE      0xBFE0
#define MP3_CTRL_CMD       (MP3_CTRL_BASE + 0)
#define MP3_CTRL_STATUS    (MP3_CTRL_BASE + 1)
#define MP3_CTRL_INDEX_L   (MP3_CTRL_BASE + 2)
#define MP3_CTRL_INDEX_H   (MP3_CTRL_BASE + 3)
#define MP3_CTRL_ELAPSED_L (MP3_CTRL_BASE + 4)
#define MP3_CTRL_ELAPSED_H (MP3_CTRL_BASE + 5)
#define MP3_CTRL_TOTAL_L   (MP3_CTRL_BASE + 6)
#define MP3_CTRL_TOTAL_H   (MP3_CTRL_BASE + 7)
#define MP3_CTRL_MODE      (MP3_CTRL_BASE + 8)
#define MP3_STATUS_PLAYING 0x01
#define MP3_STATUS_ERROR   0x04
#define MP3_STATUS_EOF     0x08
#define MP3_STATUS_PAUSED  0x40
#define MP3_CMD_SELECT     0x01
#define MP3_CMD_PLAY       0x02
#define MP3_CMD_STOP       0x03
#define MP3_CMD_PAUSE      0x04
#define MP3_CMD_RESUME     0x05
#define MP3_CMD_NEXT       0x08
#define MP3_CMD_PREVIOUS   0x09
#define MP3_PLAY_MODE_SINGLE 0
#define MP3_PLAY_MODE_ALL    1
#define MP3_PLAY_MODE_RANDOM 2
#define DATA_BUFFER_END CTRL_FH_STATUS_TEXT_BASE
#define DATA_BUFFER_SIZE (DATA_BUFFER_END - MEMORY_START)
#define CMD_APPLY_FILTER 0x01
#define CMD_FIND_FIRST  0x02
#define CMD_ENTER_DIR   0x03
#define CMD_DETECT_MAPPER 0x04
#define CMD_SET_MAPPER 0x05
#define CMD_SET_SOURCE  0x06
#define CMD_LOAD_OPTIONS 0x07
#define CMD_SAVE_OPTIONS 0x08
#define CMD_PREPARE_QUICK_RUN 0x09
#define CMD_CYCLE_SD_PARTITION 0x0A
#define CMD_DELETE_SD_FILE 0x0B
#define CMD_LOAD_LAST_SELECTION 0x0C
#define CMD_SAVE_LAST_SELECTION 0x0D
#define CMD_FH_LIST_PAGE 0x40
#define CMD_FH_DOWNLOAD  0x41
#define CMD_FH_SEARCH    0x42
#define CMD_FH_WIFI_STATUS 0x43
#define CMD_FH_WIFI_CONFIG 0x44
#define CTRL_FH_PROGRESS_L CTRL_MAPPER
#define CTRL_FH_PROGRESS_H CTRL_ACK
#define CTRL_FH_RESULT CTRL_AUDIO
#define CTRL_FH_STATUS_TEXT_BASE 0xBF80
#define CTRL_FH_STATUS_TEXT_SIZE 64
#define CTRL_SD_PARTITION_INFO_BASE CTRL_FH_STATUS_TEXT_BASE
#define CTRL_SD_PARTITION_INFO_SIZE 32
#define CTRL_CHIP_ID_BASE (CTRL_FH_STATUS_TEXT_BASE + CTRL_FH_STATUS_TEXT_SIZE - 17)
#define CTRL_CHIP_ID_SIZE 17
#define CTRL_FH_FLAG_OFFSET MAX_FILE_NAME_LENGTH
#define CTRL_FH_SIZE_OFFSET (CTRL_FH_FLAG_OFFSET + 1)
#define CTRL_FH_RECORD_SIZE (MAX_FILE_NAME_LENGTH + 3)

#define SOURCE_MODE_ALL   0x00
#define SOURCE_MODE_FLASH 0x01
#define SOURCE_MODE_SD    0x02
#define SOURCE_MODE_FILEHUNTER 0x03

#define DATA_MAGIC_0 'P'
#define DATA_MAGIC_1 'V'
#define DATA_MAGIC_2 'E'
#define DATA_MAGIC_3 'X'
#define DATA_VERSION 1
#define DATA_HEADER_SIZE 24
#define DATA_HDR_MAGIC 0
#define DATA_HDR_VERSION 4
#define DATA_HDR_FLAGS 5
#define DATA_HDR_SIZE 6
#define DATA_HDR_TOTAL_COUNT 8
#define DATA_HDR_PAGE_INDEX 10
#define DATA_HDR_PAGE_COUNT 12
#define DATA_HDR_TABLE_OFFSET 14
#define DATA_HDR_TABLE_SIZE 16
#define DATA_HDR_STRING_OFFSET 18
#define DATA_HDR_STRING_SIZE 20
#define DATA_HDR_PAYLOAD_OFFSET 22
#define DATA_RECORD_TABLE_ENTRY_SIZE 4
#define TLV_NAME_OFFSET 0x01
#define TLV_MAPPER 0x02
#define TLV_SIZE 0x03

#define MENU_ROW_FORMAT_80 " %-61.61s %6s %-2s %-5s"
#define MENU_ROW_FORMAT_40 " %-21.21s %6s %-2s %-5s"

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
extern int currentPage;    // Current page
extern int totalPages;     // Total pages
extern int currentIndex;   // Current file index
extern unsigned int totalFiles;     // Total files
extern unsigned long totalSize;
extern ROMRecord records[FILES_PER_PAGE]; // Page buffer for ROM records

// Declare the functions
unsigned long read_ulong(const unsigned char *ptr);
int isEndOfData(const unsigned char *memory);
void readROMData(ROMRecord *records, unsigned int *recordCount, unsigned long *sizeTotal);
int putchar (int character);
void invert_chars(unsigned char startChar, unsigned char endChar);
char* mapper_description(int number);
unsigned char record_mapper_code(unsigned char mapper);
int record_mapper_is_override(unsigned char mapper);
int record_is_folder(const ROMRecord *record);
int record_is_mp3(const ROMRecord *record);
void trim_name_to_buffer(const char *src, char *dst, int max_len);
int build_sliding_name_window(const char *str, int startPos, char *out, unsigned char width);
void displayMenu();
void load_page_records(unsigned int page_index);
void navigateMenu();
void helpMenu();
void loadGame(int index);
void wait_ctrl_cmd(void);
unsigned char run_ctrl_cmd(unsigned char cmd);
void save_last_selection(unsigned int index);
void launch_wifi_config(void);
void execute_rst00();
void main();
void delay_ms(uint16_t milliseconds);



