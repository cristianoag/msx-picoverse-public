// MSX PICOVERSE PROJECT
// pva_hal_pico.c - PicoVerse Audio Engine: I2S output and clock (RP2350, core 1).
//
// pico_audio_i2s (pico-extras) on Explorer's PIO1 SM2 with the same DAC pins
// and live-pool handoff as its menu MP3
// player uses them. Must run on core 1: the DMA completion IRQ is registered
// on the calling core and must never interrupt the core 0 bus loop.
//
// Never blocks: acquire returns NULL when every buffer is queued. When no
// buffer is queued pico_audio_i2s plays silence by itself.
//
// Cold-start producer buffers come from the engine arena; a menu preview's
// existing pool is adopted without allocating another pipeline.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "pico/util/buffer.h"
#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "pva_internal.h"
#include "../picoamp.h"

#define PVA_I2S_DATA_PIN  29
#define PVA_I2S_BCLK_PIN  30   // LRCLK = BCLK + 1 = 31
#define PVA_I2S_MUTE_PIN  32   // high = muted
#define PVA_I2S_BUFFERS   5    // ~130 ms of 44.1 kHz audio queued
#define PVA_I2S_FRAMES    1152

static audio_format_t audio_format = {
    .sample_freq = 44100,
    .format = AUDIO_BUFFER_FORMAT_PCM_S16,
    .channel_count = 2,
};

static struct audio_buffer_format producer_format = {
    .format = &audio_format,
    .sample_stride = 4,
};

static struct audio_buffer_pool *pool;
static struct audio_buffer *current;
static uint8_t master_volume = 100;

void picoamp_audio_set_volume(uint8_t percent)
{
    master_volume = percent;
}

void picoamp_audio_adopt(struct audio_buffer_pool *handoff)
{
    pool = handoff;
}

// audio_new_producer_pool() without the heap
static struct audio_buffer_pool *new_arena_pool(void)
{
    audio_buffer_pool_t *ac = pva_alloc(sizeof(audio_buffer_pool_t));
    audio_buffer_t *bufs = pva_alloc(PVA_I2S_BUFFERS * sizeof(audio_buffer_t));
    mem_buffer_t *mems = pva_alloc(PVA_I2S_BUFFERS * sizeof(mem_buffer_t));
    uint8_t *bytes = pva_alloc(PVA_I2S_BUFFERS * PVA_I2S_FRAMES * 4u);
    if (!ac || !bufs || !mems || !bytes)
        return NULL;
    for (int i = 0; i < PVA_I2S_BUFFERS; i++)
    {
        mems[i].size = PVA_I2S_FRAMES * 4u;
        mems[i].bytes = bytes + (size_t)i * PVA_I2S_FRAMES * 4u;
        mems[i].flags = 0;
        bufs[i].format = &producer_format;
        bufs[i].buffer = &mems[i];
        bufs[i].max_sample_count = PVA_I2S_FRAMES;
        bufs[i].sample_count = 0;
        bufs[i].next = (i != PVA_I2S_BUFFERS - 1) ? &bufs[i + 1] : NULL;
    }
    ac->type = ac_producer;
    ac->format = producer_format.format;
    ac->free_list_spin_lock = spin_lock_init(SPINLOCK_ID_AUDIO_FREE_LIST_LOCK);
    ac->free_list = bufs;
    ac->prepared_list_spin_lock = spin_lock_init(SPINLOCK_ID_AUDIO_PREPARED_LISTS_LOCK);
    ac->prepared_list = NULL;
    ac->prepared_list_tail = NULL;
    ac->connection = NULL;          // set by audio_i2s_connect()
    return ac;
}

bool pva_hal_audio_init(void)
{
    gpio_init(PVA_I2S_MUTE_PIN);
    gpio_set_dir(PVA_I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(PVA_I2S_MUTE_PIN, 1);

    if (pool)
    {
        audio_i2s_set_enabled(false);
        pool->format = &audio_format;
        audio_i2s_set_enabled(true);
        return true;
    }

    pool = new_arena_pool();
    if (!pool)
        return false;

    int dma = dma_claim_unused_channel(false);
    if (dma < 0)
        return false;
    dma_channel_unclaim((uint)dma);   // audio_i2s_setup claims it again

    struct audio_i2s_config cfg = {
        .data_pin = PVA_I2S_DATA_PIN,
        .clock_pin_base = PVA_I2S_BCLK_PIN,
        .dma_channel = (uint8_t)dma,
        .pio_sm = 2,
    };
    if (!audio_i2s_setup(&audio_format, &cfg))
        return false;
    if (!audio_i2s_connect(pool))
        return false;
    audio_i2s_set_enabled(true);
    return true;
}

void pva_hal_audio_mute(bool mute)
{
    gpio_put(PVA_I2S_MUTE_PIN, mute ? 1 : 0);
}

int16_t *pva_hal_audio_acquire(uint32_t *max_frames)
{
    if (!pool)
        return NULL;
    if (!current)
        current = take_audio_buffer(pool, false);
    if (!current)
        return NULL;
    *max_frames = current->max_sample_count;
    return (int16_t *)current->buffer->bytes;
}

void pva_hal_audio_commit(uint32_t frames, uint32_t sample_rate)
{
    if (!current)
        return;
    if (master_volume != 100u)
    {
        int16_t *samples = (int16_t *)current->buffer->bytes;
        for (uint32_t i = 0; i < frames * 2u; i++)
        {
            int32_t sample = (int32_t)samples[i] * master_volume / 100;
            samples[i] = sample > 32767 ? 32767 : sample < -32768 ? -32768 : (int16_t)sample;
        }
    }
    // pico_audio_i2s retunes the PIO clock when the producer rate changes
    if (sample_rate && audio_format.sample_freq != sample_rate)
        audio_format.sample_freq = sample_rate;
    current->sample_count = frames;
    give_audio_buffer(pool, current);
    current = NULL;
}

// ---------------------------------------------------------------- crashes
// A crash or a panic (pico_malloc out of heap, say) stops the core. Leave a
// note in the register window, which core 0 keeps serving, so the MSX side can
// tell what happened instead of only "no heartbeat".
static void pva_fault_note(uint8_t kind, uint32_t pc, const char *text)
{
    if (!pva_win) return;
    pva_win[PVA_R_FAULT_PC] = (uint8_t)pc;
    pva_win[PVA_R_FAULT_PC + 1] = (uint8_t)(pc >> 8);
    pva_win[PVA_R_FAULT_PC + 2] = (uint8_t)(pc >> 16);
    pva_win[PVA_R_FAULT_PC + 3] = (uint8_t)(pc >> 24);
    if (text)
        for (int i = 0; i < 63 && text[i]; i++) pva_win[PVA_R_FAULT_TEXT + i] = (uint8_t)text[i];
    pva_win[PVA_R_FAULT] = kind;
}

void __attribute__((used)) pva_fault_record(uint32_t *frame)
{
    pva_fault_note(1, frame[6], get_core_num() ? "HardFault on core 1" : "HardFault on core 0");
    for (;;) __asm volatile("wfe");
}

void __attribute__((naked)) pva_hardfault_entry(void)
{
    __asm volatile(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "b pva_fault_record\n"
        "1:\n"
        "mrs r0, msp\n"
        "b pva_fault_record\n");
}

// The microSD library's crash handler (crash.c) halts in a breakpoint; ours
// leaves the note first. Installed by core 1 for the shared vector table.
void pva_hal_install_fault_handler(void)
{
    // straight into the (RAM) vector table: the SDK's exception_*_handler()
    // calls assert that no handler is installed yet, and crash.c has one
    uint32_t vtor = scb_hw->vtor;
    if (vtor < 0x20000000u || vtor >= 0x20082000u) return;     // not in SRAM: leave it
    uint32_t save = save_and_disable_interrupts();
    ((void (**)(void))vtor)[3] = pva_hardfault_entry;          // vector 3 = HardFault
    __dmb();
    restore_interrupts(save);
}

void __attribute__((used)) pva_panic(const char *fmt)
{
    uint32_t lr;
    __asm volatile("mov %0, lr" : "=r"(lr));
    pva_fault_note(2, lr, fmt);
    for (;;) __asm volatile("wfe");
}

uint32_t pva_hal_millis(void)
{
    return to_ms_since_boot(get_absolute_time());
}
