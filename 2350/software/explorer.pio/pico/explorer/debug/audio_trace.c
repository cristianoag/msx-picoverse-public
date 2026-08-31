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
volatile bool atrace_capture = true;
volatile bool atrace_psg_only = false;
volatile uint8_t atrace_psg_sel = 0xFFu;
volatile uint32_t atrace_trigger = ATRACE_TRIG_NONE;

static void (*atrace_state_cb)(void) = NULL;
static uint32_t atrace_last_heartbeat_ms;
static uint32_t atrace_last_dump_ms;
static uint32_t atrace_dumps_emitted;

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
    atrace_psg_sel = 0xFFu;
    atrace_trigger = ATRACE_TRIG_NONE;
    atrace_last_heartbeat_ms = to_ms_since_boot(get_absolute_time());
    atrace_last_dump_ms = atrace_last_heartbeat_ms;
    atrace_dumps_emitted = 0u;
}

void atrace_set_state_cb(void (*cb)(void))
{
    atrace_state_cb = cb;
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
           "  h  this help\r\n");
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
        if ((now - atrace_last_dump_ms) >= ATRACE_DUMP_MIN_INTERVAL_MS)
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

    if ((now - atrace_last_heartbeat_ms) >= ATRACE_HEARTBEAT_MS)
    {
        atrace_last_heartbeat_ms = now;
        atrace_print_counters();
    }
}

#endif // EXPLORER_AUDIO_TRACE
