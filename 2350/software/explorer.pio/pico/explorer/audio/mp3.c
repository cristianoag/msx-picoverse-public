#include "mp3.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "ff.h"
#include "mp3dec.h"
#include "../storage/sd_activity.h"

#if EXPLORER_USB_STDIO_DEBUG
#include <malloc.h>
extern char __end__, __HeapLimit;
static bool mp3_first_frame_logged;

static void mp3_debug_heap(const char *stage) {
    struct mallinfo info = mallinfo();
    unsigned long capacity = (uintptr_t)&__HeapLimit - (uintptr_t)&__end__;
    printf("MP3: %s heap capacity=%lu allocated=%lu arena=%lu arena_free=%lu top_free=%lu\n",
           stage, capacity, (unsigned long)info.uordblks, (unsigned long)info.arena,
           (unsigned long)info.fordblks, (unsigned long)info.keepcost);
}
#endif

// I2S clock pins must be consecutive: clock_pin_base = BCLK, clock_pin_base+1 = LRCLK.
#define MP3_I2S_DATA_PIN 29
#define MP3_I2S_BCLK_PIN 30
#define MP3_I2S_WSEL_PIN 31
#define MP3_I2S_MUTE_PIN 32

#ifndef PICO_AUDIO_I2S_PIO
#define PICO_AUDIO_I2S_PIO 0
#endif

#if PICO_AUDIO_I2S_PIO == 2
#define MP3_I2S_PIO pio2
#elif PICO_AUDIO_I2S_PIO == 1
#define MP3_I2S_PIO pio1
#else
#define MP3_I2S_PIO pio0
#endif

#define MP3_BUFFER_FALLBACK 8192
#define MP3_READ_ERROR_LIMIT 8
#define MP3_I2S_BUFFER_SAMPLES 1152
// Six full stereo frames retain ~157 ms at 44.1kHz while leaving room for
// menu allocations and allocator fragmentation (eight exhausted the heap).
#define MP3_I2S_BUFFER_COUNT 6
#define MP3_MIN_BUFFER_SIZE 2048
#define MP3_MAX_SAMPLES_PER_CH 1152
#define MP3_PATH_MAX 256
#define DEFAULT_SAMPLE_RATE 44100
#define ESTIMATE_BITRATE_KBPS 128
#define MP3_CMD_CHECK_INTERVAL 16

typedef enum {
    AUDIO_FILE_MP3 = 0,
    AUDIO_FILE_WAV = 1,
} audio_file_kind_t;

static struct audio_buffer_pool *audio_pool;
// Set on Core 0 (via mp3_adopt_i2s_audio_pool) before relaunching the core so
// mp3_init() reuses an already-live I2S pool instead of re-initialising the
// pico_audio_i2s singleton. Consumed once by mp3_init().
static struct audio_buffer_pool *mp3_pending_handoff_pool = NULL;
static struct audio_i2s_config i2s_config = {
    .data_pin = MP3_I2S_DATA_PIN,
    .clock_pin_base = MP3_I2S_BCLK_PIN,
    .dma_channel = 0,
    .pio_sm = 2,
};
static bool i2s_ready = false;
static audio_format_t audio_format = {
    .sample_freq = DEFAULT_SAMPLE_RATE,
    .format = AUDIO_BUFFER_FORMAT_PCM_S16,
    .channel_count = 2,
};
static struct audio_buffer_format producer_format = {
    .format = &audio_format,
    .sample_stride = 4,
};

static FIL mp3_file;
static bool file_open = false;
static char mp3_selected_path[MP3_PATH_MAX] = {0};
static audio_file_kind_t selected_file_kind = AUDIO_FILE_MP3;

static volatile uint8_t status_flags = 0;
static volatile uint16_t elapsed_seconds = 0;
static volatile uint16_t total_seconds = 0;
static bool total_seconds_estimated = false;

static uint8_t mp3_buf_fallback[MP3_BUFFER_FALLBACK];
static uint8_t *mp3_buf = mp3_buf_fallback;
static size_t mp3_buf_capacity = MP3_BUFFER_FALLBACK;
static size_t mp3_buf_used = 0;
static size_t mp3_buf_pos = 0;
static HMP3Decoder mp3_decoder = NULL;

static uint32_t mp3_file_size = 0;
static uint32_t mp3_bytes_read = 0;
static uint8_t mp3_read_errors = 0;

static uint32_t sample_rate = DEFAULT_SAMPLE_RATE;
static uint64_t elapsed_samples = 0;

static uint32_t wav_data_offset = 0;
static uint32_t wav_data_size = 0;
static uint32_t wav_bytes_remaining = 0;
static uint32_t wav_loop_sample = 0;
static uint16_t wav_channels = 0;
static uint16_t wav_bits_per_sample = 0;
static uint16_t wav_block_align = 0;
static bool wav_first_buffer_logged = false;
static int16_t pcm_scratch[MP3_MAX_SAMPLES_PER_CH * 2];

static bool playing = false;
static bool paused = false;
static bool play_loop = false;
static bool muted = false;
static bool eof = false;
static bool error_flag = false;
static uint32_t fade_samples_total = 0;
static uint32_t fade_samples_remaining = 0;
static int8_t fade_direction = 0;
static bool fade_stop_when_done = false;
static char wavegame_previous_path[MP3_PATH_MAX] = {0};
static uint32_t wavegame_previous_sample = 0;
static uint32_t wavegame_previous_loop_sample = 0;
static bool wavegame_previous_loop = false;

// A small FIFO is required because the MSX menu issues SELECT followed
// by PLAY only ~10 ms apart, which on lazy Core 1 startup is shorter
// than the time mp3_init() takes to complete. With a single-slot queue
// the second command would overwrite the first.
#define MP3_CORE_CMD_NONE        0
#define MP3_CORE_CMD_SELECT      1
#define MP3_CORE_CMD_PLAY        2
#define MP3_CORE_CMD_STOP        3
#define MP3_CORE_CMD_PAUSE       4
#define MP3_CORE_CMD_RESUME      5
#define MP3_CORE_CMD_TOGGLE_MUTE 6
#define MP3_CORE_CMD_PLAY_LOOP   7
#define MP3_CORE_CMD_SHUTDOWN    8
#define MP3_CORE_CMD_WAVEGAME_PLAY          9
#define MP3_CORE_CMD_WAVEGAME_FADE_OUT     10
#define MP3_CORE_CMD_WAVEGAME_TOGGLE_PAUSE 11
#define MP3_CORE_CMD_WAVEGAME_RESUME_PREV  12
#define MP3_CORE_CMD_WAVEGAME_PLAY_INDEX   13
#define MP3_CORE_CMD_WAVEGAME_PLAY_PAUSE   14

#define MP3_CORE_CMD_QUEUE_SIZE 8u
static volatile uint8_t  core_cmd_queue[MP3_CORE_CMD_QUEUE_SIZE];
static volatile uint8_t  core_cmd_head = 0; // Core 0 writes here
static volatile uint8_t  core_cmd_tail = 0; // Core 1 reads here
static volatile char     core_cmd_path[MP3_PATH_MAX];
static volatile uint32_t core_cmd_file_size = 0;
static volatile uint32_t core_cmd_wav_start_sample = 0;
static volatile uint32_t core_cmd_wav_loop_sample = 0;
static volatile uint8_t  core_cmd_wav_seconds = 0;
static volatile uint8_t  core_cmd_wav_index = 0;
static volatile bool     core_cmd_wav_loop = false;
static volatile bool     core_cmd_wav_fade_in = false;
static volatile bool core1_running = false;
static volatile bool core1_stopped = true;
static volatile bool core1_keep_i2s_on_exit = false;
static mp3_bg_callback_t bg_callback = NULL;
static mp3_wavegame_psg_sample_callback_t wavegame_psg_sample_cb = NULL;
static mp3_wavegame_psg_rate_callback_t wavegame_psg_rate_cb = NULL;

void mp3_set_external_buffer(uint8_t *buffer, size_t size) {
    if (!buffer || size < MP3_MIN_BUFFER_SIZE) {
        return;
    }
    mp3_buf = buffer;
    mp3_buf_capacity = size;
    printf("MP3: external buffer size=%lu\n", (unsigned long)mp3_buf_capacity);
}

void mp3_wavegame_set_psg_callbacks(mp3_wavegame_psg_sample_callback_t sample_cb,
                                    mp3_wavegame_psg_rate_callback_t rate_cb) {
    wavegame_psg_sample_cb = sample_cb;
    wavegame_psg_rate_cb = rate_cb;
    if (wavegame_psg_rate_cb) {
        wavegame_psg_rate_cb(sample_rate);
    }
}

static int mp3_dma_channel = -1;

static uint16_t read_le16(const uint8_t *ptr) {
    return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static uint32_t read_le32(const uint8_t *ptr) {
    return (uint32_t)ptr[0] |
           ((uint32_t)ptr[1] << 8) |
           ((uint32_t)ptr[2] << 16) |
           ((uint32_t)ptr[3] << 24);
}

static bool path_has_extension(const char *path, const char *extension) {
    if (!path || !extension) {
        return false;
    }
    const char *dot = strrchr(path, '.');
    if (!dot || dot[1] == '\0') {
        return false;
    }
    const char *actual = dot + 1;
    while (*actual && *extension) {
        if (toupper((unsigned char)*actual) != toupper((unsigned char)*extension)) {
            return false;
        }
        actual++;
        extension++;
    }
    return *actual == '\0' && *extension == '\0';
}

static void i2s_release_sm(void) {
    if (pio_sm_is_claimed(MP3_I2S_PIO, i2s_config.pio_sm)) {
        pio_sm_unclaim(MP3_I2S_PIO, i2s_config.pio_sm);
    }
}

static bool i2s_start(void) {
    const struct audio_format *output_format = audio_i2s_setup(&audio_format, &i2s_config);
    if (!output_format || !audio_i2s_connect(audio_pool)) {
        return false;
    }
    audio_i2s_set_enabled(true);
    i2s_ready = true;
    return true;
}

static void update_status_flags(void) {
    uint8_t flags = 0;
    if (playing) flags |= MP3_STATUS_PLAYING;
    if (muted) flags |= MP3_STATUS_MUTED;
    if (error_flag) flags |= MP3_STATUS_ERROR;
    if (eof) flags |= MP3_STATUS_EOF;
    if (paused) flags |= MP3_STATUS_PAUSED;
    status_flags = flags;
}

static void set_mute(bool enable) {
    muted = enable;
    gpio_put(MP3_I2S_MUTE_PIN, muted ? 1 : 0);
    update_status_flags();
}

static void reset_decoder_state(void) {
#if EXPLORER_USB_STDIO_DEBUG
    mp3_first_frame_logged = false;
#endif
    if (mp3_decoder) {
        MP3FreeDecoder(mp3_decoder);
        mp3_decoder = NULL;
    }
    mp3_buf_used = 0;
    mp3_buf_pos = 0;
    mp3_bytes_read = 0;
    mp3_read_errors = 0;
    elapsed_samples = 0;
    elapsed_seconds = 0;
    total_seconds = 0;
    total_seconds_estimated = false;
    paused = false;
    // Do NOT reset sample_rate / audio_format.sample_freq here. They
    // track the live producer rate so pico_audio_i2s can retune the PIO clock.
    eof = false;
    error_flag = false;
    wav_data_offset = 0;
    wav_data_size = 0;
    wav_bytes_remaining = 0;
    wav_loop_sample = 0;
    wav_channels = 0;
    wav_bits_per_sample = 0;
    wav_block_align = 0;
    wav_first_buffer_logged = false;
    fade_samples_total = 0;
    fade_samples_remaining = 0;
    fade_direction = 0;
    fade_stop_when_done = false;
}

static void close_file(void) {
    if (file_open) {
        f_close(&mp3_file);
        file_open = false;
    }
}

static void fill_buffer_if_needed(void) {
    if (!file_open || eof) return;

    // Only compact when consumed data exceeds half the buffer,
    // avoiding expensive memmove on every frame decode.
    if (mp3_buf_pos >= mp3_buf_capacity / 2) {
        size_t remaining = mp3_buf_used - mp3_buf_pos;
        if (remaining > 0) {
            memmove(mp3_buf, mp3_buf + mp3_buf_pos, remaining);
        }
        mp3_buf_used = remaining;
        mp3_buf_pos = 0;
    }

    // Only read from SD when free space exceeds a quarter of the buffer,
    // batching reads into larger chunks for better SPI throughput.
    size_t free_space = mp3_buf_capacity - mp3_buf_used;
    if (free_space < mp3_buf_capacity / 4) return;

    UINT br = 0;
    sd_activity_note();
    if (f_read(&mp3_file, mp3_buf + mp3_buf_used, (UINT)free_space, &br) == FR_OK) {
        if (br > 0) {
            mp3_buf_used += br;
            mp3_bytes_read += br;
            mp3_read_errors = 0;
            if (error_flag) {
                error_flag = false;
            }
        } else {
            eof = true;
        }
    } else {
        if (++mp3_read_errors < MP3_READ_ERROR_LIMIT) {
            return;
        }
        error_flag = true;
        eof = true;
    }
}

static void update_time_counters(void) {
    if (sample_rate > 0) {
        elapsed_seconds = (uint16_t)(elapsed_samples / sample_rate);
    }
}

static void fade_clear(void) {
    fade_samples_total = 0;
    fade_samples_remaining = 0;
    fade_direction = 0;
    fade_stop_when_done = false;
}

static void fade_start(int8_t direction, uint8_t seconds, bool stop_when_done) {
    if (seconds == 0) {
        seconds = 1;
    }
    uint32_t rate = sample_rate ? sample_rate : DEFAULT_SAMPLE_RATE;
    fade_samples_total = rate * (uint32_t)seconds;
    fade_samples_remaining = fade_samples_total;
    fade_direction = direction;
    fade_stop_when_done = stop_when_done;
}

static int16_t fade_scale(int16_t sample, uint32_t gain_q15) {
    int32_t scaled = ((int32_t)sample * (int32_t)gain_q15) / 32767;
    if (scaled > 32767) return 32767;
    if (scaled < -32768) return -32768;
    return (int16_t)scaled;
}

static bool apply_fade_to_pcm(int16_t *pcm, uint32_t frames, uint16_t channels) {
    if (!pcm || frames == 0 || channels == 0 || fade_direction == 0 || fade_samples_total == 0) {
        return false;
    }

    bool stop_after_buffer = false;
    for (uint32_t frame = 0; frame < frames; frame++) {
        uint32_t gain_q15 = 32767u;
        if (fade_samples_remaining > 0) {
            if (fade_direction > 0) {
                uint32_t done = fade_samples_total - fade_samples_remaining;
                gain_q15 = (uint32_t)(((uint64_t)done * 32767ull) / fade_samples_total);
            } else {
                gain_q15 = (uint32_t)(((uint64_t)fade_samples_remaining * 32767ull) / fade_samples_total);
            }
        } else if (fade_direction < 0) {
            gain_q15 = 0;
        }

        for (uint16_t ch = 0; ch < channels; ch++) {
            uint32_t pos = frame * (uint32_t)channels + ch;
            pcm[pos] = fade_scale(pcm[pos], gain_q15);
        }

        if (fade_samples_remaining > 0) {
            fade_samples_remaining--;
            if (fade_samples_remaining == 0) {
                stop_after_buffer = (fade_direction < 0 && fade_stop_when_done);
                if (!stop_after_buffer) {
                    fade_clear();
                }
            }
        }
    }
    return stop_after_buffer;
}

static void mp3_apply_frame_sample_rate(const MP3FrameInfo *info) {
    if (!info || info->samprate <= 0) {
        return;
    }

    uint32_t frame_rate = (uint32_t)info->samprate;
    if (frame_rate == sample_rate && audio_format.sample_freq == frame_rate) {
        return;
    }

    printf("MP3: sample rate %lu -> %lu\n", (unsigned long)sample_rate, (unsigned long)frame_rate);
    sample_rate = frame_rate;
    audio_format.sample_freq = frame_rate;
}

static bool wav_apply_sample_rate(uint32_t wav_rate) {
    if (wav_rate == 0) {
        return false;
    }
    if (wav_rate != sample_rate || audio_format.sample_freq != wav_rate) {
        printf("WAV: sample rate %lu -> %lu\n", (unsigned long)sample_rate, (unsigned long)wav_rate);
        sample_rate = wav_rate;
        audio_format.sample_freq = wav_rate;
        if (wavegame_psg_rate_cb) {
            wavegame_psg_rate_cb(wav_rate);
        }
    }
    return true;
}

static int16_t mix_clamp_i16(int32_t sample) {
    if (sample > 32767) return 32767;
    if (sample < -32768) return -32768;
    return (int16_t)sample;
}

static bool resolve_open_file_size(void) {
    if (mp3_file_size != 0) {
        return true;
    }

    FSIZE_t size = f_size(&mp3_file);
    if (size == 0 || size > UINT32_MAX) {
        printf("MP3: invalid file size=%lu\n", (unsigned long)size);
        return false;
    }
    mp3_file_size = (uint32_t)size;
    printf("MP3: resolved file size=%lu\n", (unsigned long)mp3_file_size);
    return true;
}

static int output_pcm_to_i2s(const int16_t *pcm, int output_samps, int channels) {
    if (output_samps <= 0 || channels <= 0) {
        return 0;
    }
    struct audio_buffer *buffer = take_audio_buffer(audio_pool, true);
    if (!buffer) {
        return 0;
    }

    int16_t *dst = (int16_t *)buffer->buffer->bytes;
    if (channels == 1) {
        int frames = output_samps;
        if (frames > MP3_I2S_BUFFER_SAMPLES) {
            frames = MP3_I2S_BUFFER_SAMPLES;
        }
        for (int i = 0; i < frames; i++) {
            int16_t s = pcm[i];
            if (wavegame_psg_sample_cb) {
                s = mix_clamp_i16((int32_t)s + wavegame_psg_sample_cb());
            }
            dst[i * 2] = s;
            dst[i * 2 + 1] = s;
        }
        buffer->sample_count = frames;
    } else {
        int frames = output_samps / channels;
        if (frames > MP3_I2S_BUFFER_SAMPLES) {
            frames = MP3_I2S_BUFFER_SAMPLES;
        }
        if (wavegame_psg_sample_cb) {
            for (int i = 0; i < frames; i++) {
                int16_t psg = wavegame_psg_sample_cb();
                dst[i * 2] = mix_clamp_i16((int32_t)pcm[i * channels] + psg);
                dst[i * 2 + 1] = mix_clamp_i16((int32_t)pcm[i * channels + 1] + psg);
            }
        } else {
            memcpy(dst, pcm, (size_t)frames * 2u * sizeof(int16_t));
        }
        buffer->sample_count = frames;
    }

    give_audio_buffer(audio_pool, buffer);
    return buffer->sample_count;
}

static bool build_child_path(const char *dir, const char *name, char *out, size_t out_size) {
    if (!dir || !dir[0] || !name || !name[0] || !out || out_size == 0) {
        return false;
    }
    int written;
    if (strcmp(dir, "/") == 0) {
        written = snprintf(out, out_size, "/%s", name);
    } else {
        written = snprintf(out, out_size, "%s/%s", dir, name);
    }
    return written > 0 && (size_t)written < out_size;
}

static bool audio_file_exists(const char *path) {
    FILINFO info;
    return path && path[0] && f_stat(path, &info) == FR_OK && !(info.fattrib & AM_DIR);
}

static uint32_t read_cfg_line_offset(const char *path, uint8_t line_number) {
    if (!path || !path[0] || line_number == 0) {
        return 0;
    }
    FIL cfg;
    if (f_open(&cfg, path, FA_READ) != FR_OK) {
        return 0;
    }
    char line[32];
    uint8_t current = 0;
    uint32_t value = 0;
    while (f_gets(line, sizeof(line), &cfg)) {
        current++;
        if (current == line_number) {
            value = (uint32_t)strtoul(line, NULL, 10);
            break;
        }
    }
    f_close(&cfg);
    return value;
}

static bool wavegame_resolve_song(const char *dir, uint8_t index, char *path, size_t path_size,
                                  uint32_t *start_sample, uint32_t *loop_sample) {
    char name[16];
    char cfg_path[MP3_PATH_MAX];
    *start_sample = 0;
    *loop_sample = 0;

    snprintf(name, sizeof(name), "%02u.wav", (unsigned)index);
    if (build_child_path(dir, name, path, path_size) && audio_file_exists(path)) {
        snprintf(name, sizeof(name), "%02u.cfg", (unsigned)index);
        if (build_child_path(dir, name, cfg_path, sizeof(cfg_path))) {
            *loop_sample = read_cfg_line_offset(cfg_path, 1);
            *start_sample = read_cfg_line_offset(cfg_path, 2);
        }
        return true;
    }

    if (!build_child_path(dir, "multi.wav", path, path_size) || !audio_file_exists(path)) {
        return false;
    }
    if (build_child_path(dir, "multi.cfg", cfg_path, sizeof(cfg_path))) {
        *loop_sample = read_cfg_line_offset(cfg_path, 1);
        *start_sample = read_cfg_line_offset(cfg_path, (uint8_t)(index + 1u));
    }
    return true;
}

static bool wav_parse_header(void) {
    uint8_t header[12];
    UINT br = 0;
    bool have_fmt = false;
    bool have_data = false;
    uint32_t wav_sample_rate = 0;

    if (f_lseek(&mp3_file, 0) != FR_OK ||
        f_read(&mp3_file, header, sizeof(header), &br) != FR_OK || br != sizeof(header)) {
        return false;
    }

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }

    uint32_t pos = 12;
    while (pos + 8 <= mp3_file_size) {
        uint8_t chunk[8];
        if (f_lseek(&mp3_file, pos) != FR_OK ||
            f_read(&mp3_file, chunk, sizeof(chunk), &br) != FR_OK || br != sizeof(chunk)) {
            return false;
        }

        uint32_t chunk_size = read_le32(chunk + 4);
        uint32_t chunk_data = pos + 8;
        uint32_t next_pos = chunk_data + chunk_size + (chunk_size & 1u);
        if (chunk_data > mp3_file_size || chunk_size > mp3_file_size - chunk_data) {
            return false;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            UINT fmt_read = (chunk_size < sizeof(fmt)) ? (UINT)chunk_size : (UINT)sizeof(fmt);
            if (chunk_size < 16) {
                return false;
            }
            if (f_lseek(&mp3_file, chunk_data) != FR_OK ||
                f_read(&mp3_file, fmt, fmt_read, &br) != FR_OK || br != fmt_read) {
                return false;
            }
            uint16_t fmt_tag = read_le16(fmt);
            wav_channels = read_le16(fmt + 2);
            wav_sample_rate = read_le32(fmt + 4);
            wav_block_align = read_le16(fmt + 12);
            wav_bits_per_sample = read_le16(fmt + 14);
            bool is_pcm = (fmt_tag == 1);
            if (!is_pcm && fmt_tag == 0xFFFE && chunk_size >= 40) {
                static const uint8_t pcm_guid_tail[14] = {
                    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                    0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
                };
                is_pcm = (read_le16(fmt + 24) == 1 && memcmp(fmt + 26, pcm_guid_tail, sizeof(pcm_guid_tail)) == 0);
            }
            if (!is_pcm || wav_channels == 0 || wav_channels > 2 ||
                wav_block_align == 0 ||
                (wav_bits_per_sample != 8 && wav_bits_per_sample != 16 &&
                 wav_bits_per_sample != 24 && wav_bits_per_sample != 32)) {
                return false;
            }
            uint16_t expected_align = (uint16_t)(wav_channels * ((wav_bits_per_sample + 7u) / 8u));
            if (wav_block_align != expected_align) {
                return false;
            }
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            wav_data_offset = chunk_data;
            wav_data_size = chunk_size;
            have_data = true;
        }

        if (have_fmt && have_data) {
            break;
        }
        pos = next_pos;
    }

    if (!have_fmt || !have_data || wav_data_size == 0 || !wav_apply_sample_rate(wav_sample_rate)) {
        printf("WAV: parse failed fmt=%u data=%u data_size=%lu rate=%lu\n",
               have_fmt ? 1u : 0u, have_data ? 1u : 0u,
               (unsigned long)wav_data_size, (unsigned long)wav_sample_rate);
        return false;
    }

    wav_bytes_remaining = wav_data_size;
    wav_first_buffer_logged = false;
    if (wav_sample_rate > 0 && wav_block_align > 0) {
        uint32_t frame_count = wav_data_size / wav_block_align;
        total_seconds = (uint16_t)(frame_count / wav_sample_rate);
        total_seconds_estimated = false;
    }
    printf("WAV: fmt ch=%u rate=%lu bits=%u align=%u data_offset=%lu data_size=%lu total=%u\n",
           (unsigned)wav_channels, (unsigned long)wav_sample_rate,
           (unsigned)wav_bits_per_sample, (unsigned)wav_block_align,
           (unsigned long)wav_data_offset, (unsigned long)wav_data_size,
           (unsigned)total_seconds);
    return f_lseek(&mp3_file, wav_data_offset) == FR_OK;
}

static int16_t wav_sample_to_s16(const uint8_t *src) {
    if (wav_bits_per_sample == 8) {
        return (int16_t)(((int)src[0] - 128) << 8);
    }
    if (wav_bits_per_sample == 16) {
        return (int16_t)read_le16(src);
    }
    if (wav_bits_per_sample == 24) {
        int32_t sample = (int32_t)src[0] | ((int32_t)src[1] << 8) | ((int32_t)src[2] << 16);
        if (sample & 0x00800000) {
            sample |= (int32_t)0xFF000000;
        }
        return (int16_t)(sample >> 8);
    }
    int32_t sample = (int32_t)read_le32(src);
    return (int16_t)(sample >> 16);
}

static bool wav_seek_to_sample(uint32_t sample) {
    if (wav_block_align == 0) {
        return false;
    }
    uint64_t byte_offset = (uint64_t)sample * (uint64_t)wav_block_align;
    if (byte_offset > wav_data_size) {
        byte_offset = wav_data_size;
    }
    byte_offset -= byte_offset % wav_block_align;
    if (f_lseek(&mp3_file, wav_data_offset + (uint32_t)byte_offset) != FR_OK) {
        error_flag = true;
        return false;
    }
    wav_bytes_remaining = wav_data_size - (uint32_t)byte_offset;
    elapsed_samples = (uint64_t)((uint32_t)byte_offset / wav_block_align);
    update_time_counters();
    eof = false;
    error_flag = false;
    return true;
}

static void wav_rewind_data(void) {
    (void)wav_seek_to_sample(wav_loop_sample);
}

static void wav_update(void) {
    if (wav_bytes_remaining == 0) {
        eof = true;
        playing = false;
        return;
    }

    size_t max_bytes = (size_t)MP3_I2S_BUFFER_SAMPLES * wav_block_align;
    if (max_bytes > mp3_buf_capacity) {
        max_bytes = mp3_buf_capacity;
    }
    if (max_bytes > wav_bytes_remaining) {
        max_bytes = wav_bytes_remaining;
    }
    max_bytes -= max_bytes % wav_block_align;
    if (max_bytes == 0) {
        eof = true;
        playing = false;
        return;
    }

    UINT br = 0;
    sd_activity_note();
    if (f_read(&mp3_file, mp3_buf, (UINT)max_bytes, &br) != FR_OK) {
        printf("WAV: read error remaining=%lu request=%lu\n",
               (unsigned long)wav_bytes_remaining, (unsigned long)max_bytes);
        error_flag = true;
        eof = true;
        playing = false;
        return;
    }
    if (br == 0) {
        eof = true;
        playing = false;
        return;
    }

    uint32_t aligned = br - (br % wav_block_align);
    uint32_t frames = aligned / wav_block_align;
    uint16_t bytes_per_sample = (uint16_t)(wav_bits_per_sample / 8u);
    for (uint32_t frame = 0; frame < frames; frame++) {
        const uint8_t *src = mp3_buf + (frame * wav_block_align);
        pcm_scratch[frame * wav_channels] = wav_sample_to_s16(src);
        if (wav_channels == 2) {
            pcm_scratch[frame * 2 + 1] = wav_sample_to_s16(src + bytes_per_sample);
        }
    }

    if (aligned <= wav_bytes_remaining) {
        wav_bytes_remaining -= aligned;
    } else {
        wav_bytes_remaining = 0;
    }

    bool stop_after_fade = apply_fade_to_pcm(pcm_scratch, frames, wav_channels);
    int frames_sent = output_pcm_to_i2s(pcm_scratch, (int)(frames * wav_channels), wav_channels);
    if (frames_sent > 0) {
        if (!wav_first_buffer_logged) {
            printf("WAV: first buffer bytes=%lu frames=%lu sent=%d first=%d\n",
                   (unsigned long)aligned, (unsigned long)frames,
                   frames_sent, (int)pcm_scratch[0]);
            wav_first_buffer_logged = true;
        }
        elapsed_samples += (uint64_t)frames_sent;
        update_time_counters();
    }

    if (wav_bytes_remaining == 0) {
        printf("WAV: eof elapsed=%u\n", (unsigned)elapsed_seconds);
        eof = true;
        playing = false;
    }
    if (stop_after_fade) {
        printf("WAV: fade stop\n");
        playing = false;
        eof = true;
        fade_clear();
    }
}

void mp3_deinit(void) {
    printf("MP3: deinit\n");

    playing = false;
    set_mute(true);  // Mute before tearing down
    close_file();

    // Stop I2S: disables PIO SM, DMA IRQ, frees in-flight buffer
    if (i2s_ready) {
        audio_i2s_set_enabled(false);
        i2s_ready = false;

        // Release only the I2S state machine; the MSX bus PIO programs stay live.
        i2s_release_sm();
    }

    // Release DMA channel
    if (mp3_dma_channel >= 0) {
        dma_channel_abort(mp3_dma_channel);
        dma_irqn_set_channel_enabled(0, mp3_dma_channel, false);
        dma_channel_unclaim(mp3_dma_channel);
        mp3_dma_channel = -1;
    }

    // Free MP3 decoder
    if (mp3_decoder) {
        MP3FreeDecoder(mp3_decoder);
        mp3_decoder = NULL;
    }

    audio_pool = NULL;
    mp3_buf_used = 0;
    mp3_buf_pos = 0;
    core_cmd_head = 0;
    core_cmd_tail = 0;
    core_cmd_path[0] = '\0';
    eof = false;
    error_flag = false;
    paused = false;
    status_flags = 0;
}

static void mp3_prepare_i2s_handoff(void) {
    playing = false;
    paused = false;
    set_mute(true);
    close_file();

    if (mp3_decoder) {
        MP3FreeDecoder(mp3_decoder);
        mp3_decoder = NULL;
    }

    mp3_buf_used = 0;
    mp3_buf_pos = 0;
    mp3_bytes_read = 0;
    mp3_read_errors = 0;
    elapsed_samples = 0;
    elapsed_seconds = 0;
    total_seconds = 0;
    total_seconds_estimated = false;
    core_cmd_head = 0;
    core_cmd_tail = 0;
    core_cmd_path[0] = '\0';
    eof = false;
    error_flag = false;
    status_flags = 0;
}

void mp3_init(void) {
    printf("MP3: init\n");

    // If a previously handed-off I2S pool was adopted (WAVEGAME relaunch after
    // a menu MP3/WAV), reuse the live pico_audio_i2s pipeline instead of
    // re-running audio_i2s_setup()/audio_new_producer_pool(). The DMA IRQ was
    // already re-enabled on Core 0 via the set_enabled() toggle in
    // claim_rom_audio_handoff_pool(), so here we only re-bind the pool.
    if (mp3_pending_handoff_pool) {
        audio_pool = mp3_pending_handoff_pool;
        mp3_pending_handoff_pool = NULL;
        gpio_init(MP3_I2S_MUTE_PIN);
        gpio_set_dir(MP3_I2S_MUTE_PIN, GPIO_OUT);
        set_mute(true);
        i2s_ready = true;
        reset_decoder_state();
        update_status_flags();
        printf("MP3: init reused handoff pool\n");
        return;
    }

    if (mp3_dma_channel < 0) {
        for (int ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
            if (!dma_channel_is_claimed(ch)) {
                mp3_dma_channel = ch;
                break;
            }
        }
        if (mp3_dma_channel < 0) {
            printf("MP3: no free DMA channel\n");
            error_flag = true;
            update_status_flags();
            return;
        }
        printf("MP3: dma channel=%d\n", mp3_dma_channel);
    }
    gpio_init(MP3_I2S_MUTE_PIN);
    gpio_set_dir(MP3_I2S_MUTE_PIN, GPIO_OUT);
    set_mute(true);  // Start muted to prevent noise during folder browsing

#if EXPLORER_USB_STDIO_DEBUG
    printf("MP3: producer buffers=%u frames=%u PCM_bytes=%u\n",
           MP3_I2S_BUFFER_COUNT, MP3_I2S_BUFFER_SAMPLES,
           MP3_I2S_BUFFER_COUNT * MP3_I2S_BUFFER_SAMPLES * producer_format.sample_stride);
    mp3_debug_heap("before producer pool");
#endif
    audio_pool = audio_new_producer_pool(&producer_format, MP3_I2S_BUFFER_COUNT, MP3_I2S_BUFFER_SAMPLES);
    if (!audio_pool) {
        printf("MP3: audio pool alloc failed\n");
        error_flag = true;
        update_status_flags();
        return;
    }

#if EXPLORER_USB_STDIO_DEBUG
    mp3_debug_heap("producer pool ready; starting I2S");
#endif
    i2s_config.dma_channel = (uint)mp3_dma_channel;
    if (!i2s_start()) {
        printf("MP3: i2s setup/connect failed\n");
        error_flag = true;
        update_status_flags();
        return;
    }
    reset_decoder_state();
    update_status_flags();
#if EXPLORER_USB_STDIO_DEBUG
    mp3_debug_heap("init complete");
#endif
}

// Called from Core 1 only
static void mp3_do_select(const char *path, uint32_t file_size) {
    printf("MP3: select '%s' size=%lu\n", path ? path : "(null)", (unsigned long)file_size);
    playing = false;
    paused = false;
    play_loop = false;
    close_file();

    reset_decoder_state();
    mp3_file_size = file_size;

    if (!path || !path[0]) {
        printf("MP3: select failed (empty path)\n");
        error_flag = true;
        update_status_flags();
        return;
    }

    strncpy(mp3_selected_path, path, sizeof(mp3_selected_path) - 1);
    mp3_selected_path[sizeof(mp3_selected_path) - 1] = '\0';
    selected_file_kind = path_has_extension(mp3_selected_path, "WAV") ? AUDIO_FILE_WAV : AUDIO_FILE_MP3;
    file_open = false;
    update_status_flags();
}

// Internal: enqueue a command from Core 0. Drops the command if the
// queue is full (cannot happen in practice because Core 1 drains the
// queue every loop iteration once mp3_init() completes).
static void core_cmd_push(uint8_t cmd) {
    uint8_t next = (uint8_t)((core_cmd_head + 1u) % MP3_CORE_CMD_QUEUE_SIZE);
    if (next == core_cmd_tail) {
        printf("MP3: cmd queue full, dropping cmd=%u\n", (unsigned)cmd);
        return;
    }
    core_cmd_queue[core_cmd_head] = cmd;
    __mem_fence_release();
    core_cmd_head = next;
}

// Called from Core 0 — queues a select command for Core 1.
void mp3_select_file(const char *path, uint32_t file_size) {
    if (!path || !path[0]) return;
    // Path is shared between Core 0 and Core 1. The MSX menu issues
    // SELECT and waits before issuing the next SELECT, so this single
    // path slot is sufficient (each SELECT is consumed before another
    // arrives).
    strncpy((char *)core_cmd_path, path, MP3_PATH_MAX - 1);
    ((char *)core_cmd_path)[MP3_PATH_MAX - 1] = '\0';
    core_cmd_file_size = file_size;
    core_cmd_push(MP3_CORE_CMD_SELECT);
}

// Called from Core 0 — queues a command for Core 1.
void mp3_send_cmd(uint8_t cmd) {
    core_cmd_push(cmd);
}

void mp3_wavegame_play(const char *path, bool loop, uint32_t start_sample, uint32_t loop_sample, bool fade_in) {
    if (!path || !path[0]) return;
    strncpy((char *)core_cmd_path, path, MP3_PATH_MAX - 1);
    ((char *)core_cmd_path)[MP3_PATH_MAX - 1] = '\0';
    core_cmd_file_size = 0;
    core_cmd_wav_start_sample = start_sample;
    core_cmd_wav_loop_sample = loop_sample;
    core_cmd_wav_loop = loop;
    core_cmd_wav_fade_in = fade_in;
    core_cmd_push(MP3_CORE_CMD_WAVEGAME_PLAY);
}

void mp3_wavegame_play_index(const char *dir, uint8_t index, bool loop) {
    if (!dir || !dir[0]) return;
    strncpy((char *)core_cmd_path, dir, MP3_PATH_MAX - 1);
    ((char *)core_cmd_path)[MP3_PATH_MAX - 1] = '\0';
    core_cmd_wav_index = index;
    core_cmd_wav_loop = loop;
    core_cmd_push(MP3_CORE_CMD_WAVEGAME_PLAY_INDEX);
}

void mp3_wavegame_play_pause(const char *dir) {
    if (!dir || !dir[0]) return;
    strncpy((char *)core_cmd_path, dir, MP3_PATH_MAX - 1);
    ((char *)core_cmd_path)[MP3_PATH_MAX - 1] = '\0';
    core_cmd_push(MP3_CORE_CMD_WAVEGAME_PLAY_PAUSE);
}

void mp3_wavegame_fade_out(uint8_t seconds) {
    core_cmd_wav_seconds = seconds;
    core_cmd_push(MP3_CORE_CMD_WAVEGAME_FADE_OUT);
}

void mp3_wavegame_toggle_pause(void) {
    core_cmd_push(MP3_CORE_CMD_WAVEGAME_TOGGLE_PAUSE);
}

void mp3_wavegame_resume_previous(void) {
    core_cmd_push(MP3_CORE_CMD_WAVEGAME_RESUME_PREV);
}

bool mp3_core1_is_ready(void) {
    return core1_running;
}

bool mp3_core1_has_stopped(void) {
    return core1_stopped;
}

void mp3_adopt_i2s_audio_pool(struct audio_buffer_pool *pool) {
    mp3_pending_handoff_pool = pool;
}

struct audio_buffer_pool *mp3_take_i2s_audio_pool(void) {
    if (!i2s_ready || !audio_pool) {
        return NULL;
    }

    struct audio_buffer *stale_buffer;
    while ((stale_buffer = get_full_audio_buffer(audio_pool, false)) != NULL) {
        stale_buffer->sample_count = 0;
        queue_free_audio_buffer(audio_pool, stale_buffer);
    }

    sample_rate = DEFAULT_SAMPLE_RATE;
    audio_format.sample_freq = DEFAULT_SAMPLE_RATE;

    struct audio_buffer_pool *handoff_pool = audio_pool;
    audio_pool = NULL;
    i2s_ready = false;
    mp3_dma_channel = -1;
    return handoff_pool;
}

struct audio_buffer_pool *mp3_force_i2s_handoff_from_core0(void) {
    playing = false;
    paused = false;
    set_mute(true);
    status_flags = 0;
    core_cmd_head = 0;
    core_cmd_tail = 0;

    if (!i2s_ready || !audio_pool) {
        return NULL;
    }

    sample_rate = DEFAULT_SAMPLE_RATE;
    audio_format.sample_freq = DEFAULT_SAMPLE_RATE;

    struct audio_buffer_pool *handoff_pool = audio_pool;
    audio_pool = NULL;
    i2s_ready = false;
    mp3_dma_channel = -1;
    return handoff_pool;
}

static void mp3_save_wavegame_previous(void) {
    if (!file_open || selected_file_kind != AUDIO_FILE_WAV || mp3_selected_path[0] == '\0') {
        return;
    }
    strncpy(wavegame_previous_path, mp3_selected_path, sizeof(wavegame_previous_path) - 1);
    wavegame_previous_path[sizeof(wavegame_previous_path) - 1] = '\0';
    wavegame_previous_sample = (uint32_t)elapsed_samples;
    wavegame_previous_loop_sample = wav_loop_sample;
    wavegame_previous_loop = play_loop;
}

// Called from Core 1 only
static void mp3_do_play_ex(bool loop, uint32_t start_sample, uint32_t loop_sample, bool fade_in) {
    printf("MP3: play kind=%u path='%s'\n", (unsigned)selected_file_kind, mp3_selected_path);

    if (!file_open && mp3_selected_path[0] != '\0') {
        if (f_open(&mp3_file, mp3_selected_path, FA_READ) != FR_OK) {
            printf("MP3: file open failed\n");
            error_flag = true;
            update_status_flags();
            return;
        }
        if (!resolve_open_file_size()) {
            close_file();
            error_flag = true;
            update_status_flags();
            return;
        }

        if (selected_file_kind == AUDIO_FILE_WAV) {
            if (!wav_parse_header()) {
                printf("WAV: unsupported or invalid file\n");
                close_file();
                error_flag = true;
                update_status_flags();
                return;
            }
        } else {
            UINT br = 0;
            uint8_t hdr[10];
            if (f_read(&mp3_file, hdr, sizeof(hdr), &br) == FR_OK && br == sizeof(hdr)) {
                if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
                    uint32_t id3_size = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) | ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
                    uint32_t skip = id3_size + 10;
                    if (hdr[5] & 0x10) {
                        skip += 10;
                    }
                    if (skip < mp3_file_size) {
                        f_lseek(&mp3_file, skip);
                    } else {
                        f_lseek(&mp3_file, 0);
                    }
                } else {
                    f_lseek(&mp3_file, 0);
                }
            } else {
                f_lseek(&mp3_file, 0);
            }
        }
        file_open = true;
    }
    if (file_open && !error_flag) {
        play_loop = loop;
        wav_loop_sample = loop_sample;
        // If the previous play reached EOF, rewind the file to the start
        if (eof) {
            if (selected_file_kind == AUDIO_FILE_WAV) {
                wav_rewind_data();
            } else {
                f_lseek(&mp3_file, 0);
                reset_decoder_state();
            }
        }
        if (selected_file_kind == AUDIO_FILE_WAV && !wav_seek_to_sample(start_sample)) {
            close_file();
            update_status_flags();
            return;
        }
        playing = true;
        paused = false;
        eof = false;
        if (selected_file_kind == AUDIO_FILE_MP3 && total_seconds == 0 && mp3_file_size > 0) {
            uint64_t bits = (uint64_t)mp3_file_size * 8ull;
            uint64_t denom = (uint64_t)ESTIMATE_BITRATE_KBPS * 1000ull;
            total_seconds = (uint16_t)(bits / denom);
            total_seconds_estimated = true;
        }
        if (fade_in) {
            fade_start(1, 1, false);
        } else {
            fade_clear();
        }
        set_mute(false);  // Unmute when starting playback
        printf("MP3: playback started kind=%u status=%02x\n", (unsigned)selected_file_kind, (unsigned)status_flags);
    }
    update_status_flags();
}

static void mp3_do_play(bool loop) {
    mp3_do_play_ex(loop, 0, 0, false);
}

static void mp3_do_stop(void) {
    printf("MP3: stop\n");
    set_mute(true);  // Mute when stopping
    playing = false;
    paused = false;
    play_loop = false;
    close_file();
    reset_decoder_state();
    update_status_flags();
}

static void mp3_do_pause(void) {
    printf("MP3: pause\n");
    if (playing) {
        playing = false;
        paused = true;
        set_mute(true);
    }
    update_status_flags();
}

static void mp3_do_resume(void) {
    printf("MP3: resume\n");
    if (paused && file_open && !error_flag) {
        paused = false;
        playing = true;
        eof = false;
        set_mute(false);
    } else if (!file_open && mp3_selected_path[0] != '\0') {
        mp3_do_play(play_loop);
    }
    update_status_flags();
}

static void mp3_do_fade_out(uint8_t seconds) {
    printf("MP3: fade out seconds=%u\n", (unsigned)(seconds ? seconds : 1u));
    if (!playing) {
        mp3_do_stop();
        return;
    }
    fade_start(-1, seconds, true);
    update_status_flags();
}

static void mp3_do_toggle_pause(void) {
    if (playing) {
        mp3_do_pause();
    } else {
        mp3_do_resume();
    }
}

static void mp3_do_wavegame_play(const char *path, bool loop, uint32_t start_sample,
                                 uint32_t loop_sample, bool fade_in) {
    if (!path || !path[0]) {
        return;
    }
    mp3_save_wavegame_previous();
    mp3_do_select(path, 0u);
    mp3_do_play_ex(loop, start_sample, loop_sample, fade_in);
}

static void mp3_do_wavegame_play_index(const char *dir, uint8_t index, bool loop) {
    char path[MP3_PATH_MAX];
    uint32_t start_sample = 0;
    uint32_t loop_sample = 0;
    if (!wavegame_resolve_song(dir, index, path, sizeof(path), &start_sample, &loop_sample)) {
        printf("WAVEGAME: missing index=%u\n", (unsigned)index);
        return;
    }
    printf("WAVEGAME: index=%u loop=%u start=%lu loop_offset=%lu path=%s\n",
           (unsigned)index, loop ? 1u : 0u,
           (unsigned long)start_sample, (unsigned long)loop_sample, path);
    mp3_do_wavegame_play(path, loop, start_sample, loop_sample, false);
}

static void mp3_do_wavegame_play_pause(const char *dir) {
    char path[MP3_PATH_MAX];
    if (!build_child_path(dir, "pause.wav", path, sizeof(path)) || !audio_file_exists(path)) {
        printf("WAVEGAME: missing pause.wav\n");
        return;
    }
    printf("WAVEGAME: pause path=%s\n", path);
    mp3_do_wavegame_play(path, false, 0, 0, false);
}

static void mp3_do_wavegame_resume_previous(void) {
    if (wavegame_previous_path[0] == '\0') {
        printf("WAVEGAME: no previous song\n");
        return;
    }
    char path[MP3_PATH_MAX];
    strncpy(path, wavegame_previous_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    printf("WAVEGAME: resume previous path=%s sample=%lu\n",
           path, (unsigned long)wavegame_previous_sample);
    mp3_do_select(path, 0u);
    mp3_do_play_ex(wavegame_previous_loop, wavegame_previous_sample,
                   wavegame_previous_loop_sample, true);
}

static void mp3_do_toggle_mute(void) {
    printf("MP3: toggle mute\n");
    set_mute(!muted);
}

static void mp3_process_commands(void) {
    if (core_cmd_head == core_cmd_tail) return;
    uint8_t cmd = core_cmd_queue[core_cmd_tail];
    __mem_fence_acquire();
    core_cmd_tail = (uint8_t)((core_cmd_tail + 1u) % MP3_CORE_CMD_QUEUE_SIZE);

    switch (cmd) {
        case MP3_CORE_CMD_SELECT: {
            char path_copy[MP3_PATH_MAX];
            strncpy(path_copy, (const char *)core_cmd_path, MP3_PATH_MAX);
            path_copy[MP3_PATH_MAX - 1] = '\0';
            uint32_t fsize = core_cmd_file_size;
            mp3_do_select(path_copy, fsize);
            break;
        }
        case MP3_CORE_CMD_PLAY:
            mp3_do_play(false);
            break;
        case MP3_CORE_CMD_PLAY_LOOP:
            mp3_do_play(true);
            break;
        case MP3_CORE_CMD_STOP:
            mp3_do_stop();
            break;
        case MP3_CORE_CMD_PAUSE:
            mp3_do_pause();
            break;
        case MP3_CORE_CMD_RESUME:
            mp3_do_resume();
            break;
        case MP3_CORE_CMD_TOGGLE_MUTE:
            mp3_do_toggle_mute();
            break;
        case MP3_CORE_CMD_WAVEGAME_PLAY: {
            char path_copy[MP3_PATH_MAX];
            strncpy(path_copy, (const char *)core_cmd_path, MP3_PATH_MAX);
            path_copy[MP3_PATH_MAX - 1] = '\0';
            mp3_do_wavegame_play(path_copy, core_cmd_wav_loop,
                                 core_cmd_wav_start_sample,
                                 core_cmd_wav_loop_sample,
                                 core_cmd_wav_fade_in);
            break;
        }
        case MP3_CORE_CMD_WAVEGAME_FADE_OUT:
            mp3_do_fade_out(core_cmd_wav_seconds);
            break;
        case MP3_CORE_CMD_WAVEGAME_TOGGLE_PAUSE:
            mp3_do_toggle_pause();
            break;
        case MP3_CORE_CMD_WAVEGAME_RESUME_PREV:
            mp3_do_wavegame_resume_previous();
            break;
        case MP3_CORE_CMD_WAVEGAME_PLAY_INDEX: {
            char dir_copy[MP3_PATH_MAX];
            strncpy(dir_copy, (const char *)core_cmd_path, MP3_PATH_MAX);
            dir_copy[MP3_PATH_MAX - 1] = '\0';
            mp3_do_wavegame_play_index(dir_copy, core_cmd_wav_index, core_cmd_wav_loop);
            break;
        }
        case MP3_CORE_CMD_WAVEGAME_PLAY_PAUSE: {
            char dir_copy[MP3_PATH_MAX];
            strncpy(dir_copy, (const char *)core_cmd_path, MP3_PATH_MAX);
            dir_copy[MP3_PATH_MAX - 1] = '\0';
            mp3_do_wavegame_play_pause(dir_copy);
            break;
        }
        case MP3_CORE_CMD_SHUTDOWN:
            mp3_prepare_i2s_handoff();
            core1_keep_i2s_on_exit = true;
            core1_running = false;
            __mem_fence_release();
            break;
        default:
            break;
    }
}

void mp3_update(void) {
    if (error_flag) {
        playing = false;
        set_mute(true);  // Mute on error
        update_status_flags();
        return;
    }

    if (!playing || !file_open) {
        update_status_flags();
        return;
    }

    if (selected_file_kind == AUDIO_FILE_WAV) {
        wav_update();
        if (!playing && eof && play_loop && !error_flag && file_open) {
            wav_rewind_data();
            eof = false;
            playing = true;
        }
        if (!playing) {
            set_mute(true);
        }
        update_status_flags();
        return;
    }

    if (!mp3_decoder) {
#if EXPLORER_USB_STDIO_DEBUG
        mp3_debug_heap("before decoder init");
#endif
        mp3_decoder = MP3InitDecoder();
        if (!mp3_decoder) {
            printf("MP3: decoder init failed\n");
            error_flag = true;
            playing = false;
            set_mute(true);
            update_status_flags();
            return;
        }
#if EXPLORER_USB_STDIO_DEBUG
        printf("MP3: decoder ready\n");
#endif
    }

    int decode_errors = 0;
    int frames_decoded = 0;
    // Decode continuously until compressed buffer needs refill.
    // take_audio_buffer(true) naturally rate-limits to DMA speed.
    // Check commands every MP3_CMD_CHECK_INTERVAL frames for responsiveness.
    for (;;) {
        size_t available = (mp3_buf_used > mp3_buf_pos) ? (mp3_buf_used - mp3_buf_pos) : 0;
        if (available < MP3_MIN_BUFFER_SIZE) {
            if (eof) playing = false;
            break;
        }

        // Periodic command check for responsiveness (~416ms at 44.1kHz)
        if (frames_decoded > 0 && (frames_decoded % MP3_CMD_CHECK_INTERVAL) == 0) {
            mp3_process_commands();
            if (!playing) break;
        }

        size_t base_pos = mp3_buf_pos;
        unsigned char *read_ptr = mp3_buf + mp3_buf_pos;
        int bytes_left = (int)available;
        int offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (offset < 0) {
            mp3_buf_pos = mp3_buf_used;
            break;
        }
        read_ptr += offset;
        bytes_left -= offset;

#if EXPLORER_USB_STDIO_DEBUG
        bool log_frame = !mp3_first_frame_logged;
        mp3_first_frame_logged = true;
        if (log_frame)
            printf("MP3: first decode bytes=%d\n", bytes_left);
#endif
        int err = MP3Decode(mp3_decoder, &read_ptr, &bytes_left, pcm_scratch, 0);
#if EXPLORER_USB_STDIO_DEBUG
        if (log_frame)
            printf("MP3: first decode result=%d remaining=%d\n", err, bytes_left);
#endif
        if (read_ptr >= mp3_buf && read_ptr <= (mp3_buf + mp3_buf_used)) {
            mp3_buf_pos = (size_t)(read_ptr - mp3_buf);
        }

        if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            break;
        }
        if (err != ERR_MP3_NONE) {
            mp3_buf_pos = base_pos + (size_t)offset + 1;
            if (++decode_errors >= 6) break;
            continue;
        }
        decode_errors = 0;

        MP3FrameInfo info;
        MP3GetLastFrameInfo(mp3_decoder, &info);
        mp3_apply_frame_sample_rate(&info);
        if (info.bitrate > 0 && mp3_file_size > 0 && (total_seconds == 0 || total_seconds_estimated)) {
            uint64_t bits = (uint64_t)mp3_file_size * 8ull;
            uint64_t denom = (uint64_t)info.bitrate * 1000ull;
            total_seconds = (uint16_t)(bits / denom);
            total_seconds_estimated = false;
        }

        if (info.outputSamps > 0 && info.nChans > 0) {
#if EXPLORER_USB_STDIO_DEBUG
            if (log_frame)
                printf("MP3: first I2S submit samples=%d channels=%d\n", info.outputSamps, info.nChans);
#endif
            int frames_sent = output_pcm_to_i2s(pcm_scratch, info.outputSamps, info.nChans);
#if EXPLORER_USB_STDIO_DEBUG
            if (log_frame)
                printf("MP3: first I2S submit complete frames=%d\n", frames_sent);
#endif
            if (frames_sent > 0) {
                elapsed_samples += (uint64_t)frames_sent;
                if (sample_rate > 0) {
                    elapsed_seconds = (uint16_t)(elapsed_samples / sample_rate);
                }
            }
        }

        frames_decoded++;
    }

    // Mute if playback stopped during decode (EOF or command)
    if (!playing) {
        set_mute(true);
    }

    update_time_counters();
    update_status_flags();
}

void mp3_core1_loop(void) {
    printf("MP3: core1 loop start\n");
    core1_stopped = false;
    core1_keep_i2s_on_exit = false;
    __mem_fence_release();
    mp3_init();
    core1_running = true;
    __mem_fence_release();

    while (core1_running) {
        mp3_process_commands();
        if (playing) {
            // Active playback: tight loop to keep the I2S buffer fed.
            if (selected_file_kind == AUDIO_FILE_MP3) {
                fill_buffer_if_needed();
            }
            mp3_update();
        } else {
            // Idle: yield to avoid contending with Core 0 on shared
            // buses (XIP, SIO, peripherals). 200 us is well below any
            // user-perceptible MP3 command latency yet stops Core 1
            // from starving Core 0's SD enumeration / PIO service.
            sleep_us(200);
        }
        if (bg_callback) {
            bg_callback();
        }
    }

    if (!core1_keep_i2s_on_exit) {
        mp3_deinit();
    }
    core1_keep_i2s_on_exit = false;
    core1_running = false;
    core1_stopped = true;
    __mem_fence_release();
}

void mp3_set_bg_callback(mp3_bg_callback_t cb) {
    bg_callback = cb;
}

void mp3_stop_core1(void) {
    core1_running = false;
}

void mp3_request_shutdown(void) {
    core_cmd_push(MP3_CORE_CMD_SHUTDOWN);
}

uint8_t mp3_get_status(void) {
    return status_flags;
}

uint16_t mp3_get_elapsed_seconds(void) {
    return elapsed_seconds;
}

uint16_t mp3_get_total_seconds(void) {
    return total_seconds;
}

void mp3_auto_play(const char *path, uint32_t file_size) {
    mp3_do_select(path, file_size);
    mp3_do_play(false);
}
