// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// emuy8950.cpp - ymfm Yamaha Y8950 (MSX-AUDIO) wrapper for the standalone
//                MSX-AUDIO cartridge firmware
//
// The Y8950 is the OPL1 FM core plus an ADPCM-B (delta-T) engine with an
// external sample RAM. This wrapper hosts the BSD-licensed ymfm y8950 core and
// provides the four services ymfm expects from its host:
//
//   * FM timers        - ymfm reports timer durations in Y8950 master clocks;
//                        exactly 72 master clocks elapse per generated output
//                        sample (output rate = clock / (18 operators * 4
//                        prescale)), so the timers are advanced by 72 per
//                        Y8950_Clock() call. Getting this wrong makes
//                        timer-paced MSX-AUDIO replayers run at the wrong
//                        tempo (or hang waiting for a tick that never comes).
//   * /INT             - a timer overflow raises the maskable interrupt, which
//                        the firmware routes to the MSX /INT line.
//   * ADPCM sample RAM - openMSX Boosted_audio.xml declares 256 kB of sample
//                        RAM; it lives in the cartridge's external PSRAM.
//   * Y8950 I/O pins   - the Philips Music Module wires the four Y8950 I/O
//                        pins to the DAC enable (IO0) and the sound enable
//                        (IO3). openMSX reads them back as 0b1000, which is
//                        what this wrapper returns as well.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include "emuy8950.h"

#include "ymfm/ymfm_opl.h"

#include <cstdint>
#include <cstring>

namespace
{

// Y8950 master clocks per generated output sample. ymfm computes FM timer
// durations as period * OPERATORS(18) * clock_prescale(4), and the output rate
// is clock / (18 * 4), so one output sample is exactly 72 master clocks.
constexpr int32_t Y8950_TIMER_CLOCKS_PER_SAMPLE = 72;

// NOTE ON OUTPUT LEVEL - do not add a post-gain here.
//
// ymfm's y8950::generate() finishes with output->roundtrip_fp(), which models
// the external YM3014 DAC: it CLAMPS to +/-32767 and then quantises to a 10.3
// floating-point value. The sample this wrapper receives is therefore already a
// full-scale, already-clipped 16-bit signal. An earlier revision multiplied it
// by 1.5 to match the loudness of the other PicoVerse emulated sound profiles,
// which clipped a second time on loud passages: nine OPL channels summed at
// ymfm's rshift of 1 routinely reach full scale on their own, so the extra gain
// only added hard-clipping harmonics and made instruments sound noticeably
// different from openMSX (which mixes the Y8950 well below full scale - the
// Boosted_audio extension asks for <volume>12000</volume>). Unity gain is the
// correct choice for this chip.

void (*g_irq_line_cb)(bool asserted) = nullptr;

class PicoVerseY8950Interface final : public ymfm::ymfm_interface
{
public:
    void reset(uint8_t *sample_ram, uint32_t sample_ram_size)
    {
        m_timer_clocks[0] = -1;
        m_timer_clocks[1] = -1;
        m_irq_asserted = false;
        m_sample_ram = sample_ram;
        m_sample_ram_mask = (sample_ram_size != 0) ? (sample_ram_size - 1u) : 0u;
        if (m_sample_ram && sample_ram_size)
            std::memset(m_sample_ram, 0, sample_ram_size);
    }

    // Advance the FM timers by one generated output sample.
    void clock()
    {
        for (uint32_t timer = 0; timer < 2; ++timer)
        {
            if (m_timer_clocks[timer] >= 0)
            {
                m_timer_clocks[timer] -= Y8950_TIMER_CLOCKS_PER_SAMPLE;
                if (m_timer_clocks[timer] <= 0)
                {
                    m_timer_clocks[timer] = -1;
                    if (m_engine)
                        m_engine->engine_timer_expired(timer);
                }
            }
        }
    }

    void ymfm_set_timer(uint32_t timer, int32_t duration_in_clocks) override
    {
        if (timer < 2)
            m_timer_clocks[timer] = duration_in_clocks;
    }

    // Every host access is serviced synchronously while /WAIT freezes the Z80,
    // so from the MSX's point of view the chip completes instantly. Reporting a
    // busy window would only make host busy-polls spin on a bit that clears as
    // audio samples are generated, stalling the bus for no benefit.
    void ymfm_set_busy_end(uint32_t clocks) override { (void)clocks; }
    bool ymfm_is_busy() override { return false; }

    void ymfm_update_irq(bool asserted) override
    {
        m_irq_asserted = asserted;
        if (g_irq_line_cb)
            g_irq_line_cb(asserted);
    }

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
    {
        if (type == ymfm::ACCESS_ADPCM_B)
        {
            if (m_sample_ram == nullptr)
                return 0xff;
            return m_sample_ram[address & m_sample_ram_mask];
        }

        if (type == ymfm::ACCESS_IO)
        {
            // address 0 = the four Y8950 I/O pins. On the Philips Music Module
            // IO2-IO1 are unconnected and IO3/IO0 are outputs that read back as
            // '1'/'0' respectively, and openMSX additionally reports the unused
            // upper nibble as all ones (Y8950::peekReg case 0x19 returns
            // `(output & enable) | (input & ~enable) | 0xF0`).
            if (address == 0)
                return 0xf8;
            // address 1 = the MSX-AUDIO keyboard connector; none attached.
            return 0xff;
        }

        return 0xff;
    }

    void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override
    {
        if (type == ymfm::ACCESS_ADPCM_B)
        {
            if (m_sample_ram)
                m_sample_ram[address & m_sample_ram_mask] = data;
            return;
        }

        // ACCESS_IO writes drive the Music Module's sound-enable (IO3) and DAC
        // enable (IO0) pins. ymfm only passes the bits the host programmed as
        // outputs, without the direction mask, so the released-pin default
        // ('1' for IO3) cannot be reconstructed here. The cartridge therefore
        // keeps the Y8950 output permanently enabled - the safe direction,
        // since the alternative failure mode is silence.
        (void)address;
        (void)data;
    }

private:
    int32_t  m_timer_clocks[2] = { -1, -1 };
    bool     m_irq_asserted = false;
    uint8_t *m_sample_ram = nullptr;
    uint32_t m_sample_ram_mask = 0;
};

PicoVerseY8950Interface g_interface;
ymfm::y8950 g_y8950(g_interface);

inline int32_t clamp_i16(int32_t value)
{
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return value;
}

} // namespace

extern "C" void Y8950_SetIrqCallback(void (*cb)(bool asserted))
{
    g_irq_line_cb = cb;
}

extern "C" void Y8950_Reset(uint8_t *sample_ram, uint32_t sample_ram_size)
{
    g_interface.reset(sample_ram, sample_ram_size);
    g_y8950.reset();
}

extern "C" void Y8950_Write(uint32_t offset, uint8_t data)
{
    g_y8950.write(offset & 1u, data);
}

extern "C" uint8_t Y8950_Read(uint32_t offset)
{
    uint8_t value = g_y8950.read(offset & 1u);

    // Status port: on a real Y8950 bits 1 and 2 always read as 1 (openMSX
    // Y8950::peekStatus returns `(status & ...) | 0x06`). ymfm returns the raw
    // engine status for the y8950, unlike its ym3526/ym3812 cores which do OR
    // in 0x06, so add it here rather than patching the vendored core.
    if ((offset & 1u) == 0u)
        value |= 0x06u;

    return value;
}

extern "C" int32_t Y8950_Clock(void)
{
    ymfm::y8950::output_data sample;
    g_y8950.generate(&sample, 1);
    g_interface.clock();

    // Unity gain - see the note at the top of this file. roundtrip_fp() has
    // already clamped this to 16 bits; clamp_i16 is only a safety net.
    return clamp_i16(sample.data[0]);
}
