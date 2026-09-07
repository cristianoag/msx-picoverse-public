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

    // ----- YM2413 / FM-PAC (MSX-MUSIC profile) -----------------------------
    uint32_t opll_sel;         // register-select writes (port 7C / addr 7FF4)
    uint32_t opll_dat;         // data writes           (port 7D / addr 7FF5)
    uint32_t opll_dat_no_sel;  // data write before any register was selected
    uint32_t opll_bad_reg;     // select of a register the YM2413 does not have
    uint32_t opll_ring_drops;  // core0 -> core1 hand-off ring full: writes LOST
    uint32_t opll_io_writes;   // arrived through I/O ports 7C/7D
    uint32_t opll_mem_writes;  // arrived through the FM-PAC 7FF4/7FF5 aperture
    uint32_t opll_key_on;      // 2x: key-on transitions seen
    uint32_t opll_key_off;     // 2x: key-off transitions seen
    uint32_t opll_rhythm_w;    // writes to the rhythm register (0E)
    uint32_t fmpac_ctl_w;      // 7FF6 control-register writes
    uint32_t fmpac_bank_w;     // 7FF7 BIOS bank writes
    uint32_t fmpac_sram_key_w; // 5FFE/5FFF SRAM unlock-key writes

    // ----- MSX-MUSIC output stage ------------------------------------------
    uint32_t fm_buffers;       // audio buffers filled by core1
    uint32_t fm_samples;       // samples generated
    uint32_t fm_limited;       // samples the soft limiter compressed
    uint32_t fm_clipped;       // samples pinned at the limiter ceiling
    uint32_t fm_starve;        // buffers that arrived late (output ran dry)
    int32_t  fm_peak;          // largest |mixed sample| since the last clear
    int32_t  fm_peak_fm;       // largest |FM contribution|
    int32_t  fm_peak_psg;      // largest |PSG contribution|
    // Where the per-sample time actually goes, in microseconds accumulated per
    // window. Guessing at this from the outside has repeatedly been wrong, so
    // the three costs that make up the audio loop are timed directly.
    uint32_t us_fm;            // inside OPLL_calc
    uint32_t us_psg;           // inside PSG_calc
    uint32_t us_io;            // inside msx_music_service_io
    // The mirrored PSG is summed into the FM mix through a gate that opens only
    // while a PSG channel is audible. Each transition kicks the ~35 Hz DC
    // blocker in front of the mixer, which then takes about 4.5 ms to settle,
    // so a rapidly flipping gate shows up as a wandering level. These count how
    // often that happens.
    uint32_t psg_gate_on;      // gate opened (PSG became audible)
    uint32_t psg_gate_off;     // gate closed (PSG went silent)
    uint32_t psg_gate_samples; // samples rendered with the gate open
} atrace_counters_t;

// Accumulated over one reporting window (100 ms) and emitted as a single line,
// so the whole MSX-MUSIC log streams on its own with nothing to type.
typedef struct
{
    int32_t  peak;
    int32_t  fm_peak;
    int32_t  psg_peak;
    uint32_t limited;
    uint64_t sumsq;
    uint32_t n;
    uint32_t psg_on;
    uint32_t psg_flip;
    uint8_t  psg_prev;   // PSG gate state at the previous sample
    uint8_t  psg_seen;   // whether psg_prev holds a real value yet
} atrace_fm_acc_t;

extern atrace_counters_t atrace_cnt;
extern atrace_ev_t atrace_ring[2][ATRACE_RING_SIZE];
extern uint32_t atrace_head[2];
extern atrace_fm_acc_t atrace_fm_acc;
extern uint32_t atrace_fm_busy_us;
extern volatile bool atrace_capture;      // master capture enable
extern volatile bool atrace_psg_only;     // ring records PSG traffic only
extern volatile uint8_t atrace_psg_sel;   // currently selected PSG register
extern volatile uint8_t atrace_opll_sel;  // currently selected YM2413 register
extern volatile uint16_t atrace_opll_key_state; // per-channel key-on state, for transition counting
extern volatile uint32_t atrace_trigger;  // pending auto-dump reason (0 = none)

// Reasons reported by an automatic trigger.
#define ATRACE_TRIG_NONE     0u
#define ATRACE_TRIG_DROP     1u
#define ATRACE_TRIG_BADSEL   2u
#define ATRACE_TRIG_IOSTALL  3u
#define ATRACE_TRIG_MEMSTALL 4u
#define ATRACE_TRIG_OPLLDROP 5u

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
// YM2413 / FM-PAC producers
// ---------------------------------------------------------------------------

// A YM2413 register write as it arrived from the MSX. `bus_addr` is the real
// bus address so the log distinguishes the two routes into the chip without a
// separate flag: 007C/007D are the I/O ports, 7FF4/7FF5 the FM-PAC memory
// aperture. Called before the write is applied (or queued), so the timestamp is
// the moment the MSX issued it.
static inline void __not_in_flash_func(atrace_note_opll_write)(uint16_t bus_addr, bool is_data, uint8_t data)
{
    if (bus_addr >= 0x4000u)
        atrace_cnt.opll_mem_writes++;
    else
        atrace_cnt.opll_io_writes++;

    if (!is_data)
    {
        atrace_cnt.opll_sel++;
        // The YM2413 has 00-07 (custom instrument), 0E (rhythm), 0F (test),
        // 10-18 (F-number low), 20-28 (key/block/F-number high) and 30-38
        // (instrument/volume). Anything else is a register that does not exist.
        bool valid = (data <= 0x0Fu) ||
                     (data >= 0x10u && data <= 0x18u) ||
                     (data >= 0x20u && data <= 0x28u) ||
                     (data >= 0x30u && data <= 0x38u);
        if (!valid)
            atrace_cnt.opll_bad_reg++;
        atrace_opll_sel = data;
        return;
    }

    atrace_cnt.opll_dat++;
    uint8_t reg = atrace_opll_sel;
    if (reg == 0xFFu)
        atrace_cnt.opll_dat_no_sel++;

    if (reg == 0x0Eu)
        atrace_cnt.opll_rhythm_w++;
    else if (reg >= 0x20u && reg <= 0x28u)
    {
        // Bit 4 of 20-28 is key-on. Counting the transitions tells us whether
        // the music driver is still triggering notes, which is the difference
        // between "the FM is playing and sounds wrong" and "the FM stopped
        // being driven".
        uint16_t bit = (uint16_t)(1u << (reg - 0x20u));
        if (data & 0x10u)
        {
            if (!(atrace_opll_key_state & bit))
            {
                atrace_opll_key_state |= bit;
                atrace_cnt.opll_key_on++;
            }
        }
        else if (atrace_opll_key_state & bit)
        {
            atrace_opll_key_state &= (uint16_t)~bit;
            atrace_cnt.opll_key_off++;
        }
    }
}

// The core0 -> core1 hand-off ring is full, so this register write never
// reaches the emulator. Dropped writes are the classic cause of a note that
// never stops or a volume that never comes back down.
static inline void __not_in_flash_func(atrace_note_opll_drop)(void)
{
    atrace_cnt.opll_ring_drops++;
}

static inline void __not_in_flash_func(atrace_note_fmpac_ctl)(uint16_t addr, uint8_t data)
{
    (void)data;
    if (addr == 0x7FF6u)
        atrace_cnt.fmpac_ctl_w++;
    else if (addr == 0x7FF7u)
        atrace_cnt.fmpac_bank_w++;
    else
        atrace_cnt.fmpac_sram_key_w++;
}

static inline void __not_in_flash_func(atrace_note_fm_starve)(void)
{
    atrace_cnt.fm_starve++;
}

// Time core1 actually spent generating audio, accumulated per window. This is
// the metric that says whether the audio core is saturated: a buffer is 5149 us
// of audio, so if filling it takes anywhere near that, there is no headroom
// left and the output will start breaking up.
static inline void __not_in_flash_func(atrace_note_fm_busy)(uint32_t us)
{
    atrace_fm_busy_us += us;
}

// Component timers. Each wraps one call in the per-sample path so the window
// line can report where the budget is being spent rather than only that it ran
// out. The timer read is a couple of cycles of overhead against calls that cost
// hundreds, so the breakdown stays representative.
#define ATRACE_TIME_CALL(accum, expr)                       \
    do {                                                    \
        uint32_t _t0 = timer_hw->timerawl;                  \
        (expr);                                             \
        (accum) += timer_hw->timerawl - _t0;                \
    } while (0)

#define ATRACE_TIME_FM(expr)  ATRACE_TIME_CALL(atrace_cnt.us_fm, expr)
#define ATRACE_TIME_PSG(expr) ATRACE_TIME_CALL(atrace_cnt.us_psg, expr)
#define ATRACE_TIME_IO(expr)  ATRACE_TIME_CALL(atrace_cnt.us_io, expr)

// ---------------------------------------------------------------------------
// MSX-MUSIC output-stage metering (per sample, integer only)
// ---------------------------------------------------------------------------

static inline void __not_in_flash_func(atrace_note_fm_sample)(int32_t fm, int32_t psg,
                                                              int32_t mixed, bool limited,
                                                              bool psg_audible)
{
    if (!atrace_capture)
        return;

    int32_t a = mixed < 0 ? -mixed : mixed;
    int32_t f = fm < 0 ? -fm : fm;
    int32_t p = psg < 0 ? -psg : psg;

    if (a >= 32767)
        atrace_cnt.fm_clipped++;

    if (a > atrace_fm_acc.peak)     atrace_fm_acc.peak = a;
    if (f > atrace_fm_acc.fm_peak)  atrace_fm_acc.fm_peak = f;
    if (p > atrace_fm_acc.psg_peak) atrace_fm_acc.psg_peak = p;
    atrace_fm_acc.sumsq += (uint64_t)a * (uint64_t)a;
    atrace_fm_acc.n++;
    if (limited)
        atrace_fm_acc.limited++;

    // PSG mirror gate. A transition here is what re-excites the DC blocker in
    // msx_music_calc_psg_sample(), so counting them separates "the PSG is
    // genuinely playing" from "the gate is chattering".
    uint8_t on = psg_audible ? 1u : 0u;
    if (on)
    {
        atrace_fm_acc.psg_on++;
        atrace_cnt.psg_gate_samples++;
    }
    if (atrace_fm_acc.psg_seen && on != atrace_fm_acc.psg_prev)
    {
        atrace_fm_acc.psg_flip++;
        if (on) atrace_cnt.psg_gate_on++;
        else    atrace_cnt.psg_gate_off++;
    }
    atrace_fm_acc.psg_prev = on;
    atrace_fm_acc.psg_seen = 1u;
}

// ---------------------------------------------------------------------------
// Consumer side - called from core1 once per audio buffer, never per sample.
// ---------------------------------------------------------------------------

void atrace_init(void);
void atrace_set_state_cb(void (*cb)(void));
void atrace_set_opll_state_cb(void (*cb)(void));
void atrace_fm_buffer_done(void);
void atrace_check_stalls(PIO mem_pio, uint sm_mem_wr, PIO io_pio, uint sm_io_wr, uint sm_io_rd);
void atrace_poll(void);
void atrace_dump(const char *reason);

#else // !EXPLORER_AUDIO_TRACE

#define atrace_note_io_write(port, data)   ((void)0)
#define atrace_note_psg_drop()             ((void)0)
#define atrace_note_scc_write(addr, data)  ((void)0)
#define atrace_note_opll_write(a, d, v)    do { (void)(a); (void)(d); (void)(v); } while (0)
#define atrace_note_opll_drop()            ((void)0)
#define atrace_note_fmpac_ctl(addr, data)  do { (void)(addr); (void)(data); } while (0)
#define atrace_note_fm_starve()            ((void)0)
#define atrace_note_fm_busy(us)            do { (void)(us); } while (0)
#define ATRACE_TIME_FM(expr)               (expr)
#define ATRACE_TIME_PSG(expr)              (expr)
#define ATRACE_TIME_IO(expr)               (expr)
#define atrace_note_fm_sample(f, p, m, l, a) \
    do { (void)(f); (void)(p); (void)(m); (void)(l); (void)(a); } while (0)
#define atrace_fm_buffer_done()            ((void)0)
#define atrace_init()                      ((void)0)
#define atrace_set_state_cb(cb)            ((void)0)
#define atrace_set_opll_state_cb(cb)       ((void)0)
#define atrace_check_stalls(a, b, c, d, e) ((void)0)
#define atrace_poll()                      ((void)0)
#define atrace_dump(reason)                ((void)0)

#endif // EXPLORER_AUDIO_TRACE

#ifdef __cplusplus
}
#endif

#endif /* _AUDIO_TRACE_H_ */
