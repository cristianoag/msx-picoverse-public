// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// menu_ui.c - MSX Explorer menu screen rendering and text-mode handling
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/
//
// Rows are zero-based: row 22 holds options and row 23 holds status/input.
// Complete-row VRAM transfers avoid CHPUT scrolling when the last row fills.

#include <string.h>
#include <stdint.h>
#include "menu_ui.h"
#include "menu.h"
#include "menu_input.h"

__at (BIOS_HWVER) unsigned char msx_version;
__at (BIOS_LINL40) unsigned char text_columns;
__at (BIOS_RG4SAV) unsigned char vdp_reg4;

#define MENU_UI_HLINE_SOURCE_CHAR 0x17
#define MENU_UI_HLINE_PRINT_CHAR  0x7E

static unsigned char menu_ui_listing; // Detail/help screens omit list metadata.
static char menu_ui_sd_text[32];

// Clear [start_row, end_row). Naked wrappers read SDCC stack arguments after
// the return address; CALSLT reaches the BIOS slot and IX must be preserved.
static void clear_rows_vram(unsigned char start_row, unsigned char end_row, unsigned char width) __naked
{
    (void)start_row;
    (void)end_row;
    (void)width;
    __asm
    ld      hl, #2
    add     hl, sp
    ld      e, (hl)
    inc     hl
    ld      d, (hl)
    inc     hl
    ld      c, (hl)

    ld      a, d
    sub     e
    jr      z, clear_rows_done
    ld      b, a

    ld      hl, (#BIOS_TXTNAM)
    ld      a, e
    ld      e, c
    ld      d, #0
    or      a
    jr      z, clear_rows_calc_done

clear_rows_calc_loop:
    add     hl, de
    dec     a
    jr      nz, clear_rows_calc_loop

clear_rows_calc_done:
clear_rows_loop:
    push    bc
    push    de
    push    hl
    ld      a, #32
    ld      b, #0
    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_FILVRM
    call    BIOS_CALSLT
    pop     ix
    pop     hl
    pop     de
    pop     bc
    add     hl, de
    djnz    clear_rows_loop

clear_rows_done:
    ret
    __endasm;
}

// blit_row_vram - Copy `width` bytes from a RAM buffer straight into the text
// name table at the given row using BIOS LDIRVM, replacing per-character CHPUT. 
static void blit_row_vram(unsigned char row, const char *src, unsigned char width) __naked
{
    (void)row;
    (void)src;
    (void)width;
    __asm
    ld      hl, #2
    add     hl, sp
    ld      b, (hl)        ; B = row
    inc     hl
    ld      e, (hl)        ; src low
    inc     hl
    ld      d, (hl)        ; DE = src
    inc     hl
    ld      c, (hl)        ; C = width

    push    de             ; save src
    push    bc             ; save row(B) + width(C)

    ; dest = TXTNAM + row * width
    ld      l, c           ; HL = width
    ld      h, #0
    ex      de, hl         ; DE = width
    ld      hl, #0
    ld      a, b           ; A = row
    or      a
    jr      z, blit_addr_done

blit_addr_loop:
    add     hl, de
    dec     a
    jr      nz, blit_addr_loop

blit_addr_done:
    ld      de, (#BIOS_TXTNAM)
    add     hl, de         ; HL = dest VRAM address
    ex      de, hl         ; DE = dest VRAM

    pop     bc             ; B = row, C = width
    ld      b, #0          ; BC = width (length)
    pop     hl             ; HL = src

    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_LDIRVM
    call    BIOS_CALSLT
    pop     ix
    ret
    __endasm;
}

int menu_ui_supports_80_column_mode(void) {
    return msx_version >= 1;
}

const char *menu_ui_status_text(const char *text_40, const char *text_80) {
    return use_80_columns ? text_80 : text_40;
}

static void menu_ui_copy_char_pattern(unsigned char source_char, unsigned char target_char)
{
    unsigned int base = ((unsigned int)vdp_reg4) << 11;
    unsigned int source_address = base + ((unsigned int)source_char * 8);
    unsigned int target_address = base + ((unsigned int)target_char * 8);

    for (unsigned char i = 0; i < 8; i++) {
        Vpoke(target_address + i, Vpeek(source_address + i));
    }
}

void menu_ui_set_text_mode(int enable_80) {
    if (enable_80) {
        text_columns = 80;
        use_80_columns = 1;
        name_col_width = NAME_COL_WIDTH_80;
    } else {
        text_columns = 40;
        use_80_columns = 0;
        name_col_width = NAME_COL_WIDTH;
    }
    __asm
    ld     iy,(#BIOS_EXPTBL-1)
    push   ix
    ld     ix,#BIOS_INITXT
    call   BIOS_CALSLT
    pop    ix
    __endasm;

    // Copy the BIOS line glyph to a printable character for frame delimiters.
    menu_ui_copy_char_pattern(MENU_UI_HLINE_SOURCE_CHAR, MENU_UI_HLINE_PRINT_CHAR);
}

void menu_ui_init_text_mode(void) {
    int enable_80 = (!MENU_FORCE_40_COLUMNS && menu_ui_supports_80_column_mode());
    menu_ui_set_text_mode(enable_80);
}

void invert_chars(unsigned char startChar, unsigned char endChar)
{
    unsigned int srcAddress, dstAddress;
    unsigned char patternByte;
    unsigned char i, c;
    unsigned int base = ((unsigned int)vdp_reg4) << 11;

    // Text mode has no per-character inverse attribute. Store complemented
    // glyphs 96 character codes higher; renderers select them by adding 96.
    for (c = startChar; c <= endChar; c++)
    {
        srcAddress  = base + ((unsigned int)c * 8);
        dstAddress = srcAddress + (96 * 8);

        for (i = 0; i < 8; i++)
        {
            patternByte = Vpeek(srcAddress + i);
            patternByte = ~patternByte;
            Vpoke(dstAddress  + i, patternByte);
        }
    }
}

int menu_ui_try_toggle_columns(void) {
    int target_80 = !use_80_columns;
    if (target_80) {
        if (MENU_FORCE_40_COLUMNS) {
            menu_ui_wait_key_with_blinking_status("80-col off");
            menu_ui_clear_last_line();
            return 0;
        }
        if (!menu_ui_supports_80_column_mode()) {
            menu_ui_wait_key_with_blinking_status("No 80-column");
            menu_ui_clear_last_line();
            return 0;
        }
    }
    menu_ui_set_text_mode(target_80);
    invert_chars(32, 126);
    return 1;
}

unsigned char menu_ui_row_width(void) {
    return (unsigned char)(use_80_columns ? 80 : SCREEN_WIDTH);
}

void menu_ui_clear_rows(unsigned char start_row, unsigned char end_row) {
    if (start_row >= end_row) {
        return;
    }
    clear_rows_vram(start_row, end_row, menu_ui_row_width());
}

static void menu_ui_print_title_line(void) {
    if (use_80_columns) {
        printf("MSX PICOVERSE 2350%44s[EXPLORER %s]", "", EXPLORER_VERSION);
    } else {
        printf("MSX PICOVERSE 2350%4s[EXPLORER %s]", "", EXPLORER_VERSION);
    }
}

static void menu_ui_print_delimiter_line(void) {
    unsigned char width = use_80_columns ? 78 : 38;
    for (unsigned char i = 0; i < width; i++) {
        PrintChar(MENU_UI_HLINE_PRINT_CHAR);
    }
}

// File Hunter catalog label (" ROM"/" DSK"): list type column, detail screen
// name label and the list footer.
const char *menu_ui_fh_type_label(void) {
    return fh_catalog_type == CTRL_FH_TYPE_DSK ? " DSK" : " ROM";
}

void menu_ui_print_footer_line(unsigned char selected) {
    static const char options_40[] = "F1 FL\0F2 SD\0F3 FH\0F4 WIFI\0F5 HELP";
    static const char options_80[] = "F1 FLASH\0F2 SD CARD\0F3 FILE HUNTER\0F4 WIFI\0F5 HELP";
    menu_ui_render_footer(use_80_columns ? options_80 : options_40, 5, selected);
}

// NUL-separated ROM labels avoid pointer tables. Callers supply at least two
// labels that fit with one blank between them; host tests check these tables.
void menu_ui_render_footer(const char *options, unsigned char count, unsigned char selected) {
    unsigned char width = menu_ui_row_width();
    unsigned char inset = use_80_columns ? 1 : 0;
    unsigned char bracket_width = inset * 2;
    // Keep two screen columns unused, plus a blank at each end for highlighting.
    unsigned char space = width - 4;
    const char *label = options;
    for (unsigned char item = 0; item < count; item++) {
        unsigned char label_len = (unsigned char)strlen(label);
        space -= label_len + bracket_width;
        label += label_len + 1;
    }
    unsigned char gaps = count - 1;
    unsigned char gap_width = space / gaps;
    // Give remainder columns to the first gaps; widths differ by at most one.
    unsigned char extra = space - gap_width * gaps;
    unsigned char start = 1;
    char row[80];

    memset(row, ' ', width);
    for (unsigned char i = 0; i < count; i++) {
        unsigned char len = (unsigned char)strlen(options);
        if (inset) {
            row[start] = '[';
            row[start + len + 1] = ']';
        }
        memcpy(row + start + inset, options, len);
        options += len + 1;
        len += bracket_width;
        if (selected == (unsigned char)(i + 1)) {
            char *highlight = row + start - 1;
            for (unsigned char remaining = len + 2; remaining; remaining--) {
                *highlight = (char)(*highlight + 96);
                highlight++;
            }
        }
        start += len + gap_width;
        if (i < extra) {
            start++;
        }
    }
    blit_row_vram(22, row, width);
}

// Capture the browsing summary before ROM options/File Hunter reuse the window.
// The last parenthesis starts free space even if the partition label has "(...)".
void menu_ui_capture_sd_status(void) {
    const char *info = (const char *)(CTRL_SD_PARTITION_INFO_BASE + 2);
    const char *free_text = 0;
    unsigned char len = 0;
    unsigned char whole_len;

    while (*info) {
        if (*info == '(') {
            free_text = info + 1;
        }
        info++;
    }
    while (free_text && len < 10 && free_text[len] >= '0' && free_text[len] <= '9') {
        len++;
    }
    whole_len = len;
    if (len && free_text[len] == '.' && free_text[len + 1] >= '0' && free_text[len + 1] <= '9') {
        len += 2;
    }
    if (Peek(CTRL_STATUS) == CTRL_STATUS_SD_MISSING) {
        strcpy(menu_ui_sd_text, "No microSD card");
    } else if (!whole_len || strncmp(free_text + len, "MB FREE)", 8)) {
        strcpy(menu_ui_sd_text, "Available: unknown");
    } else {
        strcpy(menu_ui_sd_text, "Available: ");
        memcpy(menu_ui_sd_text + 11, free_text, len);
        strcpy(menu_ui_sd_text + 11 + len, len == whole_len ? ".0 MB" : " MB");
    }
}

static void menu_ui_render_last_line(const char *text) {
    unsigned char width = menu_ui_row_width();
    unsigned char col = 0;
    unsigned char summary_len = 0;
    const char *summary = 0;
    char row[81];
    static char summary_text[32];

    if (menu_ui_listing) {
        if (!text && menu_shortcut_selection == MENU_SHORTCUT_MICROSD) {
            if (menu_move_pending) {
                text = "M: move here";
            } else if (menu_pick_download) {
                text = "A: set download folder";
            }
        }
        sprintf(row, "Page %02d/%02d", currentPage, totalPages);
        col = (unsigned char)strlen(row);
        if (menu_shortcut_selection == MENU_SHORTCUT_FLASH) {
            unsigned int free_kb = *(volatile unsigned int *)CTRL_FLASH_FREE_KB;
            if (free_kb == 0xFFFFU) {
                strcpy(summary_text, "Flash list full");
            } else {
                // Fixed-point MB tenths round down without linking float support.
                sprintf(summary_text, "Available: %u.%u MB", free_kb >> 10, ((free_kb & 1023U) * 10U) >> 10);
            }
            summary = summary_text;
        } else if (menu_shortcut_selection == MENU_SHORTCUT_MICROSD) {
            summary = menu_ui_sd_text;
        } else if (menu_shortcut_selection == MENU_SHORTCUT_FILEHUNTER) {
            sprintf(summary_text, "Listing:%s", menu_ui_fh_type_label());
            summary = summary_text;
        }
        summary_len = summary ? (unsigned char)strlen(summary) : 0;
        // NULL requests normal status; "" hides only the blinking message.
        if (!text && menu_shortcut_selection == MENU_SHORTCUT_FILEHUNTER) {
            const char *network = Peek(CTRL_NET_STATUS) == NET_STATUS_ONLINE ? "Net: Online" : "Net: Offline";
            if (col + summary_len + strlen(network) + 6 <= width) {
                text = network;
            }
        }
        // A message takes priority over source metadata, never the page prefix.
        // Six columns account for the separators on both sides of the summary.
        if (summary_len && (!text || !*text || col + summary_len + strlen(text) + 6 <= width)) {
            memcpy(row + col, " | ", 3);
            col += 3;
            for (unsigned char i = 0; i < summary_len && col < width; i++) {
                row[col++] = summary[i];
            }
        }
    }
    if (text && *text) {
        if (col && col + 3 <= width) {
            memcpy(row + col, " | ", 3);
            col += 3;
        }
        while (*text && col < width) {
            row[col++] = *text++;
        }
    }
    unsigned char padding = (width - col) / 2;
    // memmove handles overlap while shifting the composed text into the center.
    memmove(row + padding, row, col);
    memset(row, ' ', padding);
    memset(row + padding + col, ' ', width - padding - col);
    blit_row_vram(23, row, width);
}

static void menu_ui_render_frame(unsigned char listing) {
    menu_ui_listing = listing;
    if (!listing) {
        menu_ui_clear_rows(2, 23);
    }
    Locate(0, 0);
    menu_ui_print_title_line();
    Locate(0, 1);
    menu_ui_print_delimiter_line();
    Locate(0, 21);
    menu_ui_print_delimiter_line();
    if (listing) {
        menu_ui_print_footer_line(menu_shortcut_selection);
    }
    menu_ui_render_last_line(0);
}

void menu_ui_render_menu_frame(void) {
    menu_ui_render_frame(1);
    frame_rendered = 1;
}

void menu_ui_render_detail_frame(void) {
    menu_ui_render_frame(0);
}

void menu_ui_update_list_footer(void) {
    menu_ui_print_footer_line(menu_shortcut_selection);
    menu_ui_clear_last_line();
}

void menu_ui_clear_last_line(void) {
    menu_ui_render_last_line(0);
}

void menu_ui_print_last_line_text(const char *text) {
    menu_ui_render_last_line(text);
}

void menu_ui_print_progress(const char *action, unsigned int percent) {
    char text[32];
    sprintf(text, "%s: %u%%", action, percent);
    menu_ui_print_last_line_text(text);
}

void menu_ui_blink_last_line(const char *text, unsigned char *visible, unsigned char *tick, unsigned char period) {
    if (++(*tick) >= period) {
        *tick = 0;
        *visible = !(*visible);
        if (*visible) {
            menu_ui_print_last_line_text(text);
        } else {
            menu_ui_render_last_line("");
        }
    }
}

void menu_ui_wait_key_with_blinking_status(const char *text) {
    unsigned char visible = 1;
    unsigned char tick = 0;

    menu_ui_print_last_line_text(text);
    while (!bios_chsns()) {
        delay_ms(20);
        menu_ui_blink_last_line(text, &visible, &tick, 8);
    }
    (void)bios_chget();
}

// menu_ui_read_line - Edit buffer (its current text is the starting value) after
// prompt on the last line. buffer must hold max_len + 1 bytes; the field is
// also limited to the room left on the line, which can shorten the starting
// text. Returns 1 on Enter after an edit, 2 on Enter with the text untouched,
// 0 on ESC, -1 on F4 and -2 on F5.
int menu_ui_read_line(const char *prompt, char *buffer, unsigned char max_len)
{
    unsigned char width = (unsigned char)(menu_ui_row_width() - 2);
    unsigned char col = (unsigned char)strlen(prompt);
    unsigned char len;
    unsigned char i;
    unsigned char edited = 0;
    char ch;
    int result = 0;

    if (max_len > (unsigned char)(width - col)) {
        max_len = (unsigned char)(width - col);
    }
    buffer[max_len] = '\0';
    // A shortened initial value is not an edit; Enter must not rename it.
    len = (unsigned char)strlen(buffer);
    menu_ui_clear_rows(23, 24);
    Locate(0, 23);
    printf("%s%s", prompt, buffer);
    for (i = (unsigned char)(col + len); i < width; i++) {
        PrintChar(' ');
    }

    while (1) {
        Locate((unsigned char)(col + len), 23);
        ch = (char)bios_chget();
        if (ch == 13) {
            result = edited ? 1 : 2;
            break;
        }
        if (ch == 27) {
            break;
        }
        if (ch == MENU_KEY_F4_CONFIG || ch == MENU_KEY_F5_HELP) {
            result = ch == MENU_KEY_F4_CONFIG ? -1 : -2;
            break;
        }
        if (ch == 8 || ch == 127) {
            if (len > 0) {
                buffer[--len] = '\0';
                Locate((unsigned char)(col + len), 23);
                PrintChar(' ');
                edited = 1;
            }
        } else if (ch >= 32 && ch <= 126 && len < max_len) {
            PrintChar((unsigned char)ch);
            buffer[len++] = ch;
            buffer[len] = '\0';
            edited = 1;
        }
    }
    menu_ui_clear_last_line();
    return result;
}

void menu_ui_print_str_inverted_width(const char *str, unsigned char width)
{
    if (width == 0) {
        return;
    }
    unsigned char capped_width = width > 80 ? 80 : width;
    unsigned char i = 0;

    while (i < capped_width && str[i]) {
        PrintChar((unsigned char)(str[i] + 96));
        i++;
    }
    while (i < capped_width) {
        PrintChar((unsigned char)(' ' + 96));
        i++;
    }
}

void menu_ui_render_selectable_line(unsigned char row, const char *text, int selected) {
    unsigned char width = menu_ui_row_width();
    char buffer[81];
    unsigned char len = 0;
    unsigned char content_width;
    unsigned char selected_width;
    unsigned char i;

    if (width > 80) {
        width = 80;
    }
    content_width = width > 1 ? (unsigned char)(width - 1) : 0;
    selected_width = width > 3 ? (unsigned char)(width - 3) : content_width;

    if (text) {
        len = (unsigned char)strlen(text);
        if (len > content_width) {
            len = content_width;
        }
    }

    // Build the full row (selection marker + content) in RAM, applying the
    // inverted-glyph offset (+96) for the selected row, then write it to VRAM
    // in a single block transfer instead of one CHPUT per column.
    buffer[0] = selected ? '>' : ' ';
    if (selected) {
        for (i = 0; i < selected_width; i++) {
            unsigned char ch = (i < len) ? (unsigned char)text[i] : ' ';
            buffer[1 + i] = (char)(ch + 96);
        }
        for (i = selected_width; i < content_width; i++) {
            buffer[1 + i] = ' ';
        }
    } else {
        for (i = 0; i < content_width; i++) {
            buffer[1 + i] = (i < len) ? text[i] : ' ';
        }
    }

    blit_row_vram(row, buffer, width);
}

// blit_vram - Copy `length` bytes from RAM to VRAM address `dest` with the BIOS
// LDIRVM, in one call.
static void blit_vram(unsigned int dest, const char *src, unsigned int length) __naked
{
    (void)dest;
    (void)src;
    (void)length;
    __asm
    ld      hl, #2
    add     hl, sp
    ld      e, (hl)
    inc     hl
    ld      d, (hl)        ; DE = dest
    inc     hl
    ld      c, (hl)
    inc     hl
    ld      b, (hl)        ; BC = src (temporarily)
    inc     hl
    ld      a, (hl)
    inc     hl
    ld      h, (hl)
    ld      l, a           ; HL = length
    push    bc
    ld      b, h
    ld      c, l           ; BC = length
    pop     hl             ; HL = src

    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_LDIRVM
    call    BIOS_CALSLT
    pop     ix
    ret
    __endasm;
}

// menu_ui_blit_rows - Write `rows` screen rows, already laid out in RAM at the
// current row width, to the text screen starting at `first_row`. One block
// transfer paints a whole list page at once.
void menu_ui_blit_rows(unsigned char first_row, const char *src, unsigned char rows) {
    unsigned char width = menu_ui_row_width();
    blit_vram((unsigned int)(*(volatile unsigned int *)BIOS_TXTNAM + (unsigned int)first_row * width),
              src, (unsigned int)rows * width);
}

// The rows of the list on screen (rows 2 to 20), kept in RAM so that a page is
// built once and written to VRAM in a single transfer, and a single row can be
// rebuilt and redrawn without touching the others.
char menu_ui_list_rows[FILES_PER_PAGE * 80];

// menu_ui_build_row - Build one list row straight into its final screen form:
// the selection marker, the name (truncated or padded to the name column), the
// size right-aligned in 6 columns, the 2-column source and the 5-column type.
// A selected row has its content in the inverted glyphs (+96), as
// menu_ui_render_selectable_line() does. The layout matches the old
// " %-61.61s %6s %-2s %-5s" (80 columns) / " %-21.21s ..." (40 columns) rows.
void menu_ui_build_row(char *out, const char *name, const char *size_text, const char *source, const char *type, unsigned char selected) {
    unsigned char width = menu_ui_row_width();
    unsigned char name_width = use_80_columns ? 61 : 21;
    unsigned char length;
    unsigned char i;
    char *field = out + name_width + 2;

    memset(out, ' ', width);
    for (i = 0; i < name_width && name[i]; i++) {
        out[1 + i] = name[i];
    }
    length = (unsigned char)strlen(size_text);
    if (length > 6) {
        length = 6;
    }
    memcpy(field + 6 - length, size_text, length);
    field += 7;
    field[0] = source[0];
    field[1] = source[1];
    field += 3;
    while (*type) {
        *field++ = *type++;
    }
    if (selected) {
        out[0] = '>';
        for (i = 1; i <= width - 3; i++) {
            out[i] += 96;
        }
    }
}

// menu_ui_size_text - "<value>KB" or "<value>MB" without printf.
void menu_ui_size_text(char *out, unsigned int value, char unit) {
    static const unsigned int powers[] = {10000u, 1000u, 100u, 10u};
    unsigned char started = 0;
    unsigned char i;

    for (i = 0; i < 4; i++) {
        char digit = '0';
        while (value >= powers[i]) {
            value -= powers[i];
            digit++;
        }
        if (digit != '0' || started) {
            *out++ = digit;
            started = 1;
        }
    }
    *out++ = (char)('0' + value);
    *out++ = unit;
    *out++ = 'B';
    *out = '\0';
}
