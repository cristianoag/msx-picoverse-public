// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// audio_trace.c - Ring buffer capture and USB CDC reporting for the SCC + PSG
//                 mirror investigation.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include "audio_trace.h"

#if EXPLORER_AUDIO_TRACE

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/regs/pio.h"

// The tracer is built together with the USB CDC stdio transport, so reporting
// is always possible in principle. It is still gated on an actual host
// connection: with no terminal attached printf() blocks for the full stdout
// timeout on every call, which would stall the audio core.
#define ATRACE_LINK_UP() stdio_usb_connected()

atrace_counters_t atrace_cnt;
atrace_ev_t atrace_ring[2][ATRACE_RING_SIZE];
uint32_t atrace_head[2];
atrace_fm_acc_t atrace_fm_acc;
uint32_t atrace_fm_busy_us;
volatile bool atrace_capture = true;
volatile bool atrace_psg_only = false;
volatile uint8_t atrace_psg_sel = 0xFFu;
volatile uint8_t atrace_opll_sel = 0xFFu;
volatile uint16_t atrace_opll_key_state = 0u;
volatile uint32_t atrace_trigger = ATRACE_TRIG_NONE;

static void (*atrace_state_cb)(void) = NULL;
static void (*atrace_opll_state_cb)(void) = NULL;
static uint32_t atrace_last_heartbeat_ms;
static uint32_t atrace_last_dump_ms;
static uint32_t atrace_dumps_emitted;
static uint32_t atrace_fm_last_done; // timestamp of the previous completed buffer

// MSX-MUSIC streaming state. The whole FM log is emitted on a timer so a
// capture needs nothing but a connected terminal.
#define ATRACE_FM_WINDOW_MS  100u   // one data line per window
#define ATRACE_FM_SUMMARY_MS 5000u  // cumulative totals + configuration
static uint32_t atrace_fm_window_ms;
static uint32_t atrace_fm_summary_ms;
static bool     atrace_fm_banner_done;
static uint32_t atrace_fm_t0_ms;
// Counter snapshots taken at the start of each window, so the emitted line
// reports what happened during that window rather than since boot.
static uint32_t atrace_w_dat, atrace_w_kon, atrace_w_koff;
static uint32_t atrace_w_drop, atrace_w_late, atrace_w_bufs;
static uint32_t atrace_w_iolost, atrace_w_psgw;

// Printing is slow enough to starve the audio pipeline, so automatic dumps are
// rate limited and capped.  A tester chasing a reproducible fault can always
// force one from the terminal.
#define ATRACE_DUMP_MIN_INTERVAL_MS 2000u
#define ATRACE_DUMP_MAX_EVENTS      400u
#define ATRACE_HEARTBEAT_MS         1000u

void atrace_init(void)
{
    memset(&atrace_cnt, 0, sizeof(atrace_cnt));
    memset(atrace_head, 0, sizeof(atrace_head));
    memset(&atrace_fm_acc, 0, sizeof(atrace_fm_acc));
    atrace_psg_sel = 0xFFu;
    atrace_opll_sel = 0xFFu;
    atrace_opll_key_state = 0u;
    atrace_trigger = ATRACE_TRIG_NONE;
    atrace_last_heartbeat_ms = to_ms_since_boot(get_absolute_time());
    atrace_last_dump_ms = atrace_last_heartbeat_ms;
    atrace_dumps_emitted = 0u;
    atrace_fm_window_ms = atrace_last_heartbeat_ms;
    atrace_fm_summary_ms = atrace_last_heartbeat_ms;
    atrace_fm_t0_ms = atrace_last_heartbeat_ms;
    atrace_fm_banner_done = false;
    atrace_fm_last_done = 0u;
    atrace_fm_busy_us = 0u;
    atrace_w_dat = atrace_w_kon = atrace_w_koff = 0u;
    atrace_w_drop = atrace_w_late = atrace_w_bufs = 0u;
    atrace_w_iolost = atrace_w_psgw = 0u;
}

void atrace_set_state_cb(void (*cb)(void))
{
    atrace_state_cb = cb;
}

void atrace_set_opll_state_cb(void (*cb)(void))
{
    atrace_opll_state_cb = cb;
}

static uint32_t atrace_isqrt(uint64_t v)
{
    uint64_t r = 0;
    uint64_t bit = 1ull << 42;
    while (bit > v)
        bit >>= 2;
    while (bit)
    {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return (uint32_t)r;
}

// Called once per audio buffer. Only counts the buffer and detects a late one;
// all the level accumulation happens per sample and is emitted per window.
void __not_in_flash_func(atrace_fm_buffer_done)(void)
{
    atrace_cnt.fm_buffers++;

    uint32_t now = timer_hw->timerawl;
    // A buffer holds 256 samples = 5805 us of audio. If two consecutive buffers
    // are further apart than that, core1 did not keep up and the I2S output ran
    // dry in between - the real underrun test, unlike "no free buffer", which
    // just means core1 is ahead of the DAC.
    if (atrace_cnt.fm_buffers > 1u && (uint32_t)(now - atrace_fm_last_done) > 6200u)
        atrace_cnt.fm_starve++;
    atrace_fm_last_done = now;
}

static const char *atrace_type_name(uint8_t type)
{
    switch (type)
    {
    case ATRACE_EV_PSG_SEL:   return "PSG.SEL";
    case ATRACE_EV_PSG_DAT:   return "PSG.DAT";
    case ATRACE_EV_IO_OTHER:  return "IO.WR  ";
    case ATRACE_EV_PSG_DROP:  return "PSG.DROP";
    case ATRACE_EV_SCC_WR:    return "SCC.WR ";
    case ATRACE_EV_IO_STALL:  return "IO.STALL";
    case ATRACE_EV_MEM_STALL: return "MEM.STALL";
    case ATRACE_EV_MARK:      return "MARK   ";
    default:                  return "?      ";
    }
}

static const char *atrace_trigger_name(uint32_t reason)
{
    switch (reason)
    {
    case ATRACE_TRIG_DROP:     return "PSG hand-off ring overflow";
    case ATRACE_TRIG_BADSEL:   return "PSG register select > 15";
    case ATRACE_TRIG_IOSTALL:  return "I/O write captor stalled (writes lost)";
    case ATRACE_TRIG_MEMSTALL: return "memory write captor stalled (writes lost)";
    default:                   return "manual";
    }
}

// PSG register names, so the log is readable without a datasheet next to it.
static const char *atrace_psg_reg_name(uint8_t reg)
{
    static const char *names[16] = {
        "A.freq.lo", "A.freq.hi", "B.freq.lo", "B.freq.hi",
        "C.freq.lo", "C.freq.hi", "noise.per", "MIXER",
        "A.vol",     "B.vol",     "C.vol",     "env.lo",
        "env.hi",    "env.shape", "ioA",       "ioB"
    };
    return (reg < 16u) ? names[reg] : "INVALID";
}

// Walk both per-core rings oldest-to-newest and merge them by timestamp.
void atrace_dump(const char *reason)
{
    if (!ATRACE_LINK_UP())
        return;

    uint32_t head[2];
    uint32_t idx[2];
    uint32_t left[2];
    for (uint32_t c = 0; c < 2u; c++)
    {
        head[c] = atrace_head[c];
        uint32_t count = (head[c] > ATRACE_RING_SIZE) ? ATRACE_RING_SIZE : head[c];
        idx[c] = head[c] - count;
        left[c] = count;
    }

    uint32_t total = left[0] + left[1];
    uint32_t skip = (total > ATRACE_DUMP_MAX_EVENTS) ? (total - ATRACE_DUMP_MAX_EVENTS) : 0u;

    printf("\r\n===== atrace dump (%s) events=%lu shown=%lu =====\r\n",
           reason, (unsigned long)total, (unsigned long)(total - skip));
    printf("   delta_us  event      addr  D0-7  A8-15  detail\r\n");

    uint32_t first_t = 0u;
    bool have_first = false;
    uint32_t prev_t = 0u;

    while (left[0] || left[1])
    {
        uint32_t c;
        if (!left[0])
            c = 1u;
        else if (!left[1])
            c = 0u;
        else
        {
            const atrace_ev_t *e0 = &atrace_ring[0][idx[0] & ATRACE_RING_MASK];
            const atrace_ev_t *e1 = &atrace_ring[1][idx[1] & ATRACE_RING_MASK];
            // Unsigned difference keeps the ordering correct across the 32-bit
            // microsecond wrap (once every ~71 minutes).
            c = ((uint32_t)(e0->t - e1->t) & 0x80000000u) ? 0u : 1u;
        }

        const atrace_ev_t ev = atrace_ring[c][idx[c] & ATRACE_RING_MASK];
        idx[c]++;
        left[c]--;

        if (!have_first)
        {
            first_t = ev.t;
            prev_t = ev.t;
            have_first = true;
        }

        if (skip)
        {
            skip--;
            prev_t = ev.t;
            continue;
        }

        printf("%10lu  %-9s %04X  %02X    %02X     ",
               (unsigned long)(ev.t - prev_t), atrace_type_name(ev.type),
               ev.addr, ev.data, (unsigned)(ev.addr >> 8));

        switch (ev.type)
        {
        case ATRACE_EV_PSG_SEL:
            printf("select r%u (%s)%s", ev.data, atrace_psg_reg_name(ev.data),
                   (ev.data > 15u) ? "  <<< INVALID" : "");
            if (ev.data == (ev.addr & 0xFFu))
                printf("  [D=port: STALE BUS]");
            if (ev.aux <= 15u)
                printf("  [A8-15=r%u %s]", ev.aux, atrace_psg_reg_name(ev.aux));
            break;
        case ATRACE_EV_PSG_DAT:
            printf("r%u %s = %02X", ev.aux, atrace_psg_reg_name(ev.aux), ev.data);
            if (ev.data == (ev.addr & 0xFFu))
                printf("  [D=port: STALE BUS]");
            printf("  [A8-15=%02X]", (unsigned)(ev.addr >> 8));
            break;
        case ATRACE_EV_IO_OTHER:
            printf("port %02X = %02X", ev.addr & 0xFFu, ev.data);
            if (ev.data == (ev.addr & 0xFFu))
                printf("  [D=port: STALE BUS]");
            else if (ev.data == (uint8_t)(ev.addr >> 8))
                printf("  [D=A8-15: bus OK]");
            break;
        case ATRACE_EV_SCC_WR:
            printf("addr %04X = %02X", ev.addr, ev.data);
            break;
        case ATRACE_EV_PSG_DROP:
            printf("core0->core1 PSG ring FULL, write discarded");
            break;
        case ATRACE_EV_IO_STALL:
            printf("I/O write FIFO overflow: MSX writes were LOST");
            break;
        case ATRACE_EV_MEM_STALL:
            printf("memory write FIFO overflow: MSX writes were LOST");
            break;
        default:
            break;
        }
        printf(" (core%lu)\r\n", (unsigned long)c);
        prev_t = ev.t;
    }

    printf("----- window %lu us -----\r\n", (unsigned long)(prev_t - first_t));
    fflush(stdout);
}

void atrace_print_counters(void)
{
    if (!ATRACE_LINK_UP())
        return;

    printf("[atrace] io=%lu psg=%lu (sel=%lu dat=%lu) badSel=%lu datNoSel=%lu "
           "drops=%lu scc=%lu (wave=%lu freq=%lu vol=%lu) "
           "ioStall=%lu memStall=%lu ioRdStall=%lu\r\n",
           (unsigned long)atrace_cnt.io_writes,
           (unsigned long)atrace_cnt.psg_io,
           (unsigned long)atrace_cnt.psg_sel,
           (unsigned long)atrace_cnt.psg_dat,
           (unsigned long)atrace_cnt.psg_bad_sel,
           (unsigned long)atrace_cnt.psg_dat_no_sel,
           (unsigned long)atrace_cnt.psg_ring_drops,
           (unsigned long)atrace_cnt.scc_writes,
           (unsigned long)atrace_cnt.scc_wave,
           (unsigned long)atrace_cnt.scc_freq,
           (unsigned long)atrace_cnt.scc_vol,
           (unsigned long)atrace_cnt.io_stalls,
           (unsigned long)atrace_cnt.mem_stalls,
           (unsigned long)atrace_cnt.io_rd_stalls);
    // Data-bus verdict: on a healthy bus the sampled byte matches A8..A15 for
    // "OUT (n),A" traffic. Matching the port instead means D0..D7 still held
    // the operand byte from the preceding opcode fetch, i.e. the slot data
    // buffer had not driven the real value by the time we latched it.
    uint32_t n = atrace_cnt.io_writes ? atrace_cnt.io_writes : 1u;
    printf("[atrace] bus: D==port %lu (%lu%%)  D==A8-15 %lu (%lu%%)  "
           "portA0 writes with valid A8-15 %lu/%lu\r\n",
           (unsigned long)atrace_cnt.io_data_eq_port,
           (unsigned long)(100u * atrace_cnt.io_data_eq_port / n),
           (unsigned long)atrace_cnt.io_data_eq_ahi,
           (unsigned long)(100u * atrace_cnt.io_data_eq_ahi / n),
           (unsigned long)atrace_cnt.psg_ahi_valid,
           (unsigned long)atrace_cnt.psg_sel);
    fflush(stdout);
}

static void atrace_print_psg_histogram(void)
{
    if (!ATRACE_LINK_UP())
        return;
    printf("[atrace] PSG writes per register:\r\n");
    for (uint8_t r = 0; r < 16u; r++)
    {
        printf("   r%-2u %-10s %lu\r\n", r, atrace_psg_reg_name(r),
               (unsigned long)atrace_cnt.psg_reg_w[r]);
    }
    uint32_t invalid = 0u;
    for (uint8_t r = 16u; r < 32u; r++)
        invalid += atrace_cnt.psg_reg_w[r];
    printf("   r16..r31 (invalid) %lu\r\n", (unsigned long)invalid);
    fflush(stdout);
}

// MSX-MUSIC summary. `buf/s` is the load-bearing number: core1 must produce
// 44100/256 = 172 buffers per second. Anything less means the audio core is
// being starved and the I2S output is repeating or gapping.
static void atrace_fm_summary(void)
{
    uint32_t ns = atrace_cnt.fm_samples ? atrace_cnt.fm_samples : 1u;
    printf("[fm.sum] FMwrites sel=%lu dat=%lu (io=%lu mem=%lu) badReg=%lu noSel=%lu "
           "DROPPED=%lu | keyOn=%lu keyOff=%lu rhythm=%lu | FMPAC ctl=%lu bank=%lu\r\n",
           (unsigned long)atrace_cnt.opll_sel,
           (unsigned long)atrace_cnt.opll_dat,
           (unsigned long)atrace_cnt.opll_io_writes,
           (unsigned long)atrace_cnt.opll_mem_writes,
           (unsigned long)atrace_cnt.opll_bad_reg,
           (unsigned long)atrace_cnt.opll_dat_no_sel,
           (unsigned long)atrace_cnt.opll_ring_drops,
           (unsigned long)atrace_cnt.opll_key_on,
           (unsigned long)atrace_cnt.opll_key_off,
           (unsigned long)atrace_cnt.opll_rhythm_w,
           (unsigned long)atrace_cnt.fmpac_ctl_w,
           (unsigned long)atrace_cnt.fmpac_bank_w);
    printf("[fm.sum] out buffers=%lu samples=%lu late=%lu peak=%ld (fm=%ld psg=%ld) "
           "limited=%lu%% clipped=%lu | PSGgate on=%lu off=%lu open=%lu%% drops=%lu | "
           "ioFIFOlost=%lu memFIFOlost=%lu\r\n",
           (unsigned long)atrace_cnt.fm_buffers,
           (unsigned long)atrace_cnt.fm_samples,
           (unsigned long)atrace_cnt.fm_starve,
           (long)atrace_cnt.fm_peak,
           (long)atrace_cnt.fm_peak_fm,
           (long)atrace_cnt.fm_peak_psg,
           (unsigned long)(100u * atrace_cnt.fm_limited / ns),
           (unsigned long)atrace_cnt.fm_clipped,
           (unsigned long)atrace_cnt.psg_gate_on,
           (unsigned long)atrace_cnt.psg_gate_off,
           (unsigned long)(100u * atrace_cnt.psg_gate_samples / ns),
           (unsigned long)atrace_cnt.psg_ring_drops,
           (unsigned long)atrace_cnt.io_stalls,
           (unsigned long)atrace_cnt.mem_stalls);
    if (atrace_opll_state_cb)
        atrace_opll_state_cb();

    // Where the audio core's time goes. A buffer is 5149 us of audio, so these
    // are the numbers that say what to optimise when it cannot keep up.
    uint32_t nb = atrace_cnt.fm_buffers ? atrace_cnt.fm_buffers : 1u;
    printf("[fm.cost] per buffer: OPLL=%lu us  PSG=%lu us  serviceIO=%lu us  "
           "(budget 5149 us for 256 samples)\r\n",
           (unsigned long)(atrace_cnt.us_fm / nb),
           (unsigned long)(atrace_cnt.us_psg / nb),
           (unsigned long)(atrace_cnt.us_io / nb));
    fflush(stdout);
}

// The whole MSX-MUSIC log streams by itself: a header once, then one data line
// every 100 ms and a cumulative summary every 5 s. Nothing has to be typed.
static void atrace_fm_stream(uint32_t now)
{
    if (!atrace_fm_banner_done)
    {
        atrace_fm_banner_done = true;
        atrace_fm_t0_ms = now;
        atrace_fm_window_ms = now;
        atrace_fm_summary_ms = now;
        printf("\r\n===== MSX-MUSIC (FM-PAC) trace - one line per 100 ms =====\r\n"
               "t      seconds since the FM output started\r\n"
               "buf    audio buffers produced in the window (19 = correct, less = starved)\r\n"
               "pk/rms peak and RMS of the final mixed output\r\n"
               "fm     peak of the FM contribution alone\r\n"
               "psg    peak of the PSG mirror contribution alone\r\n"
               "lim    samples the soft limiter compressed\r\n"
               "wr     YM2413 register writes that arrived\r\n"
               "kOn    key-on / key-off transitions (is the driver still playing?)\r\n"
               "pOn    %% of samples with the PSG mirror gate open\r\n"
               "flip   PSG mirror gate transitions (each one kicks the DC blocker)\r\n"
               "drp    YM2413 register writes LOST to a full hand-off ring\r\n"
               "ioLost I/O write FIFO overflows - MSX I/O writes LOST this window.\r\n"
               "       This game sends its YM2413 writes through I/O ports 7C/7D,\r\n"
               "       so a non-zero ioLost means FM register writes were dropped.\r\n"
               "psgW   PSG register writes that arrived\r\n"
               "cpu%%   share of real time core1 spent generating audio.\r\n"
               "       Near 100 means the audio core is saturated and the output\r\n"
               "       will break up - that is what a robotic or stuttering tone is.\r\n"
               "late   buffers that missed real time (output ran dry)\r\n"
               "\r\n"
               "     t  buf     pk    rms     fm    psg   lim   wr  kOn kOff pOn flip drp ioLost psgW cpu% late\r\n");
        atrace_fm_summary();
    }

    if ((now - atrace_fm_window_ms) < ATRACE_FM_WINDOW_MS)
        return;

    uint32_t elapsed = now - atrace_fm_window_ms;
    atrace_fm_window_ms = now;

    // Snapshot and clear the per-sample accumulator for the next window.
    atrace_fm_acc_t w = atrace_fm_acc;
    memset(&atrace_fm_acc, 0, sizeof(atrace_fm_acc));

    uint32_t dat  = atrace_cnt.opll_dat;
    uint32_t kon  = atrace_cnt.opll_key_on;
    uint32_t koff = atrace_cnt.opll_key_off;
    uint32_t drop = atrace_cnt.opll_ring_drops;
    uint32_t late = atrace_cnt.fm_starve;
    uint32_t bufs = atrace_cnt.fm_buffers;
    uint32_t iolost = atrace_cnt.io_stalls;
    uint32_t psgw = atrace_cnt.psg_dat;

    // Fold the window into the running totals the 5 s summary reports.
    atrace_cnt.fm_samples += w.n;
    atrace_cnt.fm_limited += w.limited;
    atrace_cnt.psg_gate_samples += w.psg_on;
    if (w.peak > atrace_cnt.fm_peak)         atrace_cnt.fm_peak = w.peak;
    if (w.fm_peak > atrace_cnt.fm_peak_fm)   atrace_cnt.fm_peak_fm = w.fm_peak;
    if (w.psg_peak > atrace_cnt.fm_peak_psg) atrace_cnt.fm_peak_psg = w.psg_peak;

    if (w.n)
    {
        // Busy time against wall-clock time for the window. elapsed is in ms.
        uint32_t busy = atrace_fm_busy_us;
        atrace_fm_busy_us = 0u;
        uint32_t cpu = elapsed ? (busy / (elapsed * 10u)) : 0u;

        printf("%6lu %4lu %6ld %6lu %6ld %6ld %5lu %4lu %4lu %4lu %3lu %4lu %3lu %6lu %4lu %4lu %4lu%s%s%s%s\r\n",
               (unsigned long)((now - atrace_fm_t0_ms) / 100u),   /* tenths of a second */
               (unsigned long)(bufs - atrace_w_bufs),
               (long)w.peak,
               (unsigned long)atrace_isqrt(w.sumsq / w.n),
               (long)w.fm_peak,
               (long)w.psg_peak,
               (unsigned long)w.limited,
               (unsigned long)(dat - atrace_w_dat),
               (unsigned long)(kon - atrace_w_kon),
               (unsigned long)(koff - atrace_w_koff),
               (unsigned long)(100u * w.psg_on / w.n),
               (unsigned long)w.psg_flip,
               (unsigned long)(drop - atrace_w_drop),
               (unsigned long)(iolost - atrace_w_iolost),
               (unsigned long)(psgw - atrace_w_psgw),
               (unsigned long)cpu,
               (unsigned long)(late - atrace_w_late),
               (drop != atrace_w_drop) ? "  <<< FM WRITES LOST (ring)" : "",
               (iolost != atrace_w_iolost) ? "  <<< I/O WRITES LOST (FIFO)" : "",
               (late != atrace_w_late) ? "  <<< OUTPUT UNDERRAN" : "",
               (cpu >= 90u) ? "  <<< AUDIO CORE SATURATED" : "");
    }

    atrace_w_dat = dat;
    atrace_w_kon = kon;
    atrace_w_koff = koff;
    atrace_w_drop = drop;
    atrace_w_late = late;
    atrace_w_bufs = bufs;
    atrace_w_iolost = iolost;
    atrace_w_psgw = psgw;
    (void)elapsed;

    if ((now - atrace_fm_summary_ms) >= ATRACE_FM_SUMMARY_MS)
    {
        atrace_fm_summary_ms = now;
        atrace_fm_summary();
    }
}

static void atrace_print_help(void)
{
    if (!ATRACE_LINK_UP())
        return;
    printf("\r\n[atrace] commands:\r\n"
           "  d  dump the event ring now\r\n"
           "  s  print counters + live PSG/SCC register state\r\n"
           "  r  print PSG write histogram per register\r\n"
           "  a  toggle capture of non-PSG events (I/O + SCC)\r\n"
           "  p  pause / resume capture\r\n"
           "  c  clear counters and ring\r\n"
           "  m  drop a marker into the ring (press when you hear the noise)\r\n"
           "  h  this help\r\n"
           "  (the MSX-MUSIC / FM-PAC log streams automatically - nothing to type)\r\n");
    fflush(stdout);
}

void atrace_check_stalls(PIO mem_pio, uint sm_mem_wr, PIO io_pio, uint sm_io_wr, uint sm_io_rd)
{
    uint32_t bit;

    // The I/O bus state machines only exist when PSG Mirror (or another I/O
    // consumer) initialised them; msx_io_bus is all-zero otherwise. Without
    // this guard the poll dereferences a NULL PIO, reads a ROM word as if it
    // were FDEBUG and reports a large, entirely fictitious stall count.
    if (mem_pio)
    {
        bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + sm_mem_wr);
        if (mem_pio->fdebug & bit)
        {
            mem_pio->fdebug = bit; // write-1-to-clear
            atrace_cnt.mem_stalls++;
            atrace_put(ATRACE_EV_MEM_STALL, (uint16_t)sm_mem_wr, 0u, 0u);
            atrace_trigger = ATRACE_TRIG_MEMSTALL;
        }
    }

    if (!io_pio)
        return;

    bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + sm_io_wr);
    if (io_pio->fdebug & bit)
    {
        io_pio->fdebug = bit;
        atrace_cnt.io_stalls++;
        atrace_put(ATRACE_EV_IO_STALL, (uint16_t)sm_io_wr, 0u, 0u);
        atrace_trigger = ATRACE_TRIG_IOSTALL;
    }

    bit = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm_io_rd);
    if (io_pio->fdebug & bit)
    {
        io_pio->fdebug = bit;
        atrace_cnt.io_rd_stalls++;
    }
}

void atrace_poll(void)
{
    if (!ATRACE_LINK_UP())
        return;

    int ch = getchar_timeout_us(0);
    if (ch != PICO_ERROR_TIMEOUT)
    {
        switch (ch)
        {
        case 'd': atrace_dump("manual"); break;
        case 's':
            atrace_print_counters();
            if (atrace_state_cb)
                atrace_state_cb();
            break;
        case 'r': atrace_print_psg_histogram(); break;
        case 'a':
            atrace_psg_only = !atrace_psg_only;
            printf("[atrace] non-PSG events %s\r\n", atrace_psg_only ? "OFF" : "ON");
            break;
        case 'p':
            atrace_capture = !atrace_capture;
            printf("[atrace] capture %s\r\n", atrace_capture ? "RUNNING" : "PAUSED");
            break;
        case 'c':
            atrace_init();
            printf("[atrace] counters and ring cleared\r\n");
            break;
        case 'm':
            atrace_put(ATRACE_EV_MARK, 0u, 0u, 0u);
            printf("[atrace] marker inserted\r\n");
            break;
        case 'h':
        case '?': atrace_print_help(); break;
        default: break;
        }
        fflush(stdout);
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());

    uint32_t reason = atrace_trigger;
    if (reason != ATRACE_TRIG_NONE)
    {
        atrace_trigger = ATRACE_TRIG_NONE;
        // While the MSX-MUSIC log is streaming, the legacy SCC/PSG trigger dump
        // must stay out of the way. It prints hundreds of lines over CDC, which
        // blocks this core long enough to overflow the very I/O FIFO it is
        // reporting on - the dump then re-triggers itself, floods the capture
        // and manufactures the underruns it claims to have found.
        if (!atrace_cnt.fm_buffers && (now - atrace_last_dump_ms) >= ATRACE_DUMP_MIN_INTERVAL_MS)
        {
            atrace_last_dump_ms = now;
            atrace_dumps_emitted++;
            printf("\r\n[atrace] *** TRIGGER: %s ***\r\n", atrace_trigger_name(reason));
            atrace_print_counters();
            atrace_dump(atrace_trigger_name(reason));
            if (atrace_state_cb)
                atrace_state_cb();
        }
    }

    // MSX-MUSIC streams its own log on a timer, so a capture needs nothing but
    // a connected terminal. It only starts once the FM output core is running.
    if (atrace_cnt.fm_buffers)
        atrace_fm_stream(now);

    if ((now - atrace_last_heartbeat_ms) >= ATRACE_HEARTBEAT_MS)
    {
        atrace_last_heartbeat_ms = now;
        // The SCC/PSG bus counters are only interesting while the FM log is
        // not running; otherwise they just dilute it.
        if (!atrace_cnt.fm_buffers)
            atrace_print_counters();
    }
}

#endif // EXPLORER_AUDIO_TRACE
