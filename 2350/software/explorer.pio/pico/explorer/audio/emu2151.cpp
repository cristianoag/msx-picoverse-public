#include "emu2151.h"

#include "ymfm/ymfm_opm.h"

#include <cstdint>

namespace
{
class ExplorerYm2151Interface final : public ymfm::ymfm_interface
{
public:
    void reset()
    {
        m_busy_clocks = 0;
        m_timer_clocks[0] = -1;
        m_timer_clocks[1] = -1;
        m_irq_asserted = false;
        m_ct = 0;
    }

    void clock()
    {
        if (m_busy_clocks > 0)
            --m_busy_clocks;

        for (uint32_t timer = 0; timer < 2; ++timer)
        {
            if (m_timer_clocks[timer] >= 0 && --m_timer_clocks[timer] <= 0)
            {
                m_timer_clocks[timer] = -1;
                if (m_engine)
                    m_engine->engine_timer_expired(timer);
            }
        }
    }

    virtual void ymfm_set_timer(uint32_t timer, int32_t duration_in_clocks) override
    {
        if (timer < 2)
            m_timer_clocks[timer] = duration_in_clocks;
    }

    virtual void ymfm_set_busy_end(uint32_t clocks) override
    {
        m_busy_clocks = clocks;
    }

    virtual bool ymfm_is_busy() override
    {
        return m_busy_clocks > 0;
    }

    virtual void ymfm_update_irq(bool asserted) override
    {
        m_irq_asserted = asserted;
    }

    virtual void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override
    {
        if (type == ymfm::ACCESS_IO && address == 0)
            m_ct = data & 0x03u;
    }

    bool irq_asserted() const { return m_irq_asserted; }
    uint8_t ct1() const { return (m_ct >> 1) & 0x01u; }
    uint8_t ct2() const { return m_ct & 0x01u; }

private:
    uint32_t m_busy_clocks = 0;
    int32_t m_timer_clocks[2] = {-1, -1};
    bool m_irq_asserted = false;
    uint8_t m_ct = 0;
};

ExplorerYm2151Interface g_interface;
ymfm::ym2151 g_ym2151_chip(g_interface);
ymfm::ym2164 g_ym2164_chip(g_interface);
bool g_use_ym2164 = false;
} // namespace

extern "C" void OPM_Clock(opm_t *chip, int32_t *output, uint8_t *sh1, uint8_t *sh2, uint8_t *so)
{
    ymfm::ym2151::output_data sample;
    if (g_use_ym2164)
        g_ym2164_chip.generate(&sample, 1);
    else
        g_ym2151_chip.generate(&sample, 1);
    g_interface.clock();

    if (chip)
    {
        chip->last_output[0] = sample.data[0];
        chip->last_output[1] = sample.data[1];
    }

    if (output)
    {
        output[0] = sample.data[0];
        output[1] = sample.data[1];
    }

    if (sh1)
        *sh1 = 0;
    if (sh2)
        *sh2 = 0;
    if (so)
        *so = 0;
}

extern "C" void OPM_Write(opm_t *chip, uint32_t port, uint8_t data)
{
    (void)chip;
    if (g_use_ym2164)
        g_ym2164_chip.write(port, data);
    else
        g_ym2151_chip.write(port, data);
}

extern "C" uint8_t OPM_Read(opm_t *chip, uint32_t port)
{
    (void)chip;
    return g_use_ym2164 ? g_ym2164_chip.read(port) : g_ym2151_chip.read(port);
}

extern "C" uint8_t OPM_ReadIRQ(opm_t *chip)
{
    (void)chip;
    return g_interface.irq_asserted() ? 1u : 0u;
}

extern "C" uint8_t OPM_ReadCT1(opm_t *chip)
{
    (void)chip;
    return g_interface.ct1();
}

extern "C" uint8_t OPM_ReadCT2(opm_t *chip)
{
    (void)chip;
    return g_interface.ct2();
}

extern "C" void OPM_SetIC(opm_t *chip, uint8_t ic)
{
    (void)chip;
    (void)ic;
}

extern "C" void OPM_Reset(opm_t *chip, uint32_t flags)
{
    g_use_ym2164 = (flags & opm_flags_ym2164) != 0;
    g_interface.reset();
    if (g_use_ym2164)
        g_ym2164_chip.reset();
    else
        g_ym2151_chip.reset();
    if (chip)
    {
        chip->last_output[0] = 0;
        chip->last_output[1] = 0;
    }
}
