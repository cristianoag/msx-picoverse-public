// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// menu.c - MSX ROM with the menu program for the MSX PICOVERSE 2350 project
// 
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#define MENU_KEEP_FUSION_TAG // Keep the single FUSION-C credit string (see menu.h)
#include <string.h>
#include <stdint.h>
#include "menu.h"
#include "menu_state.h"
#include "menu_ui.h"
#include "menu_input.h"
#include "screen_rom.h"
#include "explorer_fh.h"

#define SEARCH_MAX_LEN 24

// The Pico supplies one page at a time; the selection index spans the full list.
int currentPage;
int totalPages;
int currentIndex;
unsigned int totalFiles;
ROMRecord records[FILES_PER_PAGE];
static unsigned char menu_message_row;
static void open_file_hunter(unsigned char resume);
static unsigned char restore_pending; // waiting for the Pico to locate the last executed entry
static unsigned char menu_in_subfolder; // the listing starts with the ".." entry
static const char flash_mapper_error[] = "Mapper/audio unsupported";
static const char flash_progress_label[] = "Copying to flash";

// --- Forward declarations (UI + Pico protocol) ---
static void send_query_to_pico(const char *query);
static unsigned int read_match_index(void);
static void readROMData(unsigned int *recordCount);
static void draw_record_row(unsigned char row, const ROMRecord *record, const char *name_override, unsigned char selected);
static void enter_directory(int index);
static void refresh_menu_state(const char *loading_text);
static void wait_command_with_blinking_status(const char *text, unsigned int wait_limit);
static void draw_menu_row(unsigned char row, ROMRecord *record, int selected);
static void redefine_function_keys(void);
static void switch_browse_source(unsigned char source_mode, const char *loading_text);
static void switch_menu_source(unsigned char source_mode);
void msx_wait(uint16_t times_jiffy);
static void disk_led_poll(void);

// --- ROM record helpers ---
// Wire integers are little-endian; no alignment is required in the page buffer.
static unsigned long read_ulong(const unsigned char *ptr) {
    return (unsigned long)ptr[0] |
           ((unsigned long)ptr[1] << 8) |
           ((unsigned long)ptr[2] << 16) |
           ((unsigned long)ptr[3] << 24);
}

// Older firmware terminates its flat record table with an erased (0xFF) record.
static int isEndOfData(const unsigned char *memory) {
    for (unsigned int i = 0; i < ROM_RECORD_SIZE; i++) {
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

int record_needs_flash(const ROMRecord *record) {
    return (record->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG)) == SOURCE_SD_FLAG &&
           record->Mapper != (SOURCE_SD_FLAG | 23) &&
           record->Size > SD_ROM_MAX_SIZE_KB * 1024UL;
}

// build_record_size_text - Size column text: "<n>KB" up to 1024 KB, else "<n>MB"
// rounded up. Works on 16-bit pieces of the byte size: no 32-bit division.
static void build_record_size_text(unsigned long size, char *out) {
    unsigned int high = (unsigned int)(size >> 16);
    unsigned int kb_in_mb = (((unsigned int)(size & 0xFFFFu) >> 10) | ((high & 15u) << 6)); // KB left over in the MB
    unsigned int mb = high >> 4;

    if (mb > 1 || (mb == 1 && kb_in_mb)) {
        menu_ui_size_text(out, mb + (kb_in_mb != 0), 'M');
    } else {
        menu_ui_size_text(out, (mb << 10) | kb_in_mb, 'K');
    }
}

// build_record_row - Build the screen row of a list record in `out`.
static void build_record_row(const ROMRecord *record, const char *name_override, char *out, unsigned char selected) {
    const char *source = (record->Mapper & SOURCE_SD_FLAG) ? "SD" : "FL";
    const char *type_label = " ROM";
    char size_text[8];

    size_text[0] = '\0';
    if (record_is_folder(record)) {
        type_label = "<DIR>";
        source = "SD";
    } else {
        if (record_is_mp3(record) && ((record->Mapper & AUDIO_TYPE_MASK) == AUDIO_TYPE_WAV)) {
            type_label = " WAV";
            source = "SD";
        } else if (record_is_mp3(record)) {
            type_label = " MP3";
            source = "SD";
        } else if (record_mapper_code(record->Mapper) == 23) {
            type_label = " DSK";
        }
        build_record_size_text(record->Size, size_text);
    }
    menu_ui_build_row(out, name_override ? name_override : record->Name, size_text, source, type_label, selected);
}

// draw_record_row - Rebuild one list row in its slot of the page buffer and write it to the screen.
static void draw_record_row(unsigned char row, const ROMRecord *record, const char *name_override, unsigned char selected) {
    char *slot = menu_ui_list_rows + (unsigned int)(row - 2) * menu_ui_row_width();

    build_record_row(record, name_override, slot, selected);
    menu_ui_blit_rows(row, slot, 1);
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
        POKE_U16(CTRL_QUERY_BASE, index);
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
    readROMData(&totalFiles);
    menu_in_subfolder = totalFiles && record_is_folder(&records[0]) && records[0].Name[0] == '.' &&
                        records[0].Name[1] == '.' && records[0].Name[2] <= ' ';
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
    if (menu_shortcut_selection == MENU_SHORTCUT_MICROSD) {
        menu_ui_capture_sd_status();
    }
    menu_ui_clear_last_line();
    displayMenu();
    if (menu_shortcut_selection == MENU_SHORTCUT_MICROSD && Peek(CTRL_STATUS) == CTRL_STATUS_SD_MISSING) {
        menu_ui_print_last_line_text(menu_ui_status_text("No microSD card", "No microSD card inserted."));
    }
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

static unsigned char wait_flash_copy(void) {
    const char *error;
    while (Peek(CTRL_CMD)) {
        unsigned char progress = Peek(CTRL_MAPPER);
        if (progress == FLASH_COPY_DETECTING) {
            menu_ui_print_last_line_text("Mapper...");
        } else if (progress <= 100) {
            menu_ui_print_progress(use_80_columns ? flash_progress_label : "Flash", (unsigned int)progress);
        }
        delay_ms(100);
    }
    if (Peek(CTRL_ACK)) return 1;
    switch (Peek(CTRL_MAPPER)) {
        case FLASH_COPY_NO_SPACE:  error = "No space: clean F1."; break;
        case FLASH_COPY_TOO_LARGE: error = "ROM too big for flash."; break;
        case FLASH_COPY_DUPLICATE: error = "Name exists. Rename ROM."; break;
        case FLASH_COPY_MAPPER:    error = flash_mapper_error; break;
        default:                  error = "Flash failed. Press key."; break;
    }
    menu_ui_wait_key_with_blinking_status(error);
    menu_ui_clear_last_line();
    return 0;
}

// run_record_command - Run a Pico command on the selected entry: delete or copy
// to flash (prompt asks for Y/N confirmation; the copy shows a progress
// percentage), or rename (prompt is 0: the new name is edited first). Creating
// a folder (prompt 0) edits an empty name and needs no selected entry.
static void run_record_command(const char *prompt, unsigned char cmd) {
    ROMRecord *record = &records[currentIndex % FILES_PER_PAGE];
    char name[MAX_FILE_NAME_LENGTH + 1];

    if (cmd != CMD_CREATE_FOLDER && menu_message_row) {
        return;
    }
    if (cmd != CMD_CREATE_FOLDER && record_is_folder(record)) {
        // Only D works on a folder (not "..") and removes it when it is empty.
        if (cmd != CMD_DELETE_SD_FILE || (record->Name[0] == '.' && record->Name[1] == '.' && !record->Name[2])) {
            return;
        }
        prompt = "Delete folder? Y/N";
    }
    if (prompt) {
        menu_ui_print_last_line_text(prompt);
        if ((bios_chget_quiet() | 0x20) != 'y') {
            menu_ui_clear_last_line();
            return;
        }
    } else {
        strcpy(name, cmd == CMD_CREATE_FOLDER ? "" : record->Name);
        // Only an edited name is sent: the field may have shortened a long one.
        int edit_result = menu_ui_read_line(cmd == CMD_CREATE_FOLDER ? "Folder: " : "Name: ", name, MAX_FILE_NAME_LENGTH - 1);
        if (edit_result == -1) {
            launch_wifi_config();
        } else if (edit_result == -2) {
            helpMenu(HELP_PAGE_LIST);
            displayMenu();
        }
        if (edit_result != 1 || !name[0]) {
            menu_ui_clear_last_line();
            return;
        }
        // The Pico captures the new name written to the start of the data window.
        memcpy((void *)MEMORY_START, name, strlen(name) + 1);
    }
    POKE_U16(CTRL_QUERY_BASE, currentIndex);
    Poke(CTRL_CMD, cmd);
    menu_ui_print_last_line_text("Wait");
    if (cmd == CMD_COPY_TO_FLASH) {
        (void)wait_flash_copy();
    } else {
        for (unsigned int wait = 0; wait < 60000u && Peek(CTRL_CMD); wait++) {
            delay_ms(100);
        }
        if (!Peek(CTRL_ACK)) {
            menu_ui_wait_key_with_blinking_status(
                cmd == CMD_DELETE_SD_FILE && Peek(CTRL_MAPPER) == DELETE_ERR_NOT_EMPTY ? "Folder not empty. Press key." : "Failed");
        } else if (!prompt) {
            restore_pending = 1;
        }
    }
    refresh_menu_state(0);
}

// move_file - M marks the selected microSD file; M in another folder moves it
// there. The Pico keeps the marked path; the menu only remembers that one is
// pending so the line-24 hint shows and the destination can be browsed.
static void move_file(void) {
    ROMRecord *record = &records[currentIndex % FILES_PER_PAGE];
    const char *error = "Move failed. Press key.";

    if (!menu_move_pending) {
        if (!totalFiles || menu_message_row || record_is_folder(record)) {
            return;
        }
        POKE_U16(CTRL_QUERY_BASE, currentIndex);
        menu_move_pending = run_ctrl_cmd(CMD_MOVE_MARK);
        if (!menu_move_pending) {
            menu_ui_wait_key_with_blinking_status("Only files can move.");
        }
        menu_ui_clear_last_line();
        return;
    }
    menu_ui_print_last_line_text("Wait");
    Poke(CTRL_CMD, CMD_MOVE_PASTE);
    wait_ctrl_cmd();
    if (Peek(CTRL_ACK)) {
        menu_move_pending = 0;
        restore_pending = 1;
        refresh_menu_state(0);
        return;
    }
    if (Peek(CTRL_MAPPER) == MOVE_ERR_SAME) {
        menu_move_pending = 0;
        error = 0;
    } else if (Peek(CTRL_MAPPER) == MOVE_ERR_EXISTS) {
        error = "Name exists there. Press key.";
    }
    if (error) {
        menu_ui_wait_key_with_blinking_status(error);
    }
    menu_ui_clear_last_line();
}

// open_file_hunter - Run File Hunter and show the list it hands back to. File
// Hunter can ask for the download folder to be picked (A there): it returns the
// microSD list with menu_pick_download set.
static void open_file_hunter(unsigned char resume) {
    unsigned char return_shortcut = menu_shortcut_selection;
    unsigned char next_source;

    menu_move_pending = 0;
    menu_pick_download = 0;
    next_source = explorer_fh_run(resume);
    if (next_source == SOURCE_MODE_ALL) {
        next_source = return_shortcut == MENU_SHORTCUT_MICROSD ? SOURCE_MODE_SD : SOURCE_MODE_FLASH;
    }
    switch_menu_source(next_source);
}

// set_download_folder - A in the microSD list: the folder being browsed becomes
// the File Hunter download folder. When File Hunter asked for it, go back there.
static void set_download_folder(void) {
    const char *error = "Could not save. Press key.";

    menu_ui_print_last_line_text("Wait");
    Poke(CTRL_CMD, CMD_SET_DOWNLOAD_DIR);
    wait_ctrl_cmd();
    if (Peek(CTRL_ACK)) {
        if (menu_pick_download) {
            open_file_hunter(1);
            return;
        }
        error = "Download folder saved. Press key.";
    } else if (Peek(CTRL_MAPPER) == DOWNLOAD_DIR_ERR_LONG) {
        error = "Path too long (96 max). Press key.";
    }
    menu_ui_wait_key_with_blinking_status(error);
    menu_ui_clear_last_line();
}

// PVEX pages contain a record table, TLV fields and a shared name pool.
// Offsets are relative to MEMORY_START; unknown TLV types can be skipped.
static int parse_page_buffer(void) {
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
    }

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

    if (parse_page_buffer()) {
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
        // Keep the legacy wire stride, including the unused flash-offset word.
        memory += ROM_RECORD_SIZE;
    }

    for (; i < FILES_PER_PAGE; i++) {
        clear_record(&records[i]);
    }
}

// --- ROM list loading ---

// Modern firmware supplies the count through control registers. Retain the
// flat-table fallback for firmware without paging, caching only the first page.
static void readROMData(unsigned int *recordCount) {
    unsigned char *memory = (unsigned char *)MEMORY_START;
    unsigned int count;

    unsigned char ctrl_status = *((unsigned char *)CTRL_STATUS);
    menu_message_row = 0;
    if (ctrl_status == CTRL_STATUS_SD_MISSING) {
        strncpy(records[0].Name, menu_ui_status_text("No microSD card", "No microSD card inserted."), MAX_FILE_NAME_LENGTH);
        records[0].Name[MAX_FILE_NAME_LENGTH] = '\0';
        records[0].Mapper = SOURCE_SD_FLAG;
        records[0].Size = 0;
        paging_enabled = 0;
        *recordCount = 1;
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
        load_page_records(0);
        return;
    }

    count = 0;
    while (count < MAX_ROM_RECORDS && !isEndOfData(memory)) {
        if (count < FILES_PER_PAGE) {
            MemCopy(records[count].Name, memory, MAX_FILE_NAME_LENGTH);
            for (int i = strlen(records[count].Name); i < MAX_FILE_NAME_LENGTH; i++) {
                records[count].Name[i] = ' ';
            }
            records[count].Name[MAX_FILE_NAME_LENGTH] = '\0';
            records[count].Mapper = memory[MAX_FILE_NAME_LENGTH];
            records[count].Size = read_ulong(&memory[MAX_FILE_NAME_LENGTH + 1]);
        }
        memory += ROM_RECORD_SIZE;
        count++;
    }

    *recordCount = count;
}

// --- BIOS wrappers and timing ---

// mini_printf.c prints through this symbol.
// Use CHPUT through CALSLT so output works while the cartridge occupies page 1.
int putchar (int character)
{
    __asm
    ld      hl, #2              ; first stack argument follows the return address
    add     hl, sp
    ld      a, (hl)             ; low byte of character

    ld      iy,(#BIOS_EXPTBL-1)  ;BIOS slot in iyh
    push    ix                  ; preserve the SDCC frame pointer
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
    fnkstr[64] = MENU_KEY_F5_HELP;
}

static void switch_browse_source(unsigned char source_mode, const char *loading_text) {
    unsigned char payload[2];

    Poke(MP3_CTRL_CMD, MP3_CMD_STOP);
    menu_shortcut_selection = (source_mode == SOURCE_MODE_SD) ? MENU_SHORTCUT_MICROSD : MENU_SHORTCUT_FLASH;
    if (source_mode != SOURCE_MODE_SD) {
        menu_move_pending = 0;
        menu_pick_download = 0;
    }
    menu_ui_update_list_footer();
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
    POKE_U16(CTRL_QUERY_BASE, index);
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
        menu_move_pending = 0;
        Poke(CTRL_CMD, CMD_CYCLE_SD_PARTITION);
        refresh_menu_state(menu_ui_status_text("Load SD", "Loading from microSD..."));
    }
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


// bios_gtstck - Read a joystick direction using the MSX BIOS routine GTSTCK
// port 1 and 2 select the joystick ports. Returns 0 (centered) or 1-8 (clockwise from up).
static unsigned char bios_gtstck(unsigned char port) __naked
{
    (void)port;
    __asm
    ld      hl,#2
    add     hl,sp
    ld      a,(hl)
    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_GTSTCK
    call    BIOS_CALSLT
    pop     ix
    ld      l,a
    ld      h,#0
    ret
    __endasm;
}

// bios_gttrig - Read a trigger button using the MSX BIOS routine GTTRIG
// 1/3 select port 1 buttons A/B and 2/4 select port 2 buttons A/B. Returns 0xFF when pressed.
static unsigned char bios_gttrig(unsigned char button) __naked
{
    (void)button;
    __asm
    ld      hl,#2
    add     hl,sp
    ld      a,(hl)
    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_GTTRIG
    call    BIOS_CALSLT
    pop     ix
    ld      l,a
    ld      h,#0
    ret
    __endasm;
}

// Joystick state, initialized in main(). Directions repeat at a controlled rate and the
// fire buttons only trigger once per press, because the raw joystick is too fast for the menu.
static unsigned char joy_last_dir;
static unsigned char joy_fire_down;
static unsigned int joy_repeat_at;

// poll_joystick - Poll both joystick ports and translate them to menu key codes
// Returns the cursor key code for a direction (with first-press delay and auto-repeat),
// 32 (Space, run the ROM) for a new press of any fire button, or 0 when there is nothing to report.
static int poll_joystick(void)
{
    volatile unsigned int *jiffyPtr = (volatile unsigned int *)JIFFY;
    unsigned char fire = bios_gttrig(1) | bios_gttrig(2) | bios_gttrig(3) | bios_gttrig(4);
    unsigned char dir = bios_gtstck(1);

    if (!dir) {
        dir = bios_gtstck(2);
    }

    if (fire) {
        if (!joy_fire_down) {
            joy_fire_down = 1;
            return 32;
        }
    } else {
        joy_fire_down = 0;
    }

    if (!(dir & 1)) {
        joy_last_dir = 0; // centered or a diagonal
        return 0;
    }

    unsigned int now = *jiffyPtr;
    if (dir != joy_last_dir) {
        joy_last_dir = dir;
        joy_repeat_at = now + JOY_FIRST_DELAY;
    } else if ((int)(now - joy_repeat_at) >= 0) {
        joy_repeat_at = now + JOY_REPEAT_DELAY;
    } else {
        return 0;
    }
    // GTSTCK 1/3/5/7 = up/right/down/left -> cursor key codes 30/28/31/29
    return dir == 1 ? 30 : dir == 3 ? 28 : dir == 5 ? 31 : 29;
}

// Idle polling keeps the disk LED and selected name animation responsive.
static int wait_for_key_with_scroll(void)
{
    volatile unsigned int *jiffyPtr = (volatile unsigned int *)JIFFY;
    unsigned int lastTick = *jiffyPtr;
    int startPos = 0;
    const unsigned int scrollDelay = 30U; // 0.5 seconds at 60 Hz
    char name_window[MAX_FILE_NAME_LENGTH + 1];

    while (1) {
        unsigned char peek = bios_chsns();
        if (peek) {
            return (int)bios_chget();
        }

        {
            int joyKey = poll_joystick();
            if (joyKey) {
                return joyKey;
            }
        }

        disk_led_poll();

        // Unsigned subtraction also handles the 16-bit BIOS tick wrapping.
        if ((unsigned int)(*jiffyPtr - lastTick) >= scrollDelay) {
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
                            draw_record_row((unsigned char)((currentIndex % FILES_PER_PAGE) + 2), record, name_window, 1);
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
const char *mapper_description(int number) {
    // Array of strings for the descriptions. Static so SDCC keeps it in ROM
    // instead of rebuilding the pointer table on the stack at every call.
    static const char *const descriptions[] = {"PLA-16", "PLA-32", "KonSCC", "PLN-48", "ASC-08", "ASC-16", "Konami", "NEO-8", "NEO-16", "SYSTEM", "SYSTEM", "ASC16X", "PLN-64", "MANBW2"};
    unsigned char code = record_mapper_code((unsigned char)number);
    if ((code >= 15 && code <= 21) || code == MAPPER_PICOAMP) {
        return "SYSTEM";
    }
    if (code == 22) {
        return "ASC16X-FR";
    }
    if (code == 23) {
        return "DSK";
    }
    if (code == 0 || code > 14) {
        return "Unknown";
    }
    return descriptions[code - 1];
}

// --- Menu rendering ---

static void draw_menu_row(unsigned char row, ROMRecord *record, int selected) {
    draw_record_row(row, record, NULL, (unsigned char)selected);
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

    // Build the whole page (rows 2 to 20, empty ones blank) and paint it in one transfer.
    unsigned char width = menu_ui_row_width();
    char *out = menu_ui_list_rows;
    unsigned int line = 0;
    for (unsigned int idx = startIndex; idx < endIndex; idx++, line++) {
        build_record_row(&records[line], NULL, out, !menu_message_row && idx == (unsigned int)currentIndex);
        out += width;
    }
    memset(out, ' ', (FILES_PER_PAGE - line) * width);
    menu_ui_blit_rows(2, menu_ui_list_rows, FILES_PER_PAGE);

    menu_ui_update_list_footer();
}

// --- Menu screens ---

// helpMenu - F5 help. The pages are kept in the Pico firmware (help_text.h) so
// they can be long and differ between 40 and 80 columns: each page is asked for
// with CMD_HELP_PAGE and its lines are read from the data window. LEFT/RIGHT
// change the page, any other key closes the help.
void helpMenu(unsigned char page)
{
    unsigned char count;
    unsigned char key;
    char status[48];

    while (1) {
        const char *text = (const char *)MEMORY_START + 1;

        menu_ui_render_detail_frame();
        Poke(CTRL_QUERY_BASE, page);
        Poke(CTRL_QUERY_BASE + 1, use_80_columns);
        // CTRL_ACK means something else while File Hunter is open, so the Pico
        // puts the page count in the first byte of the data window.
        (void)run_ctrl_cmd(CMD_HELP_PAGE);
        count = *(const unsigned char *)MEMORY_START;
        if (count > 16) {
            count = 0;
        }
        // Like the other screens: separator, footer and status first, then the content.
        menu_ui_print_footer_line(MENU_SHORTCUT_HELP);
        sprintf(status, "Help %u/%u - LEFT/RIGHT page, key closes", (unsigned int)(unsigned char)(page + 1), (unsigned int)count);
        menu_ui_print_last_line_text(status);
        for (unsigned char row = 2; count && row <= 20 && *text; row++) {
            unsigned char heading = *text == '\1';
            if (heading) {
                text++;
            }
            unsigned char len = (unsigned char)strlen(text);
            Locate(0, row);
            if (heading) {
                menu_ui_print_str_inverted_width(text, len);
            } else {
                printf("%s", text);
            }
            text += len + 1;
        }
        key = bios_chget_quiet();
        if (key == 28 && page + 1 < count) {
            page++;
        } else if (key == 29 && page) {
            page--;
        } else if (key != 28 && key != 29) {
            break;
        }
    }
    // The Pico rebuilds the list page the help text replaced in the data window.
    (void)run_ctrl_cmd(CMD_HELP_END);
    if (key == '1' || key == '2' || key == '3' || key == MENU_KEY_F4_CONFIG) {
        menu_pending_key = key;
    }
    frame_rendered = 0;
}


// A large SD ROM must become a flash entry before options/Run can operate on it.
// Smaller entries can launch directly; quick_run asks the Pico for saved defaults.
void loadGame(int index, unsigned char quick_run)
{
    ROMRecord *record = &records[index % FILES_PER_PAGE];
    if (record_is_folder(record) || record_is_mp3(record))
    {
        return;
    }
    Poke(MP3_CTRL_CMD, MP3_CMD_STOP);
    if (record_needs_flash(record)) {
        menu_ui_print_last_line_text("ROM>4MB: Copy to flash? Y/N");
        if ((bios_chget_quiet() | 0x20) != 'y') {
            menu_ui_clear_last_line();
            return;
        }
        menu_ui_print_progress(use_80_columns ? flash_progress_label : "Flash", 0);
        POKE_U16(CTRL_QUERY_BASE, index);
        (void)run_ctrl_cmd(CMD_LOAD_OPTIONS);
        Poke(CTRL_CMD, CMD_PREPARE_FLASH_ROM);
        if (!wait_flash_copy()) return;
        menu_shortcut_selection = MENU_SHORTCUT_FLASH;
        restore_pending = 1;
        frame_rendered = 0;
        refresh_menu_state(0);
        show_rom_screen((unsigned int)currentIndex);
        return;
    }
    if (quick_run) {
        POKE_U16(CTRL_QUERY_BASE, index);
        (void)run_ctrl_cmd(CMD_PREPARE_QUICK_RUN);
        record->Mapper = (record->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG)) | Peek(CTRL_MAPPER);
    }
    unsigned char mapper = record_mapper_code(record->Mapper);
    if (mapper == MAPPER_ASCII16X_FR &&
        (record->Size > 8388608UL ||
         (record->Size > SD_ROM_MAX_SIZE_KB * 1024UL && Peek(CTRL_AUDIO) == AUDIO_PROFILE_MSX_MUSIC))) {
        menu_ui_wait_key_with_blinking_status(flash_mapper_error);
        menu_ui_clear_last_line();
        return;
    }
    if (mapper != 0)
    {
        /* ASC16X-FR syncs the cartridge with its .FLA image on the microSD
           before the game starts, which takes a few seconds on a large ROM.
           Warn here rather than in the ROM screen so both ways of launching
           a game - Enter from the detail page and Space from the list - show
           it. */
        if (mapper == MAPPER_ASCII16X_FR)
        {
            menu_ui_print_last_line_text("Preparing FlashROM image...");
        }
        save_last_selection((unsigned int)index); // Remember it for the next boot
        Poke(ROM_SELECT_REGISTER, index); // Set the game index (absolute)
        execute_rst00(); // Execute RST 00h to reset the MSX computer and load the game
        execute_rst00();
    }
}

// --- Input/navigation ---

// navigateMenu - Navigate the menu
// This function will navigate the menu. It will wait for the user to press a key and then act based on the key pressed. The user can navigate the menu using the arrow keys
// to move between files/pages, Enter to open an entry, F5 for help and C to toggle columns.
// The function will update the current page and current index based on the key pressed and display the menu again.
void navigateMenu() 
{
    char key;
    char search_query[SEARCH_MAX_LEN + 1];

    while (1) 
    {
        key = menu_pending_key;
        menu_pending_key = 0;
        if (!key) {
            key = wait_for_key_with_scroll();
        }
        switch (key) {
            case '1':
                switch_menu_source(SOURCE_MODE_FLASH);
                continue;
            case '2':
                switch_menu_source(SOURCE_MODE_SD);
                continue;
            case '3':
                open_file_hunter(0);
                continue;
            case MENU_KEY_F4_CONFIG:
                launch_wifi_config();
                continue;
            case MENU_KEY_F5_HELP:
                helpMenu(HELP_PAGE_LIST);
                displayMenu();
                continue;
            case 'c':
            case 'C':
                if (menu_ui_try_toggle_columns()) {
                    if (menu_message_row) {
                        readROMData(&totalFiles);
                    }
                    frame_rendered = 0;
                    displayMenu();
                }
                continue;
            case 'p':
            case 'P':
                cycle_sd_partition();
                continue;
            case 'w':
            case 'W':
                if (menu_shortcut_selection == MENU_SHORTCUT_MICROSD) {
                    run_record_command(0, CMD_CREATE_FOLDER);
                }
                continue;
            case 'm':
            case 'M':
                if (menu_shortcut_selection == MENU_SHORTCUT_MICROSD) {
                    move_file();
                }
                continue;
            case 'a':
            case 'A':
                if (menu_shortcut_selection == MENU_SHORTCUT_MICROSD) {
                    set_download_folder();
                }
                continue;
            case 27:
                if ((menu_move_pending || menu_pick_download) && !menu_in_subfolder) {
                    // Nothing above the root: Esc drops the pending move or folder pick instead.
                    menu_move_pending = 0;
                    menu_pick_download = 0;
                    menu_ui_clear_last_line();
                    continue;
                }
                enter_directory(-1);
                refresh_menu_state(0);
                continue;
        }
        if (totalFiles == 0) {
            continue;
        }
        unsigned int currentRow = (currentIndex % FILES_PER_PAGE) + 2;
        draw_menu_row((unsigned char)currentRow, &records[currentIndex % FILES_PER_PAGE], 0);
        switch (key) {
            case 30: // Up arrow
                if (currentIndex > 0) // Check if we are not at the first file
                {
                    currentIndex--;
                    if (currentIndex < ((currentPage-1) * FILES_PER_PAGE))  // Check if we need to move to the previous page
                    {
                        currentPage--; // Move to the previous page
                        displayMenu(); // Display the menu
                    }
                }
                break;
            case 31: // Down arrow
                if (currentIndex < totalFiles - 1) {
                    currentIndex++;
                }
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
            case 68: // D - delete the microSD file or flash entry
            case 100: // d
                run_record_command("Delete? Y/N", CMD_DELETE_SD_FILE);
                break;
            case 70: // F - copy the microSD ROM to flash
            case 102: // f
                if (menu_shortcut_selection == MENU_SHORTCUT_MICROSD) {
                    run_record_command("To flash? Y/N", CMD_COPY_TO_FLASH);
                }
                break;
            case 82: // R - rename the ROM/DSK file or flash entry
            case 114: // r
                if (!record_is_mp3(&records[currentIndex % FILES_PER_PAGE])) {
                    run_record_command(0, CMD_RENAME_ENTRY);
                }
                break;
            case 47: // / - Search
            {
                int search_result;
                search_query[0] = '\0';
                search_result = menu_ui_read_line("Search: ", search_query, SEARCH_MAX_LEN);
                if (search_result == -1) {
                    launch_wifi_config();
                } else if (search_result == -2) {
                    helpMenu(HELP_PAGE_LIST);
                } else if (search_result) {
                    const char *message = menu_ui_status_text("Searching...", "Searching the ROM list...");
                    send_query_to_pico(search_query);
                    Poke(CTRL_CMD, CMD_FIND_FIRST);
                    wait_command_with_blinking_status(message, 1000);
                    unsigned int match = read_match_index();
                    if (match == 0xFFFFu || match >= totalFiles) {
                        menu_ui_wait_key_with_blinking_status(menu_ui_status_text("Not found", "No matching ROM found."));
                        menu_ui_clear_last_line();
                    } else {
                        currentIndex = (int)match;
                        currentPage = (currentIndex / FILES_PER_PAGE) + 1;
                        menu_ui_clear_last_line();
                    }
                }
                menu_ui_clear_last_line();
                displayMenu();
                break;
            }
            case 13: // Enter
            case 32: { // Space
                ROMRecord *record = &records[currentIndex % FILES_PER_PAGE];
                if (menu_message_row) {
                    break;
                }
                if (record_is_folder(record)) {
                    char selected_name[CTRL_QUERY_SIZE];
                    trim_name_to_buffer(record->Name, selected_name, CTRL_QUERY_SIZE - 1);
                    if (selected_name[0] == '.' && selected_name[1] == '.' && selected_name[2] == '\0') {
                        enter_directory(-1);
                    } else {
                        enter_directory(currentIndex);
                    }
                    refresh_menu_state(0);
                } else if (key == 13 || record_is_mp3(record)) {
                    show_rom_screen((unsigned int)currentIndex);
                } else {
                    quick_run_rom((unsigned int)currentIndex);
                }
                break;
            }
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
    menu_pending_key = 0;
    menu_move_pending = 0;
    menu_pick_download = 0;
    joy_last_dir = 0;
    joy_fire_down = 1; // a fire button held during boot must be released before it runs a ROM
    joy_repeat_at = 0;
    fh_catalog_type = CTRL_FH_TYPE_ROM;
    
    readROMData(&totalFiles);
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
