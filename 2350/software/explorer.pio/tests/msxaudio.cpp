// Host test of the MSX-AUDIO (Y8950) emulation core as the Explorer firmware
// builds it: compiled with OPL4_OUTPUT_STEP=2, like ymfm_opl.cpp, so it proves
// that the Y8950 stays at its native 49716 Hz while the OPL4 keeps its half rate.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "emu278b.h"
#include "emuy8950.h"
#include "ymfm/ymfm_opl.h"

#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); std::exit(1); } } while (0)

// The reduced rate belongs to the OPL4 register set only.
static_assert(OPL4_OUTPUT_STEP == 2, "build with -DOPL4_OUTPUT_STEP=2 like the firmware");
static_assert(ymfm::fm_output_step<ymfm::opl_registers_base<1>>::value == 1, "Y8950/OPL is native rate");
static_assert(ymfm::fm_output_step<ymfm::opl_registers_base<4>>::value == 2, "OPL4 keeps its half rate");

static int irq_asserts;
static void on_irq(bool asserted) { if (asserted) irq_asserts++; }

static uint8_t ram[256 * 1024];
alignas(16) static uint8_t arena[65536];

static void w(int reg, int value) { Y8950_Write(0, (uint8_t)reg); Y8950_Write(1, (uint8_t)value); }

static unsigned rising_crossings(unsigned samples)
{
    unsigned count = 0;
    int32_t prev = 0;
    for (unsigned i = 0; i < samples; i++)
    {
        int32_t s = Y8950_Clock();
        if (prev <= 0 && s > 0)
            count++;
        prev = s;
    }
    return count;
}

int main()
{
    CHECK(Y8950_StateSize() > 0 && Y8950_StateSize() < sizeof(arena) / 2, "state size is small enough for the arena");
    CHECK(!Y8950_Attach(arena + 1, Y8950_StateSize()), "misaligned memory is refused");
    CHECK(!Y8950_Attach(arena, 16), "too small memory is refused");
    CHECK(Y8950_Attach(arena, Y8950_StateSize()), "attach in the arena");
    Y8950_SetIrqCallback(on_irq);
    Y8950_Reset(ram, sizeof(ram));

    CHECK((Y8950_Read(0) & 0x06) == 0x06, "status bits 1 and 2 always read as 1");

    // 440 Hz on channel 0 (carrier only, modulator at minimum level). With
    // fnum 0x244 and block 4 the Y8950 gives 580 * 49716 / 2^16 = 440.0 Hz, so a
    // half-rate build would measure 880.
    w(0x20, 0x21); w(0x23, 0x21);
    w(0x40, 0x3F); w(0x43, 0x00);
    w(0x60, 0xF0); w(0x63, 0xF0);
    w(0x80, 0x0F); w(0x83, 0x0F);
    w(0xC0, 0x01);
    w(0xA0, 0x44); w(0xB0, 0x32);
    for (int i = 0; i < 2000; i++) Y8950_Clock();
    unsigned hz = rising_crossings(49716);
    std::printf("Y8950 channel 0 fnum 0x244 block 4: %u Hz over 49716 samples\n", hz);
    CHECK(hz >= 438 && hz <= 442, "FM pitch is right at the native 49716 Hz");
    w(0xB0, 0x12); // key off

    // Timer 1: 256 ticks of 80 us = 20.48 ms = 1018 samples at 49716 Hz. A timer
    // counted in the wrong sample length would run at half or twice the tempo.
    irq_asserts = 0;
    w(0x04, 0x80);
    w(0x02, 0x00);
    w(0x04, 0x01);
    unsigned fired = 0;
    for (unsigned i = 1; i <= 4000 && !fired; i++)
    {
        Y8950_Clock();
        if (Y8950_Read(0) & 0x40) fired = i;
    }
    std::printf("Y8950 timer 1 flag after %u samples\n", fired);
    CHECK(fired >= 1008 && fired <= 1028, "timer 1 fires after 20.48 ms");
    CHECK(irq_asserts == 1 && (Y8950_Read(0) & 0x80) != 0, "timer raises /INT once and sets the IRQ status bit");
    w(0x04, 0x80);
    CHECK((Y8950_Read(0) & 0xC0) == 0 && irq_asserts == 1, "flag reset releases the status bits");

    // ADPCM-B sample RAM as an MSX-AUDIO program uploads and reads it back: the
    // data port is register 0x0F (an earlier ymfm routed the read to 0x09).
    uint8_t pattern[24];
    for (unsigned i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(0xA5 ^ (i * 29));
    w(0x07, 0x01);                       // reset
    w(0x08, 0x02);                       // 8-bit DRAM granularity, RAM
    w(0x09, 0x00); w(0x0A, 0x00);        // start address 0
    w(0x0B, 0xFF); w(0x0C, 0xFF);        // stop address
    w(0x07, 0x60);                       // record, external memory
    for (unsigned i = 0; i < sizeof(pattern); i++) w(0x0F, pattern[i]);
    CHECK(std::memcmp(ram, pattern, sizeof(pattern)) == 0, "uploaded bytes land in the ADPCM RAM");
    w(0x07, 0x01);
    w(0x07, 0x20);                       // external memory, read back
    Y8950_Write(0, 0x0F);
    (void)Y8950_Read(1); (void)Y8950_Read(1); // two dummy reads
    bool same = true;
    for (unsigned i = 0; i < sizeof(pattern); i++)
        same = same && Y8950_Read(1) == pattern[i];
    CHECK(same, "ADPCM RAM reads back through the data port");

    // The OPL4 in the same translation unit keeps its half rate: one OPL4
    // sample stands for two native periods, so the 20.48 ms timer 1 fires after
    // 20.48 ms * 22050 = 451 generated samples.
    alignas(16) static uint8_t opl4_arena[32768];
    static uint8_t opl4_rom[1024], opl4_ram[1024];
    CHECK(OPL4_Attach(opl4_arena, OPL4_StateSize()), "OPL4 attach");
    opl4_t chip;
    OPL4_Reset(&chip, opl4_rom, sizeof(opl4_rom), opl4_ram, sizeof(opl4_ram));
    OPL4_Write(&chip, 0, 0x02); OPL4_Write(&chip, 1, 0x00);
    OPL4_Write(&chip, 0, 0x04); OPL4_Write(&chip, 1, 0x01);
    unsigned opl4_fired = 0;
    for (unsigned i = 1; i <= 2000 && !opl4_fired; i++)
    {
        int32_t out[2];
        OPL4_Clock(&chip, out);
        if (OPL4_Read(&chip, 0) & 0x40) opl4_fired = i;
    }
    std::printf("OPL4 timer 1 flag after %u samples (22.05 kHz)\n", opl4_fired);
    CHECK(opl4_fired >= 445 && opl4_fired <= 458, "OPL4 timer still runs at its half rate");

    std::puts("PASS: Y8950 native-rate pitch and timer, ADPCM RAM upload/readback, OPL4 half rate unchanged");
    return 0;
}
