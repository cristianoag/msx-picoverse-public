// MSX PICOVERSE PROJECT
// pva_markets.c - PicoVerse Audio Engine: a market monitor: indices, shares,
// currencies, commodities and rates, by region (engine 0.13).
//
// The prices come from Yahoo Finance's chart service (an unofficial source,
// delayed by up to 15 or 20 minutes for most exchanges):
//   https://query1.finance.yahoo.com/v8/finance/spark?symbols=A,B,..&range=1d&interval=1d
// answers for up to 20 symbols at once, about 240 bytes each:
//   {"^AEX":{"timestamp":[..],"symbol":"^AEX","fulldayPrice":1141.0,"close":[1141.0],
//            "previousClose":null,"chartPreviousClose":1140.12, ...}, ...}
// and with interval=15m for one symbol the day's course (the chart).
//
// The catalogue (pva_markets_cat.c) has the regions and their lists; "My list"
// is the user's own ([markets] list=ASML.AS,AAPL,BTC-EUR in PICOAMP.INI, any
// Yahoo symbols, up to 40). The MSX opens a list (MKT_OPEN): the engine fetches
// its prices, 20 symbols a request, and again every minute while the list stays
// open ([markets] refresh=seconds; every 5 minutes while an https station
// plays: the ESP-01 has one TLS connection, the station lends it for each
// request and connects again, playing from its buffer meanwhile). The MSX gets the rows as text, ready to show
// (MKT_ROWS), and the chart as column heights (MKT_CHART_GET).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pva_internal.h"

#define BATCH           20u             // symbols a request (the service's limit)
#define URL_MAX_M       400u            // (the web client's path: 420)
#define REFRESH_S       60u
#define REFRESH_MIN_S   20u
#define REFRESH_TLS_S   300u            // while an https station plays
#define TLS_REST_MS     20000u         // a request waits this long at most for an https station's buffer
#define RETRY_MS        20000u
#define RETRY_MAX_MS    (5u * 60u * 1000u)
#define MY_MAX          40u             // My list
#define MY_TEXT         240u
#define PTS_MAX         264u            // chart points kept (a year of days: 256; more: every 2nd, 4th...)
#define TAPE_MAX        900u            // the headlines' tape
#define TAPE_S          900u            // fetched this often
#define TAPE_RETRY_S    300u
#define ROW_TEXT        39u             // name 14, price 10, change 8, percent 7

static const char *const hosts[] = { "query1.finance.yahoo.com", "query2.finance.yahoo.com" };
static const char agent[] = "Mozilla/5.0 (compatible; PicoAmp/1.2; MSX PicoVerse 2350)";

// a price and the previous close, in units of 10^-sc (as many decimals as 9 digits leave: the engine's
// memory is small). have: 1 price, 2 previous close, 4 / 8 from the field that counts most
typedef struct { int32_t price, prev; uint8_t have, sc; } quote_t;
typedef struct { const char *sym, *name; uint8_t dec; } row_t;

static bool        available;
static pva_json_t *J;                   // (the clock's: one web request at a time)
static quote_t    *qt;                  // PVA_MKT_ROWS
static const pva_mkt_item_t *items;     // the open list's (NULL: My list)
static char       *my_text;             // My list's symbols, 0-terminated each
static uint32_t   *pt_t;                // chart: the points' times,
static int32_t    *pt_v;                //        prices in units of 10^-ch_sc
static uint16_t    pt_stride = 1;       // every pt_stride-th point of the reply is kept
static uint8_t    *ord;                 // the rows' order (sort_mode)
static uint8_t     sort_mode;
static char       *tape;                // the headlines, one after the other
static uint16_t    tape_len, tape_new, tape_pos;
static uint32_t    tape_due;
static bool        tape_have_due;
static uint8_t     tape_region = 0xFF;
static uint8_t     ch_sc;
static uint8_t     region;
static uint8_t     open_list = 0xFF;    // in the region (its lists, then My list)
static const pva_mkt_members_t *sub;    // an index's members shown in its place (NULL: the list itself)
static uint8_t     nrows;
static uint8_t     state = PVA_MKT_IDLE, gen, last_err;
static uint8_t     host_i;
static uint32_t    next_due, retry_ms = RETRY_MS, updated_ms;
static bool        fetch_now, have_data, stale;
static char        updated[8];
// the request running
enum { Q_NONE, Q_QUOTES, Q_CHART, Q_TAPE };
static uint8_t     q = Q_NONE;
static uint8_t     chunk_first, chunk_n, chunk_got;
static uint8_t     cur_row = 0xFF;      // the row whose object is being read
// the chart
static uint8_t     ch_state = PVA_MKT_IDLE, ch_row, ch_range;
static uint16_t    ch_n;
static bool        ch_pending;
static uint32_t    ch_start, ch_end;
static int32_t     ch_prev, ch_price;
static bool        ch_have_prev, ch_have_price;

// ---------------------------------------------------------------- the catalogue
static uint8_t region_count(void) { uint8_t n = 0; while (pva_mkt_region_codes[n]) n++; return n; }

// the region's i-th list (NULL: none)
static const pva_mkt_list_t *region_list(uint8_t i)
{
    for (const pva_mkt_list_t *l = pva_mkt_lists; l->name; l++)
        if (l->region == region && i-- == 0) return l;
    return NULL;
}

static uint8_t region_lists(void)
{
    uint8_t n = 0;
    while (region_list(n)) n++;
    return n;
}

// My list: the symbols of [markets] list= (to my_text), how many
static uint8_t my_load(void)
{
    char v[MY_TEXT];
    if (!my_text || !pva_cfg_get("markets.list", v, sizeof v)) return 0;
    uint8_t n = 0;
    size_t o = 0;
    for (const char *p = v; *p && n < MY_MAX; )
    {
        while (*p == ',' || *p == ' ' || *p == ';') p++;
        size_t k = 0;
        while (p[k] && p[k] != ',' && p[k] != ' ' && p[k] != ';') k++;
        if (!k) break;
        if (k <= 15u && o + k + 1u <= MY_TEXT)
        {
            for (size_t i = 0; i < k; i++)
            {
                char c = p[i];
                my_text[o++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
            }
            my_text[o++] = 0;
            n++;
        }
        p += k;
    }
    return n;
}

// the region: [markets] region=, else by the clock's time zone, else Europe
static void region_load(void)
{
    char v[24];
    if (pva_cfg_get("markets.region", v, sizeof v))
    {
        for (uint8_t i = 0; pva_mkt_region_codes[i]; i++)
            if (!strcmp(v, pva_mkt_region_codes[i])) { region = i; return; }
    }
    const char *z = pva_clock_zone();
    if (!strncmp(z, "America/", 8)) region = 1;
    else if (!strncmp(z, "Asia/", 5) || !strncmp(z, "Australia/", 10) || !strncmp(z, "Pacific/", 8)) region = 2;
    else region = 0;
}

// ---------------------------------------------------------------- numbers
// "1141.03", "-4.0E-4": the value in millionths (false: not a number)
static bool parse_num(const char *s, int64_t *out)
{
    bool neg = false;
    if (*s == '-') { neg = true; s++; } else if (*s == '+') s++;
    if ((*s < '0' || *s > '9') && *s != '.') return false;
    int64_t m = 0;              // the digits
    int sc = 0;                 // m * 10^sc
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; s++) { if (digits < 17) { m = m * 10 + (*s - '0'); if (m) digits++; } else sc++; }
    if (*s == '.')
        for (s++; *s >= '0' && *s <= '9'; s++) if (digits < 17) { m = m * 10 + (*s - '0'); sc--; if (m) digits++; }
    if (*s == 'e' || *s == 'E')
    {
        int e = atoi(s + 1);
        if (e > 30) e = 30;
        if (e < -30) e = -30;
        sc += e;
    }
    sc += 6;
    while (sc > 0 && m < INT64_MAX / 10) { m *= 10; sc--; }
    while (sc < 0)
    {
        int64_t r = m % 10;
        m /= 10;
        if (sc == -1 && r >= 5) m++;
        sc++;
    }
    *out = neg ? -m : m;
    return true;
}

static const int32_t p10[] = { 1, 10, 100, 1000, 10000, 100000, 1000000 };

// millionths to units of 10^-sc, rounded
static int64_t to_scale(int64_t micro, uint8_t sc)
{
    int64_t d = p10[6 - sc];
    return (micro >= 0 ? micro + d / 2 : micro - d / 2) / d;
}

// the most decimals (up to 6) with which the value has 9 digits or less
static uint8_t scale_for(int64_t micro)
{
    uint8_t sc = 6;
    if (micro < 0) micro = -micro;
    while (sc && micro >= 1000000000) { micro /= 10; sc--; }
    return sc;
}

// v (units of 10^-sc) with 'dec' decimals, rounded; a '+' before a positive one when asked
static void fmt_num(char *out, size_t size, int64_t v, uint8_t sc, uint8_t dec, bool sign)
{
    if (dec > sc) dec = sc;
    bool neg = v < 0;
    uint64_t a = (uint64_t)(neg ? -v : v);
    uint64_t div = (uint64_t)p10[sc - dec];
    a = (a + div / 2u) / div;                   // in units of 10^-dec
    uint64_t whole = a / (uint64_t)p10[dec], frac = a % (uint64_t)p10[dec];
    if (!a) neg = false;
    const char *sg = neg ? "-" : (sign && a) ? "+" : "";
    // (not %llu: the cartridge's printf does not take 64-bit numbers; at most 9 digits here)
    if (whole > 999999999u) whole = 999999999u;
    if (dec) snprintf(out, size, "%s%lu.%0*lu", sg, (unsigned long)whole, (int)dec, (unsigned long)frac);
    else snprintf(out, size, "%s%lu", sg, (unsigned long)whole);
}

// the change in hundredths of a percent
static int32_t pct100(int64_t price, int64_t prev)
{
    if (prev <= 0) return 0;
    int64_t d = (price - prev) * 10000;
    int64_t r = (d >= 0 ? d + prev / 2 : d - prev / 2) / prev;
    if (r > 999999) r = 999999;
    if (r < -999999) r = -999999;
    return (int32_t)r;
}

static void fmt_pct(char *out, size_t size, int32_t p)
{
    uint32_t a = (uint32_t)(p < 0 ? -p : p);
    snprintf(out, size, "%s%lu.%02lu%%", p < 0 ? "-" : p > 0 ? "+" : "", (unsigned long)(a / 100u), (unsigned long)(a % 100u));
}

// ---------------------------------------------------------------- the reply
// the open list's row i
static row_t row(uint8_t i)
{
    row_t r;
    if (items) { r.sym = items[i].sym; r.name = items[i].name; r.dec = items[i].dec; return r; }
    const char *s = my_text;
    while (i--) s += strlen(s) + 1;
    size_t k = strlen(s);
    r.sym = r.name = s;
    r.dec = (uint8_t)(k > 2 && !strcmp(s + k - 2, "=X") ? 4 : 2);       // a currency pair: 4 decimals
    return r;
}

// a price of the row: both of its values keep the same scale
static void put_quote(quote_t *t, bool is_prev, int64_t micro)
{
    uint8_t sc = scale_for(micro);
    bool other = (t->have & (is_prev ? 1u : 2u)) != 0;
    if (other && t->sc < sc) sc = t->sc;
    if (other && t->sc > sc)
    {
        int32_t *o = is_prev ? &t->price : &t->prev;
        int32_t d = p10[t->sc - sc];
        *o = (*o + d / 2) / d;
    }
    t->sc = sc;
    if (is_prev) t->prev = (int32_t)to_scale(micro, sc); else t->price = (int32_t)to_scale(micro, sc);
}

static uint8_t find_row(const char *sym, size_t n)
{
    uint8_t a = q == Q_CHART ? ch_row : chunk_first;
    uint8_t b = q == Q_CHART ? (uint8_t)(ch_row + 1u) : (uint8_t)(chunk_first + chunk_n);
    for (uint8_t i = a; i < b && i < nrows; i++)
    {
        const char *s = row(i).sym;
        if (strlen(s) == n && !strncmp(s, sym, n)) return i;
    }
    return 0xFF;
}

static void json_cb(pva_json_t *j, int ev, const char *path, const char *v, uint16_t idx)
{
    (void)j;
    if (ev != PVA_JSON_VALUE) return;
    const char *dot = strrchr(path, '.');       // "SYMBOL.field" (a symbol may have dots)
    if (!dot) return;
    const char *f = dot + 1;
    if (stale) return;
    uint8_t r = find_row(path, (size_t)(dot - path));
    if (r == 0xFF) return;
    if (r != cur_row) { cur_row = r; if (q == Q_QUOTES) chunk_got++; }
    int64_t x;
    if (q == Q_QUOTES)
    {
        quote_t *t = &qt[r];
        if (!strcmp(f, "fulldayPrice")) { if (parse_num(v, &x)) { put_quote(t, false, x); t->have |= 5u; } }
        else if (!strcmp(f, "close")) { if (!(t->have & 4u) && parse_num(v, &x)) { put_quote(t, false, x); t->have |= 1u; } }
        else if (!strcmp(f, "chartPreviousClose")) { if (parse_num(v, &x) && x > 0) { put_quote(t, true, x); t->have |= 10u; } }
        else if (!strcmp(f, "previousClose")) { if (!(t->have & 8u) && parse_num(v, &x) && x > 0) { put_quote(t, true, x); t->have |= 2u; } }
        return;
    }
    // the chart: the times, then the prices (a null: no trade in that quarter). The first price sets the
    // scale, with a digit to spare for the day's course
    if (!strcmp(f, "timestamp"))
    {
        // more points than fit: every 2nd is kept, then every 4th... (the times come before the prices)
        uint32_t ts = (uint32_t)strtoul(v, NULL, 10);
        if (idx % pt_stride) return;
        uint16_t k = (uint16_t)(idx / pt_stride);
        if (k >= PTS_MAX)
        {
            for (uint16_t i = 0; i < PTS_MAX / 2u; i++) pt_t[i] = pt_t[i * 2u];
            pt_stride *= 2u;
            ch_n = (uint16_t)(PTS_MAX / 2u);
            if (idx % pt_stride) return;
            k = (uint16_t)(idx / pt_stride);
        }
        pt_t[k] = ts;
        pt_v[k] = 0;
        if (k >= ch_n) ch_n = (uint16_t)(k + 1u);
        return;
    }
    if (!strcmp(f, "start")) { ch_start = (uint32_t)strtoul(v, NULL, 10); return; }
    if (!strcmp(f, "end")) { ch_end = (uint32_t)strtoul(v, NULL, 10); return; }
    bool is_close = !strcmp(f, "close"), is_cprev = !strcmp(f, "chartPreviousClose"), is_prev = !strcmp(f, "previousClose"),
         is_price = !strcmp(f, "fulldayPrice");
    if (!(is_close || is_cprev || is_prev || is_price) || !parse_num(v, &x) || x <= 0) return;
    if (ch_sc == 0xFF) { ch_sc = scale_for(x * 2); }
    // a price too large for the scale (years ago it was a fraction of it): a decimal less for all
    while (ch_sc && x >= (int64_t)2000000000 * p10[6 - ch_sc])
    {
        ch_sc--;
        for (uint16_t i = 0; i < ch_n; i++) pt_v[i] /= 10;
        ch_prev /= 10;
        ch_price /= 10;
    }
    if (x >= (int64_t)2000000000 * p10[6 - ch_sc]) return;
    int32_t y = (int32_t)to_scale(x, ch_sc);
    if (is_close) { if (!(idx % pt_stride) && idx / pt_stride < PTS_MAX) pt_v[idx / pt_stride] = y; }
    else if (is_cprev) { ch_prev = y; ch_have_prev = true; }
    else if (is_prev) { if (!ch_have_prev) { ch_prev = y; ch_have_prev = true; } }
    else { ch_price = y; ch_have_price = true; }
}

static bool body(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n && !J->done; i++) pva_json_byte(J, (char)p[i]);
    return J->done;
}

static void stamp(void)
{
    uint8_t h, m, wd;
    if (pva_clock_local(&h, &m, &wd)) snprintf(updated, sizeof updated, "%02u:%02u", (unsigned)h, (unsigned)m);
    else updated[0] = 0;
    updated_ms = pva_hal_millis();
}

static uint32_t refresh_ms(void)
{
    char v[12];
    uint32_t s = REFRESH_S;
    if (pva_cfg_get("markets.refresh", v, sizeof v) && atoi(v) > 0) s = (uint32_t)atoi(v);
    if (s < REFRESH_MIN_S) s = REFRESH_MIN_S;
    if (s > 3600u) s = 3600u;
    if (s < REFRESH_TLS_S && pva_net_active() && pva_net_is_tls()) s = REFRESH_TLS_S;
    return s * 1000u;
}

// the rows' order: as the list has them, or by percent (rows without a price last)
static void resort(void)
{
    for (uint8_t i = 0; i < nrows; i++) ord[i] = i;
    if (!sort_mode) return;
    #define PCT(r) ((qt[r].have & 3u) == 3u ? pct100(qt[r].price, qt[r].prev) : INT32_MIN)
    for (uint8_t i = 1; i < nrows; i++)
    {
        uint8_t r = ord[i];
        int32_t p = PCT(r);
        uint8_t k = i;
        while (k)
        {
            int32_t o = PCT(ord[k - 1]);
            bool before = p != INT32_MIN && (o == INT32_MIN || (sort_mode == 1 ? p > o : p < o));
            if (!before) break;
            ord[k] = ord[k - 1];
            k--;
        }
        ord[k] = r;
    }
    #undef PCT
}
static uint8_t at(uint8_t r) { return r < nrows ? ord[r] : r; }

// the tape: the titles of the feed's items, " +++ " between them
static struct { uint8_t st; char tag[8]; uint8_t tn; bool item, in_title; char ent[8]; uint8_t en; uint32_t cp; uint8_t more; } T;

static void tape_put(char c)
{
    if (tape_new + 1u < TAPE_MAX && !(c == ' ' && tape_new && tape[tape_new - 1] == ' ')) tape[tape_new++] = c;
}

static void tape_char(uint32_t cp)
{
    char c = cp < 128u ? (char)cp : pva_fold_ascii(cp);
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c >= 32) tape_put(c);
}

static bool tape_body(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint8_t c = p[i];
        if (T.st == 1)                          // in a tag
        {
            if (c == '>')
            {
                T.tag[T.tn] = 0;
                T.st = 0;
                if (!strcmp(T.tag, "item")) T.item = true;
                else if (!strcmp(T.tag, "/item")) T.item = false;
                else if (!strcmp(T.tag, "title") && T.item)
                {
                    if (tape_new + 60u > TAPE_MAX) return true;     // (no room for one more)
                    if (tape_new) { tape_put(' '); tape_put('+'); tape_put('+'); tape_put('+'); tape_put(' '); }
                    T.in_title = true;
                }
                else if (!strcmp(T.tag, "/title")) T.in_title = false;
            }
            else if (T.tn + 1u < sizeof T.tag) T.tag[T.tn++] = (char)c;
            continue;
        }
        if (c == '<') { T.st = 1; T.tn = 0; T.en = 0; T.more = 0; continue; }
        if (!T.in_title) continue;
        if (T.en)                               // "&amp;"
        {
            if (c == ';')
            {
                T.ent[T.en] = 0;
                const char *e = T.ent + 1;
                uint32_t cp = !strcmp(e, "amp") ? '&' : !strcmp(e, "lt") ? '<' : !strcmp(e, "gt") ? '>' : !strcmp(e, "quot") ? '"' :
                              !strcmp(e, "apos") ? '\'' : e[0] == '#' ? (e[1] == 'x' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10)) : ' ';
                tape_char(cp);
                T.en = 0;
            }
            else if (T.en + 1u < sizeof T.ent) T.ent[T.en++] = (char)c;
            else T.en = 0;
            continue;
        }
        if (c == '&') { T.ent[0] = '&'; T.en = 1; continue; }
        if (T.more) { T.cp = (T.cp << 6) | (c & 0x3Fu); if (!--T.more) tape_char(T.cp); continue; }     // UTF-8
        if (c >= 0xF0) { T.cp = c & 7u; T.more = 3; }
        else if (c >= 0xE0) { T.cp = c & 15u; T.more = 2; }
        else if (c >= 0xC0) { T.cp = c & 31u; T.more = 1; }
        else tape_char(c);
    }
    return false;
}

static void failed(bool http_ok)
{
    last_err = http_ok ? PVA_NERR_HTTP : pva_web_error() ? pva_web_error() : PVA_NERR_HTTP;
    host_i ^= 1u;                               // the other server next time
    pva_log("markets: failed (%u, HTTP %u)", (unsigned)last_err, (unsigned)pva_web_http());
}

static void fetch_done(bool ok)
{
    uint8_t was = q;
    q = Q_NONE;
    uint32_t now = pva_hal_millis();
    if (was == Q_TAPE)
    {
        if (stale) stale = false;
        tape_have_due = true;
        if (tape_new > 20u) { tape_len = tape_new; tape_pos = 0; tape_due = now + TAPE_S * 1000u; }
        else tape_due = now + TAPE_RETRY_S * 1000u;
        return;
    }
    if (stale) { stale = false; return; }      // its list was closed meanwhile
    if (was == Q_CHART)
    {
        if (ch_pending) return;                 // (another one was asked for meanwhile: this one is not shown)
        if (ok && ch_n)
        {
            // the points that have a price, in their order
            uint16_t k = 0;
            for (uint16_t i = 0; i < ch_n; i++) if (pt_v[i] > 0) { pt_t[k] = pt_t[i]; pt_v[k] = pt_v[i]; k++; }
            ch_n = k;
        }
        if (ok && ch_n) ch_state = PVA_MKT_DONE;
        else { ch_state = PVA_MKT_FAILED; if (!ok) failed(false); else last_err = PVA_NERR_HTTP; }
        return;
    }
    if (was != Q_QUOTES) return;
    if (ok && chunk_got)
    {
        chunk_first = (uint8_t)(chunk_first + chunk_n);
        if (chunk_first < nrows) return;        // (the next ones: pva_markets_poll)
        have_data = true;
        state = PVA_MKT_DONE;
        last_err = 0;
        retry_ms = RETRY_MS;
        resort();
        gen++;
        stamp();
        next_due = now + refresh_ms();
        chunk_first = 0xFF;
        return;
    }
    failed(ok);
    if (!have_data) state = PVA_MKT_FAILED;
    gen++;
    chunk_first = 0xFF;
    next_due = now + retry_ms;
    if (retry_ms < RETRY_MAX_MS) retry_ms *= 2u;
}

// "%5EAEX,EURUSD%3DX": the symbol as a URL takes it
static size_t put_sym(char *out, size_t size, const char *s)
{
    size_t o = 0;
    for (; *s && o + 4u < size; s++)
    {
        char c = *s;
        bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '_';
        if (plain) out[o++] = c;
        else o += (size_t)snprintf(out + o, size - o, "%%%02X", (unsigned)(uint8_t)c);
    }
    out[o] = 0;
    return o;
}

static bool start(uint8_t what, const char *url)
{
    J->cb = json_cb;
    pva_json_reset(J);
    cur_row = 0xFF;
    q = what;
    pva_web_agent(agent);
    if (pva_web_get(url, body, fetch_done) != PVA_OK) { q = Q_NONE; return false; }
    return true;
}

static void start_quotes(void)
{
    char url[URL_MAX_M + 64];
    size_t o = (size_t)snprintf(url, sizeof url, "https://%s/v8/finance/spark?symbols=", hosts[host_i]);
    uint8_t k = 0;
    while (k < BATCH && chunk_first + k < nrows)
    {
        char e[52];
        size_t n = put_sym(e, sizeof e, row((uint8_t)(chunk_first + k)).sym);
        if (o + n + 1u > URL_MAX_M - 28u) break;
        if (k) url[o++] = ',';
        memcpy(url + o, e, n);
        o += n;
        k++;
    }
    snprintf(url + o, sizeof url - o, "&range=1d&interval=1d");
    chunk_n = k;
    chunk_got = 0;
    start(Q_QUOTES, url);
}

static void start_chart(void)
{
    static const char *const rg[PVA_MKT_RANGES] = { "range=1d&interval=15m", "range=5d&interval=60m", "range=1mo&interval=1d", "range=6mo&interval=1d",
                                                    "range=ytd&interval=1d", "range=1y&interval=1d", "range=5y&interval=1wk", "range=max&interval=1mo" };
    char url[160], e[52];
    put_sym(e, sizeof e, row(ch_row).sym);
    snprintf(url, sizeof url, "https://%s/v8/finance/spark?symbols=%s&%s", hosts[host_i], e, rg[ch_range]);
    ch_n = 0;
    ch_state = PVA_MKT_LOADING;
    pt_stride = 1;
    ch_start = ch_end = 0;
    ch_have_prev = ch_have_price = false;
    ch_sc = 0xFF;
    if (!start(Q_CHART, url)) { ch_state = PVA_MKT_FAILED; last_err = PVA_NERR_NOWIFI; }
}

// the region's market headlines: Yahoo Finance's feed for its main indices
static void start_tape(void)
{
    static const char *const syms[] = { "%5EAEX,%5EGDAXI,%5EFTSE,%5EFCHI", "%5EGSPC,%5EDJI,%5EIXIC", "%5EN225,%5EHSI,000001.SS" };
    char url[160];
    snprintf(url, sizeof url, "https://feeds.finance.yahoo.com/rss/2.0/headline?s=%s&region=US&lang=en-US", syms[region < 3u ? region : 0]);
    memset(&T, 0, sizeof T);
    tape_new = 0;
    tape_region = region;
    q = Q_TAPE;
    pva_web_agent(agent);
    if (pva_web_get(url, tape_body, fetch_done) != PVA_OK) { q = Q_NONE; tape_have_due = true; tape_due = pva_hal_millis() + TAPE_RETRY_S * 1000u; }
}

// ---------------------------------------------------------------- polling
void pva_markets_poll(void)
{
    static bool waiting;
    static uint32_t wait_from;
    if (!available || q != Q_NONE || open_list == 0xFF || pva_web_busy() || !pva_hal_net_present()) return;
    uint32_t now = pva_hal_millis();
    bool more = chunk_first != 0xFF && chunk_first < nrows;         // the list's next symbols
    bool due = nrows && (fetch_now || (int32_t)(now - next_due) >= 0);
    bool tp = tape_region != region || !tape_have_due || (int32_t)(now - tape_due) >= 0;    // the tape: after the prices
    if (tp && tape_region != region) tape_len = 0;
    if (!more && !ch_pending && !due && !tp) { waiting = false; return; }
    // an https station lends its TLS connection for a request and plays from its buffer meanwhile:
    // the request waits a while for that buffer to be full
    if (!waiting) { waiting = true; wait_from = now; }
    if (pva_net_active() && pva_net_is_tls() && pva_net_capacity() && now - wait_from < TLS_REST_MS &&
        pva_net_buffered() < pva_net_capacity() - pva_net_capacity() / 20u) return;
    waiting = false;
    if (ch_pending) { ch_pending = false; start_chart(); return; }     // (before a long list's next prices)
    if (more) { start_quotes(); return; }
    if (!due) { start_tape(); return; }
    fetch_now = false;
    chunk_first = 0;
    start_quotes();
}

void pva_markets_state(uint8_t *o)
{
    o[0] = state;
    o[1] = gen;
    o[2] = ch_state;
    o[3] = last_err;
}

void pva_markets_init(void)
{
    // about 1.3 KB of the engine's memory (the stream RAM's spare is the station directory's)
    J = pva_clock_json();
    qt = pva_alloc(PVA_MKT_ROWS * sizeof *qt);
    my_text = pva_alloc(MY_TEXT);
    pt_t = pva_alloc(PTS_MAX * sizeof *pt_t);
    pt_v = pva_alloc(PTS_MAX * sizeof *pt_v);
    ord = pva_alloc(PVA_MKT_ROWS);
    tape = pva_alloc(TAPE_MAX);
    available = qt && my_text && pt_t && pt_v && ord && tape;
    if (!available) pva_log("markets: no memory");
    chunk_first = 0xFF;
}

// ---------------------------------------------------------------- commands
static void close_list(void)
{
    open_list = 0xFF;
    sub = NULL;
    nrows = 0;
    have_data = false;
    state = PVA_MKT_IDLE;
    ch_state = PVA_MKT_IDLE;
    ch_pending = false;
    chunk_first = 0xFF;
    updated[0] = 0;
    if (q != Q_NONE) stale = true;      // (a request that runs ends by itself; its answer is not used)
}

static int cmd_lists(void)
{
    region_load();
    uint8_t n = region_lists();
    uint8_t my = my_load();
    pva_reply_byte(region);
    pva_reply_byte(region_count());
    pva_reply_byte((uint8_t)(n + (my ? 1u : 0u)));
    pva_reply_byte(open_list);
    pva_reply_str(pva_mkt_region_names[region]);
    for (uint8_t i = 0; i < n; i++) pva_reply_str(region_list(i)->name);
    if (my) pva_reply_str("My list");
    return PVA_OK;
}

static int cmd_region(uint8_t r)
{
    region_load();
    if (r != 0xFF)
    {
        if (r == 0xFE) r = (uint8_t)((region + 1u) % region_count());
        if (r >= region_count()) return PVA_E_RANGE;
        if (!pva_cfg_set("markets.region", pva_mkt_region_codes[r])) return PVA_E_FULL;
        if (r != region) close_list();
        region = r;
    }
    pva_reply_byte(region);
    pva_reply_str(pva_mkt_region_names[region]);
    return PVA_OK;
}

static int cmd_open(uint8_t list)
{
    if (list == 0xFF) { close_list(); pva_reply_byte(0); return PVA_OK; }
    region_load();
    uint8_t n = region_lists();
    if (list == PVA_MKT_NOW && open_list != 0xFF) list = sub ? PVA_MKT_NOW : open_list;
    if ((list == open_list && !sub && nrows) || (list == PVA_MKT_NOW && sub))
    {
        fetch_now = true;                       // the same one again: its prices now
        pva_reply_byte(nrows);
        return PVA_OK;
    }
    const pva_mkt_list_t *l = list < n ? region_list(list) : NULL;
    uint8_t my = list == n ? my_load() : 0;
    if (!l && !my) return PVA_E_RANGE;
    close_list();
    items = l ? l->items : NULL;
    nrows = l ? l->n : my;
    if (nrows > PVA_MKT_ROWS) nrows = PVA_MKT_ROWS;
    memset(qt, 0, PVA_MKT_ROWS * sizeof *qt);
    open_list = list;
    resort();
    state = PVA_MKT_LOADING;
    last_err = 0;
    retry_ms = RETRY_MS;
    fetch_now = true;
    gen++;
    pva_reply_byte(nrows);
    return PVA_OK;
}

// the members of the index in row r, in the list's place
static int cmd_members(uint8_t r)
{
    if (open_list == 0xFF || sub) return PVA_E_STATE;
    if (r >= nrows) return PVA_E_RANGE;
    const char *sym = row(at(r)).sym;
    const pva_mkt_members_t *m = pva_mkt_members;
    while (m->sym && strcmp(m->sym, sym)) m++;
    if (!m->sym) return PVA_E_NOTFOUND;
    uint8_t list = open_list;
    close_list();
    open_list = list;
    sub = m;
    items = m->items;
    nrows = m->n > PVA_MKT_ROWS ? PVA_MKT_ROWS : m->n;
    memset(qt, 0, PVA_MKT_ROWS * sizeof *qt);
    resort();
    state = PVA_MKT_LOADING;
    last_err = 0;
    retry_ms = RETRY_MS;
    fetch_now = true;
    gen++;
    pva_reply_byte(nrows);
    return PVA_OK;
}

static int cmd_sort(uint8_t m)
{
    if (m != 0xFF)
    {
        if (m > 2u) return PVA_E_RANGE;
        sort_mode = m;
        resort();
        gen++;
    }
    pva_reply_byte(sort_mode);
    return PVA_OK;
}

// the tape: 80 characters from where it is, and one on
static int cmd_tape(void)
{
    char w[81];
    if (tape_len < 40u || tape_region != region) { pva_reply_str(""); return PVA_OK; }
    uint16_t period = (uint16_t)(tape_len + 5u);            // (" +++ " between its end and its start)
    if (tape_pos >= period) tape_pos = 0;
    for (uint8_t i = 0; i < 80u; i++)
    {
        uint16_t k = (uint16_t)((tape_pos + i) % period);
        w[i] = k < tape_len ? tape[k] : " +++ "[k - tape_len];
    }
    w[80] = 0;
    tape_pos++;
    pva_reply_str(w);
    return PVA_OK;
}

// the headlines of the instrument in row r: Yahoo Finance's feed for its symbol, read by the news reader
static int cmd_news(uint8_t r)
{
    if (open_list == 0xFF) return PVA_E_STATE;
    if (r >= nrows) return PVA_E_RANGE;
    row_t w = row(at(r));
    char url[160], e[52];
    put_sym(e, sizeof e, w.sym);
    snprintf(url, sizeof url, "https://feeds.finance.yahoo.com/rss/2.0/headline?s=%s&region=US&lang=en-US", e);
    return pva_news_fetch_url(w.name, url);
}

// a row's text: name, price, change, percent in their columns
static char row_text(uint8_t r, char *out, size_t size)
{
    const quote_t *t = &qt[r];
    row_t w = row(r);
    char p[24] = "-", c[24] = "", pc[16] = "";
    char dir = '?';
    if (t->have & 1u)
    {
        fmt_num(p, sizeof p, t->price, t->sc, w.dec, false);
        if (t->have & 2u)
        {
            int32_t h = pct100(t->price, t->prev);
            fmt_num(c, sizeof c, (int64_t)t->price - t->prev, t->sc, w.dec, true);
            fmt_pct(pc, sizeof pc, h);
            dir = h > 0 ? '+' : h < 0 ? '-' : ' ';
        }
        else dir = ' ';
    }
    snprintf(out, size, "%-14.14s%10.10s%8.8s%7.7s", w.name, p, c, pc);
    return dir;
}

static int cmd_rows(uint8_t first, uint8_t count)
{
    if (open_list == 0xFF) return PVA_E_STATE;
    if (first > nrows) first = nrows;
    if (count > nrows - first) count = (uint8_t)(nrows - first);
    if (count > 40u) count = 40u;                   // (41 bytes a row, 2 KB of reply)
    pva_reply_byte(nrows);
    pva_reply_byte(count);
    for (uint8_t i = 0; i < count; i++)
    {
        char t[ROW_TEXT + 1];
        pva_reply_byte((uint8_t)row_text(at((uint8_t)(first + i)), t, sizeof t));
        pva_reply_str(t);
    }
    return PVA_OK;
}

static int cmd_info(void)
{
    if (open_list == 0xFF) return PVA_E_STATE;
    uint32_t age = have_data ? (pva_hal_millis() - updated_ms) / 1000u : 0xFFFFu;
    if (age > 0xFFFEu && have_data) age = 0xFFFEu;
    const pva_mkt_list_t *l = region_list(open_list);
    pva_reply_byte(state);
    pva_reply_byte(nrows);
    pva_reply_byte((uint8_t)age);
    pva_reply_byte((uint8_t)(age >> 8));
    pva_reply_str(sub ? sub->name : l ? l->name : "My list");
    pva_reply_str(pva_mkt_region_names[region]);
    pva_reply_str(updated);
    return PVA_OK;
}

static int cmd_chart(uint8_t r, uint8_t range)
{
    if (open_list == 0xFF) return PVA_E_STATE;
    if (r >= nrows) return PVA_E_RANGE;
    ch_row = at(r);
    ch_range = range < PVA_MKT_RANGES ? range : 0;
    ch_state = PVA_MKT_LOADING;
    ch_pending = true;
    return PVA_OK;
}

// the chart as 'width' columns of 0 (nothing) or 1..'levels'
static int cmd_chart_get(uint8_t width, uint8_t levels)
{
    if (ch_state != PVA_MKT_DONE || !ch_n) return PVA_E_STATE;
    if (width < 8u || width > 240u || levels < 8u || levels > 120u) return PVA_E_ARGS;
    int32_t lo = pt_v[0], hi = pt_v[0];
    for (uint16_t i = 1; i < ch_n; i++) { if (pt_v[i] < lo) lo = pt_v[i]; if (pt_v[i] > hi) hi = pt_v[i]; }
    int32_t last = ch_have_price ? ch_price : pt_v[ch_n - 1];
    int32_t dlo = lo, dhi = hi;                         // the period's low and high
    if (ch_have_prev) { if (ch_prev < lo) lo = ch_prev; if (ch_prev > hi) hi = ch_prev; }
    int64_t span = (int64_t)hi - lo;
    if (span <= 0) span = 1;
    // the day: from the exchange's opening to its closing time when the reply has them
    uint32_t first = pt_t[0], lastt = pt_t[ch_n - 1];
    uint32_t t0 = ch_start && ch_start <= first ? ch_start : first;
    uint32_t t1 = ch_end > lastt ? ch_end : lastt;
    if (t1 - t0 > 2u * 86400u) { t0 = first; t1 = lastt; }
    uint32_t dur = t1 > t0 ? t1 - t0 : 1u;
    bool by_time = ch_range == 0;
    #define LEVEL(v) (uint8_t)(1 + ((int64_t)(v) - lo) * (int64_t)(levels - 1u) / span)
    row_t w = row(ch_row);
    char t[40], a[24], b[16];
    pva_reply_byte(width);
    pva_reply_byte(ch_have_prev ? LEVEL(ch_prev) : 0);
    pva_reply_str(w.name);
    fmt_num(t, sizeof t, last, ch_sc, w.dec, false);
    pva_reply_str(t);
    if (ch_have_prev)
    {
        fmt_num(a, sizeof a, (int64_t)last - ch_prev, ch_sc, w.dec, true);
        fmt_pct(b, sizeof b, pct100(last, ch_prev));
        snprintf(t, sizeof t, "%s  %s", a, b);
    }
    else t[0] = 0;
    pva_reply_str(t);
    fmt_num(t, sizeof t, dhi, ch_sc, w.dec, false);
    pva_reply_str(t);
    fmt_num(t, sizeof t, dlo, ch_sc, w.dec, false);
    pva_reply_str(t);
    if (ch_have_prev) fmt_num(t, sizeof t, ch_prev, ch_sc, w.dec, false); else t[0] = 0;
    pva_reply_str(t);
    // a column's price: the day by its time (between the points around it; none after the last
    // point), the other ranges point after point over the width
    uint16_t k = 0;
    for (uint8_t x = 0; x < width; x++)
    {
        int64_t v;
        if (by_time)
        {
            uint32_t tx = t0 + (uint32_t)(((uint64_t)dur * x) / (width - 1u));
            if (tx < first || tx > lastt) { pva_reply_byte(0); continue; }
            while (k + 1u < ch_n && pt_t[k + 1] <= tx) k++;
            v = pt_v[k];
            if (k + 1u < ch_n && pt_t[k + 1] > pt_t[k])
                v += ((int64_t)pt_v[k + 1] - pt_v[k]) * (int64_t)(tx - pt_t[k]) / (int64_t)(pt_t[k + 1] - pt_t[k]);
        }
        else
        {
            uint32_t f = ch_n > 1u ? (uint32_t)x * (ch_n - 1u) * 256u / (width - 1u) : 0;    // the place among the points, in 256ths
            k = (uint16_t)(f >> 8);
            v = pt_v[k];
            if (k + 1u < ch_n) v += ((int64_t)pt_v[k + 1] - pt_v[k]) * (int64_t)(f & 255u) / 256;
        }
        uint8_t h = LEVEL(v);
        pva_reply_byte((uint8_t)(h | (ch_have_prev && v < ch_prev ? 0x80u : 0u)));
    }
    // the price scale: round prices, 3 to 9 of them
    {
        int64_t step = 1, base = 1;
        static const uint8_t m125[] = { 1, 2, 5 };
        for (uint8_t e = 0; e < 54u; e++)
        {
            step = base * m125[e % 3u];
            if (span / step <= 8) break;
            if (e % 3u == 2u) base *= 10;
        }
        uint8_t zeros = 0;
        for (int64_t z = step; zeros < ch_sc && z % 10 == 0; z /= 10) zeros++;
        uint8_t dec = (uint8_t)(ch_sc - zeros);
        int64_t v0 = ((int64_t)lo + step - 1) / step * step;
        uint8_t n = 0;
        for (int64_t v = v0; v <= hi && n < 9u; v += step) n++;
        pva_reply_byte(n);
        n = 0;
        for (int64_t v = v0; v <= hi && n < 9u; v += step, n++)
        {
            pva_reply_byte(LEVEL(v));
            fmt_num(t, sizeof t, v, ch_sc, dec, false);
            pva_reply_str(t);
        }
    }
    // the time scale, in the place's time: the day's hours; the days of 5 days; a month's weeks; the
    // months; the years. A label where the unit changes, when there is room after the one before it
    {
        static const char *const wdn[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
        static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        uint8_t cols[8];
        char txt[8][8];
        uint8_t n = 0;
        int lastcol = -100;
        int y;
        uint8_t mo, d, h, mi, wd;
        if (by_time)
        {
            uint32_t hours = dur / 3600u, st = hours <= 10u ? 2u : hours <= 16u ? 3u : 6u;
            pva_clock_civil(t0, &y, &mo, &d, &h, &mi, &wd);
            uint32_t tt = t0 - (uint32_t)(h % st) * 3600u - (uint32_t)mi * 60u - t0 % 60u;       // the round hour at or before t0
            for (; tt <= t1 && n < 8u; tt += st * 3600u)
            {
                if (tt < t0) continue;
                pva_clock_civil(tt, &y, &mo, &d, &h, &mi, &wd);
                cols[n] = (uint8_t)(((uint64_t)(tt - t0) * (width - 1u)) / dur);
                snprintf(txt[n], sizeof txt[n], "%02u:00", (unsigned)h);
                n++;
            }
        }
        else
        {
            int years = 1, months = 1, y1;
            uint8_t mo1;
            pva_clock_civil(pt_t[ch_n - 1], &y1, &mo1, &d, &h, &mi, &wd);
            pva_clock_civil(pt_t[0], &y, &mo, &d, &h, &mi, &wd);
            int sp = y1 - y;
            if (ch_range == 7) years = sp <= 7 ? 1 : sp <= 14 ? 2 : sp <= 35 ? 5 : sp <= 70 ? 10 : 20;
            if ((y1 * 12 + mo1) - (y * 12 + mo) > 8) months = 2;        // (more than 8 months: every 2nd)
            long prev_key = -1;
            for (uint16_t i = 0; i < ch_n && n < 8u; i++)
            {
                pva_clock_civil(pt_t[i], &y, &mo, &d, &h, &mi, &wd);
                long key;
                char lab[8];
                switch (ch_range)
                {
                    case 1:  key = y * 400L + mo * 32L + d; snprintf(lab, sizeof lab, "%s", wdn[wd]); break;
                    case 2:  key = (long)((pt_t[i] / 86400u + 3u) / 7u); snprintf(lab, sizeof lab, "%u %s", (unsigned)d, mon[mo - 1]); break;
                    case 3: case 4: case 5: key = (y * 12L + mo - 1) / months; snprintf(lab, sizeof lab, "%s", mon[mo - 1]); break;
                    case 6:  key = y; snprintf(lab, sizeof lab, "%d", y); break;
                    default: key = y / years; snprintf(lab, sizeof lab, "%d", y); break;
                }
                if (key == prev_key) continue;
                bool first_pt = prev_key < 0;
                prev_key = key;
                // (the first point is the middle of its unit unless it starts one: no label there but for the days)
                if (first_pt && ch_range != 1) continue;
                int col = ch_n > 1u ? (int)((uint32_t)i * (width - 1u) / (ch_n - 1u)) : 0;
                if (col < lastcol + (int)(strlen(lab) + 1u) * 3) continue;
                cols[n] = (uint8_t)col;
                snprintf(txt[n], sizeof txt[n], "%s", lab);
                lastcol = col;
                n++;
            }
        }
        pva_reply_byte(n);
        for (uint8_t i = 0; i < n; i++) { pva_reply_byte(cols[i]); pva_reply_str(txt[i]); }
    }
    #undef LEVEL
    return PVA_OK;
}

int pva_markets_cmd(uint8_t code, const uint8_t *a)
{
    if (!available) return PVA_E_NOWIFI;
    switch (code)
    {
        case PVA_C_MKT_LISTS:     return cmd_lists();
        case PVA_C_MKT_REGION:    return cmd_region(a[0]);
        case PVA_C_MKT_OPEN:      return cmd_open(a[0]);
        case PVA_C_MKT_ROWS:      return cmd_rows(a[0], a[1]);
        case PVA_C_MKT_MEMBERS:   return cmd_members(a[0]);
        case PVA_C_MKT_NEWS:      return cmd_news(a[0]);
        case PVA_C_MKT_INFO:      return cmd_info();
        case PVA_C_MKT_CHART:     return cmd_chart(a[0], a[1]);
        case PVA_C_MKT_SORT:      return cmd_sort(a[0]);
        case PVA_C_MKT_TAPE:      return cmd_tape();
        case PVA_C_MKT_CHART_GET: return cmd_chart_get(a[0], a[1]);
        default:                  return PVA_E_UNKNOWN;
    }
}
