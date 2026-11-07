// MSX PICOVERSE PROJECT
// pva_esp.c - PicoVerse Audio Engine: the ESP-01 link, shared by several clients.
//
// The UNAPI ESP8266 firmware answers one request at a time:
//   request   [fn] [len MSB] [len LSB] [payload]
//   response  [fn] [rc] [len MSB] [len LSB] [payload]   (payload only if rc = 0)
// Each client (the stream, the station directory) has a slot with its own
// request and answer. A slot's request waits until the wire is free; slots that
// wait take turns, so a stream and a directory query share the link fairly.
//
// A request that is cancelled while its answer is on its way still has that
// answer read off the wire (and dropped): otherwise its bytes could be taken
// for the answer to the next request (two TCP_RCV answers look alike).
//
// The UART belongs to the engine while at least one client holds the link
// (pva_esp_acquire/release); the MSX side's WiFi registers are quiet then.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pva_internal.h"
#ifdef PVA_HOST
#include <stdio.h>
#include <stdlib.h>
static int esp_trace = -1;
#define TRACE(...) do { if (esp_trace < 0) esp_trace = getenv("PVA_ESP_TRACE") != NULL; if (esp_trace) { printf("    [esp %u] ", (unsigned)pva_hal_millis()); printf(__VA_ARGS__); printf("\n"); } } while (0)
#else
#define TRACE(...) do { } while (0)
#endif

#define MAX_SLOTS   3

static pva_esp_slot_t *slots[MAX_SLOTS];
static uint8_t  nslots, turn;
static uint8_t  holders;            // clients holding the link
static bool     link_open;

// the answer on the wire
static bool     busy;               // a request was sent, its answer is not complete
static pva_esp_slot_t *active;      // NULL: its slot gave it up (answer is dropped)
static uint8_t  cur_fn;
static uint32_t cur_deadline;
static const uint8_t *tx_p;
static size_t   tx_left;
static uint8_t  ph;                 // 0 hunting fn, 1 rc, 2 len hi, 3 len lo, 4 payload
static uint8_t  rc;
static uint16_t plen, pgot;
static uint8_t  pfirst[sizeof ((pva_esp_slot_t *)0)->pfirst];
static uint32_t rcv_data;
// The ESP does one thing at a time: until it has answered a request it does
// not even read the next one (a TLS handshake can take seconds). So when a
// client stops waiting (its time-out, or it gave the request up), the answer
// is still awaited and dropped, for up to GRACE_MS more. Only then is the wire
// freed, after the line has been quiet for DRAIN_MS, so that late bytes cannot
// be taken for the answer to the next request.
#define GRACE_MS    12000u
#define DRAIN_MS    60u
static bool     in_grace;
// a dropped TCP_OPEN that succeeded leaves a connection nobody knows: close it
static pva_esp_slot_t closer;
static uint8_t  orphan;
static bool     draining;
static uint32_t drain_until;
uint32_t pva_esp_timeouts, pva_esp_drained;

void pva_esp_register(pva_esp_slot_t *s)
{
    if (nslots < MAX_SLOTS) slots[nslots++] = s;
}

bool pva_esp_acquire(void)
{
    if (!link_open)
    {
        if (!pva_hal_net_open()) return false;
        link_open = true;
        busy = false;
        draining = false;
        active = NULL;
    }
    holders++;
    return true;
}

void pva_esp_release(void)
{
    if (holders) holders--;
    if (holders || !link_open) return;
    pva_hal_net_close();
    TRACE("link closed%s", busy ? " WHILE BUSY" : "");
    link_open = false;
    busy = false;
    active = NULL;
    for (uint8_t i = 0; i < nslots; i++) slots[i]->queued = false;
}

bool pva_esp_link_open(void) { return link_open; }

void pva_esp_request(pva_esp_slot_t *s, uint8_t fn, const uint8_t *a, size_t n, const uint8_t *b, size_t m, uint32_t timeout)
{
    if (active == s) active = NULL;         // an older answer still on the wire is dropped
    size_t len = n + m;
    if (3 + len > sizeof s->txq) len = sizeof s->txq - 3;
    if (n > len) n = len;
    if (m > len - n) m = len - n;
    s->txq[0] = fn;
    s->txq[1] = (uint8_t)(len >> 8);
    s->txq[2] = (uint8_t)len;
    if (n) memcpy(s->txq + 3, a, n);
    if (m) memcpy(s->txq + 3 + n, b, m);
    s->txq_len = (uint16_t)(3 + len);
    s->want_fn = fn;
    s->timeout_ms = timeout;
    s->queued = true;
    s->rsp_done = s->rsp_timeout = false;
    s->rc = 0;
    s->pgot = 0;
    s->rcv_data = 0;
}

void pva_esp_finish_now(pva_esp_slot_t *s, uint8_t fn)
{
    // "done" without going to the ESP (nothing to close, say)
    if (active == s) active = NULL;
    s->queued = false;
    s->want_fn = fn;
    s->rsp_done = true;
    s->rsp_timeout = false;
    s->rc = 0;
    s->pgot = 0;
}

bool pva_esp_finished(const pva_esp_slot_t *s) { return s->rsp_done || s->rsp_timeout; }

void pva_esp_clear(pva_esp_slot_t *s)
{
    if (active == s) active = NULL;
    s->queued = false;
    s->want_fn = 0;
    s->rsp_done = s->rsp_timeout = false;
}

void pva_esp_shorten(pva_esp_slot_t *s, uint32_t ms)
{
    uint32_t now = pva_hal_millis();
    if (active == s && (int32_t)(cur_deadline - (now + ms)) > 0) cur_deadline = now + ms;
    if (s->queued && s->timeout_ms > ms) s->timeout_ms = ms;
}

static void finish_answer(void)
{
    if (!active && cur_fn == PVA_FN_TCPOPEN && rc == 0 && pgot >= 1 && pfirst[0])
        orphan = pfirst[0];
    TRACE("answer fn %u rc %u len %u%s", cur_fn, rc, plen, active ? "" : " (dropped)");
    busy = false;
    if (!active) return;
    pva_esp_slot_t *s = active;
    active = NULL;
    s->rc = rc;
    s->pgot = pgot;
    memcpy(s->pfirst, pfirst, sizeof pfirst);
    s->rcv_data = rcv_data;
    s->rsp_done = true;
}

static void payload_bytes(const uint8_t *p, size_t n)
{
    // keep the first bytes; TCP_RCV data (after the 2 urgent bytes) goes to the slot's handler
    while (n)
    {
        if (pgot < sizeof pfirst && !(cur_fn == PVA_FN_TCPRCV && pgot >= 2))
        {
            pfirst[pgot++] = *p++;
            n--;
            continue;
        }
        size_t k = n;
        if (k > (size_t)(plen - pgot)) k = (size_t)(plen - pgot);
        if (cur_fn == PVA_FN_TCPRCV && active && active->data)
        {
            active->data(p, k);
            rcv_data += (uint32_t)k;
        }
        pgot = (uint16_t)(pgot + k);
        p += k;
        n -= k;
    }
}

void pva_esp_io(void)
{
    if (!link_open) return;
    uint32_t now = pva_hal_millis();
    // the next request goes out when the wire is free; waiting slots take turns
    if (draining && (int32_t)(now - drain_until) >= 0) draining = false;
    if (orphan && !closer.queued && !busy)
    {
        TRACE("closing orphan connection %u", orphan);
        pva_esp_request(&closer, PVA_FN_TCPCLOSE, &orphan, 1, NULL, 0, 1000u);
        orphan = 0;
    }
    if (!busy && !draining && closer.queued)
    {
        closer.queued = false;
        active = NULL;              // nobody waits for this answer
        busy = true;
        in_grace = false;
        cur_fn = closer.want_fn;
        cur_deadline = now + closer.timeout_ms;
        tx_p = closer.txq;
        tx_left = closer.txq_len;
        ph = 0; rc = 0; plen = pgot = 0; rcv_data = 0;
    }
    if (!busy && !draining)
    {
        for (uint8_t k = 0; k < nslots; k++)
        {
            uint8_t i = (uint8_t)((turn + k) % nslots);
            pva_esp_slot_t *s = slots[i];
            if (!s->queued) continue;
            s->queued = false;
            turn = (uint8_t)((i + 1) % nslots);
            active = s;
            busy = true;
            in_grace = false;
            cur_fn = s->want_fn;
            TRACE("send fn %u slot %u timeout %u", cur_fn, i, (unsigned)s->timeout_ms);
            cur_deadline = now + s->timeout_ms;
            tx_p = s->txq;
            tx_left = s->txq_len;
            ph = 0;
            rc = 0;
            plen = pgot = 0;
            rcv_data = 0;
            break;
        }
    }
    if (busy && tx_left)
    {
        size_t w = pva_hal_net_write(tx_p, tx_left);
        tx_p += w;
        tx_left -= w;
    }

    uint8_t buf[256];
    for (int rounds = 0; rounds < 8; rounds++)
    {
        size_t n = pva_hal_net_read(buf, sizeof buf);
        if (!n) break;
        if (draining) { pva_esp_drained += (uint32_t)n; drain_until = pva_hal_millis() + DRAIN_MS; continue; }
        size_t i = 0;
        while (i < n)
        {
            if (!busy || tx_left) { i = n; break; }      // nothing asked yet: stray bytes
            uint8_t b = buf[i];
            switch (ph)
            {
                case 0: if (b == cur_fn) ph = 1; i++; break;
                case 1: rc = b; ph = 2; i++; break;
                case 2: plen = (uint16_t)(b << 8); ph = 3; i++; break;
                case 3:
                    plen = (uint16_t)(plen | b);
                    pgot = 0;
                    i++;
                    if (plen > 4096u) { ph = 0; break; }    // implausible: keep hunting
                    if (rc != 0 || plen == 0) { finish_answer(); break; }
                    ph = 4;
                    break;
                default:
                {
                    size_t k = n - i;
                    if (k > (size_t)(plen - pgot)) k = (size_t)(plen - pgot);
                    payload_bytes(buf + i, k);
                    i += k;
                    if (pgot >= plen) finish_answer();
                    break;
                }
            }
        }
    }
    if (busy && (int32_t)(pva_hal_millis() - cur_deadline) >= 0)
    {
        pva_esp_timeouts++;
        TRACE("TIMEOUT fn %u%s", cur_fn, active ? "" : in_grace ? " (grace over)" : " (dropped)");
        if (active) { active->rsp_timeout = true; active = NULL; }
        if (!in_grace)
        {
            // the client stops waiting; the wire waits on for the answer
            in_grace = true;
            cur_deadline = pva_hal_millis() + GRACE_MS;
        }
        else
        {
            busy = false;
            in_grace = false;
            draining = true;
            drain_until = pva_hal_millis() + DRAIN_MS;
        }
    }
}
