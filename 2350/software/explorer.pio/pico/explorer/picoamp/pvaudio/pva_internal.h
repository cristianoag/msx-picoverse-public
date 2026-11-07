// MSX PICOVERSE PROJECT
// pva_internal.h - PicoVerse Audio Engine internals (core 1 side).

#ifndef PVA_INTERNAL_H
#define PVA_INTERNAL_H

#include "pva.h"

#ifdef PVA_HOST
#define pva_barrier() __sync_synchronize()
#else
#include "hardware/sync.h"
#define pva_barrier() __dmb()
#endif

// register window + mailbox, shared by both cores
extern volatile uint8_t *pva_win;
typedef struct
{
    volatile uint32_t post_count;   // core 0: +1 per CMD_CODE write
    volatile uint8_t  token;
    volatile uint8_t  code;
} pva_mailbox_t;
extern pva_mailbox_t pva_mailbox;

// arena
void *pva_alloc(size_t size);       // zeroed, 4-byte aligned; NULL when full
size_t pva_arena_free(void);
size_t pva_arena_size(void);

// log (engine/LOG command)
void pva_log(const char *fmt, ...);
size_t pva_log_dump(char *out, size_t size);
uint16_t pva_log_count(void);

// hardware layer (pva_hal_pico.c, or the host/emulator one)
bool     pva_hal_audio_init(void);
int16_t *pva_hal_audio_acquire(uint32_t *max_frames);   // NULL: no free buffer
void     pva_hal_audio_commit(uint32_t frames, uint32_t sample_rate);
void     pva_hal_audio_mute(bool mute);
uint32_t pva_hal_millis(void);

// ESP-01 link (UNAPI ESP8266 firmware behind the cartridge UART). The engine
// owns it between open and close; the MSX side's WiFi registers are quiet then.
bool     pva_hal_net_open(void);                         // false: no ESP link
bool     pva_hal_net_present(void);
void     pva_hal_install_fault_handler(void);           // crash notes in the window (firmware)                      // an ESP answered at start-up
void     pva_hal_net_close(void);
size_t   pva_hal_net_write(const uint8_t *p, size_t n);  // non-blocking, bytes taken
size_t   pva_hal_net_read(uint8_t *p, size_t n);         // non-blocking, bytes given
// RAM for the stream buffer outside the arena (NULL: none)
uint8_t *pva_hal_stream_ram(size_t *size);

// decoder (pva_decode.c)
typedef struct
{
    char     title[PVA_ST_TITLE_LEN];
    char     artist[PVA_ST_ARTIST_LEN];
    char     album[PVA_ST_ALBUM_LEN];
    bool     tagged;
    uint8_t  format;                // PVA_DE_MP3 / PVA_DE_WAV
    uint32_t total_seconds;
    uint32_t bitrate_kbps;
    uint32_t sample_rate;
    uint8_t  channels;
    bool     seekable;
} pva_track_info_t;

bool pva_dec_init(void);
int  pva_dec_open(const char *fatfs_path, pva_track_info_t *info);  // PVA_OK / PVA_E_xxx
void pva_dec_close(void);
bool pva_dec_active(void);
// Produce at most one block of audio. Returns 1 if audio was produced,
// 0 if nothing could be done now (no free buffer), -1 at the end of the track.
int  pva_dec_step(void);
int  pva_dec_seek(uint32_t seconds);
uint32_t pva_dec_elapsed(void);
void pva_dec_set_volume(uint8_t vol);     // 0..100
void pva_dec_set_balance(uint8_t bal);    // 0 left .. 50 centre .. 100 right
void pva_dec_vu(uint8_t *l, uint8_t *r);
uint8_t pva_dec_last_fresult(void);

// shared ESP link (pva_esp.c)
#define PVA_FN_NETSTATE     3u
#define PVA_FN_TCPOPEN      13u
#define PVA_FN_TCPCLOSE     14u
#define PVA_FN_TCPABORT     15u
#define PVA_FN_TCPSTATE     16u
#define PVA_FN_TCPSEND      17u
#define PVA_FN_TCPRCV       18u
#define PVA_FN_DNS          206u
#define PVA_RC_NO_FREE_CONN 9u      // UNAPI ERR_NO_FREE_CONN
typedef struct pva_esp_slot
{
    uint8_t  txq[320];
    uint16_t txq_len;
    uint8_t  want_fn;           // 0 = nothing asked
    bool     queued;            // waits for the wire
    uint32_t timeout_ms;
    // the answer
    bool     rsp_done, rsp_timeout;
    uint8_t  rc;
    uint16_t pgot;              // payload bytes received
    uint8_t  pfirst[8];         // first payload bytes (ip, conn, state)
    uint32_t rcv_data;          // TCP_RCV data bytes (given to 'data')
    void   (*data)(const uint8_t *p, size_t n);
} pva_esp_slot_t;
void pva_esp_register(pva_esp_slot_t *s);
bool pva_esp_acquire(void);     // take the link (false: no ESP)
void pva_esp_release(void);
bool pva_esp_link_open(void);
void pva_esp_request(pva_esp_slot_t *s, uint8_t fn, const uint8_t *a, size_t n, const uint8_t *b, size_t m, uint32_t timeout);
void pva_esp_finish_now(pva_esp_slot_t *s, uint8_t fn);
bool pva_esp_finished(const pva_esp_slot_t *s);
void pva_esp_clear(pva_esp_slot_t *s);
void pva_esp_shorten(pva_esp_slot_t *s, uint32_t ms);
void pva_esp_io(void);
uint8_t pva_net_conn(void);     // the stream's ESP connection (0 = none)
uint8_t *pva_net_spare(size_t *size);   // stream RAM the ring does not use
void   pva_net_lend_tls(bool on);     // a web request borrows the stream's TLS connection (the ESP-01 has one)
bool   pva_net_tls_free(void);        // the stream holds none now
bool   pva_net_is_tls(void);          // the stream's URL is https
void   *pva_net_spare_take(size_t n);   // a piece of it for good (NULL: none left)
uint8_t pva_dir_conn(void);     // the directory's

// the Radio Browser directory (pva_dir.c)
#define PVA_DIRQ_COUNTRIES  1
#define PVA_DIRQ_STATIONS   2
bool        pva_dir_init(void);
bool        pva_dir_available(void);
int         pva_dir_start(uint8_t kind, const char *path);  // PVA_OK / PVA_E_NOWIFI / PVA_E_BUSY
void        pva_dir_cancel(void);
int         pva_dir_more(const char *path);     // continue the list (entries are added)
uint16_t    pva_dir_raw(void);                  // stations in the last reply, before the filters
void        pva_dir_poll(void);
bool        pva_dir_busy(void);
uint8_t     pva_dir_state(void);    // PVA_DIRS_xxx
uint8_t     pva_dir_error(void);
uint16_t    pva_dir_http(void);
uint8_t     pva_dir_kind(void);
bool        pva_dir_truncated(void);
uint16_t    pva_dir_count(void);
const char *pva_dir_name(uint16_t i);
const char *pva_dir_url(uint16_t i);
const char *pva_dir_uuid(uint16_t i);
uint16_t    pva_dir_num(uint16_t i);  // bitrate (stations) or station count (countries)
void        pva_dir_click(const char *uuid);
void        pva_dir_urlenc(char *out, size_t size, const char *s);
uint8_t    *pva_dir_cache(size_t *size);    // RAM the engine may use to keep a list

// stream input for the decoder (pva_decode.c)
int  pva_dec_open_stream(pva_track_info_t *info);       // MP3 from pva_net_read()
bool pva_dec_is_stream(void);
void pva_dec_frame_info(pva_track_info_t *info);        // rate/channels/bitrate of the last frame

// network streams (pva_net.c)
typedef struct
{
    uint8_t  state;                 // PVA_NET_xxx
    uint8_t  err;                   // PVA_NERR_xxx of the last failure
    uint16_t http;                  // last HTTP status
    uint32_t rx_bytes;              // audio bytes received
    uint32_t rate_bps;              // bytes per second, about the last 2 s
    uint16_t reconnects;
    uint16_t rcv_max;               // TCP_RCV request size in use
    uint32_t dns_ms, open_ms;       // time of the last DNS lookup and TCP/TLS open
    uint32_t back_ms;               // after the last lend: its reconnect, until audio comes again
    uint32_t gaps;                  // receive pauses longer than 500 ms
    uint32_t longest_gap_ms;
    uint32_t esp_errors;            // ESP answers with an error code
    uint32_t overflow;              // UART bytes lost (ring full)
    bool     tls;
    uint8_t  title_gen;             // +1 for every new stream title
} pva_net_stats_t;

bool pva_net_init(size_t keep);     // keep: bytes of the stream RAM left to the spare (the news)
int    pva_net_start(const char *url);          // PVA_OK, PVA_E_ARGS, PVA_E_NOWIFI
void   pva_net_stop(void);
void   pva_net_poll(void);
bool   pva_net_active(void);
size_t pva_net_read(uint8_t *dst, size_t n);    // audio bytes from the buffer
size_t pva_net_buffered(void);
size_t pva_net_capacity(void);
bool   pva_net_ended(void);                     // failed for good and the buffer is empty
const pva_net_stats_t *pva_net_stats(void);
const char *pva_net_station(void);              // icy-name (or "")
const char *pva_net_stream_title(void);         // StreamTitle (or "")
const char *pva_net_host(void);

// equalizer (pva_eq.c)
void pva_eq_set(const int8_t *gains);           // PVA_EQ_BANDS gains in dB
void pva_eq_get(int8_t *gains);
void pva_eq_reset(void);                        // a new track: clear the filter state
void pva_eq_process(int16_t *stereo, uint32_t frames, uint32_t rate);

// visualisation (pva_viz.c)
void pva_viz_init(void);
void pva_viz_feed(const int16_t *stereo, uint32_t frames, uint32_t rate);
void pva_viz_poll(bool playing);
void pva_viz_get(uint8_t *levels, uint8_t *peaks, uint8_t *vu_lr);

// streaming JSON (pva_json.c)
#define PVA_JSON_DEPTH      6
#define PVA_JSON_VALUE      1       // a value: path, its text, index in the innermost array
#define PVA_JSON_END        2       // an object ended (path = where it was)
typedef struct pva_json pva_json_t;
typedef void (*pva_json_cb)(pva_json_t *j, int ev, const char *path, const char *val, uint16_t idx);
struct pva_json
{
    pva_json_cb cb;
    void    *user;
    uint8_t  depth;
    bool     is_arr[PVA_JSON_DEPTH];
    uint16_t idx[PVA_JSON_DEPTH];
    char     name[PVA_JSON_DEPTH][32];
    bool     in_str, esc, is_key, expect_key, have_val, seen, done;
    uint8_t  uhex, klen;
    uint32_t ucp;
    char     key[32];
    char     val[200];
    uint16_t vlen;
    char     pbuf[96];
};
void pva_json_reset(pva_json_t *j);         // keeps cb and user
void pva_json_byte(pva_json_t *j, char c);

// web requests (pva_web.c)
#define PVA_WEB_IDLE        0
#define PVA_WEB_BUSY        1
#define PVA_WEB_DONE        2
#define PVA_WEB_FAILED      3
typedef bool (*pva_web_body_fn)(const uint8_t *p, size_t n);   // true: enough
typedef void (*pva_web_done_fn)(bool ok);
bool     pva_web_init(void);
int      pva_web_get(const char *url, pva_web_body_fn body, pva_web_done_fn done);
void     pva_web_poll(void);
bool     pva_web_busy(void);
uint8_t  pva_web_result(void);
uint8_t  pva_web_error(void);           // PVA_NERR_xxx
uint16_t pva_web_http(void);

// settings text, PICOAMP.INI (pva_cfg.c)
#define PVA_CFG_MAX         1000u   // the MSX reads it in one reply
void        pva_cfg_load(const char *t, size_t n);
const char *pva_cfg_text(size_t *n);
bool        pva_cfg_get(const char *sk, char *out, size_t size);   // "section.key"
bool        pva_cfg_set(const char *sk, const char *val);          // false: no room
uint8_t     pva_cfg_gen(void);

// time, place and weather now (pva_clock.c)
void pva_clock_http_date(const char *v);
bool pva_clock_have_time(void);
void pva_clock_poll(void);
void pva_clock_enable(void);
void pva_clock_cfg_changed(void);
int  pva_clock_info(void);
int  pva_clock_loc_ip(void);
int  pva_clock_search(const char *text);
int  pva_clock_list(void);
int  pva_clock_pick(uint8_t i);
int  pva_clock_wx(void);
bool pva_clock_local(uint8_t *hour, uint8_t *min, uint8_t *wday);
void pva_engine_extra(uint8_t *out);     // PVA_CK_EXTRA.. (sleep, alarm, news)

// news feeds (pva_news.c)
int  pva_news_cmd(uint8_t code, const uint8_t *args);   // PVA_C_NEWS_xxx
#define PVA_NEWS_KEEP (24u * 1024u)    // the news' memory from the stream RAM (Nextor modes)
void pva_news_init(size_t keep);       // takes 'keep' bytes of the stream RAM (before the directory does)
void pva_news_poll(void);
void pva_news_state(uint8_t *out);      // PVA_CK_NEWS_STATE.. (11 bytes)
typedef struct { const char *name; } pva_news_topic_t;
typedef struct { uint8_t topic; const char *code, *name, *url; } pva_news_feed_t;
extern const pva_news_topic_t pva_news_topics[];   // ends with a NULL name
extern const pva_news_feed_t  pva_news_cat[];      // ends with a NULL name
extern const char pva_news_default_favs[];
char    pva_fold_ascii(uint32_t c);     // a character to the MSX font's ASCII (0: none)
int64_t pva_days_from_civil(int y, unsigned m, unsigned d);
void    pva_civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d);
bool    pva_clock_utc(int64_t *now, int32_t *offset);   // false: no time yet (offset 0 without the zone)

// replies (pva_engine.c)
void pva_reply_byte(uint8_t b);
void pva_reply_str(const char *s);
size_t pva_log_tail(char *out, size_t size);     // the newest log lines, 0-terminated each
uint32_t pva_web_length(void);                   // the body's Content-Length (0: none)
bool     pva_web_lending(void);                  // the radio lent its TLS connection to this request
int      pva_web_get_from(const char *url, uint32_t from, pva_web_body_fn body, pva_web_done_fn done);
void     pva_web_hold(bool on);                  // the body's reading waits (the connection stays)
void pva_reply_patch16(uint32_t at, uint16_t v);   // a u16 at that place of the reply (little-endian)

// helpers
int  pva_kind_from_name(const char *name);   // PVA_DE_xxx or 0
void pva_copy_ascii(char *dst, size_t size, const char *src, size_t src_len);

#endif
