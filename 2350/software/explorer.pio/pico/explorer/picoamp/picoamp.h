#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hardware/pio.h"

#define PICOAMP_ROM_SIZE (64u * 1024u)
#define PICOAMP_ARENA_SIZE (128u * 1024u)
#define PICOAMP_STACK_SIZE (8u * 1024u)
#define PICOAMP_STREAM_SIZE (256u * 1024u)

struct audio_buffer_pool;

void picoamp_bootstrap(PIO bus, uint sm_read, uint sm_write);
bool picoamp_prepare(const uint8_t *rom, size_t rom_size,
                     void *workspace, size_t workspace_size,
                     uint8_t *stream, size_t stream_size,
                     struct audio_buffer_pool *pool, bool wifi, uint8_t volume);
void picoamp_run(PIO bus, uint sm_read, uint sm_write) __attribute__((noreturn));
void picoamp_audio_adopt(struct audio_buffer_pool *pool);
void picoamp_audio_set_volume(uint8_t percent);
