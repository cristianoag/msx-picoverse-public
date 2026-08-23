// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// explorer_fh.c - MSX Explorer File Hunter (online ROM catalog) client
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include <stdio.h>
#include "menu.h"
#include "menu_ui.h"
#include "menu_input.h"
#include "menu_state.h"
#include "explorer_fh.h"

#define EXPLORER_FH_NAME_MAX MAX_FILE_NAME_LENGTH
#define EXPLORER_FH_STATUS_READY CTRL_MAGIC
#define EXPLORER_FH_STATUS_BUSY 0x5A
#define EXPLORER_FH_RESULT_SAVING 0x02
#define EXPLORER_FH_MAX_QUERY 24
#define EXPLORER_FH_FLAG_MESSAGE 0x80

typedef struct {
    char name[EXPLORER_FH_NAME_MAX + 1];
    unsigned char flags;
    unsigned int size_kb;
} ExplorerFHRecord;

static unsigned int fh_current_page;
static unsigned int fh_total_pages;
static unsigned int fh_current_index;
static unsigned int fh_total_files;
static ExplorerFHRecord fh_records[FILES_PER_PAGE];
static char fh_status_right[CTRL_FH_STATUS_TEXT_SIZE];
static unsigned char fh_message_row;
static unsigned char fh_net_state;

static void fh_redraw(void);
static void fh_read_status_text(void);
static void fh_show_detail(unsigned int index);
static void fh_draw_status_left(const char *text);
static void fh_update_message_row_text(void);
static void fh_wait_ready_status(const char *text);
static void fh_refresh_network_status(unsigned char force);
static unsigned char fh_record_fits_sd_cache(const ExplorerFHRecord *record);

static unsigned int fh_read_u16(unsigned int addr)
{
    return (unsigned int)Peek(addr) | ((unsigned int)Peek(addr + 1) << 8);
}

static void fh_write_query(const char *query)
{
    unsigned int i;
    unsigned int len = query ? strlen(query) : 0;
    if (len >= CTRL_QUERY_SIZE) {
        len = CTRL_QUERY_SIZE - 1;
    }
    for (i = 0; i < CTRL_QUERY_SIZE; i++) {
        Poke(CTRL_QUERY_BASE + i, i < len ? query[i] : 0);
    }
}

static void fh_write_selected_index(unsigned int index)
{
    Poke(CTRL_QUERY_BASE + 0, (unsigned char)(index & 0xFF));
    Poke(CTRL_QUERY_BASE + 1, (unsigned char)((index >> 8) & 0xFF));
}

static void fh_wait_ready(void)
{
    while (Peek(CTRL_CMD) != 0) {
        delay_ms(20);
    }
}

static void fh_wait_ready_status(const char *text)
{
    unsigned char blink_state = 1;
    unsigned char blink_tick = 0;

    fh_draw_status_left(text);
    while (Peek(CTRL_CMD) != 0) {
        delay_ms(20);
        menu_ui_blink_last_line(text, &blink_state, &blink_tick, 4);
    }
}

static void fh_draw_status_left(const char *text)
{
    menu_ui_print_last_line_text(text);
}

static void fh_clear_line(unsigned char row)
{
    unsigned char i;
    unsigned char width = menu_ui_row_width();
    Locate(0, row);
    for (i = 0; i < width; i++) {
        PrintChar(' ');
    }
}

static void fh_draw_detail_status(const char *text)
{
    fh_clear_line(7);
    Locate(0, 7);
    if (text) {
        if (use_80_columns) {
            printf("%.79s", text);
        } else {
            printf("%.39s", text);
        }
    }
}

static void fh_draw_detail_progress(unsigned int percent)
{
    char text[64];
    char gauge[21];
    unsigned char gauge_index;
    unsigned char gauge_fill;

    if (percent > 100U) {
        percent = 100U;
    }
    gauge_fill = (unsigned char)((percent * 20U + 50U) / 100U);
    for (gauge_index = 0; gauge_index < 20; gauge_index++) {
        gauge[gauge_index] = (gauge_index < gauge_fill) ? '#' : ' ';
    }
    gauge[20] = '\0';
    if (use_80_columns) {
        sprintf(text, "Download Progress: %u%% [%s]", percent, gauge);
    } else {
        sprintf(text, "Progress: %u%% [%s]", percent, gauge);
    }
    fh_draw_detail_status(text);
}

static void fh_wait_download_ready(void)
{
    unsigned int last_percent = 0xFFFFU;
    unsigned char saving_visible = 0;

    while (Peek(CTRL_CMD) != 0) {
        if (Peek(CTRL_FH_RESULT) == EXPLORER_FH_RESULT_SAVING) {
            if (!saving_visible) {
                saving_visible = 1;
                fh_draw_detail_status("Saving to microSD card...");
            }
        } else {
            unsigned int percent = fh_read_u16(CTRL_FH_PROGRESS_L);
            if (percent != last_percent) {
                last_percent = percent;
                fh_draw_detail_progress(percent);
            }
        }
        delay_ms(20);
    }

    while (Peek(CTRL_FH_RESULT) == EXPLORER_FH_RESULT_SAVING) {
        if (!saving_visible) {
            saving_visible = 1;
            fh_draw_detail_status("Saving to microSD card...");
        }
        delay_ms(100);
    }
}

static unsigned char fh_record_fits_sd_cache(const ExplorerFHRecord *record)
{
    return record && record->size_kb <= SD_ROM_MAX_SIZE_KB;
}

static void fh_read_page_records(void)
{
    unsigned int row;
    for (row = 0; row < FILES_PER_PAGE; row++) {
        unsigned char *src = (unsigned char *)(MEMORY_START + (row * CTRL_FH_RECORD_SIZE));
        memcpy(fh_records[row].name, src, EXPLORER_FH_NAME_MAX);
        fh_records[row].name[EXPLORER_FH_NAME_MAX] = '\0';
        fh_records[row].flags = src[CTRL_FH_FLAG_OFFSET];
        fh_records[row].size_kb = (unsigned int)src[CTRL_FH_SIZE_OFFSET] | ((unsigned int)src[CTRL_FH_SIZE_OFFSET + 1] << 8);
        if (!use_80_columns && (fh_records[row].flags & EXPLORER_FH_FLAG_MESSAGE) != 0 && strncmp(fh_records[row].name, "Offline", 7) == 0) {
            strncpy(fh_records[row].name, "Network Offline", EXPLORER_FH_NAME_MAX);
            fh_records[row].name[EXPLORER_FH_NAME_MAX] = '\0';
        }
    }
}

static void fh_update_message_row_text(void)
{
    if (fh_total_files == 1 && (fh_records[0].flags & EXPLORER_FH_FLAG_MESSAGE) != 0 && strncmp(fh_records[0].name, "Offline", 7) == 0) {
        strncpy(fh_records[0].name, menu_ui_status_text("Network Offline", "Network: Offline"), EXPLORER_FH_NAME_MAX);
        fh_records[0].name[EXPLORER_FH_NAME_MAX] = '\0';
    }
}

static void fh_load_page(unsigned int page, const char *status_text)
{
    Poke(CTRL_PAGE, (unsigned char)page);
    Poke(CTRL_CMD, CMD_FH_LIST_PAGE);
    if (status_text) {
        fh_wait_ready_status(status_text);
    } else {
        fh_wait_ready();
    }
    fh_total_files = fh_read_u16(CTRL_COUNT_L);
    fh_total_pages = (fh_total_files + FILES_PER_PAGE - 1) / FILES_PER_PAGE;
    if (fh_total_pages == 0) {
        fh_total_pages = 1;
    }
    fh_read_page_records();
    fh_message_row = (fh_total_files == 1 && (fh_records[0].flags & EXPLORER_FH_FLAG_MESSAGE) != 0);
    fh_update_message_row_text();
}

static void fh_search(const char *query, const char *status_text)
{
    fh_write_query(query);
    Poke(CTRL_CMD, CMD_FH_SEARCH);
    if (status_text) {
        fh_wait_ready_status(status_text);
    } else {
        fh_wait_ready();
    }
    fh_current_page = 0;
    fh_current_index = 0;
    fh_load_page(0, status_text);
}

static void fh_download(unsigned int index)
{
    fh_write_selected_index(index);
    fh_draw_detail_progress(0);
    Poke(CTRL_CMD, CMD_FH_DOWNLOAD);
    fh_wait_download_ready();
}

static void fh_read_status_text(void)
{
    unsigned int i;
    for (i = 0; i + 1 < sizeof(fh_status_right); i++) {
        char ch = *((char *)(CTRL_FH_STATUS_TEXT_BASE + i));
        fh_status_right[i] = ch;
        if (ch == '\0') {
            return;
        }
    }
    fh_status_right[sizeof(fh_status_right) - 1] = '\0';
}

static void fh_size_text(unsigned int size_kb, char *out)
{
    if (size_kb < 1024U) {
        sprintf(out, "%uKB", size_kb);
    } else {
        sprintf(out, "%uMB", (size_kb + 1023U) / 1024U);
    }
}

static void fh_build_row_text_with_name(const ExplorerFHRecord *record, const char *name, char *out, unsigned char width)
{
    char size_text[8];
    unsigned char i;
    unsigned char len;
    fh_size_text(record->size_kb, size_text);
    if (use_80_columns) {
        sprintf(out, MENU_ROW_FORMAT_80, name, size_text, "FH", " ROM");
    } else {
        sprintf(out, MENU_ROW_FORMAT_40, name, size_text, "FH", " ROM");
    }

    len = (unsigned char)strlen(out);
    for (i = len; i < width; i++) {
        out[i] = ' ';
    }
    out[width] = '\0';
}

static void fh_build_row_text(const ExplorerFHRecord *record, char *out, unsigned char width)
{
    fh_build_row_text_with_name(record, record->name, out, width);
}

static void fh_draw_list(void)
{
    unsigned int row;
    char row_text[81];
    unsigned char width = menu_ui_row_width();

    fh_update_message_row_text();
    menu_ui_clear_rows(2, 21);
    for (row = 0; row < FILES_PER_PAGE; row++) {
        unsigned int index = (fh_current_page * FILES_PER_PAGE) + row;
        if (index >= fh_total_files || fh_records[row].name[0] == '\0') {
            continue;
        }
        fh_build_row_text(&fh_records[row], row_text, width);
        menu_ui_render_selectable_line((unsigned char)(row + 2), row_text + 1, !fh_message_row && index == fh_current_index);
    }
}

static void fh_draw_row_for_index(unsigned int index)
{
    unsigned int row;
    char row_text[81];

    if (index >= fh_total_files) {
        return;
    }
    if ((index / FILES_PER_PAGE) != fh_current_page) {
        return;
    }

    row = index % FILES_PER_PAGE;
    if (fh_records[row].name[0] == '\0') {
        return;
    }

    fh_build_row_text(&fh_records[row], row_text, menu_ui_row_width());
    menu_ui_render_selectable_line((unsigned char)(row + 2), row_text + 1, index == fh_current_index);
}

static void fh_position_cursor_on_selection(void)
{
    if (fh_total_files > 0 && !fh_message_row) {
        Locate(0, (unsigned char)((fh_current_index % FILES_PER_PAGE) + 2));
    }
}

// Asks the Pico to re-check the ESP-01 link (it rate limits the probe itself)
// and repaints the indicator only when the reported state changed.
static void fh_refresh_network_status(unsigned char force)
{
    unsigned char state;

    Poke(CTRL_CMD, CMD_FH_WIFI_STATUS);
    fh_wait_ready();
    state = (Peek(CTRL_NET_STATUS) == NET_STATUS_ONLINE);
    if (!force && state == fh_net_state) {
        return;
    }
    fh_net_state = state;
    fh_draw_status_left(menu_ui_status_text(state ? "Net: Online" : "Net: Offline",
                                            state ? "Network: Online" : "Network: Offline"));
    fh_position_cursor_on_selection();
}

static void fh_move_selection(unsigned int new_index)
{
    unsigned int old_index = fh_current_index;
    unsigned int old_page = fh_current_page;

    fh_current_index = new_index;
    fh_current_page = fh_current_index / FILES_PER_PAGE;
    if (fh_current_page != old_page) {
        fh_load_page(fh_current_page, menu_ui_status_text("Loading...", "Loading File Hunter list..."));
        fh_redraw();
        return;
    }

    fh_draw_row_for_index(old_index);
    fh_draw_row_for_index(fh_current_index);
    fh_position_cursor_on_selection();
}

static char fh_wait_key_with_scroll(void)
{
    volatile unsigned int *jiffyPtr = (volatile unsigned int *)JIFFY;
    unsigned int lastTick = *jiffyPtr;
    unsigned int lastNetTick = *jiffyPtr;
    int startPos = 0;
    const unsigned int scrollDelay = 30U;
    char row_text[81];
    char name_window[MAX_FILE_NAME_LENGTH + 1];

    while (1) {
        if (bios_chsns()) {
            return (char)bios_chget();
        }

        if ((unsigned int)(*jiffyPtr - lastNetTick) >= 180U) {
            fh_refresh_network_status(0);
            lastNetTick = *jiffyPtr;
        }

        if (fh_total_files > 0 && !fh_message_row) {
            unsigned int row = fh_current_index % FILES_PER_PAGE;
            const ExplorerFHRecord *record = &fh_records[row];
            size_t len = strlen(record->name);

            if ((use_80_columns ? (len >= name_col_width) : (len > name_col_width)) && (unsigned int)(*jiffyPtr - lastTick) >= scrollDelay) {
                int lenInt = (int)len;
                int attempts = 0;
                int printed = 0;

                while (attempts < lenInt && !printed) {
                    printed = build_sliding_name_window(record->name, startPos, name_window, name_col_width);
                    if (printed) {
                        fh_build_row_text_with_name(record, name_window, row_text, menu_ui_row_width());
                        Locate(0, (unsigned char)(row + 2));
                        PrintChar('>');
                        menu_ui_print_str_inverted_width(row_text + 1, (unsigned char)(menu_ui_row_width() - 3));
                    }
                    startPos++;
                    if (startPos >= lenInt + 1) {
                        startPos = 0;
                    }
                    attempts++;
                }
                lastTick = *jiffyPtr;
            }
        }
    }
}

static void fh_render_frame(void)
{
    unsigned int saved_page = currentPage;
    unsigned int saved_total = totalPages;
    unsigned char saved_selection = menu_shortcut_selection;

    currentPage = (int)(fh_current_page + 1);
    totalPages = (int)fh_total_pages;
    menu_shortcut_selection = MENU_SHORTCUT_FILEHUNTER;
    frame_rendered = 0;
    menu_ui_render_menu_frame();

    currentPage = (int)saved_page;
    totalPages = (int)saved_total;
    menu_shortcut_selection = saved_selection;
}

static void fh_redraw(void)
{
    fh_render_frame();
    fh_draw_list();
    fh_refresh_network_status(1);
}

static int fh_read_search_query(char *buffer, unsigned int max_len)
{
    unsigned int len = 0;
    unsigned char prompt_col = 8;
    char key;

    if (max_len == 0) {
        return 0;
    }
    buffer[0] = '\0';
    if (use_80_columns) {
        fh_draw_status_left("Search: ");
    } else {
        unsigned char col;
        unsigned char content_width = (unsigned char)(menu_ui_row_width() - 2);
        Locate(0, 23);
        printf("Search: ");
        for (col = prompt_col; col < content_width; col++) {
            PrintChar(' ');
        }
    }
    Locate(prompt_col, 23);

    while (1) {
        key = (char)bios_chget();
        if (key == 27) {
            buffer[0] = '\0';
            return 0;
        }
        if (key == 13) {
            buffer[len] = '\0';
            return len > 0;
        }
        if ((key == 8 || key == 127) && len > 0) {
            len--;
            buffer[len] = '\0';
            Locate((unsigned char)(prompt_col + len), 23);
            PrintChar(' ');
            Locate((unsigned char)(prompt_col + len), 23);
        } else if (key >= 32 && key <= 126 && len + 1 < max_len) {
            buffer[len++] = key;
            buffer[len] = '\0';
            PrintChar((unsigned char)key);
        }
    }
}

static void fh_render_detail_footer(void)
{
    Locate(0, 22);
    printf("[ESC - BACK] [ENTER - DOWNLOAD]");

}

static void fh_render_detail_screen(const ExplorerFHRecord *record)
{
    char name[MAX_FILE_NAME_LENGTH + 1];
    char size_text[8];
    unsigned char can_download = fh_record_fits_sd_cache(record);

    menu_ui_render_detail_frame();

    trim_name_to_buffer(record->name, name, MAX_FILE_NAME_LENGTH);
    fh_size_text(record->size_kb, size_text);

    Locate(0, 3);
    if (use_80_columns) {
        printf("    ROM: %-71.71s", name);
    } else {
        printf("    ROM: %-29.29s", name);
    }

    Locate(0, 4);
    printf("   Size: %-8.8s", size_text);
    Locate(0, 5);
    printf(" Source: FH");

    menu_ui_render_selectable_line(9, can_download ? "Action: Download" : "Action: Too large", 1);
    if (can_download) {
        fh_draw_detail_status("");
    } else {
        fh_draw_detail_status("File exceeds 4MB limit!");
    }
    fh_render_detail_footer();
}

static void fh_show_detail(unsigned int index)
{
    unsigned int row;
    ExplorerFHRecord *record;
    char key;

    if (fh_message_row || index >= fh_total_files || (index / FILES_PER_PAGE) != fh_current_page) {
        return;
    }
    row = index % FILES_PER_PAGE;
    record = &fh_records[row];
    if (record->name[0] == '\0' || (record->flags & EXPLORER_FH_FLAG_MESSAGE) != 0) {
        return;
    }

    fh_render_detail_screen(record);

    while (1) {
        key = (char)bios_chget();
        if (key == 27) {
            fh_redraw();
            return;
        }
        if (key == MENU_KEY_F4_CONFIG) {
            launch_wifi_config();
            return;
        }
        if (key == 'h' || key == 'H') {
            helpMenu();
            fh_render_detail_screen(record);
        }
        if (key == 'c' || key == 'C') {
            if (menu_ui_try_toggle_columns()) {
                fh_render_detail_screen(record);
            }
        }
        if (key == 13 || key == ' ') {
            if (!fh_record_fits_sd_cache(record)) {
                fh_draw_detail_status("Download blocked: exceeds 4MB limit!");
                continue;
            }
            fh_render_detail_screen(record);
            fh_download(index);
            if (Peek(CTRL_FH_RESULT)) {
                fh_draw_detail_status("Saved to microSD. Press key.");
            } else {
                fh_read_status_text();
                if (fh_status_right[0] != '\0') {
                    fh_draw_detail_status(fh_status_right);
                } else {
                    fh_draw_detail_status("Download or save failed. Press key.");
                }
            }
            (void)bios_chget();
            fh_redraw();
            return;
        }
    }
}

unsigned char explorer_fh_run(void)
{
    char key;
    char search_query[EXPLORER_FH_MAX_QUERY + 1];
    const char *retrieving_status = menu_ui_status_text("Retrieving...", "Retrieving File Hunter list...");
    const char *page_status = menu_ui_status_text("Loading...", "Loading File Hunter page...");
    const char *search_status = menu_ui_status_text("Searching...", "Searching File Hunter list...");

    fh_current_page = 0;
    fh_total_pages = 1;
    fh_current_index = 0;
    fh_total_files = 0;
    fh_message_row = 0;
    memset(fh_records, 0, sizeof(fh_records));
    memset(fh_status_right, 0, sizeof(fh_status_right));

    fh_render_frame();
    // Empty query = File Hunter latest releases. Issued as a search so the Pico
    // always refetches, instead of reusing a catalog left over from a previous
    // visit (which would otherwise still hold the last search results).
    fh_search("", retrieving_status);
    fh_redraw();

    while (1) {
        key = fh_wait_key_with_scroll();
        switch (key) {
            case '1':
                frame_rendered = 0;
                return SOURCE_MODE_FLASH;
            case '2':
                frame_rendered = 0;
                return SOURCE_MODE_SD;
            case '3':
                break;
            case MENU_KEY_F4_CONFIG:
                launch_wifi_config();
                break;
            case 'h':
            case 'H':
                helpMenu();
                fh_redraw();
                break;
            case 30:
                if (fh_current_index > 0) {
                    fh_move_selection(fh_current_index - 1);
                }
                break;
            case 31:
                if (fh_current_index + 1 < fh_total_files) {
                    fh_move_selection(fh_current_index + 1);
                }
                break;
            case 28:
                if (fh_current_page + 1 < fh_total_pages) {
                    fh_current_page++;
                    fh_current_index = fh_current_page * FILES_PER_PAGE;
                    fh_load_page(fh_current_page, page_status);
                    fh_redraw();
                }
                break;
            case 29:
                if (fh_current_page > 0) {
                    fh_current_page--;
                    fh_current_index = fh_current_page * FILES_PER_PAGE;
                    fh_load_page(fh_current_page, page_status);
                    fh_redraw();
                }
                break;
            case '/':
                if (fh_read_search_query(search_query, sizeof(search_query))) {
                    fh_search(search_query, search_status);
                    fh_redraw();
                } else {
                    fh_redraw();
                }
                break;
            case 13:
            case ' ':
                if (!fh_message_row) {
                    fh_show_detail(fh_current_index);
                }
                break;
            case 'c':
            case 'C':
                if (menu_ui_try_toggle_columns()) {
                    fh_redraw();
                }
                break;
            case 27:
                frame_rendered = 0;
                menu_ui_render_menu_frame();
                return SOURCE_MODE_ALL;
        }
    }
}
