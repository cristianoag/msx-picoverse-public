// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// menu.c - MSX ROM with the menu program for the MSX PICOVERSE 2350 project
// 
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include <stdint.h>
#include "menu.h"
#include "menu_state.h"
#include "menu_ui.h"
#include "menu_input.h"
#include "screen_rom.h"
#include "explorer_fh.h"

#define SEARCH_MAX_LEN 24

// Control Variables (definitions)
int currentPage;
int totalPages;
int currentIndex;
unsigned int totalFiles;
unsigned long totalSize;
ROMRecord records[FILES_PER_PAGE];
static unsigned char menu_message_row;
static unsigned char restore_pending; // waiting for the Pico to locate the last executed entry

// --- Forward declarations (UI + Pico protocol) ---
static int read_search_query(char *buffer, int max_len);
static void send_query_to_pico(const char *query);
static unsigned int read_match_index(void);
static unsigned int bios_calatr(void);
int record_is_folder(const ROMRecord *record);
int record_is_mp3(const ROMRecord *record);
void trim_name_to_buffer(const char *src, char *dst, int max_len);
static void build_menu_row_text(const ROMRecord *record, const char *name_override, char *out, unsigned char width);
static void enter_directory(int index);
static void refresh_menu_state(const char *loading_text);
static void wait_key_with_blinking_status(const char *text);
static void wait_command_with_blinking_status(const char *text, unsigned int wait_limit);
static void draw_menu_row(unsigned char row, ROMRecord *record, int selected);
static void redefine_function_keys(void);
static void switch_browse_source(unsigned char source_mode, const char *loading_text);
static void switch_menu_source(unsigned char source_mode);
static void print_chip_id_line(void);
void msx_wait(uint16_t times_jiffy);
void delay_ms(uint16_t milliseconds);
static void disk_led_poll(void);

// --- ROM record helpers ---
// read_ulong - Read a 4-byte value from the memory area
// This function will read a 4-byte value from the memory area pointed by ptr and return the value as an unsigned long
// Parameters:
//   ptr - Pointer to the memory area to read the value from
// Returns:
//   The 4-byte value as an unsigned long 
unsigned long read_ulong(const unsigned char *ptr) {
    return (unsigned long)ptr[0] |
           ((unsigned long)ptr[1] << 8) |
           ((unsigned long)ptr[2] << 16) |
           ((unsigned long)ptr[3] << 24);
}

// isEndOfData - Check if the memory area is the end of the data
// This function will check if the memory area pointed by memory is the end of the data. The end of the data is defined by all bytes being 0xFF.
// Parameters:
//   memory - Pointer to the memory area to check
// Returns:
//   1 if the memory area is the end of the data, 0 otherwise
int isEndOfData(const unsigned char *memory) {
    for (int i = 0; i < ROM_RECORD_SIZE; i++) {
        if (memory[i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}

static unsigned int read_u16_le(const unsigned char *ptr) {
    return (unsigned int)ptr[0] | ((unsigned int)ptr[1] << 8);
}

static int buffer_has_magic(const unsigned char *memory) {
    return memory[DATA_HDR_MAGIC] == (unsigned char)DATA_MAGIC_0 &&
           memory[DATA_HDR_MAGIC + 1] == (unsigned char)DATA_MAGIC_1 &&
           memory[DATA_HDR_MAGIC + 2] == (unsigned char)DATA_MAGIC_2 &&
           memory[DATA_HDR_MAGIC + 3] == (unsigned char)DATA_MAGIC_3;
}

static void clear_record(ROMRecord *record) {
    record->Name[0] = '\0';
    record->Mapper = 0;
    record->Size = 0;
    record->Offset = 0;
}

static void read_string_from_pool(char *dst, const unsigned char *base,
                                  unsigned int pool_offset, unsigned int pool_size,
                                  unsigned int string_offset) {
    dst[0] = '\0';
    if (string_offset >= pool_size) {
        return;
    }
    const unsigned char *src = base + pool_offset + string_offset;
    unsigned int max_len = MAX_FILE_NAME_LENGTH;
    unsigned int remaining = pool_size - string_offset;
    if (remaining < max_len) {
        max_len = remaining;
    }
    unsigned int i = 0;
    while (i < max_len) {
        unsigned char ch = src[i];
        if (ch == '\0') {
            break;
        }
        dst[i] = (char)ch;
        i++;
    }
    dst[i] = '\0';
}

// --- Pico control protocol (menu <-> firmware) ---

// read_total_count - Get total ROM count from Pico control registers.
static unsigned int read_total_count() {
    return (unsigned int)(*((unsigned char *)CTRL_COUNT_L)) |
           ((unsigned int)(*((unsigned char *)CTRL_COUNT_H)) << 8);
}

// bios_calatr - Return VRAM attribute/color table address for current cursor
static unsigned int bios_calatr(void) __naked
{
    __asm
    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_CALATR
    call    BIOS_CALSLT
    pop     ix
    ret
    __endasm;

}

// send_query_to_pico - Write the search query buffer into Pico memory.
static void send_query_to_pico(const char *query) {
    unsigned int i = 0;
    unsigned int len = 0;
    if (query) {
        len = strlen(query);
    }
    if (len >= CTRL_QUERY_SIZE) {
        len = CTRL_QUERY_SIZE - 1;
    }
    for (i = 0; i < CTRL_QUERY_SIZE; i++) {
        unsigned char value = 0;
        if (i < len) {
            value = (unsigned char)query[i];
        }
        Poke(CTRL_QUERY_BASE + i, value);
    }
}

// read_match_index - Read the Pico-calculated match index.
static unsigned int read_match_index(void) {
    return (unsigned int)(*((unsigned char *)CTRL_MATCH_L)) |
           ((unsigned int)(*((unsigned char *)CTRL_MATCH_H)) << 8);
}

// record_is_folder - Check if a ROM record represents a folder.
int record_is_folder(const ROMRecord *record) {
    return (record->Mapper & FOLDER_FLAG) != 0;
}

int record_is_mp3(const ROMRecord *record) {
    return (record->Mapper & MP3_FLAG) != 0;
}

unsigned char record_mapper_code(unsigned char mapper) {
    return mapper & ~(SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG);
}

int record_mapper_is_override(unsigned char mapper) {
    (void)mapper;
    return 0;
}

static void build_menu_row_text(const ROMRecord *record, const char *name_override, char *out, unsigned char width) {
    const char *source = (record->Mapper & SOURCE_SD_FLAG) ? "SD" : "FL";
    const char *type_label = " ROM";
    char size_text[8] = "";
    const char *name = name_override ? name_override : record->Name;
    unsigned long size_kb = record->Size / 1024u;

    if (record_is_folder(record)) {
        type_label = "<DIR>";
        source = "SD";
    } else if (record_is_mp3(record) && ((record->Mapper & AUDIO_TYPE_MASK) == AUDIO_TYPE_WAV)) {
        type_label = " WAV";
        source = "SD";
    } else if (record_is_mp3(record)) {
        type_label = " MP3";
        source = "SD";
    }

    if (!record_is_folder(record)) {
        if (size_kb <= 1024u) {
            sprintf(size_text, "%luKB", size_kb);
        } else {
            unsigned long size_mb = (size_kb + 1023u) / 1024u;
            sprintf(size_text, "%luMB", size_mb);
        }
    }

    if (use_80_columns) {
        sprintf(out, MENU_ROW_FORMAT_80, name, size_text, source, type_label);
    } else {
        sprintf(out, MENU_ROW_FORMAT_40, name, size_text, source, type_label);
    }

    int len = (int)strlen(out);
    for (int i = len; i < (int)width; i++) {
        out[i] = ' ';
    }
    out[width] = '\0';
}

int build_sliding_name_window(const char *str, int startPos, char *out, unsigned char width)
{
    size_t len = strlen(str);

    if (width == 0) {
        out[0] = '\0';
        return 0;
    }

    if (len == 0) {
        for (size_t i = 0; i < width; i++) {
            out[i] = ' ';
        }
        out[width] = '\0';
        return 0;
    }

    if (len < width) {
        memcpy(out, str, len);
        for (size_t i = len; i < width; i++) {
            out[i] = ' ';
        }
        out[width] = '\0';
        return 1;
    }

    int cycle = (int)len + 1;  /* virtual space separator between repetitions */
    int base = startPos % cycle;
    if (base < 0) {
        base += cycle;
    }

    int hasVisible = 0;
    for (size_t i = 0; i < width; i++) {
        int idx = (base + (int)i) % cycle;
        unsigned char ch = (idx < (int)len) ? (unsigned char)str[idx] : ' ';
        out[i] = (char)ch;
        if (ch != ' ') {
            hasVisible = 1;
        }
    }
    out[width] = '\0';
    return hasVisible;
}

// trim_name_to_buffer - Trim trailing spaces from a name and copy to buffer.
void trim_name_to_buffer(const char *src, char *dst, int max_len) {
    int len = 0;
    while (len < max_len && src[len] != '\0') {
        dst[len] = src[len];
        len++;
    }
    while (len > 0 && dst[len - 1] == ' ') {
        len--;
    }
    dst[len] = '\0';
}

// enter_directory - Send command to Pico to enter a directory.
static void enter_directory(int index) {
    const char *loading_text = menu_ui_status_text("Loading...", "Loading folder...");
    Poke(MP3_CTRL_CMD, MP3_CMD_STOP);
    menu_ui_print_last_line_text(loading_text);
    unsigned char blink_state = 1;
    unsigned char blink_tick = 0;
    for (unsigned int wait = 0; wait < 100; wait++) {
        if (Peek(CTRL_CMD) == 0) {
            break;
        }
        delay_ms(10);
        menu_ui_blink_last_line(loading_text, &blink_state, &blink_tick, 8);
    }
    send_query_to_pico("");
    if (index >= 0) {
        Poke(CTRL_QUERY_BASE + 0, (unsigned char)(index & 0xFF));
        Poke(CTRL_QUERY_BASE + 1, (unsigned char)((index >> 8) & 0xFF));
        Poke(CTRL_QUERY_BASE + 2, CTRL_MAGIC);
    }
    Poke(CTRL_CMD, CMD_ENTER_DIR);
    for (unsigned int wait = 0; wait < 500; wait++) {
        if (Peek(CTRL_CMD) == 0) {
            break;
        }
        delay_ms(10);
        menu_ui_blink_last_line(loading_text, &blink_state, &blink_tick, 8);
    }
}

// refresh_menu_state - Refresh the menu state by reading total files and pages from Pico.
static void refresh_menu_state(const char *loading_text) {
    const char *message = loading_text ? loading_text : menu_ui_status_text("Loading...", "Loading menu...");
    unsigned int count = 0xFFFFu;
    unsigned int last = 0xFFFFu;
    unsigned int stable = 0;
    unsigned int limit = restore_pending ? 500u : 100u;
    unsigned char blink_state = 1;
    unsigned char blink_tick = 0;
    menu_ui_print_last_line_text(message);
    for (unsigned int wait = 0; wait < limit; wait++) {
        // While restoring, the Pico raises CTRL_ACK once the list is rebuilt and
        // the saved entry has been located: an exact signal, so there is no need
        // to guess completion from a settling record count.
        if (restore_pending) {
            if (Peek(CTRL_ACK) == CTRL_MAGIC) {
                break;
            }
        } else {
            count = read_total_count();
            if (count != 0xFFFFu && count == last) {
                stable++;
                if (stable >= 2) {
                    break;
                }
            } else {
                stable = 0;
            }
            last = count;
        }
        delay_ms(10);
        menu_ui_blink_last_line(message, &blink_state, &blink_tick, 8);
    }
    readROMData(records, &totalFiles, &totalSize);
    totalPages = (int)((totalFiles + FILES_PER_PAGE - 1) / FILES_PER_PAGE);
    if (totalPages == 0) {
        totalPages = 1;
    }
    currentPage = 1;
    currentIndex = 0;
    if (restore_pending) {
        restore_pending = 0;
        unsigned int match = read_match_index();
        if (match < totalFiles) {
            currentIndex = (int)match;
            currentPage = (currentIndex / FILES_PER_PAGE) + 1;
        }
    }
    menu_ui_clear_last_line();
    displayMenu();
    if (menu_shortcut_selection == MENU_SHORTCUT_MICROSD && Peek(CTRL_STATUS) == CTRL_STATUS_SD_MISSING) {
        menu_ui_print_last_line_text(menu_ui_status_text("No microSD card", "No microSD card inserted."));
    }
}

static void wait_key_with_blinking_status(const char *text) {
    unsigned char blink_state = 1;
    unsigned char blink_tick = 0;

    menu_ui_print_last_line_text(text);
    while (!bios_chsns()) {
        delay_ms(20);
        menu_ui_blink_last_line(text, &blink_state, &blink_tick, 8);
    }
    (void)bios_chget();
}

static void wait_command_with_blinking_status(const char *text, unsigned int wait_limit) {
    unsigned char blink_state = 1;
    unsigned char blink_tick = 0;

    menu_ui_print_last_line_text(text);
    for (unsigned int wait = 0; wait < wait_limit; wait++) {
        if (Peek(CTRL_CMD) == 0) {
            break;
        }
        delay_ms(10);
        menu_ui_blink_last_line(text, &blink_state, &blink_tick, 8);
    }
}

// load_page_records - Request a page from Pico and load it into the local records array.
static int parse_page_buffer(unsigned int page_index) {
    unsigned char *base = (unsigned char *)MEMORY_START;
    if (!buffer_has_magic(base)) {
        return 0;
    }

    unsigned int header_size = read_u16_le(base + DATA_HDR_SIZE);
    if (header_size < DATA_HEADER_SIZE) {
        return 0;
    }

    unsigned int page_count = read_u16_le(base + DATA_HDR_PAGE_COUNT);
    unsigned int table_offset = read_u16_le(base + DATA_HDR_TABLE_OFFSET);
    unsigned int table_size = read_u16_le(base + DATA_HDR_TABLE_SIZE);
    unsigned int string_offset = read_u16_le(base + DATA_HDR_STRING_OFFSET);
    unsigned int string_size = read_u16_le(base + DATA_HDR_STRING_SIZE);

    if (table_offset + table_size > DATA_BUFFER_SIZE) {
        return 0;
    }
    if (string_offset + string_size > DATA_BUFFER_SIZE) {
        return 0;
    }

    if (page_count > FILES_PER_PAGE) {
        page_count = FILES_PER_PAGE;
    }

    for (unsigned int i = 0; i < FILES_PER_PAGE; i++) {
        clear_record(&records[i]);
    }

    for (unsigned int i = 0; i < page_count; i++) {
        unsigned int entry_offset = table_offset + (i * DATA_RECORD_TABLE_ENTRY_SIZE);
        unsigned int record_offset = read_u16_le(base + entry_offset);
        unsigned int record_length = read_u16_le(base + entry_offset + 2);

        if (record_offset + record_length > DATA_BUFFER_SIZE) {
            continue;
        }

        unsigned char mapper = 0;
        unsigned long size = 0;
        char name[MAX_FILE_NAME_LENGTH + 1];
        name[0] = '\0';

        unsigned int pos = 0;
        unsigned char *rec = base + record_offset;
        while (pos + 2 <= record_length) {
            unsigned char type = rec[pos];
            unsigned char len = rec[pos + 1];
            pos += 2;
            if (pos + len > record_length) {
                break;
            }
            switch (type) {
                case TLV_NAME_OFFSET: {
                    if (len >= 2) {
                        unsigned int name_offset = read_u16_le(rec + pos);
                        if (name_offset != 0xFFFFu) {
                            read_string_from_pool(name, base, string_offset, string_size, name_offset);
                        }
                    }
                    break;
                }
                case TLV_MAPPER:
                    if (len >= 1) {
                        mapper = rec[pos];
                    }
                    break;
                case TLV_SIZE:
                    if (len >= 4) {
                        size = read_ulong(rec + pos);
                    }
                    break;
                default:
                    break;
            }
            pos += len;
        }

        strncpy(records[i].Name, name, MAX_FILE_NAME_LENGTH);
        records[i].Name[MAX_FILE_NAME_LENGTH] = '\0';
        records[i].Mapper = mapper;
        records[i].Size = size;
        records[i].Offset = 0;
    }

    (void)page_index;
    return 1;
}

void load_page_records(unsigned int page_index) {
    unsigned char *memory = (unsigned char *)MEMORY_START;
    unsigned int i;

    Poke(CTRL_PAGE, (unsigned char)page_index);
    for (unsigned int wait = 0; wait < 1000; wait++) {
        if (*((unsigned char *)CTRL_PAGE) == (unsigned char)page_index) {
            break;
        }
    }

    if (parse_page_buffer(page_index)) {
        return;
    }

    for (i = 0; i < FILES_PER_PAGE; i++) {
        unsigned int recordIndex = (page_index * FILES_PER_PAGE) + i;
        if (recordIndex >= totalFiles || isEndOfData(memory)) {
            break;
        }
        MemCopy(records[i].Name, memory, MAX_FILE_NAME_LENGTH);
        for (int j = strlen(records[i].Name); j < MAX_FILE_NAME_LENGTH; j++) {
            records[i].Name[j] = ' ';
        }
        records[i].Name[MAX_FILE_NAME_LENGTH] = '\0';
        records[i].Mapper = memory[MAX_FILE_NAME_LENGTH];
        records[i].Size = read_ulong(&memory[MAX_FILE_NAME_LENGTH + 1]);
        records[i].Offset = read_ulong(&memory[MAX_FILE_NAME_LENGTH + 5]);
        memory += ROM_RECORD_SIZE;
    }

    for (; i < FILES_PER_PAGE; i++) {
        clear_record(&records[i]);
    }
}

// --- ROM list loading ---

// readROMData - Read the ROM records from the memory area or Pico paging buffer.
// This function will read the ROM records from the memory area pointed by memory and store them in the records array. The function will stop reading
// when it reaches the end of the data or the maximum number of records is reached.
// Parameters:
//   records - Pointer to the array of ROM records to store the data
//   recordCount - Pointer to the variable to store the number of records read
void readROMData(ROMRecord *records, unsigned int *recordCount, unsigned long *sizeTotal) {
    unsigned char *memory = (unsigned char *)MEMORY_START;
    unsigned int count;
    unsigned long total;

    unsigned char ctrl_status = *((unsigned char *)CTRL_STATUS);
    menu_message_row = 0;
    if (ctrl_status == CTRL_STATUS_SD_MISSING) {
        strncpy(records[0].Name, menu_ui_status_text("No microSD card", "No microSD card inserted."), MAX_FILE_NAME_LENGTH);
        records[0].Name[MAX_FILE_NAME_LENGTH] = '\0';
        records[0].Mapper = SOURCE_SD_FLAG;
        records[0].Size = 0;
        records[0].Offset = 0;
        paging_enabled = 0;
        *recordCount = 1;
        *sizeTotal = 0;
        menu_message_row = 1;
        return;
    }
    count = read_total_count();
    paging_enabled = (ctrl_status == CTRL_MAGIC);
    if (!paging_enabled) {
        if (count > FILES_PER_PAGE && count != 0xFFFFu) {
            paging_enabled = 1;
        }
    }

    if (paging_enabled) {
        *recordCount = count;
        *sizeTotal = 0;
        load_page_records(0);
        return;
    }

    count = 0;
    total = 0;
    while (count < MAX_ROM_RECORDS && !isEndOfData(memory)) {
        if (count < FILES_PER_PAGE) {
            // Copy Name (page buffer only)
            MemCopy(records[count].Name, memory, MAX_FILE_NAME_LENGTH);
            for (int i = strlen(records[count].Name); i < MAX_FILE_NAME_LENGTH; i++) {
                records[count].Name[i] = ' ';
            }
            records[count].Name[MAX_FILE_NAME_LENGTH] = '\0';
            records[count].Mapper = memory[MAX_FILE_NAME_LENGTH];
            records[count].Size = read_ulong(&memory[MAX_FILE_NAME_LENGTH + 1]);
            records[count].Offset = read_ulong(&memory[MAX_FILE_NAME_LENGTH + 5]);
        }
        total += read_ulong(&memory[MAX_FILE_NAME_LENGTH + 1]);
        memory += ROM_RECORD_SIZE;
        count++;
    }

    *recordCount = count;
    *sizeTotal = total;
}

// --- BIOS wrappers and timing ---

// putchar - Print a character on the screen
// This function will override the putchar function to use the MSX BIOS routine to print characters on the screen
// This is to deal with the mess we have between the Fusion-C putchar and the SDCC Z80 library putchar
// Parameters:
//  character - The character to print
// Returns:
//  The character printed
int putchar (int character)
{
    __asm
    ld      hl, #2              ;Get the return address of the function
    add     hl, sp              ;Bypass the return address of the function 
    ld      a, (hl)              ;Get the character to print

    ld      iy,(#BIOS_EXPTBL-1)  ;BIOS slot in iyh
    push    ix                     ;save ix
    ld      ix,#BIOS_CHPUT       ;address of BIOS routine
    call    BIOS_CALSLT          ;interslot call
    pop ix                      ;restore ix
    __endasm;

    return character;
}

// execute_rst00 - Execute the RST 00h instruction to reset the MSX computer
void execute_rst00() {
    __asm
        rst 0x00
    __endasm;
}

// clear_fkeys - Clear the function key strings in the BIOS area
void clear_fkeys()
{
    __asm
    ld hl, #BIOS_FNKSTR    ; Load the starting address of function key strings into HL
    ld de, #0xF880    ; Load the next address into DE for block fill
    ld bc, #160       ; Set BC to 160, the number of bytes to clear
    ld (hl), #0       ; Initialize the first byte to 0

clear_loop:
    ldi              ; Load (HL) with (DE), increment HL and DE, decrement BC
    dec hl           ; Adjust HL back to the correct position
    ld (hl), #0      ; Set the current byte to 0
    inc hl           ; Move to the next byte
    dec bc           ; Decrement the byte counter
    ld a, b          ; Check if BC has reached zero
    or c
    jp nz, clear_loop ; Repeat until all bytes are cleared
    __endasm;
}

static void redefine_function_keys(void) {
    char *fnkstr = (char *)BIOS_FNKSTR;
    clear_fkeys();
    fnkstr[0] = '1';
    fnkstr[16] = '2';
    fnkstr[32] = '3';
    fnkstr[48] = MENU_KEY_F4_CONFIG;
}

static void switch_browse_source(unsigned char source_mode, const char *loading_text) {
    unsigned char payload[2];

    Poke(MP3_CTRL_CMD, MP3_CMD_STOP);
    menu_shortcut_selection = (source_mode == SOURCE_MODE_SD) ? MENU_SHORTCUT_MICROSD : MENU_SHORTCUT_FLASH;
    menu_ui_update_footer_page();
    payload[0] = source_mode;
    payload[1] = 0;
    send_query_to_pico((const char *)payload);
    Poke(CTRL_CMD, CMD_SET_SOURCE);
    refresh_menu_state(loading_text);
}

static void switch_menu_source(unsigned char source_mode) {
    switch_browse_source(
        source_mode,
        (source_mode == SOURCE_MODE_SD)
            ? menu_ui_status_text("Load SD", "Loading from microSD...")
            : menu_ui_status_text("Load Flash", "Loading from flash memory..."));
}

// wait_ctrl_cmd - Wait until the Pico reports the pending command as finished.
void wait_ctrl_cmd(void) {
    for (unsigned int wait = 0; wait < 500; wait++) {
        if (Peek(CTRL_CMD) == 0) {
            break;
        }
        delay_ms(10);
    }
}

// run_ctrl_cmd - Send a command byte to the Pico, wait for it to complete and
// return the acknowledge byte.
unsigned char run_ctrl_cmd(unsigned char cmd) {
    Poke(CTRL_CMD, cmd);
    wait_ctrl_cmd();
    return Peek(CTRL_ACK);
}

// save_last_selection - Ask the Pico to store the entry being executed on the
// microSD card so the next boot reopens the menu on it.
void save_last_selection(unsigned int index) {
    Poke(CTRL_QUERY_BASE + 0, (unsigned char)(index & 0xFF));
    Poke(CTRL_QUERY_BASE + 1, (unsigned char)((index >> 8) & 0xFF));
    (void)run_ctrl_cmd(CMD_SAVE_LAST_SELECTION);
}

// restore_last_selection - Open the menu on the source, folder and entry that
// was executed last, falling back to the flash list when there is none. The
// list is built, located and drawn in a single refresh pass.
static void restore_last_selection(void) {
    unsigned char source = run_ctrl_cmd(CMD_LOAD_LAST_SELECTION) ? Peek(CTRL_MAPPER) : 0;

    restore_pending = (source == SOURCE_MODE_SD || source == SOURCE_MODE_FLASH);
    switch_menu_source(restore_pending ? source : SOURCE_MODE_FLASH);
}

static void cycle_sd_partition(void) {
    if (menu_shortcut_selection == MENU_SHORTCUT_MICROSD) {
        Poke(CTRL_CMD, CMD_CYCLE_SD_PARTITION);
        refresh_menu_state(menu_ui_status_text("Load SD", "Loading from microSD..."));
    }
}

static void print_chip_id_line(void) {
    char chip_id[CTRL_CHIP_ID_SIZE];
    unsigned char i;

    for (i = 0; i < CTRL_CHIP_ID_SIZE - 1; i++) {
        chip_id[i] = *((char *)(CTRL_CHIP_ID_BASE + i));
    }
    chip_id[CTRL_CHIP_ID_SIZE - 1] = '\0';
    printf("Chip ID: %s", chip_id);
}

void launch_wifi_config(void) {
    const char *message = menu_ui_status_text("Wi-Fi setup...", "Opening Wi-Fi setup...");
    unsigned char blink_state = 1;
    unsigned char blink_tick = 0;

    menu_ui_print_last_line_text(message);
    Poke(CTRL_CMD, CMD_FH_WIFI_CONFIG);
    while (Peek(CTRL_CMD) != 0) {
        delay_ms(20);
        menu_ui_blink_last_line(message, &blink_state, &blink_tick, 8);
    }
    execute_rst00();
    execute_rst00();
}


// wait_for_key_with_scroll - Wait for a key press with scrolling effect
// This function will wait for a key press and return the key code. While waiting, it
static int wait_for_key_with_scroll(void)
{
    volatile unsigned int *jiffyPtr = (volatile unsigned int *)JIFFY;
    unsigned int lastTick = *jiffyPtr;
    int startPos = 0;
    const unsigned int scrollDelay = 30U; // 0.5 seconds at 60 Hz
    char row_text[81];
    char name_window[MAX_FILE_NAME_LENGTH + 1];

    while (1)
    {
        
        // check for key press
        unsigned char peek = bios_chsns();
        if (peek) {
            return (int)bios_chget();
        }

        // mirror background microSD traffic (MP3 streaming, list refresh)
        disk_led_poll();

        // handle scrolling
        if ((unsigned int)(*jiffyPtr - lastTick) >= scrollDelay) {
            if (!use_80_columns && menu_shortcut_selection == MENU_SHORTCUT_MICROSD) {
                menu_ui_clear_last_line();
            }
            if (totalFiles > 0 && !menu_message_row) {
            ROMRecord *record = &records[currentIndex % FILES_PER_PAGE];
            size_t len = strlen(record->Name);
            if (!record_is_folder(record) && (use_80_columns ? (len >= name_col_width) : (len > name_col_width))) {
                int attempts = 0;
                int printed = 0;
                int lenInt = (int)len;

                while (attempts < lenInt && !printed) {
                    printed = build_sliding_name_window(record->Name, startPos, name_window, name_col_width);
                    if (printed) {
                        build_menu_row_text(record, name_window, row_text, menu_ui_row_width());
                        menu_ui_render_selectable_line((unsigned char)((currentIndex % FILES_PER_PAGE) + 2), row_text + 1, 1);
                    }
                    startPos++;
                    if (startPos >= lenInt + 1) {
                        startPos = 0;
                    }
                    attempts++;
                }
            }
            }
            lastTick = *jiffyPtr;
        }


    }
    
}

// mapper_description - Get the description of the mapper type
// This function will return the description of the mapper type based on the mapper number.
char* mapper_description(int number) {
    // Array of strings for the descriptions
    const char *descriptions[] = {"PLA-16", "PLA-32", "KonSCC", "PLN-48", "ASC-08", "ASC-16", "Konami", "NEO-8", "NEO-16", "SYSTEM", "SYSTEM", "ASC16X", "PLN-64", "MANBW2"};
    number = record_mapper_code((unsigned char)number);
    if (number >= 15 && number <= 21) {
        return "SYSTEM";
    }
    if (number <= 0 || number > 14) {
        return "Unknown";
    }
    return descriptions[number - 1];
}

// --- Menu rendering ---

// displayMenu - Display the menu on the screen
// This function will display the menu on the screen. It will print the header, the files on the current page and the footer with the page number and options.
static void draw_menu_row(unsigned char row, ROMRecord *record, int selected) {
    char row_text[81];
    build_menu_row_text(record, NULL, row_text, menu_ui_row_width());
    menu_ui_render_selectable_line(row, row_text + 1, selected);
}

void displayMenu() {
    if (!frame_rendered) {
        menu_ui_render_menu_frame();
    }
    if (paging_enabled && currentPage > 0) {
        load_page_records((unsigned int)(currentPage - 1));
    }

    unsigned int startIndex = (currentPage - 1) * FILES_PER_PAGE;
    unsigned int endIndex = startIndex + FILES_PER_PAGE;

    if (endIndex > totalFiles) {
        endIndex = totalFiles;
    }

    unsigned int line = 0;
    for (unsigned int idx = startIndex; idx < endIndex; idx++, line++) {
        draw_menu_row((unsigned char)(2 + line), &records[line], !menu_message_row && idx == (unsigned int)currentIndex);
    }

    {
        unsigned char start_row = (unsigned char)(2 + line);
        menu_ui_clear_rows(start_row, 21);
    }

    menu_ui_update_footer_page();
}

// --- Menu screens ---

// helpMenu - Display the help menu on the screen
// This function will display the help menu on the screen. It will print the help information and the keys to navigate the menu.
void helpMenu()
{
    Cls();
    Locate(0,0);
    menu_ui_print_title_line();
    Locate(0, 1);
    menu_ui_print_delimiter_line();
    Locate(0, 2);
    menu_ui_print_str_inverted_width("Navigation", 10);
    Locate(0, 3);
    printf("UP/DOWN - Move selection");
    Locate(0, 4);
    printf("LEFT/RIGHT - Change page or option");
    Locate(0, 5);
    printf("ENTER - Detail  /  SPACE - Run");
    Locate(0, 6);
    printf("ESC - Back or leave folder");
    Locate(0, 7);
    menu_ui_print_str_inverted_width("Sources", 7);
    Locate(0, 8);
    printf("F1 - Flash  F2 - SD  F3 - FileHunter");
    Locate(0, 9);
    printf("F4 - Wi-Fi setup");
    Locate(0, 10);
    menu_ui_print_str_inverted_width("List Commands", 13);
    Locate(0, 11);
    printf("/ - Search / P - Cycle SD partition");
    Locate(0, 12);
    printf("H - Show this help / D - Delete file");
    Locate(0, 13);
    printf("C - Toggle 40/80 columns");
    Locate(0, 14);
    menu_ui_print_str_inverted_width("ROM Detail", 10);
    Locate(0, 15);
    printf("UP/DOWN Select Mapper/Audio/PSG/Action");
    Locate(0, 16);
    printf("LEFT/RIGHT Change mapper/audio/PSG");
    Locate(0, 17);
    printf("ENTER/SPACE Configure/Run selected ROM");
    Locate(0, 18);
    menu_ui_print_str_inverted_width("File Hunter", 11);
    Locate(0, 19);
    printf("ENTER/SPACE Open detail/Download ROM");
    Locate(0, 20);
    printf("ESC         Return to Explorer menu");
    Locate(0, 21);
    menu_ui_print_delimiter_line();
    Locate(0, 22);
    print_chip_id_line();
    Locate(0, 23);
    printf("Press any key to return.");
    (void)bios_chget();
    frame_rendered = 0;
}


// loadGame - Load the game from the flash memory
// This function will load the game from the flash memory based on the index. 
void loadGame(int index) 
{
    ROMRecord *record = &records[index % FILES_PER_PAGE];
    if (record_is_folder(record) || record_is_mp3(record))
    {
        return;
    }
    Poke(MP3_CTRL_CMD, MP3_CMD_STOP);
    if ((record->Mapper & ~SOURCE_SD_FLAG) != 0)
    {
        save_last_selection((unsigned int)index); // Remember it for the next boot
        Poke(ROM_SELECT_REGISTER, index); // Set the game index (absolute)
        execute_rst00(); // Execute RST 00h to reset the MSX computer and load the game
        execute_rst00();
    }
}

// --- Input/navigation ---

// navigateMenu - Navigate the menu
// This function will navigate the menu. It will wait for the user to press a key and then act based on the key pressed. The user can navigate the menu using the arrow keys
// to move up and down the files, left and right to move between pages, enter to load the game, H to display the help screen and C to display the config screen.
// The function will update the current page and current index based on the key pressed and display the menu again.
void navigateMenu() 
{
    char key;
    char search_query[SEARCH_MAX_LEN + 1];

    while (1) 
    {
        //debug
        Locate(0, 23);
        //printf("Key: %3d", key);
        //printf("Size: %05lu/15872", totalSize/1024);
        if (totalFiles == 0) {
            key = (char)wait_for_key_with_scroll();
            if (key == '1') {
                switch_menu_source(SOURCE_MODE_FLASH);
            }
            if (key == '2') {
                switch_menu_source(SOURCE_MODE_SD);
            }
            if (key == '3') {
                switch_menu_source(explorer_fh_run() == SOURCE_MODE_SD ? SOURCE_MODE_SD : SOURCE_MODE_FLASH);
            }
            if (key == MENU_KEY_F4_CONFIG) {
                launch_wifi_config();
            }
            if (key == 'p' || key == 'P') {
                cycle_sd_partition();
            }
            if (key == 'h' || key == 'H') {
                helpMenu();
                displayMenu();
            }
            if (key == 27) {
                enter_directory(-1);
                refresh_menu_state(0);
            }
            continue;
        }

        unsigned int currentRow = (currentIndex%FILES_PER_PAGE) + 2;

        key = wait_for_key_with_scroll();
        //key = KeyboardRead();
        //key = InputChar();

        draw_menu_row((unsigned char)currentRow, &records[currentIndex % FILES_PER_PAGE], 0);
        switch (key) 
        {
            case '1':
                switch_menu_source(SOURCE_MODE_FLASH);
                break;
            case '2':
                switch_menu_source(SOURCE_MODE_SD);
                break;
            case '3':
            {
                unsigned char return_shortcut = menu_shortcut_selection;
                unsigned char next_source = explorer_fh_run();
                if (next_source == SOURCE_MODE_SD) {
                    switch_menu_source(SOURCE_MODE_SD);
                } else if (next_source == SOURCE_MODE_FLASH) {
                    switch_menu_source(SOURCE_MODE_FLASH);
                } else if (return_shortcut == MENU_SHORTCUT_MICROSD) {
                    switch_menu_source(SOURCE_MODE_SD);
                } else {
                    switch_menu_source(SOURCE_MODE_FLASH);
                }
            }
                break;
            case MENU_KEY_F4_CONFIG:
                launch_wifi_config();
                break;
            case 30: // Up arrow
                if (currentIndex > 0) // Check if we are not at the first file
                {
                    if (currentIndex%FILES_PER_PAGE >= 0) currentIndex--; // Move to the previous file
                    if (currentIndex < ((currentPage-1) * FILES_PER_PAGE))  // Check if we need to move to the previous page
                    {
                        currentPage--; // Move to the previous page
                        displayMenu(); // Display the menu
                    }
                }
                break;
            case 31: // Down arrow
                if ((currentIndex%FILES_PER_PAGE < FILES_PER_PAGE) && currentIndex < totalFiles-1) currentIndex++; // Move to the next file
                if (currentIndex >= (currentPage * FILES_PER_PAGE)) // Check if we need to move to the next page
                {
                    currentPage++; // Move to the next page
                    displayMenu(); // Display the menu
                }
                break;
            case 28: // Right arrow
                if (currentPage < totalPages) // Check if we are not on the last page
                {
                    currentPage++; // Move to the next page
                    currentIndex = (currentPage-1) * FILES_PER_PAGE; // Move to the first file of the page
                    displayMenu(); // Display the menu
                }
                break;
            case 29: // Left arrow
                if (currentPage > 1) // Check if we are not on the first page
                {
                    currentPage--; // Move to the previous page
                    currentIndex = (currentPage-1) * FILES_PER_PAGE; // Move to the first file of the page
                    displayMenu(); // Display the menu
                }
                break;
            case 27: // ESC
                enter_directory(-1);
                refresh_menu_state(0);
                break;
            case 72: // H - Help (uppercase H)
            case 104: // h - Help (lowercase h)
                // Help
                helpMenu(); // Display the help menu
                displayMenu();
                break;
            case 99: // C 
            case 67: // c 
                if (menu_ui_try_toggle_columns()) {
                    if (menu_message_row) {
                        readROMData(records, &totalFiles, &totalSize);
                    }
                    frame_rendered = 0;
                    displayMenu();
                }
                break;
            case 80: // P
            case 112: // p
                cycle_sd_partition();
                break;
            case 68: // D
            case 100: // d
                if (menu_shortcut_selection == MENU_SHORTCUT_MICROSD && !menu_message_row && !record_is_folder(&records[currentIndex % FILES_PER_PAGE])) {
                    menu_ui_print_last_line_text("Delete? Y/N");
                    key = (char)bios_chget();
                    if (key == 'y' || key == 'Y') {
                        Poke(CTRL_QUERY_BASE + 0, (unsigned char)(currentIndex & 0xFF));
                        Poke(CTRL_QUERY_BASE + 1, (unsigned char)((currentIndex >> 8) & 0xFF));
                        Poke(CTRL_CMD, CMD_DELETE_SD_FILE);
                        wait_command_with_blinking_status("Del...", 1000);
                        refresh_menu_state(0);
                    } else {
                        menu_ui_clear_last_line();
                    }
                }
                break;
            case 47: // / - Search
            {
                int search_result = read_search_query(search_query, SEARCH_MAX_LEN);
                if (search_result < 0) {
                    launch_wifi_config();
                    break;
                }
                if (search_result) {
                    const char *message = menu_ui_status_text("Searching...", "Searching the ROM list...");
                    send_query_to_pico(search_query);
                    Poke(CTRL_CMD, CMD_FIND_FIRST);
                    wait_command_with_blinking_status(message, 1000);
                    unsigned int match = read_match_index();
                    if (match == 0xFFFFu || match >= totalFiles) {
                        wait_key_with_blinking_status(menu_ui_status_text("Not found", "No matching ROM found."));
                        menu_ui_clear_last_line();
                    } else {
                        currentIndex = (int)match;
                        currentPage = (currentIndex / FILES_PER_PAGE) + 1;
                        menu_ui_clear_last_line();
                    }
                }
                displayMenu();
                break;
            }
            case 13: // Enter
            case 32: // Space
                if (menu_message_row) {
                    break;
                }
                if (record_is_folder(&records[currentIndex % FILES_PER_PAGE])) {
                    char selected_name[CTRL_QUERY_SIZE];
                    trim_name_to_buffer(records[currentIndex % FILES_PER_PAGE].Name, selected_name, CTRL_QUERY_SIZE - 1);
                    if (selected_name[0] == '.' && selected_name[1] == '.' && selected_name[2] == '\0') {
                        enter_directory(-1);
                    } else {
                        enter_directory(currentIndex);
                    }
                    refresh_menu_state(0);
                } else if (key == 13 || record_is_mp3(&records[currentIndex % FILES_PER_PAGE])) {
                    show_rom_screen((unsigned int)currentIndex);
                } else {
                    quick_run_rom((unsigned int)currentIndex);
                }
                break;
        }
        if (totalFiles > 0 && !menu_message_row) {
            unsigned char row = (unsigned char)((currentIndex % FILES_PER_PAGE) + 2);
            draw_menu_row(row, &records[currentIndex % FILES_PER_PAGE], 1);
            Locate(0, row);
        }
    }
}

void main() {
    // Initialize the variables
    currentPage = 1; // Start on page 1
    currentIndex = 0; // Start at the first file - index 0
    paging_enabled = 0;
    use_80_columns = 0;
    name_col_width = NAME_COL_WIDTH;
    frame_rendered = 0;
    menu_message_row = 0;
    menu_shortcut_selection = MENU_SHORTCUT_FLASH;
    
    readROMData(records, &totalFiles, &totalSize);
    totalPages = (int)((totalFiles + FILES_PER_PAGE - 1) / FILES_PER_PAGE);

    if (totalPages == 0) {
        totalPages = 1;
    }
    menu_ui_init_text_mode();
    invert_chars(32, 126); // Invert the characters from 32 to 126
    clear_fkeys();

    menu_ui_render_menu_frame();
    restore_last_selection();
    *((unsigned int *)BIOS_GETPNT) = *((unsigned int *)BIOS_PUTPNT);
    redefine_function_keys();
    // Activate navigation
    navigateMenu();
}


static int read_search_query(char *buffer, int max_len) {
    int len = 0;
    const char *prompt = "Search: ";
    unsigned char prompt_col = (unsigned char)strlen(prompt);
    unsigned char content_width = (unsigned char)(menu_ui_row_width() - 2);
    unsigned char right_len = use_80_columns ? 47 : 0;
    unsigned char left_limit = content_width > right_len ? (unsigned char)(content_width - right_len) : 0;
    int input_limit = max_len;

    if (left_limit <= prompt_col) {
        return 0;
    }

    if (input_limit > (int)(left_limit - prompt_col)) {
        input_limit = (int)(left_limit - prompt_col);
    }

    buffer[0] = '\0';
    if (use_80_columns) {
        menu_ui_print_last_line_text(prompt);
    } else {
        Locate(0, 23);
        printf("Search: ");
        for (unsigned char col = prompt_col; col < content_width; col++) {
            PrintChar(' ');
        }
    }
    Locate(prompt_col, 23);
    for (int i = 0; i < input_limit; i++) {
        PrintChar(' ');
    }
    Locate(prompt_col, 23);

    while (1) {
        char ch = (char)bios_chget();
        if (ch == MENU_KEY_F4_CONFIG) {
            buffer[0] = '\0';
            return -1;
        }
        if (ch == 13) {
            buffer[len] = '\0';
            return 1;
        }
        if (ch == 27) {
            buffer[0] = '\0';
            return 0;
        }
        if (ch == 8 || ch == 127) {
            if (len > 0) {
                len--;
                buffer[len] = '\0';
                Locate(prompt_col + len, 23);
                printf(" ");
                Locate(prompt_col + len, 23);
            }
            continue;
        }
        if (ch >= 32 && ch <= 126) {
            if (len < input_limit) {
                buffer[len++] = ch;
                buffer[len] = '\0';
                Locate(prompt_col + len - 1, 23);
                PrintChar((unsigned char)ch);
            }
        }
    }
}

#pragma disable_warning 85
void msx_wait(uint16_t times_jiffy)
{
    __asm
    ei
    WAIT:
        halt
        dec hl
        ld a,h
        or l
        jr nz, WAIT
        ret
    __endasm;
}

void delay_ms(uint16_t milliseconds)
{
    msx_wait(milliseconds / 20);
    disk_led_poll();
}

// --- microSD activity LED ---
// The Pico bumps CTRL_DISK_ACT on every microSD access, so the menu mirrors the
// traffic on the keyboard CAPS LED. The LED is driven through the PPI bit
// set/reset register (port 0xAB, port C bit 6, active low) so the keyboard row
// bits kept in port C are left untouched.

#define DISK_LED_BLINK_TICKS 6u // ~100 ms between toggles at 60 Hz
#define DISK_LED_IDLE_TICKS 12u // ~200 ms without traffic ends the blinking

static unsigned char disk_led_count; // last CTRL_DISK_ACT value seen
static unsigned char disk_led_busy;  // 1 while the card is considered active
static unsigned char disk_led_lit;   // 1 while the LED is on
static unsigned char disk_led_seen;  // JIFFY tick of the last counter change
static unsigned char disk_led_tick;  // JIFFY tick of the last LED toggle

// caps_led_on - Light the CAPS LED (reset PPI port C bit 6).
static void caps_led_on(void) __naked
{
    __asm
    ld a, #0x0C
    out (#0xAB), a
    ret
    __endasm;
}

// caps_led_off - Turn the CAPS LED off (set PPI port C bit 6).
static void caps_led_off(void) __naked
{
    __asm
    ld a, #0x0D
    out (#0xAB), a
    ret
    __endasm;
}

// disk_led_poll - Blink the CAPS LED while the Pico reports microSD activity
// and hand the LED back to the BIOS CAPS state once the card goes idle.
static void disk_led_poll(void)
{
    unsigned char now = *((volatile unsigned char *)JIFFY);
    unsigned char count = *((volatile unsigned char *)CTRL_DISK_ACT);

    if (count != disk_led_count) {
        disk_led_count = count;
        disk_led_seen = now;
        disk_led_busy = 1;
    }
    if (!disk_led_busy) {
        return;
    }

    if ((unsigned char)(now - disk_led_seen) >= DISK_LED_IDLE_TICKS) {
        disk_led_busy = 0;
        disk_led_lit = *((volatile unsigned char *)BIOS_CAPST) ? 1 : 0;
    } else if ((unsigned char)(now - disk_led_tick) >= DISK_LED_BLINK_TICKS) {
        disk_led_tick = now;
        disk_led_lit = disk_led_lit ? 0 : 1;
    } else {
        return;
    }

    if (disk_led_lit) {
        caps_led_on();
    } else {
        caps_led_off();
    }
}

