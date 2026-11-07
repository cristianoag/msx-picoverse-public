#pragma once

#include <stdbool.h>
#include <stddef.h>

extern void *(*pv_mp3_alloc_fn)(size_t size);
void *pv_mp3_alloc(size_t size);
bool pv_mp3_use_menu_allocator(void);
