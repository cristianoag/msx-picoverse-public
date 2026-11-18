#include "emu278b.h"

#include "ymfm/ymfm_opl.h"

#include <cstdint>
#include <cstring>
#include <new>

extern "C" {
#if defined(OPL4_USB_DEBUG) && OPL4_USB_DEBUG
// Debug counters (read by opl4_main.c when USB CDC debug is enabled). These
// reveal whether the host is actually arming the FM timers and how often they
// fire, which is the key to diagnosing FM-timer-paced replayer tempo.
volatile uint32_t opl4_dbg_timer_arm  = 0;   // ymfm_set_timer with a real period
volatile uint32_t opl4_dbg_timer_fire = 0;   // a timer reached zero and signalled
volatile uint32_t opl4_dbg_irq_assert = 0;   // ymfm_update_irq(true) (timer IRQ)

// Output-level diagnostics. The three YMF278B output pin pairs (DO0/DO1/DO2)
// are summed to one stereo pair in OPL4_Clock, gained, and then passed through
// a soft-knee limiter. These track, per dump interval, how often the limiter
// engaged and the peak magnitude of each DO pair and of the gained mix, to show
// whether an overload comes from the mix-scaled main output (DO2) or the
// full-scale aux outputs (DO0/DO1).
volatile uint32_t opl4_dbg_clip     = 0;   // samples (L or R) above the limiter knee
volatile uint32_t opl4_dbg_peak_raw = 0;   // max |post-gain mix| before limiting
volatile uint32_t opl4_dbg_peak_do0 = 0;   // max |DO0| (FM aux)
volatile uint32_t opl4_dbg_peak_do1 = 0;   // max |DO1| (PCM aux)
volatile uint32_t opl4_dbg_peak_do2 = 0;   // max |DO2| (main mix)

// Saturation actually happens one stage earlier, inside ymf278b::generate,
// where output->clamp16() limits DO2 - by the time OPL4_Clock sees the sample
// it is already clamped, so opl4_dbg_clip above always reads 0. These two are
// written by ymf278b::generate from the PRE-clamp value and are the real
// distortion indicator.
volatile uint32_t opl4_dbg_do2_clip = 0;   // pre-clamp DO2 samples out of range
volatile uint32_t opl4_dbg_do2_peak = 0;   // max |pre-clamp DO2|

// Polyphony actually rendered, published by the FM/PCM engines whenever they
// recompute their active-channel mask. `_or` accumulates every voice that was
// active at any point in the interval, `_max` is the largest simultaneous
// count; together they show whether a song's voices are reaching the synth.
volatile uint32_t opl4_dbg_fm_active_or   = 0;
volatile uint32_t opl4_dbg_fm_active_max  = 0;
volatile uint32_t opl4_dbg_pcm_active_or  = 0;
volatile uint32_t opl4_dbg_pcm_active_max = 0;

// Channels the HOST actually keyed on during the interval. Compared against
// opl4_dbg_pcm_active_or this separates "the MSX stopped playing that track"
// from "the key-on arrived but the voice never went active".
volatile uint32_t opl4_dbg_pcm_key_or = 0;

// Key-offs the host issued, and voices still sounding after being released.
// Together these tell a lost key-off apart from a release envelope that simply
// takes a long time to fade - both are heard as a "hanging note".
volatile uint32_t opl4_dbg_pcm_keyoff_or = 0;
volatile uint32_t opl4_dbg_pcm_rel_or = 0;
volatile uint32_t opl4_dbg_pcm_rev_or = 0;
#define OPL4_DBG_INC(counter) ((counter)++)
#else
#define OPL4_DBG_INC(counter) ((void)0)
#endif

// /INT line callback. The firmware (opl4_main.c) registers this to drive the
// MSX /INT pin (GPIO40) when the YMF278B FM timers raise/clear an interrupt;
// MoonSound replayers such as MoonBlaster pace song tempo from that interrupt.
typedef void (*opl4_irq_line_cb_t)(bool asserted);
static opl4_irq_line_cb_t opl4_irq_line_cb = nullptr;
void opl4_set_irq_line_callback(opl4_irq_line_cb_t cb) { opl4_irq_line_cb = cb; }
}

namespace
{
constexpr uint32_t OPL4_MEMORY_MASK = 0x3fffffu;

#ifndef OPL4_OUTPUT_STEP
#define OPL4_OUTPUT_STEP 1
#endif

// YMF278B master-clock cycles per generated output sample (output rate is
// input_clock / 768). FM timer durations from ymfm are in master clocks, so
// the interface advances the timers by this many clocks per generated sample.
constexpr int32_t OPL4_CLOCKS_PER_SAMPLE = 768 * OPL4_OUTPUT_STEP;

// Final output gain, as a NUM/2^SHIFT fraction of the summed DO0+DO1+DO2 pins.
//
// ymfm leaves the YMF278B at a level well below the rest of the PicoVerse audio
// profiles, which is why an OPL4 image needed far more amplifier volume than an
// SCC or PSG one to sound equally loud. ymfm marks its YMF278B mixing "needs
// verification" and applies no output normalisation to this chip (it does for
// others, e.g. the OPLL is scaled by 128/9), so its raw level is not a level
// anything was calibrated against.
//
// Calibrating against openMSX, whose wave part is matched to real MoonSound
// recordings, using the peak a single voice/channel can contribute to one
// output channel, expressed as a fraction of full scale:
//
//   openMSX MoonSound wave voice  32767 * (1/32768) * (17000/32767) -> 0.292
//   openMSX SCC channel             119 * (1/128)   * (9000/32767)  -> 0.102
//   openMSX PSG channel             1.0 * (1.0)     * (21000/32767) -> 0.255
//   (both openMSX figures include its 0.5625 master volume and the sqrt(0.5)
//    centre-pan factor a mono device gets; the MoonSound is stereo, so it does
//    not get that factor.)
//
// The explorer firmware normalises its emulated chips so the chip's own maximum
// fills the 16-bit output: an SCC channel peaks at 9600/5 * 109/32 = 6540
// (0.200 of full scale) and a PSG channel at 0xFF<<4<<2 = 16320 (0.498), i.e.
// both sit a consistent ~1.96x above openMSX's calibration. The OPL4 wave voice
// peaks at 8168 (0.249), or 0.85x openMSX - about 7 dB below the level the same
// hardware plays SCC and PSG music at, which is the reported difference.
//
// 96/32 = 3x lifts a wave voice to 0.75 of full scale, putting the OPL4 on the
// same footing as the other profiles (a little above it, since OPL4 music
// rarely drives its voices to maximum level the way SCC/PSG music does) while
// keeping a single maximum-level voice below the limiter knee. The FM/wave
// balance is untouched: ymfm's two parts already meet at the same peak level
// per channel that openMSX's do, so one gain applied to the mixed output keeps
// the balance the chip's own mix registers (0xF8/0xF9) ask for.
#ifndef OPL4_OUTPUT_GAIN_NUM
#define OPL4_OUTPUT_GAIN_NUM 96
#endif
#ifndef OPL4_OUTPUT_GAIN_SHIFT
#define OPL4_OUTPUT_GAIN_SHIFT 5
#endif

// Soft-knee limiter for the gained mix, the same shape the explorer firmware
// applies to its SCC + PSG sum. The YMF278B can key 24 wave voices and 18 FM
// channels at once, so a sum that exceeds full scale is normal on dense songs;
// hard-clipping it squares off sustained chords and is heard as harsh buzz.
// Below the threshold the signal passes through bit-exact, above it the excess
// is compressed along an asymptote that approaches but never reaches full
// scale.
constexpr int32_t OPL4_LIMIT_THRESHOLD = 26000;
constexpr int32_t OPL4_LIMIT_KNEE      = 12000;

class ExplorerYmf278bInterface final : public ymfm::ymfm_interface
{
public:
    void reset(const uint8_t *rom, uint32_t rom_size, uint8_t *sample_ram, uint32_t sample_ram_size)
    {
        m_busy_clocks = 0;
        m_timer_clocks[0] = -1;
        m_timer_clocks[1] = -1;
        m_irq_asserted = false;
        m_rom = rom;
        m_rom_size = rom_size;
        m_sample_ram = sample_ram;
        m_sample_ram_size = sample_ram_size;
        if (m_sample_ram && m_sample_ram_size)
            std::memset(m_sample_ram, 0, m_sample_ram_size);
    }

    void clock()
    {
        // Advance the FM timers. ymfm expresses timer durations (via
        // ymfm_set_timer) in YMF278B master-clock units: update_timer() sets
        // period * OPERATORS(36) * clock_prescale(19) = period * 684 clocks.
        // The chip output rate is input_clock / 768, so exactly 768 master
        // clocks elapse for every generated output sample, and clock() is
        // called once per generated sample. An earlier version decremented the
        // timers by 1 per sample instead of 768, running them 768x too slow -
        // which made FM-timer-paced MoonSound replayers (e.g. MoonBlaster)
        // crawl even though synthesis was real-time.
        for (uint32_t timer = 0; timer < 2; ++timer)
        {
            if (m_timer_clocks[timer] >= 0)
            {
                m_timer_clocks[timer] -= OPL4_CLOCKS_PER_SAMPLE;
                if (m_timer_clocks[timer] <= 0)
                {
                    m_timer_clocks[timer] = -1;
                    OPL4_DBG_INC(opl4_dbg_timer_fire);
                    if (m_engine)
                        m_engine->engine_timer_expired(timer);
                }
            }
        }
    }

    virtual void ymfm_set_timer(uint32_t timer, int32_t duration_in_clocks) override
    {
        if (timer < 2)
        {
            m_timer_clocks[timer] = duration_in_clocks;
            if (duration_in_clocks > 0)
                OPL4_DBG_INC(opl4_dbg_timer_arm);
        }
    }

    virtual void ymfm_set_busy_end(uint32_t clocks) override
    {
        // The standalone cartridge services every host access synchronously
        // while /WAIT freezes the Z80, so from the host's point of view the
        // chip completes instantly and is never busy. Tracking the busy window
        // would make MoonTest's `in a,(0C4h); rra; jr c` busy poll spin on a
        // bit that only clears as audio samples are generated, which can stall
        // the MSX bus. Ignore the busy window entirely.
        (void)clocks;
        m_busy_clocks = 0;
    }

    virtual bool ymfm_is_busy() override
    {
        return false;
    }

    virtual void ymfm_update_irq(bool asserted) override
    {
        m_irq_asserted = asserted;
        if (asserted)
            OPL4_DBG_INC(opl4_dbg_irq_assert);
        // Drive the MSX /INT line so FM-timer-paced replayers receive the
        // interrupt. The callback handles open-drain assert/release.
        if (opl4_irq_line_cb)
            opl4_irq_line_cb(asserted);
    }

    virtual uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
    {
        if (type != ymfm::ACCESS_PCM)
            return 0xff;

        address &= OPL4_MEMORY_MASK;
        if (address < m_rom_size && m_rom)
            return m_rom[address];

        uint32_t ram_address = address - m_rom_size;
        if (m_sample_ram && ram_address < m_sample_ram_size)
            return m_sample_ram[ram_address];

        return 0xff;
    }

    virtual void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override
    {
        if (type != ymfm::ACCESS_PCM)
            return;

        address &= OPL4_MEMORY_MASK;
        if (address < m_rom_size)
            return;

        uint32_t ram_address = address - m_rom_size;
        if (m_sample_ram && ram_address < m_sample_ram_size)
            m_sample_ram[ram_address] = data;
    }

private:
    uint32_t m_busy_clocks = 0;
    int32_t m_timer_clocks[2] = {-1, -1};
    bool m_irq_asserted = false;
    const uint8_t *m_rom = nullptr;
    uint32_t m_rom_size = 0;
    uint8_t *m_sample_ram = nullptr;
    uint32_t m_sample_ram_size = 0;
};

static inline int32_t clamp_i32_to_i16(int32_t value)
{
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return value;
}

// Compress anything above the knee toward full scale instead of clipping it.
static inline int32_t soft_limit_i16(int32_t sample)
{
    int32_t mag = (sample < 0) ? -sample : sample;
    if (mag <= OPL4_LIMIT_THRESHOLD)
        return sample;

    int32_t over = mag - OPL4_LIMIT_THRESHOLD;
    // Cap the excess so the 32-bit product below cannot overflow at any build
    // gain; everything past this point maps to within one LSB of full scale.
    if (over > 0x40000)
        over = 0x40000;
    constexpr int32_t headroom = 32767 - OPL4_LIMIT_THRESHOLD;
    int32_t limited = OPL4_LIMIT_THRESHOLD + (headroom * over) / (over + OPL4_LIMIT_KNEE);
    return (sample < 0) ? -limited : limited;
}

ExplorerYmf278bInterface g_interface;

// The YMF278B state (about 17 KB) is not a static: Explorer's SRAM is full, so
// the firmware lends the OPLL table arena and the chip is built in it with
// placement new (OPL4_Attach) when the OPL4 profile is launched.
ymfm::ymf278b *g_ymf278b = nullptr;
} // namespace

extern "C" uint32_t OPL4_StateSize(void)
{
    return (uint32_t)sizeof(ymfm::ymf278b);
}

extern "C" bool OPL4_Attach(void *memory, uint32_t size)
{
    if (!memory || size < sizeof(ymfm::ymf278b) || ((uintptr_t)memory & (alignof(ymfm::ymf278b) - 1u)) != 0)
        return false;
    g_ymf278b = new (memory) ymfm::ymf278b(g_interface);
    return true;
}

extern "C" void OPL4_Reset(opl4_t *chip, const uint8_t *rom, uint32_t rom_size, uint8_t *sample_ram, uint32_t sample_ram_size)
{
    g_interface.reset(rom, rom_size, sample_ram, sample_ram_size);
    g_ymf278b->reset();
    if (chip)
    {
        chip->last_output[0] = 0;
        chip->last_output[1] = 0;
        chip->pcm_address = 0;
    }
}

extern "C" void OPL4_Clock(opl4_t *chip, int32_t *output)
{
    ymfm::ymf278b::output_data sample;
    g_ymf278b->generate(&sample, 1);
    g_interface.clock();

    int32_t sum_l = ((sample.data[0] + sample.data[2] + sample.data[4]) * OPL4_OUTPUT_GAIN_NUM) >> OPL4_OUTPUT_GAIN_SHIFT;
    int32_t sum_r = ((sample.data[1] + sample.data[3] + sample.data[5]) * OPL4_OUTPUT_GAIN_NUM) >> OPL4_OUTPUT_GAIN_SHIFT;

#if defined(OPL4_USB_DEBUG) && OPL4_USB_DEBUG
    {
        auto absu = [](int32_t v) -> uint32_t { return (uint32_t)(v < 0 ? -v : v); };
        uint32_t a;
        a = absu(sample.data[4]) > absu(sample.data[5]) ? absu(sample.data[4]) : absu(sample.data[5]);
        if (a > opl4_dbg_peak_do2) opl4_dbg_peak_do2 = a;
        a = absu(sum_l) > absu(sum_r) ? absu(sum_l) : absu(sum_r);
        if (a > opl4_dbg_peak_raw) opl4_dbg_peak_raw = a;
        if (a > (uint32_t)OPL4_LIMIT_THRESHOLD)
            opl4_dbg_clip++;
    }
#endif

    int32_t left = clamp_i32_to_i16(soft_limit_i16(sum_l));
    int32_t right = clamp_i32_to_i16(soft_limit_i16(sum_r));

    if (chip)
    {
        chip->last_output[0] = left;
        chip->last_output[1] = right;
    }

    if (output)
    {
        output[0] = left;
        output[1] = right;
    }
}

extern "C" void OPL4_Write(opl4_t *chip, uint32_t port, uint8_t data)
{
    if (chip && ((port & 7u) == 4u))
        chip->pcm_address = data;
    g_ymf278b->write(port, data);
}

extern "C" uint8_t OPL4_Read(opl4_t *chip, uint32_t port)
{
    // The YMF278B PCM device-ID is read back through wave register 0x02 with
    // bits 7-5 reading 0x20. ymfm already ORs 0x20 into that read inside
    // read_data_pcm(), so the value must be passed through untouched: masking
    // it (as an earlier revision did) stripped the device ID and made MoonTest
    // report "Unknown".
    (void)chip;
    return g_ymf278b->read(port);
}
