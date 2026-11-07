// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// menu_state.h - Public interface for shared MSX Explorer menu state
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef MENU_STATE_H
#define MENU_STATE_H

#define SCREEN_WIDTH 40

#define MENU_SHORTCUT_NONE       0
#define MENU_SHORTCUT_FLASH      1
#define MENU_SHORTCUT_MICROSD    2
#define MENU_SHORTCUT_FILEHUNTER 3
#define MENU_SHORTCUT_HELP       5

extern int paging_enabled;
extern int use_80_columns;
extern unsigned char name_col_width;
extern int frame_rendered;
extern unsigned char menu_shortcut_selection;
// Help queues a source/configuration key so nested detail screens unwind first.
extern unsigned char menu_pending_key;
// A file is marked with M and waits for a destination folder.
extern unsigned char menu_move_pending;
// File Hunter asked for its download folder: the microSD list is open to pick one with A.
extern unsigned char menu_pick_download;
extern unsigned char fh_catalog_type; // File Hunter catalog: CTRL_FH_TYPE_ROM or CTRL_FH_TYPE_DSK

#endif
