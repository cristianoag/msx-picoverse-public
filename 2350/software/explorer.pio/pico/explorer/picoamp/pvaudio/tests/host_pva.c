// Host test for the PicoVerse Audio Engine.
//
// Builds a FAT16 image with FatFS, then drives the register window the way
// the SymbOS player does: commands through ARGS/CMD_TOKEN/CMD_CODE, replies
// polled through DONE_TOKEN, the status block read with its sequence byte.
// Core 1 runs "concurrently": it is polled between MSX accesses.
//
// usage: host_pva <assets dir>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include "ff.h"
#include "../pva.h"
#include "../pva_internal.h"

int  host_disk_open(const char *path, uint32_t create_sectors);
void host_wav_open(const char *path);
void host_wav_close(void);
void host_advance_ms(uint32_t ms);
extern uint32_t host_audio_frames, host_audio_last_rate, host_audio_underruns;
extern bool host_audio_muted, host_audio_unlimited;
extern void (*host_audio_sink)(const int16_t *pcm, uint32_t frames, uint32_t rate);
// simulated ESP (host_esp.c)
int  host_esp_load_audio(const char *mp3_path);
void host_esp_reset(void);
int  host_esp_open_conns(void);
bool host_esp_owned(void);
extern bool host_esp_present;
extern uint8_t host_esp_wifi_state;
extern uint32_t host_esp_server_bps, host_esp_drop_after, host_esp_stall_at, host_esp_stall_ms, host_esp_tls_ms;
extern uint32_t host_esp_opens, host_esp_closes, host_esp_tls_opens, host_esp_requests;
extern char host_esp_last_sni[64], host_esp_last_get[300];
void host_esp_leftover(bool tls);
extern uint32_t host_esp_rtt_ms;
extern bool host_esp_api_down, host_esp_api_all_down;
extern uint32_t host_esp_clicks, host_esp_api_bps;
extern char host_esp_last_click[48], host_esp_last_api[300];
extern uint32_t host_esp_delay_next_rcv, host_esp_state_asks;
extern bool host_esp_close_silent;
extern long long host_esp_date_base;
extern bool host_esp_web_down;
extern uint32_t host_esp_web_requests;
extern char host_esp_last_web[600];
extern bool host_esp_range;
extern uint32_t host_esp_ranges, host_esp_redirs;

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else if (verbose) { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int verbose = 1;

// ------------------------------------------------------------------ MSX side
static unsigned access_count;
static void core1_tick(void) { pva_core1_poll(); host_advance_ms(1); }

static uint8_t rd(uint16_t a)
{
    if (++access_count % 100u == 0u) core1_tick();   // a Z80 makes ~100+ accesses per ms
    return pva_mem_read(a);
}
static void wr(uint16_t a, uint8_t v)
{
    if (++access_count % 100u == 0u) core1_tick();
    pva_mem_write(a, v);
}

static uint8_t token;
static uint8_t reply[PVA_REPLY_SIZE + 1];
static uint16_t reply_len;

// one command, waiting like the app does (it checks once per timer tick)
static int cmd(uint8_t code, const void *args, size_t n)
{
    const uint8_t *a = args;
    for (size_t i = 0; i < n; i++) wr((uint16_t)(PVA_WIN_BASE + PVA_R_ARGS + i), a[i]);
    token++;
    wr(PVA_WIN_BASE + PVA_R_CMD_TOKEN, token);
    wr(PVA_WIN_BASE + PVA_R_CMD_CODE, code);
    for (int tick = 0; tick < 2000; tick++)
    {
        if (rd(PVA_WIN_BASE + PVA_R_DONE_TOKEN) == token)
        {
            int r = rd(PVA_WIN_BASE + PVA_R_RESULT);
            reply_len = (uint16_t)(rd(PVA_WIN_BASE + PVA_R_REPLY_LEN) | (rd(PVA_WIN_BASE + PVA_R_REPLY_LEN + 1) << 8));
            for (uint16_t i = 0; i < reply_len && i < PVA_REPLY_SIZE; i++) reply[i] = rd((uint16_t)(PVA_WIN_BASE + PVA_R_REPLY + i));
            reply[reply_len] = 0;
            return r;
        }
        for (int k = 0; k < 20; k++) core1_tick();   // 20 ms between checks
    }
    return -1;
}

static int cmd_str(uint8_t code, const char *s) { return cmd(code, s, strlen(s) + 1); }
static int cmd_u16(uint8_t code, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; return cmd(code, b, 2); }
static int cmd_u8(uint8_t code, uint8_t v) { return cmd(code, &v, 1); }

static uint8_t st[PVA_STATUS_SIZE];
static void read_status(void)
{
    for (int tries = 0; tries < 10; tries++)
    {
        uint8_t s1 = rd(PVA_WIN_BASE + PVA_R_STATUS);
        for (unsigned i = 0; i < PVA_STATUS_SIZE; i++) st[i] = rd((uint16_t)(PVA_WIN_BASE + PVA_R_STATUS + i));
        uint8_t s2 = rd(PVA_WIN_BASE + PVA_R_STATUS);
        if (s1 == s2 && !(s1 & 1)) return;
    }
    printf("  (status never consistent)\n");
}
static uint16_t st16(int o) { return (uint16_t)(st[o] | (st[o + 1] << 8)); }
static const char *ststr(int o) { return (const char *)&st[o]; }

typedef struct { uint8_t type; uint32_t size; char name[PVA_NAME_MAX + 1]; } entry_t;
static entry_t ents[300];
static int list_dir(uint16_t count)
{
    int n = 0;
    while (n < count)
    {
        uint8_t a[3] = { (uint8_t)n, (uint8_t)(n >> 8), 40 };
        if (cmd(PVA_C_DIR_READ, a, 3) != PVA_OK) return -1;
        if (!reply_len) break;
        for (uint16_t p = 0; p < reply_len && n < 300; )
        {
            entry_t *e = &ents[n++];
            e->type = reply[p];
            e->size = reply[p + 1] | (reply[p + 2] << 8) | ((uint32_t)reply[p + 3] << 16) | ((uint32_t)reply[p + 4] << 24);
            uint8_t l = reply[p + 5];
            memcpy(e->name, &reply[p + 6], l);
            e->name[l] = 0;
            p += 6u + l;
        }
    }
    return n;
}
static int find(int n, const char *name) { for (int i = 0; i < n; i++) if (!strcmp(ents[i].name, name)) return i; return -1; }

static void run_ms(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) core1_tick(); }

// the clock block (PVA_R_CLOCK)
static uint8_t ck[PVA_CLOCK_SIZE];
static void read_clock(void)
{
    for (int t = 0; t < 10; t++)
    {
        uint8_t s0 = rd(PVA_WIN_BASE + PVA_R_CLOCK);
        for (unsigned i = 0; i < PVA_CLOCK_SIZE; i++) ck[i] = rd((uint16_t)(PVA_WIN_BASE + PVA_R_CLOCK + i));
        if (!(s0 & 1) && rd(PVA_WIN_BASE + PVA_R_CLOCK) == s0) return;
    }
}
// the local time the simulated servers' Date gives, at offset 'om' minutes:
// 'clock y-m-d h:m'
static void expect_time(int om, char *out, size_t size)
{
    long long t = host_esp_date_base + pva_hal_millis() / 1000u + om * 60;
    time_t tt = (time_t)t;
    struct tm g;
    gmtime_r(&tt, &g);
    snprintf(out, size, "%04d-%02d-%02d %02d:%02d wd%d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, (g.tm_wday + 6) % 7);
}
static void clock_text(char *out, size_t size)
{
    read_clock();
    snprintf(out, size, "%04d-%02d-%02d %02d:%02d wd%d", ck[PVA_CK_YEAR] | (ck[PVA_CK_YEAR + 1] << 8), ck[PVA_CK_MONTH], ck[PVA_CK_DAY],
             ck[PVA_CK_HOUR], ck[PVA_CK_MIN], ck[PVA_CK_WDAY]);
}
// the i-th of the 0-terminated strings from s on
static const char *nth_str(const char *s, int i) { while (i-- > 0) s += strlen(s) + 1; return s; }
#define WAIT_CLOCK(cond, ms) do { for (int w_ = 0; w_ < (ms) / 50; w_++) { run_ms(50); read_clock(); if (cond) break; } } while (0)

// ------------------------------------------------------------------ image
static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("missing %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) exit(2);
    fclose(f);
    return b;
}
static void put(const char *dst, const void *data, size_t len)
{
    FIL f; UINT bw;
    if (f_open(&f, dst, FA_CREATE_ALWAYS | FA_WRITE) || f_write(&f, data, (UINT)len, &bw) || bw != len) { printf("write %s failed\n", dst); exit(2); }
    f_close(&f);
}
static const char *assets;
static void put_file(const char *dst, const char *src)
{
    char p[512]; size_t len;
    snprintf(p, sizeof p, "%s/%s", assets, src);
    uint8_t *b = slurp(p, &len);
    put(dst, b, len);
    free(b);
}

static void build_image(void)
{
    static FATFS fs;
    static BYTE work[4096];
    host_disk_open("pva_test.img", 64 * 2048);             // 64 MB
    MKFS_PARM opt = { FM_FAT, 0, 0, 0, 0 };                // FAT16, as Nextor uses
    if (f_mkfs("0:", &opt, work, sizeof work)) { printf("mkfs failed\n"); exit(2); }
    f_mount(&fs, "0:", 1);
    f_mkdir("0:/MUSIC");
    f_mkdir("0:/MUSIC/Caf\xC3\xA9 del Mar");                // UTF-8 folder name
    f_mkdir("0:/MUSIC/Empty");
    f_mkdir("0:/SYMBOS");
    f_mkdir("0:/System Volume Information");
    f_chmod("0:/System Volume Information", AM_HID | AM_SYS, AM_HID | AM_SYS);
    put_file("0:/MUSIC/Caf\xC3\xA9 del Mar/01 Sine.mp3", "cbr128_v23.mp3");
    put_file("0:/MUSIC/Caf\xC3\xA9 del Mar/02 VBR.mp3", "vbr_v24.mp3");
    put_file("0:/MUSIC/Caf\xC3\xA9 del Mar/03 Mono.mp3", "mono22_v1.mp3");
    put_file("0:/MUSIC/Caf\xC3\xA9 del Mar/04 Tone.wav", "tone.wav");
    put_file("0:/MUSIC/long.mp3", "long192.mp3");
    put_file("0:/MUSIC/readme.txt", "tone.wav");
    put_file("0:/MUSIC/._long.mp3", "tone.wav");
    put("0:/SYMBOS/SYM.COM", "x", 1);
    {
        const char *r = "#EXTM3U\r\n#EXTINF:-1,Test FM\r\nhttp://radio.test/stream.mp3\r\n#EXTINF:-1,Secure Radio\r\nhttps://secure.test/stream.mp3\r\n";
        put("0:/SYMBOS/RADIO.M3U", r, strlen(r));
    }
    const char *m3u =
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "#EXTINF:20,Sine\r\n"
        "Caf\xC3\xA9 del Mar/01 Sine.mp3\r\n"
        "\r\n"
        "missing track.mp3\r\n"
        "C:\\Users\\me\\Music\\long.mp3\r\n"
        "#EXTINF:-1,Plain Radio\r\n"
        "http://radio.test/plain\r\n"
        "/MUSIC/Caf\xC3\xA9 del Mar/04 Tone.wav\r\n";
    put("0:/MUSIC/Mix.m3u", m3u, strlen(m3u));
    // many files in one folder
    f_mkdir("0:/MUSIC/Many");
    uint8_t *b; size_t bl; char p[300];
    snprintf(p, sizeof p, "%s/mono22_v1.mp3", assets);
    b = slurp(p, &bl);
    for (int i = 0; i < 120; i++)
    {
        snprintf(p, sizeof p, "0:/MUSIC/Many/Track %03d with a rather long file name for paging.mp3", 119 - i);
        put(p, b, bl);
    }
    free(b);
    f_mount(NULL, "0:", 0);
}

// ------------------------------------------------------------------ audio check
static int64_t sink_sum_abs;
static uint32_t sink_frames;
static uint64_t sink_l, sink_r;
static void sink(const int16_t *pcm, uint32_t frames, uint32_t rate)
{
    (void)rate;
    for (uint32_t i = 0; i < frames * 2; i++) sink_sum_abs += pcm[i] < 0 ? -pcm[i] : pcm[i];
    for (uint32_t i = 0; i < frames; i++) { sink_l += pcm[2 * i] < 0 ? -pcm[2 * i] : pcm[2 * i]; sink_r += pcm[2 * i + 1] < 0 ? -pcm[2 * i + 1] : pcm[2 * i + 1]; }
    sink_frames += frames;
}
static double sink_level(void) { double v = sink_frames ? (double)sink_sum_abs / (sink_frames * 2.0) : 0; sink_sum_abs = 0; sink_frames = 0; return v; }

// ------------------------------------------------------------------ tests
int main(int argc, char **argv)
{
    assets = argc > 1 ? argv[1] : "assets";
    build_image();
    {
        char sp[300];
        snprintf(sp, sizeof sp, "%s/cbr128_v23.mp3", assets);
        host_esp_load_audio(sp);
    }

    static uint8_t arena[120 * 1024];     // as the firmware: 128 KB left by Nextor - 8 KB core 1 stack
    pva_init(arena, sizeof arena);
    pva_core1_init();
    host_audio_sink = sink;

    printf("[identification and diagnostics]\n");
    char id[9];
    for (int i = 0; i < 8; i++) id[i] = (char)rd((uint16_t)(PVA_WIN_BASE + i));
    id[8] = 0;
    CHECK(!strcmp(id, "PVAUDIO"), "ID '%s', API %u", id, rd(PVA_WIN_BASE + 8));
    CHECK(rd(PVA_WIN_BASE + 0xFFF) == 0xFF && rd(0x5000) == 0xFF && rd(0x3FFF) == 0xFF, "outside the window reads FFh");
    uint8_t hb0 = rd(PVA_WIN_BASE + PVA_R_HEARTBEAT);
    run_ms(50);
    CHECK((uint8_t)(rd(PVA_WIN_BASE + PVA_R_HEARTBEAT) - hb0) >= 5, "heartbeat runs");
    CHECK(cmd(PVA_C_PING, NULL, 0) == PVA_OK && !strncmp((char *)reply, "PVAUDIO ", 8), "PING -> '%s'", reply);
    read_status();
    CHECK(st[PVA_ST_FLAGS] & PVA_F_MOUNTED, "card mounted at start, mount FRESULT %u", st[PVA_ST_MOUNT_FR]);
    CHECK(cmd(PVA_C_DIAG, NULL, 0) == PVA_OK && strstr((char *)reply, "FAT16"), "DIAG:\n%s", reply);
    CHECK(cmd(0x7E, NULL, 0) == PVA_E_UNKNOWN, "unknown command refused");

    printf("[folders]\n");
    CHECK(cmd_str(PVA_C_DIR_OPEN, "/") == PVA_OK, "open root");
    uint16_t cnt = (uint16_t)(reply[0] | (reply[1] << 8));
    CHECK(!strcmp((char *)reply + 2, "/"), "path '/'");
    int n = list_dir(cnt);
    CHECK(n == 3 && !strcmp(ents[0].name, "Internet radio") && !strcmp(ents[1].name, "MUSIC") && !strcmp(ents[2].name, "SYMBOS"),
          "root: %d entries, the radio directory first (hidden folder left out)", n);
    CHECK(cmd_str(PVA_C_DIR_OPEN, "\\MUSIC\\") == PVA_OK, "open \\MUSIC\\ (backslashes, trailing slash)");
    cnt = (uint16_t)(reply[0] | (reply[1] << 8));
    n = list_dir(cnt);
    for (int i = 0; i < n; i++) printf("    %d %-40s %u\n", ents[i].type, ents[i].name, (unsigned)ents[i].size);
    CHECK(n == 6 && ents[0].type == PVA_DE_PARENT, "MUSIC: %d entries, '..' first", n);
    int cafe = find(n, "Cafe del Mar");
    CHECK(cafe == 1 && ents[cafe].type == PVA_DE_DIR, "UTF-8 folder shown as 'Cafe del Mar', folders first");
    CHECK(find(n, "readme.txt") < 0 && find(n, "._long.mp3") < 0, "non-audio and ._ files left out");
    CHECK(find(n, "Mix.m3u") > 0 && ents[find(n, "Mix.m3u")].type == PVA_DE_M3U, "M3U listed");
    CHECK(cmd_u16(PVA_C_DIR_ENTER, (uint16_t)cafe) == PVA_OK, "enter the UTF-8 folder by index");
    cnt = (uint16_t)(reply[0] | (reply[1] << 8));
    CHECK(!strcmp((char *)reply + 2, "/MUSIC/Cafe del Mar"), "path shown '%s'", reply + 2);
    n = list_dir(cnt);
    CHECK(n == 5 && find(n, "01 Sine.mp3") == 1 && ents[4].type == PVA_DE_WAV, "4 tracks + '..'");
    CHECK(cmd_u16(PVA_C_DIR_ENTER, 0) == PVA_OK && !strcmp((char *)reply + 2, "/MUSIC"), "'..' goes back to /MUSIC");
    n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
    int many = find(n, "Many");
    CHECK(cmd_u16(PVA_C_DIR_ENTER, (uint16_t)many) == PVA_OK, "enter Many");
    cnt = (uint16_t)(reply[0] | (reply[1] << 8));
    n = list_dir(cnt);
    CHECK(cnt == 121 && n == 121 && !strncmp(ents[1].name, "Track 000", 9) && !strncmp(ents[120].name, "Track 119", 9),
          "121 entries over several DIR_READ pages, sorted ('%s' .. '%s')", ents[1].name, ents[120].name);
    CHECK(strlen(ents[5].name) <= PVA_NAME_MAX, "long names cut to %d", PVA_NAME_MAX);
    CHECK(cmd_str(PVA_C_DIR_OPEN, "/NOPE") == PVA_E_NOTFOUND, "missing folder -> NOTFOUND");
    CHECK(cmd_str(PVA_C_DIR_OPEN, "") == PVA_OK && !strcmp((char *)reply + 2, "/MUSIC/Many"), "empty path reopens the current folder");
    CHECK(cmd_u16(PVA_C_DIR_ENTER, 999) == PVA_E_RANGE, "index out of range");

    printf("[playback in real time, browsing while playing]\n");
    host_audio_unlimited = false;
    cmd_str(PVA_C_DIR_OPEN, "/MUSIC/Caf\xC3\xA9 del Mar");
    n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
    CHECK(cmd_u16(PVA_C_PLAY_INDEX, 1) == PVA_OK, "play '01 Sine.mp3'");
    run_ms(200);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING, "state playing");
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "Sine Cafe Song") && !strcmp(ststr(PVA_ST_ARTIST), "Unit Tester") && !strcmp(ststr(PVA_ST_ALBUM), "PicoVerse Tests"),
          "ID3v2.3 tags: '%s' / '%s' / '%s'", ststr(PVA_ST_TITLE), ststr(PVA_ST_ARTIST), ststr(PVA_ST_ALBUM));
    CHECK(st16(PVA_ST_TOTAL) >= 19 && st16(PVA_ST_TOTAL) <= 21 && st16(PVA_ST_BITRATE) == 128 && st16(PVA_ST_RATE) == 4410 && st[PVA_ST_CHANNELS] == 2,
          "20 s, %u kbit/s, %u Hz, %u ch", st16(PVA_ST_BITRATE), st16(PVA_ST_RATE) * 10, st[PVA_ST_CHANNELS]);
    CHECK(st16(PVA_ST_PL_COUNT) == 4 && st16(PVA_ST_PL_POS) == 0, "play list = the 4 tracks of the folder");
    CHECK(!host_audio_muted, "output unmuted");
    uint32_t f0 = host_audio_frames, t0 = pva_hal_millis();
    for (int s = 0; s < 5; s++)
    {
        run_ms(1000);
        cmd_str(PVA_C_DIR_OPEN, "/MUSIC/Many");                // browse while playing
        list_dir(121);
        cmd_str(PVA_C_DIR_OPEN, "/MUSIC");
    }
    read_status();
    uint32_t played = host_audio_frames - f0, ms = pva_hal_millis() - t0;
    CHECK(host_audio_underruns == 0, "no audio underruns while browsing (%u)", host_audio_underruns);
    uint32_t expect = (uint32_t)((uint64_t)44100u * ms / 1000u);
    CHECK(played + 6000u > expect && played < expect + 6000u, "audio kept pace with real time: %u frames in %u ms", played, ms);
    CHECK(st16(PVA_ST_ELAPSED) + 1u >= ms / 1000u && st16(PVA_ST_ELAPSED) <= ms / 1000u + 1u, "elapsed %u s", st16(PVA_ST_ELAPSED));
    CHECK(st[PVA_ST_VU_L] > 3 && st[PVA_ST_VU_R] > 3, "VU %u/%u", st[PVA_ST_VU_L], st[PVA_ST_VU_R]);

    printf("[pause, volume, seek]\n");
    CHECK(cmd(PVA_C_PAUSE, NULL, 0) == PVA_OK, "pause");
    read_status();
    uint16_t e0 = st16(PVA_ST_ELAPSED);
    run_ms(2000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PAUSED && st16(PVA_ST_ELAPSED) == e0 && host_audio_muted, "paused, time stands still");
    CHECK(cmd(PVA_C_PAUSE, NULL, 0) == PVA_OK, "resume");
    run_ms(500);
    sink_level();
    run_ms(500);
    double full = sink_level();
    CHECK(cmd_u8(PVA_C_VOLUME, 40) == PVA_OK, "volume 40");
    run_ms(500);
    sink_level();
    run_ms(500);
    double low = sink_level();
    CHECK(low < full * 0.1 && low > full * 0.01, "volume 40 is about 30 dB down (%.0f -> %.0f)", full, low);
    cmd_u8(PVA_C_VOLUME, 80);
    CHECK(cmd_u8(PVA_C_MUTE, 2) == PVA_OK, "mute");
    run_ms(500);
    sink_level();
    run_ms(500);
    double mut = sink_level();
    read_status();
    CHECK(mut < full * 0.001 && (st[PVA_ST_FLAGS] & PVA_F_MUTED) && st[PVA_ST_VOLUME] == 80, "muted: silent, the volume stays 80 (%.0f)", mut);
    CHECK(cmd_u8(PVA_C_MUTE, 2) == PVA_OK, "sound on again");
    run_ms(500);
    sink_level();
    run_ms(500);
    double back = sink_level();
    read_status();
    CHECK(back > full * 0.5 && !(st[PVA_ST_FLAGS] & PVA_F_MUTED), "the sound is back (%.0f)", back);
    CHECK(cmd_u16(PVA_C_SEEK, 15) == PVA_OK, "seek to 15 s");
    run_ms(300);
    read_status();
    CHECK(st16(PVA_ST_ELAPSED) >= 15 && st16(PVA_ST_ELAPSED) <= 16, "elapsed %u s after seek", st16(PVA_ST_ELAPSED));

    printf("[end of track -> next, VBR/Xing, mono 22 kHz, ID3v1, WAV]\n");
    uint16_t tid = st16(PVA_ST_TRACK_ID);
    run_ms(6000);
    read_status();
    CHECK(st16(PVA_ST_TRACK_ID) == tid + 1 && st16(PVA_ST_PL_POS) == 1, "advanced to track 2");
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "VBR Four") && st16(PVA_ST_TOTAL) >= 14 && st16(PVA_ST_TOTAL) <= 16 && st16(PVA_ST_RATE) == 4800,
          "VBR: ID3v2.4 '%s', %u s from the Xing header, %u Hz", ststr(PVA_ST_TITLE), st16(PVA_ST_TOTAL), st16(PVA_ST_RATE) * 10);
    CHECK(cmd(PVA_C_NEXT, NULL, 0) == PVA_OK, "next");
    run_ms(300);
    read_status();
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "Mono V1") && st[PVA_ST_CHANNELS] == 1 && st16(PVA_ST_RATE) == 2205, "ID3v1 '%s', mono 22050 Hz", ststr(PVA_ST_TITLE));
    CHECK(cmd(PVA_C_NEXT, NULL, 0) == PVA_OK, "next");
    run_ms(300);
    read_status();
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "04 Tone") && st[PVA_ST_FORMAT] == PVA_DE_WAV && st16(PVA_ST_TOTAL) == 5, "WAV, title from file name '%s'", ststr(PVA_ST_TITLE));
    run_ms(5500);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_STOPPED, "end of the play list -> stopped");
    CHECK(host_audio_underruns == 0, "still no underruns (%u)", host_audio_underruns);

    printf("[prev, repeat, shuffle]\n");
    CHECK(cmd(PVA_C_PREV, NULL, 0) == PVA_OK, "prev from stopped");
    run_ms(200);
    read_status();
    CHECK(st16(PVA_ST_PL_POS) == 2 && st[PVA_ST_STATE] == PVA_STATE_PLAYING, "track 3");
    run_ms(4000);
    CHECK(cmd(PVA_C_PREV, NULL, 0) == PVA_OK, "prev after 4 s restarts the track");
    run_ms(200);
    read_status();
    CHECK(st16(PVA_ST_PL_POS) == 2 && st16(PVA_ST_ELAPSED) <= 1, "same track from the start");
    CHECK(cmd_u8(PVA_C_MODE, PVA_MODE_REPEAT_ONE) == PVA_OK, "repeat one");
    tid = st16(PVA_ST_TRACK_ID);
    run_ms(9000);
    read_status();
    CHECK(st16(PVA_ST_PL_POS) == 2 && st16(PVA_ST_TRACK_ID) == tid + 1, "track repeated");
    CHECK(cmd_u8(PVA_C_MODE, PVA_MODE_SHUFFLE | PVA_MODE_REPEAT_ALL) == PVA_OK, "shuffle + repeat all");
    read_status();
    CHECK(st16(PVA_ST_PL_POS) == 0 && st[PVA_ST_MODE] == (PVA_MODE_SHUFFLE | PVA_MODE_REPEAT_ALL), "current track stays first in the shuffled order");
    int seen[4] = { 0 };
    for (int i = 0; i < 8; i++)
    {
        cmd(PVA_C_NEXT, NULL, 0);
        run_ms(100);
        read_status();
        const char *t = ststr(PVA_ST_TITLE);
        if (!strcmp(t, "Sine Cafe Song")) seen[0]++;
        else if (!strcmp(t, "VBR Four")) seen[1]++;
        else if (!strcmp(t, "Mono V1")) seen[2]++;
        else if (!strcmp(t, "04 Tone")) seen[3]++;
    }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3], "every track came up in shuffle (%d %d %d %d)", seen[0], seen[1], seen[2], seen[3]);
    cmd_u8(PVA_C_MODE, 0);
    CHECK(cmd(PVA_C_STOP, NULL, 0) == PVA_OK, "stop");
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_STOPPED && host_audio_muted && !st[PVA_ST_TITLE], "stopped and muted");

    printf("[M3U play list]\n");
    cmd_str(PVA_C_DIR_OPEN, "/MUSIC");
    n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
    CHECK(cmd_u16(PVA_C_PLAY_INDEX, (uint16_t)find(n, "Mix.m3u")) == PVA_OK, "play Mix.m3u");
    run_ms(200);
    read_status();
    CHECK(st16(PVA_ST_PL_COUNT) == 5 && st[PVA_ST_PL_KIND] == 1, "5 entries (comments and blank line skipped, stream URL kept), %u", st16(PVA_ST_PL_COUNT));
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "Sine Cafe Song"), "relative UTF-8 path plays");
    cmd(PVA_C_NEXT, NULL, 0);
    run_ms(200);
    read_status();
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "Long One") && st16(PVA_ST_PL_POS) == 2, "missing entry skipped, Windows path found by name");
    CHECK(st16(PVA_ST_TOTAL) >= 239 && st16(PVA_ST_TOTAL) <= 241, "CBR 192k length %u s", st16(PVA_ST_TOTAL));
    cmd(PVA_C_NEXT, NULL, 0);
    run_ms(300);
    read_status();
    CHECK((st[PVA_ST_FLAGS] & PVA_F_STREAM) && !strcmp(ststr(PVA_ST_ALBUM), "Plain Radio") && !strcmp(ststr(PVA_ST_TITLE), "Plain Radio"),
          "stream entry in the M3U, named by #EXTINF ('%s')", ststr(PVA_ST_TITLE));
    cmd(PVA_C_NEXT, NULL, 0);
    run_ms(200);
    read_status();
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "04 Tone") && !(st[PVA_ST_FLAGS] & PVA_F_STREAM), "absolute path plays after the stream");
    cmd(PVA_C_STOP, NULL, 0);

    printf("[play by path, errors]\n");
    CHECK(cmd_str(PVA_C_PLAY_PATH, "/MUSIC/long.mp3") == PVA_OK, "PLAY_PATH");
    run_ms(100);
    CHECK(cmd_u16(PVA_C_SEEK, 200) == PVA_OK, "seek in a long CBR file");
    run_ms(300);
    read_status();
    CHECK(st16(PVA_ST_ELAPSED) >= 200 && st16(PVA_ST_ELAPSED) <= 201, "elapsed %u", st16(PVA_ST_ELAPSED));
    CHECK(cmd_str(PVA_C_PLAY_PATH, "/MUSIC/none.mp3") == PVA_E_NOTFOUND, "missing file -> NOTFOUND");
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_STOPPED, "stopped after the failed play");
    CHECK(cmd_str(PVA_C_PLAY_PATH, "MUSIC\\long.mp3") == PVA_OK, "path without leading separator plays from the root");
    CHECK(cmd_str(PVA_C_PLAY_PATH, "A:\\MUSIC\\.\\Cafe del Mar\\..\\long.mp3") == PVA_OK, "drive, '.', '..' and backslashes");
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING, "playing after a DOS style path");
    cmd(PVA_C_STOP, NULL, 0);
    CHECK(cmd_str(PVA_C_PLAY_PATH, "A:\\") == PVA_E_ARGS, "the root is not a file");
    CHECK(cmd(PVA_C_LOG, NULL, 0) == PVA_OK && strstr((char *)reply, "open"), "LOG has the failures:\n%s", reply);

    {
        cmd(PVA_C_STOP, NULL, 0);
        run_ms(200);
        read_status();
        uint8_t g0 = st[PVA_ST_DISP_GEN];
        run_ms(3000);
        read_status();
        CHECK(st[PVA_ST_DISP_GEN] == g0, "stopped: the display generation stays put for 3 s (%u -> %u)", g0, st[PVA_ST_DISP_GEN]);
    }

    printf("[card removed / remounted]\n");
    host_disk_open("/nonexistent/none.img", 0);
    CHECK(cmd(PVA_C_MOUNT, NULL, 0) == PVA_E_NOCARD, "MOUNT without a card -> NOCARD");
    read_status();
    CHECK(!(st[PVA_ST_FLAGS] & PVA_F_MOUNTED) && st[PVA_ST_MOUNT_FR] == FR_NOT_READY, "status shows the mount FRESULT %u", st[PVA_ST_MOUNT_FR]);
    CHECK(cmd_str(PVA_C_DIR_OPEN, "/") == PVA_E_NOCARD, "DIR_OPEN without a card -> NOCARD");
    host_disk_open("pva_test.img", 0);
    CHECK(cmd_str(PVA_C_DIR_OPEN, "/MUSIC") == PVA_OK, "DIR_OPEN mounts the card again");


    printf("[internet radio through the simulated ESP]\n");
    {
        char p2[300];
        snprintf(p2, sizeof p2, "%s/cbr128_v23.mp3", assets);
        CHECK(host_esp_load_audio(p2) == 0, "stream source %s", p2);
    }
    host_audio_unlimited = false;
    host_audio_underruns = 0;
    host_esp_server_bps = 24000;
    {
        static const char a1[] = "http://radio.test/stream.mp3\0";
        CHECK(cmd(PVA_C_PLAY_URL, a1, sizeof a1) == PVA_OK, "PLAY_URL accepted at once (the connection is made in the background)");
    }
    read_status();
    CHECK((st[PVA_ST_FLAGS] & PVA_F_STREAM) && (st[PVA_ST_FLAGS] & PVA_F_BUFFERING) && st[PVA_ST_PL_KIND] == 2 && st[PVA_ST_FORMAT] == PVA_DE_URL,
          "stream flags, buffering, play list kind 2, net state %u", st[PVA_ST_NET_STATE]);
    CHECK(host_esp_owned(), "the engine owns the ESP link");
    uint32_t sf0 = host_audio_frames;
    run_ms(3000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && !(st[PVA_ST_FLAGS] & PVA_F_BUFFERING) && st[PVA_ST_NET_STATE] == PVA_NET_STREAM,
          "playing after the prefill, net state %u, buffer %u%%", st[PVA_ST_NET_STATE], st[PVA_ST_BUF_PCT]);
    CHECK(!strcmp(ststr(PVA_ST_ALBUM), "Test FM") && !strcmp(ststr(PVA_ST_TITLE), "Song A") && !strcmp(ststr(PVA_ST_ARTIST), "Artist One"),
          "station '%s', ICY title split into artist '%s' and title '%s'", ststr(PVA_ST_ALBUM), ststr(PVA_ST_ARTIST), ststr(PVA_ST_TITLE));
    CHECK(strstr(host_esp_last_get, "GET /stream.mp3 HTTP/1.0\r\nHost: radio.test\r\n") && strstr(host_esp_last_get, "Icy-MetaData: 1"),
          "HTTP/1.0 request with Host and Icy-MetaData");
    CHECK(st16(PVA_ST_BITRATE) == 128 && st16(PVA_ST_RATE) > 0 && st16(PVA_ST_TOTAL) == 0 && !(st[PVA_ST_FLAGS] & PVA_F_SEEKABLE),
          "128 kbit/s, %u Hz, no length, not seekable", st16(PVA_ST_RATE) * 10);
    CHECK(st16(PVA_ST_BUF_KB) == 72, "72 KB stream buffer (the Nextor modes: 24 KB of the stream RAM for the news)");
    uint32_t rate_hz = st16(PVA_ST_RATE) * 10u;
    bool seen_b = false;
    uint32_t f1 = host_audio_frames;
    for (int i = 0; i < 20; i++)
    {
        run_ms(500);
        read_status();
        if (!strcmp(ststr(PVA_ST_TITLE), "Song B")) seen_b = true;
    }
    uint32_t splayed = host_audio_frames - f1;
    CHECK(splayed > rate_hz * 95u / 10u && splayed < rate_hz * 105u / 10u, "10 s of audio in 10 s (%u frames)", splayed);
    CHECK(host_audio_underruns == 0 && st16(PVA_ST_UNDERRUNS) == 0, "no underruns (audio %u, stream %u)", host_audio_underruns, st16(PVA_ST_UNDERRUNS));
    CHECK(seen_b, "the stream title changes (Song B)");
    CHECK(st16(PVA_ST_NET_RATE) >= 150 && st16(PVA_ST_NET_RATE) <= 260, "receive rate %u.%u KB/s", st16(PVA_ST_NET_RATE) / 10, st16(PVA_ST_NET_RATE) % 10);
    CHECK(cmd(PVA_C_DIAG, NULL, 0) == PVA_OK && strstr((char *)reply, "Stream: radio.test") && strstr((char *)reply, "KB/s"), "DIAG has the stream:\n%s", reply);
    (void)sf0;

    {
        // the display generation: about once a second (elapsed), not with every buffer wobble
        read_status();
        uint8_t g0 = st[PVA_ST_DISP_GEN];
        int changes = 0;
        uint8_t last = g0;
        for (int i = 0; i < 100; i++) { run_ms(50); read_status(); if (st[PVA_ST_DISP_GEN] != last) { changes++; last = st[PVA_ST_DISP_GEN]; } }
        CHECK(changes >= 4 && changes <= 8, "display generation changed %d times in 5 s of streaming (buffer %u%%, shown %u%%)",
              changes, st[PVA_ST_BUF_PCT], st[PVA_ST_DISP_BUF]);
    }

    printf("[pause a stream, the buffer fills, no false reconnect]\n");
    cmd(PVA_C_PAUSE, NULL, 0);
    run_ms(15000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PAUSED && st[PVA_ST_BUF_PCT] >= 95 && st16(PVA_ST_RECONNECTS) == 0,
          "paused 15 s: buffer %u%%, reconnects %u", st[PVA_ST_BUF_PCT], st16(PVA_ST_RECONNECTS));
    host_audio_underruns = 0;
    cmd(PVA_C_PAUSE, NULL, 0);
    run_ms(3000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && host_audio_underruns == 0, "resumed from the buffer");
    CHECK(cmd(PVA_C_SEEK, "\x10\x00", 2) == PVA_E_STATE, "seeking a stream is refused");
    cmd(PVA_C_STOP, NULL, 0);
    run_ms(50);
    CHECK(host_esp_open_conns() == 0 && !host_esp_owned(), "STOP closes the connection and gives the ESP back");

    printf("[connection drops: the buffer covers the reconnect]\n");
    host_esp_drop_after = 150000;
    host_audio_underruns = 0;
    {
        static const char a2[] = "http://radio.test/stream.mp3\0My Station\0";
        CHECK(cmd(PVA_C_PLAY_URL, a2, sizeof a2) == PVA_OK, "PLAY_URL with a station name");
    }
    run_ms(16000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st16(PVA_ST_RECONNECTS) >= 1 && host_audio_underruns == 0 && st16(PVA_ST_UNDERRUNS) == 0,
          "still playing after %u reconnect(s), underruns %u/%u", st16(PVA_ST_RECONNECTS), host_audio_underruns, st16(PVA_ST_UNDERRUNS));
    CHECK(!strcmp(ststr(PVA_ST_ALBUM), "My Station"), "the given station name wins over icy-name ('%s')", ststr(PVA_ST_ALBUM));
    host_esp_drop_after = 0;
    cmd(PVA_C_STOP, NULL, 0);

    printf("[server stalls longer than the buffer]\n");
    host_esp_stall_at = 60000;
    host_esp_stall_ms = 14000;
    {
        static const char a3[] = "http://radio.test/plain\0";
        cmd(PVA_C_PLAY_URL, a3, sizeof a3);
    }
    run_ms(30000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st16(PVA_ST_UNDERRUNS) >= 1 && st16(PVA_ST_RECONNECTS) >= 1 && !(st[PVA_ST_FLAGS] & PVA_F_BUFFERING),
          "no data for 14 s: watchdog reconnect, refill, playing again (underruns %u, reconnects %u)", st16(PVA_ST_UNDERRUNS), st16(PVA_ST_RECONNECTS));
    host_esp_stall_at = 0;
    cmd(PVA_C_STOP, NULL, 0);

    printf("[server slower than the bitrate]\n");
    host_esp_server_bps = 12000;
    {
        static const char a4[] = "http://radio.test/plain\0";
        cmd(PVA_C_PLAY_URL, a4, sizeof a4);
    }
    run_ms(20000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st16(PVA_ST_UNDERRUNS) >= 1, "keeps going with refills (%u underruns)", st16(PVA_ST_UNDERRUNS));
    host_esp_server_bps = 24000;
    cmd(PVA_C_STOP, NULL, 0);

    printf("[redirects, .pls play list, HTTPS]\n");
    {
        static const char a5[] = "http://radio.test/redirect\0";
        cmd(PVA_C_PLAY_URL, a5, sizeof a5);
    }
    run_ms(2500);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st[PVA_ST_NET_STATE] == PVA_NET_STREAM && st16(PVA_ST_HTTP) == 200, "302 to radio2.test:8000 followed");
    CHECK(cmd(PVA_C_DIAG, NULL, 0) == PVA_OK && strstr((char *)reply, "Stream: radio2.test"), "now on radio2.test");
    {
        static const char a6[] = "http://radio.test/relative\0";
        cmd(PVA_C_PLAY_URL, a6, sizeof a6);
    }
    run_ms(2500);
    read_status();
    CHECK(st[PVA_ST_NET_STATE] == PVA_NET_STREAM && !strcmp(ststr(PVA_ST_ALBUM), "Test FM"), "relative Location followed");
    CHECK(cmd_str(PVA_C_PLAY_PATH, "http://radio.test/list.pls") == PVA_OK, "PLAY_PATH takes a URL too");
    run_ms(3000);
    read_status();
    CHECK(st[PVA_ST_NET_STATE] == PVA_NET_STREAM && !strcmp(ststr(PVA_ST_ALBUM), "Test FM") && !(st[PVA_ST_FLAGS] & PVA_F_TLS), ".pls play list opened ('%s'), not TLS", ststr(PVA_ST_ALBUM));
    uint32_t tls0 = host_esp_tls_opens;
    {
        static const char a7[] = "https://secure.test/stream.mp3\0";
        cmd(PVA_C_PLAY_URL, a7, sizeof a7);
    }
    run_ms(5000);
    read_status();
    CHECK(host_esp_tls_opens == tls0 + 1 && !strcmp(host_esp_last_sni, "secure.test"), "TLS open with the host name for SNI ('%s')", host_esp_last_sni);
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && !strcmp(ststr(PVA_ST_ALBUM), "Secure Radio") && (st[PVA_ST_FLAGS] & PVA_F_TLS), "HTTPS stream plays, flagged TLS");
    CHECK(cmd(PVA_C_DIAG, NULL, 0) == PVA_OK && strstr((char *)reply, "(TLS)"), "DIAG shows TLS and the open time");
    cmd(PVA_C_STOP, NULL, 0);

    printf("[next station while a stream plays]\n");
    {
        static const char an[] = "http://radio.test/stream.mp3\0";
        cmd(PVA_C_PLAY_URL, an, sizeof an);
        run_ms(3000);
        int r = cmd(PVA_C_NEXT, NULL, 0);
        CHECK(r == PVA_OK, "NEXT on a playing stream answers (%d)", r);
        run_ms(3000);
        read_status();
        CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st[PVA_ST_NET_STATE] == PVA_NET_STREAM && st16(PVA_ST_TRACK_ID) > 0,
              "the station started again (net state %u)", st[PVA_ST_NET_STATE]);
        cmd(PVA_C_STOP, NULL, 0);
        CHECK(cmd_str(PVA_C_PLAY_PATH, "/SYMBOS/RADIO.M3U") == PVA_OK, "M3U with two stations");
        run_ms(4000);
        read_status();
        CHECK(!strcmp(ststr(PVA_ST_ALBUM), "Test FM") && st[PVA_ST_NET_STATE] == PVA_NET_STREAM, "first station '%s'", ststr(PVA_ST_ALBUM));
        r = cmd(PVA_C_NEXT, NULL, 0);
        CHECK(r == PVA_OK, "NEXT to the HTTPS station answers (%d)", r);
        for (int i = 0; i < 8; i++) { run_ms(1000); read_status(); printf("    t+%ds state %u net %u buf %u%% album '%s' title '%s'\n", i + 1, st[PVA_ST_STATE], st[PVA_ST_NET_STATE], st[PVA_ST_BUF_PCT], ststr(PVA_ST_ALBUM), ststr(PVA_ST_TITLE)); }
        CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && !strcmp(ststr(PVA_ST_ALBUM), "Secure Radio") && st[PVA_ST_NET_STATE] == PVA_NET_STREAM,
              "second station plays ('%s')", ststr(PVA_ST_ALBUM));
        cmd(PVA_C_STOP, NULL, 0);
    }

    printf("[leftover ESP connections from before an MSX reset]\n");
    host_esp_leftover(true);                    // e.g. an HTTPS download of a SymbOS program
    host_esp_leftover(false);
    {
        static const char al[] = "https://secure.test/stream.mp3\0";
        cmd(PVA_C_PLAY_URL, al, sizeof al);
    }
    run_ms(6000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st[PVA_ST_NET_STATE] == PVA_NET_STREAM, "TLS refused (rc 9), leftovers aborted, the stream plays");
    CHECK(cmd(PVA_C_LOG, NULL, 0) == PVA_OK && strstr((char *)reply, "closing old connections"), "logged");
    cmd(PVA_C_STOP, NULL, 0);
    run_ms(100);
    CHECK(host_esp_open_conns() == 0, "nothing left open in the ESP");

    printf("[far server: the ESP's TCP window limits the rate]\n");
    host_esp_rtt_ms = 183;                      // 2144 bytes per 183 ms = 11.7 KB/s < 16 KB/s
    {
        static const char af[] = "http://radio.test/plain\0";
        cmd(PVA_C_PLAY_URL, af, sizeof af);
    }
    run_ms(30000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st16(PVA_ST_UNDERRUNS) >= 1 && st16(PVA_ST_NET_RATE) < 125,
          "plays with refills at %u.%u KB/s (%u underruns)", st16(PVA_ST_NET_RATE) / 10, st16(PVA_ST_NET_RATE) % 10, st16(PVA_ST_UNDERRUNS));
    CHECK(cmd(PVA_C_LOG, NULL, 0) == PVA_OK && strstr((char *)reply, "got 11.7 KB/s, needs 16.0"), "the log says why:\n%s", reply);
    CHECK(cmd(PVA_C_DIAG, NULL, 0) == PVA_OK && strstr((char *)reply, "11.7 KB/s (needs 16.0)"), "DIAG says it too:\n%s", reply);
    host_esp_rtt_ms = 0;
    cmd(PVA_C_STOP, NULL, 0);

    printf("[stream errors]\n");
    struct { const char *u; uint8_t nerr; uint16_t http; const char *what; } errs[] = {
        { "http://radio.test/aac", PVA_NERR_FORMAT, 200, "AAC refused" },
        { "http://radio.test/missing", PVA_NERR_HTTP, 404, "404" },
        { "http://nowhere.test/x.mp3", PVA_NERR_DNS, 0, "unknown host" },
        { "http://radio2.test:9999/live", PVA_NERR_CONNECT, 0, "connection refused" },
    };
    for (unsigned i = 0; i < sizeof errs / sizeof errs[0]; i++)
    {
        char a[300];
        size_t l = strlen(errs[i].u);
        memcpy(a, errs[i].u, l + 1);
        a[l + 1] = 0;
        cmd(PVA_C_PLAY_URL, a, l + 2);
        run_ms(1500);
        read_status();
        CHECK(st[PVA_ST_STATE] == PVA_STATE_STOPPED && st[PVA_ST_ERROR] == PVA_E_NET && st[PVA_ST_NET_ERR] == errs[i].nerr &&
              (!errs[i].http || st16(PVA_ST_HTTP) == errs[i].http) && !host_esp_owned(),
              "%s: stopped, net error %u, HTTP %u", errs[i].what, st[PVA_ST_NET_ERR], st16(PVA_ST_HTTP));
    }
    host_esp_wifi_state = 1;
    {
        static const char a8[] = "http://radio.test/plain\0";
        cmd(PVA_C_PLAY_URL, a8, sizeof a8);
    }
    run_ms(10000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_STOPPED && st[PVA_ST_NET_ERR] == PVA_NERR_NOWIFI, "WiFi not connected: gives up after a while");
    host_esp_wifi_state = 2;
    host_esp_present = false;
    {
        static const char a9[] = "http://radio.test/plain\0";
        CHECK(cmd(PVA_C_PLAY_URL, a9, sizeof a9) == PVA_E_NOWIFI, "no ESP (image without -w): NOWIFI");
    }
    host_esp_present = true;
    CHECK(cmd_str(PVA_C_PLAY_URL, "ftp://radio.test/x") == PVA_E_ARGS, "only http and https");

    printf("[equalizer]\n");
    {
        // response at a few frequencies, straight through pva_eq_process
        #define EQ_N 8192
        static int16_t buf[EQ_N * 2];
        double resp[5];
        const double fq[5] = { 60, 250, 1000, 4000, 12000 };
        struct { int8_t g[5]; double want[5]; const char *what; } cases[] = {
            { { 12, 0, 0, 0, 0 }, { 0, -10.5, -12, -12, -12 }, "+12 dB at 60 Hz (the preamp takes 12 dB off)" },
            { { 0, 0, -12, 0, 0 }, { 0, -0.6, -12, -0.6, 0 }, "-12 dB at 1 kHz" },
            { { 0, 0, 0, 0, 6 }, { -6, -6, -6, -5.5, 0 }, "+6 dB at 12 kHz" },
            { { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, "flat" },
        };
        for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++)
        {
            pva_eq_set(cases[c].g);
            bool ok = true;
            for (int k = 0; k < 5; k++)
            {
                pva_eq_reset();
                double in2 = 0, out2 = 0;
                for (int n = 0; n < EQ_N; n++)
                    buf[2 * n] = buf[2 * n + 1] = (int16_t)(8000.0 * sin(2 * M_PI * fq[k] * n / 44100.0));
                for (int n = 0; n < EQ_N; n++) in2 += (double)buf[2 * n] * buf[2 * n];
                pva_eq_process(buf, EQ_N, 44100);
                for (int n = EQ_N / 2; n < EQ_N; n++) out2 += (double)buf[2 * n] * buf[2 * n];
                resp[k] = 10 * log10((out2 * 2) / in2);
                if (fabs(resp[k] - cases[c].want[k]) > 1.5) ok = false;
            }
            CHECK(ok, "%s: %.1f %.1f %.1f %.1f %.1f dB at 60/250/1k/4k/12k", cases[c].what, resp[0], resp[1], resp[2], resp[3], resp[4]);
        }
        {
            // flat is bit-exact; a 22.05 kHz stream with the 12 kHz band set stays sane
            int8_t z[5] = { 0 }, hi[5] = { 0, 0, 0, 0, 12 };
            pva_eq_set(z);
            for (int n = 0; n < 256; n++) buf[n] = (int16_t)(n * 97);
            pva_eq_process(buf, 128, 44100);
            bool same = true;
            for (int n = 0; n < 256; n++) if (buf[n] != (int16_t)(n * 97)) same = false;
            CHECK(same, "flat: the samples are untouched");
            pva_eq_set(hi);
            bool sane = true;
            for (int n = 0; n < EQ_N; n++) buf[2 * n] = buf[2 * n + 1] = (int16_t)(20000.0 * sin(2 * M_PI * 9000.0 * n / 22050.0));
            pva_eq_process(buf, EQ_N, 22050);
            for (int n = 0; n < EQ_N * 2; n++) if (buf[n] == 32767 || buf[n] == -32768) sane = false;
            CHECK(sane, "22.05 kHz with +12 dB at the top: no clipping, no blow-up");
            pva_eq_set(z);
        }
        // through the engine: the command, the status, and the level of a playing file
        cmd_str(PVA_C_DIR_OPEN, "/MUSIC/Caf\xC3\xA9 del Mar");
        cmd_u16(PVA_C_PLAY_INDEX, 1);
        run_ms(1500);
        sink_level();
        run_ms(2000);
        double flat = sink_level();
        int8_t cut[5] = { -12, -12, -12, -12, -12 };
        CHECK(cmd(PVA_C_EQ, cut, 5) == PVA_OK, "EQ command");
        read_status();
        CHECK((int8_t)st[PVA_ST_EQ] == -12 && (int8_t)st[PVA_ST_EQ + 4] == -12, "status shows the gains");
        run_ms(500);
        sink_level();
        run_ms(2000);
        double low = sink_level();
        int8_t big[5] = { 40, -40, 0, 0, 0 };
        cmd(PVA_C_EQ, big, 5);
        read_status();
        CHECK(low < flat * 0.5 && low > 0, "all bands -12 dB: level %.0f -> %.0f", flat, low);
        CHECK((int8_t)st[PVA_ST_EQ] == 12 && (int8_t)st[PVA_ST_EQ + 1] == -12, "gains are kept within +-12 dB");
        int8_t z[5] = { 0 };
        cmd(PVA_C_EQ, z, 5);
        printf("[balance]\n");
        struct { uint8_t b; double l, r; } bal[] = { { 50, 1, 1 }, { 0, 1, 0 }, { 100, 0, 1 }, { 75, 0.5, 1 }, { 25, 1, 0.5 } };
        double ref_l = 0, ref_r = 0;
        for (unsigned k = 0; k < sizeof bal / sizeof bal[0]; k++)
        {
            CHECK(cmd_u8(PVA_C_BALANCE, bal[k].b) == PVA_OK, "BALANCE %u", bal[k].b);
            run_ms(300);
            sink_l = sink_r = 0;
            run_ms(1500);
            double l = (double)sink_l, r = (double)sink_r;
            if (k == 0) { ref_l = l; ref_r = r; }
            read_status();
            CHECK(st[PVA_ST_BALANCE] == bal[k].b && fabs(l / ref_l - bal[k].l) < 0.05 && fabs(r / ref_r - bal[k].r) < 0.05,
                  "balance %u: left %.2f, right %.2f", bal[k].b, l / ref_l, r / ref_r);
        }
        cmd_u8(PVA_C_BALANCE, 50);
        cmd(PVA_C_STOP, NULL, 0);
    }

    printf("[visualisation]\n");
    {
        // straight through pva_viz: a tone lights its band, silence falls back
        static int16_t tone[2 * 1024];
        uint8_t lv[PVA_VIZ_BANDS], pk[PVA_VIZ_BANDS], vu[2];
        const double fr[3] = { 100.0, 1000.0, 8000.0 };
        int prev_band = -1;
        bool ok_all = true;
        for (int k = 0; k < 3; k++)
        {
            for (int n = 0; n < 1024; n++)
            {
                int16_t v = (int16_t)(16000.0 * sin(2 * M_PI * fr[k] * n / 44100.0));
                tone[2 * n] = v;
                tone[2 * n + 1] = (int16_t)(v / 4);
            }
            for (int u = 0; u < 15; u++)
            {
                pva_viz_feed(tone, 1024, 44100);
                host_advance_ms(21);
                pva_viz_poll(true);
            }
            pva_viz_get(lv, pk, vu);
            int best = 0;
            for (int b = 1; b < PVA_VIZ_BANDS; b++) if (lv[b] > lv[best]) best = b;
            int far_lv = lv[(best + 12) % PVA_VIZ_BANDS];
            bool ok = best > prev_band && lv[best] >= 26 && far_lv <= lv[best] - 10 && vu[0] > vu[1];
            ok_all &= ok;
            CHECK(ok, "%.0f Hz: band %d at %u (far band %d), VU %u/%u", fr[k], best, lv[best], far_lv, vu[0], vu[1]);
            prev_band = best;
        }
        CHECK(rd(PVA_WIN_BASE + PVA_R_VIZ + PVA_VIZ_COUNT) == PVA_VIZ_BANDS && !(rd(PVA_WIN_BASE + PVA_R_VIZ + PVA_VIZ_SEQ) & 1) &&
              rd(PVA_WIN_BASE + PVA_R_VIZ + PVA_VIZ_LEVEL + prev_band) == lv[prev_band], "in the window at C00h");
        int8_t sc[PVA_VIZ_WAVE];
        int mx = 0;
        for (int i = 0; i < PVA_VIZ_WAVE; i++) { sc[i] = (int8_t)rd((uint16_t)(PVA_WIN_BASE + PVA_R_VIZ + PVA_VIZ_SCOPE + i)); if (abs(sc[i]) > mx) mx = abs(sc[i]); }
        CHECK(mx > 20 && mx < 60, "scope samples (largest %d)", mx);
        uint8_t top = lv[prev_band];
        for (int u = 0; u < 5; u++) { host_advance_ms(21); pva_viz_poll(false); }
        pva_viz_get(lv, pk, vu);
        CHECK(lv[prev_band] == top - 5 && pk[prev_band] == top && vu[0] < 28, "stopped: the bar falls a step each update (%u), the peak holds (%u)",
              lv[prev_band], pk[prev_band]);
        for (int u = 0; u < 60; u++) { host_advance_ms(21); pva_viz_poll(false); }
        pva_viz_get(lv, pk, vu);
        CHECK(lv[prev_band] == 0 && pk[prev_band] == 0, "then all at 0");
        // a playing file moves the bars through the engine itself
        cmd_str(PVA_C_DIR_OPEN, "/MUSIC/Caf\xC3\xA9 del Mar");
        cmd_u16(PVA_C_PLAY_INDEX, 1);
        run_ms(2000);
        int sum = 0;
        for (int b = 0; b < PVA_VIZ_BANDS; b++) sum += rd((uint16_t)(PVA_WIN_BASE + PVA_R_VIZ + PVA_VIZ_LEVEL + b));
        uint8_t s0 = rd(PVA_WIN_BASE + PVA_R_VIZ + PVA_VIZ_SEQ);
        run_ms(200);
        uint8_t s1 = rd(PVA_WIN_BASE + PVA_R_VIZ + PVA_VIZ_SEQ);
        uint8_t vul = rd(PVA_WIN_BASE + PVA_R_VIZ + PVA_VIZ_VU), vur = rd(PVA_WIN_BASE + PVA_R_VIZ + PVA_VIZ_VU + 1);
        CHECK(vul > 15 && vur > 15, "VU in the window while playing: %u %u", vul, vur);
        CHECK(sum > 0 && (uint8_t)(s1 - s0) >= 10 && (uint8_t)(s1 - s0) <= 22, "playing: bars up (sum %d), %u updates in 200 ms", sum, (uint8_t)(s1 - s0) / 2);
        cmd(PVA_C_STOP, NULL, 0);
    }

    printf("[internet radio directory]\n");
    {
        // wait for a list: DIR_ENTER said BUSY, DIR_STATE says when it is there
        #define WAIT_LIST(ms) do { for (int w = 0; w < (ms) / 50; w++) { run_ms(50); read_status(); if (st[PVA_ST_DIR_STATE] != PVA_DIRS_LOADING) break; } } while (0)
        CHECK(cmd_str(PVA_C_DIR_OPEN, "/") == PVA_OK, "open root");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(n > 0 && !strcmp(ents[0].name, "Internet radio") && ents[0].type == PVA_DE_DIR, "root starts with 'Internet radio'");
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 0) == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio"), "enter it: '%s'", reply + 2);
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        for (int i = 0; i < n; i++) printf("    %d %s\n", ents[i].type, ents[i].name);
        CHECK(n == 8 && ents[0].type == PVA_DE_PARENT && find(n, "Favourites") == 1 && find(n, "By country") == 5 && find(n, "By genre") == 6 && find(n, "By decade") == 7,
              "home: '..', 3 rankings, country, genre, decade");
        uint32_t req0 = host_esp_requests;
        read_status();
        uint8_t g0 = st[PVA_ST_DIR_GEN];
        uint32_t t0 = pva_hal_millis();
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 5) == PVA_E_BUSY, "By country: BUSY (fetching)");
        read_status();
        CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_LOADING && st[PVA_ST_DIR_GEN] == g0, "DIR_STATE loading, the listing stays");
        WAIT_LIST(10000);
        CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_DONE && st[PVA_ST_DIR_GEN] != g0, "list in after %lu ms, DIR_GEN changed",
              (unsigned long)(pva_hal_millis() - t0));
        CHECK(strstr(host_esp_last_get, "GET /json/countries?hidebroken=true&order=name HTTP/1.0") &&
              strstr(host_esp_last_get, "User-Agent: PicoPlay/") && strstr(host_esp_last_get, "Host: all.api.radio-browser.info"),
              "request:\n%s", host_esp_last_get);
        CHECK(cmd_str(PVA_C_DIR_OPEN, "") == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio/By country"), "reopen: '%s'", reply + 2);
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        for (int i = 0; i < n; i++) printf("    %d %-30s %u\n", ents[i].type, ents[i].name, (unsigned)ents[i].size);
        CHECK(n == 5 && !strcmp(ents[1].name, "Cote D'ivoire") && ents[2].size == 3210 && !strcmp(ents[4].name, "The Netherlands"),
              "countries (UTF-8 escape folded, empty one left out, station counts)");
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 4) == PVA_E_BUSY, "The Netherlands: BUSY");
        WAIT_LIST(10000);
        CHECK(strstr(host_esp_last_api, "country=The%20Netherlands&countryExact=true") && strstr(host_esp_last_api, "codec=MP3") &&
              strstr(host_esp_last_api, "hidebroken=true"), "query %s", host_esp_last_api);
        CHECK(cmd_str(PVA_C_DIR_OPEN, "") == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio/By country/The Netherlands"), "'%s'", reply + 2);
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        for (int i = 0; i < n; i++) printf("    %d %-30s %u\n", ents[i].type, ents[i].name, (unsigned)ents[i].size);
        CHECK(n == 3 && ents[1].type == PVA_DE_URL && !strcmp(ents[1].name, "Test FM") && ents[1].size == 128 &&
              !strcmp(ents[2].name, "Plain \"Quoted\" Radio"), "2 stations (escapes, padding taken off), bitrate as size");
        uint32_t req1 = host_esp_requests;
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 0) == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio/By country") && host_esp_requests == req1,
              "'..': the countries again, without fetching");
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 0) == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio"), "'..': home");
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 6) == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio/By genre"), "By genre (no fetch)");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        int jazz = find(n, "Jazz");
        CHECK(n > 40 && jazz > 0 && find(n, "Hip hop") > 0, "%d genres", n - 1);
        CHECK(cmd_u16(PVA_C_DIR_ENTER, (uint16_t)jazz) == PVA_E_BUSY, "Jazz: BUSY");
        WAIT_LIST(10000);
        CHECK(strstr(host_esp_last_api, "tag=jazz&order=clickcount&reverse=true"), "query %s", host_esp_last_api);
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        for (int i = 0; i < n; i++) printf("    %d %-30s %u\n", ents[i].type, ents[i].name, (unsigned)ents[i].size);
        CHECK(n == 4 && find(n, "Radio Two Live") == 1 && find(n, "Secure Jazz") == 3 && ents[3].size == 192 &&
              find(n, "Cafe AAC") < 0 && find(n, "Broken FM") < 0, "MP3 stations only, broken ones left out");
        CHECK(req0 < host_esp_requests && host_esp_open_conns() == 0 && !host_esp_owned(), "no connection left, link handed back");

        printf("[playing from the directory]\n");
        uint32_t c0 = host_esp_clicks;
        CHECK(cmd_u16(PVA_C_PLAY_INDEX, 1) == PVA_OK, "play 'Radio Two Live'");
        run_ms(4000);
        read_status();
        CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && (st[PVA_ST_FLAGS] & PVA_F_STREAM) && st16(PVA_ST_PL_COUNT) == 3 && st16(PVA_ST_PL_POS) == 0 &&
              st[PVA_ST_PL_KIND] == 2 && !strcmp(ststr(PVA_ST_ALBUM), "Radio Two Live"),
              "plays, play list = the 3 stations, station '%s'", ststr(PVA_ST_ALBUM));
        CHECK(host_esp_clicks == c0 + 1 && strstr(host_esp_last_click, "52543be04c01"), "click counted (%s)", host_esp_last_click);
        CHECK(cmd(PVA_C_NEXT, NULL, 0) == PVA_OK, "next station");
        run_ms(4000);
        read_status();
        CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st16(PVA_ST_PL_POS) == 1 && !strcmp(ststr(PVA_ST_ALBUM), "Plain \"Quoted\" Radio") &&
              host_esp_clicks == c0 + 2, "'%s', clicked too", ststr(PVA_ST_ALBUM));
        run_ms(3000);
        read_status();
        uint16_t u0 = st16(PVA_ST_UNDERRUNS);
        // browse while the stream plays
        cmd_u16(PVA_C_DIR_ENTER, 0);                // genres
        cmd_u16(PVA_C_DIR_ENTER, 0);                // home
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 7) == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio/By decade"), "By decade");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        int e80 = find(n, "1980s");
        CHECK(e80 > 0 && cmd_u16(PVA_C_DIR_ENTER, (uint16_t)e80) == PVA_E_BUSY, "1980s while a stream plays: BUSY");
        WAIT_LIST(10000);
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_DONE && n == 3 && find(n, "Test FM") == 1 && strstr(host_esp_last_api, "tag=80s"), "80s: %d stations", n - 1);
        run_ms(3000);
        read_status();
        CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st16(PVA_ST_UNDERRUNS) == u0 && host_esp_open_conns() == 1,
              "the stream played on (underruns %u -> %u)", u0, st16(PVA_ST_UNDERRUNS));
        cmd(PVA_C_STOP, NULL, 0);
        run_ms(500);

        printf("[search]\n");
        CHECK(cmd_str(PVA_C_DIR_SEARCH, "  jAzZ ") == PVA_E_BUSY, "search 'jAzZ': BUSY");
        WAIT_LIST(10000);
        CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_DONE && strstr(host_esp_last_api, "name=jAzZ&"), "asked '%s'", host_esp_last_api);
        cmd_str(PVA_C_DIR_OPEN, "");
        char sp[80];
        snprintf(sp, sizeof sp, "%s", (char *)reply + 2);
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(!strcmp(sp, "/Internet radio/Search/jAzZ") && n == 2 && find(n, "Secure Jazz") == 1,
              "'%s': %d station(s)", sp, n - 1);
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 0) == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio"), ".. goes home");
        CHECK(cmd_str(PVA_C_DIR_SEARCH, "caf\xc3\xa9 aac") == PVA_E_BUSY, "search with a space and UTF-8");
        WAIT_LIST(10000);
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(strstr(host_esp_last_api, "name=caf%C3%A9%20aac&") && n >= 1, "encoded: '%s' (%d)", host_esp_last_api, n - 1);
        CHECK(cmd_str(PVA_C_DIR_SEARCH, "zzzz") == PVA_E_BUSY, "search 'zzzz'");
        WAIT_LIST(10000);
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_DONE && n == 1, "nothing found: only '..' (%d)", n);
        CHECK(cmd_str(PVA_C_DIR_SEARCH, "   ") == PVA_E_ARGS, "an empty search: ARGS");

        printf("[favourites by the engine]\n");
        CHECK(cmd_str(PVA_C_DIR_SEARCH, "jazz") == PVA_E_BUSY, "search 'jazz'");
        WAIT_LIST(10000);
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        int sj = find(n, "Secure Jazz");
        CHECK(sj > 0 && cmd_u16(PVA_C_FAV_ADD, (uint16_t)sj) == PVA_E_RDONLY, "FAV_ADD with Nextor's card: RDONLY");
        pva_allow_card_writes(true);
        f_unlink("0:/RADIOFAV.M3U");
        CHECK(cmd_u16(PVA_C_FAV_ADD, (uint16_t)sj) == PVA_OK && !strcmp((char *)reply, "Secure Jazz"), "FAV_ADD: '%s'", reply);
        CHECK(cmd_u16(PVA_C_FAV_ADD, (uint16_t)sj) == PVA_E_EXISTS, "again: EXISTS");
        {
            FIL f; char b[600]; UINT got = 0;
            CHECK(f_open(&f, "0:/RADIOFAV.M3U", FA_READ) == FR_OK, "RADIOFAV.M3U written");
            f_read(&f, b, sizeof b - 1, &got); b[got] = 0; f_close(&f);
            CHECK(!strcmp(b, "#EXTM3U\r\n#EXTINF:-1,Secure Jazz\r\nhttps://secure.test/stream.mp3\r\n"), "the file: '%s'", b);
        }
        CHECK(cmd_u16(PVA_C_FAV_ADD, 0xFFFF) == PVA_E_STATE, "FAV_ADD of the stream when none plays: STATE");
        cmd_str(PVA_C_DIR_OPEN, "/Internet radio");
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 1) == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio/Favourites"), "Favourites");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        int fj = find(n, "Secure Jazz");
        CHECK(n == 2 && fj == 1, "the favourite is in the folder (%d)", n - 1);
        {
            uint8_t dr[4] = { 1, 0, 1, PVA_DR_FLAGS };
            cmd(PVA_C_DIR_READ, dr, 4);
            bool tls = reply[0] == PVA_DE_URL && (reply[4] & 0x80);
            dr[3] = 0;
            cmd(PVA_C_DIR_READ, dr, 4);
            CHECK(tls && !(reply[4] & 0x80), "DIR_READ with A5h: the https station is flagged (not without it)");
        }
        CHECK(cmd_u16(PVA_C_FAV_DEL, (uint16_t)fj) == PVA_OK, "FAV_DEL");
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(n == 1, "the folder has only '..' (%d)", n);
        {
            FIL f; char b[200]; UINT got = 0;
            f_open(&f, "0:/RADIOFAV.M3U", FA_READ); f_read(&f, b, sizeof b - 1, &got); b[got] = 0; f_close(&f);
            CHECK(!strcmp(b, "#EXTM3U\r\n"), "the file: '%s'", b);
        }
        {
            // a file of PicoPlay's with no line end at its end; remove the middle one
            FIL f; UINT put = 0; char b[400]; UINT got = 0;
            const char *t = "#EXTM3U\n#EXTINF:-1,One\nhttp://one.test/a\n#EXTINF:-1,Two\nhttp://two.test/b\nhttp://three.test/c";
            f_open(&f, "0:/RADIOFAV.M3U", FA_CREATE_ALWAYS | FA_WRITE); f_write(&f, t, (UINT)strlen(t), &put); f_close(&f);
            cmd_str(PVA_C_DIR_OPEN, "/Internet radio");
            cmd_u16(PVA_C_DIR_ENTER, 1);
            n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
            int f2 = find(n, "Two");
            CHECK(n == 4 && f2 == 2 && cmd_u16(PVA_C_FAV_DEL, (uint16_t)f2) == PVA_OK, "FAV_DEL of the middle one (%d)", n - 1);
            f_open(&f, "0:/RADIOFAV.M3U", FA_READ); f_read(&f, b, sizeof b - 1, &got); b[got] = 0; f_close(&f);
            CHECK(!strcmp(b, "#EXTM3U\n#EXTINF:-1,One\nhttp://one.test/a\nhttp://three.test/c"), "the file: '%s'", b);
            cmd_str(PVA_C_DIR_OPEN, "");
            n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
            CHECK(n == 3, "two left (%d)", n - 1);
            CHECK(cmd_u16(PVA_C_FAV_ADD, 1) == PVA_E_EXISTS, "a favourite from the favourites: EXISTS");
            // add one to it: a line end first
            cmd_str(PVA_C_DIR_SEARCH, "jazz");
            WAIT_LIST(10000);
            cmd_str(PVA_C_DIR_OPEN, "");
            n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
            CHECK(cmd_u16(PVA_C_FAV_ADD, (uint16_t)find(n, "Secure Jazz")) == PVA_OK, "FAV_ADD to that file");
            f_open(&f, "0:/RADIOFAV.M3U", FA_READ); f_read(&f, b, sizeof b - 1, &got); b[got] = 0; f_close(&f);
            CHECK(!strcmp(b, "#EXTM3U\n#EXTINF:-1,One\nhttp://one.test/a\nhttp://three.test/c\r\n#EXTINF:-1,Secure Jazz\r\nhttps://secure.test/stream.mp3\r\n"),
                  "the file: '%s'", b);
            // a full file
            f_open(&f, "0:/RADIOFAV.M3U", FA_CREATE_ALWAYS | FA_WRITE);
            for (int k = 0; k < 4050; k++) f_write(&f, "#", 1, &put);
            f_write(&f, "\r\n", 2, &put);
            f_close(&f);
            CHECK(cmd_u16(PVA_C_FAV_ADD, (uint16_t)find(n, "Secure Jazz")) == PVA_E_FULL, "a full file: FULL");
        }
        f_unlink("0:/RADIOFAV.M3U");
        pva_allow_card_writes(false);
        cmd_str(PVA_C_DIR_OPEN, "/Internet radio");
        cmd_str(PVA_C_DIR_OPEN, "/Internet radio");


        printf("[clock, place and settings]\n");
        {
            char a[64], b[64];
            read_clock();
            CHECK((ck[PVA_CK_FLAGS] & PVA_CKF_TIME) && !(ck[PVA_CK_FLAGS] & PVA_CKF_ZONE), "the Radio Browser's Date set the clock (flags %02X)", ck[PVA_CK_FLAGS]);
            clock_text(a, sizeof a); expect_time(0, b, sizeof b);
            CHECK(!strcmp(a, b), "UTC before a zone: %s (%s)", a, b);
            uint32_t wr0 = host_esp_web_requests;
            run_ms(3000);
            CHECK(host_esp_web_requests == wr0, "no web requests before PicoAmp starts the clock");
            f_unlink("0:/PICOAMP.INI");
            CHECK(cmd(PVA_C_CFG_LOAD, NULL, 0) == PVA_OK, "CFG_LOAD without a file");
            CHECK(cmd_str(PVA_C_CFG_GET, "clock.face") == PVA_E_NOTFOUND, "no setting: NOTFOUND");
            WAIT_CLOCK((ck[PVA_CK_FLAGS] & PVA_CKF_WEATHER), 15000);
            CHECK((ck[PVA_CK_FLAGS] & (PVA_CKF_ZONE | PVA_CKF_WEATHER)) == (PVA_CKF_ZONE | PVA_CKF_WEATHER), "by IP: zone and weather (flags %02X)", ck[PVA_CK_FLAGS]);
            CHECK(strstr(host_esp_last_web, "latitude=52.3759&longitude=4.8975") && strstr(host_esp_last_web, "timezone=auto"), "the forecast for the IP's place: %s", host_esp_last_web);
            int16_t om = (int16_t)(ck[PVA_CK_OFFSET] | (ck[PVA_CK_OFFSET + 1] << 8));
            int16_t t10 = (int16_t)(ck[PVA_CK_TEMP] | (ck[PVA_CK_TEMP + 1] << 8));
            CHECK(om == 60 && t10 == -4 && ck[PVA_CK_WCODE] == 61 && !strcmp((char *)&ck[PVA_CK_ABBR], "CET"), "UTC+60 min, -0.4 C, code 61, CET (%d %d %u %s)", om, t10, ck[PVA_CK_WCODE], (char *)&ck[PVA_CK_ABBR]);
            clock_text(a, sizeof a); expect_time(60, b, sizeof b);
            CHECK(!strcmp(a, b), "local time %s (%s)", a, b);
            CHECK(cmd(PVA_C_CLOCK_INFO, NULL, 0) == PVA_OK && !strcmp((char *)reply, "Amsterdam, North Holland, NL") &&
                  !strcmp((char *)reply + strlen((char *)reply) + 1, "Europe/Amsterdam"), "CLOCK_INFO: '%s'", reply);
            uint8_t s0 = ck[PVA_CK_SEC];
            run_ms(2000); read_clock();
            CHECK(ck[PVA_CK_SEC] == (s0 + 2) % 60, "two seconds later: %u -> %u", s0, ck[PVA_CK_SEC]);
            // places by name
            CHECK(cmd_str(PVA_C_LOC_SEARCH, "zur") == PVA_OK, "LOC_SEARCH 'zur'");
            read_clock();
            CHECK(ck[PVA_CK_LOC_STATE] == PVA_LOCS_LOADING, "loading");
            WAIT_CLOCK(ck[PVA_CK_LOC_STATE] != PVA_LOCS_LOADING, 10000);
            CHECK(ck[PVA_CK_LOC_STATE] == PVA_LOCS_DONE && ck[PVA_CK_LOC_COUNT] == 2, "2 places (state %u)", ck[PVA_CK_LOC_STATE]);
            CHECK(cmd(PVA_C_LOC_LIST, NULL, 0) == PVA_OK && reply[0] == 2 && !strcmp((char *)reply + 1, "Zurich, CH") &&
                  !strcmp((char *)reply + 1 + strlen((char *)reply + 1) + 1, "Zurich, New York, US"), "LOC_LIST: '%s'", reply + 1);
            CHECK(cmd_u8(PVA_C_LOC_PICK, 5) == PVA_E_RANGE, "LOC_PICK out of range");
            uint8_t g0 = ck[PVA_CK_GEN];
            CHECK(cmd_u8(PVA_C_LOC_PICK, 0) == PVA_OK, "LOC_PICK 0");
            WAIT_CLOCK(ck[PVA_CK_GEN] != g0 && !(ck[PVA_CK_FLAGS] & PVA_CKF_BUSY) && strstr(host_esp_last_web, "latitude=47.36667"), 10000);
            CHECK(strstr(host_esp_last_web, "latitude=47.36667&longitude=8.55"), "the forecast for Zurich: %s", host_esp_last_web);
            CHECK(cmd(PVA_C_CLOCK_INFO, NULL, 0) == PVA_OK && !strcmp((char *)reply, "Zurich, CH"), "CLOCK_INFO: '%s'", reply);
            CHECK(cmd_str(PVA_C_CFG_GET, "location.mode") == PVA_OK && !strcmp((char *)reply, "place"), "location.mode=%s", reply);
            CHECK(cmd_str(PVA_C_LOC_SEARCH, "nowhere") == PVA_OK, "LOC_SEARCH 'nowhere'");
            WAIT_CLOCK(ck[PVA_CK_LOC_STATE] != PVA_LOCS_LOADING, 10000);
            CHECK(ck[PVA_CK_LOC_STATE] == PVA_LOCS_DONE && ck[PVA_CK_LOC_COUNT] == 0, "no places");
            // the other side of the world
            cmd_str(PVA_C_LOC_SEARCH, "sydney");
            WAIT_CLOCK(ck[PVA_CK_LOC_STATE] != PVA_LOCS_LOADING, 10000);
            CHECK(cmd_u8(PVA_C_LOC_PICK, 0) == PVA_OK, "Sydney");
            WAIT_CLOCK((int16_t)(ck[PVA_CK_OFFSET] | (ck[PVA_CK_OFFSET + 1] << 8)) == 660, 10000);
            t10 = (int16_t)(ck[PVA_CK_TEMP] | (ck[PVA_CK_TEMP + 1] << 8));
            CHECK(t10 == 216 && ck[PVA_CK_WCODE] == 1 && !strcmp((char *)&ck[PVA_CK_ABBR], "AEDT"), "UTC+11, 21.6 C, AEDT (%d %s)", t10, (char *)&ck[PVA_CK_ABBR]);
            clock_text(a, sizeof a); expect_time(660, b, sizeof b);
            CHECK(!strcmp(a, b), "Sydney: %s (%s), the next day", a, b);
            // settings text
            {
                uint8_t kv[] = "clock.face\0analog";
                CHECK(cmd(PVA_C_CFG_SET, kv, sizeof kv) == PVA_OK, "CFG_SET clock.face");
            }
            CHECK(cmd_str(PVA_C_CFG_GET, "Clock.Face") == PVA_OK && !strcmp((char *)reply, "analog"), "CFG_GET (any case): %s", reply);
            CHECK(cmd(PVA_C_CFG_TEXT, NULL, 0) == PVA_OK, "CFG_TEXT");
            reply[reply_len] = 0;
            CHECK(!strcmp((char *)reply, "[location]\r\nmode=place\r\nname=Sydney, New South Wales, AU\r\nlat=-33.86785\r\nlon=151.20732\r\n\r\n[clock]\r\nface=analog\r\n"),
                  "the text:\n%s", reply);
            char saved[400];
            snprintf(saved, sizeof saved, "%s", (char *)reply);
            CHECK(cmd(PVA_C_CFG_SAVE, NULL, 0) == PVA_E_RDONLY, "CFG_SAVE with Nextor's card: RDONLY");
            pva_allow_card_writes(true);
            CHECK(cmd(PVA_C_CFG_SAVE, NULL, 0) == PVA_OK, "CFG_SAVE");
            {
                FIL f; char t[400]; UINT got = 0;
                f_open(&f, "0:/PICOAMP.INI", FA_READ); f_read(&f, t, sizeof t - 1, &got); t[got] = 0; f_close(&f);
                CHECK(!strcmp(t, saved), "PICOAMP.INI written (%u bytes)", (unsigned)got);
                // someone edited it: comments, other order, no line end at the end
                const char *e = "; my settings\r\n[clock]\r\n; the face\r\nFace = digital\r\n\r\n[location]\nmode=ip\nname=x";
                UINT put = 0;
                f_open(&f, "0:/PICOAMP.INI", FA_CREATE_ALWAYS | FA_WRITE); f_write(&f, e, (UINT)strlen(e), &put); f_close(&f);
            }
            CHECK(cmd(PVA_C_CFG_LOAD, NULL, 0) == PVA_OK && cmd_str(PVA_C_CFG_GET, "clock.face") == PVA_OK && !strcmp((char *)reply, "digital"), "read again: face=%s", reply);
            {
                uint8_t k1[] = "clock.face\0analog", k2[] = "clock.time\0msx", k3[] = "screen.colours\0music", k4[] = "location.lat\0" "1";
                cmd(PVA_C_CFG_SET, k1, sizeof k1); cmd(PVA_C_CFG_SET, k2, sizeof k2); cmd(PVA_C_CFG_SET, k3, sizeof k3); cmd(PVA_C_CFG_SET, k4, sizeof k4);
            }
            cmd(PVA_C_CFG_TEXT, NULL, 0);
            reply[reply_len] = 0;
            CHECK(!strcmp((char *)reply, "; my settings\r\n[clock]\r\n; the face\r\nFace = analog\r\ntime=msx\r\n\r\n[location]\nmode=ip\nname=x\r\nlat=1\r\n\r\n[screen]\r\ncolours=music\r\n"),
                  "edited text kept:\n%s", reply);
            WAIT_CLOCK(!(ck[PVA_CK_FLAGS] & PVA_CKF_BUSY) && (int16_t)(ck[PVA_CK_OFFSET] | (ck[PVA_CK_OFFSET + 1] << 8)) == 60, 15000);
            CHECK(cmd(PVA_C_CLOCK_INFO, NULL, 0) == PVA_OK && !strcmp((char *)reply, "Amsterdam, North Holland, NL"), "mode=ip from the file: '%s'", reply);
            {
                uint8_t big[PVA_ARGS_SIZE];
                memset(big, 'x', sizeof big);
                memcpy(big, "x.y", 4);
                big[200] = 0;
                int r = PVA_OK;
                for (int i = 0; i < 10 && r == PVA_OK; i++) { big[0] = (uint8_t)('a' + i); r = cmd(PVA_C_CFG_SET, big, 201); }
                CHECK(r == PVA_E_FULL, "a full text: FULL");
            }
            pva_allow_card_writes(false);
            f_unlink("0:/PICOAMP.INI");
            cmd(PVA_C_CFG_LOAD, NULL, 0);
            host_esp_web_down = true;
            cmd_str(PVA_C_LOC_SEARCH, "zur");
            WAIT_CLOCK(ck[PVA_CK_LOC_STATE] != PVA_LOCS_LOADING, 20000);
            CHECK(ck[PVA_CK_LOC_STATE] == PVA_LOCS_FAILED && ck[PVA_CK_ERR] == PVA_NERR_CONNECT, "search with the server down: FAILED (%u)", ck[PVA_CK_ERR]);
            host_esp_web_down = false;
            CHECK(cmd(PVA_C_LOC_LIST, NULL, 0) == PVA_E_STATE, "no list after a failure");
        }


        printf("[weather, sleep timer, alarm, news]\n");
        {
            WAIT_CLOCK((ck[PVA_CK_FLAGS] & PVA_CKF_WEATHER) && !(ck[PVA_CK_FLAGS] & PVA_CKF_BUSY), 15000);
            CHECK(strstr(host_esp_last_web, "&daily=weather_code,temperature_2m_max") != NULL, "the forecast asked for: %.60s", host_esp_last_web + 80);
            CHECK(cmd(PVA_C_WX_GET, NULL, 0) == PVA_OK && reply_len == PVA_WX_DAY0 + 6 * PVA_WX_DAYSIZE, "WX_GET (%u bytes)", reply_len);
            CHECK(reply[PVA_WX_NDAYS] == 5, "5 days with a forecast (the 6th is null): %u", reply[PVA_WX_NDAYS]);
            #define S16(o) ((int16_t)(reply[o] | (reply[(o) + 1] << 8)))
            CHECK(S16(PVA_WX_TEMP) == -4 && S16(PVA_WX_FEEL) == -35 && reply[PVA_WX_HUM] == 87 &&
                  reply[PVA_WX_CODE] == 61 && reply[PVA_WX_WIND] == 15 && reply[PVA_WX_IS_DAY] == 0,
                  "now: %d (feels %d), %u%%, code %u, %u km/h", S16(PVA_WX_TEMP), S16(PVA_WX_FEEL), reply[PVA_WX_HUM], reply[PVA_WX_CODE], reply[PVA_WX_WIND]);
            const uint8_t *d0 = reply + PVA_WX_DAY0, *d4 = d0 + 4 * PVA_WX_DAYSIZE, *d5 = d0 + 5 * PVA_WX_DAYSIZE;
            CHECK(d0[PVA_WXD_WDAY] == 3 && d0[PVA_WXD_MDAY] == 29 && d0[PVA_WXD_CODE] == 61 && d0[PVA_WXD_PP] == 80 &&
                  (int16_t)(d0[PVA_WXD_TMAX] | (d0[PVA_WXD_TMAX + 1] << 8)) == 84 && (int16_t)(d0[PVA_WXD_TMIN] | (d0[PVA_WXD_TMIN + 1] << 8)) == -12 &&
                  d0[PVA_WXD_WIND] == 25, "today: Thursday 29, code 61, 80%%, 8.4 / -1.2, 25 km/h (%u %u %u %u %d %d %u)", d0[0], d0[1], d0[2], d0[3],
                  (int16_t)(d0[PVA_WXD_TMAX] | (d0[PVA_WXD_TMAX + 1] << 8)), (int16_t)(d0[PVA_WXD_TMIN] | (d0[PVA_WXD_TMIN + 1] << 8)), d0[PVA_WXD_WIND]);
            CHECK(d4[PVA_WXD_WDAY] == 0 && d4[PVA_WXD_MDAY] == 2 && d4[PVA_WXD_CODE] == 95 && d4[PVA_WXD_PP] == 100 && d4[PVA_WXD_WIND] == 62,
                  "day 5: Monday 2, thunder (%u %u %u)", d4[PVA_WXD_WDAY], d4[PVA_WXD_CODE], d4[PVA_WXD_WIND]);
            (void)d5;
            // the sleep timer
            CHECK(cmd_str(PVA_C_PLAY_URL, "http://radio.test/stream.mp3") == PVA_OK, "a stream plays");
            run_ms(3000);
            uint8_t sl[2] = { 1, 0 };
            CHECK(cmd(PVA_C_SLEEP, sl, 2) == PVA_OK, "SLEEP 1 minute");
            run_ms(100); read_clock();
            CHECK(ck[PVA_CK_SLEEP] == 1, "1 minute left (%u)", ck[PVA_CK_SLEEP]);
            run_ms(45000); read_status();
            CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING, "still playing after 45 s (fading)");
            run_ms(16000); read_status(); read_clock();
            CHECK(st[PVA_ST_STATE] == PVA_STATE_STOPPED && ck[PVA_CK_SLEEP] == 0, "stopped after a minute (state %u)", st[PVA_ST_STATE]);
            // the alarm: what plays now, a minute from now
            uint8_t al[3] = { PVA_AL_ON, 7, 30 };
            cmd(PVA_C_CFG_TEXT, NULL, 0);
            CHECK(cmd(PVA_C_ALARM, al, 3) == PVA_E_STATE, "ALARM with nothing to play: STATE");
            uint8_t pu[] = "http://radio2.test:8000/live\0Radio Two Live";
            cmd(PVA_C_PLAY_URL, pu, sizeof pu);
            run_ms(2000);
            read_clock();
            uint8_t ah = ck[PVA_CK_HOUR], am = (uint8_t)(ck[PVA_CK_MIN] + 1u);
            if (am == 60) { am = 0; ah = (uint8_t)((ah + 1u) % 24u); }
            al[1] = ah; al[2] = am;
            CHECK(cmd(PVA_C_ALARM, al, 3) == PVA_OK, "ALARM %02u:%02u", ah, am);
            CHECK(cmd_str(PVA_C_CFG_GET, "alarm.play") == PVA_OK && !strcmp((char *)reply, "http://radio2.test:8000/live"), "alarm.play=%s", reply);
            CHECK(cmd_str(PVA_C_CFG_GET, "alarm.name") == PVA_OK && !strcmp((char *)reply, "Radio Two Live"), "alarm.name=%s", reply);
            cmd(PVA_C_STOP, NULL, 0);
            run_ms(100); read_clock();
            CHECK(ck[PVA_CK_ALARM] == PVA_AL_ON && ck[PVA_CK_AL_HOUR] == ah && ck[PVA_CK_AL_MIN] == am, "the clock block shows it");
            WAIT_CLOCK(ck[PVA_CK_ALARM] & PVA_AL_RINGING, 65000);
            read_status();
            CHECK((ck[PVA_CK_ALARM] & PVA_AL_RINGING) && st[PVA_ST_STATE] == PVA_STATE_PLAYING && !strcmp(ststr(PVA_ST_ALBUM), "Radio Two Live"),
                  "it rings: %s plays", ststr(PVA_ST_ALBUM));
            cmd(PVA_C_PAUSE, NULL, 0);
            run_ms(100); read_clock();
            CHECK(!(ck[PVA_CK_ALARM] & PVA_AL_RINGING) && (ck[PVA_CK_ALARM] & PVA_AL_ON), "a key stops the ringing, the alarm stays on");
            cmd(PVA_C_STOP, NULL, 0);
            al[0] = 0;
            CHECK(cmd(PVA_C_ALARM, al, 3) == PVA_OK && cmd_str(PVA_C_CFG_GET, "alarm.on") == PVA_OK && !strcmp((char *)reply, "off"), "alarm off");
            // news: the favourites (by default a few world services) and the catalogue
            #define NTH(i) nth_str((char *)reply + 1, i)
            CHECK(cmd(PVA_C_NEWS_FEEDS, NULL, 0) == PVA_OK && reply[0] == 5 && !strcmp(NTH(0), "BBC News, World") && !strcmp(NTH(4), "NOS Nieuws"),
                  "5 favourites by default: %s ... %s", NTH(0), NTH(4));
            CHECK(cmd(PVA_C_NEWS_TOPICS, NULL, 0) == PVA_OK && reply[0] >= 10 && !strcmp(NTH(0), "World news"), "%u topics, the first: %s", reply[0], NTH(0));
            CHECK(cmd_u8(PVA_C_NEWS_CAT, 0) == PVA_OK && reply[0] >= 10 && !strcmp(NTH(0), "*BBC News, World") && !strcmp(NTH(1), " BBC News, top stories"),
                  "topic 0: %u feeds, '%s', '%s'", reply[0], NTH(0), NTH(1));
            CHECK(cmd_u8(PVA_C_NEWS_CAT, 99) == PVA_E_RANGE, "no topic 99");
            uint8_t ad[2] = { 0, 1 };
            CHECK(cmd(PVA_C_NEWS_ADD, ad, 2) == PVA_OK && cmd(PVA_C_NEWS_FEEDS, NULL, 0) == PVA_OK && reply[0] == 6 && !strcmp(NTH(5), "BBC News, top stories"),
                  "added: 6 favourites, the last %s", NTH(5));
            CHECK(cmd(PVA_C_NEWS_ADD, ad, 2) == PVA_E_EXISTS, "added again: EXISTS");
            CHECK(cmd_str(PVA_C_CFG_GET, "news.feeds") == PVA_OK && !strcmp((char *)reply, "bbcw,alj,dw,guardw,nos,bbc"), "news.feeds=%s", reply);
            uint8_t mv[2] = { 5, 0 };
            cmd(PVA_C_NEWS_MOVE, mv, 2);
            CHECK(cmd_str(PVA_C_CFG_GET, "news.feeds") == PVA_OK && !strcmp((char *)reply, "bbcw,alj,dw,guardw,bbc,nos"), "moved up: %s", reply);
            mv[0] = 0; mv[1] = 1;
            cmd(PVA_C_NEWS_MOVE, mv, 2);
            CHECK(cmd_str(PVA_C_CFG_GET, "news.feeds") == PVA_OK && !strcmp((char *)reply, "alj,bbcw,dw,guardw,bbc,nos"), "moved down: %s", reply);
            CHECK(cmd_u8(PVA_C_NEWS_DEL, 5) == PVA_OK && cmd_str(PVA_C_CFG_GET, "news.feeds") == PVA_OK && !strcmp((char *)reply, "alj,bbcw,dw,guardw,bbc"), "removed: %s", reply);
            CHECK(cmd_u8(PVA_C_NEWS_DEL, 9) == PVA_E_RANGE, "no favourite 10");
            CHECK(cmd_u8(PVA_C_NEWS_CAT, 2) == PVA_OK && !strcmp(NTH(0), " NOS Nieuws"), "NOS no longer a favourite: '%s'", NTH(0));
            {
                uint8_t k1[] = "news.feed1\0Test|http://news.test/rss", k2[] = "news.feed2\0RDF|http://news.test/rdf",
                        k3[] = "news.feed3\0Atom|http://news.test/atom", k4[] = "news.feed4\0Many|http://news.test/many",
                        k5[] = "news.feed5\0Content|http://news.test/content", kf[] = "news.feeds\0#1,#2,#3,#4,#5";
                cmd(PVA_C_CFG_SET, k1, sizeof k1); cmd(PVA_C_CFG_SET, k2, sizeof k2); cmd(PVA_C_CFG_SET, k3, sizeof k3);
                cmd(PVA_C_CFG_SET, k4, sizeof k4); cmd(PVA_C_CFG_SET, k5, sizeof k5); cmd(PVA_C_CFG_SET, kf, sizeof kf);
            }
            CHECK(cmd(PVA_C_NEWS_FEEDS, NULL, 0) == PVA_OK && reply[0] == 5 && !strcmp(NTH(0), "Test") && !strcmp(NTH(4), "Content"), "feeds of one's own: %u, %s", reply[0], NTH(0));
            #define WAIT_NEWS() WAIT_CLOCK(ck[PVA_CK_NEWS_STATE] != PVA_NEWS_LOADING, 20000)
            #define WAIT_ART() WAIT_CLOCK(ck[PVA_CK_ART_STATE] != PVA_ART_LOADING, 30000)
            CHECK(cmd_u8(PVA_C_NEWS_FETCH, 0) == PVA_OK, "NEWS_FETCH 0");
            read_clock();
            CHECK(ck[PVA_CK_NEWS_STATE] == PVA_NEWS_LOADING, "loading");
            WAIT_NEWS();
            CHECK(ck[PVA_CK_NEWS_STATE] == PVA_NEWS_DONE && ck[PVA_CK_NEWS_COUNT] == 3 && ck[PVA_CK_NEWS_FEED] == 0, "3 stories (state %u, %u)", ck[PVA_CK_NEWS_STATE], ck[PVA_CK_NEWS_COUNT]);
            uint8_t ls[3] = { 0, 10, 70 };
            CHECK(cmd(PVA_C_NEWS_LIST, ls, 3) == PVA_OK, "NEWS_LIST");
            const char *n0 = (char *)reply, *n1 = n0 + strlen(n0) + 1, *n2 = n1 + strlen(n1) + 1, *n3 = n2 + strlen(n2) + 1;
            CHECK(strlen(n0) == 5 && !strcmp(n1, "Cafe owners & \"friends\" meet"), "story 1: '%s' '%s'", n0, n1);
            CHECK(!strcmp(n3, "Second & \"quoted\" story"), "title 2 (an HTML-like <tag> goes): '%s'", n3);
            ls[2] = 12;
            cmd(PVA_C_NEWS_LIST, ls, 3);
            n1 = (char *)reply + strlen((char *)reply) + 1;
            CHECK(!strcmp(n1, "Cafe owne..."), "a title cut to 12: '%s'", n1);
            uint8_t tx[6] = { 0, 0, 0, 0, 10, 30 };
            CHECK(cmd(PVA_C_NEWS_TEXT, tx, 6) == PVA_OK && reply[0] == 2 && reply[1] == 0 && reply[2] == 0 &&
                  !strcmp((char *)reply + 3, "The first story, with spaces") && !strcmp(nth_str((char *)reply + 3, 1), "and a line."),
                  "the summary in lines of 30: %u lines, '%s'", reply[0], reply + 3);
            tx[1] = 2; tx[5] = 12;
            cmd(PVA_C_NEWS_TEXT, tx, 6);
            CHECK(reply[0] == 3 && !strcmp((char *)reply + 3, "Cafe owners") && !strcmp(nth_str((char *)reply + 3, 2), "meet"), "the title in lines of 12: %u", reply[0]);
            CHECK(cmd_u8(PVA_C_NEWS_ITEM, 0) == PVA_OK && !strcmp(nth_str((char *)reply, 1), "news.test") && strstr((char *)reply, "29 Oct 2026"),
                  "ITEM 0: '%s', '%s'", reply, nth_str((char *)reply, 1));
            tx[0] = 1; tx[1] = 0; tx[5] = 78;
            cmd(PVA_C_NEWS_TEXT, tx, 6);
            CHECK(!strcmp((char *)reply + 3, "Escaped & HTML link."), "escaped HTML: '%s'", reply + 3);
            tx[0] = 2;
            CHECK(cmd(PVA_C_NEWS_TEXT, tx, 6) == PVA_OK && reply[0] == 0, "no description: 0 lines");
            tx[0] = 3;
            CHECK(cmd(PVA_C_NEWS_TEXT, tx, 6) == PVA_E_RANGE && cmd_u8(PVA_C_NEWS_ITEM, 3) == PVA_E_RANGE, "story 4: RANGE");
            // the full article: the page at the story's link
            CHECK(cmd_u8(PVA_C_NEWS_FULL, 0) == PVA_OK, "NEWS_FULL 0");
            read_clock();
            CHECK(ck[PVA_CK_ART_STATE] == PVA_ART_LOADING && ck[PVA_CK_ART_STORY] == 0, "the article: loading");
            WAIT_ART();
            CHECK(ck[PVA_CK_ART_STATE] == PVA_ART_DONE && !strcmp(host_esp_last_web, "/1"), "the article: in (%u), from %s", ck[PVA_CK_ART_STATE], host_esp_last_web);
            tx[0] = 0; tx[1] = 1; tx[5] = 78;
            cmd(PVA_C_NEWS_TEXT, tx, 6);
            {
                const char *l0 = (char *)reply + 3, *l2 = nth_str(l0, 2), *l4 = nth_str(l0, 4);
                CHECK(reply[0] == 5 && reply[2] == PVA_TXF_ARTICLE && !strcmp(l0, "The first paragraph of the article, long enough to be kept as text.") && !nth_str(l0, 1)[0],
                      "the article: %u lines (flags %u): '%s'", reply[0], reply[2], l0);
                CHECK(!strcmp(l2, "Second paragraph with a link & an entity, cafe and \"quotes\" - done."), "its paragraph 2: '%s'", l2);
                CHECK(!strcmp(l4, "Third paragraph: the end of the article text, again long enough - really."), "its paragraph 3 (menus, captions, scripts, footer left out): '%s'", l4);
            }
            tx[0] = 1;
            cmd(PVA_C_NEWS_TEXT, tx, 6);
            CHECK(reply[2] == 0, "another story: its summary (flags %u)", reply[2]);
            CHECK(cmd_u8(PVA_C_NEWS_FULL, 2) == PVA_OK && (read_clock(), ck[PVA_CK_ART_STATE] == PVA_ART_FAILED), "no link: FAILED at once");
            tx[0] = 2;
            cmd(PVA_C_NEWS_TEXT, tx, 6);
            CHECK(reply[2] == PVA_TXF_FAILED, "and the summary shows (flags %u)", reply[2]);
            // the whole text in the feed
            cmd_u8(PVA_C_NEWS_FETCH, 4); WAIT_NEWS();
            CHECK(ck[PVA_CK_NEWS_COUNT] == 1 && cmd_u8(PVA_C_NEWS_ITEM, 0) == PVA_OK && reply_len > 2 && reply[reply_len - 1] == 1, "a feed with content: flagged");
            cmd_u8(PVA_C_NEWS_FULL, 0); WAIT_ART();
            tx[0] = 0; tx[2] = 0; tx[4] = 4;
            cmd(PVA_C_NEWS_TEXT, tx, 6);
            CHECK(ck[PVA_CK_ART_STATE] == PVA_ART_DONE && !strcmp(host_esp_last_web, "/content") && reply[2] == PVA_TXF_ARTICLE &&
                  !strncmp((char *)reply + 3, "Paragraph 1 of the whole story", 30), "from the feed: %u lines, '%s'", reply[0] | reply[1] << 8, reply + 3);
            uint16_t alines = (uint16_t)(reply[0] | reply[1] << 8);
            tx[2] = (uint8_t)(alines - 1); tx[3] = 0;
            cmd(PVA_C_NEWS_TEXT, tx, 6);
            CHECK(alines >= 23 && strstr((char *)reply + 3, "two.") && reply_len < 90, "the last line: '%s'", reply + 3);
            cmd_u8(PVA_C_NEWS_FETCH, 1); WAIT_NEWS();
            tx[0] = 0; tx[1] = 0; tx[2] = 0;
            cmd(PVA_C_NEWS_TEXT, tx, 6);
            CHECK(ck[PVA_CK_NEWS_COUNT] == 1 && !strcmp((char *)reply + 3, "RDF text") && cmd_u8(PVA_C_NEWS_ITEM, 0) == PVA_OK && strstr((char *)reply, "29 Oct 2026"),
                  "RSS 1.0: '%s'", reply);
            cmd_u8(PVA_C_NEWS_FETCH, 2); WAIT_NEWS();
            cmd(PVA_C_NEWS_TEXT, tx, 6);
            CHECK(ck[PVA_CK_NEWS_COUNT] == 1 && !strcmp((char *)reply + 3, "Atom text"), "Atom: '%s'", reply + 3);
            cmd_u8(PVA_C_NEWS_ITEM, 0);
            CHECK(!strcmp(nth_str((char *)reply, 1), "x") || !strcmp(nth_str((char *)reply, 1), ""), "Atom's <link href>: '%s'", nth_str((char *)reply, 1));
            cmd_u8(PVA_C_NEWS_FETCH, 3); WAIT_NEWS();
            uint8_t nn = ck[PVA_CK_NEWS_COUNT];
            CHECK(ck[PVA_CK_NEWS_STATE] == PVA_NEWS_DONE && nn >= 8 && nn <= 40, "a long feed: %u stories kept (as many as fit, at most 40)", nn);
            // the feed has the oldest first: the newest are kept (the oldest made room), newest first
            uint8_t l2[3] = { 0, 2, 70 };
            cmd(PVA_C_NEWS_LIST, l2, 3);
            CHECK(!strcmp(nth_str((char *)reply, 1), "Story 59") && !strcmp(nth_str((char *)reply, 3), "Story 58"), "newest first: %s, %s", nth_str((char *)reply, 1), nth_str((char *)reply, 3));
            uint8_t l3[3] = { (uint8_t)(nn - 1u), 2, 70 };
            char e1[16];
            snprintf(e1, sizeof e1, "Story %02u", (unsigned)(60u - nn));
            cmd(PVA_C_NEWS_LIST, l3, 3);
            CHECK(!strcmp(nth_str((char *)reply, 1), e1) && !nth_str((char *)reply, 2)[0], "the oldest kept: %s (of %u)", nth_str((char *)reply, 1), nn);
            // S: the feed's order, then newest first again (kept in [news] sort=)
            CHECK(cmd_u8(PVA_C_NEWS_SORT, 0xFF) == PVA_OK && reply[0] == 1, "NEWS_SORT ask: newest first (%u)", reply[0]);
            CHECK(cmd_u8(PVA_C_NEWS_SORT, 2) == PVA_OK && reply[0] == 0, "NEWS_SORT toggle: the feed's order");
            cmd(PVA_C_NEWS_LIST, l2, 3);
            snprintf(e1, sizeof e1, "Story %02u", (unsigned)(60u - nn));
            CHECK(!strcmp(nth_str((char *)reply, 1), e1), "the feed's order: %s first", nth_str((char *)reply, 1));
            CHECK(cmd_str(PVA_C_CFG_GET, "news.sort") == PVA_OK && !strcmp((char *)reply, "feed"), "news.sort=%s", reply);
            CHECK(cmd_u8(PVA_C_NEWS_SORT, 1) == PVA_OK && reply[0] == 1, "NEWS_SORT 1: newest first");
            cmd(PVA_C_NEWS_LIST, l2, 3);
            CHECK(!strcmp(nth_str((char *)reply, 1), "Story 59"), "newest first again: %s", nth_str((char *)reply, 1));
            CHECK(cmd_u8(PVA_C_NEWS_FETCH, 7) == PVA_E_RANGE, "no favourite 8");
            CHECK(cmd(PVA_C_NEWS_INFO, NULL, 0) == PVA_OK && !strncmp((char *)reply, "Feed 4: in, ", 12) && strstr(nth_str((char *)reply, 1), "Memory: stories") &&
                  reply_len < 1024, "NEWS_INFO: '%s' / '%s' (%u bytes)", reply, nth_str((char *)reply, 1), reply_len);
            {
                // an https feed while an https station plays: the ESP-01 has one TLS connection,
                // the stream lends it and connects again
                static const char sx[] = "https://secure.test/stream.mp3\0";
                cmd(PVA_C_PLAY_URL, sx, sizeof sx);
                run_ms(40000);                  // (its buffer fills)
                read_status();
                uint16_t un0 = st16(PVA_ST_UNDERRUNS);
                CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st[PVA_ST_NET_STATE] == PVA_NET_STREAM, "the TLS station plays");
                uint8_t ks[] = "news.feed1\0Secure|https://news.test/rss";
                cmd(PVA_C_CFG_SET, ks, sizeof ks);
                uint32_t t0 = host_esp_tls_opens, tls_ms = host_esp_tls_ms;
                host_esp_tls_ms = 800;          // (as the ESP-01 did on the cartridge: 0.5-1 s)
                cmd_u8(PVA_C_NEWS_FETCH, 0); WAIT_NEWS();
                CHECK(ck[PVA_CK_NEWS_STATE] == PVA_NEWS_DONE && ck[PVA_CK_NEWS_COUNT] == 3, "an https feed while it plays: %u stories (state %u, error %u)",
                      ck[PVA_CK_NEWS_COUNT], ck[PVA_CK_NEWS_STATE], ck[PVA_CK_NEWS_ERR]);
                run_ms(6000);
                read_status();
                CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st[PVA_ST_NET_STATE] == PVA_NET_STREAM && host_esp_tls_opens >= t0 + 2,
                      "the station has its TLS connection again (%u opens, net state %u)", host_esp_tls_opens - t0, st[PVA_ST_NET_STATE]);
                uint8_t kc[] = "news.feed1\0Secure|https://news.test/content";
                cmd(PVA_C_CFG_SET, kc, sizeof kc);
                cmd_u8(PVA_C_NEWS_FETCH, 0); WAIT_NEWS();
                run_ms(5000);
                cmd_u8(PVA_C_NEWS_FULL, 0); WAIT_ART();
                uint8_t tq[6] = { 0, 1, 0, 0, 2, 78 };
                cmd(PVA_C_NEWS_TEXT, tq, 6);
                CHECK(ck[PVA_CK_ART_STATE] == PVA_ART_DONE && (reply[2] & PVA_TXF_ARTICLE), "an article over https while it plays (flags %u)", reply[2]);
                run_ms(6000);
                read_status();
                CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st[PVA_ST_NET_STATE] == PVA_NET_STREAM, "the station plays on after it");

                // after a lend the station connects again to the URL it ended at (its redirects
                // were followed once), not to its first URL
                {
                    static const char sg[] = "https://secure.test/go\0";
                    cmd(PVA_C_PLAY_URL, sg, sizeof sg);
                    run_ms(30000);
                    uint32_t r0 = host_esp_redirs;
                    cmd_u8(PVA_C_NEWS_FETCH, 0); WAIT_NEWS();
                    run_ms(6000);
                    read_status();
                    CHECK(r0 >= 1 && host_esp_redirs == r0 && st[PVA_ST_STATE] == PVA_STATE_PLAYING && pva_net_stats()->back_ms > 0 && pva_net_stats()->back_ms < 3000,
                          "after a lend: straight to the URL in use (redirects %u -> %u, back in %lu ms)", r0, host_esp_redirs, (unsigned long)pva_net_stats()->back_ms);
                }
                // a big page over https while the https station plays: pieces, the radio filling
                // its buffer between them, each next piece from where the last one ended (Range)
                uint8_t kb[] = "news.feed1\0Big|https://news.test/bigfeed";
                cmd(PVA_C_CFG_SET, kb, sizeof kb);
                cmd_u8(PVA_C_NEWS_FETCH, 0); WAIT_NEWS();
                run_ms(8000);
                read_status();
                uint16_t un1 = st16(PVA_ST_UNDERRUNS);
                uint32_t rg0 = host_esp_ranges;
                cmd_u8(PVA_C_NEWS_FULL, 0);
                bool waited = false, holdseen = false;
                for (int w_ = 0; w_ < 3600 && ck[PVA_CK_ART_STATE] != PVA_ART_DONE && ck[PVA_CK_ART_STATE] != PVA_ART_FAILED; w_++)
                {
                    run_ms(50); read_clock();
                    if (ck[PVA_CK_ART_WAIT] == 1) waited = true;
                    if (ck[PVA_CK_ART_WAIT] == 2) holdseen = true;
                }
                (void)holdseen;
                cmd(PVA_C_NEWS_TEXT, tq, 6);
                read_status();
                CHECK(ck[PVA_CK_ART_STATE] == PVA_ART_DONE && !(reply[2] & PVA_TXF_PART) && host_esp_ranges >= rg0 + 2 && waited,
                      "a big page in pieces (%u Range requests, flags %u, state %u)", host_esp_ranges - rg0, reply[2], ck[PVA_CK_ART_STATE]);
                {
                    uint8_t ta[6] = { 0, 1, 0, 0, 4, 78 };
                    cmd(PVA_C_NEWS_TEXT, ta, 6);
                    uint16_t nl = (uint16_t)(reply[0] | (reply[1] << 8));
                    bool first = false, last = false;
                    for (uint32_t i = 3; i + 14 < reply_len; i++) if (!memcmp(reply + i, "Paragraph 1 ", 12)) first = true;
                    ta[2] = (uint8_t)(nl - 4u); ta[3] = (uint8_t)((nl - 4u) >> 8);
                    cmd(PVA_C_NEWS_TEXT, ta, 6);
                    for (uint32_t i = 3; i + 14 < reply_len; i++) if (!memcmp(reply + i, "Paragraph 40 ", 13)) last = true;
                    CHECK(last && first && nl > 100, "all of it: paragraphs 1 to 40 (%u lines)", nl);
                }
                CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st16(PVA_ST_UNDERRUNS) == un1, "the radio played on without a gap (underruns %u -> %u)", un1, st16(PVA_ST_UNDERRUNS));
                // ESC while it comes in: what is there stays, as a part; Enter: the rest
                run_ms(8000);
                cmd_u8(PVA_C_NEWS_FETCH, 0); WAIT_NEWS();
                run_ms(8000);
                cmd_u8(PVA_C_NEWS_FULL, 0);
                WAIT_CLOCK(ck[PVA_CK_ART_WAIT] == 1, 20000);
                CHECK(cmd_u8(PVA_C_NEWS_FULL, 0xFF) == PVA_OK, "NEWS_FULL FFh stops it");
                read_clock();
                cmd(PVA_C_NEWS_TEXT, tq, 6);
                CHECK(ck[PVA_CK_ART_STATE] == PVA_ART_DONE && (reply[2] & PVA_TXF_PART), "stopped: a part (flags %u, state %u)", reply[2], ck[PVA_CK_ART_STATE]);
                cmd_u8(PVA_C_NEWS_FULL, 0);
                read_clock();
                CHECK(ck[PVA_CK_ART_STATE] == PVA_ART_LOADING, "Enter: the rest comes");
                WAIT_CLOCK(ck[PVA_CK_ART_STATE] != PVA_ART_LOADING, 180000);
                cmd(PVA_C_NEWS_TEXT, tq, 6);
                CHECK(ck[PVA_CK_ART_STATE] == PVA_ART_DONE && !(reply[2] & PVA_TXF_PART), "then all of it (flags %u)", reply[2]);
                // a server without Range: it sends all of it again, the part read is skipped
                host_esp_range = false;
                run_ms(8000);
                cmd_u8(PVA_C_NEWS_FETCH, 0); WAIT_NEWS();
                run_ms(8000);
                cmd_u8(PVA_C_NEWS_FULL, 0);
                WAIT_CLOCK(ck[PVA_CK_ART_STATE] != PVA_ART_LOADING, 180000);
                cmd(PVA_C_NEWS_TEXT, tq, 6);
                CHECK(ck[PVA_CK_ART_STATE] == PVA_ART_DONE && (reply[2] & PVA_TXF_PART), "no Range: a part (flags %u, KB %u)", reply[2], ck[PVA_CK_ART_KB] | (ck[PVA_CK_ART_KB + 1] << 8));
                cmd_u8(PVA_C_NEWS_FULL, 0);
                WAIT_CLOCK(ck[PVA_CK_ART_STATE] != PVA_ART_LOADING, 180000);
                cmd(PVA_C_NEWS_TEXT, tq, 6);
                CHECK(ck[PVA_CK_ART_STATE] == PVA_ART_DONE && !(reply[2] & PVA_TXF_PART), "Enter: all of it, the radio waiting (flags %u)", reply[2]);
                host_esp_range = true;
                host_esp_tls_ms = tls_ms;
                CHECK(st16(PVA_ST_UNDERRUNS) == un0 && host_audio_underruns == 0, "and played on from its buffer meanwhile (underruns %u)", st16(PVA_ST_UNDERRUNS) - un0);
                cmd(PVA_C_STOP, NULL, 0);
            }
            uint8_t kb[] = "news.feed1\0Down|http://down.test/rss";
            cmd(PVA_C_CFG_SET, kb, sizeof kb);
            cmd_u8(PVA_C_NEWS_FETCH, 0); WAIT_NEWS();
            CHECK(ck[PVA_CK_NEWS_STATE] == PVA_NEWS_FAILED && ck[PVA_CK_NEWS_ERR] == PVA_NERR_DNS, "an unknown host: FAILED, DNS (%u)", ck[PVA_CK_NEWS_ERR]);
            cmd(PVA_C_CFG_LOAD, NULL, 0);
        }

        printf("[directory troubles]\n");
        host_esp_api_down = true;
        cmd_u16(PVA_C_DIR_ENTER, 0);                // decades
        cmd_u16(PVA_C_DIR_ENTER, 0);                // home
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 2) == PVA_E_BUSY, "Top stations with the first server down");
        WAIT_LIST(20000);
        CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_DONE && strstr(host_esp_last_get, "Host: nl1.api.radio-browser.info") &&
              strstr(host_esp_last_api, "order=clickcount&reverse=true"), "the list came from a mirror (Content-Length reply)");
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(n == 8 && find(n, "Long URL") < 0 && find(n, "HLS One") < 0, "top: %d stations (HLS and over-long address left out)", n - 1);
        host_esp_api_down = false;
        host_esp_api_all_down = true;
        cmd_u16(PVA_C_DIR_ENTER, 0);                // home
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 3) == PVA_E_BUSY, "Most voted with all servers down");
        WAIT_LIST(40000);
        CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_FAILED && st[PVA_ST_DIR_ERR] == PVA_NERR_CONNECT, "FAILED, error %u", st[PVA_ST_DIR_ERR]);
        CHECK(cmd_str(PVA_C_DIR_OPEN, "") == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio"), "still at home");
        host_esp_api_all_down = false;
        {
            // the servers take HTTP but do not answer: the same server over HTTPS
            extern bool host_esp_api_http_silent;
            host_esp_api_http_silent = true;
            uint32_t tl0 = host_esp_tls_opens;
            CHECK(cmd_u16(PVA_C_DIR_ENTER, 3) == PVA_E_BUSY, "Most voted, the servers silent over HTTP");
            WAIT_LIST(40000);
            CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_DONE && host_esp_tls_opens > tl0 && strstr(host_esp_last_get, "Host: all.api.radio-browser.info"),
                  "it came over HTTPS from the first server (state %u, error %u)", st[PVA_ST_DIR_STATE], st[PVA_ST_DIR_ERR]);
            cmd_u16(PVA_C_DIR_ENTER, 0);            // home
            static const char sx[] = "https://secure.test/stream.mp3\0";
            cmd(PVA_C_PLAY_URL, sx, sizeof sx);
            run_ms(30000);
            uint32_t tlsms = host_esp_tls_ms;
            host_esp_tls_ms = 800;
            CHECK(cmd_u16(PVA_C_DIR_ENTER, 3) == PVA_E_BUSY, "again, while an https station plays");
            WAIT_LIST(40000);
            run_ms(5000);
            read_status();
            CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_DONE && st[PVA_ST_STATE] == PVA_STATE_PLAYING && st[PVA_ST_NET_STATE] == PVA_NET_STREAM,
                  "the station lent its TLS connection and plays on (list state %u, net state %u)", st[PVA_ST_DIR_STATE], st[PVA_ST_NET_STATE]);
            host_esp_tls_ms = tlsms;
            cmd(PVA_C_STOP, NULL, 0);
            host_esp_api_http_silent = false;
            cmd_u16(PVA_C_DIR_ENTER, 0);            // home
        }
        host_esp_api_bps = 500;                     // slow server: go back while it loads
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 2) == PVA_E_BUSY, "Top stations from a slow server");
        run_ms(1500);
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 0) == PVA_OK && !strcmp((char *)reply + 2, "/"), "'..' while loading: back on the card");
        run_ms(2000);
        read_status();
        CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_IDLE && host_esp_open_conns() == 0 && !host_esp_owned(), "fetch dropped, connection closed");
        host_esp_api_bps = 20000;
        CHECK(pva_dir_start(PVA_DIRQ_STATIONS, "/json/stations/search?tag=many&codec=MP3") == PVA_OK, "a long list");
        for (int w = 0; w < 200 && pva_dir_state() == PVA_DIRS_LOADING; w++) run_ms(50);
        CHECK(pva_dir_state() == PVA_DIRS_DONE && pva_dir_count() == 150 && !strcmp(pva_dir_name(149), "Many Station 149"),
              "150 stations kept (%u)", pva_dir_count());
        CHECK(cmd_str(PVA_C_DIR_OPEN, "/Internet radio") == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio"), "DIR_OPEN of the radio path");
        host_esp_present = false;
        cmd_str(PVA_C_DIR_OPEN, "/");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(n == 2 && !strcmp(ents[0].name, "MUSIC"), "no ESP: no radio entry in the root");
        host_esp_present = true;
        CHECK(cmd(PVA_C_DIAG, NULL, 0) == PVA_OK && strstr((char *)reply, "Radio list:"), "DIAG:\n%s", reply);

        printf("[more stations]\n");
        cmd_str(PVA_C_DIR_OPEN, "/Internet radio");
        cmd_u16(PVA_C_DIR_ENTER, 6);                // By genre
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        int rock = find(n, "Rock");
        CHECK(rock > 0 && cmd_u16(PVA_C_DIR_ENTER, (uint16_t)rock) == PVA_E_BUSY, "Rock");
        WAIT_LIST(15000);
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(n == 62 && !strcmp(ents[60].name, "Rock Station 059") && !strcmp(ents[61].name, "More stations...") && ents[61].type == PVA_DE_DIR,
              "60 stations and 'More stations...' (%d entries)", n);
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 61) == PVA_E_BUSY, "more: BUSY");
        WAIT_LIST(15000);
        CHECK(strstr(host_esp_last_api, "limit=60&offset=60"), "query %s", host_esp_last_api);
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(n == 122 && !strcmp(ents[61].name, "Rock Station 060") && !strcmp(ents[121].name, "More stations..."), "120 stations (%d entries)", n);
        cmd_u16(PVA_C_DIR_ENTER, 121);
        WAIT_LIST(15000);
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(n == 151 && !strcmp(ents[150].name, "Rock Station 149"), "all 150, no 'more' after the last page (%d entries)", n);
        CHECK(cmd_u16(PVA_C_ENTRY_INFO, 150) == PVA_OK && !strcmp((char *)reply, "http://radio.test/plain?n=149") &&
              !strcmp((char *)reply + strlen((char *)reply) + 1, "Rock Station 149"), "ENTRY_INFO: '%s'", reply);
        CHECK(cmd_u16(PVA_C_PLAY_INDEX, 150) == PVA_OK, "play the last one");
        run_ms(3000);
        read_status();
        CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st16(PVA_ST_PL_COUNT) == 150 && st16(PVA_ST_PL_POS) == 149,
              "plays %u of %u", st16(PVA_ST_PL_POS) + 1, st16(PVA_ST_PL_COUNT));
        CHECK(cmd_u16(PVA_C_ENTRY_INFO, 0xFFFF) == PVA_OK && !strcmp((char *)reply, "http://radio.test/plain?n=149") &&
              !strcmp((char *)reply + strlen((char *)reply) + 1, "Rock Station 149"), "ENTRY_INFO of the playing stream");
        cmd(PVA_C_STOP, NULL, 0);

        printf("[favourites]\n");
        cmd_str(PVA_C_DIR_OPEN, "/Internet radio");
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 1) == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio/Favourites") &&
              reply[0] == 1, "no favourites file: only '..'");
        {
            FIL f;
            UINT bw;
            const char *t = "#EXTM3U\r\n#EXTINF:-1,My Fav\r\nhttp://radio.test/plain\r\nhttp://radio2.test:8000/live\r\n# a note\r\n";
            CHECK(f_open(&f, "0:/RADIOFAV.M3U", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK && f_write(&f, t, strlen(t), &bw) == FR_OK && f_close(&f) == FR_OK,
                  "(write RADIOFAV.M3U as PicoPlay would)");
        }
        CHECK(cmd_str(PVA_C_DIR_OPEN, "") == PVA_OK, "reopen reads the file again");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        for (int i = 0; i < n; i++) printf("    %d %s\n", ents[i].type, ents[i].name);
        CHECK(n == 3 && !strcmp(ents[1].name, "My Fav") && ents[1].type == PVA_DE_URL && !strcmp(ents[2].name, "http://radio2.test:8000/live"),
              "2 favourites (a nameless one shows its address)");
        CHECK(cmd_u16(PVA_C_ENTRY_INFO, 2) == PVA_OK && !strcmp((char *)reply, "http://radio2.test:8000/live"), "ENTRY_INFO of a favourite");
        CHECK(cmd_u16(PVA_C_PLAY_INDEX, 1) == PVA_OK, "play 'My Fav'");
        run_ms(3000);
        read_status();
        CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st16(PVA_ST_PL_COUNT) == 2 && !strcmp(ststr(PVA_ST_ALBUM), "My Fav"),
              "plays '%s', play list of 2", ststr(PVA_ST_ALBUM));
        CHECK(cmd(PVA_C_NEXT, NULL, 0) == PVA_OK, "next favourite");
        run_ms(3000);
        read_status();
        CHECK(st16(PVA_ST_PL_POS) == 1 && st[PVA_ST_STATE] == PVA_STATE_PLAYING, "plays the second");
        cmd(PVA_C_STOP, NULL, 0);
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 0) == PVA_OK && !strcmp((char *)reply + 2, "/Internet radio"), "'..' from favourites: home");
        cmd_str(PVA_C_DIR_OPEN, "/");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(find(n, "RADIOFAV.M3U") > 0, "the file is in the root too (an M3U)");
        f_unlink("0:/RADIOFAV.M3U");
        run_ms(500);

        printf("[directory: a big reply that ends with the close, quick station changes]\n");
        host_esp_close_silent = true;
        cmd_str(PVA_C_DIR_OPEN, "/Internet radio");
        uint32_t ta = pva_hal_millis(), sa = host_esp_state_asks;
        CHECK(cmd_u16(PVA_C_DIR_ENTER, 2) == PVA_E_BUSY, "Top stations (no Content-Length, the ESP does not report the close)");
        WAIT_LIST(15000);
        CHECK(st[PVA_ST_DIR_STATE] == PVA_DIRS_DONE && pva_hal_millis() - ta < 3000, "in after %lu ms (the JSON end is seen; TCP_STATE asked %u times)",
              (unsigned long)(pva_hal_millis() - ta), host_esp_state_asks - sa);
        cmd_u16(PVA_C_DIR_ENTER, 0);
        cmd_u16(PVA_C_DIR_ENTER, 5);                // By country (kept)
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        int nc = find(n, "New Caledonia");
        CHECK(nc > 0 && cmd_u16(PVA_C_DIR_ENTER, (uint16_t)nc) == PVA_E_BUSY, "New Caledonia");
        WAIT_LIST(15000);
        cmd_str(PVA_C_DIR_OPEN, "");
        n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
        CHECK(n == 4 && find(n, "NC Secure") == 1, "3 stations");
        host_esp_tls_ms = 5000;                     // a slow TLS server
        uint32_t o0 = host_esp_opens;
        for (int k = 1; k <= 3; k++)
        {
            uint32_t tc = pva_hal_millis();
            int r = cmd_u16(PVA_C_PLAY_INDEX, (uint16_t)k);
            CHECK(r == PVA_OK && pva_hal_millis() - tc < 200, "play station %d: answered in %lu ms", k, (unsigned long)(pva_hal_millis() - tc));
            run_ms(k < 3 ? 1500 : 12000);           // change while the TLS open is still going on
        }
        read_status();
        CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && st[PVA_ST_NET_STATE] == PVA_NET_STREAM && !strcmp(ststr(PVA_ST_ALBUM), "NC Live"),
              "the last one plays ('%s', net %u)", ststr(PVA_ST_ALBUM), st[PVA_ST_NET_STATE]);
        CHECK(host_esp_open_conns() == 1, "one connection open (%d, %u opened)", host_esp_open_conns(), host_esp_opens - o0);
        uint16_t u1 = st16(PVA_ST_UNDERRUNS);
        host_esp_delay_next_rcv = 2600;             // an answer comes after its time-out
        run_ms(8000);
        read_status();
        extern uint32_t pva_esp_timeouts, pva_esp_drained;
        CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING && pva_esp_timeouts >= 1 && host_esp_open_conns() == 1,
              "a late answer is waited for and dropped (%u time-outs, %u stray bytes), still playing, underruns %u -> %u",
              pva_esp_timeouts, pva_esp_drained, u1, st16(PVA_ST_UNDERRUNS));
        host_esp_tls_ms = 1800;
        cmd(PVA_C_STOP, NULL, 0);
        run_ms(1000);
    }
    CHECK(cmd(PVA_C_LOG, NULL, 0) == PVA_OK && strstr((char *)reply, "stream:"), "the log tells the story:\n%s", reply);
    run_ms(100);
    CHECK(host_esp_open_conns() == 0 && !host_esp_owned() && host_esp_opens == host_esp_closes,
          "no ESP connection left open (%u opened, %u closed, %d open)", host_esp_opens, host_esp_closes, host_esp_open_conns());
    host_audio_unlimited = true;

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
