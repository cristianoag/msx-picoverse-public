#include <stdint.h>
#include <string.h>
#include "../audio/emu2413.h"
#include "mp3_alloc_dispatch.h"

#define MENU_DECODER_POOL_SIZE (24u * 1024u + 512u)

void *(*pv_mp3_alloc_fn)(size_t size);
static uint8_t *menu_pool;
static size_t menu_used;

void *pv_mp3_alloc(size_t size)
{
    return pv_mp3_alloc_fn ? pv_mp3_alloc_fn(size) : NULL;
}

static void *menu_alloc(size_t size)
{
    size = (size + 7u) & ~(size_t)7u;
    if (!menu_pool || size > MENU_DECODER_POOL_SIZE - menu_used)
        return NULL;
    void *result = menu_pool + menu_used;
    menu_used += size;
    memset(result, 0, size);
    return result;
}

bool pv_mp3_use_menu_allocator(void)
{
    // Menu/WAVEGAME decoding and MSX-MUSIC are mutually exclusive.
    menu_pool = OPLL_borrow_table_memory(MENU_DECODER_POOL_SIZE);
    menu_used = 0;
    pv_mp3_alloc_fn = menu_alloc;
    return menu_pool != NULL;
}
