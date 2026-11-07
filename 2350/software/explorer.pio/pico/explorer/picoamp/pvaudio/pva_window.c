// MSX PICOVERSE PROJECT
// pva_window.c - PicoVerse Audio Engine: register window (core 0), arena, log.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pva_internal.h"

volatile uint8_t *pva_win;              // in the arena (SRAM), allocated first
pva_mailbox_t pva_mailbox;

// ---------------------------------------------------------------- core 0
uint8_t PVA_RAM_FUNC(pva_mem_read)(uint16_t addr)
{
    uint16_t rel = (uint16_t)(addr - PVA_WIN_BASE);
    if (rel >= PVA_WIN_SIZE)
        return 0xFFu;
    return pva_win[rel];
}

void PVA_RAM_FUNC(pva_mem_write)(uint16_t addr, uint8_t data)
{
    uint16_t rel = (uint16_t)(addr - PVA_WIN_BASE);
    if (rel >= PVA_R_ARGS && rel < PVA_R_ARGS + PVA_ARGS_SIZE)
        pva_win[rel] = data;
    else if (rel == PVA_R_CMD_TOKEN)
        pva_win[rel] = data;
    else if (rel == PVA_R_CMD_CODE)
    {
        pva_mailbox.token = pva_win[PVA_R_CMD_TOKEN];
        pva_mailbox.code = data;
        pva_barrier();
        pva_mailbox.post_count = pva_mailbox.post_count + 1u;
    }
}

// ---------------------------------------------------------------- arena
static uint8_t *arena_base;
static size_t arena_size, arena_used;

void *pva_alloc(size_t size)
{
    size = (size + 3u) & ~(size_t)3u;
    if (!arena_base || arena_used + size > arena_size)
        return NULL;
    void *p = arena_base + arena_used;
    arena_used += size;
    memset(p, 0, size);
    return p;
}

size_t pva_arena_free(void) { return arena_size - arena_used; }
size_t pva_arena_size(void) { return arena_size; }

// ---------------------------------------------------------------- log
#define LOG_LINES   32
#define LOG_LEN     56
static char (*log_buf)[LOG_LEN];
static uint16_t log_total;

void pva_log(const char *fmt, ...)
{
    if (!log_buf)
        return;
    char *line = log_buf[log_total % LOG_LINES];
    uint32_t t = pva_hal_millis();
    int n = snprintf(line, LOG_LEN, "%5lu.%01lu ", (unsigned long)(t / 1000u), (unsigned long)((t / 100u) % 10u));
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, LOG_LEN - (size_t)n, fmt, ap);
    va_end(ap);
    log_total++;
#ifdef PVA_HOST_LOG
    printf("[pva] %s\n", line);
#endif
}

size_t pva_log_dump(char *out, size_t size)
{
    size_t pos = 0;
    uint16_t first = log_total > LOG_LINES ? (uint16_t)(log_total - LOG_LINES) : 0;
    for (uint16_t i = first; i < log_total && log_buf; i++)
    {
        const char *line = log_buf[i % LOG_LINES];
        size_t l = strlen(line);
        if (pos + l + 2 > size)
            break;
        memcpy(out + pos, line, l);
        pos += l;
        out[pos++] = 13;
        out[pos++] = 10;
    }
    return pos;
}

uint16_t pva_log_count(void) { return log_total; }

// the newest log lines that fit in 'size' (oldest of them first), each 0-terminated
size_t pva_log_tail(char *out, size_t size)
{
    if (!log_buf || !size) return 0;
    uint16_t first = log_total > LOG_LINES ? (uint16_t)(log_total - LOG_LINES) : 0, from = log_total;
    size_t need = 0;
    while (from > first)
    {
        size_t l = strlen(log_buf[(from - 1u) % LOG_LINES]) + 1u;
        if (need + l > size) break;
        need += l;
        from--;
    }
    size_t pos = 0;
    for (uint16_t i = from; i < log_total; i++)
    {
        const char *line = log_buf[i % LOG_LINES];
        size_t l = strlen(line) + 1u;
        memcpy(out + pos, line, l);
        pos += l;
    }
    return pos;
}

void pva_init(uint8_t *arena, size_t size)
{
    arena_base = arena;
    arena_size = size;
    arena_used = 0;
    pva_win = pva_alloc(PVA_WIN_SIZE);
    memset((void *)pva_win, 0xFF, PVA_WIN_SIZE);
    memset((void *)&pva_win[PVA_R_ID], 0, 0x100);
    memset((void *)&pva_win[PVA_R_FAULT_TEXT], 0, 64);
    memset((void *)&pva_win[PVA_R_VIZ], 0, 0x100);
    memcpy((void *)&pva_win[PVA_R_ID], "PVAUDIO", 7);
    pva_win[PVA_R_ID + 8] = PVA_API_VERSION;
    pva_win[PVA_R_ID + 9] = PVA_FW_MAJOR;
    pva_win[PVA_R_ID + 10] = PVA_FW_MINOR;
    memset((void *)&pva_win[PVA_R_STATUS], 0, PVA_STATUS_SIZE);
    memset((void *)&pva_win[PVA_R_ARGS], 0, PVA_ARGS_SIZE);
    memset((void *)&pva_win[PVA_R_REPLY], 0, PVA_REPLY_SIZE);
    pva_win[PVA_R_BUSY] = 1;                // until core 1 is up
    pva_mailbox.post_count = 0;
    log_buf = pva_alloc(sizeof(char[LOG_LINES][LOG_LEN]));
    log_total = 0;
}
