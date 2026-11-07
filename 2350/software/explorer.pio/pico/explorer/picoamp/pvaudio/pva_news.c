// MSX PICOVERSE PROJECT
// pva_news.c - PicoVerse Audio Engine: a news reader for PicoAmp.
//
// Favourite feeds: [news] feeds=code,code,... in PICOAMP.INI, the codes of the
// catalogue (pva_news_cat.c) or #n for a feed of one's own ([news] feedN =
// Name|URL); without the key a few world services. A feed is read as it comes
// in over the ESP-01 (the web client's slot): the <item>s' title,
// description, date and link (RSS 2.0 and RSS 1.0/RDF; Atom's <entry>,
// <summary>, <updated> and <link href> too). CDATA, entities and HTML are
// taken out, paragraphs kept, the text made plain ASCII; up to MAX_ITEMS
// stories are kept in memory taken from the arena when the first feed is
// asked for.
//
// The stories are shown newest first ([news] sort=date, the default) or in the
// feed's order (sort=feed); stories without a date keep their place after the
// dated ones. When there is no more room the oldest story goes for a newer one
// (a feed with the oldest first still shows its newest), so all of a feed is read.
//
// The full article: the item's <content:encoded> when the feed has it, else
// the page at its link, whose <p> paragraphs are taken (scripts, menus,
// headers and footers left out). It comes in while the MSX reads; the MSX
// asks for wrapped lines (NEWS_TEXT) and gets what is there so far.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pva_internal.h"

#define MAX_FAVS        20u
#define MAX_ITEMS       40u
#define POOL_WANT       24576u          // the stories (titles, summaries, links)
#define POOL_MIN        3072u
#define ART_WANT        16384u          // a full article's text
#define ART_MIN         2048u
#define MARGIN          3072u           // arena left for the rest
#define TITLE_MAX       160u
#define SUM_MAX         1200u
#define LINK_MAX        400u
#define DATE_MAX        40u
#define FIELD_MAX       2400u           // a field as it comes (with HTML), before it is cleaned
#define PAGE_MAX        (768u * 1024u)  // an article page: at most this much is read
#define FEED_MAX        (512u * 1024u)  // a feed: at most this much is read
#define FAVS_TEXT       320u

typedef struct { uint16_t title, text, link, raw; int64_t t; bool content; uint8_t pos; } item_t;   // pos: its place in the feed
static item_t   items[MAX_ITEMS];
static uint8_t  nitems;
static bool     full;                   // no more room: the rest of the feed is left
static char    *pool, *fbuf, *art;
static size_t   pool_size, pool_used, art_size;
static bool     mem_tried;
static uint8_t  st = PVA_NEWS_IDLE, feed_no = 0xFF, err;
static bool     pending;
static uint8_t  want_feed;

// the full article
static uint8_t  a_st = PVA_ART_IDLE, a_story = 0xFF, a_phase, a_pos;
static uint32_t f_got;                  // the feed's bytes read
static bool     a_pending, a_stop, a_part, a_force;
static size_t   a_len;
static uint32_t a_got;                  // the bytes of the feed or page handled (where the next piece starts)
// pieces: with the radio's TLS connection lent, a piece ends before the radio's buffer runs
// out; the radio fills it again, then the next piece comes from where this one ended (Range)
static bool     a_inweb, a_cut, a_wait, a_first, a_hold, a_esc;
static uint8_t  a_gen, a_wgen, a_empty, a_pieces;
static uint32_t a_new, a_skip, a_twait, a_thold;
// a feed in pieces too (as the article), from f_got on
static bool     f_cut, f_wait, f_first, f_norange;
static uint8_t  f_gen, f_wgen, f_empty;
static uint32_t f_new, f_twait;
static bool     radio_cut_at(uint32_t got_new);
static char     a_url[LINK_MAX + 1];
static uint8_t  a_feed;

// the XML parser
enum { X_TEXT, X_TAG, X_CDATA, X_COMMENT, X_SKIP };
enum { M_LIST, M_ART };
static uint8_t  xs, xmode, xitem;       // xitem: items seen (M_ART)
static char     tag[24];
static uint8_t  tlen;
static bool     tag_close, tag_skip, tag_link;
static uint8_t  cd_match;               // "]]>" seen so far
static char     cdpre[9];               // "![CDATA[" being matched
static uint8_t  cdpre_n;
static bool     in_item;
static uint8_t  field;                  // F_xxx
static size_t   flen;
static uint32_t clen;                   // <content:encoded>'s size (M_LIST)
static char     ent[10];
static uint8_t  elen;
static bool     in_ent;
static char     attrs[LINK_MAX + 40];
static size_t   alen;
static char     f_title[TITLE_MAX + 1], f_text[SUM_MAX + 1], f_link[LINK_MAX + 1], f_raw[DATE_MAX + 1];
static int64_t  f_t;
enum { F_NONE, F_TITLE, F_TEXT, F_DATE, F_LINK, F_CONTENT };

// ---------------------------------------------------------------- favourites
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int cat_find(const char *code, size_t n)
{
    for (int i = 0; pva_news_cat[i].name; i++)
    {
        const char *c = pva_news_cat[i].code;
        size_t k = 0;
        while (k < n && c[k] && lc(code[k]) == c[k]) k++;
        if (k == n && !c[k]) return i;
    }
    return -1;
}

// the favourites as text: "code,code,#2"
static void favs_get(char *s, size_t n)
{
    if (pva_cfg_get("news.feeds", s, n)) return;
    snprintf(s, n, "%s", pva_news_default_favs);
    for (unsigned k = 1; k <= 9; k++)
    {
        char key[16], v[8];
        snprintf(key, sizeof key, "news.feed%u", k);
        if (pva_cfg_get(key, v, sizeof v) && v[0])
        {
            size_t l = strlen(s);
            snprintf(s + l, n - l, ",#%u", k);
        }
    }
}

// the i-th code in the list: its start and length (false: no such one)
static bool tok(const char *s, uint8_t i, const char **t, size_t *tn)
{
    uint8_t k = 0;
    while (*s)
    {
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        const char *b = s;
        while (*s && *s != ',') s++;
        const char *e = s;
        while (e > b && e[-1] == ' ') e--;
        if (k++ == i) { *t = b; *tn = (size_t)(e - b); return true; }
    }
    return false;
}

static uint8_t tok_count(const char *s)
{
    const char *t;
    size_t n;
    uint8_t k = 0;
    while (k < 255 && tok(s, k, &t, &n)) k++;
    return k;
}

// a code to its name and URL
static bool resolve(const char *t, size_t n, char *name, size_t ns, char *url, size_t us)
{
    if (n >= 2 && t[0] == '#')
    {
        char key[16], v[LINK_MAX + 48];
        snprintf(key, sizeof key, "news.feed%.*s", (int)(n - 1 > 3 ? 3 : n - 1), t + 1);
        if (!pva_cfg_get(key, v, sizeof v)) return false;
        const char *bar = strchr(v, '|');
        if (!bar) return false;
        size_t k = (size_t)(bar - v);
        if (k >= ns) k = ns - 1;
        memcpy(name, v, k);
        name[k] = 0;
        const char *u = bar + 1;
        while (*u == ' ') u++;
        snprintf(url, us, "%s", u);
    }
    else
    {
        int c = cat_find(t, n);
        if (c < 0) return false;
        snprintf(name, ns, "%s", pva_news_cat[c].name);
        snprintf(url, us, "%s", pva_news_cat[c].url);
    }
    return name[0] && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8));
}

static bool fav(uint8_t i, char *name, size_t ns, char *url, size_t us)
{
    char s[FAVS_TEXT];
    favs_get(s, sizeof s);
    const char *t;
    size_t n;
    return tok(s, i, &t, &n) && resolve(t, n, name, ns, url, us);
}

static bool in_favs(const char *s, const char *code)
{
    const char *t;
    size_t n;
    for (uint8_t i = 0; tok(s, i, &t, &n); i++)
        if (n == strlen(code) && cat_find(t, n) >= 0 && !strcmp(pva_news_cat[cat_find(t, n)].code, code)) return true;
    return false;
}

static int favs_put(const char *s)
{
    return pva_cfg_set("news.feeds", s) ? PVA_OK : PVA_E_FULL;
}

static int news_feeds(void)
{
    char s[FAVS_TEXT], name[40], url[LINK_MAX + 1];
    favs_get(s, sizeof s);
    uint8_t n = tok_count(s);
    pva_reply_byte(n);
    const char *t;
    size_t tn;
    for (uint8_t i = 0; i < n; i++)
    {
        tok(s, i, &t, &tn);
        if (!resolve(t, tn, name, sizeof name, url, sizeof url)) snprintf(name, sizeof name, "? %.*s", (int)(tn > 30 ? 30 : tn), t);
        pva_reply_str(name);
    }
    return PVA_OK;
}

static int news_topics(void)
{
    uint8_t n = 0;
    while (pva_news_topics[n].name) n++;
    pva_reply_byte(n);
    for (uint8_t i = 0; i < n; i++) pva_reply_str(pva_news_topics[i].name);
    return PVA_OK;
}

// the topic's feeds: "*Name" (a favourite) or " Name"
static int news_cat(uint8_t topic)
{
    uint8_t n = 0;
    for (int i = 0; pva_news_cat[i].name; i++) if (pva_news_cat[i].topic == topic) n++;
    if (!n) return PVA_E_RANGE;
    char s[FAVS_TEXT], e[48];
    favs_get(s, sizeof s);
    pva_reply_byte(n);
    for (int i = 0; pva_news_cat[i].name; i++)
        if (pva_news_cat[i].topic == topic)
        {
            snprintf(e, sizeof e, "%c%s", in_favs(s, pva_news_cat[i].code) ? '*' : ' ', pva_news_cat[i].name);
            pva_reply_str(e);
        }
    return PVA_OK;
}

static int news_add(uint8_t topic, uint8_t idx)
{
    int c = -1;
    for (int i = 0, k = 0; pva_news_cat[i].name; i++)
        if (pva_news_cat[i].topic == topic && k++ == idx) { c = i; break; }
    if (c < 0) return PVA_E_RANGE;
    char s[FAVS_TEXT];
    favs_get(s, sizeof s);
    if (in_favs(s, pva_news_cat[c].code)) return PVA_E_EXISTS;
    if (tok_count(s) >= MAX_FAVS) return PVA_E_FULL;
    size_t l = strlen(s);
    if (l + strlen(pva_news_cat[c].code) + 2 > sizeof s) return PVA_E_FULL;
    snprintf(s + l, sizeof s - l, "%s%s", l ? "," : "", pva_news_cat[c].code);
    return favs_put(s);
}

// the list without its i-th code, or with codes i and j swapped
static int favs_edit(uint8_t i, int8_t dir)
{
    char s[FAVS_TEXT], o[FAVS_TEXT];
    favs_get(s, sizeof s);
    uint8_t n = tok_count(s);
    if (i >= n) return PVA_E_RANGE;
    int j = dir ? (int)i + dir : -1;
    if (dir && (j < 0 || j >= n)) return PVA_E_RANGE;
    size_t ol = 0;
    o[0] = 0;
    for (uint8_t k = 0; k < n; k++)
    {
        uint8_t from = k;
        if (dir && k == i) from = (uint8_t)j;
        else if (dir && k == j) from = i;
        else if (!dir && k == i) continue;
        const char *t;
        size_t tn;
        tok(s, from, &t, &tn);
        ol += (size_t)snprintf(o + ol, sizeof o - ol, "%s%.*s", ol ? "," : "", (int)tn, t);
        if (ol >= sizeof o) return PVA_E_FULL;
    }
    // the stories shown belong to a feed whose number may have moved
    if (feed_no != 0xFF && st == PVA_NEWS_DONE)
    {
        if (!dir) { if (feed_no == i) feed_no = 0xFE; else if (feed_no > i) feed_no--; }
        else if (feed_no == i) feed_no = (uint8_t)j;
        else if (feed_no == j) feed_no = i;
    }
    return favs_put(o);
}

// ---------------------------------------------------------------- memory, dates
// the Nextor modes: the memory from the stream RAM, at the start
void pva_news_init(size_t keep)
{
    if (keep < FIELD_MAX + 1u + POOL_MIN + ART_MIN + 16u) return;
    size_t r = keep - FIELD_MAX - 1u - 16u;
    size_t a = r * 2u / 5u;
    fbuf = pva_net_spare_take(FIELD_MAX + 1u);
    pool = fbuf ? pva_net_spare_take(r - a) : NULL;
    art = pool ? pva_net_spare_take(a) : NULL;
    if (!pool || !art) { fbuf = pool = art = NULL; return; }
    pool_size = r - a;
    art_size = a;
    mem_tried = true;
    pva_log("news: %u bytes for stories, %u for an article (stream RAM)", (unsigned)pool_size, (unsigned)art_size);
}

static bool news_mem(void)
{
    if (pool) return true;
    if (mem_tried) return false;
    mem_tried = true;
    size_t fr = pva_arena_free();
    if (fr < MARGIN + FIELD_MAX + 1u + POOL_MIN) { pva_log("news: no memory (%u)", (unsigned)fr); return false; }
    size_t r = fr - MARGIN - FIELD_MAX - 1u;
    pool_size = r * 3u / 5u;
    if (pool_size > POOL_WANT) pool_size = POOL_WANT;
    if (pool_size < POOL_MIN) pool_size = POOL_MIN;
    art_size = r - pool_size;
    if (art_size > ART_WANT) art_size = ART_WANT;
    if (art_size < ART_MIN) art_size = 0;
    fbuf = pva_alloc(FIELD_MAX + 1u);
    pool = fbuf ? pva_alloc(pool_size) : NULL;
    art = pool && art_size ? pva_alloc(art_size) : NULL;
    if (!art) art_size = 0;
    pva_log("news: %u bytes for stories, %u for an article", (unsigned)pool_size, (unsigned)art_size);
    return pool != NULL;
}

static const char mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
static const char wdays[] = "MonTueWedThuFriSatSun";

// "29 Oct 2026 20:15:00 GMT" / "+0100" / "EST", "2026-10-29T18:30:00Z" / "+01:00" -> UTC seconds (0: not understood)
static int64_t parse_date(const char *s)
{
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    const char *z = NULL;
    while (*s == ' ') s++;
    if (s[0] >= '0' && s[0] <= '9' && s[4] == '-')
    {
        if (sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &hh, &mm) < 5) return 0;
        const char *p = s + 16;
        if (*p == ':') { ss = atoi(p + 1); p += 3; }
        while (*p == '.' || (*p >= '0' && *p <= '9')) p++;
        z = p;
    }
    else
    {
        const char *c = strchr(s, ',');
        if (c && c - s < 5) s = c + 1;
        char mon[4] = "";
        int n = 0;
        if (sscanf(s, " %d %3s %d %d:%d%n", &d, mon, &y, &hh, &mm, &n) < 5) return 0;
        const char *p = s + n;
        if (*p == ':') { ss = atoi(p + 1); p += 3; }
        const char *mp = strstr(mons, mon);
        if (!mon[0] || !mp || (mp - mons) % 3) return 0;
        mo = (int)((mp - mons) / 3 + 1);
        while (*p == ' ') p++;
        z = p;
    }
    if (y < 100) y += 2000;
    if (y < 1990 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23 || mm > 59) return 0;
    int64_t t = pva_days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + hh * 3600 + mm * 60 + ss;
    int off = 0;
    if (z && (*z == '+' || *z == '-'))
    {
        int h = 0, m = 0;
        if (z[3] == ':') sscanf(z + 1, "%2d:%2d", &h, &m);
        else { int v = atoi(z + 1); h = v / 100; m = v % 100; }
        off = (h * 60 + m) * 60;
        if (*z == '-') off = -off;
    }
    else if (z)
    {
        static const struct { char n[5]; int8_t h; } zs[] = { { "EST", -5 }, { "EDT", -4 }, { "CST", -6 }, { "CDT", -5 },
            { "MST", -7 }, { "MDT", -6 }, { "PST", -8 }, { "PDT", -7 }, { "CET", 1 }, { "CEST", 2 }, { "BST", 1 } };
        for (unsigned i = 0; i < sizeof zs / sizeof zs[0]; i++)
            if (!strncmp(z, zs[i].n, strlen(zs[i].n))) { off = zs[i].h * 3600; break; }
    }
    return t - off;
}

// a story's time: "20:15" today, else "29 Oct"; long: "Thu 29 Oct 2026 21:15"
static void fmt_date(const item_t *it, char *o, size_t n, bool lng)
{
    if (!it->t)
    {
        snprintf(o, n, "%.*s", lng ? 40 : 5, it->raw != 0xFFFF ? pool + it->raw : "");
        return;
    }
    int64_t now;
    int32_t off;
    if (!pva_clock_utc(&now, &off)) { now = 0; off = 0; }
    int64_t t = it->t + off;
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int32_t rem = (int32_t)(t - days * 86400);
    int y;
    unsigned m, d;
    pva_civil_from_days(days, &y, &m, &d);
    unsigned wd = (unsigned)(((days % 7) + 7 + 3) % 7);
    int64_t nl = now + off;
    int64_t today = nl >= 0 ? nl / 86400 : (nl - 86399) / 86400;
    if (lng) snprintf(o, n, "%.3s %u %.3s %d %02d:%02d", wdays + wd * 3, d, mons + (m - 1) * 3, y, (int)(rem / 3600), (int)(rem / 60 % 60));
    else if (now && days == today) snprintf(o, n, "%02d:%02d", (int)(rem / 3600), (int)(rem / 60 % 60));
    else snprintf(o, n, "%2u %.3s", d, mons + (m - 1) * 3);
}

// ---------------------------------------------------------------- HTML to text
// Both the full article (feed content or a web page, into 'art') and a field
// being cleaned (into a small buffer) go through this.
enum { H_TEXT, H_TAG, H_ATTR, H_COMMENT, H_RAW, H_ENT };
typedef struct
{
    char    *out;
    size_t  *len, size;
    bool     page;              // a web page: only <p> text, not in menus etc.
    uint8_t  s;
    char     tag[12];
    uint8_t  tl;
    bool     close, quote_d, quote_s;
    char     raw[12];           // "script" while in it
    uint8_t  rm;                // "</script" matched so far
    uint8_t  cm;                // "-->" matched so far
    char     ent[10];
    uint8_t  el;
    uint32_t u8;                // a UTF-8 sequence being put together
    uint8_t  u8n;
    bool     sp;                // a space is due before the next character
    size_t   para;              // where the paragraph began
    uint8_t  inp, skip;         // page: in <p>, in a menu/header/footer...
    bool     article, done, full;
} html_t;
static html_t H;

static void h_start(char *out, size_t *len, size_t size, bool page)
{
    memset(&H, 0, sizeof H);
    H.out = out;
    H.len = len;
    H.size = size;
    H.page = page;
    *len = 0;
}

static bool h_on(void) { return !H.page || (H.inp && !H.skip); }

static void h_emit(char a)
{
    if (!h_on() || H.full) return;
    if (a == ' ') { if (*H.len > H.para) H.sp = true; return; }
    if (H.sp && *H.len + 1 < H.size) H.out[(*H.len)++] = ' ';
    H.sp = false;
    if (*H.len + 1 >= H.size) { H.full = true; return; }
    H.out[(*H.len)++] = a;
}

static void h_cp(uint32_t c)
{
    if (c == '\r' || c == '\n' || c == '\t' || c == 0xA0) c = ' ';
    char a = pva_fold_ascii(c);
    if (a) h_emit(a);
}

// the end of a paragraph: short ones on a web page are left out (buttons, captions)
static void h_para(void)
{
    H.sp = false;
    size_t n = *H.len - H.para;
    if (H.page && n && n < 40) *H.len = H.para;
    else if (n && *H.len + 1 < H.size) H.out[(*H.len)++] = '\n';
    H.para = *H.len;
}

static bool tag_is(const char *t, const char *const *l)
{
    for (; *l; l++) if (!strcmp(t, *l)) return true;
    return false;
}

static void h_tag(void)
{
    static const char *const blocks[] = { "p", "br", "div", "li", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "tr", "ul", "ol", "section", "article", "table", "hr", NULL };
    static const char *const raws[] = { "script", "style", "noscript", "svg", "template", "iframe", "head", NULL };
    static const char *const skips[] = { "nav", "header", "footer", "aside", "form", "figure", "figcaption", "button", "label", "select", "menu", NULL };
    H.tag[H.tl] = 0;
    const char *t = H.tag;
    if (!t[0]) return;
    if (!H.close && tag_is(t, raws)) { snprintf(H.raw, sizeof H.raw, "%s", t); H.rm = 0; H.s = H_RAW; return; }
    if (H.page)
    {
        if (tag_is(t, skips)) { if (H.close) { if (H.skip) H.skip--; } else if (H.skip < 200) H.skip++; }
        if (!strcmp(t, "p")) { if (H.close) { h_para(); if (H.inp) H.inp--; return; } h_para(); H.inp = 1; return; }
        if (!strcmp(t, "article")) { if (!H.close) H.article = true; else if (H.article && *H.len >= 600) H.done = true; }
    }
    if (tag_is(t, blocks)) h_para();
}

static void h_entity(void)
{
    H.ent[H.el] = 0;
    uint32_t cp = 0;
    if (H.ent[0] == '#') cp = H.ent[1] == 'x' || H.ent[1] == 'X' ? (uint32_t)strtoul(H.ent + 2, NULL, 16) : (uint32_t)strtoul(H.ent + 1, NULL, 10);
    else
    {
        static const struct { char n[7]; uint16_t c; } es[] = { { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
            { "apos", '\'' }, { "nbsp", ' ' }, { "rsquo", '\'' }, { "lsquo", '\'' }, { "rdquo", '"' }, { "ldquo", '"' },
            { "ndash", '-' }, { "mdash", '-' }, { "hellip", 0x2026 }, { "euro", 0x20AC }, { "eacute", 0xE9 },
            { "egrave", 0xE8 }, { "euml", 0xEB }, { "auml", 0xE4 }, { "ouml", 0xF6 }, { "uuml", 0xFC }, { "szlig", 0xDF },
            { "iuml", 0xEF }, { "ccedil", 0xE7 }, { "aacute", 0xE1 }, { "oacute", 0xF3 }, { "iacute", 0xED },
            { "ntilde", 0xF1 }, { "copy", 'c' }, { "shy", 0 } };
        for (unsigned i = 0; i < sizeof es / sizeof es[0]; i++) if (!strcmp(H.ent, es[i].n)) { cp = es[i].c; if (!cp) return; break; }
        if (!cp) { h_emit('&'); for (uint8_t i = 0; i < H.el; i++) h_emit(H.ent[i]); h_emit(';'); return; }
    }
    if (cp == 0x2026) { h_emit('.'); h_emit('.'); h_emit('.'); return; }
    if (cp == 0x20AC) { h_emit('E'); h_emit('U'); h_emit('R'); return; }
    h_cp(cp);
}

static void h_byte(uint8_t c)
{
    switch (H.s)
    {
        case H_TEXT:
            if (c == '<') { H.s = H_TAG; H.tl = 0; H.close = false; return; }
            if (c == '&') { H.s = H_ENT; H.el = 0; return; }
            if (c < 0x80) { H.u8n = 0; h_cp(c); return; }
            if (c >= 0xC0) { H.u8 = c & (c >= 0xF0 ? 0x07 : c >= 0xE0 ? 0x0F : 0x1F); H.u8n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1; return; }
            if (H.u8n) { H.u8 = H.u8 << 6 | (c & 0x3F); if (!--H.u8n) { if (H.u8 == 0x2026) { h_emit('.'); h_emit('.'); h_emit('.'); } else if (H.u8 == 0x20AC) { h_emit('E'); h_emit('U'); h_emit('R'); } else h_cp(H.u8); } }
            return;
        case H_ENT:
            if (c == ';') { H.s = H_TEXT; h_entity(); return; }
            if (H.el < sizeof H.ent - 1 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#')) { H.ent[H.el++] = (char)c; return; }
            H.s = H_TEXT;
            h_emit('&');
            for (uint8_t i = 0; i < H.el; i++) h_emit(H.ent[i]);
            h_byte(c);
            return;
        case H_TAG:
            if (H.tl == 0 && !H.close && c == '/') { H.close = true; return; }
            if (H.tl == 0 && c == '!') { H.s = H_COMMENT; H.cm = 0; return; }
            if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/')
            {
                H.s = c == '>' ? H_TEXT : H_ATTR;
                H.quote_d = H.quote_s = false;
                h_tag();
                if (H.s == H_RAW && c != '>') H.s = H_ATTR, H.raw[sizeof H.raw - 1] = 1;   // (raw after the attributes)
                return;
            }
            if (H.tl < sizeof H.tag - 1) H.tag[H.tl++] = lc((char)c);
            return;
        case H_ATTR:
            if (c == '"' && !H.quote_s) H.quote_d = !H.quote_d;
            else if (c == '\'' && !H.quote_d) H.quote_s = !H.quote_s;
            else if (c == '>' && !H.quote_d && !H.quote_s)
            {
                if (H.raw[sizeof H.raw - 1] == 1) { H.raw[sizeof H.raw - 1] = 0; H.s = H_RAW; H.rm = 0; }
                else H.s = H_TEXT;
            }
            return;
        case H_COMMENT:                 // <!-- ... --> (cm 10..12: dashes seen) and <!DOCTYPE ...> (cm FF)
            if (H.cm == 0) { H.cm = c == '-' ? 1 : 0xFF; if (c == '>') H.s = H_TEXT; return; }
            if (H.cm == 0xFF) { if (c == '>') H.s = H_TEXT; return; }
            if (H.cm == 1) { H.cm = c == '-' ? 10 : 0xFF; return; }
            if (c == '-') { if (H.cm < 12) H.cm++; return; }
            if (c == '>' && H.cm == 12) { H.s = H_TEXT; return; }
            H.cm = 10;
            return;
        case H_RAW:                     // to "</script"
        {
            size_t rl = strlen(H.raw);
            char want = H.rm == 0 ? '<' : H.rm == 1 ? '/' : H.raw[H.rm - 2];
            if (lc((char)c) == want) { if (++H.rm == rl + 2) { H.s = H_ATTR; H.quote_d = H.quote_s = false; H.raw[0] = 0; } }
            else H.rm = c == '<' ? 1 : 0;
            return;
        }
    }
}

// HTML of a field (entities decoded by the XML reader already) to text with paragraphs
static void clean(char *dst, size_t size, const char *s, size_t n)
{
    size_t len = 0;
    html_t save = H;                    // (an article may be coming in meanwhile)
    h_start(dst, &len, size, false);
    for (size_t i = 0; i < n; i++) h_byte((uint8_t)s[i]);
    while (len && (dst[len - 1] == '\n' || dst[len - 1] == ' ')) len--;
    dst[len] = 0;
    H = save;
}

// ---------------------------------------------------------------- the feed
static uint16_t pool_add(const char *s)
{
    size_t n = strlen(s) + 1;
    if (pool_used + n > pool_size) return 0xFFFF;
    memcpy(pool + pool_used, s, n);
    uint16_t o = (uint16_t)pool_used;
    pool_used += n;
    return o;
}

static void item_start(void)
{
    in_item = true;
    f_title[0] = f_text[0] = f_link[0] = f_raw[0] = 0;
    f_t = 0;
    clen = 0;
}

// the story ending at the pool's next one (or the pool's end) out: the stories after it move down
static void item_drop(uint8_t k)
{
    size_t b = items[k].title;
    size_t e = k + 1u < nitems ? items[k + 1u].title : pool_used;
    size_t n = e - b;
    memmove(pool + b, pool + e, pool_used - e);
    pool_used -= n;
    for (uint8_t j = (uint8_t)(k + 1u); j < nitems; j++)
    {
        items[j].title -= (uint16_t)n;
        items[j].text -= (uint16_t)n;
        items[j].link -= (uint16_t)n;
        if (items[j].raw != 0xFFFF) items[j].raw -= (uint16_t)n;
        items[j - 1u] = items[j];
    }
    nitems--;
}

// the oldest story with a date (-1: none)
static int item_oldest(void)
{
    int o = -1;
    for (uint8_t i = 0; i < nitems; i++)
        if (items[i].t && (o < 0 || items[i].t < items[o].t)) o = i;
    return o;
}

static void item_end(void)
{
    in_item = false;
    uint8_t pos = xitem;
    if (xitem < 255) xitem++;
    if (xmode == M_ART)
    {
        if (pos == a_pos) a_stop = true;    // its content is in
        return;
    }
    if (!f_title[0]) return;
    size_t need = strlen(f_title) + strlen(f_text) + strlen(f_link) + 3u + (!f_t && f_raw[0] ? strlen(f_raw) + 1u : 0u);
    if (need > pool_size) return;
    // no room: the oldest goes, when this one is newer (else this one is left out)
    while (nitems >= MAX_ITEMS || pool_used + need > pool_size)
    {
        int o = item_oldest();
        if (o < 0 || !f_t || f_t <= items[o].t) { full = true; return; }
        item_drop((uint8_t)o);
    }
    item_t it;
    it.title = pool_add(f_title);
    it.text = pool_add(f_text);
    it.link = pool_add(f_link);
    it.t = f_t;
    it.raw = !f_t && f_raw[0] ? pool_add(f_raw) : 0xFFFF;
    it.content = clen > strlen(f_text) + 400u;
    it.pos = pos;
    items[nitems++] = it;
}

// the order: newest first (stories without a date after them, as in the feed), or the feed's
static bool sort_by_date(void)
{
    char v[8];
    return !(pva_cfg_get("news.sort", v, sizeof v) && !strcmp(v, "feed"));
}

static bool item_before(const item_t *a, const item_t *b, bool by_date)
{
    if (by_date && (a->t || b->t))
    {
        if (!a->t) return false;
        if (!b->t) return true;
        if (a->t != b->t) return a->t > b->t;
    }
    return a->pos < b->pos;
}

static void items_sort(void)
{
    bool d = sort_by_date();
    uint8_t ap = a_story < nitems ? items[a_story].pos : 0xFF;
    for (uint8_t i = 1; i < nitems; i++)
    {
        item_t x = items[i];
        uint8_t j = i;
        while (j && item_before(&x, &items[j - 1u], d)) { items[j] = items[j - 1u]; j--; }
        items[j] = x;
    }
    if (ap != 0xFF)                     // (the article's story: where it is now)
        for (uint8_t i = 0; i < nitems; i++) if (items[i].pos == ap) { a_story = i; break; }
}

static int news_sort(uint8_t how)
{
    bool d = sort_by_date();
    if (how != 0xFF)
    {
        bool want = how == 2 ? !d : how != 0;
        if (!pva_cfg_set("news.sort", want ? "date" : "feed")) return PVA_E_FULL;
        if (st == PVA_NEWS_DONE) items_sort();
        d = want;
    }
    pva_reply_byte(d ? 1 : 0);
    return PVA_OK;
}

static void field_end(void)
{
    fbuf[flen] = 0;
    if (xmode == M_LIST)
    {
        if (field == F_TITLE) { clean(f_title, sizeof f_title, fbuf, flen); for (char *p = f_title; *p; p++) if (*p == '\n') *p = ' '; }
        else if (field == F_TEXT && !f_text[0]) clean(f_text, sizeof f_text, fbuf, flen);
        else if (field == F_LINK && !f_link[0])
        {
            char *p = fbuf;
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
            size_t k = strlen(p);
            while (k && (p[k - 1] == ' ' || p[k - 1] == '\n' || p[k - 1] == '\r' || p[k - 1] == '\t')) k--;
            snprintf(f_link, sizeof f_link, "%.*s", (int)k, p);
        }
        else if (field == F_DATE && !f_t)
        {
            f_t = parse_date(fbuf);
            if (!f_t) clean(f_raw, sizeof f_raw, fbuf, flen);
        }
    }
    else if (field == F_CONTENT) h_para();
    field = F_NONE;
    flen = 0;
}

static void put(char c)
{
    if (field == F_CONTENT)
    {
        if (xmode == M_LIST) clen++;
        else h_byte((uint8_t)c);
        return;
    }
    if (field && flen < FIELD_MAX) fbuf[flen++] = c;
}

static void put_cp(uint32_t cp)
{
    if (cp < 0x80) put((char)cp);
    else if (cp < 0x800) { put((char)(0xC0 | (cp >> 6))); put((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { put((char)(0xE0 | (cp >> 12))); put((char)(0x80 | ((cp >> 6) & 0x3F))); put((char)(0x80 | (cp & 0x3F))); }
}

// an XML entity: the character goes on as text (in HTML fields: as HTML, so
// "&lt;b&gt;" is a tag and "&amp;amp;" an ampersand)
static void entity(void)
{
    ent[elen] = 0;
    if (ent[0] == '#')
    {
        uint32_t cp = ent[1] == 'x' || ent[1] == 'X' ? (uint32_t)strtoul(ent + 2, NULL, 16) : (uint32_t)strtoul(ent + 1, NULL, 10);
        if (cp == '&' && field != F_LINK) { put('&'); put('a'); put('m'); put('p'); put(';'); }   // (an ampersand in HTML)
        else put_cp(cp);
    }
    else if (!strcmp(ent, "amp")) put('&');
    else if (!strcmp(ent, "lt")) put('<');
    else if (!strcmp(ent, "gt")) put('>');
    else if (!strcmp(ent, "quot")) put('"');
    else if (!strcmp(ent, "apos")) put('\'');
    else { put('&'); for (uint8_t i = 0; i < elen; i++) put(ent[i]); put(';'); }
}

static void text_char(char c)
{
    if (!field) return;
    if (in_ent)
    {
        if (c == ';') { in_ent = false; entity(); return; }
        if (elen < sizeof ent - 1 && c != ' ' && c != '<') { ent[elen++] = c; return; }
        in_ent = false;
        put('&');
        for (uint8_t i = 0; i < elen; i++) put(ent[i]);
    }
    if (c == '&') { in_ent = true; elen = 0; return; }
    put(c);
}

// <link href="..." rel="alternate"/> (Atom)
static void link_attrs(void)
{
    attrs[alen] = 0;
    const char *r = strstr(attrs, "rel=");
    if (r && strncmp(r + 5, "alternate", 9)) return;
    const char *h = strstr(attrs, "href=");
    if (!h || (h[5] != '"' && h[5] != '\'')) return;
    char q = h[5];
    const char *b = h + 6, *e = strchr(b, q);
    if (!e || f_link[0]) return;
    snprintf(f_link, sizeof f_link, "%.*s", (int)(e - b), b);
}

static void tag_done(void)
{
    tag[tlen] = 0;
    // the name without a namespace prefix ("dc:date" -> "date")
    const char *t = strchr(tag, ':');
    bool ns = t != NULL;
    t = t ? t + 1 : tag;
    tag_link = false;
    if (!tag_close)
    {
        if (!strcmp(t, "item") || !strcmp(t, "entry")) item_start();
        else if (in_item && !tag_skip)
        {
            uint8_t f = !strcmp(t, "title") ? F_TITLE : (!strcmp(t, "description") || !strcmp(t, "summary")) ? F_TEXT :
                        (!strcmp(t, "pubDate") || !strcmp(t, "date") || !strcmp(t, "updated") || !strcmp(t, "published")) ? F_DATE :
                        (!strcmp(t, "link") && !ns) ? F_LINK :
                        (!strcmp(t, "encoded") || (!strcmp(t, "content") && !ns)) ? F_CONTENT : F_NONE;
            if (f == F_DATE && f_t) f = F_NONE;
            if (f == F_CONTENT && xmode == M_ART && xitem != a_pos) f = F_NONE;
            if (f) { field = f; flen = 0; in_ent = false; }
            if (f == F_LINK) { tag_link = true; alen = 0; }
        }
    }
    else
    {
        if (field) field_end();
        if (!strcmp(t, "item") || !strcmp(t, "entry")) item_end();
    }
}

static void xml_byte(char c)
{
    switch (xs)
    {
        case X_TEXT:
            if (c == '<') { xs = X_TAG; tlen = 0; tag_close = false; tag_skip = false; cdpre_n = 0; return; }
            text_char(c);
            return;
        case X_TAG:
            if (tlen == 0 && !tag_close && cdpre_n == 0 && c == '/') { tag_close = true; return; }
            if (tlen == 0 && !tag_close && (c == '!' || cdpre_n))
            {
                // <![CDATA[ ... ]]> or <!-- ... -->
                if (cdpre_n < 8) cdpre[cdpre_n++] = c;
                cdpre[cdpre_n] = 0;
                if (!strcmp(cdpre, "![CDATA[")) { xs = X_CDATA; cd_match = 0; return; }
                if (!strcmp(cdpre, "!--")) { xs = X_COMMENT; cd_match = 0; return; }
                if (strncmp("![CDATA[", cdpre, cdpre_n) && strncmp("!--", cdpre, cdpre_n)) xs = X_SKIP;
                return;
            }
            if (c == '?') { xs = X_SKIP; return; }
            if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/')
            {
                if (c == '/' && !tag_close) tag_skip = true;     // <tag/>: empty
                tag_done();
                if (c == '>') { xs = X_TEXT; if (tag_skip && field) field_end(); }
                else xs = X_SKIP;
                return;
            }
            if (tlen < sizeof tag - 1) tag[tlen++] = c;
            return;
        case X_SKIP:                    // the rest of a tag (attributes)
            if (tag_link && alen < sizeof attrs - 1) attrs[alen++] = c;
            if (c == '/') tag_skip = true;
            else if (c == '>')
            {
                xs = X_TEXT;
                if (tag_link) { link_attrs(); tag_link = false; }
                if (tag_skip && field && !tag_close) field_end();
            }
            else if (c != ' ') tag_skip = false;
            return;
        case X_CDATA:
            if (c == ']') { if (cd_match < 2) cd_match++; else put(']'); return; }
            if (c == '>' && cd_match == 2) { xs = X_TEXT; return; }
            while (cd_match) { put(']'); cd_match--; }
            put(c);
            return;
        case X_COMMENT:
            if (c == '-') { if (cd_match < 2) cd_match++; return; }
            if (c == '>' && cd_match == 2) { xs = X_TEXT; return; }
            cd_match = 0;
            return;
    }
}

static void xml_reset(uint8_t mode)
{
    xs = X_TEXT;
    xmode = mode;
    xitem = 0;
    in_item = false;
    field = F_NONE;
    flen = 0;
    in_ent = false;
    tag_link = false;
}

static bool body(const uint8_t *p, size_t n)
{
    if (f_wgen != f_gen) return true;   // (a feed no longer wanted)
    if (f_first)
    {
        f_first = false;
        if (f_got && pva_web_http() == 200) { f_norange = true; return true; }   // (it would come again)
    }
    if (radio_cut_at(f_new)) { f_cut = true; return true; }
    for (size_t i = 0; i < n; i++) xml_byte((char)p[i]);
    f_got += (uint32_t)n;
    f_new += (uint32_t)n;
    return f_got >= FEED_MAX;           // (all of it, else)
}

static void fetch_done(bool ok)
{
    if (f_wgen != f_gen) return;
    if (f_norange) { f_norange = false; ok = true; pva_log("news: the feed's server does not do Range: %u stories", (unsigned)nitems); }
    else if (f_cut)
    {
        // a piece: the radio fills its buffer, then the rest of the feed (Range)
        f_cut = false;
        f_empty = f_new < 512u ? (uint8_t)(f_empty + 1u) : 0;
        pva_log("news: feed piece, %lu bytes, %u stories", (unsigned long)f_new, (unsigned)nitems);
        if (f_empty < 3u) { f_wait = true; f_twait = pva_hal_millis(); return; }
        ok = true;
    }
    if (ok || nitems)
    {
        items_sort();
        st = PVA_NEWS_DONE;
        err = 0;
    }
    else
    {
        st = PVA_NEWS_FAILED;
        err = pva_web_error() ? pva_web_error() : PVA_NERR_HTTP;
    }
    pva_log("news: %u stories (%s)", (unsigned)nitems, ok ? "ok" : "failed");
}

// ---------------------------------------------------------------- the article
#define PIECE_MIN       2048u           // a piece reads at least this much
#define RADIO_WAIT_MS   15000u          // the longest wait for the radio's buffer between pieces

static uint32_t radio_bps(void)         // the stream's bytes per second
{
    pva_track_info_t i;
    memset(&i, 0, sizeof i);
    if (pva_dec_is_stream()) pva_dec_frame_info(&i);
    return i.bitrate_kbps ? (uint32_t)i.bitrate_kbps * 125u : 16000u;
}

// what the radio's buffer must keep for its reconnect (DNS cached, TLS open, HTTP head)
static size_t radio_reserve(void)
{
    size_t cap = pva_net_capacity();
    const pva_net_stats_t *ns = pva_net_stats();
    uint32_t ms = ns->back_ms && ns->back_ms < 10000u ? ns->back_ms + 1000u :       // (measured)
                  (ns->open_ms && ns->open_ms < 8000u ? ns->open_ms : 2500u) + 1500u;
    size_t need = (size_t)(radio_bps() * ms / 1000u);
    if (need < cap / 4u) need = cap / 4u;
    if (need > cap * 3u / 4u) need = cap * 3u / 4u;
    return need;
}

// the radio lent its TLS connection and its buffer gets low: this piece ends
static bool radio_cut_at(uint32_t got_new)
{
    if (!pva_web_lending() || !pva_net_capacity()) return false;
    if (got_new < PIECE_MIN) return false;
    return pva_net_buffered() < radio_reserve();
}

static bool radio_cut(void)
{
    if (a_force) return false;
    if (!radio_cut_at(a_skip ? PIECE_MIN : a_new)) return false;
    a_cut = true;
    return true;
}

// the start of each piece of the body: p, n past what was read before. false: stop
static bool art_take(const uint8_t **p, size_t *n)
{
    if (a_wgen != a_gen) return false;              // (an article no longer wanted)
    if (a_first)
    {
        a_first = false;
        if (a_got && pva_web_http() == 200)         // the server sends all of it again
        {
            a_skip = a_got;
            if (!a_force)
            {
                // pieces cannot work here: a part (Enter: the rest, the radio waiting)
                pva_log("news: the server does not do Range");
                a_cut = true;
                a_empty = 0xF0;
                return false;
            }
            pva_log("news: no Range here, %lu bytes skipped", (unsigned long)a_skip);
        }
    }
    if (radio_cut()) return false;
    if (a_skip)
    {
        size_t k = *n < a_skip ? *n : (size_t)a_skip;
        a_skip -= (uint32_t)k;
        *p += k;
        *n -= k;
    }
    a_got += (uint32_t)*n;
    a_new += (uint32_t)*n;
    return true;
}

static bool art_feed_body(const uint8_t *p, size_t n)
{
    if (!art_take(&p, &n)) return true;
    for (size_t i = 0; i < n && !a_stop; i++) xml_byte((char)p[i]);
    return a_stop || H.full;
}

static bool art_page_body(const uint8_t *p, size_t n)
{
    if (!art_take(&p, &n)) return true;
    for (size_t i = 0; i < n && !H.done && !H.full; i++) h_byte(p[i]);
    return H.done || H.full || a_got >= PAGE_MAX;
}

static void art_end(bool ok)
{
    if (!a_part) while (a_len && (art[a_len - 1] == '\n' || art[a_len - 1] == ' ')) a_len--;
    art[a_len] = 0;
    a_st = ok ? PVA_ART_DONE : PVA_ART_FAILED;
    a_wait = false;
    if (!ok) { a_len = 0; a_part = false; }
    pva_log("news: article %s%s, %u bytes of text from %u in %u piece(s)", ok ? "in" : "failed", a_part ? " (a part)" : "",
            (unsigned)a_len, (unsigned)a_got, (unsigned)a_pieces);
}

// a piece ended: true when the next one follows (once the radio has its buffer back)
static bool piece_end(bool ok)
{
    if (!a_cut) return false;
    a_cut = false;
    pva_log("news: piece %u, %lu bytes, the radio %u%% full", (unsigned)a_pieces, (unsigned long)a_new,
            (unsigned)(pva_net_capacity() ? pva_net_buffered() * 100u / pva_net_capacity() : 0));
    a_empty = a_new < 512u && ok ? (uint8_t)(a_empty + 1u) : 0;
    if (!ok || a_empty >= 3u)
    {
        a_part = true;                  // no headway: a part (Enter: the rest, the radio waiting)
        a_esc = false;
        art_end(a_len > 0 || a_got > 0);
        return true;
    }
    a_wait = true;
    a_twait = pva_hal_millis();
    return true;
}

static void art_feed_done(bool ok)
{
    if (a_wgen != a_gen) return;
    a_inweb = false;
    if (piece_end(ok || a_new)) return;
    h_para();
    size_t want = strlen(pool + items[a_story].text) + 200u;
    if (a_len >= want || (a_len >= 300 && H.full)) { art_end(true); return; }
    if (a_url[0]) { a_phase = 2; a_got = 0; a_empty = 0; a_pending = true; return; }   // the page then
    art_end(false);
}

static void art_page_done(bool ok)
{
    if (a_wgen != a_gen) return;
    a_inweb = false;
    if (piece_end(ok || a_new)) return;
    h_para();
    art_end(a_len >= 200);
}

static void art_cancel(void)            // ESC: what came in stays, as a part
{
    if (a_st != PVA_ART_LOADING) return;
    a_gen++;
    a_pending = false;
    a_inweb = false;
    a_cut = false;
    a_part = true;
    a_esc = true;
    pva_log("news: article stopped");
    art_end(a_len > 0 || a_got > 0);
}

static int news_full(uint8_t story)
{
    if (story == 0xFF) { art_cancel(); return PVA_OK; }
    if (st != PVA_NEWS_DONE || story >= nitems) return PVA_E_RANGE;
    if (!art) return PVA_E_FULL;
    bool mine = a_story == story && a_feed == feed_no;
    if (mine && a_st == PVA_ART_DONE && a_part && a_got)
    {
        // a part: the rest now, from where it ended; after no headway the radio waits
        // for it (after ESC: in pieces again)
        if (!pva_hal_net_present()) return PVA_E_NOWIFI;
        a_force = !a_esc;
        a_esc = false;
        a_part = false;
        a_empty = 0;
        a_gen++;
        a_st = PVA_ART_LOADING;
        a_pending = true;
        return PVA_OK;
    }
    if (mine && a_st != PVA_ART_IDLE && a_st != PVA_ART_FAILED && !(a_st == PVA_ART_DONE && a_part)) return PVA_OK;   // (coming in, or in)
    if (!pva_hal_net_present()) return PVA_E_NOWIFI;
    a_gen++;
    a_force = false;
    a_esc = false;
    a_part = false;
    a_wait = false;
    a_cut = false;
    a_empty = 0;
    a_pieces = 0;
    a_story = story;
    a_pos = items[story].pos;
    a_len = 0;
    a_got = 0;
    art[0] = 0;
    snprintf(a_url, sizeof a_url, "%s", pool + items[story].link);
    a_phase = items[story].content ? 1 : 2;
    if (a_phase == 2 && !a_url[0]) { a_st = PVA_ART_FAILED; return PVA_OK; }
    a_feed = feed_no;
    a_st = PVA_ART_LOADING;
    a_pending = true;
    return PVA_OK;
}

// the radio plays without a lent connection: the article's reading waits while the
// radio's buffer is below half (until 3/4, at most 8 s at a time)
static void art_pace(void)
{
    bool want = false;
    if (a_inweb && !a_force && !pva_web_lending() && pva_net_active() && pva_net_capacity())
    {
        size_t b = pva_net_buffered(), cap = pva_net_capacity();
        uint32_t now = pva_hal_millis();
        if (a_hold) want = b < cap * 3u / 4u && now - a_thold < 8000u;
        else if (b < cap / 2u && now - a_thold > 2000u) { want = true; a_thold = now; }
    }
    if (want != a_hold) { a_hold = want; if (!want) a_thold = pva_hal_millis(); }
    pva_web_hold(a_hold);
}

// ---------------------------------------------------------------- lines
// paragraphs ('\n') wrapped at 'width'; an empty line between paragraphs
// (not in a title). Lines from 'first' on, at most 'count', go to the reply.
static uint16_t wrap(const char *s, size_t n, uint8_t width, bool gaps, uint16_t first, uint8_t count)
{
    uint16_t line = 0;
    size_t i = 0;
    char buf[100];
    if (width < 10) width = 10;
    if (width > 96) width = 96;
    bool any = false;
    while (i < n)
    {
        size_t pe = i;
        while (pe < n && s[pe] != '\n') pe++;
        if (pe > i)
        {
            if (any && gaps) { if (line >= first && count) { pva_reply_str(""); count--; } line++; }
            any = true;
            while (i < pe)
            {
                while (i < pe && s[i] == ' ') i++;
                if (i >= pe) break;
                size_t e = i + width;
                if (e >= pe) e = pe;
                else
                {
                    size_t b = e;
                    while (b > i && s[b] != ' ') b--;
                    if (b > i) e = b;         // at a space (else a long word is cut)
                }
                if (line >= first && count)
                {
                    size_t k = e - i;
                    memcpy(buf, s + i, k);
                    while (k && buf[k - 1] == ' ') k--;
                    buf[k] = 0;
                    pva_reply_str(buf);
                    count--;
                }
                line++;
                i = e;
            }
        }
        i = pe + 1;
    }
    return line;
}

// TEXT story, which (0 summary, 1 the full article or else the summary, 2 the title), first (u16), count, width
static int news_text(const uint8_t *a)
{
    uint8_t story = a[0], which = a[1], count = a[4], width = a[5];
    uint16_t first = (uint16_t)(a[2] | a[3] << 8);
    if (st != PVA_NEWS_DONE || story >= nitems) return PVA_E_RANGE;
    const char *s;
    size_t n;
    uint8_t flags = 0;
    bool mine = a_story == story && a_feed == feed_no;
    if (which == 2) { s = pool + items[story].title; n = strlen(s); }
    else if (which == 1 && mine && (a_st == PVA_ART_DONE || (a_st == PVA_ART_LOADING && a_len)))
    {
        s = art;
        n = a_len;
        flags |= 2;
    }
    else { s = pool + items[story].text; n = strlen(s); }
    if (which == 1 && mine && a_st == PVA_ART_LOADING) flags |= 1;
    if (which == 1 && mine && a_st == PVA_ART_FAILED) flags |= 4;
    if (which == 1 && mine && a_st == PVA_ART_DONE && a_part) flags |= PVA_TXF_PART;
    pva_reply_byte(0);                   // (the line count: below)
    pva_reply_byte(0);
    pva_reply_byte(flags);
    uint16_t total = wrap(s, n, width, which != 2, first, count);
    pva_reply_patch16(0, total);
    return PVA_OK;
}

// LIST first, count, width: per story its time ("20:15" or "29 Oct") and title (cut to width)
static int news_list(uint8_t first, uint8_t count, uint8_t width)
{
    if (st != PVA_NEWS_DONE) return PVA_E_STATE;
    if (!width || width > TITLE_MAX) width = TITLE_MAX;
    char d[24], t[TITLE_MAX + 4];
    for (uint8_t i = first; i < nitems && count; i++, count--)
    {
        fmt_date(&items[i], d, sizeof d, false);
        pva_reply_str(d);
        const char *s = pool + items[i].title;
        size_t l = strlen(s);
        if (l > width) snprintf(t, sizeof t, "%.*s...", (int)(width - 3), s);
        else snprintf(t, sizeof t, "%s", s);
        pva_reply_str(t);
    }
    return PVA_OK;
}

// ITEM story: its date (long) and the site ("www.bbc.co.uk")
static int news_item(uint8_t i)
{
    if (st != PVA_NEWS_DONE || i >= nitems) return PVA_E_RANGE;
    char d[48], h[48];
    fmt_date(&items[i], d, sizeof d, true);
    pva_reply_str(d);
    const char *l = pool + items[i].link, *p = strstr(l, "://");
    p = p ? p + 3 : l;
    size_t k = 0;
    while (p[k] && p[k] != '/' && p[k] != ':' && p[k] != '?' && k < sizeof h - 1) k++;
    memcpy(h, p, k);
    h[k] = 0;
    pva_reply_str(h);
    pva_reply_byte(items[i].content ? 1 : 0);
    return PVA_OK;
}

// INFO: the news' state and memory, then the engine's last log lines
static int news_info(void)
{
    char t[120];
    static const char *const sts[] = { "idle", "loading", "in", "failed", "no memory" };
    snprintf(t, sizeof t, "Feed %u: %s, %u stories, error %u (web error %u, HTTP %u)", (unsigned)(feed_no + 1u),
             st < 5 ? sts[st] : "?", (unsigned)nitems, (unsigned)err, (unsigned)pva_web_error(), (unsigned)pva_web_http());
    pva_reply_str(t);
    snprintf(t, sizeof t, "Memory: stories %u (%u used), article %u, engine free %u",
             (unsigned)pool_size, (unsigned)pool_used, (unsigned)art_size, (unsigned)pva_arena_free());
    pva_reply_str(t);
    snprintf(t, sizeof t, "Article: state %u, story %u, %u bytes of text, %u KB read",
             (unsigned)a_st, (unsigned)(a_story == 0xFF ? 0u : a_story + 1u), (unsigned)a_len, (unsigned)(a_got / 1024u));
    pva_reply_str(t);
    pva_reply_str("The engine's last log lines:");
    static char lt[620];                // (the MSX reads 1 KB)
    size_t n = pva_log_tail(lt, sizeof lt);
    for (size_t i = 0; i < n; i += strlen(lt + i) + 1u) pva_reply_str(lt + i);
    return PVA_OK;
}

// ---------------------------------------------------------------- commands
static int news_fetch(uint8_t f)
{
    char name[40], url[LINK_MAX + 1];
    if (!fav(f, name, sizeof name, url, sizeof url)) return PVA_E_RANGE;
    if (!pva_hal_net_present()) return PVA_E_NOWIFI;
    if (!news_mem()) { st = PVA_NEWS_NOMEM; return PVA_E_FULL; }
    want_feed = f;
    pending = true;
    f_gen++;
    f_wait = false;
    f_cut = false;
    f_norange = false;
    f_got = 0;
    f_empty = 0;
    a_gen++;                            // (an article coming in: it stops)
    a_wait = false;
    a_inweb = false;
    st = PVA_NEWS_LOADING;
    feed_no = f;
    err = 0;
    a_pending = false;                  // (an article of the other stories: gone)
    a_st = PVA_ART_IDLE;
    a_story = 0xFF;
    return PVA_OK;
}

int pva_news_cmd(uint8_t c, const uint8_t *a)
{
    switch (c)
    {
        case PVA_C_NEWS_FEEDS:  return news_feeds();
        case PVA_C_NEWS_FETCH:  return news_fetch(a[0]);
        case PVA_C_NEWS_LIST:   return news_list(a[0], a[1], a[2]);
        case PVA_C_NEWS_ITEM:   return news_item(a[0]);
        case PVA_C_NEWS_TOPICS: return news_topics();
        case PVA_C_NEWS_CAT:    return news_cat(a[0]);
        case PVA_C_NEWS_ADD:    return news_add(a[0], a[1]);
        case PVA_C_NEWS_DEL:    return favs_edit(a[0], 0);
        case PVA_C_NEWS_MOVE:   return favs_edit(a[0], a[1] ? 1 : -1);
        case PVA_C_NEWS_FULL:   return news_full(a[0]);
        case PVA_C_NEWS_TEXT:   return news_text(a);
        case PVA_C_NEWS_INFO:   return news_info();
        case PVA_C_NEWS_SORT:   return news_sort(a[0]);
    }
    return PVA_E_UNKNOWN;
}

void pva_news_poll(void)
{
    art_pace();
    if (pva_web_busy()) return;
    if (f_wait && st == PVA_NEWS_LOADING && !pending)
    {
        uint32_t now = pva_hal_millis();
        size_t cap = pva_net_capacity();
        bool ready = !pva_net_active() || !cap || pva_net_buffered() >= cap * 9u / 10u;
        if (now - f_twait < 500u || (!ready && now - f_twait < RADIO_WAIT_MS)) return;
        f_wait = false;
        char name[40], url[LINK_MAX + 1];
        if (!fav(want_feed, name, sizeof name, url, sizeof url)) { fetch_done(false); return; }
        f_wgen = f_gen;
        f_first = true;
        f_new = 0;
        if (pva_web_get_from(url, f_got, body, fetch_done) != PVA_OK) fetch_done(false);
        return;
    }
    if (a_wait && a_st == PVA_ART_LOADING)
    {
        // between pieces: the radio's buffer full again (or no radio)
        uint32_t now = pva_hal_millis();
        size_t cap = pva_net_capacity();
        bool ready = !pva_net_active() || !cap || pva_net_buffered() >= cap * 9u / 10u;
        if (now - a_twait < 500u || (!ready && now - a_twait < RADIO_WAIT_MS)) return;
        a_wait = false;
        a_pending = true;
    }
    if (pending)
    {
        pending = false;
        char name[40], url[LINK_MAX + 1];
        if (!fav(want_feed, name, sizeof name, url, sizeof url)) { st = PVA_NEWS_FAILED; return; }
        nitems = 0;
        pool_used = 0;
        full = false;
        f_got = 0;
        f_new = 0;
        f_first = true;
        f_wgen = f_gen;
        xml_reset(M_LIST);
        if (pva_web_get(url, body, fetch_done) != PVA_OK) { st = PVA_NEWS_FAILED; err = PVA_NERR_NOWIFI; }
        return;
    }
    if (a_pending && a_st == PVA_ART_LOADING)
    {
        a_pending = false;
        a_stop = false;
        bool resume = a_got > 0;        // the next piece: the parser goes on where it was
        a_wgen = a_gen;
        a_first = true;
        a_skip = 0;
        a_new = 0;
        a_cut = false;
        a_pieces++;
        int r;
        if (a_phase == 1)
        {
            char name[40], url[LINK_MAX + 1];
            if (!fav(a_feed, name, sizeof name, url, sizeof url)) { art_end(false); return; }
            if (!resume) { xml_reset(M_ART); h_start(art, &a_len, art_size, false); }
            r = pva_web_get_from(url, a_got, art_feed_body, art_feed_done);
        }
        else
        {
            if (!resume) h_start(art, &a_len, art_size, true);
            r = pva_web_get_from(a_url, a_got, art_page_body, art_page_done);
        }
        if (r != PVA_OK) { a_part = a_len > 0; art_end(a_len > 0); return; }
        a_inweb = true;
    }
}

void pva_news_state(uint8_t *o)
{
    o[0] = st;
    o[1] = st == PVA_NEWS_DONE ? nitems : 0;
    o[2] = feed_no;
    o[3] = err;
    o[4] = a_st;
    o[5] = a_story;
    uint16_t l = (uint16_t)(a_len > 0xFFFF ? 0xFFFF : a_len), k = (uint16_t)(a_got / 1024u > 0xFFFF ? 0xFFFF : a_got / 1024u);
    o[6] = (uint8_t)l;
    o[7] = (uint8_t)(l >> 8);
    o[8] = (uint8_t)k;
    o[9] = (uint8_t)(k >> 8);
    uint32_t len = pva_web_length();    // (the page or feed being read for the article)
    o[10] = a_st == PVA_ART_LOADING && len && a_got <= len ? (uint8_t)((uint64_t)a_got * 100u / len) : 0xFF;
    o[11] = a_st == PVA_ART_LOADING && a_wait ? 1u : a_st == PVA_ART_LOADING && a_hold ? 2u : 0u;
}
