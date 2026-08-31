// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// audio_trace.h - Low-overhead bus/audio instrumentation for the SCC + PSG
//                 mirror investigation (USB CDC diagnostics build only).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/
//
// The tracer records bus events into a per-core ring buffer and prints them
// over USB CDC on demand.  Each core owns its own ring, so a producer only
// ever does a plain increment on a variable no other core writes: no locks,
// no atomics, no cross-core exclusive monitor dependency.  The two rings are
// merged by timestamp when they are dumped.
//
// Everything compiles away to nothing unless EXPLORER_AUDIO_TRACE is set.

#ifndef _AUDIO_TRACE_H_
#define _AUDIO_TRACE_H_

#include <stdint.h>
#include <stdbool.h>

#ifndef EXPLORER_AUDIO_TRACE
#define EXPLORER_AUDIO_TRACE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if EXPLORER_AUDIO_TRACE

#include "hardware/pio.h"
#include "hardware/timer.h"
#include "pico/platform.h"

// Event kinds stored in the ring.
#define ATRACE_EV_PSG_SEL   1u  // arg=port (0xA0), data=register number selected
#define ATRACE_EV_PSG_DAT   2u  // arg=register number, data=value written
#define ATRACE_EV_IO_OTHER  3u  // arg=port, data=value (non-PSG I/O write)
#define ATRACE_EV_PSG_DROP  4u  // core0->core1 hand-off ring overflowed
#define ATRACE_EV_SCC_WR    5u  // arg=MSX address, data=value
#define ATRACE_EV_IO_STALL  6u  // I/O write captor stalled: bus writes were LOST
#define ATRACE_EV_MEM_STALL 7u  // memory write captor stalled: writes were LOST
#define ATRACE_EV_MARK      8u  // manual marker from the operator

#define ATRACE_RING_BITS 9u
#define ATRACE_RING_SIZE (1u << ATRACE_RING_BITS)
#define ATRACE_RING_MASK (ATRACE_RING_SIZE - 1u)

typedef struct
{
    uint32_t t;      // microseconds (raw timer low word)
    uint16_t addr;   // FULL 16-bit bus address (A0..A15) as captured
    uint8_t  type;
    uint8_t  data;   // D0..D7 as sampled from the bus
    uint8_t  aux;    // PSG: register selected at the time of the event
    uint8_t  pad[3];
} atrace_ev_t;

typedef struct
{
    uint32_t io_writes;       // every I/O write captured from the MSX bus
    uint32_t psg_io;          // I/O writes decoded as PSG (0xA0/0xA1)
    uint32_t psg_sel;         // register-select writes (port 0xA0)
    uint32_t psg_dat;         // data writes (port 0xA1)
    uint32_t psg_bad_sel;     // register-select with a value > 15
    uint32_t psg_dat_no_sel;  // data write before any register was ever selected
    uint32_t psg_ring_drops;  // core0 -> core1 hand-off ring full
    uint32_t scc_writes;      // SCC register/bank writes seen
    uint32_t scc_wave;        // .. of which wavetable
    uint32_t scc_freq;        // .. of which frequency
    uint32_t scc_vol;         // .. of which volume / channel enable
    uint32_t io_stalls;       // I/O write captor RX stall events
    uint32_t mem_stalls;      // memory write captor RX stall events
    uint32_t io_rd_stalls;    // I/O read responder TX stall events
    // Data-bus integrity evidence.  For "OUT (n),A" the Z80 drives the port on
    // A0..A7 and the accumulator - the value actually being written - on
    // A8..A15.  Comparing the sampled data bus against both tells us whether
    // D0..D7 carried the real byte, echoed the port (a stale operand fetch
    // left on an undriven bus), or matched the accumulator.
    uint32_t io_data_eq_port;  // D0..D7 == A0..A7  (stale bus: previous fetch)
    uint32_t io_data_eq_ahi;   // D0..D7 == A8..A15 (data bus carried the byte)
    uint32_t psg_ahi_valid;    // port A0 writes whose A8..A15 is a legal register
    uint32_t psg_reg_w[32];    // write count per PSG register
} atrace_counters_t;

extern atrace_counters_t atrace_cnt;
extern atrace_ev_t atrace_ring[2][ATRACE_RING_SIZE];
extern uint32_t atrace_head[2];
extern volatile bool atrace_capture;      // master capture enable
extern volatile bool atrace_psg_only;     // ring records PSG traffic only
extern volatile uint8_t atrace_psg_sel;   // currently selected PSG register
extern volatile uint32_t atrace_trigger;  // pending auto-dump reason (0 = none)

// Reasons reported by an automatic trigger.
#define ATRACE_TRIG_NONE     0u
#define ATRACE_TRIG_DROP     1u
#define ATRACE_TRIG_BADSEL   2u
#define ATRACE_TRIG_IOSTALL  3u
#define ATRACE_TRIG_MEMSTALL 4u

// ---------------------------------------------------------------------------
// Producer side - must stay cheap, these run inside the bus and audio loops.
// ---------------------------------------------------------------------------

static inline void __not_in_flash_func(atrace_put)(uint8_t type, uint16_t addr, uint8_t data, uint8_t aux)
{
    if (!atrace_capture)
        return;
    uint32_t core = get_core_num();
    uint32_t idx = atrace_head[core];
    atrace_ev_t *ev = &atrace_ring[core][idx & ATRACE_RING_MASK];
    ev->t = timer_hw->timerawl;
    ev->addr = addr;
    ev->type = type;
    ev->data = data;
    ev->aux = aux;
    atrace_head[core] = idx + 1u;
}

// Every I/O write captured from the MSX bus, PSG or not.  Takes the FULL 16-bit
// address so the caller's A8..A15 is preserved: on an "OUT (n),A" the Z80 puts
// the port on A0..A7 and the value being written on A8..A15, which gives us an
// independent copy of the data byte to check the sampled data bus against.
static inline void __not_in_flash_func(atrace_note_io_write)(uint16_t addr, uint8_t data)
{
    uint8_t port = (uint8_t)(addr & 0xFFu);
    uint8_t ahi  = (uint8_t)(addr >> 8);

    atrace_cnt.io_writes++;
    if (data == port)
        atrace_cnt.io_data_eq_port++;
    if (data == ahi)
        atrace_cnt.io_data_eq_ahi++;

    if (port == 0xA0u)
    {
        atrace_cnt.psg_io++;
        atrace_cnt.psg_sel++;
        if (data > 15u)
        {
            atrace_cnt.psg_bad_sel++;
            atrace_trigger = ATRACE_TRIG_BADSEL;
        }
        if (ahi <= 15u)
            atrace_cnt.psg_ahi_valid++;
        atrace_psg_sel = data;
        atrace_put(ATRACE_EV_PSG_SEL, addr, data, ahi);
        return;
    }

    if (port == 0xA1u)
    {
        atrace_cnt.psg_io++;
        atrace_cnt.psg_dat++;
        uint8_t reg = atrace_psg_sel;
        if (reg == 0xFFu)
            atrace_cnt.psg_dat_no_sel++;
        atrace_cnt.psg_reg_w[reg & 0x1Fu]++;
        atrace_put(ATRACE_EV_PSG_DAT, addr, data, reg);
        return;
    }

    if (!atrace_psg_only)
        atrace_put(ATRACE_EV_IO_OTHER, addr, data, ahi);
}

static inline void __not_in_flash_func(atrace_note_psg_drop)(void)
{
    atrace_cnt.psg_ring_drops++;
    atrace_trigger = ATRACE_TRIG_DROP;
    atrace_put(ATRACE_EV_PSG_DROP, 0u, 0u, 0u);
}

// SCC register map (standard SCC, base 0x9800): 0x800-0x87F wavetables,
// 0x880-0x889 frequency, 0x88A-0x88E volume, 0x88F channel enable.  The
// SCC+ (enhanced) layout shifts these to 0x8A0/0x8AA, so both are classified.
static inline void __not_in_flash_func(atrace_note_scc_write)(uint16_t addr, uint8_t data)
{
    atrace_cnt.scc_writes++;
    uint32_t rel = (uint32_t)(addr & 0x0FFFu);
    if (rel < 0x800u)
        ; // bank / enable register, not an audio register
    else if (rel <= 0x87Fu)
        atrace_cnt.scc_wave++;
    else if (rel <= 0x889u)
        atrace_cnt.scc_freq++;
    else if (rel <= 0x88Fu)
        atrace_cnt.scc_vol++;
    else if (rel <= 0x89Fu)
        atrace_cnt.scc_wave++;   // SCC+ fifth wavetable
    else if (rel <= 0x8A9u)
        atrace_cnt.scc_freq++;   // SCC+ frequency
    else if (rel <= 0x8AFu)
        atrace_cnt.scc_vol++;    // SCC+ volume / channel enable
    if (!atrace_psg_only)
        atrace_put(ATRACE_EV_SCC_WR, addr, data, 0u);
}

// ---------------------------------------------------------------------------
// Consumer side - called from core1 once per audio buffer, never per sample.
// ---------------------------------------------------------------------------

void atrace_init(void);
void atrace_set_state_cb(void (*cb)(void));
void atrace_check_stalls(PIO mem_pio, uint sm_mem_wr, PIO io_pio, uint sm_io_wr, uint sm_io_rd);
void atrace_poll(void);
void atrace_dump(const char *reason);

#else // !EXPLORER_AUDIO_TRACE

#define atrace_note_io_write(port, data)   ((void)0)
#define atrace_note_psg_drop()             ((void)0)
#define atrace_note_scc_write(addr, data)  ((void)0)
#define atrace_init()                      ((void)0)
#define atrace_set_state_cb(cb)            ((void)0)
#define atrace_check_stalls(a, b, c, d, e) ((void)0)
#define atrace_poll()                      ((void)0)
#define atrace_dump(reason)                ((void)0)

#endif // EXPLORER_AUDIO_TRACE

#ifdef __cplusplus
}
#endif

#endif /* _AUDIO_TRACE_H_ */
