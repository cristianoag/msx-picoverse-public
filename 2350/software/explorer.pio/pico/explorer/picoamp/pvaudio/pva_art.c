// MSX PICOVERSE PROJECT
// pva_art.c - PicoVerse Audio Engine: what plays, as a picture, ready for the MSX.
//
// A file: the picture in its ID3 tag (the front cover first), else an image in
// its folder (cover.jpg, folder.jpg, front.jpg ...). A station: its logo. And
// online (engine 0.16, where the engine has the memory: the PicoAmp ROM):
//
//   * a station's logo: Radio Browser is asked for the station (by its uuid
//     when it came from the directory, else by its address, else by its exact
//     name) and its "favicon" is fetched;
//   * the song that plays on a station ("Artist - Title"), and a file without
//     a picture (by its tags: artist and album, else artist and title): the
//     iTunes search is asked, its "artworkUrl100" is fetched.
//
// Every picture from the internet comes through the wsrv.nl image proxy, as a
// small baseline JPEG of 80 x 80 pixels (whatever it was: PNG, ICO, SVG, a
// progressive JPEG; transparent parts on the player's navy), over plain HTTP:
// the ESP-01's one TLS connection stays the station's. Nothing waits for it:
// the station plays at once, a picture comes when it is there.
//
// What shows: a file's own picture, else its online cover; a station's song
// cover, else its logo; else one of the default pictures, at random (three in
// the engine, and any JPEG in PICOAMP/DEFAULTS on the card). Each is decoded
// (TJpgDec, baseline JPEG, made small while decoding) and converted to SCREEN 4
// cells in small steps in the main loop (pva_cover.c), so that the music does
// not stop for it.
//
// Where the engine may write the card (the PicoAmp ROM), what was found online
// is kept there (PICOAMP/LOGOS and PICOAMP/COVERS, 1.6 KB a picture; also "there
// is none"), so it is not asked again. [cover] online=off in PICOAMP.INI turns
// the online part off, [cover] defaults=off the default pictures.
//
// Memory: about 49 KB of the arena when it has it (the PicoAmp ROM); else the
// stream ring, which no stream uses while a file plays (then: a file's own
// picture and the default pictures, while a file plays).
//
// The player says once which palette it uses and which entries the cover may
// set (COVER_SETUP); COVER_GET gives the result. PVA_ST_COVER_GEN goes +1 when
// the cover changes (also to none), PVA_ST_COVER_STATE says what there is.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "pva_internal.h"
#include "pva_cover.h"
#include "tjpgd/tjpgd.h"

#define KEY_MAX     260
#define IMG_MAX     (5u * 1024u)    // a picture from the proxy (80 x 80, quality 80: 2-4 KB)
#define JOB_MS      20000u          // a lookup and its picture, at most

static pva_cover_t *cv;
static uint8_t     *work;           // TJpgDec's
static uint8_t     *out;            // PVA_COVER_FULL
static FIL         *f;
static uint8_t     *img;            // IMG_MAX (online only)
static uint8_t     *logo_keep;      // PVA_COVER_FULL: the station's logo, converted (online only)
static uint32_t     f_left;         // bytes of the picture still to read
static const uint8_t *m_src;        // (or from memory)
static size_t       m_len, m_pos;
static uint8_t      pal[32];
static uint16_t     free_mask;      // (bit 0: the player sets a palette for every row of cells)
static bool         set_up;
static uint8_t      state = PVA_COVER_NONE;
static uint8_t      gen;

#define WORK_SIZE   (TJPGD_WORKSPACE_SIZE + 64)
#define A4(n)       (((n) + 3u) & ~3u)
#define MEM_SIZE    (A4(sizeof(pva_cover_t)) + A4(WORK_SIZE) + A4(PVA_COVER_FULL) + A4(sizeof(FIL)) + 16u)
#define JOB_ROOM    436u
#define LQ_SIZE     448u
#define MEM_ONLINE  (MEM_SIZE + IMG_MAX + A4(PVA_COVER_FULL) + 2u * JOB_ROOM + LQ_SIZE + HEAD)    // (+ the jobs, the request)

static bool    on;                  // covers are made
static uint8_t *own;                // the arena's, when it gave it (then also online)

// what shows, in order: a file's own picture, a song's or a file's online cover, a logo, a default
enum { P_NONE, P_DEFAULT, P_LOGO, P_ONLINE, P_LOCAL };
static uint8_t shown;               // what out has (or is being made into)
static uint8_t making;              // what the conversion makes
static uint8_t *conv_to;            // where: out, or logo_keep (a logo made while a song's cover shows)
static char    conv_path[40];       // its copy on the card ("": none)
static bool    logo_ok;             // logo_keep has the station's logo
static bool    lent;                // the pictures have the memory
static char    key[KEY_MAX];        // a file's picture: where it came from (an album's folder image once)

// where the pictures come from (for a bigger picture of what shows): a file's or a default's; the
// addresses of the song's cover and the station's logo (the originals, before the proxy)
// (they are at the start of the arena's block, which stays when the pictures borrow the rest: the
// firmware's RAM outside the arena is short; without the arena's block, none)
#define URL_MAX     240
typedef struct
{
    char path[KEY_MAX];
    char song[URL_MAX], logo[URL_MAX];
} head_t;
#define HEAD        A4(sizeof(head_t))
static head_t  *hd;
static uint8_t  src_kind;           // PVA_SRC_FILE, PVA_SRC_MEM (src_pos: the default)
static uint32_t src_pos, src_len;
static char     no_url[1];          // (no head: nowhere to keep it)
#define src_path    (hd ? hd->path : no_url)
#define url_song    (hd ? hd->song : no_url)
#define url_logo    (hd ? hd->logo : no_url)

static void src_set(uint8_t kind, const char *path, uint32_t pos, uint32_t len)
{
    src_kind = kind;
    if (hd) snprintf(hd->path, sizeof hd->path, "%s", path ? path : "");
    src_pos = pos;
    src_len = len;
}

// a picture's address for a bigger one: iTunes' 100 x 100 cover as 600 x 600
static void url_keep(char *d, const char *u, bool song)
{
    if (d == no_url) return;
    snprintf(d, URL_MAX, "%s", u);
    char *p = song ? strstr(d, "/100x100") : NULL;
    if (p) memcpy(p + 1, "600x600", 7);
}

// what plays
enum { I_NONE, I_FILE, I_STREAM };
static uint8_t  item;
static uint32_t rnd = 0x2545F491u;
static uint8_t  dflt_pick;          // the default picture of this item

static void online_place(uint8_t *m);
static void place(uint8_t *m)
{
    if (m == own) { hd = (head_t *)m; m += HEAD; }
    else hd = NULL;
    cv = (pva_cover_t *)m;
    m += A4(sizeof(pva_cover_t));
    work = m;
    m += A4(WORK_SIZE);
    out = m;
    m += A4(PVA_COVER_FULL);
    f = (FIL *)m;
    m += A4(sizeof(FIL));
    if (own)
    {
        img = m;
        m += IMG_MAX;
        logo_keep = m;
        m += A4(PVA_COVER_FULL);
        online_place(m);
    }
}

// the memory now (an item starts): false none
static bool mem(void)
{
    uint8_t *m = own ? own : pva_net_ring_idle(MEM_SIZE);
    if (!m) { cv = NULL; return false; }
    place(m);
    return true;
}

bool pva_art_init(void)
{
    // (the last to take from the arena: nothing comes after it. The PicoAmp ROM's arena has about
    // 46.9 KB left here (184 KB less the HAL's heap and the microSD's buffers, the engine's own),
    // the Nextor modes' a few hundred bytes. Online takes 45.5 KB.)
    if (pva_arena_free() >= MEM_ONLINE + 512u && (own = pva_alloc(MEM_ONLINE))) on = true;
    else if (MEM_ONLINE <= PVA_ART_KEEP && (own = pva_net_art_block(MEM_ONLINE))) on = true;
    if (!own) pva_log("covers: %u bytes free, online needs %u", (unsigned)pva_arena_free(), (unsigned)(MEM_ONLINE + 512u));
    else if (pva_net_ring_idle(MEM_SIZE)) on = true;
    if (own) { place(own); memset(hd, 0, sizeof *hd); }
    rnd ^= pva_hal_millis() * 2654435761u;
    return on;
}

bool pva_art_online(void) { return own != NULL; }
uint8_t pva_art_state(void) { return state; }
uint8_t pva_art_gen(void) { return gen; }

static void set_state(uint8_t s)
{
    if (s != state || s != PVA_COVER_MAKING) gen++;
    state = s;
}

static bool cfg_off(const char *k)
{
    char v[8];
    return pva_cfg_get(k, v, sizeof v) && (v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F');
}

static uint32_t fnv(const void *p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t *)p)[i]; h *= 16777619u; }
    return h;
}

// ---------------------------------------------------------------- decoding
static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    (void)jd;
    if (m_src)
    {
        if (n > m_len - m_pos) n = m_len - m_pos;
        if (buf) memcpy(buf, m_src + m_pos, n);
        m_pos += n;
        return n;
    }
    if (n > f_left) n = f_left;
    if (!buf)
    {
        if (f_lseek(f, f_tell(f) + n) != FR_OK) return 0;
        f_left -= (uint32_t)n;
        return n;
    }
    UINT br = 0;
    if (f_read(f, buf, (UINT)n, &br) != FR_OK) return 0;
    f_left -= br;
    return br;
}

static int jpg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    (void)jd;
    pva_cover_block(cv, r->left, r->top, r->right - r->left + 1, r->bottom - r->top + 1, bitmap);
    return 1;
}

// the JPEG (from m_src, or f at its place with f_left bytes) into cv->img
static void show_none(void);
static bool jpeg(const char *what)
{
    // (cv's picture is replaced: a cover in the making from it is no more)
    if (conv_to == logo_keep && logo_keep) conv_to = NULL;
    else if (state == PVA_COVER_MAKING) show_none();
    JDEC jd;
    JRESULT r = jd_prepare(&jd, jpg_in, work, WORK_SIZE, NULL);
    if (r == JDR_OK)
    {
        uint8_t s = 0;              // made small while decoding: 1/2, 1/4, 1/8 as long as 80 pixels are left
        unsigned m = jd.width < jd.height ? jd.width : jd.height;
        while (s < 3 && (m >> (s + 1)) >= PVA_COVER_PX) s++;
        pva_cover_begin(cv, jd.width >> s, jd.height >> s);
        r = jd_decomp(&jd, jpg_out, s);
        if (r == JDR_OK) { pva_cover_end(cv); pva_log("cover: %ux%u, %.40s", jd.width, jd.height, what); return true; }
    }
    pva_log("cover: %.40s: not a JPEG it can read (%d)", what, (int)r);
    return false;
}

// the picture at pos (len bytes; 0: all) of a file
static bool decode_file(const char *path, uint32_t pos, uint32_t len)
{
    m_src = NULL;
    if (f_open(f, path, FA_READ) != FR_OK) return false;
    bool ok = false;
    if (!len) len = (uint32_t)f_size(f);
    if (f_lseek(f, pos) == FR_OK) { f_left = len; ok = jpeg(path); }
    f_close(f);
    return ok;
}

static bool decode_mem(const uint8_t *p, size_t n, const char *what)
{
    m_src = p; m_len = n; m_pos = 0;
    bool ok = jpeg(what);
    m_src = NULL;
    return ok;
}

// an image in the folder of the track
static bool folder_image(const char *track, char *im, size_t size)
{
    static const char *const names[] = { "cover", "folder", "front", "album", "albumart", "AlbumArtSmall" };
    static const char *const ext[] = { ".jpg", ".jpeg" };
    const char *sl = strrchr(track, '/');
    int dl = sl ? (int)(sl - track) : 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        for (unsigned e = 0; e < 2; e++)
        {
            snprintf(im, size, "%.*s/%s%s", dl, track, names[i], ext[e]);
            FILINFO fi;
            if (f_stat(im, &fi) == FR_OK && !(fi.fattrib & AM_DIR)) return true;
        }
    return false;
}

// a palette for every row of cells: the player asks for it, [cover] rows is not off
static bool rows_on(void) { return (free_mask & 1) && !cfg_off("cover.rows"); }

// cv->img (a picture of kind p) becomes the cover, in steps; to out (shown), or
// to logo_keep (a logo kept for later); path: its copy on the card when made
static void convert(uint8_t p, uint8_t *to, const char *path)
{
    if (!set_up) return;
    making = p;
    conv_to = to;
    snprintf(conv_path, sizeof conv_path, "%s", path ? path : "");
    if (rows_on()) pva_cover_convert_begin_rows(cv, pal, free_mask, to);
    else { pva_cover_convert_begin(cv, pal, free_mask, to); memset(to + PVA_COVER_SIZE, 0, PVA_COVER_ROWS_SIZE); }
    if (to == out) { shown = p; set_state(PVA_COVER_MAKING); }
}

static void show_none(void)
{
    shown = making = P_NONE;
    if (conv_to == out) conv_to = NULL;
    if (state != PVA_COVER_NONE) set_state(PVA_COVER_NONE);
}

// ---------------------------------------------------------------- the default pictures
#define DEF_DIR "0:/PICOAMP/DEFAULTS"

static bool is_jpg(const char *n)
{
    const char *d = strrchr(n, '.');
    if (!d) return false;
    char e[6] = { 0 };
    for (int i = 0; i < 5 && d[i + 1]; i++) e[i] = (char)(d[i + 1] | 0x20);
    return !strcmp(e, "jpg") || !strcmp(e, "jpeg");
}

// the card's: how many (and the k-th's path)
static unsigned card_defaults(unsigned k, char *path, size_t size)
{
    DIR d;
    FILINFO fi;
    unsigned n = 0;
    if (f_opendir(&d, DEF_DIR) != FR_OK) return 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
    {
        if ((fi.fattrib & AM_DIR) || !is_jpg(fi.fname)) continue;
        if (path && n == k) { snprintf(path, size, DEF_DIR "/%s", fi.fname); n++; break; }
        n++;
    }
    f_closedir(&d);
    return n;
}

static void show_default(void)
{
    if (!set_up || cfg_off("cover.defaults")) { show_none(); return; }
    unsigned nc = card_defaults(0, NULL, 0), n = PVA_ART_DEFAULTS + nc;
    unsigned k = dflt_pick % n;
    bool ok;
    if (k < PVA_ART_DEFAULTS)
    {
        ok = decode_mem(pva_art_default[k], pva_art_default_len[k], "a default picture");
        src_set(PVA_SRC_MEM, NULL, k, 0);
    }
    else
    {
        char p[KEY_MAX];
        ok = card_defaults(k - PVA_ART_DEFAULTS, p, sizeof p) > k - PVA_ART_DEFAULTS && decode_file(p, 0, 0);
        src_set(PVA_SRC_FILE, p, 0, 0);
    }
    if (ok) convert(P_DEFAULT, out, NULL);
    else show_none();
}

// ---------------------------------------------------------------- the card's copies
#define LGO_HEAD    12u             // "PVLG", version, kind, 0, 0, u32 the palette's hash; the cover, the picture's address
#define LGO_VER     2

static uint32_t pal_hash(void)
{
    uint16_t m = (uint16_t)(free_mask & ~1u) | (rows_on() ? 1u : 0u);
    return fnv(&m, 2, fnv(pal, 32, 2166136261u));
}

static void card_path(char *p, size_t n, bool logo, const char *k)
{
    snprintf(p, n, "0:/PICOAMP/%s/%08lX.LGO", logo ? "LOGOS" : "COVERS", (unsigned long)fnv(k, strlen(k), 2166136261u));
}

// what is known of a picture (none: from NULL, or the cover in from)
static void card_write(const char *path, const uint8_t *from, const char *url)
{
    bool have = from != NULL;
    if (!pva_card_writes()) return;
    f_mkdir("0:/PICOAMP");
    f_mkdir("0:/PICOAMP/LOGOS");
    f_mkdir("0:/PICOAMP/COVERS");
    if (f_open(f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    uint8_t h[LGO_HEAD] = { 'P', 'V', 'L', 'G', LGO_VER, have ? PVA_COVER_READY : PVA_COVER_NONE, 0, 0 };
    uint32_t ph = pal_hash();
    memcpy(h + 8, &ph, 4);
    UINT bw;
    bool ok = f_write(f, h, LGO_HEAD, &bw) == FR_OK && bw == LGO_HEAD;
    if (ok && have) ok = f_write(f, from, PVA_COVER_FULL, &bw) == FR_OK && bw == PVA_COVER_FULL;
    if (ok && have)
    {
        char u[URL_MAX];
        memset(u, 0, sizeof u);
        snprintf(u, sizeof u, "%s", url ? url : "");
        ok = f_write(f, u, (UINT)strlen(u) + 1u, &bw) == FR_OK;
    }
    f_close(f);
    if (!ok) f_unlink(path);
    else pva_log("cover: kept %s (%s)", path + 11, have ? "picture" : "none");
}

// 1: the card has it (into out; its address into url), 0: it knows there is none, -1: it knows nothing
// (or for another palette)
static int card_read(const char *path, char *url)
{
    if (f_open(f, path, FA_READ) != FR_OK) return -1;
    uint8_t h[LGO_HEAD];
    UINT br = 0;
    int r = -1;
    if (f_read(f, h, LGO_HEAD, &br) == FR_OK && br == LGO_HEAD && !memcmp(h, "PVLG", 4) && h[4] == LGO_VER)
    {
        uint32_t ph;
        memcpy(&ph, h + 8, 4);
        if (h[5] != PVA_COVER_READY) r = 0;
        else if (ph == pal_hash() && f_read(f, out, PVA_COVER_FULL, &br) == FR_OK && br == PVA_COVER_FULL)
        {
            r = 1;
            if (url != no_url)
            {
                url[0] = 0;
                if (f_read(f, url, URL_MAX - 1, &br) == FR_OK) url[br < URL_MAX ? br : URL_MAX - 1] = 0;
                url[URL_MAX - 1] = 0;
            }
        }
    }
    f_close(f);
    return r;
}

// ---------------------------------------------------------------- online
// a job: a question (Radio Browser or iTunes), the picture it names (through the proxy)
enum { J_LOGO, J_SONG };
enum { L_IDLE, L_ASK, L_LOOKUP, L_FETCH };
#define RB_HOSTS    3
static const char *const rb_host[RB_HOSTS] = { "all.api.radio-browser.info", "de1.api.radio-browser.info", "nl1.api.radio-browser.info" };
#define PROXY       "http://wsrv.nl/?w=80&h=80&fit=contain&cbg=000049&bg=000049&output=jpg&q=80&url="

typedef struct
{
    uint8_t kind;                   // J_LOGO, J_SONG
    char    path[40];               // its copy on the card
    char    url[160], uuid[40], name[56];   // a station
    char    term[136];              // a song or an album ("artist title")
    bool    album;
} job_t;

static job_t  *jq;                  // [2]: [0] runs, [1] waits (in the arena's block)
static uint8_t jn;
static uint8_t l_state;
static uint8_t l_way, l_host;       // the question asked (uuid, address, name / song), the server
static uint32_t l_t0;
static bool    l_inflight;          // our request is with the web client (until its done callback)
static bool    l_answered;          // a server answered the question (no picture: kept as such)
static char   *l_q;                 // LQ_SIZE: the next request (also the picture's address, as it is read)
static char    l_key[16];           // the JSON key whose value is the picture's address
static uint8_t k_st;                // reading it: 0 looking for the key, 1 ':', 2 '"', 3 the value, 4 done
static char    roll[20];
static uint32_t img_len;
static bool    img_bad;

static void online_place(uint8_t *m)
{
    jq = (job_t *)m;
    l_q = (char *)(m + 2u * JOB_ROOM);
}
_Static_assert(A4(sizeof(job_t)) <= JOB_ROOM, "the jobs' room");

static void enc(char *o, size_t size, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k = 0;
    for (; *s && k + 4 < size; s++)
    {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o[k++] = (char)c;
        else { o[k++] = '%'; o[k++] = hex[c >> 4]; o[k++] = hex[c & 15]; }
    }
    o[k] = 0;
}

static void job_drop_all(void)
{
    if (l_inflight) pva_web_abort();    // (its connection closed: the ESP is the station's)
    l_state = L_IDLE;
    jn = 0;
}

static void job_next(void)
{
    l_state = L_IDLE;
    if (jn > 1) { jq[0] = jq[1]; jn = 1; }
    else jn = 0;
}

// the question for the way l_way at server l_host (false: no more ways)
static bool question(void)
{
    job_t *j = &jq[0];
    char e[300];
    if (j->kind == J_LOGO)
    {
        for (; l_way < 3; l_way++)
        {
            const char *h = rb_host[l_host];
            if (l_way == 0 && j->uuid[0]) { snprintf(l_q, LQ_SIZE, "http://%s/json/stations/byuuid/%s", h, j->uuid); return true; }
            if (l_way == 1) { enc(e, sizeof e, j->url); snprintf(l_q, LQ_SIZE, "http://%s/json/stations/byurl?url=%s", h, e); return true; }
            if (l_way == 2 && j->name[0])
            {
                enc(e, sizeof e, j->name);
                snprintf(l_q, LQ_SIZE, "http://%s/json/stations/search?name=%s&nameExact=true&limit=1&order=votes&reverse=true&hidebroken=true", h, e);
                return true;
            }
        }
        return false;
    }
    if (l_way) return false;
    enc(e, sizeof e, j->term);
    snprintf(l_q, LQ_SIZE, "http://itunes.apple.com/search?term=%s&media=music&entity=%s&limit=1", e, j->album ? "album" : "song");
    return true;
}

static void job_start(void)
{
    job_t *j = &jq[0];
    snprintf(l_key, sizeof l_key, "%s", j->kind == J_LOGO ? "favicon" : "artworkUrl100");
    l_way = 0;
    l_host = 0;
    l_answered = false;
    l_t0 = pva_hal_millis();
    if (!question()) { job_next(); return; }
    l_state = L_ASK;
    pva_log("cover: asking %s for %.36s", j->kind == J_LOGO ? "Radio Browser" : "iTunes", j->kind == J_LOGO ? j->url : j->term);
}

static void job_add(const job_t *j)
{
    if (jn == 2) jq[1] = *j;            // (a newer song instead of the waiting one)
    else jq[jn++] = *j;
    if (jn == 1) job_start();
}

// the answer: the value of l_key ("\/" in JSON: "/")
static bool lk_body(const uint8_t *p, size_t n)
{
    if (l_state != L_LOOKUP) return true;
    size_t kl = strlen(l_key);
    for (size_t i = 0; i < n && k_st < 4; i++)
    {
        char c = (char)p[i];
        switch (k_st)
        {
            case 0:
                memmove(roll, roll + 1, sizeof roll - 2);
                roll[sizeof roll - 2] = c;
                roll[sizeof roll - 1] = 0;
                if (c == '"' && roll[sizeof roll - 3 - kl] == '"' && !memcmp(roll + sizeof roll - 2 - kl, l_key, kl)) k_st = 1;
                break;
            case 1: if (c == ':') k_st = 2; else if (c != ' ') k_st = 0; break;
            case 2: if (c == '"') { k_st = 3; l_q[0] = 0; } else if (c != ' ') k_st = 0; break;
            default:
                if (c == '"') k_st = 4;
                else if (c != '\\')
                {
                    size_t k = strlen(l_q);
                    if (k + 1 < 236) { l_q[k] = c; l_q[k + 1] = 0; }
                }
                break;
        }
    }
    return k_st == 4;
}

static void lk_done(bool ok)
{
    l_inflight = false;
    if (l_state != L_LOOKUP) return;
    if (k_st == 4 && (!strncmp(l_q, "http://", 7) || !strncmp(l_q, "https://", 8)))
    {
        // the picture, through the proxy (its address kept, for a bigger one)
        url_keep(jq[0].kind == J_LOGO ? url_logo : url_song, l_q, jq[0].kind != J_LOGO);
        char e[LQ_SIZE - sizeof PROXY];
        enc(e, sizeof e, l_q);
        snprintf(l_q, LQ_SIZE, PROXY "%s", e);
        img_len = 0;
        img_bad = false;
        l_state = L_ASK;
        l_way = 0xFF;                   // (the next request is the picture)
        return;
    }
    if (ok) l_answered = true;
    if (!ok && jq[0].kind == J_LOGO && l_host + 1 < RB_HOSTS) l_host++;   // (that server: the next one, the same question)
    else l_way++;
    if (question()) { l_state = L_ASK; return; }
    if (l_answered) card_write(jq[0].path, NULL, NULL);
    pva_log("cover: none online");
    job_next();
}

static bool img_body(const uint8_t *p, size_t n)
{
    if (l_state != L_FETCH) return true;
    if (img_len + n > IMG_MAX) { img_bad = true; return true; }
    memcpy(img + img_len, p, n);
    img_len += (uint32_t)n;
    return false;
}

static void img_done(bool ok)
{
    l_inflight = false;
    if (l_state != L_FETCH) return;
    job_t j = jq[0];
    job_next();
    if (!ok || img_bad || img_len < 4 || img[0] != 0xFF || img[1] != 0xD8)
    {
        pva_log("cover: the picture did not come (%u bytes)", (unsigned)img_len);
        uint16_t http = pva_web_http();
        if ((ok && !img_bad) || (http >= 400 && http < 500)) card_write(j.path, NULL, NULL);    // (the proxy answered: no picture there)
        return;
    }
    // the picture arrived for the item that plays: shown if nothing better shows (a logo
    // behind a song's cover is made for later)
    uint8_t p = j.kind == J_LOGO ? P_LOGO : P_ONLINE;
    bool behind = shown > p && state != PVA_COVER_NONE;
    if (behind && j.kind != J_LOGO) return;
    if (state == PVA_COVER_MAKING && behind) return;    // (cv is busy for what shows: the logo next time)
    if (decode_mem(img, img_len, j.kind == J_LOGO ? "a logo" : "a cover")) convert(p, behind ? logo_keep : out, j.path);
}

static void job_poll(void)
{
    if (l_state == L_IDLE) { if (jn) job_start(); return; }
    if (pva_hal_millis() - l_t0 > JOB_MS)
    {
        pva_log("cover: too slow, given up");
        if (l_inflight) pva_web_abort();
        job_next();
        return;
    }
    if (l_state == L_ASK && !l_inflight)
    {
        bool im = l_way == 0xFF;
        pva_web_no_lend();              // (never the station's TLS connection: the music would stop)
        if (pva_web_get(l_q, im ? img_body : lk_body, im ? img_done : lk_done) == PVA_OK)
        {
            l_inflight = true;
            l_state = im ? L_FETCH : L_LOOKUP;
            k_st = 0;
            memset(roll, 0, sizeof roll);
        }
        else if (!pva_web_busy()) job_drop_all();       // (no WiFi)
    }
}

// ---------------------------------------------------------------- what plays
static bool online_ok(void) { return own && set_up && !cfg_off("cover.online") && pva_hal_net_present(); }

static void lower(char *d, size_t n, const char *s)
{
    size_t k = 0;
    for (; *s && k + 1 < n; s++) d[k++] = (*s >= 'A' && *s <= 'Z') ? (char)(*s + 32) : *s;
    d[k] = 0;
}

// a song or an album: from the card, else asked (false: nothing yet)
static bool online_cover(const char *artist, const char *what, bool album)
{
    if (!online_ok() || !artist[0] || !what[0]) return false;
    job_t j;
    memset(&j, 0, sizeof j);
    j.kind = J_SONG;
    j.album = album;
    snprintf(j.term, sizeof j.term, "%s %s", artist, what);
    char k[200];
    lower(k, sizeof k, j.term);
    card_path(j.path, sizeof j.path, false, k);
    int c = card_read(j.path, url_song);
    if (c == 1) { shown = making = P_ONLINE; set_state(PVA_COVER_READY); return true; }
    if (c == 0) return false;
    // (a song asked for while another is asked: the newer one waits, the older one goes)
    if (jn && jq[0].kind == J_SONG && l_state != L_IDLE) { if (l_inflight) pva_web_abort(); job_next(); }
    job_add(&j);
    return false;
}

// back to the station's logo (or a default picture)
static void station_fallback(void)
{
    if (logo_ok) { memcpy(out, logo_keep, PVA_COVER_FULL); shown = making = P_LOGO; set_state(PVA_COVER_READY); }
    else show_default();
}

// a file starts (fatfs path; its tag's picture: pos, len, len 0 = none; its tags)
void pva_art_track(const char *path, uint32_t pic_pos, uint32_t pic_len, const char *artist, const char *album, const char *title)
{
    if (lent) return;                   // (the pictures have the memory)
    if (!on) return;
    job_drop_all();
    logo_ok = false;
    bool again = item == I_FILE;
    item = I_FILE;
    if (!mem()) { key[0] = 0; show_none(); return; }
    char k[KEY_MAX], im[KEY_MAX];
    const char *src = NULL;
    uint32_t pos = 0, len = 0;
    if (pic_len)
    {
        snprintf(k, sizeof k, "%s#%lu", path, (unsigned long)pic_pos);
        src = path; pos = pic_pos; len = pic_len;
    }
    else if (folder_image(path, im, sizeof im))
    {
        snprintf(k, sizeof k, "%s", im);
        src = im;
    }
    else k[0] = 0;
    if (again && k[0] && !strcmp(k, key) && shown == P_LOCAL) return;     // the same picture (an album's folder image)
    snprintf(key, sizeof key, "%s", k);
    dflt_pick = (uint8_t)((rnd = rnd * 1103515245u + 12345u) >> 16);
    if (src && decode_file(src, pos, len)) { src_set(PVA_SRC_FILE, src, pos, len); convert(P_LOCAL, out, NULL); return; }
    key[0] = 0;
    // online: by the album (a file of an album), else by the title
    if (online_cover(artist, album[0] ? album : title, album[0] != 0)) return;
    show_default();
}

// a station starts (its address, its directory uuid or NULL, its name)
void pva_art_station(const char *url, const char *uuid, const char *name)
{
    if (lent) return;                   // (the pictures have the memory)
    if (!on) return;
    job_drop_all();
    item = I_STREAM;
    key[0] = 0;
    logo_ok = false;
    dflt_pick = (uint8_t)((rnd = rnd * 1103515245u + 12345u) >> 16);
    if (!mem()) { show_none(); return; }        // (the ring is the stream's: no cover without own memory)
    if (!online_ok()) { show_default(); return; }
    job_t j;
    memset(&j, 0, sizeof j);
    j.kind = J_LOGO;
    snprintf(j.url, sizeof j.url, "%s", url);
    snprintf(j.uuid, sizeof j.uuid, "%s", uuid ? uuid : "");
    snprintf(j.name, sizeof j.name, "%s", name ? name : "");
    card_path(j.path, sizeof j.path, true, url);
    int c = card_read(j.path, url_logo);
    if (c == 1) { memcpy(logo_keep, out, PVA_COVER_FULL); logo_ok = true; shown = making = P_LOGO; set_state(PVA_COVER_READY); return; }
    show_default();
    if (c == -1) job_add(&j);
}

// the song on a station changes ("" artist: not a song it can look for)
void pva_art_song(const char *artist, const char *title)
{
    if (lent) return;                   // (the pictures have the memory)
    if (!on || item != I_STREAM || !cv) return;
    // (the logo's job stays; a song's job waiting or running goes)
    if (jn && jq[jn - 1].kind == J_SONG)
    {
        if (jn == 1 && l_state != L_IDLE && l_inflight) pva_web_abort();
        jn--;
        if (!jn) l_state = L_IDLE;
    }
    if (online_cover(artist, title, false)) return;
    if (shown == P_ONLINE || shown == P_NONE) station_fallback();
}

// what shows, for a bigger picture of it
bool pva_art_source(pva_art_src_t *s)
{
    memset(s, 0, sizeof *s);
    if (state == PVA_COVER_NONE || lent) return false;
    switch (shown)
    {
        case P_LOCAL: case P_DEFAULT:
            s->kind = src_kind;
            if (src_kind == PVA_SRC_MEM)
            {
                if (src_pos >= PVA_ART_DEFAULTS) return false;
                s->p = pva_art_default_big[src_pos];
                s->len = pva_art_default_big_len[src_pos];
                s->pos = src_pos;
                s->s = "";
                return true;
            }
            s->s = src_path;
            s->pos = src_pos;
            s->len = src_len;
            return src_path[0] != 0;
        case P_ONLINE: case P_LOGO:
            s->kind = PVA_SRC_URL;
            s->s = shown == P_LOGO ? url_logo : url_song;
            return s->s[0] != 0;
    }
    return false;
}

// the pictures page: the covers' memory (covers stop meanwhile)
uint8_t *pva_art_borrow(size_t *n)
{
    if (lent) return NULL;
    job_drop_all();
    show_none();
    uint8_t *m = own;
    if (m) { m += HEAD; *n = MEM_ONLINE - HEAD; }       // (the head stays: where the covers came from)
    else if ((m = pva_net_ring_idle(MEM_SIZE))) *n = pva_net_capacity();
    if (!m) return NULL;
    cv = NULL;
    conv_to = NULL;
    lent = true;
    return m;
}

void pva_art_giveback(void)
{
    if (!lent) return;
    lent = false;
    item = I_NONE;
    key[0] = 0;
    if (own) place(own);
    pva_cover_redo();
}

// nothing plays: no cover
void pva_art_none(void)
{
    job_drop_all();
    item = I_NONE;
    logo_ok = false;
    key[0] = 0;
    if (!on) return;
    if (!own) cv = NULL;                // (the ring may be the stream's)
    show_none();
}

void pva_art_poll(void)
{
    if (lent) return;
    if (!on) return;
    job_poll();
    bool fg = state == PVA_COVER_MAKING && conv_to == out, bg = conv_to && conv_to == logo_keep;
    if ((fg || bg) && cv && pva_cover_convert_step(cv))
    {
        if (fg) set_state(PVA_COVER_READY);
        if (making == P_LOGO && logo_keep) { if (fg) memcpy(logo_keep, out, PVA_COVER_FULL); logo_ok = true; }
        if (conv_path[0]) card_write(conv_path, conv_to, making == P_LOGO ? url_logo : url_song);
        conv_to = NULL;
        conv_path[0] = 0;
    }
}

// ---------------------------------------------------------------- commands
int pva_art_cmd(uint8_t code, const uint8_t *args)
{
    if (!on) return PVA_E_STATE;
    if (code == PVA_C_COVER_SETUP)
    {
        uint16_t m = (uint16_t)(args[32] | args[33] << 8);
        bool changed = !set_up || memcmp(pal, args, 32) || m != free_mask;
        memcpy(pal, args, 32);
        free_mask = m;
        set_up = true;
        if (changed) { logo_ok = false; key[0] = 0; item = I_NONE; show_none(); }   // (from the next item on)
        return PVA_OK;
    }
    // PVA_C_COVER_GET
    if (state == PVA_COVER_MAKING) return PVA_E_BUSY;
    if (state != PVA_COVER_READY || !cv) return PVA_E_NOTFOUND;
    // (how many lines before a row the player starts to set its colours: [cover] rowearly, 0-7, as it is now)
    char v[8];
    int e = pva_cfg_get("cover.rowearly", v, sizeof v) ? atoi(v) : 2;
    out[PVA_COVER_SIZE + 1] = (uint8_t)(e < 0 ? 0 : e > 7 ? 7 : e);
    for (unsigned i = 0; i < PVA_COVER_FULL; i++) pva_reply_byte(out[i]);
    return PVA_OK;
}
