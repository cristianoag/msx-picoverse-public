// Host platform for the PicoVerse Audio Engine: FatFS disk I/O over an image
// file, an audio HAL that counts (and optionally records) the output, and a
// simulated millisecond clock. Used by the host test and the openMSX device.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "../pva_internal.h"

// ---------------- disk image ----------------
static FILE    *img;
static uint32_t img_sectors;
uint32_t host_disk_reads;

int host_disk_open(const char *path, uint32_t create_sectors)
{
    if (img) fclose(img);
    img = fopen(path, create_sectors ? "w+b" : "r+b");
    if (!img) img = fopen(path, "rb");
    if (!img) return -1;
    // no stdio buffer: in openMSX the MSX side writes the same image (SymbOS
    // saving a file), and the engine must read what is on the "card" now
    setvbuf(img, NULL, _IONBF, 0);
    if (create_sectors)
    {
        img_sectors = create_sectors;
        fseek(img, (long)create_sectors * 512 - 1, SEEK_SET);
        fputc(0, img);
        fflush(img);
    }
    else
    {
        fseek(img, 0, SEEK_END);
        img_sectors = (uint32_t)(ftell(img) / 512);
    }
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv) { return (pdrv == 0 && img) ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE pdrv)     { return (pdrv == 0 && img) ? 0 : STA_NOINIT; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || !img || sector + count > img_sectors) return RES_PARERR;
    host_disk_reads += count;
    fseek(img, (long)sector * 512, SEEK_SET);
    return fread(buff, 512, count, img) == count ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || !img || sector + count > img_sectors) return RES_PARERR;
    fseek(img, (long)sector * 512, SEEK_SET);
    return fwrite(buff, 512, count, img) == count ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv || !img) return RES_PARERR;
    switch (cmd)
    {
        case CTRL_SYNC:        fflush(img); return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buff = img_sectors; return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD *)buff = 512; return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
        default:               return RES_OK;
    }
}

DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (10u << 21) | (23u << 16); }

// ---------------- audio HAL ----------------
// The simulated I2S consumer drains 'host_audio_rate' frames per second of
// simulated time from a queue of HOST_BUFFERS buffers, like pico_audio_i2s.
#define HOST_BUFFERS 4
static int16_t  hal_buf[1152 * 2];
static FILE    *wav;
static uint32_t wav_frames, wav_rate;
uint32_t host_audio_frames;
uint32_t host_audio_last_rate;
bool     host_audio_muted = true;
bool     host_audio_unlimited = true;   // true: never full (fast tests)
static uint32_t queued_frames;          // frames waiting in the simulated queue
static uint32_t queue_buffers;
uint32_t host_audio_underruns;
void (*host_audio_sink)(const int16_t *pcm, uint32_t frames, uint32_t rate);

void host_wav_open(const char *path)
{
    wav = fopen(path, "wb");
    static const uint8_t hdr[44] = { 0 };
    fwrite(hdr, 1, 44, wav);
    wav_frames = 0;
}

void host_wav_close(void)
{
    if (!wav) return;
    uint32_t data = wav_frames * 4, riff = data + 36, fmt_sz = 16, rate = wav_rate ? wav_rate : 44100;
    uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
    uint32_t brate = rate * 4;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
    fwrite(&fmt_sz, 4, 1, wav); fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav);
    fwrite(&rate, 4, 1, wav); fwrite(&brate, 4, 1, wav); fwrite(&align, 2, 1, wav);
    fwrite(&bits, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav);
    fclose(wav);
    wav = NULL;
}

// same arena use as the firmware HAL: 5 x 1152 stereo frames + bookkeeping
bool pva_hal_audio_init(void) { return pva_alloc(5u * 1152u * 4u + 160u) != NULL; }
void pva_hal_audio_mute(bool mute) { host_audio_muted = mute; }

int16_t *pva_hal_audio_acquire(uint32_t *max_frames)
{
    if (!host_audio_unlimited && queue_buffers >= HOST_BUFFERS) return NULL;
    *max_frames = 1152;
    return hal_buf;
}

void pva_hal_audio_commit(uint32_t frames, uint32_t sample_rate)
{
    host_audio_frames += frames;
    host_audio_last_rate = sample_rate;
    if (!host_audio_unlimited) { queue_buffers++; queued_frames += frames; }
    if (host_audio_sink) host_audio_sink(hal_buf, frames, sample_rate);
    if (wav)
    {
        fwrite(hal_buf, 4, frames, wav);
        wav_frames += frames;
        wav_rate = sample_rate;
    }
}

static uint32_t fake_ms;
uint32_t pva_hal_millis(void) { return fake_ms; }

// advance simulated time; the consumer drains the queue at the last rate
void host_esp_tick(uint32_t now);

void host_advance_ms(uint32_t ms)
{
    fake_ms += ms;
    host_esp_tick(fake_ms);
    if (host_audio_unlimited) return;
    uint32_t drain = (host_audio_last_rate ? host_audio_last_rate : 44100) * ms / 1000u;
    if (queue_buffers && queued_frames <= drain) { if (!host_audio_muted) host_audio_underruns++; }
    if (queued_frames <= drain) { queued_frames = 0; queue_buffers = 0; }
    else
    {
        queued_frames -= drain;
        uint32_t b = (queued_frames + 1151u) / 1152u;
        if (b < queue_buffers) queue_buffers = b;
    }
}

void pva_hal_install_fault_handler(void) { }
