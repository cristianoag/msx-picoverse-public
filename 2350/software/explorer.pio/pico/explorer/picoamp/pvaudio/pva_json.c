// MSX PICOVERSE PROJECT
// pva_json.c - PicoVerse Audio Engine: a streaming JSON reader for web replies.
//
// The reply is fed byte by byte as it comes in; nothing of its size is kept.
// Every value (string, number, true/false/null) is given to the caller with
// its path: the keys of the objects it is in, joined with '.', and the index
// in the innermost array. An object's end is given as well, so the fields of
// one array element can be collected and used together:
//
//   {"results":[{"name":"Amsterdam","latitude":52.37}, ...]}
//      PVA_JSON_VALUE  "results.name"      "Amsterdam"  idx 0
//      PVA_JSON_VALUE  "results.latitude"  "52.37"      idx 0
//      PVA_JSON_END    "results"                        idx 0
//
// Strings are UTF-8 (\uXXXX escapes are decoded); too long ones are cut.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pva_internal.h"

void pva_json_reset(pva_json_t *j)
{
    pva_json_cb cb = j->cb;
    void *user = j->user;
    memset(j, 0, sizeof *j);
    j->cb = cb;
    j->user = user;
}

static void put(pva_json_t *j, char c)
{
    if (j->is_key) { if (j->klen < sizeof j->key - 1) j->key[j->klen++] = c; j->key[j->klen] = 0; }
    else if (j->vlen < sizeof j->val - 1) j->val[j->vlen++] = c;
}

static void put_utf8(pva_json_t *j, uint32_t cp)
{
    if (cp < 0x80) put(j, (char)cp);
    else if (cp < 0x800) { put(j, (char)(0xC0 | (cp >> 6))); put(j, (char)(0x80 | (cp & 0x3F))); }
    else { put(j, (char)(0xE0 | (cp >> 12))); put(j, (char)(0x80 | ((cp >> 6) & 0x3F))); put(j, (char)(0x80 | (cp & 0x3F))); }
}

// the path of the current position (+ 'last', the key of a value in an object)
static const char *path(pva_json_t *j, const char *last)
{
    size_t o = 0;
    for (uint8_t d = 0; d < j->depth && d < PVA_JSON_DEPTH; d++)
    {
        const char *k = j->name[d];
        if (!k[0]) continue;
        if (o && o < sizeof j->pbuf - 1) j->pbuf[o++] = '.';
        for (; *k && o < sizeof j->pbuf - 1; k++) j->pbuf[o++] = *k;
    }
    if (last && last[0])
    {
        if (o && o < sizeof j->pbuf - 1) j->pbuf[o++] = '.';
        for (; *last && o < sizeof j->pbuf - 1; last++) j->pbuf[o++] = *last;
    }
    j->pbuf[o] = 0;
    return j->pbuf;
}

static uint16_t cur_idx(pva_json_t *j)
{
    for (int d = (int)j->depth - 1; d >= 0; d--)
        if (d < PVA_JSON_DEPTH && j->is_arr[d]) return j->idx[d];
    return 0;
}

static void value_done(pva_json_t *j)
{
    j->val[j->vlen] = 0;
    uint8_t d = j->depth;
    if (d && d <= PVA_JSON_DEPTH)
    {
        bool in_obj = !j->is_arr[d - 1];
        j->cb(j, PVA_JSON_VALUE, path(j, in_obj ? j->key : NULL), j->val, cur_idx(j));
        if (!in_obj) j->idx[d - 1]++;
    }
    j->have_val = false;
}

static void open_container(pva_json_t *j, bool arr)
{
    uint8_t d = j->depth;
    if (d < PVA_JSON_DEPTH)
    {
        // its name: the key it is under (in an object), else none
        bool parent_obj = d && !j->is_arr[d - 1];
        size_t n = 0;
        if (parent_obj) { n = strlen(j->key); if (n >= sizeof j->name[0]) n = sizeof j->name[0] - 1; memcpy(j->name[d], j->key, n); }
        j->name[d][n] = 0;
        j->is_arr[d] = arr;
        j->idx[d] = 0;
    }
    j->depth++;
    j->expect_key = !arr;
    j->seen = true;
}

static void close_container(pva_json_t *j)
{
    if (!j->depth) return;
    uint8_t d = --j->depth;
    if (d < PVA_JSON_DEPTH && !j->is_arr[d])
    {
        // an object ended: its path is that of where it is
        uint8_t keep = j->depth;
        j->depth = d + 1;
        const char *p = path(j, NULL);
        j->depth = keep;
        j->cb(j, PVA_JSON_END, p, "", cur_idx(j));
    }
    if (d && d - 1 < PVA_JSON_DEPTH && j->is_arr[d - 1]) j->idx[d - 1]++;
    if (d && d - 1 < PVA_JSON_DEPTH) j->expect_key = !j->is_arr[d - 1];
    if (!j->depth) j->done = true;
}

void pva_json_byte(pva_json_t *j, char c)
{
    if (j->done) return;
    if (j->in_str)
    {
        if (j->uhex)
        {
            int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
            j->ucp = (j->ucp << 4) | (uint32_t)h;
            if (--j->uhex == 0) put_utf8(j, j->ucp);
            return;
        }
        if (j->esc)
        {
            j->esc = false;
            if (c == 'u') { j->uhex = 4; j->ucp = 0; return; }
            put(j, (c == 'n' || c == 't' || c == 'r' || c == 'b' || c == 'f') ? ' ' : c);
            return;
        }
        if (c == '\\') { j->esc = true; return; }
        if (c == '"')
        {
            j->in_str = false;
            if (j->is_key) { j->is_key = false; j->expect_key = false; }
            else value_done(j);
            return;
        }
        put(j, c);
        return;
    }
    switch (c)
    {
        case '"':
            j->in_str = true;
            j->is_key = j->depth && j->depth <= PVA_JSON_DEPTH && !j->is_arr[j->depth - 1] && j->expect_key;
            if (j->is_key) { j->klen = 0; j->key[0] = 0; }
            else j->vlen = 0;
            return;
        case '{': open_container(j, false); return;
        case '[': open_container(j, true); return;
        case '}':
        case ']':
            if (j->have_val) value_done(j);
            close_container(j);
            return;
        case ':':
            j->expect_key = false;
            return;
        case ',':
            if (j->have_val) value_done(j);
            if (j->depth && j->depth <= PVA_JSON_DEPTH) j->expect_key = !j->is_arr[j->depth - 1];
            return;
        case ' ': case '\t': case '\r': case '\n':
            if (j->have_val) value_done(j);
            return;
        default:
            // a number, true, false or null
            if (!j->have_val) { j->vlen = 0; j->have_val = true; j->is_key = false; }
            put(j, c);
            return;
    }
}
