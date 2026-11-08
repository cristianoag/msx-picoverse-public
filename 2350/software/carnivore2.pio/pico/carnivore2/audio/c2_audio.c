// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_audio.c - Carnivore2 sound section. See c2_audio.h.
//
// Level scaling follows mv16.vhd (factor M5/16 with M5 = 5,6,7,8,10,12,14,16);
// the power-on level 3 (M5 = 8) is the reference loudness of the PicoVerse
// SCC/FM/PSG engines, so the gain applied here is M5/8.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "hardware/dma.h"
#include "../carnivore2.h"
#include "emu2212.h"
#include "emu2149.h"
#include "emu2413.h"
#include "c2_audio.h"

#define RING_SIZE       1024u
#define SOUND_SHIFT     2           // all three engines are boosted 4x as in the other PicoVerse firmwares
#define BEEPER_LEVEL    3000
#define OPLL_IDLE_BUFFERS 90        // keep rendering FM for ~0.5 s after the last write (release tails)

static const uint8_t m5_table[8] = { 5u, 6u, 7u, 8u, 10u, 12u, 14u, 16u };

static SCC scc;
static PSG psg;
static OPLL *opll;

static uint8_t scc_wave_shadow[0xA0];

static volatile uint32_t ring_head, ring_tail;
static uint32_t ring[RING_SIZE];

static struct audio_buffer_pool *pool;
static bool audio_ready;
static int dma_channel = -1;

static int32_t gain_fm = 8, gain_scc = 8, gain_psg = 8, gain_beep = 8;
static bool psg_enabled, beeper_enabled, fm_mono;
static uint8_t beeper_level = 1;
static uint32_t opll_idle = OPLL_IDLE_BUFFERS;

static inline int32_t clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}

static void set_fm_pan(bool mono)
{
    if (!opll) return;
    for (uint32_t ch = 0u; ch < 14u; ++ch)
        OPLL_setPan(opll, ch, mono ? 3u : (ch < 9u ? 2u : 1u));
}

void c2a_init(void)
{
    memset(&scc, 0, sizeof(scc));
    scc.clk = C2_SCC_CLOCK;
    scc.rate = C2_SAMPLE_RATE;
    SCC_set_quality(&scc, 1);
    scc.type = SCC_ENHANCED;
    SCC_reset(&scc);
    scc.active = 1u;
    scc.mode = 0u;

    memset(&psg, 0, sizeof(psg));
    psg.rate = C2_SAMPLE_RATE;
    PSG_setVolumeMode(&psg, 2);
    PSG_setClock(&psg, C2_PSG_CLOCK);
    PSG_setQuality(&psg, 1);
    PSG_reset(&psg);

    opll = OPLL_new(C2_OPLL_CLOCK, C2_SAMPLE_RATE);
    if (opll)
    {
        OPLL_reset(opll);
        OPLL_setChipType(opll, 0);
        OPLL_resetPatch(opll, 0);
        set_fm_pan(false);
    }

    memset(scc_wave_shadow, 0, sizeof(scc_wave_shadow));
    ring_head = ring_tail = 0u;
}

uint8_t c2a_scc_wave_read(uint8_t offset)
{
    return (offset < sizeof(scc_wave_shadow)) ? scc_wave_shadow[offset] : 0u;
}

void __not_in_flash_func(c2a_post)(uint8_t kind, uint8_t a, uint8_t d)
{
    if (kind == C2A_SCC_WAVE && a < sizeof(scc_wave_shadow)) scc_wave_shadow[a] = d;

    uint32_t head = ring_head;
    uint32_t next = (head + 1u) % RING_SIZE;
    if (next == ring_tail) return;      // audio core is far behind: drop rather than stall the bus
    ring[head] = ((uint32_t)kind << 16) | ((uint32_t)a << 8) | d;
    __dmb();
    ring_head = next;
}

static void __not_in_flash_func(apply_event)(uint32_t e)
{
    uint8_t kind = (uint8_t)(e >> 16), a = (uint8_t)(e >> 8), d = (uint8_t)e;

    switch (kind)
    {
    case C2A_OPLL:
        if (opll) OPLL_writeIO(opll, 0x7Cu | (a & 1u), d);
        opll_idle = 0u;
        break;
    case C2A_PSG:
        PSG_writeIO(&psg, 0x10u | (a & 1u), d);
        break;
    case C2A_SCC_WAVE:
        if (a < 0xA0u) SCC_writeReg(&scc, a, d);
        break;
    case C2A_SCC_REG:
        if (a <= 9u) SCC_writeReg(&scc, 0xC0u + a, d);
        else if (a <= 14u) SCC_writeReg(&scc, 0xD0u + (a - 10u), d);
        else SCC_writeReg(&scc, 0xE1u, d);
        break;
    case C2A_SCC_MODE:
        SCC_writeReg(&scc, 0xE2u, d);
        break;
    case C2A_SCC_PLUS:
        scc.mode = d ? 1u : 0u;
        break;
    case C2A_BEEPER:
        beeper_level = d & 1u;
        break;
    case C2A_LEVELS:
        gain_fm = m5_table[(a >> 3) & 7u];
        gain_scc = m5_table[a & 7u];
        gain_psg = m5_table[(d >> 3) & 7u];
        gain_beep = m5_table[d & 7u];
        psg_enabled = (d & 0x80u) != 0u;
        beeper_enabled = (d & 0x40u) != 0u;
        if (fm_mono != ((a & 0x80u) != 0u))
        {
            fm_mono = (a & 0x80u) != 0u;
            set_fm_pan(fm_mono);
        }
        break;
    default:
        break;
    }
}

static inline void __not_in_flash_func(drain_ring)(void)
{
    while (ring_tail != ring_head)
    {
        uint32_t tail = ring_tail;
        uint32_t e = ring[tail];
        __dmb();
        ring_tail = (tail + 1u) % RING_SIZE;
        apply_event(e);
    }
}

static bool __not_in_flash_func(scc_audible)(void)
{
    for (uint32_t ch = 0u; ch < 5u; ++ch)
        if (scc.volume[ch] != 0u && ((scc.ch_enable_next >> ch) & 1) != 0) return true;
    return false;
}

static bool __not_in_flash_func(psg_audible)(void)
{
    uint8_t mixer = psg.reg[7];
    for (uint8_t ch = 0u; ch < 3u; ++ch)
    {
        uint8_t volume = psg.reg[8 + ch] & 0x1Fu;
        if (volume & 0x10u) { if (psg.env_freq == 0u) continue; }
        else if ((volume & 0x0Fu) == 0u) continue;
        if (((mixer >> ch) & 1u) == 0u || ((mixer >> (ch + 3u)) & 1u) == 0u) return true;
    }
    return false;
}

void __not_in_flash_func(service_scc_audio)(void)
{
    if (!audio_ready || !pool) return;

    struct audio_buffer *buffer = take_audio_buffer(pool, false);
    if (!buffer) return;

    int16_t *samples = (int16_t *)buffer->buffer->bytes;

    drain_ring();
    bool fm_on = opll && (opll->slot_key_status != 0u || opll_idle < OPLL_IDLE_BUFFERS);
    if (opll_idle < OPLL_IDLE_BUFFERS) opll_idle++;
    bool scc_on = scc_audible();
    bool psg_on = psg_enabled && psg_audible();

    for (int i = 0; i < C2_AUDIO_BUFFER_SAMPLES; ++i)
    {
        drain_ring();

        int32_t left = 0, right = 0;

        if (fm_on)
        {
            int32_t o[2];
            OPLL_calcStereo(opll, o);
            if (fm_mono) o[1] = o[0];
            left += ((o[0] << SOUND_SHIFT) * gain_fm) >> 3;
            right += ((o[1] << SOUND_SHIFT) * gain_fm) >> 3;
        }
        if (scc_on)
        {
            int32_t s = (((int32_t)SCC_calc(&scc) << SOUND_SHIFT) * gain_scc) >> 3;
            left += s; right += s;
        }
        if (psg_on)
        {
            int32_t s = (((int32_t)PSG_calc(&psg) << SOUND_SHIFT) * gain_psg) >> 3;
            left += s; right += s;
        }
        if (beeper_enabled)
        {
            int32_t s = ((beeper_level ? BEEPER_LEVEL : -BEEPER_LEVEL) * gain_beep) >> 3;
            left += s; right += s;
        }

        samples[i * 2] = (int16_t)clamp16(left);
        samples[i * 2 + 1] = (int16_t)clamp16(right);
    }

    buffer->sample_count = C2_AUDIO_BUFFER_SAMPLES;
    give_audio_buffer(pool, buffer);
}

void c2a_start(void)
{
    if (audio_ready) return;

    // Unmute the DAC (active-high mute)
    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 0);

    static audio_format_t audio_format = {
        .sample_freq = C2_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };
    static struct audio_buffer_format producer_format = {
        .format = &audio_format,
        .sample_stride = 4,
    };

    pool = audio_new_producer_pool(&producer_format, 3, C2_AUDIO_BUFFER_SAMPLES);

    for (int ch = 0; ch < NUM_DMA_CHANNELS && dma_channel < 0; ++ch)
        if (!dma_channel_is_claimed((uint)ch)) dma_channel = ch;
    if (dma_channel < 0) { pool = NULL; return; }

    static struct audio_i2s_config i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_BCLK_PIN,
        .dma_channel = 0,
        .pio_sm = 2,
    };
    i2s_config.dma_channel = (uint)dma_channel;

    audio_i2s_setup(&audio_format, &i2s_config);
    audio_i2s_connect(pool);
    audio_i2s_set_enabled(true);
    audio_ready = true;
}
