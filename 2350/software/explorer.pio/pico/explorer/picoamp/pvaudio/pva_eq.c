// MSX PICOVERSE PROJECT
// pva_eq.c - PicoVerse Audio Engine: 5-band equalizer (core 1).
//
// The DAC (UDA1334A) has no controls, so the sound is shaped here, on the
// decoded samples before the volume. Five biquad filters (RBJ cookbook):
//
//   60 Hz low shelf (corner 120 Hz), 250 Hz / 1 kHz / 4 kHz peaking (Q 1.0),
//   12 kHz high shelf (corner 7.2 kHz)
//
// each from -12 to +12 dB. The largest boost is taken off the whole signal
// first (a preamp), so a boosted band does not clip on loud music. All bands
// at 0 dB skip the filters altogether. Float maths: the RP2350's FPU runs the
// five filters on 44.1 kHz stereo in about 2 % of a core.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <math.h>
#include <string.h>
#include "pva_internal.h"

static const float band_hz[PVA_EQ_BANDS] = { 60.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f };

typedef struct { float b0, b1, b2, a1, a2; float s1[2], s2[2]; } bq_t;

static bq_t     bq[PVA_EQ_BANDS];
static int8_t   gains[PVA_EQ_BANDS];        // dB
static uint32_t coef_rate;                  // sample rate the filters are made for (0 = redo)
static bool     active;
static float    preamp = 1.0f;

static void make(bq_t *q, int band, float db, float fs)
{
    // a shelf's corner is where it reaches half its gain: put it beyond the
    // band, so 60 Hz and 12 kHz get (nearly) the whole boost or cut
    float f0 = band == 0 ? 2.0f * band_hz[0] : band == PVA_EQ_BANDS - 1 ? band_hz[band] * 0.6f : band_hz[band];
    if (f0 > fs * 0.45f) f0 = fs * 0.45f;   // 12 kHz at a 22 kHz stream
    float A = powf(10.0f, db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float cw = cosf(w0), sw = sinf(w0);
    float b0, b1, b2, a0, a1, a2;
    if (band == 0 || band == PVA_EQ_BANDS - 1)
    {
        // shelves, slope 1
        float alpha = sw / 2.0f * sqrtf(2.0f);
        float sa = 2.0f * sqrtf(A) * alpha;
        if (band == 0)
        {
            b0 = A * ((A + 1) - (A - 1) * cw + sa);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sa;
        }
        else
        {
            b0 = A * ((A + 1) + (A - 1) * cw + sa);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sa;
        }
    }
    else
    {
        float alpha = sw / 2.0f;            // Q = 1
        b0 = 1 + alpha * A;
        b1 = -2 * cw;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * cw;
        a2 = 1 - alpha / A;
    }
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0;
    q->a1 = a1 / a0; q->a2 = a2 / a0;
}

static void rebuild(uint32_t rate)
{
    int8_t top = 0;
    for (int i = 0; i < PVA_EQ_BANDS; i++)
    {
        make(&bq[i], i, (float)gains[i], (float)rate);
        if (gains[i] > top) top = gains[i];
    }
    preamp = powf(10.0f, -(float)top / 20.0f);
    coef_rate = rate;
}

void pva_eq_set(const int8_t *g)
{
    bool any = false;
    for (int i = 0; i < PVA_EQ_BANDS; i++)
    {
        int8_t v = g[i];
        if (v > PVA_EQ_MAX_DB) v = PVA_EQ_MAX_DB;
        if (v < -PVA_EQ_MAX_DB) v = -PVA_EQ_MAX_DB;
        gains[i] = v;
        if (v) any = true;
    }
    coef_rate = 0;                          // new filters on the next block
    if (any && !active) memset(bq, 0, sizeof bq);   // start from silence, no click from old state
    active = any;
}

void pva_eq_get(int8_t *g) { memcpy(g, gains, sizeof gains); }

void pva_eq_reset(void)
{
    // a new track: no ringing from the last one
    for (int i = 0; i < PVA_EQ_BANDS; i++) { bq[i].s1[0] = bq[i].s1[1] = bq[i].s2[0] = bq[i].s2[1] = 0.0f; }
}

void pva_eq_process(int16_t *buf, uint32_t frames, uint32_t rate)
{
    if (!active || !rate) return;
    if (rate != coef_rate)
    {
        // keep the filter state: only the gains or the rate changed
        float s1[PVA_EQ_BANDS][2], s2[PVA_EQ_BANDS][2];
        for (int i = 0; i < PVA_EQ_BANDS; i++) { memcpy(s1[i], bq[i].s1, sizeof s1[i]); memcpy(s2[i], bq[i].s2, sizeof s2[i]); }
        rebuild(rate);
        for (int i = 0; i < PVA_EQ_BANDS; i++) { memcpy(bq[i].s1, s1[i], sizeof s1[i]); memcpy(bq[i].s2, s2[i], sizeof s2[i]); }
    }
    for (uint32_t n = 0; n < frames; n++)
    {
        for (int ch = 0; ch < 2; ch++)
        {
            float x = (float)buf[2 * n + ch] * preamp;
            for (int i = 0; i < PVA_EQ_BANDS; i++)
            {
                bq_t *q = &bq[i];
                // transposed direct form II
                float y = q->b0 * x + q->s1[ch];
                q->s1[ch] = q->b1 * x - q->a1 * y + q->s2[ch];
                q->s2[ch] = q->b2 * x - q->a2 * y;
                x = y;
            }
            if (x > 32767.0f) x = 32767.0f;
            else if (x < -32768.0f) x = -32768.0f;
            buf[2 * n + ch] = (int16_t)(int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f);   // no lrintf call per sample
        }
    }
    // keep denormals out of the state when the music goes quiet
    for (int i = 0; i < PVA_EQ_BANDS; i++)
        for (int ch = 0; ch < 2; ch++)
        {
            if (fabsf(bq[i].s1[ch]) < 1e-15f) bq[i].s1[ch] = 0.0f;
            if (fabsf(bq[i].s2[ch]) < 1e-15f) bq[i].s2[ch] = 0.0f;
        }
}
