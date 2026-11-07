// MSX PICOVERSE PROJECT
// pva_engine.c - PicoVerse Audio Engine: commands, folders, play list (core 1).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "pva_internal.h"

#define DIR_MAX         1024u
#define DIR_POOL        (16u * 1024u)
#define PL_MAX          512u
#define PL_POOL         (16u * 1024u)
#define PATH_MAX_LEN    240u
#define STATUS_MS       40u
#define HEARTBEAT_MS    5u

typedef struct { uint32_t size; uint16_t name; uint8_t type; uint8_t pad; } dent_t;

static FATFS    fs;
static bool     mounted;
static uint8_t  mount_fr = 0xFF;
static uint8_t  last_fr;

// open folder
static dent_t  *dir_ent;
static char    *dir_pool;
static uint32_t dir_pool_used;
static uint16_t dir_count;
static char     cur_dir[PATH_MAX_LEN + 1];   // "" = root, else "/A/B"
static bool     dir_open;
static uint8_t  dir_gen;

// play list (full paths, "/A/B/x.mp3")
static uint16_t *pl_off;
static uint16_t *pl_order;
static char     *pl_pool;
static uint32_t  pl_pool_used;
static uint16_t  pl_count, pl_pos;
static uint8_t   pl_kind;
static char      pl_m3u_dir[PATH_MAX_LEN + 1];

// playback
static uint8_t  state = PVA_STATE_STOPPED;
static uint8_t  mode;
static uint8_t  volume = 80;
static bool     muted;
static uint8_t  balance = 50;
static uint8_t  play_error;
static uint16_t track_id;
static pva_track_info_t info;
static uint32_t rng = 12345;

static uint8_t *args;               // PVA_ARGS_SIZE, in the arena
static uint8_t *reply;
static uint32_t reply_len;
static uint32_t handled_post;
static uint32_t last_status_ms, last_hb_ms, last_poll_ms, max_gap_ms;
static uint8_t  hb;
static uint8_t  seq;
static bool     status_dirty = true;
static const uint8_t *stack_base;
static size_t   stack_size;
static bool     ready;              // all buffers allocated; else only PING/LOG/DIAG

// streams: play list entries "http(s)://...", optionally followed by
// STATION_SEP and a station name (from #EXTINF or PLAY_URL)
#define STATION_SEP     '\x1f'
#define URL_MAX_LEN     255u
static bool     buffering;          // a stream fills its buffer, no decoding
static uint32_t prefill;            // bytes to wait for before decoding
static uint16_t stream_underruns;
static uint8_t  shown_title_gen;
static char     given_station[PVA_ST_ALBUM_LEN];

// internet radio: virtual folders over the Radio Browser directory
#define RADIO_ENTRY     "Internet radio"
#define RADIO_PATH      "/" RADIO_ENTRY
enum { V_CARD, V_HOME, V_COUNTRIES, V_GENRES, V_DECADES, V_STATIONS, V_FAVS };
#define FAV_FILE        "/RADIOFAV.M3U"     // favourites (PicoPlay writes it through SymbOS)
#define FAV_FOLDER      "Favourites"
#define PAGE_STATIONS   60u
#define MORE_ENTRY      "More stations..."
static char     st_query[200];      // the station list's query, for "more"
static uint16_t st_offset;
static bool     st_more;            // the list ends with MORE_ENTRY
static uint8_t  vlevel;             // V_CARD: cur_dir is a folder on the card
static uint8_t  vparent;            // where ".." of a station list goes
static bool     radio_entry;        // the card's root listing starts with RADIO_ENTRY
static bool     dir_wait;           // a list is being fetched
static uint8_t  want_level, want_parent;
static char     want_title[48];
static uint8_t  dstate, derr;       // PVA_ST_DIR_STATE / DIR_ERR
static uint16_t ccache_n;           // countries kept in pva_dir_cache()

// ---------------------------------------------------------------- helpers
static void fs_refresh(void)
{
    // Nextor writes the card behind FatFS' back (raw sectors through the
    // Sunrise IDE emulation): drop FatFS' cached sector before every command.
    if (mounted && !fs.wflag)
        fs.winsect = (LBA_t)0 - 1;
}

static bool do_mount(void)
{
    f_mount(NULL, "0:", 0);
    FRESULT fr = f_mount(&fs, "0:", 1);
    mount_fr = (uint8_t)fr;
    mounted = (fr == FR_OK);
    if (mounted)
        pva_log("mounted FAT%s, %lu KB clusters", fs.fs_type == FS_FAT12 ? "12" : fs.fs_type == FS_FAT16 ? "16" : fs.fs_type == FS_FAT32 ? "32" : "ex",
                (unsigned long)(fs.csize / 2u));
    else
    {
        last_fr = (uint8_t)fr;
        pva_log("mount failed, FRESULT %d", (int)fr);
    }
    return mounted;
}

static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int name_cmp(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        char ca = upc(*a), cb = upc(*b);
        if (ca != cb || !ca) return (int)(uint8_t)ca - (int)(uint8_t)cb;
    }
}

static int dent_cmp(const void *pa, const void *pb)
{
    const dent_t *a = pa, *b = pb;
    if (a->type == PVA_DE_PARENT) return -1;
    if (b->type == PVA_DE_PARENT) return 1;
    bool da = a->type == PVA_DE_DIR, db = b->type == PVA_DE_DIR;
    if (da != db) return da ? -1 : 1;
    return name_cmp(dir_pool + a->name, dir_pool + b->name);
}

static void fatfs_path(char *out, size_t size, const char *path)
{
    snprintf(out, size, "0:%s", path[0] ? path : "/");
}

static uint32_t reply_cap = PVA_REPLY_SIZE;
static void set_reply_byte(uint8_t b)
{
    if (reply_len < reply_cap) reply[reply_len++] = b;
}

static void set_reply_str(const char *s)
{
    while (*s && reply_len < PVA_REPLY_SIZE - 1) reply[reply_len++] = (uint8_t)*s++;
    set_reply_byte(0);
}

static void set_reply_text(const char *s, size_t n)
{
    if (n > reply_cap) n = reply_cap;
    memcpy(reply, s, n);
    reply_len = (uint32_t)n;
}

void pva_reply_byte(uint8_t b) { set_reply_byte(b); }
void pva_reply_str(const char *s) { set_reply_str(s); }
void pva_reply_patch16(uint32_t at, uint16_t v) { if (at + 1 < reply_len) { reply[at] = (uint8_t)v; reply[at + 1] = (uint8_t)(v >> 8); } }

// ---------------------------------------------------------------- folders
static int read_dir(const char *path)
{
    char fp[PATH_MAX_LEN + 8];
    DIR d;
    FILINFO fno;
    fatfs_path(fp, sizeof fp, path);
    fs_refresh();
    FRESULT fr = f_opendir(&d, fp);
    if (fr != FR_OK)
    {
        last_fr = (uint8_t)fr;
        pva_log("opendir %d: %.30s", (int)fr, fp);
        return (fr == FR_NO_PATH || fr == FR_NO_FILE || fr == FR_INVALID_NAME) ? PVA_E_NOTFOUND : PVA_E_IO;
    }
    dir_count = 0;
    dir_pool_used = 0;
    if (path[0])
    {
        dir_ent[0].type = PVA_DE_PARENT;
        dir_ent[0].size = 0;
        dir_ent[0].name = 0;
        memcpy(dir_pool, "..", 3);
        dir_pool_used = 3;
        dir_count = 1;
    }
    bool full = false;
    for (;;)
    {
        fr = f_readdir(&d, &fno);
        if (fr != FR_OK) { last_fr = (uint8_t)fr; pva_log("readdir %d", (int)fr); break; }
        if (!fno.fname[0]) break;
        if (fno.fattrib & (AM_HID | AM_SYS)) continue;
        if (fno.fname[0] == '.') continue;          // ".", "..", "._" files
        uint8_t type;
        if (fno.fattrib & AM_DIR) type = PVA_DE_DIR;
        else
        {
            int k = pva_kind_from_name(fno.fname);
            if (!k) continue;
            type = (uint8_t)k;
        }
        size_t l = strlen(fno.fname) + 1;
        if (dir_count >= DIR_MAX || dir_pool_used + l > DIR_POOL) { full = true; break; }
        dent_t *e = &dir_ent[dir_count++];
        e->type = type;
        e->size = (uint32_t)fno.fsize;
        e->name = (uint16_t)dir_pool_used;
        memcpy(dir_pool + dir_pool_used, fno.fname, l);
        dir_pool_used += (uint32_t)l;
    }
    f_closedir(&d);
    if (full) pva_log("folder cut at %u entries", (unsigned)dir_count);
    qsort(dir_ent, dir_count, sizeof(dent_t), dent_cmp);
    radio_entry = false;
    if (!path[0] && pva_dir_available() && pva_hal_net_present() && dir_pool_used + sizeof RADIO_ENTRY <= DIR_POOL)
    {
        // the internet radio directory comes first in the root
        if (dir_count >= DIR_MAX) dir_count--;
        memmove(dir_ent + 1, dir_ent, dir_count * sizeof(dent_t));
        dir_ent[0].type = PVA_DE_DIR;
        dir_ent[0].size = 0;
        dir_ent[0].name = (uint16_t)dir_pool_used;
        memcpy(dir_pool + dir_pool_used, RADIO_ENTRY, sizeof RADIO_ENTRY);
        dir_pool_used += sizeof RADIO_ENTRY;
        dir_count++;
        radio_entry = true;
    }
    if (path != cur_dir)
        snprintf(cur_dir, sizeof cur_dir, "%s", path);
    vlevel = V_CARD;
    dir_open = true;
    dir_gen++;
    return PVA_OK;
}

// ---------------------------------------------------------------- internet radio
static const char *const home_names[] = { FAV_FOLDER, "Top stations", "Most voted", "Trending now", "By country", "By genre", "By decade" };
static const char *const home_order[] = { "clickcount", "votes", "clicktrend" };
static const char *const genres[] = {
    "pop", "rock", "classic rock", "hard rock", "metal", "punk", "alternative", "indie", "jazz", "smooth jazz",
    "blues", "soul", "funk", "disco", "rnb", "hip hop", "rap", "reggae", "latin", "salsa", "country", "folk",
    "classical", "opera", "ambient", "chillout", "lounge", "dance", "electronic", "house", "techno", "trance",
    "drum and bass", "dubstep", "eurodance", "schlager", "chanson", "world", "christmas", "kids", "news", "talk",
    "sports", "comedy", "soundtrack", "instrumental", "piano", "new age", "gospel", "christian",
};
static const char *const decades[][2] = {
    { "1950s", "50s" }, { "1960s", "60s" }, { "1970s", "70s" }, { "1980s", "80s" }, { "1990s", "90s" },
    { "2000s", "00s" }, { "2010s", "10s" }, { "Oldies", "oldies" },
};
#define N_HOME      (sizeof home_names / sizeof home_names[0])
#define N_GENRES    (sizeof genres / sizeof genres[0])
#define N_DECADES   (sizeof decades / sizeof decades[0])

static void v_begin(uint8_t level, const char *title)
{
    vlevel = level;
    dir_count = 1;
    dir_ent[0].type = PVA_DE_PARENT;
    dir_ent[0].size = 0;
    dir_ent[0].name = 0;
    memcpy(dir_pool, "..", 3);
    dir_pool_used = 3;
    if (level == V_HOME) snprintf(cur_dir, sizeof cur_dir, "%s", RADIO_PATH);
    else snprintf(cur_dir, sizeof cur_dir, "%s/%s", RADIO_PATH, title);
    radio_entry = false;
    dir_open = true;
}

static void v_add(const char *name, uint8_t type, uint32_t size)
{
    char nm[PVA_NAME_MAX + 1];
    pva_copy_ascii(nm, sizeof nm, name, strlen(name));
    size_t l = strlen(nm) + 1;
    if (l < 2 || dir_count >= DIR_MAX || dir_pool_used + l > DIR_POOL) return;
    dent_t *e = &dir_ent[dir_count++];
    e->type = type;
    e->size = size;
    e->name = (uint16_t)dir_pool_used;
    memcpy(dir_pool + dir_pool_used, nm, l);
    dir_pool_used += (uint32_t)l;
}

// a station with its address after the name in the pool (favourites)
static void v_add_url(const char *name, const char *url)
{
    uint16_t before = dir_count;
    size_t ul = strlen(url) + 1;
    if (dir_pool_used + PVA_NAME_MAX + 1 + ul > DIR_POOL) return;
    v_add(name[0] ? name : url, PVA_DE_URL, 0);
    if (dir_count == before) return;
    memcpy(dir_pool + dir_pool_used, url, ul);
    dir_pool_used += (uint32_t)ul;
}

static void fav_build(void);
static bool is_url(const char *p);
static void alarm_apply(void);

static void v_end(void)
{
    dir_gen++;
    status_dirty = true;
}

// countries in the cache: [u16 stations][UTF-8 name][0] ...
static const uint8_t *ccache_entry(uint16_t i)
{
    size_t sz;
    const uint8_t *c = pva_dir_cache(&sz);
    if (!c || i >= ccache_n) return NULL;
    for (uint16_t k = 0; k < i; k++) c += 2 + strlen((const char *)c + 2) + 1;
    return c;
}

static void v_build(uint8_t level)
{
    switch (level)
    {
        case V_HOME:
            v_begin(V_HOME, "");
            for (unsigned i = 0; i < N_HOME; i++) v_add(home_names[i], PVA_DE_DIR, 0);
            break;
        case V_GENRES:
            v_begin(V_GENRES, "By genre");
            for (unsigned i = 0; i < N_GENRES; i++)
            {
                char t[24];
                snprintf(t, sizeof t, "%s", genres[i]);
                t[0] = upc(t[0]);
                v_add(t, PVA_DE_DIR, 0);
            }
            break;
        case V_DECADES:
            v_begin(V_DECADES, "By decade");
            for (unsigned i = 0; i < N_DECADES; i++) v_add(decades[i][0], PVA_DE_DIR, 0);
            break;
        case V_COUNTRIES:
        {
            v_begin(V_COUNTRIES, "By country");
            size_t sz;
            const uint8_t *c = pva_dir_cache(&sz);
            for (uint16_t i = 0; c && i < ccache_n; i++)
            {
                uint16_t n = (uint16_t)(c[0] | (c[1] << 8));
                v_add((const char *)c + 2, PVA_DE_DIR, n);
                c += 2 + strlen((const char *)c + 2) + 1;
            }
            break;
        }
        case V_STATIONS:
            v_begin(V_STATIONS, want_title);
            for (uint16_t i = 0; i < pva_dir_count(); i++) v_add(pva_dir_name(i), PVA_DE_URL, pva_dir_num(i));
            if (st_more) v_add(MORE_ENTRY, PVA_DE_DIR, 0);
            break;
        case V_FAVS:
            v_begin(V_FAVS, FAV_FOLDER);
            fav_build();
            break;
    }
    v_end();
}

// a list has come in
static void radio_list_ready(void)
{
    if (want_level == V_COUNTRIES)
    {
        size_t sz, o = 0;
        uint8_t *c = pva_dir_cache(&sz);
        ccache_n = 0;
        for (uint16_t i = 0; c && i < pva_dir_count(); i++)
        {
            const char *nm = pva_dir_name(i);
            size_t l = strlen(nm) + 1;
            if (o + 2 + l > sz) break;
            uint16_t n = pva_dir_num(i);
            c[o] = (uint8_t)n; c[o + 1] = (uint8_t)(n >> 8);
            memcpy(c + o + 2, nm, l);
            o += 2 + l;
            ccache_n++;
        }
    }
    else
    {
        vparent = want_parent;
        // a full page (before the filters) may have more after it
        st_more = pva_dir_raw() >= PAGE_STATIONS && !pva_dir_truncated();
    }
    v_build(want_level);
    pva_log("radio: %u in %.40s", (unsigned)(dir_count - 1u), want_level == V_COUNTRIES ? "By country" : want_title);
}

static int radio_fetch(uint8_t level, uint8_t parent, const char *title, const char *query)
{
    char path[300];
    uint8_t k = level == V_COUNTRIES ? PVA_DIRQ_COUNTRIES : PVA_DIRQ_STATIONS;
    if (k == PVA_DIRQ_COUNTRIES) snprintf(path, sizeof path, "/json/countries?hidebroken=true&order=name");
    else snprintf(path, sizeof path, "/json/stations/search?%s&codec=MP3&hidebroken=true&limit=60", query);
    int r = pva_dir_start(k, path);
    if (k == PVA_DIRQ_STATIONS) { snprintf(st_query, sizeof st_query, "%s", query); st_offset = 0; }
    if (r != PVA_OK) { dstate = PVA_DIRS_FAILED; derr = PVA_NERR_NOWIFI; status_dirty = true; return r; }
    want_level = level;
    want_parent = parent;
    snprintf(want_title, sizeof want_title, "%s", title);
    dir_wait = true;
    dstate = PVA_DIRS_LOADING;
    derr = 0;
    status_dirty = true;
    return PVA_E_BUSY;
}

// "More stations...": the next page, added to the list
static int radio_more(void)
{
    char path[300];
    uint16_t off = (uint16_t)(st_offset + PAGE_STATIONS);
    snprintf(path, sizeof path, "/json/stations/search?%s&codec=MP3&hidebroken=true&limit=%u&offset=%u",
             st_query, (unsigned)PAGE_STATIONS, (unsigned)off);
    int r = pva_dir_more(path);
    if (r != PVA_OK) return r;
    st_offset = off;
    want_level = V_STATIONS;
    want_parent = vparent;
    dir_wait = true;
    dstate = PVA_DIRS_LOADING;
    derr = 0;
    status_dirty = true;
    return PVA_E_BUSY;
}

static void radio_poll(void)
{
    pva_dir_poll();
    if (!dir_wait) return;
    uint8_t st = pva_dir_state();
    if (st == PVA_DIRS_DONE)
    {
        dir_wait = false;
        dstate = PVA_DIRS_DONE;
        radio_list_ready();
    }
    else if (st == PVA_DIRS_FAILED || st == PVA_DIRS_IDLE)
    {
        dir_wait = false;
        dstate = PVA_DIRS_FAILED;
        derr = pva_dir_error() ? pva_dir_error() : PVA_NERR_TIMEOUT;
        status_dirty = true;
    }
}

static void radio_stop_wait(void)
{
    if (!dir_wait) return;
    pva_dir_cancel();
    dir_wait = false;
    dstate = PVA_DIRS_IDLE;
    status_dirty = true;
}

static int radio_parent(void)
{
    radio_stop_wait();
    switch (vlevel)
    {
        case V_HOME:      return read_dir("");
        case V_STATIONS:  v_build(vparent == V_COUNTRIES && !ccache_n ? V_HOME : vparent); return PVA_OK;
        default:          v_build(V_HOME); return PVA_OK;
    }
}

static int radio_enter(uint16_t idx)
{
    char q[200], enc[160];
    const char *nm = dir_pool + dir_ent[idx].name;
    switch (vlevel)
    {
        case V_HOME:
        {
            unsigned i = idx - 1u;
            if (i == 0) { radio_stop_wait(); v_build(V_FAVS); return PVA_OK; }
            if (i <= 3)
            {
                snprintf(q, sizeof q, "order=%s&reverse=true", home_order[i - 1u]);
                return radio_fetch(V_STATIONS, V_HOME, home_names[i], q);
            }
            radio_stop_wait();
            if (i == 4)
            {
                if (ccache_n) { v_build(V_COUNTRIES); return PVA_OK; }
                return radio_fetch(V_COUNTRIES, V_HOME, "By country", "");
            }
            v_build(i == 5 ? V_GENRES : V_DECADES);
            return PVA_OK;
        }
        case V_STATIONS:
            if (st_more && idx + 1u == dir_count) return radio_more();
            return PVA_E_ARGS;
        case V_COUNTRIES:
        {
            const uint8_t *c = ccache_entry((uint16_t)(idx - 1u));
            if (!c) return PVA_E_RANGE;
            pva_dir_urlenc(enc, sizeof enc, (const char *)c + 2);
            snprintf(q, sizeof q, "country=%s&countryExact=true&order=clickcount&reverse=true", enc);
            char t[48];
            snprintf(t, sizeof t, "By country/%s", nm);
            return radio_fetch(V_STATIONS, V_COUNTRIES, t, q);
        }
        case V_GENRES:
        case V_DECADES:
        {
            unsigned i = idx - 1u;
            const char *tag = vlevel == V_GENRES ? (i < N_GENRES ? genres[i] : NULL) : (i < N_DECADES ? decades[i][1] : NULL);
            if (!tag) return PVA_E_RANGE;
            pva_dir_urlenc(enc, sizeof enc, tag);
            snprintf(q, sizeof q, "tag=%s&order=clickcount&reverse=true", enc);
            char t[48];
            snprintf(t, sizeof t, "%s/%s", vlevel == V_GENRES ? "By genre" : "By decade", nm);
            return radio_fetch(V_STATIONS, vlevel, t, q);
        }
        default:
            return PVA_E_ARGS;
    }
}

// DIR_SEARCH: stations whose name has the text, as a station list under the
// directory's home (".." goes there)
static int cmd_dir_search(void)
{
    char text[64], enc[200], q[260], t[48];
    size_t n = 0;
    while (n < sizeof text - 1 && n < PVA_ARGS_SIZE && args[n]) { text[n] = (char)args[n]; n++; }
    text[n] = 0;
    char *s = text;
    while (*s == ' ') s++;
    n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    if (!*s) return PVA_E_ARGS;
    if (!pva_dir_available() || !pva_hal_net_present()) return PVA_E_NOWIFI;
    radio_stop_wait();
    pva_dir_urlenc(enc, sizeof enc, s);
    snprintf(q, sizeof q, "name=%s&order=clickcount&reverse=true", enc);
    snprintf(t, sizeof t, "Search/%.38s", s);
    pva_log("radio: search %.40s", s);
    return radio_fetch(V_STATIONS, V_HOME, t, q);
}

static void reply_dir_header(void)
{
    set_reply_byte((uint8_t)(dir_count & 0xFF));
    set_reply_byte((uint8_t)(dir_count >> 8));
    char shown[PATH_MAX_LEN + 1];
    pva_copy_ascii(shown, sizeof shown, cur_dir[0] ? cur_dir : "/", PATH_MAX_LEN);
    set_reply_str(shown);
}

// Path from the MSX side to an absolute "/A/B" path on the card ("" = root).
// Accepts '\\' or '/', an optional drive ("A:"), no leading separator
// (taken from the root), repeated separators, "." and "..".
static bool normalise_path(char *out, size_t size, const char *in)
{
    size_t o = 0;
    if (!size) return false;
    if (((in[0] >= 'A' && in[0] <= 'Z') || (in[0] >= 'a' && in[0] <= 'z')) && in[1] == ':') in += 2;
    out[0] = 0;
    while (*in)
    {
        while (*in == '/' || *in == '\\') in++;
        if (!*in) break;
        const char *e = in;
        while (*e && *e != '/' && *e != '\\') e++;
        size_t n = (size_t)(e - in);
        if (n == 1 && in[0] == '.') { in = e; continue; }
        if (n == 2 && in[0] == '.' && in[1] == '.')
        {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            out[o] = 0;
            in = e;
            continue;
        }
        if (o + 1 + n + 1 > size) return false;
        out[o++] = '/';
        memcpy(out + o, in, n);
        o += n;
        out[o] = 0;
        in = e;
    }
    return true;
}

static int cmd_dir_open(void)
{
    char path[PATH_MAX_LEN + 1];
    args[PVA_ARGS_SIZE - 1] = 0;
    if (!mounted && !do_mount()) return PVA_E_NOCARD;
    if (args[0] == 0 && vlevel != V_CARD)
    {
        if (vlevel == V_FAVS) v_build(V_FAVS);     // PicoPlay may have changed the file
        reply_dir_header();             // the radio list on show
        return PVA_OK;
    }
    if (args[0] == 0)
    {
        int r = read_dir(cur_dir);
        if (r != PVA_OK && cur_dir[0]) { cur_dir[0] = 0; r = read_dir(cur_dir); }
        if (r != PVA_OK) return r;
    }
    else
    {
        if (!normalise_path(path, sizeof path, (const char *)args)) return PVA_E_ARGS;
        radio_stop_wait();
        if (!strcmp(path, RADIO_PATH) && pva_dir_available())
        {
            v_build(V_HOME);
            reply_dir_header();
            return PVA_OK;
        }
        int r = read_dir(path);
        if (r != PVA_OK) return r;
    }
    reply_dir_header();
    return PVA_OK;
}

static int cmd_dir_enter(void)
{
    if (!dir_open) return PVA_E_STATE;
    uint16_t idx = (uint16_t)(args[0] | (args[1] << 8));
    if (idx >= dir_count) return PVA_E_RANGE;
    char path[PATH_MAX_LEN + 1];
    if (vlevel != V_CARD)
    {
        int r = dir_ent[idx].type == PVA_DE_PARENT ? radio_parent() : dir_ent[idx].type == PVA_DE_DIR ? radio_enter(idx) : PVA_E_ARGS;
        if (r != PVA_OK) return r;
        reply_dir_header();
        return PVA_OK;
    }
    if (radio_entry && idx == 0)
    {
        v_build(V_HOME);
        reply_dir_header();
        return PVA_OK;
    }
    if (dir_ent[idx].type == PVA_DE_PARENT)
    {
        snprintf(path, sizeof path, "%s", cur_dir);
        char *s = strrchr(path, '/');
        if (s) *s = 0;
    }
    else if (dir_ent[idx].type == PVA_DE_DIR)
    {
        if (strlen(cur_dir) + 1 + strlen(dir_pool + dir_ent[idx].name) > PATH_MAX_LEN) return PVA_E_ARGS;
        snprintf(path, sizeof path, "%s/%s", cur_dir, dir_pool + dir_ent[idx].name);
    }
    else
        return PVA_E_ARGS;
    int r = read_dir(path);
    if (r != PVA_OK) return r;
    reply_dir_header();
    return PVA_OK;
}

static int cmd_dir_read(void)
{
    if (!dir_open) return PVA_E_STATE;
    uint16_t first = (uint16_t)(args[0] | (args[1] << 8));
    uint8_t max = args[2];
    bool flags = args[3] == PVA_DR_FLAGS;      // (a 4th byte A5h: a station's size has PVA_DR_TLS)
    for (uint16_t i = first; i < dir_count && max; i++, max--)
    {
        const dent_t *e = &dir_ent[i];
        char shown[PVA_NAME_MAX + 1];
        pva_copy_ascii(shown, sizeof shown, dir_pool + e->name, 255);
        uint8_t l = (uint8_t)strlen(shown);
        if (reply_len + 6u + l > PVA_REPLY_SIZE) break;
        uint32_t size = e->size;
        if (flags && e->type == PVA_DE_URL)
        {
            const char *u = vlevel == V_STATIONS && i ? pva_dir_url((uint16_t)(i - 1u)) :
                            vlevel == V_FAVS ? dir_pool + e->name + strlen(dir_pool + e->name) + 1 : "";
            if (u && !strncmp(u, "https:", 6)) size |= PVA_DR_TLS;
        }
        set_reply_byte(e->type);
        set_reply_byte((uint8_t)size);
        set_reply_byte((uint8_t)(size >> 8));
        set_reply_byte((uint8_t)(size >> 16));
        set_reply_byte((uint8_t)(size >> 24));
        set_reply_byte(l);
        memcpy(reply + reply_len, shown, l);
        reply_len += l;
    }
    return PVA_OK;
}

// ---------------------------------------------------------------- play list
static uint32_t rand32(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

static void pl_clear(void)
{
    pl_count = 0;
    pl_pos = 0;
    pl_pool_used = 0;
    pl_kind = 0;
    pl_m3u_dir[0] = 0;
}

static bool pl_add(const char *path)
{
    size_t l = strlen(path) + 1;
    if (pl_count >= PL_MAX || pl_pool_used + l > PL_POOL) return false;
    pl_off[pl_count] = (uint16_t)pl_pool_used;
    pl_order[pl_count] = pl_count;
    memcpy(pl_pool + pl_pool_used, path, l);
    pl_pool_used += (uint32_t)l;
    pl_count++;
    return true;
}

// Shuffle the order, keeping track 'first' at the front.
static void pl_shuffle(uint16_t first)
{
    for (uint16_t i = 0; i < pl_count; i++) pl_order[i] = i;
    if (pl_count < 2) return;
    for (uint16_t i = pl_count - 1; i > 0; i--)
    {
        uint16_t j = (uint16_t)(rand32() % (i + 1u));
        uint16_t t = pl_order[i]; pl_order[i] = pl_order[j]; pl_order[j] = t;
    }
    for (uint16_t i = 0; i < pl_count; i++)
        if (pl_order[i] == first) { pl_order[i] = pl_order[0]; pl_order[0] = first; break; }
}

static void pl_unshuffle(void)
{
    uint16_t cur = pl_count ? pl_order[pl_pos] : 0;
    for (uint16_t i = 0; i < pl_count; i++) pl_order[i] = i;
    pl_pos = cur;
}

// Line reader for M3U files
typedef struct { FIL *f; uint8_t buf[512]; UINT len, pos; bool eof; } line_reader_t;

static bool read_line(line_reader_t *lr, char *out, size_t size)
{
    size_t o = 0;
    bool any = false;
    for (;;)
    {
        if (lr->pos >= lr->len)
        {
            if (lr->eof) break;
            if (f_read(lr->f, lr->buf, sizeof lr->buf, &lr->len) != FR_OK || lr->len == 0) { lr->eof = true; lr->len = 0; break; }
            lr->pos = 0;
        }
        char c = (char)lr->buf[lr->pos++];
        any = true;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (o + 1 < size) out[o++] = c;
    }
    out[o] = 0;
    return any;
}

static FIL *m3u_file;
static line_reader_t *m3u_lr;
static int load_m3u(const char *m3u_path)
{
    char fp[PATH_MAX_LEN + 8];
    fatfs_path(fp, sizeof fp, m3u_path);
    fs_refresh();
    FRESULT fr = f_open(m3u_file, fp, FA_READ);
    if (fr != FR_OK) { last_fr = (uint8_t)fr; return PVA_E_NOTFOUND; }
    pl_clear();
    pl_kind = 1;
    snprintf(pl_m3u_dir, sizeof pl_m3u_dir, "%s", m3u_path);
    char *s = strrchr(pl_m3u_dir, '/');
    if (s) *s = 0;
    memset(m3u_lr, 0, sizeof *m3u_lr);
    m3u_lr->f = m3u_file;
    char line[URL_MAX_LEN + 8], path[PATH_MAX_LEN + 1];
    char extinf[PVA_ST_ALBUM_LEN] = "";
    bool first = true;
    while (read_line(m3u_lr, line, sizeof line))
    {
        char *p = line;
        if (first && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
        first = false;
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l - 1] == ' ' || p[l - 1] == '\t')) p[--l] = 0;
        if (!strncmp(p, "#EXTINF:", 8))
        {
            const char *c = strchr(p, ',');
            if (c) pva_copy_ascii(extinf, sizeof extinf, c + 1, strlen(c + 1));
            continue;
        }
        if (!*p || *p == '#') continue;
        if (strstr(p, "://"))
        {
            if (strncmp(p, "http://", 7) && strncmp(p, "https://", 8)) { extinf[0] = 0; continue; }
            char e[URL_MAX_LEN + PVA_ST_ALBUM_LEN + 2];
            if (strlen(p) > URL_MAX_LEN) { extinf[0] = 0; continue; }
            int n = snprintf(e, sizeof e, "%s", p);
            if (extinf[0]) snprintf(e + n, sizeof e - (size_t)n, "%c%s", STATION_SEP, extinf);
            extinf[0] = 0;
            if (!pl_add(e)) { pva_log("play list cut at %u", (unsigned)pl_count); break; }
            continue;
        }
        extinf[0] = 0;
        for (char *q = p; *q; q++) if (*q == '\\') *q = '/';
        if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') p += 2;
        if (!pva_kind_from_name(p) || pva_kind_from_name(p) == PVA_DE_M3U) continue;
        if (p[0] == '/') snprintf(path, sizeof path, "%s", p);
        else snprintf(path, sizeof path, "%s/%s", pl_m3u_dir, p);
        if (!pl_add(path)) { pva_log("play list cut at %u", (unsigned)pl_count); break; }
    }
    f_close(m3u_file);
    pva_log("M3U: %u tracks", (unsigned)pl_count);
    return pl_count ? PVA_OK : PVA_E_FORMAT;
}

// The favourites file: an M3U (#EXTINF names, http(s) lines) in the root.
static void fav_build(void)
{
    char fp[PATH_MAX_LEN + 8];
    if (!mounted && !do_mount()) return;
    fatfs_path(fp, sizeof fp, FAV_FILE);
    fs_refresh();
    if (f_open(m3u_file, fp, FA_READ) != FR_OK) return;
    memset(m3u_lr, 0, sizeof *m3u_lr);
    m3u_lr->f = m3u_file;
    char line[URL_MAX_LEN + 8], name[PVA_NAME_MAX + 1] = "";
    while (read_line(m3u_lr, line, sizeof line))
    {
        char *p = line;
        if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l - 1] == ' ' || p[l - 1] == '\t')) p[--l] = 0;
        if (!strncmp(p, "#EXTINF:", 8))
        {
            const char *c = strchr(p, ',');
            if (c) pva_copy_ascii(name, sizeof name, c + 1, strlen(c + 1));
            continue;
        }
        if (!*p || *p == '#') continue;
        if (is_url(p) && strlen(p) <= URL_MAX_LEN) v_add_url(name, p);
        name[0] = 0;
    }
    f_close(m3u_file);
}

// a station's address and name: entry 'idx' of the open list (a station
// list or the favourites), or the stream that plays (0xFFFF)
static int station_info(uint16_t idx, char *u, size_t usize, char *nm, size_t nsize)
{
    if (idx == 0xFFFF)
    {
        if (!pl_count || !pva_dec_is_stream()) return PVA_E_STATE;
        const char *e = pl_pool + pl_off[pl_order[pl_pos]];
        const char *sep = strchr(e, STATION_SEP);
        size_t n = sep ? (size_t)(sep - e) : strlen(e);
        if (n >= usize) return PVA_E_ARGS;
        memcpy(u, e, n);
        u[n] = 0;
        snprintf(nm, nsize, "%s", given_station[0] ? given_station : info.album);
    }
    else
    {
        if (!dir_open || idx >= dir_count || dir_ent[idx].type != PVA_DE_URL) return PVA_E_ARGS;
        const char *n0 = dir_pool + dir_ent[idx].name;
        snprintf(nm, nsize, "%s", n0);
        if (vlevel == V_STATIONS) snprintf(u, usize, "%s", pva_dir_url((uint16_t)(idx - 1u)));
        else if (vlevel == V_FAVS) snprintf(u, usize, "%s", n0 + strlen(n0) + 1);
        else return PVA_E_ARGS;
    }
    return u[0] ? PVA_OK : PVA_E_ARGS;
}

// ENTRY_INFO: address and name of a station in the open list (0xFFFF = the
// stream that plays), for PicoPlay's favourites
static int cmd_entry_info(void)
{
    uint16_t idx = (uint16_t)(args[0] | (args[1] << 8));
    char u[URL_MAX_LEN + 1];
    char nm[PVA_NAME_MAX + 1];
    int r = station_info(idx, u, sizeof u, nm, sizeof nm);
    if (r != PVA_OK) return r;
    set_reply_str(u);
    set_reply_str(nm);
    return PVA_OK;
}

// ---------------------------------------------------------------- favourites
// FAV_ADD / FAV_DEL: the engine changes RADIOFAV.M3U itself, but only where
// the firmware said nobody else writes the card (no Nextor: the PicoAmp
// ROM). Under MSX-DOS the program writes the file through Nextor instead.
#define FAV_MAX 4096u               // as PicoPlay: the file stays within 4 KB
static bool card_writes;

void pva_allow_card_writes(bool on) { card_writes = on; }

// The file is changed in place through m3u_file and the line reader's
// 512-byte buffer, so the favourites need no memory of their own.
typedef struct { FSIZE_t s0, e, size; bool found, last_eol; } fav_scan_t;

static FSIZE_t lr_tell(const line_reader_t *lr) { return f_tell(lr->f) - (FSIZE_t)(lr->len - lr->pos); }

// Open RADIOFAV.M3U (mode) and find the line that is exactly 'u': s0 = the
// start of its #EXTINF line (or of the line), e = after its line end. The
// file stays open on FR_OK (also when not found); FR_NO_FILE when it is not
// there.
static FRESULT fav_scan(const char *u, BYTE mode, fav_scan_t *sc)
{
    char fp[PATH_MAX_LEN + 8];
    memset(sc, 0, sizeof *sc);
    sc->last_eol = true;
    fs_refresh();
    fatfs_path(fp, sizeof fp, FAV_FILE);
    FRESULT fr = f_open(m3u_file, fp, mode);
    if (fr != FR_OK) return fr;
    sc->size = f_size(m3u_file);
    memset(m3u_lr, 0, sizeof *m3u_lr);
    m3u_lr->f = m3u_file;
    char line[URL_MAX_LEN + 8];
    FSIZE_t start = 0, ext = (FSIZE_t)-1;
    while (!sc->found && read_line(m3u_lr, line, sizeof line))
    {
        if (!strncmp(line, "#EXTINF", 7)) ext = start;
        else if (!strcmp(line, u)) { sc->found = true; sc->s0 = ext != (FSIZE_t)-1 ? ext : start; sc->e = lr_tell(m3u_lr); }
        else if (line[0]) ext = (FSIZE_t)-1;
        start = lr_tell(m3u_lr);
    }
    if (sc->size)
    {
        BYTE c = 0;
        UINT got = 0;
        if (f_lseek(m3u_file, sc->size - 1u) == FR_OK && f_read(m3u_file, &c, 1, &got) == FR_OK && got == 1)
            sc->last_eol = c == 10 || c == 13;
    }
    return FR_OK;
}

static int fav_fail(FRESULT fr) { last_fr = (uint8_t)fr; f_close(m3u_file); return PVA_E_IO; }

// FAV_ADD: args u16 entry of the open list, 0xFFFF = the stream that plays;
// reply: the station's name
static int cmd_fav_add(void)
{
    if (!card_writes) return PVA_E_RDONLY;
    uint16_t idx = (uint16_t)(args[0] | (args[1] << 8));
    char u[URL_MAX_LEN + 1], nm[PVA_NAME_MAX + 1];
    int r = station_info(idx, u, sizeof u, nm, sizeof nm);
    if (r != PVA_OK) return r;
    if (!mounted && !do_mount()) return PVA_E_NOCARD;
    fav_scan_t sc;
    FRESULT fr = fav_scan(u, FA_READ | FA_WRITE | FA_OPEN_ALWAYS, &sc);
    if (fr != FR_OK) { last_fr = (uint8_t)fr; return PVA_E_IO; }
    if (sc.found) { f_close(m3u_file); return PVA_E_EXISTS; }
    char add[URL_MAX_LEN + PVA_NAME_MAX + 32];
    size_t n = 0;
    if (!sc.size) n = (size_t)snprintf(add, sizeof add, "#EXTM3U\r\n");
    else if (!sc.last_eol) n = (size_t)snprintf(add, sizeof add, "\r\n");
    n += (size_t)snprintf(add + n, sizeof add - n, "#EXTINF:-1,%s\r\n%s\r\n", nm, u);
    if (sc.size + n > FAV_MAX) { f_close(m3u_file); return PVA_E_FULL; }
    UINT put = 0;
    if ((fr = f_lseek(m3u_file, sc.size)) != FR_OK) return fav_fail(fr);
    if ((fr = f_write(m3u_file, add, (UINT)n, &put)) != FR_OK || put != n) return fav_fail(fr ? fr : FR_DENIED);
    if ((fr = f_close(m3u_file)) != FR_OK) { last_fr = (uint8_t)fr; return PVA_E_IO; }
    if (vlevel == V_FAVS) v_build(V_FAVS);
    pva_log("favourites: + %.40s", nm);
    set_reply_str(nm);
    return PVA_OK;
}

// FAV_DEL: args u16 entry of Internet radio/Favourites; the list is read again
static int cmd_fav_del(void)
{
    if (!card_writes) return PVA_E_RDONLY;
    uint16_t idx = (uint16_t)(args[0] | (args[1] << 8));
    if (vlevel != V_FAVS || idx == 0xFFFF) return PVA_E_ARGS;
    char u[URL_MAX_LEN + 1], nm[PVA_NAME_MAX + 1];
    int r = station_info(idx, u, sizeof u, nm, sizeof nm);
    if (r != PVA_OK) return r;
    if (!mounted && !do_mount()) return PVA_E_NOCARD;
    fav_scan_t sc;
    FRESULT fr = fav_scan(u, FA_READ | FA_WRITE, &sc);
    if (fr == FR_NO_FILE) return PVA_E_NOTFOUND;
    if (fr != FR_OK) { last_fr = (uint8_t)fr; return PVA_E_IO; }
    if (!sc.found) { f_close(m3u_file); return PVA_E_NOTFOUND; }
    // move the rest of the file down over the station's lines, then cut it
    FSIZE_t from = sc.e, to = sc.s0;
    while (from < sc.size)
    {
        UINT got = 0, put = 0;
        if ((fr = f_lseek(m3u_file, from)) != FR_OK || (fr = f_read(m3u_file, m3u_lr->buf, sizeof m3u_lr->buf, &got)) != FR_OK) return fav_fail(fr);
        if (!got) break;
        if ((fr = f_lseek(m3u_file, to)) != FR_OK || (fr = f_write(m3u_file, m3u_lr->buf, got, &put)) != FR_OK || put != got) return fav_fail(fr ? fr : FR_DENIED);
        from += got;
        to += got;
    }
    if ((fr = f_lseek(m3u_file, to)) != FR_OK || (fr = f_truncate(m3u_file)) != FR_OK) return fav_fail(fr);
    if ((fr = f_close(m3u_file)) != FR_OK) { last_fr = (uint8_t)fr; return PVA_E_IO; }
    v_build(V_FAVS);
    pva_log("favourites: - %.40s", nm);
    return PVA_OK;
}

// ---------------------------------------------------------------- settings
#define CFG_FILE        "/PICOAMP.INI"

// CFG_LOAD: read PICOAMP.INI (none: empty settings); the clock starts
static int cmd_cfg_load(void)
{
    char fp[PATH_MAX_LEN + 8];
    static char t[PVA_CFG_MAX + 1];
    UINT got = 0;
    if (mounted || do_mount())
    {
        fs_refresh();
        fatfs_path(fp, sizeof fp, CFG_FILE);
        FRESULT fr = f_open(m3u_file, fp, FA_READ);
        if (fr == FR_OK)
        {
            fr = f_read(m3u_file, t, PVA_CFG_MAX, &got);
            f_close(m3u_file);
            if (fr != FR_OK) got = 0;
        }
    }
    pva_cfg_load(t, got);
    pva_clock_enable();
    alarm_apply();
    pva_log("settings: %u bytes", (unsigned)got);
    return PVA_OK;
}

static int cmd_cfg_get(void)
{
    char k[64], v[PVA_NAME_MAX * 2];
    args[PVA_ARGS_SIZE - 1] = 0;
    snprintf(k, sizeof k, "%s", (const char *)args);
    if (!pva_cfg_get(k, v, sizeof v)) return PVA_E_NOTFOUND;
    set_reply_str(v);
    return PVA_OK;
}

static int cmd_cfg_set(void)
{
    args[PVA_ARGS_SIZE - 1] = 0;
    const char *k = (const char *)args;
    size_t kl = strlen(k);
    if (!kl || kl > 60 || kl + 2 >= PVA_ARGS_SIZE) return PVA_E_ARGS;
    const char *v = k + kl + 1;
    if (!pva_cfg_set(k, v)) return PVA_E_FULL;
    if (!strncmp(k, "location.", 9)) pva_clock_cfg_changed();
    return PVA_OK;
}

static int cmd_cfg_save(void)
{
    if (!card_writes) return PVA_E_RDONLY;
    if (!mounted && !do_mount()) return PVA_E_NOCARD;
    char fp[PATH_MAX_LEN + 8];
    size_t n;
    const char *t = pva_cfg_text(&n);
    fs_refresh();
    fatfs_path(fp, sizeof fp, CFG_FILE);
    FRESULT fr = f_open(m3u_file, fp, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { last_fr = (uint8_t)fr; return PVA_E_IO; }
    UINT put = 0;
    fr = f_write(m3u_file, t, (UINT)n, &put);
    FRESULT fc = f_close(m3u_file);
    if (fr != FR_OK || fc != FR_OK || put != n) { last_fr = (uint8_t)(fr ? fr : fc); return PVA_E_IO; }
    return PVA_OK;
}

static int cmd_cfg_text(void)
{
    size_t n;
    const char *t = pva_cfg_text(&n);
    set_reply_text(t, n);
    return PVA_OK;
}

// ---------------------------------------------------------------- playback
static void set_title_from_path(const char *path)
{
    const char *n = strrchr(path, '/');
    n = n ? n + 1 : path;
    char tmp[PVA_ST_TITLE_LEN];
    pva_copy_ascii(tmp, sizeof tmp, n, 255);
    char *dot = strrchr(tmp, '.');
    if (dot && dot != tmp) *dot = 0;
    memcpy(info.title, tmp, sizeof info.title);
}

static bool is_url(const char *p) { return !strncmp(p, "http://", 7) || !strncmp(p, "https://", 8); }

static void split_stream_title(const char *t)
{
    // "Artist - Title" -> artist, title
    const char *d = strstr(t, " - ");
    if (d && d != t)
    {
        size_t a = (size_t)(d - t);
        if (a >= sizeof info.artist) a = sizeof info.artist - 1;
        memcpy(info.artist, t, a);
        info.artist[a] = 0;
        snprintf(info.title, sizeof info.title, "%s", d + 3);
    }
    else
    {
        snprintf(info.title, sizeof info.title, "%s", t);
        info.artist[0] = 0;
    }
}

static int open_stream(const char *entry)
{
    char u[URL_MAX_LEN + 1];
    const char *sep = strchr(entry, STATION_SEP);
    size_t n = sep ? (size_t)(sep - entry) : strlen(entry);
    if (n > URL_MAX_LEN) return PVA_E_ARGS;
    memcpy(u, entry, n);
    u[n] = 0;
    given_station[0] = 0;
    const char *uuid = NULL;
    if (sep)
    {
        const char *sep2 = strchr(sep + 1, STATION_SEP);
        size_t k = sep2 ? (size_t)(sep2 - sep - 1) : strlen(sep + 1);
        if (k >= sizeof given_station) k = sizeof given_station - 1;
        memcpy(given_station, sep + 1, k);
        given_station[k] = 0;
        if (sep2) uuid = sep2 + 1;
    }
    int r = pva_net_start(u);
    if (r != PVA_OK) return r;
    if (uuid) pva_dir_click(uuid);          // Radio Browser counts the station's listeners
    r = pva_dec_open_stream(&info);
    if (r != PVA_OK) { pva_net_stop(); return r; }
    snprintf(info.album, sizeof info.album, "%s", given_station);
    if (given_station[0]) snprintf(info.title, sizeof info.title, "%s", given_station);
    else
    {
        // the host name until the station tells its name
        const char *h = strstr(u, "://");
        h = h ? h + 3 : u;
        size_t k = 0;
        while (h[k] && h[k] != '/' && h[k] != ':' && k + 1 < sizeof info.title) k++;
        memcpy(info.title, h, k);
        info.title[k] = 0;
    }
    buffering = true;
    prefill = (uint32_t)(pva_net_capacity() / 4u);
    if (prefill > 24u * 1024u) prefill = 24u * 1024u;
    stream_underruns = 0;
    shown_title_gen = 0;
    return PVA_OK;
}

static int open_track(const char *path)
{
    if (is_url(path)) return open_stream(path);
    buffering = false;
    if (pva_net_active()) pva_net_stop();
    char fp[PATH_MAX_LEN + 8];
    fatfs_path(fp, sizeof fp, path);
    fs_refresh();
    int r = pva_dec_open(fp, &info);
    if (r == PVA_E_NOTFOUND && pl_kind == 1)
    {
        // M3U written on another computer: try the file name in the list's folder
        const char *n = strrchr(path, '/');
        n = n ? n + 1 : path;
        char alt[PATH_MAX_LEN + 8];
        snprintf(alt, sizeof alt, "0:%s/%s", pl_m3u_dir, n);
        r = pva_dec_open(alt, &info);
    }
    if (r == PVA_OK && !info.title[0]) set_title_from_path(path);
    return r;
}

// Start playing play list position 'pos', skipping tracks that fail.
static int play_pos(uint16_t pos)
{
    if (!pl_count) return PVA_E_STATE;
    int r = PVA_E_STATE;
    for (uint16_t tries = 0; tries < pl_count && tries < 16; tries++)
    {
        uint16_t p = (uint16_t)((pos + tries) % pl_count);
        const char *path = pl_pool + pl_off[pl_order[p]];
        r = open_track(path);
        if (r == PVA_OK)
        {
            pl_pos = p;
            track_id++;
            play_error = 0;
            state = PVA_STATE_PLAYING;
            pva_hal_audio_mute(buffering);      // a stream stays silent until its buffer is filled
            pva_log("play %u/%u %.32s", (unsigned)(p + 1), (unsigned)pl_count, info.title);
            status_dirty = true;
            return PVA_OK;
        }
        play_error = (uint8_t)r;
        pva_log("skip %.40s (%d)", path, r);
    }
    state = PVA_STATE_STOPPED;
    status_dirty = true;
    return r;
}

static void stop_playback(void)
{
    if (pva_dec_is_stream() || pva_net_active()) pva_net_stop();
    buffering = false;
    pva_dec_close();
    state = PVA_STATE_STOPPED;
    pva_hal_audio_mute(true);
    status_dirty = true;
}

static void track_ended(void)
{
    if (pva_dec_is_stream())
    {
        // a stream only ends when it failed for good
        play_error = PVA_E_NET;
        stop_playback();
        pva_log("stream ended (error %u)", (unsigned)pva_net_stats()->err);
        return;
    }
    if (mode & PVA_MODE_REPEAT_ONE) { play_pos(pl_pos); return; }
    if (pl_pos + 1u < pl_count) { play_pos((uint16_t)(pl_pos + 1u)); return; }
    if (mode & PVA_MODE_REPEAT_ALL)
    {
        if (mode & PVA_MODE_SHUFFLE) pl_shuffle(pl_order[0]);
        play_pos(0);
        return;
    }
    stop_playback();
    pva_log("end of play list");
}

static int cmd_play_index(void)
{
    if (!dir_open) return PVA_E_STATE;
    uint16_t idx = (uint16_t)(args[0] | (args[1] << 8));
    if (idx >= dir_count) return PVA_E_RANGE;
    const dent_t *sel = &dir_ent[idx];
    char path[PATH_MAX_LEN + 1];
    if (sel->type == PVA_DE_URL && vlevel == V_STATIONS)
    {
        // the list's stations make the play list
        if (pva_dir_kind() != PVA_DIRQ_STATIONS || pva_dir_count() + 1u + (st_more ? 1u : 0u) != dir_count) return PVA_E_STATE;
        stop_playback();
        pl_clear();
        pl_kind = 2;
        uint16_t start = 0;
        for (uint16_t i = 0; i < pva_dir_count(); i++)
        {
            char e[URL_MAX_LEN + PVA_ST_ALBUM_LEN + 44];
            char nm[PVA_ST_ALBUM_LEN];
            pva_copy_ascii(nm, sizeof nm, dir_pool + dir_ent[i + 1u].name, 255);
            const char *u = pva_dir_url(i);
            if (strlen(u) > URL_MAX_LEN) continue;
            snprintf(e, sizeof e, "%s%c%s%c%s", u, STATION_SEP, nm, STATION_SEP, pva_dir_uuid(i));
            if (i + 1u == idx) start = pl_count;
            if (!pl_add(e)) break;
        }
        if (!pl_count) return PVA_E_STATE;
        if (mode & PVA_MODE_SHUFFLE) { pl_shuffle(start); start = 0; }
        return play_pos(start);
    }
    if (sel->type == PVA_DE_URL && vlevel == V_FAVS)
    {
        stop_playback();
        pl_clear();
        pl_kind = 2;
        uint16_t start = 0;
        for (uint16_t i = 1; i < dir_count; i++)
        {
            const char *nm = dir_pool + dir_ent[i].name, *u = nm + strlen(nm) + 1;
            char e[URL_MAX_LEN + PVA_ST_ALBUM_LEN + 2];
            char n2[PVA_ST_ALBUM_LEN];
            snprintf(n2, sizeof n2, "%s", nm);
            if (strlen(u) > URL_MAX_LEN) continue;
            snprintf(e, sizeof e, "%s%c%s", u, STATION_SEP, n2);
            if (i == idx) start = pl_count;
            if (!pl_add(e)) break;
        }
        if (!pl_count) return PVA_E_STATE;
        if (mode & PVA_MODE_SHUFFLE) { pl_shuffle(start); start = 0; }
        return play_pos(start);
    }
    if (sel->type == PVA_DE_M3U)
    {
        snprintf(path, sizeof path, "%s/%s", cur_dir, dir_pool + sel->name);
        stop_playback();
        int r = load_m3u(path);
        if (r != PVA_OK) return r;
        if (mode & PVA_MODE_SHUFFLE) pl_shuffle((uint16_t)(rand32() % pl_count));
        return play_pos(0);
    }
    if (sel->type != PVA_DE_MP3 && sel->type != PVA_DE_WAV) return PVA_E_FORMAT;
    stop_playback();
    pl_clear();
    uint16_t start = 0;
    for (uint16_t i = 0; i < dir_count; i++)
    {
        const dent_t *e = &dir_ent[i];
        if (e->type != PVA_DE_MP3 && e->type != PVA_DE_WAV) continue;
        if (i == idx) start = pl_count;
        snprintf(path, sizeof path, "%s/%s", cur_dir, dir_pool + e->name);
        if (!pl_add(path))
        {
            pva_log("play list cut at %u", (unsigned)pl_count);
            if (i <= idx) { pl_clear(); snprintf(path, sizeof path, "%s/%s", cur_dir, dir_pool + sel->name); pl_add(path); start = 0; }
            break;
        }
    }
    if (mode & PVA_MODE_SHUFFLE) { pl_shuffle(start); start = 0; }
    return play_pos(start);
}

static int play_url(const char *u, const char *name)
{
    size_t ul = strlen(u);
    if (!is_url(u) || ul > URL_MAX_LEN) return PVA_E_ARGS;
    char e[URL_MAX_LEN + PVA_ST_ALBUM_LEN + 2];
    int n = snprintf(e, sizeof e, "%s", u);
    if (name[0])
    {
        char nm[PVA_ST_ALBUM_LEN];
        pva_copy_ascii(nm, sizeof nm, name, strlen(name));
        snprintf(e + n, sizeof e - (size_t)n, "%c%s", STATION_SEP, nm);
    }
    stop_playback();
    pl_clear();
    pl_kind = 2;
    pl_add(e);
    return play_pos(0);
}

// args: URL, 0, optional station name, 0
static int cmd_play_url(void)
{
    args[PVA_ARGS_SIZE - 1] = 0;
    const char *u = (const char *)args;
    size_t ul = strlen(u);
    return play_url(u, ul + 1 < PVA_ARGS_SIZE ? u + ul + 1 : "");
}

static int play_path(const char *p)
{
    char path[PATH_MAX_LEN + 1];
    if (is_url(p)) return play_url(p, "");
    if (!normalise_path(path, sizeof path, p) || !path[0]) return PVA_E_ARGS;
    if (!mounted && !do_mount()) return PVA_E_NOCARD;
    stop_playback();
    if (pva_kind_from_name(path) == PVA_DE_M3U)
    {
        int r = load_m3u(path);
        if (r != PVA_OK) return r;
        return play_pos(0);
    }
    pl_clear();
    pl_add(path);
    return play_pos(0);
}

static int cmd_play_path(void)
{
    args[PVA_ARGS_SIZE - 1] = 0;
    return play_path((const char *)args);
}

// ---------------------------------------------------------------- sleep timer, alarm
#define SLEEP_FADE_MS   30000u      // the last 30 s of the sleep timer fade out
#define ALARM_FADE_MS   60000u      // the alarm fades in over a minute
static bool     sleep_on, al_on, al_weekdays, al_ringing, al_fired, al_fading;
static uint32_t sleep_end, al_fade_start, sa_last;
static uint8_t  al_hour = 7, al_min = 0;
static uint8_t  fade_vol = 0xFF;    // the volume a fade has set (FF: none)

static void fade_to(uint8_t v)
{
    if (v != fade_vol) { fade_vol = v; pva_dec_set_volume(muted ? 0 : v); }
}

static void fade_end(void)
{
    if (fade_vol != 0xFF) { fade_vol = 0xFF; pva_dec_set_volume(muted ? 0 : volume); }
}

// the [alarm] settings
static void alarm_apply(void)
{
    char v[16];
    pva_cfg_get("alarm.on", v, sizeof v);
    al_on = !strcmp(v, "on");
    pva_cfg_get("alarm.days", v, sizeof v);
    al_weekdays = !strcmp(v, "weekdays");
    pva_cfg_get("alarm.time", v, sizeof v);
    int h, m;
    if (sscanf(v, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) { al_hour = (uint8_t)h; al_min = (uint8_t)m; }
}

static void alarm_ring(void)
{
    char what[URL_MAX_LEN + 1], name[PVA_ST_ALBUM_LEN];
    pva_cfg_get("alarm.play", what, sizeof what);
    pva_cfg_get("alarm.name", name, sizeof name);
    if (!what[0]) return;
    fade_to(0);
    int r = is_url(what) ? play_url(what, name) : (mounted || do_mount() ? play_path(what) : PVA_E_NOCARD);
    pva_log("alarm: %.40s (%d)", name[0] ? name : what, r);
    if (r != PVA_OK) { fade_end(); return; }
    al_ringing = true;
    al_fading = true;
    al_fade_start = pva_hal_millis();
}

static void sleep_alarm_poll(void)
{
    uint32_t now = pva_hal_millis();
    if (now - sa_last < 100u) return;
    sa_last = now;
    if (sleep_on)
    {
        int32_t rem = (int32_t)(sleep_end - now);
        if (rem <= 0)
        {
            sleep_on = false;
            if (state != PVA_STATE_STOPPED) stop_playback();
            fade_end();
            pva_log("sleep: stopped");
        }
        else if ((uint32_t)rem < SLEEP_FADE_MS)
            fade_to((uint8_t)((uint32_t)volume * (uint32_t)rem / SLEEP_FADE_MS));
    }
    else if (al_fading)
    {
        uint32_t el = now - al_fade_start;
        if (el >= ALARM_FADE_MS) { al_fading = false; fade_end(); }
        else fade_to((uint8_t)((uint32_t)volume * el / ALARM_FADE_MS));
    }
    uint8_t h, m, wd;
    if (al_on && pva_clock_local(&h, &m, &wd))
    {
        if (h == al_hour && m == al_min)
        {
            if (!al_fired && (!al_weekdays || wd < 5)) { al_fired = true; alarm_ring(); }
        }
        else al_fired = false;
    }
}

// SLEEP: args u16 minutes, 0 = off
static int cmd_sleep(void)
{
    uint16_t m = (uint16_t)(args[0] | (args[1] << 8));
    sleep_on = m != 0;
    sleep_end = pva_hal_millis() + (uint32_t)m * 60000u;
    if (!sleep_on) fade_end();
    return PVA_OK;
}

// ALARM: args u8 flags (PVA_AL_ON, PVA_AL_WEEKDAYS), u8 hour, u8 minute
static int cmd_alarm(void)
{
    bool on = args[0] & PVA_AL_ON;
    if (args[1] > 23 || args[2] > 59) return PVA_E_ARGS;
    if (on)
    {
        // what plays now becomes the alarm's (else the one it had)
        if (pl_count && state != PVA_STATE_STOPPED)
        {
            char e[URL_MAX_LEN + PVA_ST_ALBUM_LEN + 2];
            snprintf(e, sizeof e, "%s", pl_pool + pl_off[pl_order[pl_pos]]);
            char *sep = strchr(e, STATION_SEP);
            const char *name = "";
            if (sep)
            {
                *sep = 0;
                name = sep + 1;
                char *sep2 = strchr(sep + 1, STATION_SEP);
                if (sep2) *sep2 = 0;
            }
            if (!name[0] && pva_dec_is_stream()) name = given_station[0] ? given_station : info.album;
            if (!pva_cfg_set("alarm.play", e) || !pva_cfg_set("alarm.name", name)) return PVA_E_FULL;
        }
        else
        {
            char v[8];
            if (!pva_cfg_get("alarm.play", v, sizeof v) || !v[0]) return PVA_E_STATE;
        }
    }
    char t[8];
    snprintf(t, sizeof t, "%02u:%02u", (unsigned)args[1], (unsigned)args[2]);
    if (!pva_cfg_set("alarm.on", on ? "on" : "off") || !pva_cfg_set("alarm.time", t) ||
        !pva_cfg_set("alarm.days", (args[0] & PVA_AL_WEEKDAYS) ? "weekdays" : "daily"))
        return PVA_E_FULL;
    alarm_apply();
    al_fired = false;
    al_ringing = false;
    return PVA_OK;
}

void pva_engine_extra(uint8_t *out)
{
    memset(out, 0, PVA_CLOCK_SIZE - PVA_CK_EXTRA);
    if (sleep_on)
    {
        uint32_t rem = sleep_end - pva_hal_millis();
        uint32_t mins = (rem + 59999u) / 60000u;
        out[PVA_CK_SLEEP - PVA_CK_EXTRA] = (uint8_t)(mins > 255u ? 255u : mins < 1u ? 1u : mins);
    }
    out[PVA_CK_ALARM - PVA_CK_EXTRA] = (uint8_t)((al_on ? PVA_AL_ON : 0) | (al_ringing ? PVA_AL_RINGING : 0) | (al_weekdays ? PVA_AL_WEEKDAYS : 0));
    out[PVA_CK_AL_HOUR - PVA_CK_EXTRA] = al_hour;
    out[PVA_CK_AL_MIN - PVA_CK_EXTRA] = al_min;
    pva_news_state(&out[PVA_CK_NEWS_STATE - PVA_CK_EXTRA]);
}

static int cmd_mode(void)
{
    uint8_t m = args[0] & (PVA_MODE_REPEAT_ALL | PVA_MODE_SHUFFLE | PVA_MODE_REPEAT_ONE);
    bool was = mode & PVA_MODE_SHUFFLE, now = m & PVA_MODE_SHUFFLE;
    mode = m;
    if (pl_count && now && !was)
    {
        uint16_t cur = pl_order[pl_pos];
        pl_shuffle(cur);
        pl_pos = 0;
    }
    else if (pl_count && was && !now)
        pl_unshuffle();
    return PVA_OK;
}

void pva_set_stack(const uint8_t *base, size_t size) { stack_base = base; stack_size = size; }

static size_t stack_used(void)
{
    if (!stack_base) return 0;
    size_t i = 0;
    while (i < stack_size && stack_base[i] == 0xA5) i++;   // stack grows down to the base
    return stack_size - i;
}

// ---------------------------------------------------------------- text replies
static int cmd_diag(void)
{
    char t[1400];
    uint32_t up = pva_hal_millis() / 1000u;
    // lines of at most ~56 characters: PicoPlay's text window wraps at 60
    int n = snprintf(t, sizeof t,
        "PicoVerse Audio Engine %d.%d (API %d)\r\n"
        "Uptime %lu s, heartbeat %u, longest gap %lu ms\r\n"
        "microSD: %s (mount FRESULT %d)\r\n",
        PVA_FW_MAJOR, PVA_FW_MINOR, PVA_API_VERSION,
        (unsigned long)up, (unsigned)hb, (unsigned long)max_gap_ms,
        mounted ? "mounted" : "not mounted", (int)mount_fr);
    if (mounted)
        n += snprintf(t + n, sizeof t - (size_t)n, "Volume: FAT%s, %lu sectors, cluster %u KB\r\n",
                      fs.fs_type == FS_FAT12 ? "12" : fs.fs_type == FS_FAT16 ? "16" : fs.fs_type == FS_FAT32 ? "32" : "ex",
                      (unsigned long)((fs.n_fatent - 2u) * fs.csize), (unsigned)(fs.csize / 2u));
    n += snprintf(t + n, sizeof t - (size_t)n,
        "Folder: %.36s (%u entries)\r\n"
        "Play list: %u, position %u, mode %u\r\n"
        "State %u, error %u, last FRESULT %d/%d\r\n"
        "Track: %u Hz, %u ch, %lu kbit/s, %lu s\r\n"
        "Arena free %lu, stack %lu/%lu, log %u lines\r\n",
        cur_dir[0] ? cur_dir : "/", (unsigned)dir_count,
        (unsigned)pl_count, (unsigned)pl_pos, (unsigned)mode,
        (unsigned)state, (unsigned)play_error, (int)last_fr, (int)pva_dec_last_fresult(),
        (unsigned)info.sample_rate, (unsigned)info.channels, (unsigned long)info.bitrate_kbps, (unsigned long)info.total_seconds,
        (unsigned long)pva_arena_free(), (unsigned long)stack_used(), (unsigned long)stack_size, (unsigned)pva_log_count());
    const pva_net_stats_t *ns = pva_net_stats();
    if (pva_net_active() || ns->rx_bytes || ns->err)
    {
        uint32_t need = info.bitrate_kbps * 10u / 8u;      // 0.1 KB/s
        n += snprintf(t + n, sizeof t - (size_t)n,
            "Stream: %.40s%s\r\n"
            "  net state %u, error %u, HTTP %u\r\n"
            "  %lu.%lu KB/s (needs %lu.%lu), %lu KB received\r\n"
            "  buffer %lu/%lu KB%s, underruns %u, reconnects %u\r\n"
            "  gaps %lu, longest %lu ms, ESP errors %lu, lost %lu\r\n"
            "  DNS %lu ms, open %lu ms, read size %u\r\n",
            pva_net_host(), ns->tls ? " (TLS)" : "",
            (unsigned)ns->state, (unsigned)ns->err, (unsigned)ns->http,
            (unsigned long)(ns->rate_bps / 1000u), (unsigned long)((ns->rate_bps / 100u) % 10u),
            (unsigned long)(need / 10u), (unsigned long)(need % 10u), (unsigned long)(ns->rx_bytes / 1024u),
            (unsigned long)(pva_net_buffered() / 1024u), (unsigned long)(pva_net_capacity() / 1024u), buffering ? " filling" : "",
            (unsigned)stream_underruns, (unsigned)ns->reconnects,
            (unsigned long)ns->gaps, (unsigned long)ns->longest_gap_ms, (unsigned long)ns->esp_errors, (unsigned long)ns->overflow,
            (unsigned long)ns->dns_ms, (unsigned long)ns->open_ms, (unsigned)ns->rcv_max);
    }
    else if (!pva_net_capacity())
        n += snprintf(t + n, sizeof t - (size_t)n, "Streams: no buffer memory\r\n");
    {
        int8_t g[PVA_EQ_BANDS];
        pva_eq_get(g);
        n += snprintf(t + n, sizeof t - (size_t)n, "Sound: volume %u, balance %u, EQ %d %d %d %d %d dB\r\n",
                      (unsigned)volume, (unsigned)balance, g[0], g[1], g[2], g[3], g[4]);
    }
    if (pva_dir_available())
        n += snprintf(t + n, sizeof t - (size_t)n, "Radio list: state %u, %u entries%s, error %u, HTTP %u\r\n",
                      (unsigned)pva_dir_state(), (unsigned)pva_dir_count(), pva_dir_truncated() ? " (cut)" : "",
                      (unsigned)pva_dir_error(), (unsigned)pva_dir_http());
    set_reply_text(t, (size_t)n);
    return PVA_OK;
}

static int execute(uint8_t code)
{
    // the music is the listener's again: the alarm stops ringing (and fading in)
    if (code == PVA_C_PLAY_INDEX || code == PVA_C_PLAY_PATH || code == PVA_C_PLAY_URL || code == PVA_C_STOP ||
        code == PVA_C_PAUSE || code == PVA_C_NEXT || code == PVA_C_PREV)
    {
        al_ringing = false;
        if (al_fading) { al_fading = false; fade_end(); }
    }
    if (!ready && code != PVA_C_PING && code != PVA_C_LOG && code != PVA_C_DIAG)
        return PVA_E_STATE;
    switch (code)
    {
        case PVA_C_PING:
        {
            char t[40];
            int n = snprintf(t, sizeof t, "PVAUDIO %d.%d", PVA_FW_MAJOR, PVA_FW_MINOR);
            set_reply_text(t, (size_t)n + 1u);
            return PVA_OK;
        }
        case PVA_C_MOUNT:
            stop_playback();
            radio_stop_wait();
            vlevel = V_CARD;
            dir_open = false;
            return do_mount() ? PVA_OK : PVA_E_NOCARD;
        case PVA_C_LOG:
            reply_len = (uint32_t)pva_log_dump((char *)reply, reply_cap);
            return PVA_OK;
        case PVA_C_DIAG:      return cmd_diag();
        case PVA_C_DIR_OPEN:  return cmd_dir_open();
        case PVA_C_DIR_READ:  return cmd_dir_read();
        case PVA_C_DIR_ENTER: return cmd_dir_enter();
        case PVA_C_ENTRY_INFO: return cmd_entry_info();
        case PVA_C_DIR_SEARCH: return cmd_dir_search();
        case PVA_C_FAV_ADD:   return cmd_fav_add();
        case PVA_C_FAV_DEL:   return cmd_fav_del();
        case PVA_C_CFG_LOAD:  return cmd_cfg_load();
        case PVA_C_CFG_GET:   return cmd_cfg_get();
        case PVA_C_CFG_SET:   return cmd_cfg_set();
        case PVA_C_CFG_SAVE:  return cmd_cfg_save();
        case PVA_C_CFG_TEXT:  return cmd_cfg_text();
        case PVA_C_CLOCK_INFO: return pva_clock_info();
        case PVA_C_LOC_IP:    return pva_clock_loc_ip();
        case PVA_C_LOC_SEARCH: args[PVA_ARGS_SIZE - 1] = 0; return pva_clock_search((const char *)args);
        case PVA_C_LOC_LIST:  return pva_clock_list();
        case PVA_C_LOC_PICK:  return pva_clock_pick(args[0]);
        case PVA_C_WX_GET:    return pva_clock_wx();
        case PVA_C_SLEEP:     return cmd_sleep();
        case PVA_C_ALARM:     return cmd_alarm();
        case PVA_C_NEWS_FEEDS: case PVA_C_NEWS_FETCH: case PVA_C_NEWS_LIST: case PVA_C_NEWS_ITEM:
        case PVA_C_NEWS_TOPICS: case PVA_C_NEWS_CAT: case PVA_C_NEWS_ADD: case PVA_C_NEWS_DEL:
        case PVA_C_NEWS_MOVE: case PVA_C_NEWS_FULL: case PVA_C_NEWS_TEXT: case PVA_C_NEWS_INFO:
        case PVA_C_NEWS_SORT:
            return pva_news_cmd(code, args);
        case PVA_C_PLAY_INDEX: return cmd_play_index();
        case PVA_C_PLAY_PATH: return cmd_play_path();
        case PVA_C_PLAY_URL:  return cmd_play_url();
        case PVA_C_PAUSE:
            if (state == PVA_STATE_PLAYING) { state = PVA_STATE_PAUSED; pva_hal_audio_mute(true); }
            else if (state == PVA_STATE_PAUSED) { state = PVA_STATE_PLAYING; pva_hal_audio_mute(buffering); }
            else if (state == PVA_STATE_STOPPED && pl_count) return play_pos(pl_pos);
            else return PVA_E_STATE;
            return PVA_OK;
        case PVA_C_STOP:
            stop_playback();
            play_error = 0;
            return PVA_OK;
        case PVA_C_NEXT:
            if (!pl_count) return PVA_E_STATE;
            if (pl_pos + 1u < pl_count) return play_pos((uint16_t)(pl_pos + 1u));
            return play_pos(0);
        case PVA_C_PREV:
            if (!pl_count) return PVA_E_STATE;
            if (pva_dec_active() && pva_dec_elapsed() >= 3u) return play_pos(pl_pos);
            return play_pos(pl_pos ? (uint16_t)(pl_pos - 1u) : (uint16_t)(pl_count - 1u));
        case PVA_C_SEEK:
        {
            if (!pva_dec_active()) return PVA_E_STATE;
            if (!info.seekable) return PVA_E_STATE;
            return pva_dec_seek((uint32_t)(args[0] | (args[1] << 8)));
        }
        case PVA_C_VOLUME:
            volume = args[0] > 100 ? 100 : args[0];
            pva_dec_set_volume(muted ? 0 : volume);
            if (fade_vol != 0xFF) fade_vol = 0xFE;     // (the fade sets it again)
            return PVA_OK;
        case PVA_C_MUTE:
            muted = args[0] == 2 ? !muted : args[0] != 0;
            pva_dec_set_volume(muted ? 0 : (fade_vol < 0xFE ? fade_vol : volume));
            status_dirty = true;
            set_reply_byte(muted);
            return PVA_OK;
        case PVA_C_MODE:
            return cmd_mode();
        case PVA_C_BALANCE:
            balance = args[0] > 100 ? 100 : args[0];
            pva_dec_set_balance(balance);
            status_dirty = true;
            return PVA_OK;
        case PVA_C_EQ:
            pva_eq_set((const int8_t *)args);
            status_dirty = true;
            return PVA_OK;
        default:
            return PVA_E_UNKNOWN;
    }
}

static void publish_status(void);

static void handle_command(void)
{
    uint32_t post = pva_mailbox.post_count;
    if (post == handled_post) return;
    pva_barrier();
    uint8_t token = pva_mailbox.token;
    uint8_t code = pva_mailbox.code;
    handled_post = post;
    pva_win[PVA_R_BUSY] = 1;
    pva_win[PVA_R_LAST_CMD] = code;
    pva_win[PVA_R_WHERE] = PVA_W_COMMAND;
    for (uint32_t i = 0; i < PVA_ARGS_SIZE; i++) args[i] = pva_win[PVA_R_ARGS + i];
    reply_len = 0;
    int r = execute(code);
    for (uint32_t i = 0; i < reply_len; i++) pva_win[PVA_R_REPLY + i] = reply[i];
    pva_win[PVA_R_RESULT] = (uint8_t)r;
    pva_win[PVA_R_REPLY_LEN] = (uint8_t)(reply_len & 0xFF);
    pva_win[PVA_R_REPLY_LEN + 1] = (uint8_t)(reply_len >> 8);
    // the status must already show what the command did when the MSX sees it done
    publish_status();
    status_dirty = false;
    pva_barrier();
    pva_win[PVA_R_DONE_TOKEN] = token;
    pva_win[PVA_R_BUSY] = 0;
}

// ---------------------------------------------------------------- status block
static uint8_t *st_new, *st_pub;    // PVA_STATUS_SIZE each, in the arena
static uint8_t  disp_gen, disp_buf;
static uint16_t disp_rate;
static uint32_t disp_rate_ms;
static void st8(uint32_t o, uint8_t v) { st_new[o] = v; }
static void st16(uint32_t o, uint16_t v) { st8(o, (uint8_t)v); st8(o + 1, (uint8_t)(v >> 8)); }
static void ststr(uint32_t o, const char *s, uint32_t size)
{
    uint32_t i = 0;
    for (; i + 1 < size && s[i]; i++) st8(o + i, (uint8_t)s[i]);
    for (; i < size; i++) st8(o + i, 0);
}

// Build the block, and only when it differs from the published one, copy it
// into the window inside the sequence-byte bracket (so a reader rarely has
// to retry).
static void publish_status(void)
{
    uint8_t vl = 0, vr = 0;
    if (state == PVA_STATE_PLAYING) pva_dec_vu(&vl, &vr);
    bool active = pva_dec_active();
    bool strm = active && pva_dec_is_stream();
    if (strm) pva_dec_frame_info(&info);
    st8(PVA_ST_STATE, state);
    st8(PVA_ST_FLAGS, (uint8_t)((mounted ? PVA_F_MOUNTED : 0) | (info.tagged && active ? PVA_F_TAGS : 0) |
                                (info.seekable && active ? PVA_F_SEEKABLE : 0) | (dir_open ? PVA_F_DIR_OPEN : 0) |
                                (strm ? PVA_F_STREAM : 0) | (strm && buffering ? PVA_F_BUFFERING : 0) |
                                (strm && pva_net_is_tls() ? PVA_F_TLS : 0) | (muted ? PVA_F_MUTED : 0)));
    st8(PVA_ST_VOLUME, volume);
    st16(PVA_ST_ELAPSED, active ? (uint16_t)pva_dec_elapsed() : 0);
    st16(PVA_ST_TOTAL, active ? (uint16_t)info.total_seconds : 0);
    st16(PVA_ST_BITRATE, active ? (uint16_t)info.bitrate_kbps : 0);
    st16(PVA_ST_RATE, active ? (uint16_t)(info.sample_rate / 10u) : 0);
    st8(PVA_ST_CHANNELS, active ? info.channels : 0);
    st8(PVA_ST_ERROR, play_error);
    st8(PVA_ST_FRESULT, last_fr ? last_fr : pva_dec_last_fresult());
    st16(PVA_ST_TRACK_ID, track_id);
    st8(PVA_ST_VU_L, vl);
    st8(PVA_ST_VU_R, vr);
    st16(PVA_ST_PL_POS, pl_pos);
    st16(PVA_ST_PL_COUNT, pl_count);
    st8(PVA_ST_MODE, mode);
    st16(PVA_ST_UNDERRUNS, stream_underruns);
    st8(PVA_ST_MOUNT_FR, mount_fr);
    st8(PVA_ST_DIR_GEN, dir_gen);
    st16(PVA_ST_UPTIME, (uint16_t)(pva_hal_millis() / 1000u));
    st16(PVA_ST_LOG_COUNT, pva_log_count());
    ststr(PVA_ST_TITLE, active || state != PVA_STATE_STOPPED ? info.title : "", PVA_ST_TITLE_LEN);
    ststr(PVA_ST_ARTIST, active ? info.artist : "", PVA_ST_ARTIST_LEN);
    ststr(PVA_ST_ALBUM, active ? info.album : "", PVA_ST_ALBUM_LEN);
    st8(PVA_ST_FORMAT, active ? info.format : 0);
    st8(PVA_ST_PL_KIND, pl_kind);
    {
        const pva_net_stats_t *ns = pva_net_stats();
        size_t cap = pva_net_capacity();
        st8(PVA_ST_NET_STATE, strm ? ns->state : 0);
        st8(PVA_ST_BUF_PCT, strm && cap ? (uint8_t)(pva_net_buffered() * 100u / cap) : 0);
        st16(PVA_ST_NET_RATE, strm ? (uint16_t)(ns->rate_bps / 100u) : 0);
        st16(PVA_ST_RECONNECTS, strm ? ns->reconnects : 0);
        st8(PVA_ST_NET_ERR, ns->err);
        st16(PVA_ST_HTTP, ns->http);
        st16(PVA_ST_BUF_KB, (uint16_t)(cap / 1024u));
    }
    {
        int8_t g[PVA_EQ_BANDS];
        pva_eq_get(g);
        for (int i = 0; i < PVA_EQ_BANDS; i++) st8(PVA_ST_EQ + (uint32_t)i, (uint8_t)g[i]);
    }
    st8(PVA_ST_BALANCE, balance);
    st8(PVA_ST_DIR_STATE, dstate);
    st8(PVA_ST_DIR_ERR, derr);
    // display fields: coarse buffer level and rate, then the generation
    {
        uint8_t pct = st_new[PVA_ST_BUF_PCT];
        if (!strm) disp_buf = 0;
        else if (pct >= 97) disp_buf = 100;
        else if (pct + 12u < disp_buf || pct >= disp_buf + 10u) disp_buf = (uint8_t)((pct / 10u) * 10u);
        uint32_t now = pva_hal_millis();
        if (!strm) disp_rate = 0;
        else if (now - disp_rate_ms >= 5000u || !disp_rate)
        {
            disp_rate = (uint16_t)(st_new[PVA_ST_NET_RATE] | (st_new[PVA_ST_NET_RATE + 1] << 8));
            disp_rate_ms = now;
        }
        st8(PVA_ST_DISP_BUF, disp_buf);
        st16(PVA_ST_DISP_RATE, disp_rate);
        // compare everything except the sequence, VU levels, uptime, log count,
        // the raw buffer level and rate, and the generation itself
        static uint8_t view[PVA_STATUS_SIZE];
        uint8_t tmp[PVA_STATUS_SIZE];
        memcpy(tmp, st_new, PVA_STATUS_SIZE);
        tmp[PVA_ST_SEQ] = tmp[PVA_ST_VU_L] = tmp[PVA_ST_VU_R] = 0;
        tmp[PVA_ST_UPTIME] = tmp[PVA_ST_UPTIME + 1] = 0;
        tmp[PVA_ST_LOG_COUNT] = tmp[PVA_ST_LOG_COUNT + 1] = 0;
        tmp[PVA_ST_BUF_PCT] = tmp[PVA_ST_NET_RATE] = tmp[PVA_ST_NET_RATE + 1] = 0;
        tmp[PVA_ST_DISP_GEN] = 0;
        if (memcmp(tmp, view, PVA_STATUS_SIZE))
        {
            memcpy(view, tmp, PVA_STATUS_SIZE);
            disp_gen++;
        }
        st8(PVA_ST_DISP_GEN, disp_gen);
    }
    st_new[PVA_ST_SEQ] = st_pub[PVA_ST_SEQ];
    if (!memcmp(st_new, st_pub, PVA_STATUS_SIZE)) return;
    seq = (uint8_t)(seq | 1u);
    pva_win[PVA_R_STATUS + PVA_ST_SEQ] = seq;
    pva_barrier();
    for (uint32_t i = 1; i < PVA_STATUS_SIZE; i++)
        if (st_new[i] != st_pub[i]) pva_win[PVA_R_STATUS + i] = st_new[i];
    memcpy(st_pub, st_new, PVA_STATUS_SIZE);
    pva_barrier();
    seq = (uint8_t)(seq + 1u);
    pva_win[PVA_R_STATUS + PVA_ST_SEQ] = seq;
    st_pub[PVA_ST_SEQ] = seq;
}

// ---------------------------------------------------------------- core 1
void pva_core1_init(void)
{
    pva_hal_install_fault_handler();
    args = pva_alloc(PVA_ARGS_SIZE);
    st_new = pva_alloc(PVA_STATUS_SIZE);
    st_pub = pva_alloc(PVA_STATUS_SIZE);
    m3u_file = pva_alloc(sizeof(FIL));
    m3u_lr = pva_alloc(sizeof(line_reader_t));
    dir_ent = pva_alloc(DIR_MAX * sizeof(dent_t));
    dir_pool = pva_alloc(DIR_POOL);
    pl_off = pva_alloc(PL_MAX * sizeof(uint16_t));
    pl_order = pva_alloc(PL_MAX * sizeof(uint16_t));
    pl_pool = pva_alloc(PL_POOL);
    reply = pva_alloc(PVA_REPLY_SIZE);
    bool dec_ok = pva_dec_init();
    // the Nextor modes leave the engine less RAM: the news then takes some of the stream RAM
    size_t news_keep = pva_arena_size() < 160u * 1024u ? PVA_NEWS_KEEP : 0u;
    bool net_ok = pva_net_init(news_keep);
    pva_viz_init();                     // takes some of the stream RAM; before the directory
    if (net_ok && news_keep) pva_news_init(news_keep);
    if (net_ok && !pva_web_init()) pva_log("web: no memory");
    if (net_ok && !pva_dir_init()) pva_log("radio directory: no memory");
    bool hal_ok = pva_hal_audio_init();
    pva_hal_audio_mute(true);
    pva_log("engine %d.%d start, arena free %lu", PVA_FW_MAJOR, PVA_FW_MINOR, (unsigned long)pva_arena_free());
    static uint8_t reply_fallback[64];
    if (!reply) { reply = reply_fallback; reply_cap = sizeof reply_fallback; }
    ready = dir_ent && dir_pool && pl_off && pl_order && pl_pool && reply != reply_fallback && dec_ok &&
            args && st_new && st_pub && m3u_file && m3u_lr && net_ok;
    if (!ready)
        pva_log("ERROR: arena too small, engine disabled");
    if (!hal_ok)
        pva_log("ERROR: I2S audio init failed");
    rng ^= pva_hal_millis();
    do_mount();
    handled_post = 0;                   // a command that started the engine is executed
    last_poll_ms = pva_hal_millis();
    publish_status();
    pva_win[PVA_R_BUSY] = 0;
}

bool pva_core1_busy(void) { return state == PVA_STATE_PLAYING || pva_net_active() || pva_dir_busy(); }
bool pva_command_pending(void) { return pva_mailbox.post_count != 0; }

bool pva_core1_poll(void)
{
    bool work = false;
    uint32_t now = pva_hal_millis();
    uint32_t gap = now - last_poll_ms;
    if (gap > max_gap_ms) max_gap_ms = gap;
    last_poll_ms = now;

    pva_win[PVA_R_WHERE] = PVA_W_LOOP;
    if (pva_mailbox.post_count != handled_post)
    {
        handle_command();
        work = true;
    }

    pva_win[PVA_R_WHERE] = PVA_W_RADIO;
    radio_poll();
    pva_web_poll();
    pva_clock_poll();
    pva_news_poll();
    sleep_alarm_poll();
    pva_win[PVA_R_WHERE] = PVA_W_STREAM;
    if (pva_net_active())
    {
        pva_net_poll();
        const pva_net_stats_t *ns = pva_net_stats();
        if (ns->title_gen != shown_title_gen && pva_dec_is_stream())
        {
            shown_title_gen = ns->title_gen;
            split_stream_title(pva_net_stream_title());
            status_dirty = true;
        }
        if (pva_dec_is_stream() && !given_station[0] && pva_net_station()[0] && strcmp(info.album, pva_net_station()))
        {
            snprintf(info.album, sizeof info.album, "%s", pva_net_station());
            if (!info.artist[0] && !shown_title_gen) snprintf(info.title, sizeof info.title, "%s", info.album);
            status_dirty = true;
        }
    }

    pva_win[PVA_R_WHERE] = PVA_W_DECODE;
    if (ready && state == PVA_STATE_PLAYING && pva_dec_is_stream() && buffering)
    {
        // wait until the buffer has 'prefill' bytes (or the stream gave up)
        size_t b = pva_net_buffered();
        if (b >= prefill || (pva_net_stats()->state == PVA_NET_FAILED && b))
        {
            buffering = false;
            pva_hal_audio_mute(false);
            status_dirty = true;
            pva_log("stream: playing, buffer %lu KB", (unsigned long)(b / 1024u));
        }
        else if (pva_net_ended())
            track_ended();
    }
    else if (ready && state == PVA_STATE_PLAYING)
    {
        int r = pva_dec_step();
        if (r == 0 && pva_dec_is_stream() && pva_net_buffered() < 2048u && !pva_net_ended())
        {
            // the buffer ran dry: fill it again (more this time)
            buffering = true;
            pva_hal_audio_mute(true);
            stream_underruns++;
            prefill = (uint32_t)(pva_net_capacity() / 2u);
            if (prefill > 48u * 1024u) prefill = 48u * 1024u;
            status_dirty = true;
            {
                uint32_t have = pva_net_stats()->rate_bps / 100u, need = info.bitrate_kbps * 10u / 8u;
                pva_log("stream: empty, got %lu.%lu KB/s, needs %lu.%lu",
                        (unsigned long)(have / 10u), (unsigned long)(have % 10u), (unsigned long)(need / 10u), (unsigned long)(need % 10u));
            }
        }
        if (r != 0) work = true;
        if (r < 0)
        {
            uint8_t fr = pva_dec_last_fresult();
            if (fr) { play_error = PVA_E_IO; }
            track_ended();
        }
    }

    if (now - last_hb_ms >= HEARTBEAT_MS)
    {
        last_hb_ms = now;
        pva_win[PVA_R_HEARTBEAT] = ++hb;
    }
    pva_viz_poll(state == PVA_STATE_PLAYING && !buffering);
    pva_win[PVA_R_WHERE] = PVA_W_STATUS;
    if (status_dirty || now - last_status_ms >= STATUS_MS)
    {
        last_status_ms = now;
        status_dirty = false;
        publish_status();
    }
    return work;
}
