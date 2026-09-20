#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "emu2413.h"
#define __not_in_flash_func(name) name
#include "sunrise_ide.h"
#include "fm-constants.h"
#define __dmb() ((void)0)
#define ATRACE_TIME_FM(statement) statement

static bool msx_music_ready = true, main_psg_ready = true;
static bool msx_music_shared_audio, msx_music_core1_services_io;
static OPLL *msx_music_instance;
static int *msx_music_lock, *main_psg_lock, main_psg_instance;
static uint16_t msx_music_write_ring[MSX_MUSIC_WRITE_RING_SIZE];
static uint16_t main_psg_write_ring[MAIN_PSG_WRITE_RING_SIZE];
static uint32_t msx_music_write_ring_head, msx_music_write_ring_tail;
static uint32_t main_psg_write_ring_head, main_psg_write_ring_tail;
static float msx_music_dc_x1, msx_music_dc_y1, msx_music_lp_y1;
static unsigned locks, drops, fm_writes, psg_writes;
static uint16_t fm_log[1024], psg_log[1024];
static bool refill_ring;

static uint32_t spin_lock_blocking(int *lock) { (void)lock; locks++; return 0; }
static void spin_unlock(int *lock, uint32_t save) { (void)lock; (void)save; }
// Which core the production code believes it is running on. Core 1 owns the
// OPLL; writes arriving on Core 0 must be queued rather than applied.
static unsigned test_core_num = 1u;
static unsigned get_core_num(void) { return test_core_num; }
static void atrace_note_opll_drop(void) { drops++; }
static void record_opll_write(OPLL *opll, uint8_t port, uint8_t data)
{
    fm_log[fm_writes++] = (uint16_t)((port << 8) | data);
    OPLL_writeIO(opll, port, data);
    if (refill_ring) {
        uint32_t head = msx_music_write_ring_head;
        msx_music_write_ring[head] = 0x7c00;
        msx_music_write_ring_head = (head + 1u) & MSX_MUSIC_WRITE_RING_MASK;
    }
}
#define OPLL_writeIO record_opll_write
static void PSG_writeIO(int *psg, uint8_t port, uint8_t data)
{
    (void)psg;
    psg_log[psg_writes++] = (uint16_t)((port << 8) | data);
}

struct byte_buffer { uint8_t *bytes; };
struct audio_buffer { struct byte_buffer *buffer; unsigned sample_count; };
static int16_t pcm[SCC_AUDIO_BUFFER_SAMPLES * 2];
static struct byte_buffer bytes = { (uint8_t *)pcm };
static struct audio_buffer fixture = { &bytes, 0 };
static bool msx_music_audio_started, available;
static void *msx_music_audio_pool;
static unsigned services, rendered_samples, gives, takes;
static void msx_music_service_io(void) { services++; }
static struct audio_buffer *take_audio_buffer(void *pool, bool block)
{
    (void)pool;
    assert(!block);
    takes++;
    if (!available) return NULL;
    available = false;
    return &fixture;
}
static void give_audio_buffer(void *pool, struct audio_buffer *buffer)
{
    (void)pool;
    assert(buffer == &fixture && buffer->sample_count == SCC_AUDIO_BUFFER_SAMPLES);
    for (unsigned i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++) {
        assert(pcm[2*i] == (int16_t)i && pcm[2*i+1] == (int16_t)i);
    }
    gives++;
}
static void msx_music_write_stereo_sample(int16_t *samples, int index)
{
    samples[2*index] = samples[2*index+1] = (int16_t)index;
    rendered_samples++;
}

#define SD_PDRV 0
#define RES_OK 0
#define USB_TRANSFER_TIMEOUT_US 1000u
typedef int DRESULT;
static sunrise_ide_t ide, *sd_ide_ctx, *usb_ide_ctx;
static bool usb_read_requested, usb_write_requested, usb_device_mounted;
static bool sd_device_mounted, usb_read_in_progress, usb_write_in_progress;
static uint32_t usb_read_lba, usb_write_lba, sd_lba_base, sd_block_count;
static uint32_t usb_block_size, usb_block_count;
static uint64_t usb_transfer_start_us, now_us;
static uint8_t usb_write_buffer[512], usb_read_buffer[512], current_dev_addr, current_lun;
static int disk_result;
static char events[16];
static unsigned event_count;
static jmp_buf backend_return;
static void event(char ch) { assert(event_count < sizeof(events)-1); events[event_count++] = ch; }
void service_system_audio(void) { event('A'); longjmp(backend_return, 1); }
static void tuh_task(void) { event('T'); }
static uint64_t time_us_64(void) { return now_us; }
static DRESULT disk_read_raw(int drive, uint8_t *buffer, uint32_t lba, int count)
{
    assert(drive == 0 && lba == sd_lba_base + usb_read_lba && count == 1);
    event('R'); buffer[0] = 0x5a; return disk_result;
}
static DRESULT disk_write_raw(int drive, uint8_t *buffer, uint32_t lba, int count)
{
    assert(drive == 0 && lba == sd_lba_base + usb_write_lba && count == 1);
    assert(buffer == usb_write_buffer); event('W'); return disk_result;
}
static bool read_complete_cb(void) { return true; }
static bool write_complete_cb(void) { return true; }
static bool tuh_msc_read10(uint8_t dev, uint8_t lun, uint8_t *buffer, uint32_t lba,
                           int count, bool (*callback)(void), uintptr_t offset)
{
    (void)dev; (void)lun; (void)buffer;
    assert(lba == usb_read_lba && count == 1 && callback == read_complete_cb && offset == 0);
    event('R'); usb_ide_ctx->usb_read_ready = true; usb_read_in_progress = false; return true;
}
static bool tuh_msc_write10(uint8_t dev, uint8_t lun, uint8_t *buffer, uint32_t lba,
                            int count, bool (*callback)(void), uintptr_t offset)
{
    (void)dev; (void)lun;
    assert(buffer == usb_write_buffer && lba == usb_write_lba && count == 1);
    assert(callback == write_complete_cb && offset == 0);
    event('W'); usb_ide_ctx->usb_write_ready = true; usb_write_in_progress = false; return true;
}
static void ide_advance_lba(sunrise_ide_t *ctx) { ctx->sector++; }
static void build_identify_data(uint8_t *buffer) { event('I'); buffer[0] = 0xec; }
static void build_identify_data_sd(uint8_t *buffer) { build_identify_data(buffer); }

#include "fm-production.h"

static void reset_opll(void)
{
    msx_music_instance = OPLL_new(MSX_MUSIC_CLOCK, MSX_MUSIC_SAMPLE_RATE);
    assert(msx_music_instance && !msx_music_instance->conv);
    OPLL_reset(msx_music_instance);
    OPLL_resetPatch(msx_music_instance, OPLL_2413_TONE);
    OPLL_writeReg(msx_music_instance, 0x30, 0x10);
    OPLL_writeReg(msx_music_instance, 0x10, 0x80);
    msx_music_dc_x1 = msx_music_dc_y1 = msx_music_lp_y1 = 0;
    locks = drops = fm_writes = psg_writes = 0;
    msx_music_write_ring_head = msx_music_write_ring_tail = 0;
    main_psg_write_ring_head = main_psg_write_ring_tail = 0;
    test_core_num = 1u;
}

static void test_native_render(void)
{
    int32_t baseline[8192];
    reset_opll(); msx_music_shared_audio = false; msx_music_core1_services_io = true;
    msx_music_write_io(0x7c, 0x20); msx_music_write_io(0x7d, 0x15);
    assert(locks == 2 && msx_music_instance->reg[0x20] == 0x15); locks = 0;
    for (unsigned i = 0; i < 8192; i++) baseline[i] = msx_music_calc_sample();
    assert(locks == 8192); OPLL_delete(msx_music_instance);

    reset_opll(); msx_music_shared_audio = true; msx_music_core1_services_io = false;
    test_core_num = 0u;  /* the SYSTEM producer runs on Core 0 */
    msx_music_write_io(0x7c, 0x20); msx_music_write_io(0x7d, 0x15);
    assert(locks == 0 && fm_writes == 0 && msx_music_instance->reg[0x20] == 0);
    test_core_num = 1u;  /* Core 1 owns the emulator and drains the ring */
    msx_music_drain_write_ring();
    assert(locks == 1 && fm_writes == 2 && msx_music_instance->reg[0x20] == 0x15); locks = 0;
    for (unsigned i = 0; i < 8192; i++) assert(baseline[i] == msx_music_calc_sample());
    assert(locks == 0 && drops == 0); OPLL_delete(msx_music_instance);
}

// Regression: the YM2413 keeps a single address latch, so an address write and
// its data write must be applied by the same core. The FM-PAC loaders drive two
// register interfaces at once - Core 1 drains the I/O ports (0x7C/0x7D) while
// Core 0 dispatches the memory-mapped aperture (0x7FF4/0x7FF5) from the bus
// loop - and applying both directly let them interleave halves of different
// writes, losing one register write and poking a stray value into another.
// Heard as missing or wrong instruments and notes that never stop.
static void test_latch_owned_by_one_core(void)
{
    reset_opll();
    msx_music_shared_audio = false;
    msx_music_core1_services_io = true;   // Core 1 owns the I/O FIFO

    // A write originating on Core 0 must be queued, never applied in place,
    // even though Core 1 owns the FIFO.
    test_core_num = 0u;
    msx_music_write_io(0x7c, 0x30);
    assert(fm_writes == 0 && locks == 0);
    assert(((msx_music_write_ring_head - msx_music_write_ring_tail) &
            MSX_MUSIC_WRITE_RING_MASK) == 1u);

    // Core 1 interleaves its own register write while Core 0's is still queued.
    test_core_num = 1u;
    msx_music_write_io(0x7c, 0x21);
    msx_music_write_io(0x7d, 0x1d);
    assert(msx_music_instance->reg[0x21] == 0x1d);

    // Draining Core 0's queued address now cannot corrupt the pair Core 1 just
    // completed, and its own data byte follows it in order.
    test_core_num = 0u;
    msx_music_write_io(0x7d, 0x1f);
    test_core_num = 1u;
    msx_music_drain_write_ring();
    assert(msx_music_instance->reg[0x30] == 0x1f);
    assert(msx_music_instance->reg[0x21] == 0x1d);
    assert(drops == 0);
    OPLL_delete(msx_music_instance);
}

// Standalone playback has no storage backend to yield to, so a queued burst is
// applied in full rather than 16 at a time. The snapshot of `head` taken on
// entry still bounds the loop, so a concurrent producer cannot extend it.
static void test_unbounded_drain_when_standalone(void)
{
    reset_opll();
    msx_music_shared_audio = false;
    for (unsigned i = 0; i < 20; i++)
        msx_music_write_ring_push(0x7c, (uint8_t)i);
    msx_music_drain_write_ring();
    assert(fm_writes == 20 && msx_music_write_ring_tail == 20);
    assert(drops == 0);
    OPLL_delete(msx_music_instance);
}

static void test_rings(void)
{
    reset_opll();
    // The bounded drain is a shared-scheduler feature: it exists so Core 1 can
    // yield to the storage backend. Standalone playback drains to the snapshot.
    msx_music_shared_audio = true;
    msx_music_drain_write_ring(); msx_music_drain_psg_write_ring(); assert(locks == 0);
    for (unsigned i = 0; i < 20; i++) {
        msx_music_write_ring_push(0x7c, (uint8_t)i);
        main_psg_write_ring[i] = (uint16_t)(0xa100 | i);
    }
    main_psg_write_ring_head = 20;
    msx_music_drain_write_ring(); msx_music_drain_psg_write_ring();
    assert(fm_writes == 16 && psg_writes == 16 && locks == 2);
    assert(msx_music_write_ring_tail == 16 && main_psg_write_ring_tail == 16);
    msx_music_drain_write_ring(); msx_music_drain_psg_write_ring();
    assert(fm_writes == 20 && psg_writes == 20 && locks == 4);
    for (unsigned i = 0; i < 20; i++) {
        assert(fm_log[i] == (0x7c00 | i) && psg_log[i] == (0xa100 | i));
    }
    msx_music_write_ring_tail = main_psg_write_ring_tail = 250;
    msx_music_write_ring_head = main_psg_write_ring_head = 10;
    for (unsigned i = 0; i < 16; i++) {
        msx_music_write_ring[(250+i)&255] = (uint16_t)(0x7c00 | i);
        main_psg_write_ring[(250+i)&255] = (uint16_t)(0xa100 | i);
    }
    refill_ring = true;
    msx_music_drain_write_ring(); msx_music_drain_psg_write_ring();
    refill_ring = false;
    assert(fm_writes == 36 && psg_writes == 36);
    assert(msx_music_write_ring_tail == 10 && msx_music_write_ring_head == 26);
    assert(main_psg_write_ring_tail == 10);
    msx_music_write_ring_tail = 0; msx_music_write_ring_head = 4;
    refill_ring = true;
    msx_music_drain_write_ring();
    refill_ring = false;
    assert(fm_writes == 40 && msx_music_write_ring_tail == 4 && msx_music_write_ring_head == 8);
    msx_music_ready = false;
    msx_music_drain_write_ring();
    assert(fm_writes == 40 && msx_music_write_ring_tail == 4);
    msx_music_ready = true;
    msx_music_write_ring_head = 255; msx_music_write_ring_tail = 0;
    msx_music_write_ring[255] = 0x1234;
    msx_music_write_ring_push(0x7d, 0xff);
    assert(drops == 1 && msx_music_write_ring_head == 255 && msx_music_write_ring[255] == 0x1234);
    OPLL_delete(msx_music_instance);
}

static void test_slices(void)
{
    msx_music_audio_service_buffer(); assert(services == 0);
    msx_music_audio_started = true;
    msx_music_audio_service_buffer(); assert(services == 0);
    msx_music_audio_pool = &fixture;
    msx_music_audio_service_buffer(); assert(services == 1 && takes == 1 && rendered_samples == 0);
    available = true;
    for (unsigned i = 1; i <= 16; i++) {
        msx_music_audio_service_buffer(); assert(rendered_samples == i*16);
        assert(gives == (i == 16 ? 1u : 0u) && takes == 2);
    }
    msx_music_audio_service_buffer(); assert(rendered_samples == 256 && gives == 1 && takes == 3);
    available = true;
    for (unsigned i = 0; i < 16; i++) msx_music_audio_service_buffer();
    assert(rendered_samples == 512 && gives == 2 && takes == 4);
}

static void reset_backend(void)
{
    memset(&ide, 0, sizeof(ide)); memset(events, 0, sizeof(events)); event_count = 0;
    sd_ide_ctx = usb_ide_ctx = &ide;
    sd_device_mounted = usb_device_mounted = true;
    usb_read_requested = usb_write_requested = false;
    usb_read_in_progress = usb_write_in_progress = false;
    usb_transfer_start_us = 0; now_us = 1;
    sd_lba_base = 100; usb_read_lba = usb_write_lba = 2;
    sd_block_count = usb_block_count = 100; usb_block_size = 512;
    disk_result = RES_OK; ide.state = IDE_STATE_BUSY; ide.status = ATA_STATUS_BSY;
}

static void run_backend(bool usb)
{
    if (setjmp(backend_return) == 0) {
        if (usb) run_usb_loop(); else run_sd_loop();
        assert(false);
    }
}

static void test_backends(void)
{
    for (unsigned usb = 0; usb < 2; usb++) {
        reset_backend(); usb_read_requested = true; run_backend(usb);
        assert(strcmp(events, usb ? "TRA" : "RA") == 0);
        assert(!usb_read_requested && ide.state == IDE_STATE_READ_DATA);
        assert(ide.buffer_length == 512 && (ide.status & ATA_STATUS_DRQ));

        reset_backend(); usb_write_requested = true; run_backend(usb);
        assert(strcmp(events, usb ? "TWA" : "WA") == 0);
        assert(!usb_write_requested && ide.state == IDE_STATE_IDLE && !(ide.status & ATA_STATUS_BSY));

        reset_backend(); ide.usb_identify_pending = true; run_backend(usb);
        assert(strcmp(events, usb ? "TIA" : "IA") == 0);
        assert(ide.state == IDE_STATE_READ_DATA && !ide.usb_identify_pending);

        reset_backend(); usb_read_requested = true; usb_read_lba = 100; run_backend(usb);
        assert(strcmp(events, usb ? "TA" : "A") == 0);
        assert(ide.state == IDE_STATE_IDLE && (ide.status & ATA_STATUS_ERR));

        reset_backend(); sd_ide_ctx = usb_ide_ctx = NULL; run_backend(usb);
        assert(strcmp(events, usb ? "TA" : "A") == 0);
        reset_backend(); run_backend(usb);
        assert(strcmp(events, usb ? "TA" : "A") == 0);
    }
    reset_backend(); usb_read_requested = true; disk_result = 1; run_backend(false);
    assert(strcmp(events, "RA") == 0 && (ide.status & ATA_STATUS_ERR));
    reset_backend(); usb_read_in_progress = true; usb_transfer_start_us = 1;
    now_us = USB_TRANSFER_TIMEOUT_US + 2; run_backend(true);
    assert(strcmp(events, "TA") == 0 && !usb_read_in_progress && (ide.status & ATA_STATUS_ERR));
}

int main(void)
{
    test_native_render(); test_latch_owned_by_one_core();
    test_unbounded_drain_when_standalone();
    test_rings(); test_slices(); test_backends();
    puts("PASS: native FM equivalence/lock ownership, single-core OPLL address latch, bounded FIFO/wrap/full/refill, partial-buffer lifecycle, SD/USB disk-before-audio/read/write/identify/error/idle hooks");
    return 0;
}
