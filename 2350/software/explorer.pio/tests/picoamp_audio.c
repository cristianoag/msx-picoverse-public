#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef unsigned int uint;
typedef struct { uint32_t sample_freq; } audio_format_t;
struct audio_buffer_pool { const audio_format_t *format; };
struct mem_buffer { uint8_t *bytes; };
struct audio_buffer { uint32_t sample_count; struct mem_buffer *buffer; };
struct audio_i2s_config { uint8_t data_pin, clock_pin_base, dma_channel, pio_sm; };
static audio_format_t audio_format = {44100};
static struct audio_buffer_pool *pool;
static struct audio_buffer *current;
static uint8_t master_volume = 100;
static unsigned allocated, setups, connects, enables, disables, commits;
static struct audio_buffer_pool cold_pool;
static struct audio_buffer_pool *new_arena_pool(void) { allocated++; return &cold_pool; }
static void gpio_init(unsigned pin) { (void)pin; }
static void gpio_set_dir(unsigned pin, bool output) { (void)pin; assert(output); }
static void gpio_put(unsigned pin, bool value) { (void)pin; assert(value); }
#define GPIO_OUT true
static int dma_claim_unused_channel(bool required) { assert(!required); return 3; }
static void dma_channel_unclaim(unsigned channel) { assert(channel == 3); }
static bool audio_i2s_setup(const audio_format_t *format, const struct audio_i2s_config *cfg)
{
    assert(format == &audio_format && cfg->pio_sm == 2 && cfg->dma_channel == 3);
    setups++;
    return true;
}
static bool audio_i2s_connect(struct audio_buffer_pool *value) { assert(value == pool); connects++; return true; }
static void audio_i2s_set_enabled(bool enabled) { if (enabled) enables++; else disables++; }
static void give_audio_buffer(struct audio_buffer_pool *value, struct audio_buffer *buffer)
{
    assert(value == pool && buffer == current);
    commits++;
}

#include "picoamp-audio-production.h"

int main(void)
{
    picoamp_audio_adopt(NULL);
    assert(pva_hal_audio_init());
    assert(allocated == 1 && setups == 1 && connects == 1 && enables == 1);
    struct audio_buffer_pool warm_pool = {NULL};
    picoamp_audio_adopt(&warm_pool);
    assert(pva_hal_audio_init());
    assert(pool == &warm_pool && pool->format == &audio_format);
    assert(allocated == 1 && setups == 1 && connects == 1 && enables == 2 && disables == 1);
    int16_t pcm[] = {30000, -30000, 1000, -1000};
    struct mem_buffer memory = {(uint8_t *)pcm};
    struct audio_buffer buffer = {0, &memory};
    current = &buffer;
    picoamp_audio_set_volume(200);
    pva_hal_audio_commit(2, 22050);
    assert(audio_format.sample_freq == 22050 && buffer.sample_count == 2 && commits == 1 && !current);
    assert(pcm[0] == 32767 && pcm[1] == -32768 && pcm[2] == 2000 && pcm[3] == -2000);
    current = &buffer;
    picoamp_audio_set_volume(0);
    pva_hal_audio_commit(2, 44100);
    for (unsigned i = 0; i < 4; i++) assert(pcm[i] == 0);
    puts("PASS: cold PicoAmp audio, preview-pool adoption, sample-rate updates and 0-200% volume/clipping");
    return 0;
}
