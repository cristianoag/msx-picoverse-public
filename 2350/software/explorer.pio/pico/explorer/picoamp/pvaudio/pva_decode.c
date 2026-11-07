// MSX PICOVERSE PROJECT
// pva_decode.c - PicoVerse Audio Engine: MP3/WAV decoding on core 1.
//
// MP3 uses the Helix fixed-point decoder (picomp3lib), MPEG-1/2/2.5 layer III.
// WAV: PCM 8/16 bit, mono/stereo, 8-48 kHz.
//
// pva_dec_step() decodes at most one MP3 frame (or one WAV block) and only
// when the audio queue has a free buffer, so core 1 keeps servicing Nextor's
// sector requests between frames. Reads from the microSD card are at most
// PVA_READ_CHUNK bytes.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "ff.h"
#include "mp3dec.h"
#include "pva_internal.h"
#include "pva_mp3_alloc.h"
#ifndef PVA_HOST
#include "../mp3_alloc_dispatch.h"
#endif

#define MAX_FRAME_SAMPLES   1152u
#define INBUF_SIZE          (8u * 1024u)
#define PVA_READ_CHUNK      (4u * 1024u)
#define MP3_POOL_SIZE       (24u * 1024u + 512u)   // Helix needs 23.9 KB on the RP2350
#define TAG_SCAN_MAX        (64u * 1024u)   // ID3v2 bytes we are willing to read for tags

// ---------------- decoder memory pool (Helix MPDEC_ALLOCATOR) ----------------
static uint8_t *mp3_pool;
static size_t   mp3_pool_used;

void *pva_mp3_alloc(size_t size)
{
    size = (size + 7u) & ~(size_t)7u;
    if (!mp3_pool || mp3_pool_used + size > MP3_POOL_SIZE)
        return NULL;
    void *p = mp3_pool + mp3_pool_used;
    mp3_pool_used += size;
    memset(p, 0, size);
    return p;
}

// ---------------- state ----------------
static HMP3Decoder decoder;
static uint8_t  *inbuf;
static uint32_t  in_len, in_pos;
static FIL      *filep;               // in the arena
#define file (*filep)
static bool      file_open, file_eof;
static uint32_t  file_size;
static uint32_t  audio_start;          // first byte after the ID3v2 tag
static uint32_t  audio_end;            // before an ID3v1 tag
static uint8_t   format;
static uint32_t  rate = 44100;
static uint64_t  samples_out;
static uint32_t  seek_base_s;          // elapsed time at the last seek
static uint8_t   bad_frames;
static bool      stream;              // input from pva_net_read() instead of a file
static MP3FrameInfo last_fi;
static uint8_t   last_fr;

static uint16_t  wav_channels, wav_bits;
static uint32_t  wav_data_start, wav_data_size, wav_remaining;

// MP3 duration / seeking
static uint32_t  mp3_frames_total;     // from a Xing/Info/VBRI header, 0 = unknown
static uint8_t   xing_toc[100];
static bool      have_toc;
static uint32_t  total_s;
static uint32_t  cbr_kbps;

static int16_t  *pcm;                 // MAX_FRAME_SAMPLES * 2, in the arena
static int32_t   gain_q15 = 32768;
static int32_t   bal_l_q15 = 32768, bal_r_q15 = 32768;   // balance: the far side is turned down
static uint8_t   vu_l, vu_r;
static uint32_t  vu_ms;

static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t syncsafe(const uint8_t *p) { return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) | ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F); }

// ---------------- helpers shared with the engine ----------------
int pva_kind_from_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    char e[5] = { 0 };
    for (int i = 0; i < 4 && dot[1 + i]; i++)
    {
        char c = dot[1 + i];
        e[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if (dot[1] && dot[2] && dot[3] && dot[4]) return 0;
    if (!strcmp(e, "MP3")) return PVA_DE_MP3;
    if (!strcmp(e, "WAV")) return PVA_DE_WAV;
    if (!strcmp(e, "M3U")) return PVA_DE_M3U;
    return 0;
}

// Latin-1 / UTF-8 to plain ASCII for the MSX font: accents are dropped.
char pva_fold_ascii(uint32_t c)
{
    if (c >= 0x20 && c < 0x7F) return (char)c;
    if (c == 0xA0) return ' ';
    if (c == 0x2022 || c == 0xB7) return '*';
    if (c == 0x2032 || c == 0xB4) return '\'';
    if (c == 0x200B || c == 0x200C || c == 0x200D || c == 0xFEFF || c == 0xAD) return 0;   // (zero width)
    static const char map[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (c >= 0xC0 && c <= 0xFF) return map[c - 0xC0];
    if (c == 0x2018 || c == 0x2019) return '\'';
    if (c == 0x201C || c == 0x201D) return '"';
    if (c == 0x2013 || c == 0x2014) return '-';
    return c ? '?' : 0;
}

void pva_copy_ascii(char *dst, size_t size, const char *src, size_t src_len)
{
    size_t o = 0;
    for (size_t i = 0; i < src_len && src[i] && o + 1 < size; )
    {
        uint8_t b = (uint8_t)src[i];
        uint32_t c = b;
        if (b >= 0xC0 && b < 0xE0 && i + 1 < src_len && ((uint8_t)src[i + 1] & 0xC0) == 0x80)
        {
            c = ((uint32_t)(b & 0x1F) << 6) | ((uint8_t)src[i + 1] & 0x3F);
            i += 2;
        }
        else if (b >= 0xE0 && b < 0xF0 && i + 2 < src_len)
        {
            c = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)((uint8_t)src[i + 1] & 0x3F) << 6) | ((uint8_t)src[i + 2] & 0x3F);
            i += 3;
        }
        else
            i++;
        char a = pva_fold_ascii(c);
        if (a) dst[o++] = a;
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

// ID3v2 text frame body (encoding byte + text) to ASCII
static void id3_text(char *dst, size_t size, const uint8_t *p, uint32_t len)
{
    if (len < 1) { dst[0] = 0; return; }
    uint8_t enc = p[0];
    p++; len--;
    if (enc == 1 || enc == 2)
    {
        bool be = (enc == 2);
        if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = true; p += 2; len -= 2; }
        else if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = false; p += 2; len -= 2; }
        size_t o = 0;
        for (uint32_t i = 0; i + 1 < len && o + 1 < size; i += 2)
        {
            uint32_t c = be ? ((uint32_t)p[i] << 8 | p[i + 1]) : ((uint32_t)p[i + 1] << 8 | p[i]);
            if (!c) break;
            char a = pva_fold_ascii(c);
            if (a) dst[o++] = a;
        }
        while (o > 0 && dst[o - 1] == ' ') o--;
        dst[o] = 0;
    }
    else if (enc == 3)
        pva_copy_ascii(dst, size, (const char *)p, len);
    else
    {
        size_t o = 0;
        for (uint32_t i = 0; i < len && p[i] && o + 1 < size; i++)
        {
            char a = pva_fold_ascii(p[i]);
            if (a) dst[o++] = a;
        }
        while (o > 0 && dst[o - 1] == ' ') o--;
        dst[o] = 0;
    }
}

// Read the ID3v2 tag (tags only, the audio position is set by the caller).
static void parse_id3v2(pva_track_info_t *info, uint32_t tag_size, uint8_t ver, uint8_t flags)
{
    uint32_t pos = 10;
    uint32_t end = 10 + tag_size;
    if (end > TAG_SCAN_MAX) end = TAG_SCAN_MAX;
    if (flags & 0x40)                       // extended header: skip it
    {
        uint8_t eh[4];
        UINT br;
        if (f_lseek(&file, 10) != FR_OK || f_read(&file, eh, 4, &br) != FR_OK || br != 4) return;
        uint32_t es = (ver == 4) ? syncsafe(eh) : rd32be(eh) + 4;
        pos += es;
    }
    while (pos + 10 <= end)
    {
        uint8_t fh[10];
        UINT br;
        if (f_lseek(&file, pos) != FR_OK || f_read(&file, fh, 10, &br) != FR_OK || br != 10) return;
        if (fh[0] == 0) return;             // padding
        uint32_t fs = (ver == 4) ? syncsafe(fh + 4) : rd32be(fh + 4);
        if (fs == 0 || pos + 10 + fs > 10 + tag_size) return;
        char *dst = NULL;
        size_t dsz = 0;
        if (!memcmp(fh, "TIT2", 4)) { dst = info->title; dsz = sizeof info->title; }
        else if (!memcmp(fh, "TPE1", 4)) { dst = info->artist; dsz = sizeof info->artist; }
        else if (!memcmp(fh, "TALB", 4)) { dst = info->album; dsz = sizeof info->album; }
        if (dst)
        {
            uint8_t body[200];
            uint32_t n = fs < sizeof body ? fs : sizeof body;
            if (f_read(&file, body, n, &br) == FR_OK && br == n)
            {
                id3_text(dst, dsz, body, n);
                if (dst == info->title && dst[0]) info->tagged = true;
            }
        }
        pos += 10 + fs;
    }
}

static void parse_id3v1(pva_track_info_t *info)
{
    if (file_size < 128 + audio_start) return;
    uint8_t t[128];
    UINT br;
    if (f_lseek(&file, file_size - 128) != FR_OK || f_read(&file, t, 128, &br) != FR_OK || br != 128) return;
    if (memcmp(t, "TAG", 3)) return;
    audio_end = file_size - 128;
    if (info->tagged) return;
    char tmp[31];
    memcpy(tmp, t + 3, 30); tmp[30] = 0;
    pva_copy_ascii(info->title, sizeof info->title, tmp, 30);
    memcpy(tmp, t + 33, 30); tmp[30] = 0;
    pva_copy_ascii(info->artist, sizeof info->artist, tmp, 30);
    memcpy(tmp, t + 63, 30); tmp[30] = 0;
    pva_copy_ascii(info->album, sizeof info->album, tmp, 30);
    if (info->title[0]) info->tagged = true;
}

// ---------------- input ----------------
static void close_file(void)
{
    if (file_open)
    {
        f_close(&file);
        file_open = false;
    }
    in_len = in_pos = 0;
    file_eof = false;
}

static void compact_input(void)
{
    if (in_pos == 0) return;
    uint32_t remaining = in_len - in_pos;
    if (remaining) memmove(inbuf, inbuf + in_pos, remaining);
    in_len = remaining;
    in_pos = 0;
}

static void refill(void)
{
    if (stream)
    {
        if ((in_len - in_pos) >= INBUF_SIZE / 2u) return;
        compact_input();
        in_len += (uint32_t)pva_net_read(inbuf + in_len, INBUF_SIZE - in_len);
        if (pva_net_ended() && in_len == in_pos) file_eof = true;
        return;
    }
    if (!file_open || file_eof || (in_len - in_pos) >= INBUF_SIZE / 2u)
        return;
    compact_input();
    uint32_t space = INBUF_SIZE - in_len;
    if (space > PVA_READ_CHUNK) space = PVA_READ_CHUNK;
    uint32_t pos = (uint32_t)f_tell(&file);
    if (audio_end && pos + space > audio_end)
        space = pos < audio_end ? audio_end - pos : 0;
    UINT br = 0;
    FRESULT fr = space ? f_read(&file, inbuf + in_len, space, &br) : FR_OK;
    if (fr != FR_OK)
    {
        last_fr = (uint8_t)fr;
        pva_log("read error %d at %lu", (int)fr, (unsigned long)pos);
        file_eof = true;
    }
    else if (br == 0)
        file_eof = true;
    in_len += br;
}

// ---------------- MP3 ----------------
static bool reset_decoder(void)
{
    mp3_pool_used = 0;
    decoder = MP3InitDecoder();
    bad_frames = 0;
    return decoder != NULL;
}

// Look at the first frame: Xing/Info or VBRI header, else CBR estimate.
static void mp3_probe(pva_track_info_t *info)
{
    mp3_frames_total = 0;
    have_toc = false;
    cbr_kbps = 0;
    refill();
    int off = MP3FindSyncWord(inbuf + in_pos, (int)(in_len - in_pos));
    if (off < 0) return;
    uint8_t *h = inbuf + in_pos + off;
    uint32_t avail = in_len - in_pos - (uint32_t)off;
    MP3FrameInfo fi;
    if (MP3GetNextFrameInfo(decoder, &fi, h) != ERR_MP3_NONE) return;
    info->sample_rate = (uint32_t)fi.samprate;
    info->channels = (uint8_t)fi.nChans;
    info->bitrate_kbps = (uint32_t)fi.bitrate / 1000u;
    bool mpeg1 = (h[1] & 0x18) == 0x18;
    bool mono = (h[3] & 0xC0) == 0xC0;
    uint32_t side = mpeg1 ? (mono ? 17u : 32u) : (mono ? 9u : 17u);
    uint32_t samples_per_frame = mpeg1 ? 1152u : 576u;
    uint32_t xo = 4u + side;
    if (avail > xo + 120 && (!memcmp(h + xo, "Xing", 4) || !memcmp(h + xo, "Info", 4)))
    {
        uint32_t fl = rd32be(h + xo + 4);
        uint32_t p = xo + 8;
        if (fl & 1) { mp3_frames_total = rd32be(h + p); p += 4; }
        if (fl & 2) p += 4;
        if (fl & 4) { memcpy(xing_toc, h + p, 100); have_toc = true; }
    }
    else if (avail > 36 + 26 && !memcmp(h + 36, "VBRI", 4))
        mp3_frames_total = rd32be(h + 36 + 14);
    if (mp3_frames_total && fi.samprate > 0)
        total_s = (uint32_t)(((uint64_t)mp3_frames_total * samples_per_frame) / (uint32_t)fi.samprate);
    else if (fi.bitrate > 0)
    {
        cbr_kbps = (uint32_t)fi.bitrate / 1000u;
        uint32_t bytes = (audio_end ? audio_end : file_size) - audio_start;
        total_s = (uint32_t)(((uint64_t)bytes * 8u) / (uint32_t)fi.bitrate);
    }
    if (total_s && mp3_frames_total)
        info->bitrate_kbps = (uint32_t)(((uint64_t)((audio_end ? audio_end : file_size) - audio_start) * 8u) / total_s / 1000u);
    info->total_seconds = total_s;
    info->seekable = total_s > 0;
}

static int produce_mp3(void)
{
    for (int attempt = 0; attempt < 6; attempt++)
    {
        refill();
        uint32_t avail = in_len - in_pos;
        if (avail < 4u)
            return (file_eof || (stream && pva_net_ended())) ? -1 : 0;
        int off = MP3FindSyncWord(inbuf + in_pos, (int)avail);
        if (off < 0)
        {
            in_pos = in_len - 3u;
            if (file_eof) return -1;
            continue;
        }
        in_pos += (uint32_t)off;
        avail -= (uint32_t)off;
        if (avail < 1800u && !file_eof)
        {
            refill();
            avail = in_len - in_pos;
            // a stream decodes only whole frames (the largest is 1441 bytes)
            if (stream && avail < 1800u) return pva_net_ended() ? -1 : 0;
        }
        unsigned char *p = inbuf + in_pos;
        int left = (int)avail;
        int err = MP3Decode(decoder, &p, &left, pcm, 0);
        in_pos = (uint32_t)(p - inbuf);
        if (err == ERR_MP3_NONE)
        {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(decoder, &fi);
            last_fi = fi;
            if (fi.nChans < 1 || fi.outputSamps <= 0) continue;
            uint32_t frames = (uint32_t)fi.outputSamps / (uint32_t)fi.nChans;
            if (frames > MAX_FRAME_SAMPLES) frames = MAX_FRAME_SAMPLES;
            if (fi.nChans == 1)
                for (int i = (int)frames - 1; i >= 0; i--)
                    pcm[2 * i] = pcm[2 * i + 1] = pcm[i];
            if (fi.samprate > 0) rate = (uint32_t)fi.samprate;
            bad_frames = 0;
            return (int)frames;
        }
        if (err == ERR_MP3_INDATA_UNDERFLOW)
        {
            if (file_eof) return -1;
            continue;
        }
        if (err == ERR_MP3_MAINDATA_UNDERFLOW)
            continue;
        if (in_pos < in_len) in_pos++;
        if (++bad_frames > 64u) return -1;
    }
    return 0;
}

// ---------------- WAV ----------------
static bool open_wav(pva_track_info_t *info)
{
    uint8_t h[12];
    UINT br;
    if (f_read(&file, h, 12, &br) != FR_OK || br != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return false;
    bool have_fmt = false;
    for (int guard = 0; guard < 32; guard++)
    {
        uint8_t ch[8];
        if (f_read(&file, ch, 8, &br) != FR_OK || br != 8) return false;
        uint32_t size = rd32le(ch + 4);
        if (!memcmp(ch, "fmt ", 4))
        {
            uint8_t fmt[16];
            if (size < 16 || f_read(&file, fmt, 16, &br) != FR_OK || br != 16) return false;
            uint16_t tag = rd16le(fmt);
            wav_channels = rd16le(fmt + 2);
            rate = rd32le(fmt + 4);
            wav_bits = rd16le(fmt + 14);
            if ((tag != 1 && tag != 0xFFFE) || wav_channels < 1 || wav_channels > 2 ||
                (wav_bits != 8 && wav_bits != 16) || rate < 8000 || rate > 48000)
                return false;
            have_fmt = true;
            if (f_lseek(&file, f_tell(&file) + (size - 16u) + (size & 1u)) != FR_OK) return false;
        }
        else if (!memcmp(ch, "data", 4))
        {
            if (!have_fmt) return false;
            wav_data_start = (uint32_t)f_tell(&file);
            wav_data_size = size;
            if (wav_data_start + wav_data_size > file_size) wav_data_size = file_size - wav_data_start;
            wav_remaining = wav_data_size;
            uint32_t bpf = (uint32_t)wav_channels * (wav_bits / 8u);
            info->sample_rate = rate;
            info->channels = (uint8_t)wav_channels;
            info->bitrate_kbps = rate * bpf * 8u / 1000u;
            info->total_seconds = wav_data_size / bpf / rate;
            info->seekable = true;
            return true;
        }
        else if (f_lseek(&file, f_tell(&file) + size + (size & 1u)) != FR_OK)
            return false;
    }
    return false;
}

static int produce_wav(uint32_t max_frames)
{
    if (wav_remaining == 0) return -1;
    uint32_t bpf = (uint32_t)wav_channels * (wav_bits / 8u);
    uint32_t frames = max_frames > MAX_FRAME_SAMPLES ? MAX_FRAME_SAMPLES : max_frames;
    uint32_t bytes = frames * bpf;
    if (bytes > wav_remaining) bytes = wav_remaining - (wav_remaining % bpf);
    if (bytes > PVA_READ_CHUNK) bytes = PVA_READ_CHUNK - (PVA_READ_CHUNK % bpf);
    UINT br = 0;
    FRESULT fr = FR_OK;
    if (bytes == 0 || (fr = f_read(&file, inbuf, bytes, &br)) != FR_OK || br < bpf)
    {
        if (fr != FR_OK) { last_fr = (uint8_t)fr; pva_log("wav read error %d", (int)fr); }
        return -1;
    }
    wav_remaining -= br;
    frames = br / bpf;
    for (uint32_t i = 0; i < frames; i++)
    {
        const uint8_t *s = inbuf + i * bpf;
        int16_t l, r;
        if (wav_bits == 16)
        {
            l = (int16_t)rd16le(s);
            r = (wav_channels == 2) ? (int16_t)rd16le(s + 2) : l;
        }
        else
        {
            l = (int16_t)(((int)s[0] - 128) << 8);
            r = (wav_channels == 2) ? (int16_t)(((int)s[1] - 128) << 8) : l;
        }
        pcm[2 * i] = l;
        pcm[2 * i + 1] = r;
    }
    return (int)frames;
}

// ---------------- volume / VU ----------------
// 0..100 -> Q15 gain, about 0.5 dB per step from -50 dB to 0 dB, 0 = mute.
// 0 = left only, 50 = centre, 100 = right only (the other side goes down
// linearly; the near side stays at full level)
void pva_dec_set_balance(uint8_t bal)
{
    if (bal > 100) bal = 100;
    bal_l_q15 = bal <= 50 ? 32768 : (int32_t)(32768 * (100 - bal) / 50);
    bal_r_q15 = bal >= 50 ? 32768 : (int32_t)(32768 * bal / 50);
}

void pva_dec_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    if (vol == 0) { gain_q15 = 0; return; }
    // 10^(-(100-vol)/40): computed with integer steps of 1 dB / 2
    int32_t g = 32768;
    for (int i = vol; i < 100; i++)
        g = (g * 30935) >> 15;              // * 10^(-0.5/20)
    gain_q15 = g;
}

static uint8_t vu_level(int32_t peak)
{
    // 0..15, roughly 3 dB per step
    uint8_t lvl = 0;
    int32_t t = 23170;                      // -3 dB of full scale
    while (lvl < 15 && peak < t) { t = (t * 23198) >> 15; lvl++; }
    return (uint8_t)(15 - lvl);
}

static void apply_gain_and_vu(int16_t *buf, uint32_t frames)
{
    int32_t pk_l = 0, pk_r = 0;
    int32_t gl = (gain_q15 * bal_l_q15) >> 15, gr = (gain_q15 * bal_r_q15) >> 15;
    for (uint32_t i = 0; i < frames; i++)
    {
        int32_t l = ((int32_t)buf[2 * i] * gl) >> 15;
        int32_t r = ((int32_t)buf[2 * i + 1] * gr) >> 15;
        buf[2 * i] = (int16_t)l;
        buf[2 * i + 1] = (int16_t)r;
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > pk_l) pk_l = l;
        if (r > pk_r) pk_r = r;
    }
    uint8_t nl = pk_l ? vu_level(pk_l) : 0, nr = pk_r ? vu_level(pk_r) : 0;
    uint32_t now = pva_hal_millis();
    // fast attack, ~1 step per 40 ms release
    if (nl >= vu_l) vu_l = nl;
    if (nr >= vu_r) vu_r = nr;
    if (now - vu_ms >= 40u)
    {
        vu_ms = now;
        if (vu_l > nl) vu_l--;
        if (vu_r > nr) vu_r--;
    }
}

void pva_dec_vu(uint8_t *l, uint8_t *r) { *l = vu_l; *r = vu_r; }

// ---------------- public ----------------
bool pva_dec_init(void)
{
#ifndef PVA_HOST
    pv_mp3_alloc_fn = pva_mp3_alloc;
#endif
    inbuf = pva_alloc(INBUF_SIZE);
    mp3_pool = pva_alloc(MP3_POOL_SIZE);
    filep = pva_alloc(sizeof(FIL));
    pcm = pva_alloc(MAX_FRAME_SAMPLES * 2u * sizeof(int16_t));
    pva_dec_set_volume(80);
    return inbuf && mp3_pool && filep && pcm;
}

void pva_dec_close(void)
{
    close_file();
    stream = false;
    format = 0;
    vu_l = vu_r = 0;
}

bool pva_dec_active(void) { return format != 0; }
uint8_t pva_dec_last_fresult(void) { return last_fr; }

int pva_dec_open(const char *path, pva_track_info_t *info)
{
    pva_dec_close();
    pva_eq_reset();
    memset(info, 0, sizeof *info);
    int kind = pva_kind_from_name(path);
    if (kind != PVA_DE_MP3 && kind != PVA_DE_WAV)
        return PVA_E_FORMAT;
    FRESULT fr = f_open(&file, path, FA_READ);
    if (fr != FR_OK)
    {
        last_fr = (uint8_t)fr;
        pva_log("open %d: %.36s", (int)fr, path);
        return (fr == FR_NO_FILE || fr == FR_NO_PATH || fr == FR_INVALID_NAME) ? PVA_E_NOTFOUND : PVA_E_IO;
    }
    file_open = true;
    file_eof = false;
    file_size = (uint32_t)f_size(&file);
    in_len = in_pos = 0;
    samples_out = 0;
    seek_base_s = 0;
    total_s = 0;
    audio_start = 0;
    audio_end = 0;

    if (kind == PVA_DE_WAV)
    {
        if (!open_wav(info)) { close_file(); return PVA_E_FORMAT; }
    }
    else
    {
        uint8_t h[10];
        UINT br = 0;
        if (f_read(&file, h, 10, &br) == FR_OK && br == 10 && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4)
        {
            uint32_t size = syncsafe(h + 6);
            audio_start = 10u + size + ((h[5] & 0x10) ? 10u : 0u);
            if (h[3] >= 3) parse_id3v2(info, size, h[3], h[5]);
            if (audio_start >= file_size) audio_start = 0;
        }
        parse_id3v1(info);
        if (f_lseek(&file, audio_start) != FR_OK) { close_file(); return PVA_E_IO; }
        if (!reset_decoder()) { close_file(); pva_log("decoder alloc failed"); return PVA_E_IO; }
        mp3_probe(info);
        if (info->sample_rate == 0) { close_file(); return PVA_E_FORMAT; }
        rate = info->sample_rate;
    }
    info->format = (uint8_t)kind;
    format = (uint8_t)kind;
    return PVA_OK;
}

// MP3 from the network buffer: no tags, no length, no seeking. Rate, channels
// and bitrate are known after the first frame (pva_dec_frame_info).
int pva_dec_open_stream(pva_track_info_t *info)
{
    pva_dec_close();
    pva_eq_reset();
    memset(info, 0, sizeof *info);
    in_len = in_pos = 0;
    samples_out = 0;
    seek_base_s = 0;
    total_s = 0;
    audio_start = audio_end = 0;
    file_eof = false;
    memset(&last_fi, 0, sizeof last_fi);
    if (!reset_decoder()) { pva_log("decoder alloc failed"); return PVA_E_IO; }
    stream = true;
    format = PVA_DE_MP3;
    info->format = PVA_DE_URL;
    return PVA_OK;
}

bool pva_dec_is_stream(void) { return stream; }

void pva_dec_frame_info(pva_track_info_t *info)
{
    if (last_fi.samprate <= 0) return;
    info->sample_rate = (uint32_t)last_fi.samprate;
    info->channels = (uint8_t)last_fi.nChans;
    info->bitrate_kbps = (uint32_t)last_fi.bitrate / 1000u;
}

int pva_dec_step(void)
{
    if (!format) return -1;
    uint32_t max_frames = 0;
    int16_t *out = pva_hal_audio_acquire(&max_frames);
    if (!out) return 0;
    int n = (format == PVA_DE_MP3) ? produce_mp3() : produce_wav(max_frames);
    if (n <= 0) return n;
    if ((uint32_t)n > max_frames) n = (int)max_frames;
    pva_eq_process(pcm, (uint32_t)n, rate);
    pva_viz_feed(pcm, (uint32_t)n, rate);
    apply_gain_and_vu(pcm, (uint32_t)n);
    memcpy(out, pcm, (size_t)n * 4u);
    pva_hal_audio_commit((uint32_t)n, rate);
    samples_out += (uint32_t)n;
    return 1;
}

uint32_t pva_dec_elapsed(void)
{
    return seek_base_s + (rate ? (uint32_t)(samples_out / rate) : 0u);
}

int pva_dec_seek(uint32_t seconds)
{
    if (!format || !file_open || stream) return PVA_E_STATE;
    if (total_s && seconds >= total_s) seconds = total_s ? total_s - 1u : 0u;
    uint32_t target;
    if (format == PVA_DE_WAV)
    {
        uint32_t bpf = (uint32_t)wav_channels * (wav_bits / 8u);
        uint64_t off = (uint64_t)seconds * rate * bpf;
        if (off > wav_data_size) off = wav_data_size;
        target = wav_data_start + (uint32_t)off;
        wav_remaining = wav_data_size - (uint32_t)off;
    }
    else
    {
        if (!total_s) return PVA_E_STATE;
        uint32_t bytes = (audio_end ? audio_end : file_size) - audio_start;
        uint32_t off;
        if (have_toc)
        {
            float pct = (float)seconds * 100.0f / (float)total_s;
            int a = (int)pct;
            if (a > 99) a = 99;
            float fa = xing_toc[a];
            float fb = (a < 99) ? xing_toc[a + 1] : 256.0f;
            float fx = fa + (fb - fa) * (pct - (float)a);
            off = (uint32_t)((fx / 256.0f) * (float)bytes);
        }
        else
            off = (uint32_t)(((uint64_t)seconds * bytes) / total_s);
        target = audio_start + off;
    }
    FRESULT fr = f_lseek(&file, target);
    if (fr != FR_OK) { last_fr = (uint8_t)fr; return PVA_E_IO; }
    in_len = in_pos = 0;
    file_eof = false;
    if (format == PVA_DE_MP3) reset_decoder();
    samples_out = 0;
    seek_base_s = seconds;
    return PVA_OK;
}
