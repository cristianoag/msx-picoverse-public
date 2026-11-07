// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// explorer_fh.c - MSX Explorer File Hunter (online ROM/DSK catalog) client
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
// Preserve download results before another command reuses the protocol window.
static char fh_status_text[CTRL_FH_STATUS_TEXT_SIZE];
static unsigned char fh_message_row;
static unsigned char fh_net_state;

static void fh_redraw(void);
static void fh_read_status_text(void);
static void fh_show_detail(unsigned int index);
static void fh_update_message_row_text(void);
static void fh_wait_ready_status(const char *text);
static void fh_refresh_network_status(unsigned char force);

static unsigned int fh_read_u16(unsigned int addr)
{
    return (unsigned int)Peek(addr) | ((unsigned int)Peek(addr + 1) << 8);
}

static void fh_write_query(const char *query)
{
    unsigned int i;
    unsigned int len = query ? strlen(query) : 0;
    if (len >= CTRL_FH_TYPE_OFFSET) {
        len = CTRL_FH_TYPE_OFFSET - 1;
    }
    for (i = 0; i < CTRL_FH_TYPE_OFFSET; i++) {
        Poke(CTRL_QUERY_BASE + i, i < len ? query[i] : 0);
    }
    Poke(CTRL_QUERY_BASE + CTRL_FH_TYPE_OFFSET, fh_catalog_type);
}

static void fh_write_selected_index(unsigned int index)
{
    POKE_U16(CTRL_QUERY_BASE, index);
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

    menu_ui_print_last_line_text(text);
    while (Peek(CTRL_CMD) != 0) {
        delay_ms(20);
        menu_ui_blink_last_line(text, &blink_state, &blink_tick, 4);
    }
}

static void fh_wait_download_ready(void)
{
    unsigned int last_percent = 0xFFFFU;
    unsigned char saving_visible = 0;

    while (Peek(CTRL_CMD) != 0) {
        if (Peek(CTRL_FH_RESULT) == EXPLORER_FH_RESULT_SAVING) {
            if (!saving_visible) {
                saving_visible = 1;
                menu_ui_print_last_line_text("Finalizing microSD file...");
            }
        } else {
            unsigned int percent = fh_read_u16(CTRL_FH_PROGRESS_L);
            if (percent != last_percent) {
                last_percent = percent;
                menu_ui_print_progress("Download", percent);
            }
        }
        delay_ms(20);
    }

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
    menu_ui_print_progress("Download", 0);
    Poke(CTRL_CMD, CMD_FH_DOWNLOAD);
    fh_wait_download_ready();
}

static void fh_read_status_text(void)
{
    unsigned int i;
    for (i = 0; i + 1 < sizeof(fh_status_text); i++) {
        char ch = *((char *)(CTRL_FH_STATUS_TEXT_BASE + i));
        fh_status_text[i] = ch;
        if (ch == '\0') {
            return;
        }
    }
    fh_status_text[sizeof(fh_status_text) - 1] = '\0';
}

static void fh_size_text(unsigned int size_kb, char *out)
{
    if (size_kb < 1024U) {
        menu_ui_size_text(out, size_kb, 'K');
    } else {
        menu_ui_size_text(out, (size_kb + 1023U) / 1024U, 'M');
    }
}

// fh_build_row - Build the screen row of a catalog record in `out`.
static void fh_build_row(const ExplorerFHRecord *record, const char *name, char *out, unsigned char selected)
{
    char size_text[8];

    fh_size_text(record->size_kb, size_text);
    menu_ui_build_row(out, name, size_text, "FH", menu_ui_fh_type_label(), selected);
}

// fh_draw_row - Rebuild one catalog row in its slot of the page buffer and write it to the screen.
static void fh_draw_row(unsigned char row, const ExplorerFHRecord *record, const char *name, unsigned char selected)
{
    char *slot = menu_ui_list_rows + (unsigned int)row * menu_ui_row_width();

    fh_build_row(record, name, slot, selected);
    menu_ui_blit_rows((unsigned char)(row + 2), slot, 1);
}

static void fh_draw_list(void)
{
    unsigned int row;
    unsigned char width = menu_ui_row_width();
    char *out = menu_ui_list_rows;

    fh_update_message_row_text();
    // Whole page in one transfer; rows without a record stay blank.
    for (row = 0; row < FILES_PER_PAGE; row++, out += width) {
        unsigned int index = (fh_current_page * FILES_PER_PAGE) + row;
        if (index >= fh_total_files || fh_records[row].name[0] == '\0') {
            memset(out, ' ', width);
            continue;
        }
        fh_build_row(&fh_records[row], fh_records[row].name, out, !fh_message_row && index == fh_current_index);
    }
    menu_ui_blit_rows(2, menu_ui_list_rows, FILES_PER_PAGE);
}

static void fh_draw_row_for_index(unsigned int index)
{
    unsigned int row;

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

    fh_draw_row((unsigned char)row, &fh_records[row], fh_records[row].name, index == fh_current_index);
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
    menu_ui_clear_last_line();
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
                        fh_draw_row((unsigned char)row, record, name_window, 1);
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
    currentPage = (int)(fh_current_page + 1);
    totalPages = (int)fh_total_pages;
    menu_ui_render_menu_frame();
}

static void fh_redraw(void)
{
    fh_render_frame();
    fh_draw_list();
    fh_refresh_network_status(1);
}

static void fh_render_detail_footer(void)
{
    static const char options[] = "ESC Back\0ENTER Save\0A Folder\0F5 Help";
    menu_ui_render_footer(options, 4, MENU_SHORTCUT_NONE);
}

static void fh_render_detail_screen(const ExplorerFHRecord *record)
{
    char name[MAX_FILE_NAME_LENGTH + 1];
    char size_text[8];

    menu_ui_render_detail_frame();

    trim_name_to_buffer(record->name, name, MAX_FILE_NAME_LENGTH);
    fh_size_text(record->size_kb, size_text);

    Locate(0, 3);
    if (use_80_columns) {
        printf("   %s: %-71.71s", menu_ui_fh_type_label(), name);
    } else {
        printf("   %s: %-29.29s", menu_ui_fh_type_label(), name);
    }

    Locate(0, 4);
    printf("   Size: %-8.8s", size_text);
    Locate(0, 5);
    printf(" Source: FH");
    // The Pico puts the download folder (end of the path when long) in the status text.
    Poke(CTRL_CMD, CMD_FH_DOWNLOAD_DIR);
    fh_wait_ready();
    fh_read_status_text();
    Locate(0, 6);
    printf(" Folder: %s", fh_status_text);

    menu_ui_render_selectable_line(9, "Action: Download", 1);
    menu_ui_print_last_line_text("");
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
        key = (char)bios_chget_quiet();
        if (key == 27) {
            fh_redraw();
            return;
        }
        if (key == 'a' || key == 'A') {
            // Leave File Hunter for the microSD list to pick the download folder.
            menu_pick_download = 1;
            return;
        }
        if (key == MENU_KEY_F5_HELP) {
            helpMenu(HELP_PAGE_FILEHUNTER);
            if (menu_pending_key) {
                return;
            }
            fh_render_detail_screen(record);
        }
        if (key == 'c' || key == 'C') {
            if (menu_ui_try_toggle_columns()) {
                fh_render_detail_screen(record);
            }
        }
        if (key == 13 || key == ' ') {
            fh_download(index);
            fh_read_status_text();
            if (fh_status_text[0] != '\0') {
                menu_ui_wait_key_with_blinking_status(fh_status_text);
            } else {
                menu_ui_wait_key_with_blinking_status(Peek(CTRL_FH_RESULT) ?
                    "Saved to microSD. Press key." : "Download/save failed. Press key.");
            }
            fh_redraw();
            return;
        }
    }
}

// Where File Hunter was when the user left it to pick the download folder, so
// it can open on the same results instead of fetching the catalog again.
static char fh_resume_query[EXPLORER_FH_MAX_QUERY + 1];
static unsigned int fh_resume_page;
static unsigned int fh_resume_index;

// explorer_fh_run - File Hunter. resume is set when coming back from picking the
// download folder: the Pico still holds the catalog, so only the page is reloaded.
unsigned char explorer_fh_run(unsigned char resume)
{
    char key;
    char search_query[EXPLORER_FH_MAX_QUERY + 1];
    char active_query[EXPLORER_FH_MAX_QUERY + 1];
    const char *retrieving_status = menu_ui_status_text("Retrieving...", "Retrieving File Hunter list...");
    const char *page_status = menu_ui_status_text("Loading...", "Loading File Hunter page...");
    const char *search_status = menu_ui_status_text("Searching...", "Searching File Hunter list...");

    fh_current_page = 0;
    fh_total_pages = 1;
    fh_current_index = 0;
    fh_total_files = 0;
    fh_message_row = 0;
    menu_shortcut_selection = MENU_SHORTCUT_FILEHUNTER;
    active_query[0] = '\0';
    memset(fh_records, 0, sizeof(fh_records));
    memset(fh_status_text, 0, sizeof(fh_status_text));

    fh_render_frame();
    if (resume) {
        strcpy(active_query, fh_resume_query);
        fh_current_page = fh_resume_page;
        fh_current_index = fh_resume_index;
        fh_load_page(fh_current_page, retrieving_status);
    } else {
        // Empty query = File Hunter latest releases. Issued as a search so the Pico
        // always refetches, instead of reusing a catalog left over from a previous
        // visit (which would otherwise still hold the last search results).
        fh_search(active_query, retrieving_status);
    }
    fh_redraw();

    while (1) {
        key = menu_pending_key;
        menu_pending_key = 0;
        if (!key) {
            key = fh_wait_key_with_scroll();
        }
        switch (key) {
            case '1':
                frame_rendered = 0;
                return SOURCE_MODE_FLASH;
            case '2':
                frame_rendered = 0;
                return SOURCE_MODE_SD;
            case '3':
                if (!frame_rendered) {
                    fh_redraw();
                }
                break;
            case MENU_KEY_F4_CONFIG:
                launch_wifi_config();
                break;
            case MENU_KEY_F5_HELP:
                helpMenu(HELP_PAGE_FILEHUNTER);
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
            {
                int search_result;
                search_query[0] = '\0';
                search_result = menu_ui_read_line("Search: ", search_query, EXPLORER_FH_MAX_QUERY);
                if (search_result == -1) {
                    launch_wifi_config();
                } else if (search_result == -2) {
                    helpMenu(HELP_PAGE_FILEHUNTER);
                } else if (search_result > 0 && search_query[0]) {
                    strcpy(active_query, search_query);
                    fh_search(active_query, search_status);
                }
                fh_redraw();
                break;
            }
            case 't':
            case 'T':
                // Switch between the ROM and DSK catalogs, keeping the current search.
                fh_catalog_type ^= CTRL_FH_TYPE_DSK;
                fh_render_frame();
                fh_search(active_query, retrieving_status);
                fh_redraw();
                break;
            case 'a':
            case 'A':
                strcpy(fh_resume_query, active_query);
                fh_resume_page = fh_current_page;
                fh_resume_index = fh_current_index;
                menu_pick_download = 1;
                frame_rendered = 0;
                return SOURCE_MODE_SD;
            case 13:
            case ' ':
                if (!fh_message_row) {
                    fh_show_detail(fh_current_index);
                    if (menu_pick_download) {
                        // A on the detail screen: pick the download folder, then come back here.
                        strcpy(fh_resume_query, active_query);
                        fh_resume_page = fh_current_page;
                        fh_resume_index = fh_current_index;
                        frame_rendered = 0;
                        return SOURCE_MODE_SD;
                    }
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
                return SOURCE_MODE_ALL;
        }
    }
}
