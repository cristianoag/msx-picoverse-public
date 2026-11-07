// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// menu_ui.h - Public interface for MSX Explorer menu screen rendering
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef MENU_UI_H
#define MENU_UI_H

#include "menu_state.h"

int menu_ui_supports_80_column_mode(void);
const char *menu_ui_status_text(const char *text_40, const char *text_80);
void menu_ui_set_text_mode(int enable_80);
void menu_ui_init_text_mode(void);
int menu_ui_try_toggle_columns(void);

unsigned char menu_ui_row_width(void);

void menu_ui_clear_rows(unsigned char start_row, unsigned char end_row);
void menu_ui_render_menu_frame(void);
void menu_ui_render_detail_frame(void);
void menu_ui_update_list_footer(void);
void menu_ui_capture_sd_status(void);
void menu_ui_render_footer(const char *options, unsigned char count, unsigned char selected);
void menu_ui_print_footer_line(unsigned char selected);
const char *menu_ui_fh_type_label(void);
void menu_ui_clear_last_line(void);
void menu_ui_print_last_line_text(const char *text);
void menu_ui_print_progress(const char *action, unsigned int percent);
void menu_ui_blink_last_line(const char *text, unsigned char *visible, unsigned char *tick, unsigned char period);
void menu_ui_wait_key_with_blinking_status(const char *text);
int menu_ui_read_line(const char *prompt, char *buffer, unsigned char max_len);
void menu_ui_print_str_inverted_width(const char *str, unsigned char width);
void menu_ui_render_selectable_line(unsigned char row, const char *text, int selected);
// List rows: built in RAM (menu_ui_list_rows, one row per list line) and written
// to the screen in one block transfer.
extern char menu_ui_list_rows[];
void menu_ui_build_row(char *out, const char *name, const char *size_text, const char *source, const char *type, unsigned char selected);
void menu_ui_blit_rows(unsigned char first_row, const char *src, unsigned char rows);
void menu_ui_size_text(char *out, unsigned int value, char unit);

#endif
