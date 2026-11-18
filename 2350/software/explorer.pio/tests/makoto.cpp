// Host test for the Makoto YM2608 (OPNA) emulation core (audio/emu2608.cpp).
#include "../pico/explorer/audio/emu2608.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            std::printf("FAIL: " __VA_ARGS__);            \
            std::printf("\n");                            \
            std::exit(1);                                 \
        }                                                 \
    } while (0)

static void fm1(uint8_t reg, uint8_t data) { OPNA_Write(0, reg); OPNA_Write(1, data); }
static void fm2(uint8_t reg, uint8_t data) { OPNA_Write(2, reg); OPNA_Write(3, data); }

struct Stats { int32_t peak; double hz; int32_t nonzero; unsigned last_audible; };

static Stats render(unsigned samples)
{
    Stats s = {0, 0.0, 0, 0};
    std::vector<int32_t> v(samples);
    double sum = 0;
    for (unsigned i = 0; i < samples; ++i)
    {
        int32_t out[2];
        OPNA_Clock(out);
        v[i] = out[0];
        sum += out[0];
        if (std::abs(out[0]) > s.peak) s.peak = std::abs(out[0]);
        if (out[0] != 0) ++s.nonzero;
        if (std::abs(out[0]) > 200) s.last_audible = i;
    }
    double mean = sum / samples;
    unsigned crossings = 0;
    for (unsigned i = 1; i < samples; ++i)
        if (v[i - 1] < mean && v[i] >= mean) ++crossings;
    s.hz = crossings * 44100.0 / samples;
    return s;
}

int main()
{
    alignas(16) static uint8_t arena[64 * 1024];
    static uint8_t ram[256 * 1024];
    CHECK(OPNA_StateSize() <= sizeof(arena), "chip state %u bytes", OPNA_StateSize());
    CHECK(OPNA_Attach(arena, sizeof(arena)), "attach");
    OPNA_Reset(ram, sizeof(ram));
    std::printf("chip state: %u bytes\n", OPNA_StateSize());

    // VGMPlay's Makoto_Detect sequence: status 0, ADPCM start -> 0x20, stop.
    CHECK((OPNA_Read(2) & 0x3F) == 0, "status after reset 0x%02X", OPNA_Read(2));
    fm2(0x00, 0x80);
    CHECK((OPNA_Read(2) & 0x3F) == 0x20, "status after ADPCM start 0x%02X", OPNA_Read(2));
    fm2(0x00, 0x00);
    CHECK((OPNA_Read(2) & 0x80) == 0, "never busy");

    // FM: channel 1, algorithm 7, operator 4 only, A440.
    OPNA_Reset(ram, sizeof(ram));
    fm1(0x29, 0x80);               // six-voice mode
    fm1(0xB0, 0x07);               // algorithm 7, no feedback
    fm1(0xB4, 0xC0);               // left + right
    fm1(0x40, 0x7F); fm1(0x44, 0x7F); fm1(0x48, 0x7F);
    fm1(0x3C, 0x01);               // multiple x1
    fm1(0x4C, 0x00);               // operator 4 total level 0
    fm1(0x5C, 0x1F);               // attack rate 31
    fm1(0x7C, 0x00); fm1(0x8C, 0x0F);
    fm1(0xA4, (4 << 3) | (1038 >> 8)); fm1(0xA0, 1038 & 0xFF);
    fm1(0x28, 0xF0);               // key on
    render(2000);
    Stats fm = render(44100);
    std::printf("FM: peak %d, %.1f Hz\n", fm.peak, fm.hz);
    CHECK(fm.peak > 4000, "FM audible");
    CHECK(std::fabs(fm.hz - 440.0) < 8.0, "FM pitch %.1f", fm.hz);
    fm1(0x28, 0x00);

    // SSG: channel A tone, period 284 -> 440 Hz at the 2 MHz SSG clock.
    OPNA_Reset(ram, sizeof(ram));
    fm1(0x00, 284 & 0xFF); fm1(0x01, 284 >> 8);
    fm1(0x07, 0x3E);
    fm1(0x08, 0x0F);
    render(2000);
    Stats ssg = render(44100);
    std::printf("SSG: peak %d, %.1f Hz\n", ssg.peak, ssg.hz);
    CHECK(ssg.peak > 2000, "SSG audible");
    CHECK(std::fabs(ssg.hz - 440.0) < 8.0, "SSG pitch %.1f", ssg.hz);

    // ADPCM-B data upload the way VGMPlay does it (external RAM, record mode).
    OPNA_Reset(ram, sizeof(ram));
    fm2(0x01, 0xC0);               // 1-bit DRAM, panpot
    fm2(0x10, 0x14);               // mask EOS/ZERO, keep BRDY
    fm2(0x02, 0x00); fm2(0x03, 0x00);
    fm2(0x04, 0xFF); fm2(0x05, 0xFF);
    fm2(0x00, 0x01);
    fm2(0x00, 0x60);
    OPNA_Write(2, 0x08);
    for (int i = 0; i < 16; ++i)
    {
        CHECK((OPNA_Read(2) & 0x08) != 0, "BRDY while uploading");
        OPNA_Write(3, (uint8_t)(0xA0 + i));
    }
    fm2(0x00, 0x01);
    for (int i = 0; i < 16; ++i)
        CHECK(ram[i] == 0xA0 + i, "ADPCM RAM[%d]=0x%02X", i, ram[i]);

    // FM timer A raises its flag (VGMPlay masks it, other software polls it).
    OPNA_Reset(ram, sizeof(ram));
    fm1(0x24, 0xFF); fm1(0x25, 0x03);   // timer A period: 1 tick
    fm1(0x27, 0x15);                    // load + enable timer A
    render(10);
    CHECK((OPNA_Read(0) & 0x01) != 0, "timer A flag");

    // Rhythm (ADPCM-A) plays from the internal ROM: a bass drum then a hi-hat.
    OPNA_Reset(ram, sizeof(ram));
    fm1(0x11, 0x3F);
    for (int i = 0; i < 6; ++i)
        fm1(0x18 + i, 0xDF);
    fm1(0x10, 0x01);               // bass drum
    Stats bd = render(8000);
    std::printf("Rhythm bass drum: peak %d, %u samples long\n", bd.peak, bd.last_audible);
    CHECK(bd.peak > 1000, "bass drum audible");
    // 448 bytes of ADPCM at 18.5 kHz: about 48 ms, i.e. ~2100 output samples.
    CHECK(bd.last_audible > 1700 && bd.last_audible < 2600, "bass drum length %u", bd.last_audible);
    fm1(0x10, 0x88);               // dump bass drum, keep the rest idle
    render(2000);
    fm1(0x10, 0x08);               // hi-hat
    Stats hh = render(8000);
    std::printf("Rhythm hi-hat: peak %d\n", hh.peak);
    CHECK(hh.peak > 500, "hi-hat audible");

    // Worst-case load (six FM channels with four carriers, three SSG tones)
    // rendered for one second; informational only, the budget on the RP2350
    // is far tighter than on a PC.
    OPNA_Reset(ram, sizeof(ram));
    fm1(0x29, 0x80);
    for (int ch = 0; ch < 3; ++ch)
    {
        for (int bank = 0; bank < 2; ++bank)
        {
            auto w = bank ? fm2 : fm1;
            w(0xB0 + ch, 0x07); w(0xB4 + ch, 0xC0);
            for (int op = 0; op < 4; ++op)
            {
                w(0x30 + ch + op * 4, 0x01); w(0x40 + ch + op * 4, 0x10);
                w(0x50 + ch + op * 4, 0x1F); w(0x80 + ch + op * 4, 0x0F);
            }
            w(0xA4 + ch, (3 << 3) | 4); w(0xA0 + ch, 0x20 + ch * 16);
        }
        fm1(0x00 + ch * 2, 100 + ch * 30); fm1(0x08 + ch, 0x0F);
    }
    fm1(0x07, 0x38);
    for (int ch = 0; ch < 3; ++ch) { fm1(0x28, 0xF0 | ch); fm1(0x28, 0xF4 | ch); }
    auto t0 = std::chrono::steady_clock::now();
    Stats load = render(44100);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Full load: peak %d, %.1f ms of host time per second of audio\n", load.peak, ms);
    CHECK(load.peak > 0, "full load audible");

    std::puts("PASS: Makoto YM2608 detect, FM, SSG, rhythm ROM, ADPCM-B upload and timer A");
    return 0;
}
