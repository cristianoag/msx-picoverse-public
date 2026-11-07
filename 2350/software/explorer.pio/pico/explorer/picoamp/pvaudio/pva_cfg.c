// MSX PICOVERSE PROJECT
// pva_cfg.c - PicoVerse Audio Engine: the settings file (PICOAMP.INI) as text.
//
//   ; comment
//   [clock]
//   face=analog
//
// Keys are given as "section.key" (any case). Setting a key keeps the rest of
// the text as it is (comments, order); a new key goes to the end of its
// section, a new section to the end of the text. The engine reads and writes
// the file (pva_engine.c); this keeps the text.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <string.h>
#include "pva_internal.h"

static char   text[PVA_CFG_MAX + 1];
static size_t len;
static uint8_t gen;

const char *pva_cfg_text(size_t *n) { *n = len; return text; }
uint8_t     pva_cfg_gen(void) { return gen; }

void pva_cfg_load(const char *t, size_t n)
{
    if (n > PVA_CFG_MAX) n = PVA_CFG_MAX;
    memcpy(text, t, n);
    len = n;
    text[len] = 0;
    // a NUL in the file ends it
    len = strlen(text);
    gen++;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool same_ci(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn) return false;
    for (size_t i = 0; i < an; i++) if (lc(a[i]) != lc(b[i])) return false;
    return true;
}

static void trim(const char **s, size_t *n)
{
    while (*n && (**s == ' ' || **s == '\t')) { (*s)++; (*n)--; }
    while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t' || (*s)[*n - 1] == '\r')) (*n)--;
}

// Find "section.key". Out: the value (vs, vn), the line (ls = start, le = after
// its line end), and the end of the section (se: after its last non-empty
// line; 0 = no such section).
typedef struct { size_t vs, vn, ls, le, se; bool found, sect; } where_t;

static where_t find(const char *sk)
{
    where_t r = { 0 };
    const char *dot = strchr(sk, '.');
    size_t sn = dot ? (size_t)(dot - sk) : 0;
    const char *key = dot ? dot + 1 : sk;
    size_t kn = strlen(key);
    bool in = sn == 0;                  // keys before any section: "key"
    if (in) r.sect = true;
    size_t p = 0;
    while (p < len)
    {
        size_t e = p;
        while (e < len && text[e] != '\n') e++;
        size_t next = e < len ? e + 1 : e;
        const char *l = text + p;
        size_t n = e - p;
        trim(&l, &n);
        if (n && l[0] == '[')
        {
            const char *c = memchr(l, ']', n);
            in = c && same_ci(l + 1, (size_t)(c - l - 1), sk, sn);
            if (in) { r.sect = true; r.se = next; }
        }
        else if (in && n)
        {
            r.se = next;
            const char *eq = (l[0] == ';' || l[0] == '#') ? NULL : memchr(l, '=', n);
            if (eq && !r.found)
            {
                const char *k = l;
                size_t kl = (size_t)(eq - l);
                trim(&k, &kl);
                if (same_ci(k, kl, key, kn))
                {
                    const char *v = eq + 1;
                    size_t vl = n - (size_t)(eq + 1 - l);
                    trim(&v, &vl);
                    r.found = true;
                    r.vs = (size_t)(v - text);
                    r.vn = vl;
                    r.ls = p;
                    r.le = next;
                }
            }
        }
        p = next;
    }
    return r;
}

bool pva_cfg_get(const char *sk, char *out, size_t size)
{
    where_t r = find(sk);
    if (!r.found) { if (size) out[0] = 0; return false; }
    size_t n = r.vn < size - 1 ? r.vn : size - 1;
    memcpy(out, text + r.vs, n);
    out[n] = 0;
    return true;
}

// put 'ins' (n bytes) at 'at', replacing 'cut' bytes
static bool splice(size_t at, size_t cut, const char *ins, size_t n)
{
    if (len - cut + n > PVA_CFG_MAX) return false;
    memmove(text + at + n, text + at + cut, len - at - cut + 1);
    memcpy(text + at, ins, n);
    len = len - cut + n;
    return true;
}

bool pva_cfg_set(const char *sk, const char *val)
{
    char v[400];
    size_t vn = 0;
    for (; val[vn] && vn < sizeof v - 1; vn++) v[vn] = (val[vn] == '\r' || val[vn] == '\n') ? ' ' : val[vn];
    v[vn] = 0;
    where_t r = find(sk);
    if (r.found)
    {
        if (r.vn == vn && !memcmp(text + r.vs, v, vn)) return true;
        if (!splice(r.vs, r.vn, v, vn)) return false;
        gen++;
        return true;
    }
    const char *dot = strchr(sk, '.');
    const char *key = dot ? dot + 1 : sk;
    char ln[440];
    size_t n;
    if (r.sect && r.se)
    {
        // the end of the section
        n = (size_t)snprintf(ln, sizeof ln, "%s=%s\r\n", key, v);
        size_t at = r.se;
        if (at > len) at = len;
        if (at && text[at - 1] != '\n')
        {
            if (!splice(at, 0, "\r\n", 2)) return false;
            at += 2;
        }
        if (!splice(at, 0, ln, n)) return false;
    }
    else if (dot)
    {
        n = (size_t)snprintf(ln, sizeof ln, "%s[%.*s]\r\n%s=%s\r\n",
                             len && text[len - 1] != '\n' ? "\r\n\r\n" : len ? "\r\n" : "",
                             (int)(dot - sk), sk, key, v);
        if (!splice(len, 0, ln, n)) return false;
    }
    else
    {
        n = (size_t)snprintf(ln, sizeof ln, "%s=%s\r\n", key, v);
        if (!splice(0, 0, ln, n)) return false;
    }
    gen++;
    return true;
}
