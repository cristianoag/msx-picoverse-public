#include "emu2608.h"

#include "ym2608_adpcm_rom.h"
#include "ymfm/ymfm_opn.h"

#include <cstdint>
#include <cstring>
#include <new>

namespace
{
constexpr uint32_t OPNA_CLOCK = 8000000u;       // Makoto's oscillator
constexpr uint32_t OUTPUT_RATE = 44100u;

// OPL4_OUTPUT_STEP native FM periods are rendered per frame: with 2 the FM
// runs at 27.8 kHz (pitch and tempo unchanged, half the work) and is
// interpolated up to the 44.1 kHz stream.
#ifndef OPL4_OUTPUT_STEP
#define OPL4_OUTPUT_STEP 1
#endif
constexpr uint32_t FRAME_CLOCKS = 144u * OPL4_OUTPUT_STEP; // one native FM period: 24 operators x prescale 6
constexpr uint32_t FRAME_ACCUM = OUTPUT_RATE * FRAME_CLOCKS;

// The SSG output is the mean of the 166 kHz samples of a frame.
#ifndef OPNA_FM_GAIN_NUM
#define OPNA_FM_GAIN_NUM 3
#endif
#ifndef OPNA_FM_GAIN_SHIFT
#define OPNA_FM_GAIN_SHIFT 1
#endif
#ifndef OPNA_SSG_GAIN_NUM
#define OPNA_SSG_GAIN_NUM 3
#endif
#ifndef OPNA_SSG_GAIN_SHIFT
#define OPNA_SSG_GAIN_SHIFT 2
#endif

constexpr int32_t LIMIT_THRESHOLD = 26000;
constexpr int32_t LIMIT_KNEE = 12000;

class ExplorerYm2608Interface final : public ymfm::ymfm_interface
{
public:
    void reset(uint8_t *ram, uint32_t ram_size)
    {
        m_timer_clocks[0] = -1;
        m_timer_clocks[1] = -1;
        m_ram = ram;
        m_ram_mask = (ram && ram_size) ? ram_size - 1u : 0u;
    }

    void clock(int32_t clocks)
    {
        for (uint32_t timer = 0; timer < 2; ++timer)
        {
            if (m_timer_clocks[timer] >= 0)
            {
                m_timer_clocks[timer] -= clocks;
                if (m_timer_clocks[timer] <= 0)
                {
                    m_timer_clocks[timer] = -1;
                    if (m_engine)
                        m_engine->engine_timer_expired(timer);
                }
            }
        }
    }

    virtual void ymfm_set_timer(uint32_t timer, int32_t duration_in_clocks) override
    {
        if (timer < 2)
            m_timer_clocks[timer] = duration_in_clocks;
    }

    // The host is served synchronously, so the chip is never busy for it.
    virtual void ymfm_set_busy_end(uint32_t) override {}
    virtual bool ymfm_is_busy() override { return false; }

    // The Makoto does not route the chip IRQ to the MSX /INT line that
    // VGMPlay uses (it masks the timers and ADPCM IRQs).
    virtual void ymfm_update_irq(bool) override {}

    virtual uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
    {
        if (type == ymfm::ACCESS_ADPCM_A)
            return YM2608_ADPCM_ROM[address & (YM2608_ADPCM_ROM_SIZE - 1u)];
        if (type != ymfm::ACCESS_ADPCM_B || !m_ram)
            return 0;
        return m_ram[address & m_ram_mask];
    }

    virtual void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override
    {
        if (type == ymfm::ACCESS_ADPCM_B && m_ram)
            m_ram[address & m_ram_mask] = data;
    }

private:
    int32_t m_timer_clocks[2] = {-1, -1};
    uint8_t *m_ram = nullptr;
    uint32_t m_ram_mask = 0;
};

ExplorerYm2608Interface g_interface;
ymfm::ym2608 *g_chip = nullptr;
uint32_t g_accum = 0;
int32_t g_fm[2] = {0, 0};
int32_t g_ssg = 0;
int32_t g_prev_fm[2] = {0, 0};
int32_t g_prev_ssg = 0;
int32_t g_ssg_dc = 0;

inline int32_t soft_limit(int32_t sample)
{
    int32_t mag = (sample < 0) ? -sample : sample;
    if (mag <= LIMIT_THRESHOLD)
        return sample;

    int32_t over = mag - LIMIT_THRESHOLD;
    if (over > 0x40000)
        over = 0x40000;
    constexpr int32_t headroom = 32767 - LIMIT_THRESHOLD;
    int32_t limited = LIMIT_THRESHOLD + (headroom * over) / (over + LIMIT_KNEE);
    return (sample < 0) ? -limited : limited;
}

void render_frame()
{
    g_prev_fm[0] = g_fm[0];
    g_prev_fm[1] = g_fm[1];
    g_prev_ssg = g_ssg;
    g_chip->generate_frame(&g_fm[0], &g_fm[1], &g_ssg);
    g_interface.clock((int32_t)FRAME_CLOCKS);
}

inline int32_t lerp(int32_t from, int32_t to, int32_t t256)
{
    return from + (((to - from) * t256) >> 8);
}
} // namespace

extern "C" uint32_t OPNA_StateSize(void)
{
    return (uint32_t)sizeof(ymfm::ym2608);
}

extern "C" bool OPNA_Attach(void *memory, uint32_t size)
{
    if (!memory || size < sizeof(ymfm::ym2608) || ((uintptr_t)memory & (alignof(ymfm::ym2608) - 1u)) != 0)
        return false;
    g_chip = new (memory) ymfm::ym2608(g_interface);
    g_chip->set_fidelity(ymfm::OPN_FIDELITY_MIN);
    return true;
}

extern "C" void OPNA_Reset(uint8_t *adpcm_ram, uint32_t adpcm_ram_size)
{
    g_interface.reset(adpcm_ram, adpcm_ram_size);
    g_chip->reset();
    g_accum = 0;
    g_fm[0] = g_fm[1] = 0;
    g_prev_fm[0] = g_prev_fm[1] = 0;
    g_ssg = 0;
    g_prev_ssg = 0;
    g_ssg_dc = 0;
}

extern "C" void OPNA_Clock(int32_t *output)
{
    g_accum += OPNA_CLOCK;
    while (g_accum >= FRAME_ACCUM)
    {
        g_accum -= FRAME_ACCUM;
        render_frame();
    }

    // Interpolate between the last two frames (one frame of delay).
    int32_t t256 = (int32_t)(((g_accum >> 12) << 8) / (FRAME_ACCUM >> 12));
    if (t256 > 256)
        t256 = 256;
    int32_t fm_left = lerp(g_prev_fm[0], g_fm[0], t256);
    int32_t fm_right = lerp(g_prev_fm[1], g_fm[1], t256);
    int32_t ssg_raw = lerp(g_prev_ssg, g_ssg, t256);

    // The SSG output is unipolar; a ~55 Hz high-pass removes its pedestal so
    // volume changes do not thump through the amplifier.
    g_ssg_dc += (ssg_raw - g_ssg_dc) >> 7;
    int32_t ssg = ((ssg_raw - g_ssg_dc) * OPNA_SSG_GAIN_NUM) >> OPNA_SSG_GAIN_SHIFT;
    int32_t left = ((fm_left * OPNA_FM_GAIN_NUM) >> OPNA_FM_GAIN_SHIFT) + ssg;
    int32_t right = ((fm_right * OPNA_FM_GAIN_NUM) >> OPNA_FM_GAIN_SHIFT) + ssg;
    output[0] = soft_limit(left);
    output[1] = soft_limit(right);
}

extern "C" void OPNA_Write(uint32_t port, uint8_t data)
{
    g_chip->write(port, data);
}

extern "C" uint8_t OPNA_Read(uint32_t port)
{
    return g_chip->read(port);
}
