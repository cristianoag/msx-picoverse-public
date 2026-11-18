// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// opl4_main.c - Standalone OPL4 / YMF278B / MoonSound / WozBlaster cartridge
//
// Turns the PicoVerse 2350 cartridge into a dedicated, fully compatible
// OPL4 (YMF278B) sound card with the 2 MB YRW801-M wave ROM and 2 MB of
// PCM sample RAM. This is a pure I/O device: it intercepts the MoonSound
// host ports and emulates the chip through the BSD-licensed ymfm OPL4/PCM
// core. There is no memory-mapped ROM and no mapper.
//
// Host ports (MoonSound-compatible):
//   0xC4 (R/W) - FM register select / status         (WozBlaster WB_STAT)
//   0xC5 (R/W) - FM data
//   0xC6 (R/W) - FM register select #2
//   0xC7 (R/W) - FM data #2
//   0x7E (R/W) - PCM (wavetable) register select
//   0x7F (R/W) - PCM data
//
// Architecture (single-owner ymfm):
//   Core 0 owns the ymfm YMF278B core exclusively. It both
//     (a) services the MSX I/O bus through a high-priority PIO IRQ - the
//         read responder asserts /WAIT so the Z80 is frozen until Core 0
//         supplies the register data, and every host access goes straight
//         into ymfm (or, for the host memory window at wave reg 0x06, into
//         the shared PCM buffers); and
//     (b) generates OPL4 audio samples (OPL4_Clock) into a lock-free
//         single-producer/single-consumer ring buffer in its main loop.
//   The bus IRQ is briefly masked around each small synthesis chunk so the
//   IRQ and the generator never re-enter ymfm concurrently (same core, no
//   spinlock, no cross-core write queue, no register latency).
//   Core 1 only drains the ring buffer into the pico_audio_i2s producer pool
//   and runs the I2S DMA. It never touches ymfm.
//
// Because host accesses are synchronous and /WAIT-gated, the chip is never
// actually "busy" from the host's point of view (ymfm_is_busy() == false), so
// MoonTest's busy poll always exits immediately and the MSX bus never stalls.
//
// The 2 MB YRW801-M instrument ROM is appended to this firmware in the UF2
// image, so it is found at __flash_binary_end. The 2 MB PCM sample RAM is
// allocated from external PSRAM (QMI CS1).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "pico/audio_i2s.h"
#include "loadrom.h"          // shared pin map + PSRAM constants
#include "emu278b.h"
#include "msx_opl4.pio.h"

// -----------------------------------------------------------------------
// OPL4 constants
// -----------------------------------------------------------------------
// The 22.05 kHz build generates one chip sample per two native sample periods
// (OPL4_OUTPUT_STEP=2); ymfm scales its phase, position, envelope and LFO
// increments by the same factor so pitch and tempo are unchanged.
#ifndef OPL4_SAMPLE_RATE
#define OPL4_SAMPLE_RATE          44100
#endif
#ifndef OPL4_OUTPUT_STEP
#define OPL4_OUTPUT_STEP          1
#endif
#define OPL4_AUDIO_BUFFER_SAMPLES 256
#define OPL4_SAMPLE_RAM_SIZE      (2u * 1024u * 1024u)   // 2 MB PCM sample RAM
#define OPL4_YRW801_SIZE          (2u * 1024u * 1024u)   // 2 MB YRW801-M wave ROM
#define OPL4_MEMORY_MASK          0x3FFFFFu      // 22-bit / 4 MB address space

// Mirrors the ymfm build options; reported in the debug line so a field capture
// always states which clocking model produced it.
#ifndef OPL4_SKIP_INACTIVE_FM
#define OPL4_SKIP_INACTIVE_FM 1
#endif
#ifndef OPL4_SKIP_INACTIVE_PCM
#define OPL4_SKIP_INACTIVE_PCM 1
#endif

// The PicoVerse 2350 carries 8 MB of external PSRAM. By default the 2 MB
// YRW801-M wave ROM is read directly from the appended flash image, while the
// 2 MB PCM sample RAM lives in PSRAM. Setting OPL4_YRW801_IN_PSRAM to 1 copies
// the wave ROM into PSRAM at boot for A/B timing experiments, so the entire
// 4 MB OPL4 PCM map lives in one contiguous PSRAM region (single QMI chip
// select CS1):
//     PSRAM[0 .. 2 MB)   = YRW801-M wave ROM copy (read-only to the host)
//     PSRAM[2 MB .. 4 MB)= PCM sample RAM
// ymfm then reads wave headers/samples and sample-RAM bytes from the same
// device, avoiding flash(CS0) <-> PSRAM(CS1) chip-select thrashing on every
// interleaved access. Hardware A/B testing (after the write capture/apply fix)
// showed this gives no measurable synthesis-throughput benefit - the heavy-load
// bottleneck is ymfm's per-sample math, not memory latency - while it adds a
// 2 MB boot-time copy that delays cartridge readiness. It is therefore kept
// disabled; the flash-backed wave ROM is the default.
#ifndef OPL4_YRW801_IN_PSRAM
#define OPL4_YRW801_IN_PSRAM      0
#endif

#define OPL4_PSRAM_YRW801_OFFSET  0u
#define OPL4_PSRAM_SAMPLE_OFFSET  OPL4_YRW801_SIZE

// MoonSound host ports
#define OPL4_FM_PORT_REG          0xC4u
#define OPL4_FM_PORT_DATA         0xC5u
#define OPL4_FM2_PORT_REG         0xC6u
#define OPL4_FM2_PORT_DATA        0xC7u
#define OPL4_PCM_PORT_REG         0x7Eu
#define OPL4_PCM_PORT_DATA        0x7Fu

// MSX maskable interrupt line (/INT), wired to GPIO40 on the PicoVerse 2350.
// /INT is an active-low, open-collector line shared by all cartridges, so it is
// emulated open-drain: asserted = drive low, released = hi-Z (the MSX pulls it
// high). MoonSound replayers (e.g. MoonBlaster) pace song tempo from the
// YMF278B FM-timer interrupt delivered on this line.
#define PIN_INT                   40

// The loadrom tool writes this 16-byte config header between the firmware and
// the appended YRW801-M ROM; the firmware reads it at __flash_binary_end and
// skips past it. Byte 4 is a reserved flags byte (currently always 0).
#define OPL4_CFG_HEADER_SIZE       16u
#define OPL4_CFG_MAGIC0            'P'
#define OPL4_CFG_MAGIC1            'V'
#define OPL4_CFG_MAGIC2            'O'
#define OPL4_CFG_MAGIC3            '4'

// RP2350 system clock for the OPL4 firmware (see main()).
#define OPL4_SYS_CLOCK_KHZ         300000u

// The YRW801-M wave ROM is appended right after the firmware binary in flash.
// __flash_binary_end is declared as a single byte in loadrom.h; the 2 MB ROM
// follows it. Take the address through a volatile pointer so the compiler does
// not treat the copy below as a fixed-size-object overread.
extern uint8_t __flash_binary_end;
static uint8_t *const volatile yrw801_flash_src = &__flash_binary_end;
static const uint8_t *yrw801_flash = NULL;

// Active wave-ROM pointer used by both ymfm (via OPL4_Reset) and the host
// memory window. Points at the PSRAM copy when OPL4_YRW801_IN_PSRAM, otherwise
// straight at the flash image. Resolved in opl4_init().
static const uint8_t *yrw801_rom = NULL;
static bool opl4_config_loaded = false;

// -----------------------------------------------------------------------
// OPL4 core state
//
// ymfm is shared between Core 1 (synthesis) and Core 0 (bus servicing) under a
// short-held spinlock. The full YMF278B (OPL3 FM + 24 PCM channels) needs ~one
// whole 210 MHz core to run at 44.1 kHz, so synthesis gets its own core (Core
// 1) and the bus IRQ lives on Core 0. Every ymfm access is wrapped in the lock
// for the minimum time (one register access on Core 0, one sample frame on
// Core 1). Because this is a standalone cartridge that owns /WAIT, the bus IRQ
// may block on the lock - the Z80 is frozen by /WAIT meanwhile, so a wait of at
// most one synthesised frame (~22 us) is harmless.
// -----------------------------------------------------------------------
static opl4_t           opl4_instance;
static spin_lock_t     *opl4_lock = NULL;
static volatile bool    opl4_ready = false;

// PCM sample RAM region (2 MB) in external PSRAM.
static uint8_t         *opl4_sample_ram = NULL;

static inline volatile uint8_t *__not_in_flash_func(opl4_sample_ram_volatile)(void)
{
    return (volatile uint8_t *)opl4_sample_ram;
}

// Lock-free helpers around a single ymfm access. The lock is held only for the
// duration of one register read/write so neither core stalls the other for
// long. Memory-window (wave reg 0x06) accesses do NOT take the lock - they hit
// the shared PCM buffers directly and never touch ymfm.
//
// opl4_batch_locked lets the write-apply loop hold the lock ONCE across a run
// of consecutive ymfm register writes instead of acquiring it per write: when
// set, opl4_ymfm_write_locked() writes ymfm directly (the caller already holds
// the lock). This collapses the per-write spinlock contention that, under a
// heavy voice-update burst, let the 8-deep PIO write FIFO overflow and drop
// MSX writes (missing notes).
static volatile bool opl4_batch_locked = false;

static inline void __not_in_flash_func(opl4_capture_write_fifo)(void);

// Core 1 holds opl4_lock for the whole of one generated sample (~15 us of a
// 22.7 us period), so a plain spin_lock_blocking() here leaves Core 0 stalled
// long enough for the 8-deep PIO write FIFO to fill and silently drop MSX
// writes. Keep draining the FIFO into the software ring - which is lock-free -
// while waiting for the lock.
static inline uint32_t __not_in_flash_func(opl4_lock_draining_fifo)(void)
{
    uint32_t save = save_and_disable_interrupts();
    while (!spin_try_lock_unsafe(opl4_lock))
        opl4_capture_write_fifo();
    return save;
}

static inline void __not_in_flash_func(opl4_ymfm_write_locked)(uint32_t offset, uint8_t data)
{
    if (opl4_batch_locked)
    {
        OPL4_Write(&opl4_instance, offset, data);
        return;
    }
    uint32_t save = opl4_lock_draining_fifo();
    OPL4_Write(&opl4_instance, offset, data);
    spin_unlock(opl4_lock, save);
}

static inline uint8_t __not_in_flash_func(opl4_ymfm_read_locked)(uint32_t offset)
{
    uint32_t save = spin_lock_blocking(opl4_lock);
    uint8_t data = OPL4_Read(&opl4_instance, offset);
    spin_unlock(opl4_lock, save);
    return data;
}

// MoonSound wave-register host interface (ports 0x7E address / 0x7F data).
// The host memory window (wave reg 0x06 while reg 0x02 bit0 = 1) drives a
// 22-bit access pointer over the 4 MB PCM map (0-2 MB = YRW801 wave ROM,
// 2-4 MB = PSRAM sample RAM). MoonTest streams millions of byte accesses
// through this window during its RAM test, each followed by a status poll, so
// the window is handled directly against the very buffers ymfm samples from -
// no ymfm round-trip, no busy flag, no lock. All other wave registers go
// through ymfm so wavetable playback, mixing and the device-ID readback are
// accurate.
static volatile uint8_t  opl4_wave_reg      = 0;   // wave register pointer (port 0x7E)
static volatile uint8_t  opl4_wave_reg02    = 0;   // reg 0x02 shadow (memory-access mode bit0)
static volatile uint8_t  opl4_wave_addr_hi  = 0;   // reg 0x03 raw (addr bits 21-16, masked 0x3F)
static volatile uint8_t  opl4_wave_addr_mid = 0;   // reg 0x04 raw (addr bits 15-8)
static volatile uint8_t  opl4_wave_addr_lo  = 0;   // reg 0x05 raw (addr bits 7-0)
static volatile uint32_t opl4_mem_addr      = 0;   // live 22-bit memory-window pointer

// FM data-port readback shadows for tools (e.g. MBWAVE) that probe by reading
// back recently written FM register/data ports.
static volatile uint8_t opl4_fm_reg_primary   = 0;
static volatile uint8_t opl4_fm_reg_secondary = 0;
static uint8_t opl4_fm_regs_primary[256];
static uint8_t opl4_fm_regs_secondary[256];

// Captured-write ring. Core 0 drains the 8-deep PIO write FIFO into this large
// software ring in one fast, lock-free pass (capture), then applies the entries
// to ymfm/PSRAM (apply). Decoupling capture from application keeps the PIO FIFO
// from ever overflowing during a heavy voice-update burst (which was dropping
// MSX writes and causing missing notes), while the apply step batches the ymfm
// spinlock across a run of register writes. Single producer + consumer, both on
// Core 0, so head/tail need no locking.
#define OPL4_WR_RING_SIZE 4096u
#define OPL4_WR_RING_MASK (OPL4_WR_RING_SIZE - 1u)
static uint8_t opl4_wr_ring_port[OPL4_WR_RING_SIZE];
static uint8_t opl4_wr_ring_data[OPL4_WR_RING_SIZE];
static volatile uint32_t opl4_wr_head = 0;
static volatile uint32_t opl4_wr_tail = 0;

// -----------------------------------------------------------------------
// I/O bus PIO context
// -----------------------------------------------------------------------
#define IO_PIO_READ   pio1
#define IO_SM_READ    0
#define IO_PIO_WRITE  pio0
#define IO_SM_WRITE   1
#define IO_SM_I2S     0

// Dedicated /BUSDIR driver SM. /BUSDIR is GPIO 37, outside the 32-pin window
// (GPIO base 0) the read/write SMs use for A0..A7, so it runs on its own PIO
// instance (PIO2) with GPIO base 16 (pins 16-47).
#define IO_PIO_BUSDIR pio2
#define IO_SM_BUSDIR  0

// Idle spins the read IRQ waits for (with /WR high and the write ring empty)
// before it answers a read. This exists only to catch a write that is still in
// flight when the read arrives; the Z80 is frozen by our own /WAIT for the
// whole read, so no NEW write can start and the window to cover is just the
// tail of one MSX /WR pulse (~0.6 us) plus the PIO capture delay. Each spin
// costs ~0.29 us at 300 MHz, so 32 spins is ~9 us of margin - a 15x safety
// factor. The original 512 spins held /WAIT for ~150 us on EVERY read, which
// measured up to 23% of the Z80's time inside read bursts (rd/s 1500) and
// starved MSX replayers enough to delay note-offs.
#ifndef OPL4_READ_SYNC_IDLE_SPINS
#define OPL4_READ_SYNC_IDLE_SPINS 32u
#endif

// -----------------------------------------------------------------------
// I2S audio state
// -----------------------------------------------------------------------
static struct audio_buffer_pool *audio_pool = NULL;
static int   audio_dma_channel = -1;

// -----------------------------------------------------------------------
// Optional USB CDC diagnostics (off by default; toggled in CMakeLists.txt)
//
// The per-second summary reports the health of the two-core split:
//   gen/s     - frames synthesised by Core 1 per second. Should hold ~44100;
//               if it sags, Core 1 cannot keep real time.
//   late/s    - audio buffers Core 1 finished later than their playback
//               duration (the audible underrun / starvation metric).
//   genMax    - longest single buffer-fill time on Core 1 (us).
//   c1WaitMax - longest time Core 1 waited for the ymfm lock (us), i.e. bus
//               activity stealing synthesis time.
//   rd/s,wr/s - host I/O reads/writes serviced by Core 0 per second.
//   rdMax     - longest single read-IRQ service time on Core 0 (us), including
//               the write-drain spin, lock wait and the ymfm read.
// -----------------------------------------------------------------------
#if OPL4_USB_DEBUG
static volatile uint32_t dbg_gen_frames    = 0;   // Core 1: frames synthesised
static volatile uint32_t dbg_gen_max_us    = 0;   // Core 1: max buffer-fill us
static volatile uint32_t dbg_c1_wait_max   = 0;   // Core 1: max lock-wait us
static volatile uint32_t dbg_read_count    = 0;   // Core 0 IRQ: reads serviced
static volatile uint32_t dbg_read_max_us   = 0;   // Core 0 IRQ: max read service us
static volatile uint32_t dbg_write_count   = 0;   // Core 0 IRQ: writes serviced
static volatile uint32_t dbg_late_buffers  = 0;   // Core 1: buffers finished late
static volatile uint8_t  dbg_last_status   = 0;   // Core 0: last 0xC4 byte returned

// FM timer / IRQ activity, published by the ymfm interface (emu278b.cpp). These
// tell us whether the host arms the FM timers, how fast they fire, and whether
// a timer IRQ would have been raised (the cartridge has no /INT line, so the
// MSX can only see these via status-flag polling).
extern volatile uint32_t opl4_dbg_timer_arm;
extern volatile uint32_t opl4_dbg_timer_fire;
extern volatile uint32_t opl4_dbg_irq_assert;

// Output-level diagnostics published by OPL4_Clock (emu278b.cpp): per-interval
// soft-limiter engagement count and the peak magnitude of each YMF278B output
// pin pair plus the final post-gain value. Reset each report so they read
// peak-since-last-dump.
extern volatile uint32_t opl4_dbg_clip;
extern volatile uint32_t opl4_dbg_peak_raw;
extern volatile uint32_t opl4_dbg_peak_do0;
extern volatile uint32_t opl4_dbg_peak_do1;
extern volatile uint32_t opl4_dbg_peak_do2;

// True saturation, sampled inside ymf278b::generate BEFORE output->clamp16().
// opl4_dbg_clip above counts limiter engagement downstream of that clamp;
// do2Clip/pkPre are the metrics that show audible hard clipping inside the chip
// model itself.
extern volatile uint32_t opl4_dbg_do2_clip;
extern volatile uint32_t opl4_dbg_do2_peak;

// Rendered polyphony, published by the ymfm FM/PCM engines. Reported as
// pcmAct=max/distinct and fmAct=max/distinct so a "song sounds wrong" report can
// be checked against how many voices actually reached the synthesiser.
extern volatile uint32_t opl4_dbg_fm_active_or;
extern volatile uint32_t opl4_dbg_fm_active_max;
extern volatile uint32_t opl4_dbg_pcm_active_or;
extern volatile uint32_t opl4_dbg_pcm_active_max;
extern volatile uint32_t opl4_dbg_pcm_key_or;
extern volatile uint32_t opl4_dbg_pcm_keyoff_or;
extern volatile uint32_t opl4_dbg_pcm_rel_or;
extern volatile uint32_t opl4_dbg_pcm_rev_or;

// Per-sample lock-wait timing on Core 1 costs two timer reads per generated
// sample, which inflates genMax by ~1% of the buffer budget - the same order as
// the deficit we are trying to measure. Hardware runs have consistently shown
// c1Wait <= 5 us, so leave it off unless contention is being investigated.
#ifndef OPL4_DEBUG_LOCK_TIMING
#define OPL4_DEBUG_LOCK_TIMING 0
#endif

// OPL3 FM usage diagnostics, recorded by opl4_bus_write. They reveal how a
// pure-OPL3 game (e.g. Bombaman) actually drives the FM part, to tell apart the
// candidate causes of "FM sounds off / missing parts":
//   new   - last value written to FM reg 0x105 (bit0 = NEW/OPL3 enable,
//           bit1 = NEW2/wave enable). OPL3 features need NEW=1.
//   r104  - last value written to FM reg 0x104 (4-operator connection mask).
//           Non-zero => the song uses 4-op instruments.
//   cxOr  - OR of every channel-output nibble written to regs 0xC0-0xC8 /
//           0x1C0-0x1C8 (bits: 0x1=CHA L, 0x2=CHB R, 0x4=CHC auxL, 0x8=CHD
//           auxR after >>4). If bits 0x4/0x8 appear, channels route to the
//           aux outputs, which ymfm mixes at the WRONG (full) level.
//   cxLast- last channel-output byte written.
//   fmW/s - FM *data* writes per second (ports 0xC5/0xC7). Tells whether the
//           FM part is being driven at all vs the workload being pure PCM.
//   key/s - FM key-on writes per second (regs 0xB0-0xB8 with bit5 set).
//   f8/f9 - last FM mix level (reg 0xF8, via wave port) and wave mix level
//           (reg 0xF9). reg 0xF8 sets how loud FM is relative to PCM.
static volatile uint8_t  dbg_fm_new    = 0;
static volatile uint8_t  dbg_fm_r104   = 0;
static volatile uint8_t  dbg_fm_cx_or  = 0;
static volatile uint8_t  dbg_fm_cx_last = 0;
static volatile uint32_t dbg_fm_data_writes = 0;   // free-running, ports 0xC5/0xC7
static volatile uint32_t dbg_fm_keyon = 0;         // free-running, 0xB0-0xB8 key-on
static volatile uint8_t  dbg_fm_f8    = 0x1B;      // reg 0xF8 FM mix level (reset default)
static volatile uint8_t  dbg_fm_f9    = 0;         // reg 0xF9 wave mix level

// Write-path overflow diagnostics. The OPL4 write captor (PIO0 SM1) has an
// 8-deep RX FIFO and uses a blocking push, and the write path is NOT /WAIT
// gated. If the MSX replayer fires a burst of OUT writes faster than Core 0 can
// drain them (each write takes the ymfm spinlock, contended with Core 1
// synthesis), the FIFO fills, the SM stalls on push, and the next bus write is
// DROPPED. dbg_w_full counts IRQ entries that found the FIFO already full;
// dbg_w_rxstall counts seconds in which the PIO RXSTALL flag fired (a hardware
// overflow actually happened = at least one write was lost).
static volatile uint32_t dbg_w_full    = 0;        // free-running, FIFO full on IRQ entry
static volatile uint32_t dbg_w_rxstall = 0;        // incremented in dump when RXSTALL set
static volatile uint32_t dbg_w_ringdrop = 0;       // free-running, software ring overflow

#define DBG_INC(v)        ((v)++)
#define DBG_ADD(v, n)     ((v) += (uint32_t)(n))
#define DBG_MAX(v, n)     do { uint32_t _n = (uint32_t)(n); if (_n > (v)) (v) = _n; } while (0)

// Record an FM register write (offset/reg/data already decoded). reg is the
// 8-bit register index within its bank; bank1 (0x100+) writes arrive on
// offset 3, bank0 on offset 1.
static inline void opl4_dbg_fm_write(uint8_t offset, uint8_t reg, uint8_t data)
{
    dbg_fm_data_writes++;
    if (offset == 3u && reg == 0x05u)
        dbg_fm_new = data;
    else if (offset == 3u && reg == 0x04u)
        dbg_fm_r104 = data;
    else if (reg >= 0xB0u && reg <= 0xB8u && (data & 0x20u))
        dbg_fm_keyon++;                       // key-on bit set on a channel
    else if ((offset == 1u || offset == 3u) && reg >= 0xC0u && reg <= 0xC8u)
    {
        dbg_fm_cx_last = data;
        dbg_fm_cx_or |= (uint8_t)(data >> 4);   // CHA/CHB/CHC/CHD nibble
    }
}
#define DBG_FM_WRITE(off, reg, data) opl4_dbg_fm_write((off), (reg), (data))

// Record a wave-register write (used to capture the FM/wave mix levels, which
// are reg 0xF8/0xF9 accessed through the wave port).
static inline void opl4_dbg_wave_write(uint8_t reg, uint8_t data)
{
    if (reg == 0xF8u)
        dbg_fm_f8 = data;
    else if (reg == 0xF9u)
        dbg_fm_f9 = data;
}
#define DBG_WAVE_WRITE(reg, data) opl4_dbg_wave_write((reg), (data))

static void opl4_debug_usb_boot_wait(void)
{
    absolute_time_t deadline = make_timeout_time_ms(5000);
    uint32_t next_print_ms = 0;
    while (!time_reached(deadline))
    {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now_ms - next_print_ms) >= 0)
        {
            next_print_ms = now_ms + 250u;
            printf("OPL4DBG usb-boot %lu\r\n", (unsigned long)now_ms);
        }
        sleep_ms(10);
    }
}

static void opl4_debug_dump(void)
{
    static uint32_t last_us = 0;
    static uint32_t last_gen = 0, last_rd = 0, last_wr = 0, last_late = 0;
    static uint32_t last_tarm = 0, last_tfire = 0, last_tirq = 0;
    static uint32_t last_fmw = 0, last_key = 0;
    static uint32_t last_ringdrop = 0;

    uint32_t now_us = time_us_32();
    uint32_t elapsed = now_us - last_us;
    if (elapsed < 1000000u)
        return;
    last_us = now_us;

    // Snapshot and delta the free-running counters.
    uint32_t gen  = dbg_gen_frames;
    uint32_t rd   = dbg_read_count;
    uint32_t wr   = dbg_write_count;
    uint32_t late = dbg_late_buffers;

    uint32_t gen_d  = gen - last_gen;   last_gen = gen;
    uint32_t rd_d   = rd - last_rd;     last_rd = rd;
    uint32_t wr_d   = wr - last_wr;     last_wr = wr;
    uint32_t late_d = late - last_late; last_late = late;

    // Snapshot and clear the per-interval timing maxima.
    uint32_t gen_max = dbg_gen_max_us;  dbg_gen_max_us = 0;
    uint32_t c1_wait = dbg_c1_wait_max; dbg_c1_wait_max = 0;
    uint32_t rd_max  = dbg_read_max_us; dbg_read_max_us = 0;

    // FM timer / IRQ activity deltas.
    uint32_t tarm  = opl4_dbg_timer_arm;
    uint32_t tfire = opl4_dbg_timer_fire;
    uint32_t tirq  = opl4_dbg_irq_assert;
    uint32_t tarm_d  = tarm - last_tarm;   last_tarm = tarm;
    uint32_t tfire_d = tfire - last_tfire; last_tfire = tfire;
    uint32_t tirq_d  = tirq - last_tirq;   last_tirq = tirq;

    // FM write / key-on rate deltas.
    uint32_t fmw = dbg_fm_data_writes;
    uint32_t key = dbg_fm_keyon;
    uint32_t fmw_d = fmw - last_fmw;  last_fmw = fmw;
    uint32_t key_d = key - last_key;  last_key = key;

    // Write-FIFO overflow: sample the hardware RXSTALL sticky flag for the
    // write SM, then clear it. A non-zero wStall means the captor overflowed
    // (dropped at least one MSX write) since the last report.
    uint32_t rxstall_bit = (1u << (PIO_FDEBUG_RXSTALL_LSB + IO_SM_WRITE));
    uint32_t w_stall = (IO_PIO_WRITE->fdebug & rxstall_bit) ? 1u : 0u;
    IO_PIO_WRITE->fdebug = rxstall_bit;       // write-1-to-clear
    dbg_w_rxstall += w_stall;
    uint32_t wfull = dbg_w_full;
    static uint32_t last_wfull = 0;
    uint32_t wfull_d = wfull - last_wfull; last_wfull = wfull;
    uint32_t ringdrop = dbg_w_ringdrop;
    uint32_t ringdrop_d = ringdrop - last_ringdrop; last_ringdrop = ringdrop;

    // Output-level peaks (reset each interval).
    uint32_t clip   = opl4_dbg_clip;     opl4_dbg_clip = 0;
    uint32_t praw   = opl4_dbg_peak_raw; opl4_dbg_peak_raw = 0;
    uint32_t pdo0   = opl4_dbg_peak_do0; opl4_dbg_peak_do0 = 0;
    uint32_t pdo1   = opl4_dbg_peak_do1; opl4_dbg_peak_do1 = 0;
    uint32_t pdo2   = opl4_dbg_peak_do2; opl4_dbg_peak_do2 = 0;
    uint32_t d2clip = opl4_dbg_do2_clip; opl4_dbg_do2_clip = 0;
    uint32_t d2peak = opl4_dbg_do2_peak; opl4_dbg_do2_peak = 0;

    uint32_t fmAct   = opl4_dbg_fm_active_max;  opl4_dbg_fm_active_max = 0;
    uint32_t fmAny   = opl4_dbg_fm_active_or;   opl4_dbg_fm_active_or = 0;
    uint32_t pcmAct  = opl4_dbg_pcm_active_max; opl4_dbg_pcm_active_max = 0;
    uint32_t pcmAny  = opl4_dbg_pcm_active_or;  opl4_dbg_pcm_active_or = 0;
    uint32_t pcmKey  = opl4_dbg_pcm_key_or;     opl4_dbg_pcm_key_or = 0;
    uint32_t pcmOff  = opl4_dbg_pcm_keyoff_or;  opl4_dbg_pcm_keyoff_or = 0;
    uint32_t pcmRel  = opl4_dbg_pcm_rel_or;     opl4_dbg_pcm_rel_or = 0;
    uint32_t pcmRev  = opl4_dbg_pcm_rev_or;     opl4_dbg_pcm_rev_or = 0;

    printf("OPL4DBG gen/s=%lu late/s=%lu genMax=%luus c1Wait=%luus rd/s=%lu rdMax=%luus wr/s=%lu fmW/s=%lu key/s=%lu tArm/s=%lu tFire/s=%lu irq/s=%lu wFull/s=%lu wStall=%lu ringDrop/s=%lu lim/s=%lu pkRaw=%lu pkDO2=%lu do2Clip/s=%lu pkPre=%lu pcmAct=%lu pcmOn=%06lX pcmKey=%06lX pcmOff=%06lX pcmRel=%06lX pcmRev=%06lX fmAct=%lu fmOn=%05lX f8=%02X f9=%02X wave=%02X r02=%02X wait=%u skip=%u/%u spin=%u rate=%lu clk=%lu\r\n",
           (unsigned long)gen_d,
           (unsigned long)late_d,
           (unsigned long)gen_max,
           (unsigned long)c1_wait,
           (unsigned long)rd_d,
            (unsigned long)rd_max,
           (unsigned long)wr_d,
           (unsigned long)fmw_d,
           (unsigned long)key_d,
            (unsigned long)tarm_d,
            (unsigned long)tfire_d,
            (unsigned long)tirq_d,
            (unsigned long)wfull_d,
           (unsigned long)w_stall,
            (unsigned long)ringdrop_d,
           (unsigned long)clip,
           (unsigned long)praw,
           (unsigned long)pdo2,
           (unsigned long)d2clip,
           (unsigned long)d2peak,
           (unsigned long)pcmAct,
           (unsigned long)pcmAny,
           (unsigned long)pcmKey,
           (unsigned long)pcmOff,
           (unsigned long)pcmRel,
           (unsigned long)pcmRev,
           (unsigned long)fmAct,
           (unsigned long)fmAny,
           dbg_fm_f8, dbg_fm_f9,
           opl4_wave_reg, opl4_wave_reg02,
           gpio_get(PIN_WAIT),
            (unsigned)OPL4_SKIP_INACTIVE_FM,
            (unsigned)OPL4_SKIP_INACTIVE_PCM,
            (unsigned)OPL4_READ_SYNC_IDLE_SPINS,
            (unsigned long)OPL4_SAMPLE_RATE,
            (unsigned long)(OPL4_SYS_CLOCK_KHZ / 1000u));
}
#else
#define DBG_INC(v)        ((void)0)
#define DBG_ADD(v, n)     ((void)0)
#define DBG_MAX(v, n)     ((void)0)
#define DBG_FM_WRITE(off, reg, data) ((void)0)
#define DBG_WAVE_WRITE(reg, data) ((void)0)
#define opl4_debug_usb_boot_wait() ((void)0)
#define opl4_debug_dump()          ((void)0)
#endif

static inline int16_t clamp_i16(int32_t sample)
{
    if (sample > 32767)  return 32767;
    if (sample < -32768) return -32768;
    return (int16_t)sample;
}

// -----------------------------------------------------------------------
// External PSRAM init (QMI CS1) - verbatim from the loadrom firmware
// -----------------------------------------------------------------------
static inline void __not_in_flash_func(psram_delay_cycles)(uint32_t cycles)
{
    for (volatile uint32_t cycle = 0; cycle < cycles; ++cycle)
        __asm volatile ("nop");
}

static inline void __not_in_flash_func(psram_wait_direct_done)(void)
{
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {}
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}
}

static inline void __not_in_flash_func(psram_send_direct_cmd)(uint8_t cmd, bool quad_width)
{
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    if (quad_width)
        qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                            (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                            cmd;
    else
        qmi_hw->direct_tx = cmd;
    psram_wait_direct_done();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    (void)qmi_hw->direct_rx;
}

static bool __no_inline_not_in_flash_func(psram_init)(void)
{
    gpio_set_function(PIN_PSRAM, GPIO_FUNC_XIP_CS1);

    uint32_t irq_state = save_and_disable_interrupts();

    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}

    // Warm restart path: if PSRAM remained powered it may still be in QPI mode.
    psram_send_direct_cmd(0xF5u, true);
    psram_delay_cycles(128u);

    psram_send_direct_cmd(0x66u, false);
    psram_delay_cycles(128u);
    psram_send_direct_cmd(0x99u, false);
    psram_delay_cycles(50000u);
    psram_send_direct_cmd(0x35u, false);
    psram_delay_cycles(128u);
    psram_send_direct_cmd(0xC0u, false);
    psram_delay_cycles(128u);

    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    // PSRAM QMI timing - tuned for a 300 MHz system clock (the OPL4 firmware
    // overclocks the RP2350 to 300 MHz so ymfm has the headroom to synthesise
    // the full YMF278B in real time). The clock divider is 3, keeping the PSRAM
    // device clock at 300/3 = 100 MHz (essentially the same ~105 MHz it ran at
    // when the firmware used 210 MHz with divider 2, so the device-domain timing
    // is preserved). The sysclk-domain delay fields are scaled up by ~1.43x from
    // the 210 MHz values so the absolute (nanosecond) timing is unchanged:
    //   RXDELAY 2->3, MIN_DESELECT 5->7, MAX_SELECT 26->37 (still < APS6404 8 us
    //   tCEM: 38*64/300MHz = 8.1 us window cap is respected at 37).
    // If PSRAM reads return corrupt data at 300 MHz (garbage/looping audio or a
    // hang during the boot YRW801 copy), RXDELAY is the field to retune first.
    qmi_hw->m[1].timing =
        (QMI_M0_TIMING_PAGEBREAK_VALUE_1024 << QMI_M0_TIMING_PAGEBREAK_LSB) |
        (1u << QMI_M0_TIMING_SELECT_HOLD_LSB) |
        (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
        (3u << QMI_M0_TIMING_RXDELAY_LSB) |
        (37u << QMI_M0_TIMING_MAX_SELECT_LSB) |
        (7u << QMI_M0_TIMING_MIN_DESELECT_LSB) |
        (3u << QMI_M0_TIMING_CLKDIV_LSB);
    qmi_hw->m[1].rfmt =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_LEN_VALUE_24 << QMI_M0_RFMT_DUMMY_LEN_LSB) |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB) |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB) |
        (QMI_M0_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_RFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = (0xEBu << QMI_M0_RCMD_PREFIX_LSB);
    qmi_hw->m[1].wfmt =
        (QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M0_WFMT_DUMMY_LEN_LSB) |
        (QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB) |
        (QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB) |
        (QMI_M0_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_WFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = (0x38u << QMI_M0_WCMD_PREFIX_LSB);

    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

    restore_interrupts(irq_state);

    // Verify PSRAM is functional via the uncached window.
    volatile uint32_t *psram_test = (volatile uint32_t *)0x15000000u;
    psram_test[0] = 0x12345678u;
    return psram_test[0] == 0x12345678u;
}

// -----------------------------------------------------------------------
// GPIO setup - bus pins
// -----------------------------------------------------------------------
static inline void setup_gpio(void)
{
    for (uint pin = PIN_A0; pin <= PIN_A15; ++pin)
    {
        gpio_init(pin);
        gpio_set_input_hysteresis_enabled(pin, true);
        gpio_set_dir(pin, GPIO_IN);
    }

    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
    {
        gpio_init(pin);
        gpio_set_input_hysteresis_enabled(pin, true);
        gpio_set_dir(pin, GPIO_IN);
    }

    gpio_init(PIN_RD);      gpio_set_dir(PIN_RD, GPIO_IN);
    gpio_init(PIN_WR);      gpio_set_dir(PIN_WR, GPIO_IN);
    gpio_init(PIN_IORQ);    gpio_set_dir(PIN_IORQ, GPIO_IN);
    gpio_init(PIN_SLTSL);   gpio_set_dir(PIN_SLTSL, GPIO_IN);
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN);

    // /WAIT - assert LOW during boot to freeze the Z80 until the OPL4 bus
    // responder is live (released by io_bus_init when the PIO takes over the
    // pin). This guarantees the cartridge is ready before the MSX runs any
    // code, so a game in another cartridge slot cannot start - and begin its
    // MoonSound music - before this cartridge can answer. /WAIT is open-drain
    // (driven low = asserted), so it coexists with another cartridge.
    // IMPORTANT: the held window must stay well under the Z80 DRAM-refresh
    // limit (~2 ms). The init between here and io_bus_init (PSRAM bring-up +
    // ymfm reset) is sub-millisecond; do NOT add long work (e.g. a multi-MB
    // memcpy) to that path while /WAIT is held.
    gpio_init(PIN_WAIT);
    gpio_put(PIN_WAIT, 0);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);   // drive low = assert
}

// -----------------------------------------------------------------------
// OPL4 register port mapping
// -----------------------------------------------------------------------
static inline uint8_t opl4_port_to_offset(uint8_t port)
{
    switch (port)
    {
    case OPL4_FM_PORT_REG:   return 0u;
    case OPL4_FM_PORT_DATA:  return 1u;
    case OPL4_FM2_PORT_REG:  return 2u;
    case OPL4_FM2_PORT_DATA: return 3u;
    case OPL4_PCM_PORT_REG:  return 4u;
    case OPL4_PCM_PORT_DATA: return 5u;
    default:                 return 0xFFu;
    }
}

// Build the 16-bit response token for the I/O read PIO.
//   bits[7:0]  = data byte
//   bits[15:8] = pindirs (0xFF = drive, 0x00 = float)
static inline uint16_t __not_in_flash_func(build_token)(bool drive, uint8_t data)
{
    return drive ? ((uint16_t)0xFF00u | data) : (uint16_t)data;
}

// -----------------------------------------------------------------------
// Host memory window (wave reg 0x06): direct, lock-free buffer access with
// 22-bit auto-increment. 0-2 MB = YRW801 wave ROM, 2-4 MB = PSRAM sample RAM.
// Matches the YMF278B verified behaviour: only a write to reg 0x05 commits the
// full address; reg 0x03/0x04 only stage their bytes; the pointer wraps at
// 4 MB; reg-0x03 readback masks the upper 2 bits.
// -----------------------------------------------------------------------
static inline bool __not_in_flash_func(opl4_mem_access_active)(void)
{
    return (opl4_wave_reg02 & 0x01u) != 0;
}

static inline bool __not_in_flash_func(opl4_is_memory_data_port)(uint8_t port)
{
    return port == OPL4_PCM_PORT_DATA && opl4_wave_reg == 0x06u && opl4_mem_access_active();
}

static inline uint8_t __not_in_flash_func(opl4_mem_read)(void)
{
    uint32_t addr = opl4_mem_addr & OPL4_MEMORY_MASK;
    uint8_t data = 0xFFu;
    if (addr < OPL4_YRW801_SIZE)
        data = yrw801_rom[addr];
    else
    {
        uint32_t ram = addr - OPL4_YRW801_SIZE;
        if (opl4_sample_ram && ram < OPL4_SAMPLE_RAM_SIZE)
            data = opl4_sample_ram_volatile()[ram];
    }
    opl4_mem_addr = (addr + 1u) & OPL4_MEMORY_MASK;
    return data;
}

static inline void __not_in_flash_func(opl4_mem_write)(uint8_t data)
{
    uint32_t addr = opl4_mem_addr & OPL4_MEMORY_MASK;
    if (addr >= OPL4_YRW801_SIZE)
    {
        uint32_t ram = addr - OPL4_YRW801_SIZE;
        if (opl4_sample_ram && ram < OPL4_SAMPLE_RAM_SIZE)
            opl4_sample_ram_volatile()[ram] = data;
    }
    opl4_mem_addr = (addr + 1u) & OPL4_MEMORY_MASK;
}

static inline void __not_in_flash_func(opl4_host_registers_reset)(void)
{
    memset(opl4_fm_regs_primary, 0, sizeof(opl4_fm_regs_primary));
    memset(opl4_fm_regs_secondary, 0, sizeof(opl4_fm_regs_secondary));
    opl4_wave_reg      = 0;
    opl4_wave_reg02    = 0;
    opl4_wave_addr_hi  = 0;
    opl4_wave_addr_mid = 0;
    opl4_wave_addr_lo  = 0;
    opl4_mem_addr      = 0;
    opl4_fm_reg_primary   = 0;
    opl4_fm_reg_secondary = 0;
}

// -----------------------------------------------------------------------
// Bus write dispatch (Core 0 IRQ context). FM and wave-register writes go into
// ymfm under the shared lock; only the host memory window (reg 0x06 in
// memory-access mode) bypasses ymfm and hits the shared PCM buffers directly.
// -----------------------------------------------------------------------
static inline void __not_in_flash_func(opl4_bus_write)(uint8_t offset, uint8_t data)
{
    switch (offset)
    {
    case 0u:                                   // 0xC4 FM register select
        opl4_fm_reg_primary = data;
        opl4_ymfm_write_locked(0u, data);
        break;
    case 1u:                                   // 0xC5 FM data
        opl4_fm_regs_primary[opl4_fm_reg_primary] = data;
        DBG_FM_WRITE(1u, opl4_fm_reg_primary, data);
        opl4_ymfm_write_locked(1u, data);
        break;
    case 2u:                                   // 0xC6 FM register select #2
        opl4_fm_reg_secondary = data;
        opl4_ymfm_write_locked(2u, data);
        break;
    case 3u:                                   // 0xC7 FM data #2
        opl4_fm_regs_secondary[opl4_fm_reg_secondary] = data;
        DBG_FM_WRITE(3u, opl4_fm_reg_secondary, data);
        opl4_ymfm_write_locked(3u, data);
        break;
    case 4u:                                   // 0x7E wave register pointer
        opl4_wave_reg = data;
        opl4_ymfm_write_locked(4u, data);      // ymfm m_address tracks the pointer
        break;
    case 5u:                                   // 0x7F wave data
        switch (opl4_wave_reg)
        {
        case 0x02u:                            // memory-access / device control
            opl4_wave_reg02 = data & 0x1Fu;
            opl4_ymfm_write_locked(5u, data);  // keep ymfm wavetable-header bits in sync
            break;
        case 0x03u:                            // address bits 21-16 (stage only)
            opl4_wave_addr_hi = data & 0x3Fu;
            break;
        case 0x04u:                            // address bits 15-8 (stage only)
            opl4_wave_addr_mid = data;
            break;
        case 0x05u:                            // address bits 7-0 (commit full pointer)
            opl4_wave_addr_lo = data;
            opl4_mem_addr = (((uint32_t)opl4_wave_addr_hi << 16) |
                             ((uint32_t)opl4_wave_addr_mid << 8) |
                             (uint32_t)opl4_wave_addr_lo) & OPL4_MEMORY_MASK;
            break;
        case 0x06u:                            // memory data
            if (opl4_mem_access_active())
                opl4_mem_write(data);          // direct, lock-free, no busy
            else
                opl4_ymfm_write_locked(5u, data);
            break;
        default:                               // all other PCM registers -> ymfm
            DBG_WAVE_WRITE(opl4_wave_reg, data);
            opl4_ymfm_write_locked(5u, data);
            break;
        }
        break;
    default:
        break;
    }
}

// -----------------------------------------------------------------------
// Bus read dispatch (Core 0 IRQ context). FM status reads (0xC4) are always
// answered by ymfm so timer-overflow flags and the one-shot chip ID remain
// visible even if the host leaves PCM memory-access mode enabled. The host
// memory window and address read-backs are local.
// -----------------------------------------------------------------------
static inline uint8_t __not_in_flash_func(opl4_bus_read)(uint8_t offset)
{
    switch (offset)
    {
    case 0u:                                   // 0xC4 FM status
        {
            uint8_t status = opl4_ymfm_read_locked(0u);  // accurate timer flags + one-shot ID
#if OPL4_USB_DEBUG
            dbg_last_status = status;
#endif
            return status;
        }
    case 1u:                                   // 0xC5 FM data readback (shadow)
        return opl4_fm_regs_primary[opl4_fm_reg_primary];
    case 3u:                                   // 0xC7 FM data #2 readback (shadow)
        return opl4_fm_regs_secondary[opl4_fm_reg_secondary];
    case 2u:                                   // 0xC6 read - undefined on HW
        return 0xFFu;
    case 5u:                                   // 0x7F wave data
        switch (opl4_wave_reg)
        {
        case 0x02u:                            // device ID (ymfm ORs 0x20)
            return opl4_ymfm_read_locked(5u);
        case 0x03u:
            return opl4_wave_addr_hi;
        case 0x04u:
            return opl4_wave_addr_mid;
        case 0x05u:
            return opl4_wave_addr_lo;
        case 0x06u:
            if (opl4_mem_access_active())
                return opl4_mem_read();        // direct, lock-free
            return 0xFFu;                       // reads ignored unless mem-access on
        default:
            return opl4_ymfm_read_locked(5u);
        }
    default:
        return 0xFFu;
    }
}

// -----------------------------------------------------------------------
// PIO IRQ handlers (Core 0) - service I/O writes and reads through ymfm
// -----------------------------------------------------------------------
// Capture: empty the 8-deep PIO write FIFO into the large software ring. This
// is lock-free and ~100 ns/entry, so it always keeps up with the MSX bus and
// the PIO FIFO never overflows even during a heavy voice-update burst.
static inline void __not_in_flash_func(opl4_capture_write_fifo)(void)
{
#if OPL4_USB_DEBUG
    if (pio_sm_is_rx_fifo_full(IO_PIO_WRITE, IO_SM_WRITE))
        dbg_w_full++;
#endif
    while (!pio_sm_is_rx_fifo_empty(IO_PIO_WRITE, IO_SM_WRITE))
    {
        uint32_t sample = pio_sm_get(IO_PIO_WRITE, IO_SM_WRITE);
        uint8_t port = (uint8_t)(sample & 0xFFu);
        if (opl4_port_to_offset(port) == 0xFFu)
            continue;                          // not an OPL4 port

        uint32_t head = opl4_wr_head;
        uint32_t next = (head + 1u) & OPL4_WR_RING_MASK;
        if (next == opl4_wr_tail)
        {
#if OPL4_USB_DEBUG
            dbg_w_ringdrop++;                  // ring full (should never happen)
#endif
            continue;
        }
        opl4_wr_ring_port[head] = port;
        opl4_wr_ring_data[head] = (uint8_t)((sample >> 16) & 0xFFu);
        opl4_wr_head = next;
    }
}

// Apply: drain the software ring into ymfm/PSRAM. The ymfm spinlock is acquired
// once and held across a run of consecutive register writes (opl4_batch_locked)
// and dropped for the lock-free memory-window (sample upload) writes, so a
// playback burst costs a single lock acquisition instead of one per write.
static inline void __not_in_flash_func(opl4_apply_writes)(void)
{
    uint32_t save = 0;
    while (opl4_wr_tail != opl4_wr_head)
    {
        // Applying an entry can be slow (a wave-number write pulls a 12-byte
        // header from flash/PSRAM), so keep the hardware FIFO drained as we go.
        opl4_capture_write_fifo();

        if (!opl4_ready)
        {
            opl4_wr_tail = opl4_wr_head;       // discard until ready
            break;
        }

        uint32_t tail = opl4_wr_tail;
        uint8_t port = opl4_wr_ring_port[tail];
        uint8_t data = opl4_wr_ring_data[tail];
        opl4_wr_tail = (tail + 1u) & OPL4_WR_RING_MASK;

        uint8_t offset = opl4_port_to_offset(port);
        // Classify against the current shadow state (updated by prior entries).
        // Memory-window writes are lock-free and must NOT hold the audio lock
        // (they are the high-rate sample upload); everything else goes to ymfm.
        if (opl4_is_memory_data_port(port))
        {
            if (opl4_batch_locked)
            {
                spin_unlock(opl4_lock, save);
                opl4_batch_locked = false;
            }
        }
        else
        {
            if (!opl4_batch_locked)
            {
                save = opl4_lock_draining_fifo();
                opl4_batch_locked = true;
            }
        }

        DBG_INC(dbg_write_count);
        opl4_bus_write(offset, data);
    }

    if (opl4_batch_locked)
    {
        spin_unlock(opl4_lock, save);
        opl4_batch_locked = false;
    }
}

static inline uint32_t __not_in_flash_func(opl4_drain_write_fifo)(void)
{
    opl4_capture_write_fifo();
    opl4_apply_writes();
    return (opl4_wr_head != opl4_wr_tail) ? 1u : 0u;
}

static inline void __not_in_flash_func(opl4_drain_write_fifo_until_idle)(void)
{
    uint32_t idle_spins = 0;
    while (idle_spins < OPL4_READ_SYNC_IDLE_SPINS)
    {
        opl4_capture_write_fifo();
        bool had_writes = (opl4_wr_head != opl4_wr_tail);
        opl4_apply_writes();
        if (!had_writes && gpio_get(PIN_WR))
        {
            idle_spins++;
            tight_loop_contents();
        }
        else
        {
            idle_spins = 0;
        }
    }
}

static inline void __not_in_flash_func(opl4_answer_read_fifo)(void)
{
    if (pio_sm_is_rx_fifo_empty(IO_PIO_READ, IO_SM_READ))
        return;

    // If /WAIT is already released, any queued read tokens are stale - discard.
    if (gpio_get(PIN_WAIT))
    {
        while (!pio_sm_is_rx_fifo_empty(IO_PIO_READ, IO_SM_READ))
            (void)pio_sm_get(IO_PIO_READ, IO_SM_READ);
        if (!pio_sm_is_tx_fifo_empty(IO_PIO_READ, IO_SM_READ))
            pio_sm_drain_tx_fifo(IO_PIO_READ, IO_SM_READ);
        return;
    }

    if (!pio_sm_is_tx_fifo_empty(IO_PIO_READ, IO_SM_READ))
        pio_sm_drain_tx_fifo(IO_PIO_READ, IO_SM_READ);

    if (pio_sm_is_tx_fifo_full(IO_PIO_READ, IO_SM_READ))
        return;

    uint16_t addr = (uint16_t)pio_sm_get(IO_PIO_READ, IO_SM_READ);
    uint8_t port = (uint8_t)(addr & 0xFFu);
    uint8_t offset = opl4_ready ? opl4_port_to_offset(port) : 0xFFu;

    if (offset != 0xFFu)
    {
        DBG_INC(dbg_read_count);
        uint8_t data = opl4_bus_read(offset);
        pio_sm_put(IO_PIO_READ, IO_SM_READ, build_token(true, data));
    }
    else
    {
        pio_sm_put(IO_PIO_READ, IO_SM_READ, build_token(false, 0xFFu));
    }
}

static void __not_in_flash_func(opl4_pio_write_irq_handler)(void)
{
    opl4_drain_write_fifo();
}

static void __not_in_flash_func(opl4_pio_read_irq_handler)(void)
{
#if OPL4_USB_DEBUG
    uint32_t t0 = time_us_32();
#endif
    // The Z80 is sequential, so every write preceding this read is already in
    // the write FIFO; drain it first so OUT-then-IN register/RAM probes stay
    // coherent, then answer the read.
    opl4_drain_write_fifo_until_idle();
    opl4_answer_read_fifo();
#if OPL4_USB_DEBUG
    DBG_MAX(dbg_read_max_us, time_us_32() - t0);
#endif
}

// -----------------------------------------------------------------------
// I/O bus PIO initialisation (read responder WITH /WAIT, decoded write captor)
// -----------------------------------------------------------------------
static void io_bus_init(void)
{
    uint offset_read  = pio_add_program(IO_PIO_READ, &msx_opl4_io_read_program);
    uint offset_write = pio_add_program(IO_PIO_WRITE, &msx_opl4_io_write_program);

    // --- SM0: I/O read responder (with /WAIT) ---
    pio_sm_set_enabled(IO_PIO_READ, IO_SM_READ, false);
    pio_sm_clear_fifos(IO_PIO_READ, IO_SM_READ);
    pio_sm_restart(IO_PIO_READ, IO_SM_READ);

    pio_sm_config cfg_r = msx_opl4_io_read_program_get_default_config(offset_read);
    sm_config_set_in_pins(&cfg_r, PIN_A0);
    sm_config_set_in_shift(&cfg_r, false, false, 16);
    sm_config_set_out_pins(&cfg_r, PIN_D0, 8);
    sm_config_set_out_shift(&cfg_r, true, false, 32);
    sm_config_set_set_pins(&cfg_r, PIN_WAIT, 1);
    sm_config_set_jmp_pin(&cfg_r, PIN_RD);
    sm_config_set_clkdiv(&cfg_r, 1.0f);
    pio_sm_init(IO_PIO_READ, IO_SM_READ, offset_read, &cfg_r);

    // Preload /WAIT LOW, then leave the pin direction as input. The PIO read
    // program asserts /WAIT by switching the pin to output, and releases it by
    // switching back to input, matching the shared open-drain MSX bus signal.
    pio_sm_set_pins_with_mask(IO_PIO_READ, IO_SM_READ, 0u, (1u << PIN_WAIT));
    pio_gpio_init(IO_PIO_READ, PIN_WAIT);
    pio_sm_set_consecutive_pindirs(IO_PIO_READ, IO_SM_READ, PIN_WAIT, 1, false);

    // Data bus D0-D7: hand to PIO for bidirectional control, initially input.
    for (int i = PIN_D0; i <= PIN_D7; i++)
        pio_gpio_init(IO_PIO_READ, i);
    pio_sm_set_consecutive_pindirs(IO_PIO_READ, IO_SM_READ, PIN_D0, 8, false);

    // --- PIO0 SM1: decoded I/O write captor ---
    pio_sm_set_enabled(IO_PIO_WRITE, IO_SM_WRITE, false);
    pio_sm_clear_fifos(IO_PIO_WRITE, IO_SM_WRITE);
    pio_sm_restart(IO_PIO_WRITE, IO_SM_WRITE);

    pio_sm_config cfg_w = msx_opl4_io_write_program_get_default_config(offset_write);
    sm_config_set_in_pins(&cfg_w, PIN_A0);
    sm_config_set_in_shift(&cfg_w, false, false, 32);
    sm_config_set_fifo_join(&cfg_w, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_w, PIN_WR);
    sm_config_set_clkdiv(&cfg_w, 1.0f);
    pio_sm_init(IO_PIO_WRITE, IO_SM_WRITE, offset_write, &cfg_w);

    // --- PIO2 SM0: dedicated /BUSDIR driver ---
    // /BUSDIR (GPIO 37) lies above the 32-pin window the read/write SMs use,
    // so it gets its own PIO with GPIO base 16 (pins 16-47). It mirrors the
    // read SM's selection: /WAIT (GPIO 28) low == an OPL4 read is in progress,
    // and /BUSDIR is held low until /RD (GPIO 24) goes high. IN base = /RD so
    // pin 0 = /RD and pin 4 = /WAIT (both IN-relative, base-safe).
    pio_set_gpio_base(IO_PIO_BUSDIR, 16);
    uint offset_busdir = pio_add_program(IO_PIO_BUSDIR, &msx_opl4_busdir_program);

    pio_sm_set_enabled(IO_PIO_BUSDIR, IO_SM_BUSDIR, false);
    pio_sm_clear_fifos(IO_PIO_BUSDIR, IO_SM_BUSDIR);
    pio_sm_restart(IO_PIO_BUSDIR, IO_SM_BUSDIR);

    pio_sm_config cfg_b = msx_opl4_busdir_program_get_default_config(offset_busdir);
    sm_config_set_in_pins(&cfg_b, PIN_RD);          // pin0=/RD(24), pin4=/WAIT(28)
    sm_config_set_jmp_pin(&cfg_b, PIN_IORQ);        // skip non-I/O /WAIT cycles
    sm_config_set_sideset_pins(&cfg_b, PIN_BUSSDIR);// /BUSDIR (GPIO 37)
    sm_config_set_clkdiv(&cfg_b, 1.0f);
    pio_sm_init(IO_PIO_BUSDIR, IO_SM_BUSDIR, offset_busdir, &cfg_b);

    // Drive /BUSDIR PUSH-PULL (like a real OPL4 cartridge): the pin is a
    // permanent output and the SM's side-set toggles the VALUE - high =
    // released, low = asserted. Preload HIGH so it idles released. An earlier
    // open-drain version (hi-Z when released) could let the MSX data-bus-buffer
    // DIR input float during writes and drop register writes (missing
    // instruments) on machines without a strong /BUSDIR pull-up.
    pio_sm_set_pins_with_mask64(IO_PIO_BUSDIR, IO_SM_BUSDIR,
                                (1ull << PIN_BUSSDIR), (1ull << PIN_BUSSDIR));
    pio_gpio_init(IO_PIO_BUSDIR, PIN_BUSSDIR);
    pio_sm_set_consecutive_pindirs(IO_PIO_BUSDIR, IO_SM_BUSDIR, PIN_BUSSDIR, 1, true);

    // --- Route PIO1 SM0 RX-not-empty to PIO1_IRQ_0 -> Core 0. ---
    irq_set_exclusive_handler(PIO1_IRQ_0, opl4_pio_read_irq_handler);
    irq_set_priority(PIO1_IRQ_0, 0);   // highest priority (has the bus deadline)
    pio_set_irq0_source_enabled(IO_PIO_READ, pis_sm0_rx_fifo_not_empty, true);
    irq_set_enabled(PIO1_IRQ_0, true);

    // --- Route PIO0 SM1 RX-not-empty to PIO0_IRQ_0 -> Core 0. ---
    irq_set_exclusive_handler(PIO0_IRQ_0, opl4_pio_write_irq_handler);
    irq_set_priority(PIO0_IRQ_0, 0);
    pio_set_irq0_source_enabled(IO_PIO_WRITE, pis_sm1_rx_fifo_not_empty, true);
    irq_set_enabled(PIO0_IRQ_0, true);

    // --- Enable the state machines only after the IRQ can answer reads. ---
    pio_sm_set_enabled(IO_PIO_READ,   IO_SM_READ,   true);
    pio_sm_set_enabled(IO_PIO_WRITE,  IO_SM_WRITE,  true);
    pio_sm_set_enabled(IO_PIO_BUSDIR, IO_SM_BUSDIR, true);
}

// -----------------------------------------------------------------------
// OPL4 core init (Core 0, before Core 1 launch)
// -----------------------------------------------------------------------
// MSX /INT line driver (open-drain on GPIO40). Registered with the ymfm
// interface so a YMF278B FM-timer interrupt pulls /INT low; the MSX maskable
// interrupt handler then polls the OPL4 status, services the song, and writes
// the timer IRQ-reset which clears the flag and releases /INT. The callback is
// invoked from whichever core changes the IRQ state (Core 1 when a timer fires
// during synthesis, Core 0 when a host write resets the timer); GPIO writes via
// SIO are atomic, so no lock is needed.
extern void opl4_set_irq_line_callback(void (*cb)(bool asserted));

static void __not_in_flash_func(opl4_int_line_drive)(bool asserted)
{
    if (asserted)
    {
        // Assert: actively drive the line low.
        gpio_put(PIN_INT, 0);
        gpio_set_dir(PIN_INT, GPIO_OUT);
    }
    else
    {
        // Release: return to hi-Z so the shared open-collector /INT floats back
        // high through the MSX pull-up (never actively driven high).
        gpio_set_dir(PIN_INT, GPIO_IN);
    }
}

static void opl4_load_config(void)
{
    if (opl4_config_loaded)
        return;

    // Resolve the appended payload (via the volatile source pointer so the copy
    // below is not treated as a fixed-size-object overread). The loadrom tool
    // writes a 16-byte config header (magic + reserved flags) before the
    // YRW801-M ROM; skip past it when present.
    const uint8_t *appended = (const uint8_t *)yrw801_flash_src;
    if (appended[0] == OPL4_CFG_MAGIC0 && appended[1] == OPL4_CFG_MAGIC1 &&
        appended[2] == OPL4_CFG_MAGIC2 && appended[3] == OPL4_CFG_MAGIC3)
    {
        yrw801_flash = appended + OPL4_CFG_HEADER_SIZE;
    }
    else
    {
        // Legacy image without a config header: ROM starts immediately.
        yrw801_flash = appended;
    }

    opl4_config_loaded = true;
}

static void opl4_init(void)
{
    opl4_lock = spin_lock_init(spin_lock_claim_unused(true));

    // /INT released (hi-Z) at start; the ymfm interface will pulse it via the
    // registered callback when the FM timers raise an interrupt.
    gpio_init(PIN_INT);
    gpio_put(PIN_INT, 0);                 // preload 0 so an assert only flips dir
    gpio_set_dir(PIN_INT, GPIO_IN);       // released = hi-Z
    opl4_set_irq_line_callback(opl4_int_line_drive);

    opl4_load_config();

    // 2 MB PCM sample RAM from external PSRAM (cached write-through window).
    opl4_sample_ram = (uint8_t *)(PSRAM_BASE_ADDR + OPL4_PSRAM_SAMPLE_OFFSET);

#if OPL4_YRW801_IN_PSRAM
    // Copy the 2 MB YRW801-M wave ROM from flash into PSRAM so the whole 4 MB
    // PCM map lives on a single QMI chip select. memcpy goes through the cached
    // write-through PSRAM window; flush is implicit on the write-through path.
    uint8_t *yrw801_dst = (uint8_t *)(PSRAM_BASE_ADDR + OPL4_PSRAM_YRW801_OFFSET);
    memcpy(yrw801_dst, yrw801_flash, OPL4_YRW801_SIZE);
    yrw801_rom = yrw801_dst;
#else
    // Read the wave ROM directly from flash XIP.
    yrw801_rom = yrw801_flash;
#endif

    OPL4_Reset(&opl4_instance, yrw801_rom, OPL4_YRW801_SIZE,
               opl4_sample_ram, OPL4_SAMPLE_RAM_SIZE);
    opl4_host_registers_reset();
    opl4_ready = true;
}

// -----------------------------------------------------------------------
// I2S audio init (PIO0 SM0) - called on Core 1 so the DMA IRQ lives there
// -----------------------------------------------------------------------
static void i2s_audio_init(void)
{
    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 0);   // active-high mute: low = unmuted

    static audio_format_t audio_format = {
        .sample_freq = OPL4_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };

    static struct audio_buffer_format producer_format = {
        .format = &audio_format,
        .sample_stride = 4,
    };

    audio_pool = audio_new_producer_pool(&producer_format, 3, OPL4_AUDIO_BUFFER_SAMPLES);

    for (int ch = 0; ch < NUM_DMA_CHANNELS; ++ch)
    {
        if (!dma_channel_is_claimed((uint)ch))
        {
            audio_dma_channel = ch;
            break;
        }
    }
    if (audio_dma_channel < 0)
    {
        audio_pool = NULL;
        return;
    }

    static struct audio_i2s_config i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_BCLK_PIN,
        .dma_channel = 0,
        .pio_sm = IO_SM_I2S,
    };
    i2s_config.dma_channel = (uint)audio_dma_channel;

    audio_i2s_setup(&audio_format, &i2s_config);
    audio_i2s_connect(audio_pool);
    audio_i2s_set_enabled(true);
}

// -----------------------------------------------------------------------
// Core 1 - dedicated OPL4 synthesis straight into the I2S producer pool.
//
// ymfm needs ~one whole core at 44.1 kHz, so Core 1 does nothing else. Each
// frame is generated under the shared lock so the bus IRQ on Core 0 sees a
// coherent chip; the lock is released every frame (~22 us) so a bus access on
// Core 0 never waits more than one frame. The pico_audio_i2s pool paces this
// loop to exactly 44.1 kHz via take_audio_buffer() blocking on a free buffer.
// -----------------------------------------------------------------------
static void __no_inline_not_in_flash_func(core1_opl4_i2s)(void)
{
    i2s_audio_init();

    if (!audio_pool)
    {
        while (true) { tight_loop_contents(); }
    }

#if OPL4_USB_DEBUG
    // Buffer playback duration in microseconds (256 frames / 44100 Hz).
    const uint32_t buffer_us = (uint32_t)(((uint64_t)OPL4_AUDIO_BUFFER_SAMPLES * 1000000u) /
                                          OPL4_SAMPLE_RATE);
#endif

    while (true)
    {
        struct audio_buffer *buffer = take_audio_buffer(audio_pool, true);
        int16_t *samples = (int16_t *)buffer->buffer->bytes;

#if OPL4_USB_DEBUG
        uint32_t fill_t0 = time_us_32();
#endif
        for (int i = 0; i < OPL4_AUDIO_BUFFER_SAMPLES; ++i)
        {
            int32_t out[2] = {0, 0};

#if OPL4_USB_DEBUG && OPL4_DEBUG_LOCK_TIMING
            uint32_t w0 = time_us_32();
#endif
            uint32_t save = spin_lock_blocking(opl4_lock);
#if OPL4_USB_DEBUG && OPL4_DEBUG_LOCK_TIMING
            DBG_MAX(dbg_c1_wait_max, time_us_32() - w0);
#endif
            OPL4_Clock(&opl4_instance, out);
            spin_unlock(opl4_lock, save);

            samples[i * 2]     = clamp_i16(out[0]);
            samples[i * 2 + 1] = clamp_i16(out[1]);
        }

#if OPL4_USB_DEBUG
        uint32_t fill = time_us_32() - fill_t0;
        DBG_MAX(dbg_gen_max_us, fill);
        DBG_ADD(dbg_gen_frames, OPL4_AUDIO_BUFFER_SAMPLES);
        if (fill > buffer_us)
            DBG_INC(dbg_late_buffers);
#endif

        buffer->sample_count = OPL4_AUDIO_BUFFER_SAMPLES;
        give_audio_buffer(audio_pool, buffer);
    }
}

// -----------------------------------------------------------------------
// main (Core 0)
// -----------------------------------------------------------------------
int __no_inline_not_in_flash_func(main)(void)
{
    // RP2350 flash timing + system clock. The OPL4 firmware overclocks to
    // 300 MHz so ymfm can synthesise the full YMF278B (18 FM + 24 PCM voices)
    // in real time at 44.1 kHz; at 210 MHz the loaded synthesis ran ~25-35%
    // over the per-frame budget. The flash clock divider is 3 (flash at
    // 300/3 = 100 MHz, the same speed it ran at 210 MHz/divider 2), so flash
    // XIP - used at boot to copy code to RAM and to copy the YRW801 ROM into
    // PSRAM - keeps its existing timing. Code itself runs from SRAM
    // (copy_to_ram), so runtime performance does not depend on flash speed.
    qmi_hw->m[0].timing = 0x40000203;
    set_sys_clock_khz(OPL4_SYS_CLOCK_KHZ, true);

    setup_gpio();

    // External PSRAM provides the 2 MB PCM sample RAM. Halt on failure.
    if (!psram_init())
    {
        // Release /WAIT (held since setup_gpio) so the MSX can still boot
        // without the OPL4 cartridge instead of being frozen forever.
        gpio_set_dir(PIN_WAIT, GPIO_IN);
        while (true) { tight_loop_contents(); }
    }

    opl4_init();

    // Install the I/O bus + Core 0 IRQ BEFORE launching Core 1, so the bus IRQ
    // is owned by Core 0 and Core 1 only handles synthesis + the I2S DMA.
    io_bus_init();

    multicore_launch_core1(core1_opl4_i2s);

#if OPL4_USB_DEBUG
    // Only after the cartridge is live on the bus: waiting for the USB host
    // first would leave the OPL4 unable to answer for the whole timeout, so
    // software that probes for the cartridge at boot would not detect it.
    stdio_init_all();
    opl4_debug_usb_boot_wait();
#endif

    // Core 0 services all MSX I/O from the PIO IRQs; the main loop only emits
    // USB CDC diagnostics (when enabled) and otherwise idles. Synthesis runs
    // entirely on Core 1.
    while (true)
    {
        opl4_debug_dump();
#if OPL4_USB_DEBUG
        sleep_ms(1);
#else
        __wfi();
#endif
    }
}
