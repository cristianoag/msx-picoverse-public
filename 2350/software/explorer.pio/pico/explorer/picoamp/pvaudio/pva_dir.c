// MSX PICOVERSE PROJECT
// pva_dir.c - PicoVerse Audio Engine: the Radio Browser station directory.
//
// Radio Browser (radio-browser.info) is a free, community run directory of
// internet radio stations with a JSON API. This client fetches one list at a
// time over the ESP-01 (its own slot of the shared link, so a stream can keep
// playing), reads the JSON as it comes in - nothing of the size of a reply is
// ever held - and keeps what a list needs: names, the resolved stream
// addresses, the bitrate, the station id. MP3 stations only: the engine decodes
// MP3, and HLS streams are left out.
//
//   countries   /json/countries?hidebroken=true&order=name
//   stations    /json/stations/search?...&codec=MP3&hidebroken=true&order=clickcount&reverse=true
//   a click     /json/url/<stationuuid>   (asked for by Radio Browser when a station starts)
//
// The mirrors are tried in turn. HTTP first (TLS is one connection only on the
// ESP, and a station may be using it); a redirect to HTTPS is followed. A
// mirror that does not answer over HTTP is asked again over HTTPS before the
// next one (borrowing the station's TLS connection when it has it).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pva_internal.h"

#define HOST_MAX        63u
#define PATH_MAX_D      300u
#define URL_KEEP        255u
#define MAX_ENTRIES     300u
#define MAX_REDIRECTS   3u
#define RCV_SIZE        1024u
#define T_DNS_MS        15000u
#define T_OPEN_MS       20000u
#define T_SEND_MS       3000u
#define T_RCV_MS        2000u
#define T_CLOSE_MS      1000u
#define T_QUERY_MS      90000u      // a whole list (all the mirrors, HTTP and HTTPS)
#define T_SILENT_MS     8000u       // connected, and no answer this long
#define T_LEND_MS       6000u
#define IDLE_POLL_MS    4u
#define QUIET_MS        400u        // no data this long: ask the ESP whether the server closed
#define CACHE_SIZE      6144u       // the engine keeps the country list here

static const char *const mirrors[] = {
    "all.api.radio-browser.info", "de1.api.radio-browser.info", "de2.api.radio-browser.info",
    "fi1.api.radio-browser.info", "nl1.api.radio-browser.info", "at1.api.radio-browser.info",
};
#define N_MIRRORS (sizeof mirrors / sizeof mirrors[0])

// ---------------------------------------------------------------- results
typedef struct { uint16_t name, url, uuid, num; } dent_rb_t;   // offsets in pool (0xFFFF = none)
static dent_rb_t *ents;
static char     *pool;
static size_t    pool_size, pool_used;
static uint16_t  nents;
static bool      truncated;
// a list can be continued ("more stations"): a fetch then adds to what is there
static uint16_t  base_nents;
static size_t    base_pool;
static bool      q_append;
static uint16_t  nraw;              // objects in the reply (before the filters)

static void reset_results(void)
{
    nents = base_nents;
    pool_used = base_pool;
    truncated = false;
    nraw = 0;
}

static uint16_t pool_add(const char *s, size_t n)
{
    if (pool_used + n + 1 > pool_size) { truncated = true; return 0xFFFF; }
    uint16_t o = (uint16_t)pool_used;
    memcpy(pool + pool_used, s, n);
    pool[pool_used + n] = 0;
    pool_used += n + 1;
    return o;
}

uint16_t    pva_dir_count(void) { return nents; }
static const char *at(uint16_t o) { return o == 0xFFFF ? "" : pool + o; }
const char *pva_dir_name(uint16_t i) { return i < nents ? at(ents[i].name) : ""; }
const char *pva_dir_url(uint16_t i)  { return i < nents ? at(ents[i].url) : ""; }
const char *pva_dir_uuid(uint16_t i) { return i < nents ? at(ents[i].uuid) : ""; }
uint16_t    pva_dir_num(uint16_t i)  { return i < nents ? ents[i].num : 0; }

// ---------------------------------------------------------------- JSON
static bool     discard_body;       // a click: the reply does not matter
// The replies are a top-level array of flat objects. Only fields of those
// objects (depth 2) are looked at; anything deeper is skipped.
static uint8_t  kind;               // PVA_DIRQ_xxx of the query
static int      depth;
static bool     in_str, esc, expect_key, is_key, have_val;
static bool     json_seen, json_done;   // the top-level value began / ended (the reply is complete)
static uint8_t  uhex;
static uint32_t ucp;
static char     key[20];
static uint8_t  klen;
static char    *val;                // URL_KEEP + 1
static uint16_t vlen;
// fields of the object being read
static char     f_name[96], f_codec[12], f_uuid[40];
static char    *f_url;              // URL_KEEP + 1
static uint32_t f_bitrate, f_count;
static bool     f_hls, f_bad, f_url_long;

static void obj_start(void)
{
    f_name[0] = f_codec[0] = f_uuid[0] = 0;
    f_url[0] = 0;
    f_bitrate = f_count = 0;
    f_hls = f_bad = f_url_long = false;
}

static bool eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
    {
        char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

static void obj_end(void)
{
    if (discard_body) return;
    nraw++;
    if (nents >= MAX_ENTRIES) { truncated = true; return; }
    if (!f_name[0]) return;
    dent_rb_t e = { 0xFFFF, 0xFFFF, 0xFFFF, 0 };
    if (kind == PVA_DIRQ_COUNTRIES)
    {
        if (!f_count) return;
        e.name = pool_add(f_name, strlen(f_name));
        e.num = (uint16_t)(f_count > 65535u ? 65535u : f_count);
    }
    else if (kind == PVA_DIRQ_STATIONS)
    {
        if (!eq_ci(f_codec, "MP3") || f_hls || f_bad || f_url_long || !f_url[0]) return;
        if (strncmp(f_url, "http://", 7) && strncmp(f_url, "https://", 8)) return;
        char nm[PVA_NAME_MAX + 1];
        const char *fn = f_name;
        while (*fn == ' ' || *fn == '\t' || *fn == '-' || *fn == '.') fn++;   // names padded to sort first
        pva_copy_ascii(nm, sizeof nm, fn, strlen(fn));
        if (!nm[0]) return;
        e.name = pool_add(nm, strlen(nm));
        e.url = pool_add(f_url, strlen(f_url));
        e.uuid = pool_add(f_uuid, strlen(f_uuid));
        e.num = (uint16_t)(f_bitrate > 65535u ? 0 : f_bitrate);
    }
    else return;
    if (e.name == 0xFFFF || (kind == PVA_DIRQ_STATIONS && e.url == 0xFFFF)) return;
    ents[nents++] = e;
}

static void field(bool str)
{
    val[vlen] = 0;
    if (discard_body) return;
    if (!strcmp(key, "name")) { size_t n = vlen < sizeof f_name - 1 ? vlen : sizeof f_name - 1; memcpy(f_name, val, n); f_name[n] = 0; }
    else if (!strcmp(key, "url_resolved")) { if (vlen >= URL_KEEP) f_url_long = true; else memcpy(f_url, val, (size_t)vlen + 1); }
    else if (!strcmp(key, "codec")) { size_t n = vlen < sizeof f_codec - 1 ? vlen : sizeof f_codec - 1; memcpy(f_codec, val, n); f_codec[n] = 0; }
    else if (!strcmp(key, "stationuuid")) { size_t n = vlen < sizeof f_uuid - 1 ? vlen : sizeof f_uuid - 1; memcpy(f_uuid, val, n); f_uuid[n] = 0; }
    else if (!strcmp(key, "bitrate")) f_bitrate = (uint32_t)atol(val);
    else if (!strcmp(key, "stationcount")) f_count = (uint32_t)atol(val);
    else if (!strcmp(key, "hls")) f_hls = str ? false : (val[0] == '1' || val[0] == 't');
    else if (!strcmp(key, "lastcheckok")) f_bad = !str && val[0] == '0';
}

static void put_val(char c)
{
    if (is_key) { if (klen < sizeof key - 1) key[klen++] = c; key[klen] = 0; }
    else if (vlen < URL_KEEP) val[vlen++] = c;
    else vlen = URL_KEEP;                       // too long: field() sees the cut
}

static void put_utf8(uint32_t cp)
{
    if (cp < 0x80) put_val((char)cp);
    else if (cp < 0x800) { put_val((char)(0xC0 | (cp >> 6))); put_val((char)(0x80 | (cp & 0x3F))); }
    else { put_val((char)(0xE0 | (cp >> 12))); put_val((char)(0x80 | ((cp >> 6) & 0x3F))); put_val((char)(0x80 | (cp & 0x3F))); }
}

static void json_reset(void)
{
    depth = 0;
    in_str = esc = have_val = json_seen = json_done = false;
    expect_key = true;
    uhex = 0;
}

static void json_byte(char c)
{
    if (in_str)
    {
        if (uhex)
        {
            int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
            ucp = (ucp << 4) | (uint32_t)h;
            if (--uhex == 0 && depth == 2) put_utf8(ucp);
            return;
        }
        if (esc)
        {
            esc = false;
            if (c == 'u') { uhex = 4; ucp = 0; return; }
            if (depth == 2) put_val(c == 'n' ? ' ' : c == 't' ? ' ' : c == 'r' ? ' ' : c == 'b' || c == 'f' ? ' ' : c);
            return;
        }
        if (c == '\\') { esc = true; return; }
        if (c == '"')
        {
            in_str = false;
            if (depth == 2)
            {
                if (is_key) { is_key = false; }
                else { field(true); have_val = false; }
            }
            return;
        }
        if (depth == 2) put_val(c);
        return;
    }
    switch (c)
    {
        case '"':
            in_str = true;
            if (depth == 2)
            {
                is_key = expect_key;
                if (is_key) { klen = 0; key[0] = 0; }
                else vlen = 0;
            }
            return;
        case '{':
            json_seen = true;
            depth++;
            if (depth == 2) { obj_start(); expect_key = true; have_val = false; }
            return;
        case '}':
            if (depth == 2)
            {
                if (have_val) { field(false); have_val = false; }
                obj_end();
            }
            if (depth > 0) depth--;
            if (!depth && json_seen) json_done = true;
            return;
        case '[':
            json_seen = true;
            depth++;
            return;
        case ']':
            if (depth > 0) depth--;
            if (!depth && json_seen) json_done = true;
            return;
        case ':':
            if (depth == 2) { expect_key = false; vlen = 0; }
            return;
        case ',':
            if (depth == 2)
            {
                if (have_val) { field(false); have_val = false; }
                expect_key = true;
            }
            return;
        case ' ': case '\t': case '\r': case '\n':
            return;
        default:
            // a number, true, false or null
            if (depth == 2 && !expect_key)
            {
                if (!have_val) { vlen = 0; have_val = true; }
                put_val(c);
            }
            return;
    }
}

// ---------------------------------------------------------------- HTTP client
enum { D_IDLE, D_DNS, D_OPEN, D_SEND, D_HEAD, D_BODY, D_CLOSE, D_DONE, D_FAILED, D_LEND };
enum { A_DONE, A_FAIL, A_NEXT_MIRROR, A_REDIRECT, A_TLS };
static bool d_lending, d_lent_once;

static pva_esp_slot_t *DS;
static uint8_t  d = D_IDLE;
static uint8_t  qstate = PVA_DIRS_IDLE, qerr;
static uint16_t qhttp;
static bool     held;               // this client holds the ESP link
static uint8_t  conn;
static uint8_t  mirror, redirects;
static uint8_t  after;
static bool     d_tls;
static uint16_t d_port;
static char     d_host[HOST_MAX + 1];
static char    *d_path;             // PATH_MAX_D + 1
static char    *req;                // 512
static size_t   req_len, req_pos;
static char     line[200];
static size_t   line_len;
static bool     status_seen;
static uint32_t content_len, body_got;
static char    *location;           // URL_KEEP + 1
static uint32_t t_query, wait_until, t_phase;
static bool     cancel_req;
static char     click_uuid[40];
static bool     click_pending;
static uint32_t state_asked;        // last TCP_STATE question
static bool     q_pending;          // a list waits for the running request to end
static uint8_t  q_kind;
static char    *q_path;             // PATH_MAX_D + 1
static uint8_t *cache;              // for the engine (a list to go back to)
static size_t   cache_size;

uint8_t pva_dir_conn(void) { return conn; }
uint8_t pva_dir_state(void) { return qstate; }
uint8_t pva_dir_error(void) { return qerr; }
uint16_t pva_dir_http(void) { return qhttp; }
bool    pva_dir_truncated(void) { return truncated; }
uint8_t pva_dir_kind(void) { return kind; }

static void d_fail(uint8_t err)
{
    if (!qerr) qerr = err;
}

static void d_close_then(uint8_t what)
{
    after = what;
    if (conn) pva_esp_request(DS, PVA_FN_TCPCLOSE, &conn, 1, NULL, 0, T_CLOSE_MS);
    else pva_esp_finish_now(DS, PVA_FN_TCPCLOSE);
    d = D_CLOSE;
}

static void d_release(void)
{
    if (d_lending) { pva_net_lend_tls(false); d_lending = false; }
    pva_esp_clear(DS);
    conn = 0;
    if (held) { pva_esp_release(); held = false; }
}

static void http_line(void)
{
    line[line_len] = 0;
    while (line_len && (line[line_len - 1] == '\r' || line[line_len - 1] == ' ')) line[--line_len] = 0;
    if (!status_seen)
    {
        status_seen = true;
        const char *sp = strchr(line, ' ');
        qhttp = sp ? (uint16_t)atoi(sp + 1) : 0;
        return;
    }
    char *colon = strchr(line, ':');
    if (!colon) return;
    *colon = 0;
    char *v = colon + 1;
    while (*v == ' ') v++;
    for (char *k = line; *k; k++) if (*k >= 'A' && *k <= 'Z') *k = (char)(*k + 32);
    if (!strcmp(line, "location")) snprintf(location, URL_KEEP + 1, "%s", v);
    else if (!strcmp(line, "content-length")) content_len = (uint32_t)atol(v);
    else if (!strcmp(line, "date")) pva_clock_http_date(v);
}

static void d_feed(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char c = (char)p[i];
        if (d == D_HEAD)
        {
            if (c == '\n')
            {
                if (line_len == 0 || (line_len == 1 && line[0] == '\r'))
                {
                    d = D_BODY;
                    body_got = 0;
                    json_reset();
                    continue;
                }
                http_line();
                line_len = 0;
            }
            else if (line_len < sizeof line - 1) line[line_len++] = c;
            continue;
        }
        if (d != D_BODY) return;
        body_got++;
        // a click's reply is only followed to its end
        if (qhttp == 200 && !json_done) json_byte(c);
    }
}

static bool parse_target(const char *u)
{
    const char *p;
    if (!strncmp(u, "http://", 7)) { d_tls = false; p = u + 7; d_port = 80; }
    else if (!strncmp(u, "https://", 8)) { d_tls = true; p = u + 8; d_port = 443; }
    else return false;
    size_t h = 0;
    while (*p && *p != '/' && *p != ':')
    {
        if (h >= HOST_MAX) return false;
        d_host[h++] = *p++;
    }
    d_host[h] = 0;
    if (*p == ':') { d_port = (uint16_t)atoi(p + 1); while (*p && *p != '/') p++; }
    snprintf(d_path, PATH_MAX_D + 1, "%s", *p ? p : "/");
    return h > 0;
}

static void d_connect(void)
{
    status_seen = false;
    state_asked = 0;
    json_reset();
    line_len = 0;
    location[0] = 0;
    content_len = 0;
    qhttp = 0;
    conn = 0;
    uint8_t f = 0;
    pva_esp_request(DS, PVA_FN_DNS, &f, 1, (const uint8_t *)d_host, strlen(d_host), T_DNS_MS);
    t_phase = pva_hal_millis();
    d = D_DNS;
}

static void d_start_mirror(void)
{
    d_lent_once = false;
    d_tls = false;
    d_port = 80;
    snprintf(d_host, sizeof d_host, "%s", mirrors[mirror]);
    d_connect();
}

static void send_next(void)
{
    size_t k = req_len - req_pos;
    if (k > 128u) k = 128u;
    uint8_t a[2] = { conn, 1 };
    pva_esp_request(DS, PVA_FN_TCPSEND, a, 2, (const uint8_t *)req + req_pos, k, T_SEND_MS);
    req_pos += k;
}

static void ask_rcv(void)
{
    uint8_t a[3] = { conn, (uint8_t)RCV_SIZE, (uint8_t)(RCV_SIZE >> 8) };
    pva_esp_request(DS, PVA_FN_TCPRCV, a, 3, NULL, 0, T_RCV_MS);
}

static void begin_query(const char *path, bool discard)
{
    snprintf(d_path, PATH_MAX_D + 1, "%s", path);
    discard_body = discard;
    cancel_req = false;
    mirror = 0;
    redirects = 0;
    qerr = 0;
    qhttp = 0;
    t_query = pva_hal_millis();
    d_start_mirror();
}

// What went wrong decides: another mirror, or give up.
static void d_trouble(uint8_t err)
{
    d_fail(err);
    pva_log("radio: %.36s failed (%u)", d_host, (unsigned)err);
    bool time_left = pva_hal_millis() - t_query < T_QUERY_MS;
    if (!d_tls && !discard_body && time_left && (err == PVA_NERR_TIMEOUT || err == PVA_NERR_CLOSED || err == PVA_NERR_CONNECT))
        d_close_then(A_TLS);            // the same mirror over HTTPS
    else if (mirror + 1u < N_MIRRORS && !discard_body && time_left)
        d_close_then(A_NEXT_MIRROR);
    else
        d_close_then(A_FAIL);
}

static void d_finish(void)
{
    qstate = PVA_DIRS_DONE;
    qerr = 0;
    pva_log("radio: %u entries%s, %lu KB in %lu ms", (unsigned)nents, truncated ? " (cut)" : "",
            (unsigned long)(body_got / 1024u), (unsigned long)(pva_hal_millis() - t_query));
}

void pva_dir_poll(void)
{
    if (d == D_IDLE)
    {
        if (q_pending)
        {
            q_pending = false;
            if (!held && !pva_esp_acquire()) { qerr = PVA_NERR_NOWIFI; qstate = PVA_DIRS_FAILED; return; }
            held = true;
            kind = q_kind;
            if (q_append) { base_nents = nents; base_pool = pool_used; }
            else { base_nents = 0; base_pool = 0; }
            reset_results();
            pva_log("radio: %.48s", q_path);
            begin_query(q_path, false);
            return;
        }
        // a click waits for a free moment
        if (click_pending)
        {
            click_pending = false;
            if (!held && !pva_esp_acquire()) return;
            held = true;
            char p[64];
            snprintf(p, sizeof p, "/json/url/%s", click_uuid);
            begin_query(p, true);
        }
        return;
    }
    if (d == D_DONE || d == D_FAILED) return;
    pva_esp_io();
    if (!pva_esp_finished(DS) && d != D_HEAD && d != D_BODY && d != D_LEND) return;
    uint32_t now = pva_hal_millis();
    if (cancel_req && d != D_CLOSE)
    {
        if (!pva_esp_finished(DS) && DS->want_fn) return;     // let the answer come in first
        if (d == D_OPEN && DS->rsp_done && DS->rc == 0 && DS->pgot) conn = DS->pfirst[0];
        pva_esp_clear(DS);
        cancel_req = false;
        discard_body = true;
        d_close_then(A_DONE);
        return;
    }
    switch (d)
    {
        case D_LEND:                    // the station closes its TLS connection, then this one opens
            if (pva_net_tls_free()) { pva_log("radio: TLS lent by the stream"); d_connect(); return; }
            if (now - t_phase > T_LEND_MS) d_trouble(PVA_NERR_CONNECT);
            return;
        case D_DNS:
            if (DS->rsp_timeout || DS->rc != 0 || DS->pgot < 4) { pva_esp_clear(DS); d_trouble(PVA_NERR_DNS); return; }
            {
                uint8_t a[11];
                memcpy(a, DS->pfirst, 4);
                a[4] = (uint8_t)d_port; a[5] = (uint8_t)(d_port >> 8);
                a[6] = 0xFF; a[7] = 0xFF; a[8] = 0; a[9] = 0;
                a[10] = d_tls ? 0x04 : 0x00;
                pva_esp_clear(DS);
                pva_esp_request(DS, PVA_FN_TCPOPEN, a, 11, d_tls ? (const uint8_t *)d_host : NULL, d_tls ? strlen(d_host) + 1 : 0, T_OPEN_MS);
                d = D_OPEN;
            }
            return;
        case D_OPEN:
            if (DS->rsp_timeout || DS->rc != 0 || DS->pgot < 1)
            {
                pva_log("radio: %s open failed (rc %u)", d_tls ? "TLS" : "TCP", (unsigned)DS->rc);
                if (d_tls && !DS->rsp_timeout && DS->rc == 9 && !d_lent_once && !pva_net_tls_free())
                {
                    // the ESP-01's one TLS connection is the station's: it lends it
                    pva_esp_clear(DS);
                    d_lent_once = true;
                    if (!d_lending) { d_lending = true; pva_net_lend_tls(true); }
                    t_phase = now;
                    d = D_LEND;
                    return;
                }
                pva_esp_clear(DS);
                d_trouble(PVA_NERR_CONNECT);
                return;
            }
            conn = DS->pfirst[0];
            pva_esp_clear(DS);
            {
                int n = snprintf(req, 512,
                    "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PicoPlay/0.3 (MSX PicoVerse 2350)\r\n"
                    "Accept: application/json\r\nConnection: close\r\n\r\n", d_path, d_host);
                req_len = (n > 0 && n < 512) ? (size_t)n : 0;
                req_pos = 0;
            }
            send_next();
            d = D_SEND;
            return;
        case D_SEND:
            if (DS->rsp_timeout || DS->rc != 0) { pva_esp_clear(DS); d_trouble(PVA_NERR_TIMEOUT); return; }
            pva_esp_clear(DS);
            if (req_pos < req_len) { send_next(); return; }
            d = D_HEAD;
            t_phase = now;
            wait_until = now;
            ask_rcv();
            return;
        case D_HEAD:
        case D_BODY:
            if (DS->want_fn == PVA_FN_TCPRCV)
            {
                if (!pva_esp_finished(DS)) return;
                bool timeout = DS->rsp_timeout;
                uint8_t r = DS->rc;
                uint32_t got = DS->rcv_data;
                pva_esp_clear(DS);
                if (timeout) { d_trouble(PVA_NERR_TIMEOUT); return; }
                // The reply is complete when the JSON value has ended, when all
                // the bytes Content-Length announced are in, or when the server
                // has closed. The ESP does not tell a close in TCP_RCV (it keeps
                // answering "no data"): TCP_STATE does, after a quiet moment.
                bool ended = d == D_BODY && (json_done || (content_len && body_got >= content_len) ||
                                             (qhttp >= 300 && qhttp < 400 && location[0]));
                if (r != 0 || ended)
                {
                    // the server closed (or sent all it announced): the reply is complete
                    if (d != D_BODY) { d_trouble(PVA_NERR_CLOSED); return; }
                    if (qhttp >= 300 && qhttp < 400 && location[0] && redirects < MAX_REDIRECTS)
                    {
                        redirects++;
                        d_close_then(A_REDIRECT);
                        return;
                    }
                    if (qhttp != 200) { d_fail(PVA_NERR_HTTP); d_trouble(PVA_NERR_HTTP); return; }
                    d_close_then(A_DONE);
                    return;
                }
                wait_until = got ? now : now + IDLE_POLL_MS;
                if (got) t_phase = now;
            }
            else if (DS->want_fn == PVA_FN_TCPSTATE)
            {
                if (!pva_esp_finished(DS)) return;
                bool timeout = DS->rsp_timeout;
                uint8_t r = DS->rc, cstate = DS->pfirst[1];
                uint16_t avail = (uint16_t)(DS->pfirst[2] | (DS->pfirst[3] << 8));
                bool ok = DS->pgot >= 4;
                pva_esp_clear(DS);
                state_asked = now;
                if (!timeout && (r != 0 || (ok && cstate != 4u && avail == 0)))
                {
                    // closed by the server, nothing left to read
                    if (d != D_BODY) { d_trouble(PVA_NERR_CLOSED); return; }
                    if (qhttp >= 300 && qhttp < 400 && location[0] && redirects < MAX_REDIRECTS) { redirects++; d_close_then(A_REDIRECT); return; }
                    if (qhttp != 200) { d_fail(PVA_NERR_HTTP); d_trouble(PVA_NERR_HTTP); return; }
                    d_close_then(A_DONE);
                    return;
                }
                wait_until = now;
            }
            if (!DS->want_fn && now - t_phase >= QUIET_MS && now - state_asked >= QUIET_MS)
            {
                uint8_t a = conn;
                pva_esp_request(DS, PVA_FN_TCPSTATE, &a, 1, NULL, 0, T_RCV_MS);
                return;
            }
            if (now - t_phase > T_SILENT_MS || now - t_query > T_QUERY_MS) { d_trouble(PVA_NERR_TIMEOUT); return; }
            if (!DS->want_fn && (int32_t)(now - wait_until) >= 0) ask_rcv();
            return;
        case D_CLOSE:
            pva_esp_clear(DS);
            conn = 0;
            if (cancel_req) { cancel_req = false; discard_body = true; after = A_DONE; reset_results(); }
            switch (after)
            {
                case A_NEXT_MIRROR:
                    if (d_lending) { pva_net_lend_tls(false); d_lending = false; }
                    mirror++;
                    reset_results();
                    d_start_mirror();
                    return;
                case A_TLS:
                    pva_log("radio: %.36s over HTTPS", d_host);
                    if (d_lending) { pva_net_lend_tls(false); d_lending = false; }
                    d_tls = true;
                    d_port = 443;
                    reset_results();
                    d_connect();
                    return;
                case A_REDIRECT:
                    if (!parse_target(location)) { d_fail(PVA_NERR_URL); d_close_then(A_FAIL); return; }
                    pva_log("radio: redirect to %.36s", location);
                    reset_results();
                    d_connect();
                    return;
                case A_FAIL:
                    if (d_lending) { pva_net_lend_tls(false); d_lending = false; }
                    d = D_IDLE;
                    if (!discard_body) { reset_results(); qstate = PVA_DIRS_FAILED; pva_log("radio: list failed (%u, HTTP %u)", (unsigned)qerr, (unsigned)qhttp); }
                    if (!q_pending && !click_pending) d_release();
                    return;
                default:
                    if (d_lending) { pva_net_lend_tls(false); d_lending = false; }
                    d = D_IDLE;
                    if (!discard_body) d_finish();
                    if (!q_pending && !click_pending) d_release();
                    return;
            }
        default:
            return;
    }
}

// ---------------------------------------------------------------- public
bool pva_dir_init(void)
{
    // everything lives in the stream RAM the ring leaves (none: no directory)
    size_t sz = 0;
    uint8_t *base = pva_net_spare(&sz);
    if (!base) return false;
    uint8_t *end = base + sz;
    uint8_t *m = (uint8_t *)(((uintptr_t)base + 3u) & ~(uintptr_t)3u);
    #define TAKE(p, type, n) do { p = (type)(void *)m; m += ((size_t)(n) + 3u) & ~(size_t)3u; } while (0)
    TAKE(DS, pva_esp_slot_t *, sizeof *DS);
    TAKE(d_path, char *, PATH_MAX_D + 1u);
    TAKE(q_path, char *, PATH_MAX_D + 1u);
    TAKE(req, char *, 512u);
    TAKE(location, char *, URL_KEEP + 1u);
    TAKE(val, char *, URL_KEEP + 1u);
    TAKE(f_url, char *, URL_KEEP + 1u);
    TAKE(ents, dent_rb_t *, MAX_ENTRIES * sizeof(dent_rb_t));
    TAKE(cache, uint8_t *, CACHE_SIZE);
    #undef TAKE
    if (m + 8192u > end) { ents = NULL; return false; }
    memset(base, 0, (size_t)(m - base));
    cache_size = CACHE_SIZE;
    pool = (char *)m;
    pool_size = (size_t)(end - m);
    DS->data = d_feed;
    pva_esp_register(DS);
    return true;
}

bool pva_dir_available(void) { return ents != NULL; }

// Start fetching a list ('path' is the API path with its query). The previous
// list is gone from then on; a list still coming in is dropped.
int pva_dir_start(uint8_t k, const char *path)
{
    if (!ents) return PVA_E_NOWIFI;
    if (!pva_hal_net_present()) { qerr = PVA_NERR_NOWIFI; qstate = PVA_DIRS_FAILED; return PVA_E_NOWIFI; }
    if (d != D_IDLE && !discard_body) cancel_req = true;
    q_kind = k;
    snprintf(q_path, PATH_MAX_D + 1, "%s", path);
    q_pending = true;
    q_append = false;
    nents = 0;
    qerr = 0;
    qstate = PVA_DIRS_LOADING;
    return PVA_OK;
}

// Fetch the next part of the list: the entries come after the ones there are.
int pva_dir_more(const char *path)
{
    if (!ents) return PVA_E_NOWIFI;
    if (d != D_IDLE && !discard_body) return PVA_E_BUSY;
    if (!pva_hal_net_present()) { qerr = PVA_NERR_NOWIFI; qstate = PVA_DIRS_FAILED; return PVA_E_NOWIFI; }
    q_kind = kind;
    snprintf(q_path, PATH_MAX_D + 1, "%s", path);
    q_pending = true;
    q_append = true;
    qerr = 0;
    qstate = PVA_DIRS_LOADING;
    return PVA_OK;
}

uint16_t pva_dir_raw(void) { return nraw; }

void pva_dir_cancel(void)
{
    q_pending = false;
    if (d != D_IDLE && !discard_body) cancel_req = true;
    if (qstate == PVA_DIRS_LOADING) qstate = PVA_DIRS_IDLE;
}

uint8_t *pva_dir_cache(size_t *size) { *size = cache_size; return cache; }

void pva_dir_click(const char *uuid)
{
    if (!uuid || !uuid[0] || strlen(uuid) >= sizeof click_uuid) return;
    snprintf(click_uuid, sizeof click_uuid, "%s", uuid);
    click_pending = true;
}

bool pva_dir_busy(void) { return d != D_IDLE || click_pending; }

// URL-encode 's' into 'out' (for query values)
void pva_dir_urlenc(char *out, size_t size, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < size; s++)
    {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}
