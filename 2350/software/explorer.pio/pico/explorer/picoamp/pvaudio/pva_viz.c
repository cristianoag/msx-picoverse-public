// MSX PICOVERSE PROJECT
// pva_viz.c - PicoVerse Audio Engine: data for a spectrum analyser, VU meters
// and an oscilloscope (core 1).
//
// The decoded sound (after the equalizer, before the volume, so the display
// does not change with the volume) is kept in a short ring. About 50 times a
// second the newest 512 samples go through a Hann window and an FFT (the bands
// below 1.5 kHz use a second FFT of the sound at a quarter of the rate); the
// magnitudes are grouped into PVA_VIZ_BANDS bands spaced evenly on a log scale
// from 60 Hz to 16 kHz, and turned into levels 0..PVA_VIZ_MAX (2 dB a step,
// so 0..31 spans 62 dB). A bar rises at once and falls a step per update; a
// peak stays a moment, then falls. Everything goes straight into the register
// window at PVA_R_VIZ, so a player only copies bytes and draws.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <math.h>
#include <string.h>
#include "pva_internal.h"

#define N           512u            // FFT size: 86 Hz bins, 11.6 ms at 44.1 kHz
#define RING        1024u
#define DECIM       4u              // the low bands come from a second FFT at a quarter of the
                                    // rate (21.5 Hz bins), so they are spaced on a log scale too
#define LOW_MAX_HZ  1500.0f         // bands up to here use it
#define UPDATE_MS   20u
#define PEAK_HOLD   12u             // updates a peak stays
#define F_LO        60.0f
#define F_HI        16000.0f

static int16_t  ring[RING];         // mono
static uint32_t wpos;
static uint32_t fed;                // samples since the last update
static uint32_t last_rate = 44100;
static uint32_t last_ms;
static uint8_t  seq;
static uint8_t  level[PVA_VIZ_BANDS], peak[PVA_VIZ_BANDS], hold[PVA_VIZ_BANDS];
static uint8_t  vu[2];
static int32_t  vu_peak[2];         // largest |sample| since the last update
static float   *win, *re, *im;      // N floats each, in the stream RAM (the heap is small)
static uint16_t band_lo[PVA_VIZ_BANDS], band_hi[PVA_VIZ_BANDS];   // bins [lo, hi) of each band
static bool     band_dec[PVA_VIZ_BANDS];     // from the decimated FFT
static int16_t  dring[N];           // mono at rate / DECIM
static uint32_t dpos;
static int32_t  dacc;
static uint8_t  dcount;
static uint32_t bands_rate;
static bool     ready;

static void plan_bands(uint32_t rate)
{
    float hi = F_HI < rate * 0.48f ? F_HI : rate * 0.48f;
    float edge[PVA_VIZ_BANDS + 1];
    for (unsigned b = 0; b <= PVA_VIZ_BANDS; b++) edge[b] = F_LO * powf(hi / F_LO, (float)b / PVA_VIZ_BANDS);
    int last_full = 0, last_dec = 0;
    for (unsigned b = 0; b < PVA_VIZ_BANDS; b++)
    {
        bool dec = edge[b + 1] <= LOW_MAX_HZ;
        float bin_hz = (float)rate / (float)N / (dec ? (float)DECIM : 1.0f);
        int lo = (int)(edge[b] / bin_hz + 0.5f), h = (int)(edge[b + 1] / bin_hz + 0.5f);
        int *last = dec ? &last_dec : &last_full;
        if (lo < 1) lo = 1;
        if (lo < *last) lo = *last;         // no bin twice
        if (h <= lo) h = lo + 1;             // at least one bin
        if (h > (int)(N / 2) + 1) h = (int)(N / 2) + 1;
        band_lo[b] = (uint16_t)lo;
        band_hi[b] = (uint16_t)h;
        band_dec[b] = dec;
        *last = h;
        // the centre, for a player's frequency labels
        if (pva_win)
        {
            float f = ((float)lo + (float)(h - 1)) * 0.5f * bin_hz;
            uint16_t hz = (uint16_t)(f > 65535.0f ? 65535.0f : f);
            pva_win[PVA_R_VIZ + PVA_VIZ_FREQ + 2 * b] = (uint8_t)hz;
            pva_win[PVA_R_VIZ + PVA_VIZ_FREQ + 2 * b + 1] = (uint8_t)(hz >> 8);
        }
    }
    bands_rate = rate;
}

void pva_viz_init(void)
{
    win = pva_net_spare_take(N * sizeof(float));
    re = pva_net_spare_take(N * sizeof(float));
    im = pva_net_spare_take(N * sizeof(float));
    if (!win || !re || !im) { pva_log("visualisation: no memory"); return; }
    for (unsigned i = 0; i < N; i++) win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(N - 1));
    plan_bands(44100);
    ready = true;
}

void pva_viz_feed(const int16_t *stereo, uint32_t frames, uint32_t rate)
{
    if (!ready) return;
    for (uint32_t i = 0; i < frames; i++)
    {
        int32_t l = stereo[2 * i], r = stereo[2 * i + 1];
        int32_t m = (l + r) / 2;
        ring[wpos] = (int16_t)m;
        wpos = (wpos + 1u) % RING;
        dacc += m;
        if (++dcount == DECIM)
        {
            dring[dpos] = (int16_t)(dacc / (int32_t)DECIM);
            dpos = (dpos + 1u) % N;
            dacc = 0;
            dcount = 0;
        }
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > vu_peak[0]) vu_peak[0] = l;
        if (r > vu_peak[1]) vu_peak[1] = r;
    }
    fed += frames;
    if (rate) last_rate = rate;
}

// in-place radix-2 FFT
static void fft(void)
{
    for (unsigned i = 1, j = 0; i < N; i++)
    {
        unsigned bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (unsigned len = 2; len <= N; len <<= 1)
    {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (unsigned i = 0; i < N; i += len)
        {
            float cr = 1.0f, ci = 0.0f;
            for (unsigned k = 0; k < len / 2; k++)
            {
                unsigned a = i + k, b = a + len / 2;
                float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

// dB below full scale -> 0..PVA_VIZ_MAX, 2 dB a step
static uint8_t to_level(float power_rel)
{
    if (power_rel <= 1e-12f) return 0;
    float db = 10.0f * log10f(power_rel);       // 0 dB = a full-scale sine in one bin
    int v = (int)((db + 2.0f * (PVA_VIZ_MAX + 1)) / 2.0f);
    if (v < 0) v = 0;
    if (v > PVA_VIZ_MAX) v = PVA_VIZ_MAX;
    return (uint8_t)v;
}

static void publish(bool silent)
{
    volatile uint8_t *w = pva_win + PVA_R_VIZ;
    w[PVA_VIZ_SEQ] = (uint8_t)(seq | 1u);      // odd while it changes
    w[PVA_VIZ_COUNT] = PVA_VIZ_BANDS;
    w[PVA_VIZ_TOP] = PVA_VIZ_MAX;
    for (unsigned b = 0; b < PVA_VIZ_BANDS; b++)
    {
        w[PVA_VIZ_LEVEL + b] = level[b];
        w[PVA_VIZ_PEAK + b] = peak[b];
    }
    w[PVA_VIZ_VU] = vu[0];
    w[PVA_VIZ_VU + 1] = vu[1];
    // the newest samples, 64 of them every 4th one, as signed bytes
    uint32_t p = (wpos + RING - PVA_VIZ_WAVE * 4u) % RING;
    for (unsigned i = 0; i < PVA_VIZ_WAVE; i++)
    {
        int v = silent ? 0 : ring[(p + i * 4u) % RING] / 256;
        w[PVA_VIZ_SCOPE + i] = (uint8_t)(int8_t)v;
    }
    seq = (uint8_t)(seq + 2u);
    w[PVA_VIZ_SEQ] = seq;
}

void pva_viz_poll(bool playing)
{
    if (!ready || !pva_win) return;
    uint32_t now = pva_hal_millis();
    if (now - last_ms < UPDATE_MS) return;
    last_ms = now;
    // the decoder works in bursts: with too few new samples keep what is shown
    // (zeroing the VU there made it flicker); stopped or paused, everything falls
    if (playing && fed < N / 2) return;
    bool fresh = playing;
    uint8_t now_lv[PVA_VIZ_BANDS];
    if (fresh)
    {
        if (last_rate != bands_rate) plan_bands(last_rate);
        // a full-scale sine gives |X| = 32768 * N / 4 with the Hann window
        const float ref = 32768.0f * (float)N / 4.0f;
        const float ref2 = 1.0f / (ref * ref);
        for (int pass = 0; pass < 2; pass++)
        {
            bool dec = pass == 1;
            if (dec)
                for (unsigned i = 0; i < N; i++) { re[i] = (float)dring[(dpos + i) % N] * win[i]; im[i] = 0.0f; }
            else
            {
                uint32_t p = (wpos + RING - N) % RING;
                for (unsigned i = 0; i < N; i++) { re[i] = (float)ring[(p + i) % RING] * win[i]; im[i] = 0.0f; }
            }
            fft();
            for (unsigned b = 0; b < PVA_VIZ_BANDS; b++)
            {
                if (band_dec[b] != dec) continue;
                float pmax = 0.0f;
                for (unsigned k = band_lo[b]; k < band_hi[b] && k <= N / 2; k++)
                {
                    float pw = re[k] * re[k] + im[k] * im[k];
                    if (pw > pmax) pmax = pw;
                }
                now_lv[b] = to_level(pmax * ref2);
            }
        }
        for (int c = 0; c < 2; c++)
        {
            float rel = (float)vu_peak[c] / 32768.0f;
            uint8_t v = to_level(rel * rel);
            vu[c] = v >= vu[c] ? v : (uint8_t)(vu[c] - 1);     // up at once, down a step
            vu_peak[c] = 0;
        }
        fed = 0;
    }
    else
    {
        memset(now_lv, 0, sizeof now_lv);
        if (vu[0]) vu[0]--;
        if (vu[1]) vu[1]--;
    }
    for (unsigned b = 0; b < PVA_VIZ_BANDS; b++)
    {
        // up at once, down one step per update
        if (now_lv[b] >= level[b]) level[b] = now_lv[b];
        else level[b]--;
        if (level[b] >= peak[b]) { peak[b] = level[b]; hold[b] = PEAK_HOLD; }
        else if (hold[b]) hold[b]--;
        else if (peak[b]) peak[b]--;
    }
    publish(!fresh);
}

void pva_viz_get(uint8_t *levels, uint8_t *peaks, uint8_t *vu_lr)
{
    if (levels) memcpy(levels, level, sizeof level);
    if (peaks) memcpy(peaks, peak, sizeof peak);
    if (vu_lr) { vu_lr[0] = vu[0]; vu_lr[1] = vu[1]; }
}
