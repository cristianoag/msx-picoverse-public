// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// menu_state.c - Shared MSX Explorer menu state (columns, paging, shortcuts)
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include "menu.h"
#include "menu_state.h"

int paging_enabled;
int use_80_columns;
unsigned char name_col_width;
int frame_rendered;
unsigned char menu_shortcut_selection;
unsigned char fh_catalog_type;
