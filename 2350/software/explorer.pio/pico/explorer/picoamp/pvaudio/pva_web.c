// MSX PICOVERSE PROJECT
// pva_web.c - PicoVerse Audio Engine: a small HTTP client for web data (time,
// location, weather) over the ESP-01.
//
// One GET at a time, in its own slot of the shared ESP link, so a stream and
// a station list can go on meanwhile. The body goes to the caller as it comes
// in; redirects are followed (also from HTTP to HTTPS). Every reply's Date
// header sets the engine's clock.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pva_internal.h"

#define HOST_MAX        63u
#define PATH_MAX_W      420u
#define LOC_MAX         300u
#define MAX_REDIRECTS   3u
#define RCV_SIZE        1024u
#define T_DNS_MS        15000u
#define T_OPEN_MS       20000u
#define T_SEND_MS       3000u
#define T_RCV_MS        2000u
#define T_CLOSE_MS      1000u
#define T_QUERY_MS      90000u         // (an article's page can be big)
#define T_LEND_MS       10000u         // (the stream fills its buffer first, up to 4 s)
#define IDLE_POLL_MS    4u
#define QUIET_MS        400u

enum { W_IDLE, W_DNS, W_OPEN, W_SEND, W_HEAD, W_BODY, W_CLOSE, W_LEND };
enum { A_DONE, A_FAIL, A_REDIRECT };

static pva_esp_slot_t *WS;
static uint8_t  w = W_IDLE;
static bool     held, available;
static uint8_t  conn, redirects, after, werr;
static uint16_t whttp;
static bool     w_tls;
static bool     lending, lent_once;  // the stream's TLS connection borrowed
static uint16_t w_port;
static char     w_host[HOST_MAX + 1];
static char    *w_path;             // PATH_MAX_W + 1
static char    *req;                // 512
static size_t   req_len, req_pos;
static char    *location;           // LOC_MAX + 1
static char     line[160];
static size_t   line_len;
static bool     status_seen, body_end;
static uint32_t content_len, body_got, w_from, range_total;
static bool     w_hold;              // the body waits (the radio's buffer first)
static uint32_t t_query, t_phase, wait_until, state_asked;
static pva_web_body_fn body_fn;
static pva_web_done_fn done_fn;
static uint8_t  result = PVA_WEB_IDLE;

uint8_t  pva_web_error(void) { return werr; }
uint16_t pva_web_http(void) { return whttp; }
bool     pva_web_busy(void) { return w != W_IDLE; }
uint8_t  pva_web_result(void) { return result; }
uint32_t pva_web_length(void) { return whttp == 206 && range_total ? range_total : content_len + (whttp == 206 ? w_from : 0); }   // the whole body's size (0: not said)
void     pva_web_hold(bool on) { w_hold = on; }
bool     pva_web_lending(void) { return lending; }        // the stream's TLS connection is borrowed

static void w_close_then(uint8_t what)
{
    after = what;
    if (conn) pva_esp_request(WS, PVA_FN_TCPCLOSE, &conn, 1, NULL, 0, T_CLOSE_MS);
    else pva_esp_finish_now(WS, PVA_FN_TCPCLOSE);
    w = W_CLOSE;
}

static void w_trouble(uint8_t err)
{
    if (!werr) werr = err;
    pva_log("web: %.36s failed (%u)", w_host, (unsigned)err);
    w_close_then(A_FAIL);
}

static void http_line(void)
{
    line[line_len] = 0;
    while (line_len && (line[line_len - 1] == '\r' || line[line_len - 1] == ' ')) line[--line_len] = 0;
    if (!status_seen)
    {
        status_seen = true;
        const char *sp = strchr(line, ' ');
        whttp = sp ? (uint16_t)atoi(sp + 1) : 0;
        return;
    }
    char *colon = strchr(line, ':');
    if (!colon) return;
    *colon = 0;
    char *v = colon + 1;
    while (*v == ' ') v++;
    for (char *k = line; *k; k++) if (*k >= 'A' && *k <= 'Z') *k = (char)(*k + 32);
    if (!strcmp(line, "location")) snprintf(location, LOC_MAX + 1, "%s", v);
    else if (!strcmp(line, "content-length")) content_len = (uint32_t)atol(v);
    else if (!strcmp(line, "content-range")) { const char *t = strchr(v, '/'); range_total = t && t[1] != '*' ? (uint32_t)atol(t + 1) : 0; }
    else if (!strcmp(line, "date")) pva_clock_http_date(v);
}

static void w_feed(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char c = (char)p[i];
        if (w == W_HEAD)
        {
            if (c == '\n')
            {
                if (line_len == 0 || (line_len == 1 && line[0] == '\r')) { w = W_BODY; body_got = 0; continue; }
                http_line();
                line_len = 0;
            }
            else if (line_len < sizeof line - 1) line[line_len++] = c;
            continue;
        }
        if (w != W_BODY) return;
        // the rest of this piece of the body at once
        size_t k = n - i;
        if (content_len && body_got + k > content_len) k = content_len - body_got;
        body_got += (uint32_t)k;
        if ((whttp == 200 || whttp == 206) && body_fn && !body_end && k)
        {
            if (body_fn(p + i, k)) body_end = true;     // the caller has what it wants
        }
        return;
    }
}

static bool parse_target(const char *u)
{
    const char *p;
    if (!strncmp(u, "http://", 7)) { w_tls = false; p = u + 7; w_port = 80; }
    else if (!strncmp(u, "https://", 8)) { w_tls = true; p = u + 8; w_port = 443; }
    else if (u[0] == '/') { snprintf(w_path, PATH_MAX_W + 1, "%s", u); return true; }   // same host
    else return false;
    size_t h = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?')
    {
        if (h >= HOST_MAX) return false;
        w_host[h++] = *p++;
    }
    w_host[h] = 0;
    if (*p == ':') { w_port = (uint16_t)atoi(p + 1); while (*p && *p != '/' && *p != '?') p++; }
    snprintf(w_path, PATH_MAX_W + 1, "%s%s", *p == '?' ? "/" : "", *p ? p : "/");
    return h > 0;
}

static void w_connect(void)
{
    status_seen = false;
    body_end = false;
    state_asked = 0;
    line_len = 0;
    location[0] = 0;
    content_len = 0;
    range_total = 0;
    whttp = 0;
    conn = 0;
    uint8_t f = 0;
    pva_esp_request(WS, PVA_FN_DNS, &f, 1, (const uint8_t *)w_host, strlen(w_host), T_DNS_MS);
    t_phase = pva_hal_millis();
    w = W_DNS;
}

static void send_next(void)
{
    size_t k = req_len - req_pos;
    if (k > 128u) k = 128u;
    uint8_t a[2] = { conn, 1 };
    pva_esp_request(WS, PVA_FN_TCPSEND, a, 2, (const uint8_t *)req + req_pos, k, T_SEND_MS);
    req_pos += k;
}

static void ask_rcv(void)
{
    uint8_t a[3] = { conn, (uint8_t)RCV_SIZE, (uint8_t)(RCV_SIZE >> 8) };
    pva_esp_request(WS, PVA_FN_TCPRCV, a, 3, NULL, 0, T_RCV_MS);
}

static void w_end(bool ok)
{
    w = W_IDLE;
    pva_esp_clear(WS);
    conn = 0;
    if (lending) { pva_net_lend_tls(false); lending = false; pva_log("web: the TLS connection back to the stream"); }
    if (held) { pva_esp_release(); held = false; }
    result = ok ? PVA_WEB_DONE : PVA_WEB_FAILED;
    pva_web_done_fn f = done_fn;
    done_fn = NULL;
    body_fn = NULL;
    if (ok) pva_log("web: %.30s %lu bytes in %lu ms", w_host, (unsigned long)body_got, (unsigned long)(pva_hal_millis() - t_query));
    if (f) f(ok);
}

// the reply is complete (the server closed, or all of Content-Length is in)
static void w_complete(void)
{
    if (w != W_BODY) { w_trouble(PVA_NERR_CLOSED); return; }
    if (whttp >= 300 && whttp < 400 && location[0] && redirects < MAX_REDIRECTS) { redirects++; w_close_then(A_REDIRECT); return; }
    if (whttp != 200 && !(whttp == 206 && w_from)) { w_trouble(PVA_NERR_HTTP); return; }
    w_close_then(A_DONE);
}

void pva_web_poll(void)
{
    if (w == W_IDLE) return;
    pva_esp_io();
    if (!pva_esp_finished(WS) && w != W_HEAD && w != W_BODY && w != W_LEND) return;
    uint32_t now = pva_hal_millis();
    switch (w)
    {
        case W_DNS:
            if (WS->rsp_timeout || WS->rc != 0 || WS->pgot < 4) { pva_esp_clear(WS); w_trouble(PVA_NERR_DNS); return; }
            {
                uint8_t a[11];
                memcpy(a, WS->pfirst, 4);
                a[4] = (uint8_t)w_port; a[5] = (uint8_t)(w_port >> 8);
                a[6] = 0xFF; a[7] = 0xFF; a[8] = 0; a[9] = 0;
                a[10] = w_tls ? 0x04 : 0x00;
                pva_esp_clear(WS);
                pva_esp_request(WS, PVA_FN_TCPOPEN, a, 11, w_tls ? (const uint8_t *)w_host : NULL, w_tls ? strlen(w_host) + 1 : 0, T_OPEN_MS);
                w = W_OPEN;
            }
            return;
        case W_OPEN:
            if (WS->rsp_timeout || WS->rc != 0 || WS->pgot < 1)
            {
                pva_log("web: %s open failed (rc %u)", w_tls ? "TLS" : "TCP", (unsigned)WS->rc);
                if (w_tls && !WS->rsp_timeout && WS->rc == 9 && !lent_once && !pva_net_tls_free())
                {
                    // no free connection (UNAPI ERR_NO_FREE_CONN): the ESP-01 has one for TLS,
                    // and the radio uses it; it lends it for this request
                    pva_esp_clear(WS);
                    lent_once = true;
                    lending = true;
                    pva_net_lend_tls(true);
                    t_phase = now;
                    w = W_LEND;
                    return;
                }
                pva_esp_clear(WS);
                w_trouble(PVA_NERR_CONNECT);
                return;
            }
            conn = WS->pfirst[0];
            pva_esp_clear(WS);
            {
                char range[32] = "";
                if (w_from) snprintf(range, sizeof range, "Range: bytes=%lu-\r\n", (unsigned long)w_from);
                int n = snprintf(req, 640,
                    "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PicoAmp/1.1 (MSX PicoVerse 2350)\r\n"
                    "Accept: */*\r\n%sConnection: close\r\n\r\n", w_path, w_host, range);
                req_len = (n > 0 && n < 640) ? (size_t)n : 0;
                req_pos = 0;
            }
            send_next();
            w = W_SEND;
            return;
        case W_SEND:
            if (WS->rsp_timeout || WS->rc != 0) { pva_esp_clear(WS); w_trouble(PVA_NERR_TIMEOUT); return; }
            pva_esp_clear(WS);
            if (req_pos < req_len) { send_next(); return; }
            w = W_HEAD;
            t_phase = now;
            wait_until = now;
            ask_rcv();
            return;
        case W_HEAD:
        case W_BODY:
            if (WS->want_fn == PVA_FN_TCPRCV)
            {
                if (!pva_esp_finished(WS)) return;
                bool timeout = WS->rsp_timeout;
                uint8_t r = WS->rc;
                uint32_t got = WS->rcv_data;
                pva_esp_clear(WS);
                if (timeout) { w_trouble(PVA_NERR_TIMEOUT); return; }
                bool ended = w == W_BODY && (body_end || (content_len && body_got >= content_len) ||
                                             (whttp >= 300 && whttp < 400 && location[0]));
                if (r != 0 || ended) { w_complete(); return; }
                wait_until = got ? now : now + IDLE_POLL_MS;
                if (got) t_phase = now;
            }
            else if (WS->want_fn == PVA_FN_TCPSTATE)
            {
                if (!pva_esp_finished(WS)) return;
                bool timeout = WS->rsp_timeout;
                uint8_t r = WS->rc, cstate = WS->pfirst[1];
                uint16_t avail = (uint16_t)(WS->pfirst[2] | (WS->pfirst[3] << 8));
                bool ok = WS->pgot >= 4;
                pva_esp_clear(WS);
                state_asked = now;
                if (!timeout && (r != 0 || (ok && cstate != 4u && avail == 0))) { w_complete(); return; }
                wait_until = now;
            }
            if (!WS->want_fn && now - t_phase >= QUIET_MS && now - state_asked >= QUIET_MS)
            {
                uint8_t a = conn;
                pva_esp_request(WS, PVA_FN_TCPSTATE, &a, 1, NULL, 0, T_RCV_MS);
                return;
            }
            if (w_hold && w == W_BODY && !body_end && !WS->want_fn)
            {
                // the radio's buffer first: the connection stays, the ESP keeps what comes meanwhile
                t_phase = now;
                state_asked = now;
                if (now - t_query > T_QUERY_MS / 2u) t_query = now - T_QUERY_MS / 2u;
                return;
            }
            if (now - t_phase > 10000u || now - t_query > T_QUERY_MS) { w_trouble(PVA_NERR_TIMEOUT); return; }
            if (!WS->want_fn && (int32_t)(now - wait_until) >= 0) ask_rcv();
            return;
        case W_LEND:                    // the stream closes its TLS connection, then this one opens
            if (pva_net_tls_free()) { pva_log("web: TLS lent by the stream"); w_connect(); return; }
            if (now - t_phase > T_LEND_MS) { w_trouble(PVA_NERR_CONNECT); return; }
            return;
        case W_CLOSE:
            pva_esp_clear(WS);
            conn = 0;
            if (after == A_REDIRECT)
            {
                if (!parse_target(location)) { werr = PVA_NERR_URL; w_end(false); return; }
                pva_log("web: redirect to %.40s", location);
                w_connect();
                return;
            }
            w_end(after == A_DONE);
            return;
        default:
            return;
    }
}

bool pva_web_init(void)
{
    // a small piece of the stream RAM (none: no WiFi in this firmware)
    WS = pva_net_spare_take(sizeof *WS);
    w_path = pva_net_spare_take(PATH_MAX_W + 1u);
    req = pva_net_spare_take(640u);
    location = pva_net_spare_take(LOC_MAX + 1u);
    available = WS && w_path && req && location;
    if (!available) return false;
    memset(WS, 0, sizeof *WS);
    WS->data = w_feed;
    pva_esp_register(WS);
    return true;
}

// Start a GET; 'body' gets the body (return true to say it has enough),
// 'done' the outcome. PVA_E_BUSY while another GET runs.
int pva_web_get(const char *url, pva_web_body_fn body, pva_web_done_fn done)
{
    return pva_web_get_from(url, 0, body, done);
}

// the same from byte 'from' on (Range); a server that does not do that sends all
// of it (pva_web_http() 200, not 206)
int pva_web_get_from(const char *url, uint32_t from, pva_web_body_fn body, pva_web_done_fn done)
{
    if (!available) return PVA_E_NOWIFI;
    if (!pva_hal_net_present()) return PVA_E_NOWIFI;
    if (w != W_IDLE) return PVA_E_BUSY;
    if (!parse_target(url) || url[0] == '/') return PVA_E_ARGS;
    if (!held && !pva_esp_acquire()) return PVA_E_NOWIFI;
    held = true;
    body_fn = body;
    done_fn = done;
    redirects = 0;
    werr = 0;
    lent_once = false;
    w_from = from;
    w_hold = false;
    result = PVA_WEB_BUSY;
    t_query = pva_hal_millis();
    pva_log("web: %.48s", url);
    w_connect();
    return PVA_OK;
}
