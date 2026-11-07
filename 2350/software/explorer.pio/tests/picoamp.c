#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "mp3dec.h"
#include "picoamp/picoamp_bus.h"
#include "picoamp/mp3_alloc_dispatch.h"
#include "picoamp/pvaudio/pva_internal.h"
#include "audio/emu2413.h"

int host_disk_open(const char *path, uint32_t create_sectors);
int host_esp_load_audio(const char *path);
void host_advance_ms(uint32_t ms);
extern bool host_audio_unlimited, host_esp_present;
extern uint32_t host_audio_frames;
extern size_t host_stream_ram_size;
extern void (*host_audio_sink)(const int16_t *, uint32_t, uint32_t);
extern void *pva_mp3_alloc(size_t size);

static picoamp_bus_t bus;
static uint8_t token;
static uint64_t energy;

static void tick(unsigned ms)
{
    while (ms--)
    {
        pva_core1_poll();
        host_advance_ms(1);
    }
}

static int command(uint8_t code, const void *args, size_t size)
{
    const uint8_t *bytes = args;
    picoamp_bus_write(&bus, 0xFFFF, 0x0C);
    for (size_t i = 0; i < size; i++)
        picoamp_bus_write(&bus, PVA_WIN_BASE + PVA_R_ARGS + (uint16_t)i, bytes[i]);
    picoamp_bus_write(&bus, PVA_WIN_BASE + PVA_R_CMD_TOKEN, ++token);
    picoamp_bus_write(&bus, PVA_WIN_BASE + PVA_R_CMD_CODE, code);
    for (unsigned i = 0; i < 2000; i++)
    {
        tick(1);
        if (picoamp_bus_read(&bus, PVA_WIN_BASE + PVA_R_DONE_TOKEN) == token)
            return picoamp_bus_read(&bus, PVA_WIN_BASE + PVA_R_RESULT);
    }
    fprintf(stderr, "PicoAmp command %02x timed out\n", code);
    abort();
}

static int text_command(uint8_t code, const char *args)
{
    return command(code, args, strlen(args) + 1);
}

static void put(const char *path, const void *bytes, UINT size)
{
    FIL file;
    UINT written;
    assert(f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    assert(f_write(&file, bytes, size, &written) == FR_OK && written == size);
    assert(f_close(&file) == FR_OK);
}

static void build_card(void)
{
    static FATFS fs;
    uint8_t work[4096];
    assert(host_disk_open("picoamp-test.img", 64 * 2048) == 0);
    MKFS_PARM options = { FM_FAT, 0, 0, 0, 0 };
    assert(f_mkfs("0:", &options, work, sizeof(work)) == FR_OK);
    assert(f_mount(&fs, "0:", 1) == FR_OK);
    assert(f_mkdir("0:/MUSIC") == FR_OK);

    // Independent silent MPEG-1 Layer III frames: no tags or bit reservoir.
    uint8_t mp3[417 * 200] = {0};
    for (size_t i = 0; i < sizeof(mp3); i += 417)
    {
        mp3[i] = 0xFF; mp3[i + 1] = 0xFB; mp3[i + 2] = 0x90;
    }
    put("0:/MUSIC/SILENCE.MP3", mp3, sizeof(mp3));
    FILE *fixture = fopen("silence.mp3", "wb");
    assert(fixture && fwrite(mp3, 1, sizeof(mp3), fixture) == sizeof(mp3));
    assert(fclose(fixture) == 0);

    uint32_t wav[11] = {
        0x46464952, 36 + 88200 * 2, 0x45564157, 0x20746D66, 16,
        0x00010001, 44100, 88200, 0x00100002, 0x61746164, 88200 * 2
    };
    FIL file;
    UINT written;
    assert(f_open(&file, "0:/MUSIC/TONE.WAV", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    assert(f_write(&file, wav, sizeof(wav), &written) == FR_OK && written == sizeof(wav));
    int16_t pcm[441];
    for (unsigned i = 0; i < 441; i++)
        pcm[i] = (i % 100 < 50) ? 4096 : -4096;
    for (unsigned i = 0; i < 200; i++)
        assert(f_write(&file, pcm, sizeof(pcm), &written) == FR_OK && written == sizeof(pcm));
    assert(f_close(&file) == FR_OK);
    const char playlist[] = "#EXTM3U\nSILENCE.MP3\nTONE.WAV\n";
    put("0:/MUSIC/MIX.M3U", playlist, sizeof(playlist) - 1);
    assert(f_mount(NULL, "0:", 0) == FR_OK);
}

static void sink(const int16_t *pcm, uint32_t frames, uint32_t rate)
{
    assert(rate == 44100);
    for (uint32_t i = 0; i < frames * 2; i++)
        energy += (uint64_t)abs(pcm[i]);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    static uint8_t rom[65536];
    FILE *file = fopen(argv[1], "rb");
    assert(file && fread(rom, 1, sizeof(rom), file) == sizeof(rom));
    assert(fgetc(file) == EOF && fclose(file) == 0);
    bus.rom = rom;

    // Exercise the same decoder allocator used by menu/WAVEGAME, repeatedly.
    for (unsigned i = 0; i < 10; i++)
    {
        assert(pv_mp3_use_menu_allocator());
        HMP3Decoder decoder = MP3InitDecoder();
        assert(decoder);
        MP3FreeDecoder(decoder);
    }
    build_card();
    pva_init(OPLL_borrow_table_memory(128u * 1024u), 128u * 1024u);
    pva_allow_card_writes(true);
    pv_mp3_alloc_fn = pva_mp3_alloc;
    host_stream_ram_size = 256u * 1024u;
    host_audio_unlimited = false;
    host_esp_present = false;
    pva_core1_init();
    host_audio_sink = sink;

    for (unsigned bank = 0; bank < 4; bank++)
    {
        picoamp_bus_write(&bus, 0xFFFF, 0);
        picoamp_bus_write(&bus, 0x67FF, (uint8_t)(bank | 0xFC));
        for (unsigned offset = 0; offset < 16384; offset += 127)
            assert(picoamp_bus_read(&bus, 0x4000 + offset) == rom[bank * 16384 + offset]);
    }
    picoamp_bus_write(&bus, 0xFFFF, 0x0C);
    assert(picoamp_bus_read(&bus, 0xFFFF) == 0xF3);
    assert(picoamp_bus_read(&bus, 0x4000) == 'P');
    picoamp_bus_write(&bus, 0x6000, 0);
    assert(bus.bank == 3);
    assert(picoamp_bus_read(&bus, 0x5000) == 0xFF);
    for (unsigned sub = 1; sub < 3; sub++)
    {
        picoamp_bus_write(&bus, 0xFFFF, (uint8_t)(sub << 2));
        picoamp_bus_write(&bus, 0x6000, 1);
        assert(bus.bank == 3 && picoamp_bus_read(&bus, 0x4000) == 0xFF);
    }
    assert(picoamp_bus_read(&bus, 0) == 0xFF && picoamp_bus_read(&bus, 0x8000) == 0xFF);
    assert(command(PVA_C_PING, NULL, 0) == PVA_OK);
    assert(pva_win[PVA_R_STATUS + PVA_ST_MOUNT_FR] == FR_OK);
    assert(pva_win[PVA_R_STATUS + PVA_ST_VOLUME] == 80);
    assert(text_command(PVA_C_DIR_OPEN, "/MUSIC") == PVA_OK);
    uint8_t list[] = {0, 0, 10};
    assert(command(PVA_C_DIR_READ, list, sizeof(list)) == PVA_OK);

    for (unsigned i = 0; i < 3; i++)
    {
        uint32_t before = host_audio_frames;
        assert(text_command(PVA_C_PLAY_PATH, "/MUSIC/SILENCE.MP3") == PVA_OK);
        tick(200);
        assert(host_audio_frames > before);
        assert(pva_win[PVA_R_STATUS + PVA_ST_ERROR] == PVA_OK);
        assert(command(PVA_C_STOP, NULL, 0) == PVA_OK);
    }
    assert(text_command(PVA_C_PLAY_PATH, "/MUSIC/TONE.WAV") == PVA_OK);
    tick(200);
    assert(energy > 0);
    assert(command(PVA_C_PAUSE, NULL, 0) == PVA_OK);
    tick(100);
    assert(pva_win[PVA_R_STATUS + PVA_ST_STATE] == PVA_STATE_PAUSED);
    assert(command(PVA_C_PAUSE, NULL, 0) == PVA_OK);
    uint8_t seek[] = {1, 0};
    assert(command(PVA_C_SEEK, seek, sizeof(seek)) == PVA_OK);
    assert(text_command(PVA_C_PLAY_PATH, "/MUSIC/MIX.M3U") == PVA_OK);
    assert(command(PVA_C_NEXT, NULL, 0) == PVA_OK);
    tick(100);
    assert(command(PVA_C_STOP, NULL, 0) == PVA_OK);
    assert(text_command(PVA_C_PLAY_PATH, "/MUSIC/MISSING.MP3") == PVA_E_NOTFOUND);

    const char cfg[] = "clock.face\0analog";
    assert(command(PVA_C_CFG_SET, cfg, sizeof(cfg)) == PVA_OK);
    assert(command(PVA_C_CFG_SAVE, NULL, 0) == PVA_OK);
    assert(command(PVA_C_CFG_LOAD, NULL, 0) == PVA_OK);
    assert(text_command(PVA_C_CFG_GET, "clock.face") == PVA_OK);
    assert(!strcmp((const char *)pva_win + PVA_R_REPLY, "analog"));
    pva_allow_card_writes(false);
    assert(command(PVA_C_CFG_SAVE, NULL, 0) == PVA_E_RDONLY);
    pva_allow_card_writes(true);

    const char station[] = "http://radio.test/stream.mp3\0Test FM";
    assert(command(PVA_C_PLAY_URL, station, sizeof(station)) == PVA_E_NOWIFI);
    host_esp_present = true;
    assert(host_esp_load_audio("silence.mp3") == 0);
    assert(command(PVA_C_PLAY_URL, station, sizeof(station)) == PVA_OK);
    tick(6000);
    assert(pva_win[PVA_R_STATUS + PVA_ST_ERROR] == PVA_OK);
    assert(pva_win[PVA_R_STATUS + PVA_ST_STATE] == PVA_STATE_PLAYING);
    uint8_t current[] = {0xFF, 0xFF};
    assert(command(PVA_C_FAV_ADD, current, sizeof(current)) == PVA_OK);
    assert(command(PVA_C_FAV_ADD, current, sizeof(current)) == PVA_E_EXISTS);
    assert(command(PVA_C_STOP, NULL, 0) == PVA_OK);
    assert(pva_arena_free() > 0);
    printf("PASS: PicoAmp ROM banks/subslots, 128KB arena, repeated MP3 decode, WAV/playlist, settings and simulated radio/favourites (%zu arena bytes free)\n", pva_arena_free());
    return 0;
}
