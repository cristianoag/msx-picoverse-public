// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// emuy8950.cpp - ymfm Yamaha Y8950 (MSX-AUDIO) wrapper for the Explorer firmware
//
// The Y8950 is the OPL1 FM core plus an ADPCM-B (delta-T) engine with an
// external sample RAM. This wrapper hosts the BSD-licensed ymfm y8950 core and
// provides the services ymfm expects from its host, like the standalone
// MSX-AUDIO firmware of the LoadROM tool:
//
//   * FM timers   - ymfm reports timer durations in Y8950 master clocks; exactly
//                   72 master clocks elapse per generated output sample (output
//                   rate = clock / (18 operators * 4 prescale) = 49716 Hz), so
//                   the timers are advanced by 72 per Y8950_Clock() call.
//   * /INT        - a timer overflow raises the maskable interrupt, which the
//                   firmware routes to the MSX /INT line.
//   * ADPCM RAM   - 256 KB of sample RAM, in the cartridge's external PSRAM.
//   * I/O pins    - the Philips Music Module wires the four Y8950 I/O pins to the
//                   DAC enable (IO0) and the sound enable (IO3); openMSX reads
//                   them back as 0b1000.
//
// The core runs at the native 49716 Hz (no OPL4_OUTPUT_STEP reduction): the step
// is a property of the register set (fm_output_step), and the OPL1 set the Y8950
// uses is always native rate.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include "emuy8950.h"

#include "ymfm/ymfm_opl.h"

#include <cstdint>
#include <cstring>
#include <new>

namespace
{

// Y8950 master clocks per generated output sample: ymfm computes FM timer
// durations as period * OPERATORS(18) * clock_prescale(4).
constexpr int32_t Y8950_TIMER_CLOCKS_PER_SAMPLE = 72;

// NOTE ON OUTPUT LEVEL - do not add a post-gain here. ymfm's y8950::generate()
// finishes with roundtrip_fp(), which models the external YM3014 DAC: it clamps
// to +/-32767 and quantises to a 10.3 floating-point value, so the sample is
// already full scale. A post-gain only added clipping (see the LoadROM log).

void (*g_irq_line_cb)(bool asserted) = nullptr;

class PicoVerseY8950Interface final : public ymfm::ymfm_interface
{
public:
    void reset(uint8_t *sample_ram, uint32_t sample_ram_size)
    {
        m_timer_clocks[0] = -1;
        m_timer_clocks[1] = -1;
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

    // Every host access is serviced while /WAIT freezes the Z80, so from the
    // MSX's point of view the chip completes instantly: no busy window.
    void ymfm_set_busy_end(uint32_t clocks) override { (void)clocks; }
    bool ymfm_is_busy() override { return false; }

    void ymfm_update_irq(bool asserted) override
    {
        if (g_irq_line_cb)
            g_irq_line_cb(asserted);
    }

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
    {
        if (type == ymfm::ACCESS_ADPCM_B)
            return m_sample_ram ? m_sample_ram[address & m_sample_ram_mask] : 0xffu;

        if (type == ymfm::ACCESS_IO)
        {
            // address 0 = the four Y8950 I/O pins: on the Philips Music Module
            // IO3/IO0 read back as '1'/'0' and openMSX reports the unused upper
            // nibble as ones. address 1 = the keyboard connector; none attached.
            return address == 0 ? 0xf8u : 0xffu;
        }

        return 0xffu;
    }

    void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override
    {
        if (type == ymfm::ACCESS_ADPCM_B && m_sample_ram)
            m_sample_ram[address & m_sample_ram_mask] = data;
        // ACCESS_IO writes drive the Music Module's sound-enable and DAC-enable
        // pins; the output is kept permanently enabled (the safe direction).
    }

private:
    int32_t  m_timer_clocks[2] = { -1, -1 };
    uint8_t *m_sample_ram = nullptr;
    uint32_t m_sample_ram_mask = 0;
};

PicoVerseY8950Interface g_interface;

// The y8950 object is built in firmware-lent memory (Y8950_Attach): Explorer's
// SRAM is full, so the chip lives in the borrowed OPLL table arena.
ymfm::y8950 *g_y8950 = nullptr;

inline int32_t clamp_i16(int32_t value)
{
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return value;
}

} // namespace

extern "C" uint32_t Y8950_StateSize(void)
{
    return (uint32_t)sizeof(ymfm::y8950);
}

extern "C" bool Y8950_Attach(void *memory, uint32_t size)
{
    if (!memory || size < sizeof(ymfm::y8950) || ((uintptr_t)memory & (alignof(ymfm::y8950) - 1u)) != 0)
        return false;
    g_y8950 = new (memory) ymfm::y8950(g_interface);
    return true;
}

extern "C" void Y8950_SetIrqCallback(void (*cb)(bool asserted))
{
    g_irq_line_cb = cb;
}

extern "C" void Y8950_Reset(uint8_t *sample_ram, uint32_t sample_ram_size)
{
    g_interface.reset(sample_ram, sample_ram_size);
    g_y8950->reset();
}

extern "C" void Y8950_Write(uint32_t offset, uint8_t data)
{
    g_y8950->write(offset & 1u, data);
}

extern "C" uint8_t Y8950_Read(uint32_t offset)
{
    uint8_t value = g_y8950->read(offset & 1u);

    // Status port: on a real Y8950 bits 1 and 2 always read as 1 (openMSX
    // Y8950::peekStatus). ymfm returns the raw engine status for the y8950.
    if ((offset & 1u) == 0u)
        value |= 0x06u;

    return value;
}

extern "C" int32_t Y8950_Clock(void)
{
    ymfm::y8950::output_data sample;
    g_y8950->generate(&sample, 1);
    g_interface.clock();

    return clamp_i16(sample.data[0]);
}
