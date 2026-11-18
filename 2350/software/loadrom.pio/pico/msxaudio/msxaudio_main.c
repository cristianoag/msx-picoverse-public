// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// msxaudio_main.c - Standalone MSX-AUDIO (Yamaha Y8950) cartridge firmware
//
// Turns the PicoVerse 2350 cartridge into a dedicated MSX-AUDIO sound
// cartridge, modelled on the openMSX "Philips/MSXPro MSX-Audio" extension
// (share/extensions/Boosted_audio.xml): a Philips NMS-1205 Music Module
// upgraded with the MSXPro expansion, i.e. a Y8950 with 256 kB of ADPCM sample
// RAM plus the MSX-Audio BIOS that makes it a fully compatible MSX-AUDIO
// device.
//
// openMSX extension, reproduced here:
//
//   <MSX-AUDIO>            io base 0xC0, 2 ports; type Philips; sampleram 256
//   <ROM Normal0000>       mem 0x0000/0x4000/0x8000/0xC000, each 0x3000 bytes
//   <RAM base WorkRAM>     0x1000 bytes at 0x3000, mirrored at 0x7000
//   <RAM expanded WorkRAM> 0x1000 bytes at 0xB000, mirrored at 0xF000
//
// With the 48 kB "MSX-Audio BIOS v1.3" image and the Normal0000 plain mapper,
// the ROM offset equals the Z80 address, so the resulting slot layout is:
//
//   0x0000-0x2FFF  BIOS ROM
//   0x3000-0x3FFF  base work RAM (4 kB)
//   0x4000-0x6FFF  BIOS ROM              ("AB" cartridge header at 0x4000)
//   0x7000-0x7FFF  base work RAM mirror
//   0x8000-0xAFFF  BIOS ROM
//   0xB000-0xBFFF  expanded work RAM (4 kB)
//   0xC000-0xEFFF  unmapped (past the end of the 48 kB image -> reads 0xFF)
//   0xF000-0xFFFF  expanded work RAM mirror
//
// Architecture (single owner, no locks):
//
//   Core 0 owns the MSX bus AND the ymfm Y8950 core. Its loop drains the two
//   write captors, answers the pending read (memory or Y8950, with the Z80
//   frozen by /WAIT), and then synthesises exactly ONE audio sample into a
//   lock-free ring buffer before looking at the bus again. Because a single
//   Y8950 sample costs only a couple of microseconds, the extra latency a bus
//   cycle can ever see is bounded by that - far below the Z80 DRAM refresh
//   limit - while host register accesses hit the chip directly, with no queue,
//   no cache and no cross-core lock. That is what makes Y8950 status and ADPCM
//   data reads exact, which the MSX-AUDIO BIOS depends on.
//
//   Core 1 only drains the ring into the pico_audio_i2s producer pool and runs
//   the I2S DMA. It never touches ymfm.
//
// The Y8950's native output rate is clock / 72 = 49716 Hz, so the I2S output
// runs at that rate and nothing in the audio path resamples.
//
// The MSX-Audio BIOS ROM is appended to this firmware in the UF2 image behind a
// 16-byte config header, so it is found at __flash_binary_end. The 256 kB ADPCM
// sample RAM is allocated from external PSRAM (QMI CS1).
//
// Not emulated: the NMS-1205 MIDI interface (a separate openMSX device) and the
// Music Module's 8-bit DAC on port 0x0A.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "pico/audio_i2s.h"

#include "msxaudio.h"
#include "emuy8950.h"
#include "msx_msxaudio.pio.h"

// -----------------------------------------------------------------------
// PIO / state machine allocation (see msx_msxaudio.pio)
// -----------------------------------------------------------------------
#define PIO_READ        pio0
#define SM_READ         0

#define PIO_WRITE       pio1
#define SM_MEM_WRITE    0
#define SM_IO_WRITE     1
#define SM_I2S          2

#define PIO_BUSDIR      pio2
#define SM_BUSDIR       0

// A1..A7 pattern that selects ports 0xC0/0xC1; primed into the decoding state
// machines' Y register through the TX FIFO before they are enabled.
#define MSXAUDIO_PORT_MATCH  0x60u

// -----------------------------------------------------------------------
// Slot memory model
// -----------------------------------------------------------------------
#define MSXAUDIO_BLOCK_SIZE  0x1000u          // 4 kB decode granularity
#define MSXAUDIO_WORK_RAM_SIZE 0x1000u        // openMSX <size>0x1000</size>

static uint8_t msxaudio_rom[MSXAUDIO_BIOS_ROM_SIZE];      // BIOS image in SRAM
static uint8_t msxaudio_ram_base[MSXAUDIO_WORK_RAM_SIZE]; // 0x3000 / 0x7000
static uint8_t msxaudio_ram_exp[MSXAUDIO_WORK_RAM_SIZE];  // 0xB000 / 0xF000

// The BIOS image is served straight from flash until it has been migrated into
// SRAM. Copying 48 kB out of XIP takes on the order of a millisecond, which is
// far too long to spend inside the boot /WAIT hold (the Z80 would stop
// refreshing DRAM), so the copy is done in small chunks from the bus loop once
// the cartridge is already answering cycles, and the decode tables are
// re-pointed at SRAM only when it is complete.
#define MSXAUDIO_ROM_COPY_CHUNK 128u
static const uint8_t *msxaudio_rom_source = NULL;
static uint32_t msxaudio_rom_copied = 0;

// 4 kB block decode tables. NULL == unmapped (reads float, writes discarded).
static const uint8_t *msxaudio_read_map[16];
static uint8_t       *msxaudio_write_map[16];

// 256 kB ADPCM sample RAM in external PSRAM.
static uint8_t *msxaudio_sample_ram = NULL;

// Appended flash payload: [16-byte "PVAU" config header][48 kB BIOS ROM].
extern uint8_t __flash_binary_end;
static uint8_t *const volatile msxaudio_payload = &__flash_binary_end;

// Y8950 input clock selected by the loadrom tool through the config header.
// The emulated chip always produces one output sample per 72 master clocks, so
// the clock choice is expressed entirely as the output/I2S sample rate: at
// 4 MHz the whole chip (pitch, envelopes, LFO, ADPCM rate and FM timers) runs
// 1.1174x faster than at the standard 3.579545 MHz.
static bool     msxaudio_clock_4mhz = false;
static uint32_t msxaudio_sample_rate = MSXAUDIO_SAMPLE_RATE;

// -----------------------------------------------------------------------
// Audio ring buffer (Core 0 produces, Core 1 consumes; single reader/writer)
// -----------------------------------------------------------------------
#define MSXAUDIO_AUDIO_BUFFER_SAMPLES 256u
#define MSXAUDIO_RING_SIZE            4096u
#define MSXAUDIO_RING_MASK            (MSXAUDIO_RING_SIZE - 1u)

static int16_t msxaudio_ring[MSXAUDIO_RING_SIZE];
static volatile uint32_t msxaudio_ring_head = 0;   // written by Core 0
static volatile uint32_t msxaudio_ring_tail = 0;   // written by Core 1

static struct audio_buffer_pool *audio_pool = NULL;
static int audio_dma_channel = -1;

// -----------------------------------------------------------------------
// Optional USB CDC diagnostics (off by default; toggled in CMakeLists.txt)
// -----------------------------------------------------------------------
#if MSXAUDIO_USB_DEBUG
static volatile uint32_t dbg_mem_reads   = 0;
static volatile uint32_t dbg_mem_writes  = 0;
static volatile uint32_t dbg_io_reads    = 0;
static volatile uint32_t dbg_io_writes   = 0;
static volatile uint32_t dbg_samples     = 0;
static volatile uint32_t dbg_underruns   = 0;
static volatile uint8_t  dbg_last_status = 0;
static volatile uint8_t  dbg_last_reg    = 0;
static volatile uint8_t  dbg_int_level   = 0;
#define DBG_INC(v) ((v)++)
#define DBG_SET(v, n) ((v) = (n))
#else
#define DBG_INC(v)    ((void)0)
#define DBG_SET(v, n) ((void)0)
#endif

// -----------------------------------------------------------------------
// External PSRAM init (QMI CS1), tuned for a 300 MHz system clock
// -----------------------------------------------------------------------
static inline void __not_in_flash_func(psram_delay_cycles)(uint32_t cycles)
{
    for (volatile uint32_t cycle = 0; cycle < cycles; ++cycle)
        __asm volatile ("nop");
}

static inline void __not_in_flash_func(psram_wait_direct_done)(void)
{
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {}
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}
}

static inline void __not_in_flash_func(psram_send_direct_cmd)(uint8_t cmd, bool quad_width)
{
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    if (quad_width)
        qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                            (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                            cmd;
    else
        qmi_hw->direct_tx = cmd;
    psram_wait_direct_done();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    (void)qmi_hw->direct_rx;
}

static bool __no_inline_not_in_flash_func(psram_init)(void)
{
    gpio_set_function(PIN_PSRAM, GPIO_FUNC_XIP_CS1);

    uint32_t irq_state = save_and_disable_interrupts();

    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}

    // Warm restart path: if PSRAM remained powered it may still be in QPI mode.
    psram_send_direct_cmd(0xF5u, true);
    psram_delay_cycles(128u);

    psram_send_direct_cmd(0x66u, false);
    psram_delay_cycles(128u);
    psram_send_direct_cmd(0x99u, false);
    psram_delay_cycles(50000u);
    psram_send_direct_cmd(0x35u, false);
    psram_delay_cycles(128u);
    psram_send_direct_cmd(0xC0u, false);
    psram_delay_cycles(128u);

    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    // PSRAM QMI timing for a 300 MHz system clock: divider 3 keeps the PSRAM
    // device clock at 100 MHz, and the sysclk-domain delay fields are scaled so
    // the absolute timing matches the values validated on this hardware.
    qmi_hw->m[1].timing =
        (QMI_M0_TIMING_PAGEBREAK_VALUE_1024 << QMI_M0_TIMING_PAGEBREAK_LSB) |
        (1u << QMI_M0_TIMING_SELECT_HOLD_LSB) |
        (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
        (3u << QMI_M0_TIMING_RXDELAY_LSB) |
        (37u << QMI_M0_TIMING_MAX_SELECT_LSB) |
        (7u << QMI_M0_TIMING_MIN_DESELECT_LSB) |
        (3u << QMI_M0_TIMING_CLKDIV_LSB);
    qmi_hw->m[1].rfmt =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_LEN_VALUE_24 << QMI_M0_RFMT_DUMMY_LEN_LSB) |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB) |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB) |
        (QMI_M0_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_RFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = (0xEBu << QMI_M0_RCMD_PREFIX_LSB);
    qmi_hw->m[1].wfmt =
        (QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M0_WFMT_DUMMY_LEN_LSB) |
        (QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB) |
        (QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB) |
        (QMI_M0_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_WFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = (0x38u << QMI_M0_WCMD_PREFIX_LSB);

    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

    restore_interrupts(irq_state);

    volatile uint32_t *psram_test = (volatile uint32_t *)0x15000000u;
    psram_test[0] = 0x12345678u;
    return psram_test[0] == 0x12345678u;
}

// -----------------------------------------------------------------------
// GPIO setup - bus pins, with /WAIT held asserted until the PIO takes over
// -----------------------------------------------------------------------
static void setup_gpio(void)
{
    for (uint pin = PIN_A0; pin <= PIN_A15; ++pin)
    {
        gpio_init(pin);
        gpio_set_input_hysteresis_enabled(pin, true);
        gpio_set_dir(pin, GPIO_IN);
    }

    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
    {
        gpio_init(pin);
        gpio_set_input_hysteresis_enabled(pin, true);
        gpio_set_dir(pin, GPIO_IN);
    }

    gpio_init(PIN_RD);      gpio_set_dir(PIN_RD, GPIO_IN);
    gpio_init(PIN_WR);      gpio_set_dir(PIN_WR, GPIO_IN);
    gpio_init(PIN_IORQ);    gpio_set_dir(PIN_IORQ, GPIO_IN);
    gpio_init(PIN_SLTSL);   gpio_set_dir(PIN_SLTSL, GPIO_IN);
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN);

    // /WAIT asserted (driven low) during bring-up so the Z80 stays frozen until
    // the bus responder is live and the cartridge can answer its first read.
    // The window between here and io_bus_init() is the PSRAM bring-up plus the
    // 48 kB BIOS copy, well under the ~2 ms Z80 DRAM refresh limit.
    gpio_init(PIN_WAIT);
    gpio_put(PIN_WAIT, 0);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
}

// -----------------------------------------------------------------------
// MSX /INT line (open-drain on GPIO 40): asserted = driven low,
// released = hi-Z so the shared open-collector line floats back high.
// -----------------------------------------------------------------------
static void __not_in_flash_func(msxaudio_int_line_drive)(bool asserted)
{
    if (asserted)
    {
        gpio_put(PIN_INT, 0);
        gpio_set_dir(PIN_INT, GPIO_OUT);
    }
    else
    {
        gpio_set_dir(PIN_INT, GPIO_IN);
    }
    DBG_SET(dbg_int_level, asserted ? 1u : 0u);
}

// -----------------------------------------------------------------------
// Slot memory map (openMSX RomPlain "Normal0000" + the two work RAM blocks)
// -----------------------------------------------------------------------
static void msxaudio_build_memory_map(const uint8_t *rom_base)
{
    for (uint32_t block = 0; block < 16; ++block)
    {
        msxaudio_read_map[block] = NULL;
        msxaudio_write_map[block] = NULL;
    }

    // ROM windows: Normal0000 means the ROM offset equals the Z80 address.
    static const uint32_t rom_blocks[] = { 0x0, 0x1, 0x2, 0x4, 0x5, 0x6, 0x8, 0x9, 0xA };
    for (uint32_t i = 0; i < count_of(rom_blocks); ++i)
    {
        uint32_t block = rom_blocks[i];
        msxaudio_read_map[block] = rom_base + block * MSXAUDIO_BLOCK_SIZE;
    }

    // Base work RAM at 0x3000, mirrored at 0x7000.
    msxaudio_read_map[0x3]  = msxaudio_ram_base;
    msxaudio_read_map[0x7]  = msxaudio_ram_base;
    msxaudio_write_map[0x3] = msxaudio_ram_base;
    msxaudio_write_map[0x7] = msxaudio_ram_base;

    // Expanded work RAM at 0xB000, mirrored at 0xF000.
    msxaudio_read_map[0xB]  = msxaudio_ram_exp;
    msxaudio_read_map[0xF]  = msxaudio_ram_exp;
    msxaudio_write_map[0xB] = msxaudio_ram_exp;
    msxaudio_write_map[0xF] = msxaudio_ram_exp;

    // 0xC000-0xEFFF stays unmapped: the openMSX ROM window declared there falls
    // past the end of the 48 kB BIOS image, so it reads as 0xFF.
}

static void msxaudio_init(void)
{
    // /INT released (hi-Z) at start; preload 0 so asserting only flips the
    // direction. Registered before the chip reset so the very first interrupt
    // state change reaches the pin.
    gpio_init(PIN_INT);
    gpio_put(PIN_INT, 0);
    gpio_set_dir(PIN_INT, GPIO_IN);
    Y8950_SetIrqCallback(msxaudio_int_line_drive);

    // Locate the appended BIOS ROM behind the 16-byte config header and serve
    // it from flash for now; the bus loop migrates it into SRAM afterwards.
    const uint8_t *payload = (const uint8_t *)msxaudio_payload;
    if (payload[0] == MSXAUDIO_CFG_MAGIC0 && payload[1] == MSXAUDIO_CFG_MAGIC1 &&
        payload[2] == MSXAUDIO_CFG_MAGIC2 && payload[3] == MSXAUDIO_CFG_MAGIC3)
    {
        msxaudio_clock_4mhz = (payload[4] & MSXAUDIO_CFG_FLAG_CLOCK_4MHZ) != 0;
        payload += MSXAUDIO_CFG_HEADER_SIZE;
    }
    msxaudio_sample_rate = msxaudio_clock_4mhz ? MSXAUDIO_SAMPLE_RATE_4MHZ
                                               : MSXAUDIO_SAMPLE_RATE;
    msxaudio_rom_source = payload;
    msxaudio_rom_copied = 0;

    memset(msxaudio_ram_base, 0, sizeof(msxaudio_ram_base));
    memset(msxaudio_ram_exp, 0, sizeof(msxaudio_ram_exp));
    msxaudio_build_memory_map(msxaudio_rom_source);

    // 256 kB ADPCM sample RAM from external PSRAM (cached write-through window).
    msxaudio_sample_ram = (uint8_t *)PSRAM_BASE_ADDR;
    Y8950_Reset(msxaudio_sample_ram, MSXAUDIO_SAMPLE_RAM_SIZE);
}

// -----------------------------------------------------------------------
// Bus PIO initialisation
// -----------------------------------------------------------------------
static void io_bus_init(void)
{
    // --- PIO0 SM0: unified read responder (memory + Y8950), owns /WAIT ---
    uint offset_read = pio_add_program(PIO_READ, &msxaudio_read_program);

    pio_sm_set_enabled(PIO_READ, SM_READ, false);
    pio_sm_clear_fifos(PIO_READ, SM_READ);
    pio_sm_restart(PIO_READ, SM_READ);

    pio_sm_config cfg_read = msxaudio_read_program_get_default_config(offset_read);
    sm_config_set_in_pins(&cfg_read, PIN_A0);
    sm_config_set_in_shift(&cfg_read, false, false, 32);   // shift left, no autopush
    sm_config_set_out_pins(&cfg_read, PIN_D0, 8);
    sm_config_set_out_shift(&cfg_read, true, false, 32);   // shift right, no autopull
    sm_config_set_sideset_pins(&cfg_read, PIN_WAIT);
    sm_config_set_jmp_pin(&cfg_read, PIN_SLTSL);
    sm_config_set_clkdiv(&cfg_read, 1.0f);
    pio_sm_init(PIO_READ, SM_READ, offset_read, &cfg_read);

    // /WAIT is open-drain: preload the pin output value LOW and leave the pin
    // direction as input. The program asserts /WAIT by switching the direction
    // to output (driving the preloaded low) and releases it by switching back.
    pio_sm_set_pins_with_mask(PIO_READ, SM_READ, 0u, (1u << PIN_WAIT));
    pio_gpio_init(PIO_READ, PIN_WAIT);
    pio_sm_set_consecutive_pindirs(PIO_READ, SM_READ, PIN_WAIT, 1, false);

    // D0-D7: hand over to PIO0 for bidirectional control, initially tri-stated.
    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
        pio_gpio_init(PIO_READ, pin);
    pio_sm_set_consecutive_pindirs(PIO_READ, SM_READ, PIN_D0, 8, false);

    // --- PIO1 SM0: memory write captor ---
    uint offset_mem_write = pio_add_program(PIO_WRITE, &msxaudio_mem_write_program);

    pio_sm_set_enabled(PIO_WRITE, SM_MEM_WRITE, false);
    pio_sm_clear_fifos(PIO_WRITE, SM_MEM_WRITE);
    pio_sm_restart(PIO_WRITE, SM_MEM_WRITE);

    pio_sm_config cfg_mem_write = msxaudio_mem_write_program_get_default_config(offset_mem_write);
    sm_config_set_in_pins(&cfg_mem_write, PIN_A0);
    sm_config_set_in_shift(&cfg_mem_write, false, false, 32);
    sm_config_set_fifo_join(&cfg_mem_write, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_mem_write, PIN_WR);
    sm_config_set_clkdiv(&cfg_mem_write, 1.0f);
    pio_sm_init(PIO_WRITE, SM_MEM_WRITE, offset_mem_write, &cfg_mem_write);

    // --- PIO1 SM1: decoded Y8950 I/O write captor ---
    uint offset_io_write = pio_add_program(PIO_WRITE, &msxaudio_io_write_program);

    pio_sm_set_enabled(PIO_WRITE, SM_IO_WRITE, false);
    pio_sm_clear_fifos(PIO_WRITE, SM_IO_WRITE);
    pio_sm_restart(PIO_WRITE, SM_IO_WRITE);

    pio_sm_config cfg_io_write = msxaudio_io_write_program_get_default_config(offset_io_write);
    sm_config_set_in_pins(&cfg_io_write, PIN_A0);
    sm_config_set_in_shift(&cfg_io_write, false, false, 32);
    sm_config_set_out_shift(&cfg_io_write, true, false, 32);
    // No PIO_FIFO_JOIN_RX here: this program pulls its port match value from the
    // TX FIFO once at start-up, and joining the FIFOs to RX would remove the TX
    // FIFO entirely (the one-shot pull would stall forever). The 4-deep RX FIFO
    // is ample because only 0xC0/0xC1 writes are captured and the bus loop
    // drains it on every pass.
    sm_config_set_jmp_pin(&cfg_io_write, PIN_WR);
    sm_config_set_clkdiv(&cfg_io_write, 1.0f);
    pio_sm_init(PIO_WRITE, SM_IO_WRITE, offset_io_write, &cfg_io_write);

    // --- PIO2 SM0: dedicated /BUSDIR driver (GPIO base 16 reaches GPIO 37) ---
    pio_set_gpio_base(PIO_BUSDIR, 16);
    uint offset_busdir = pio_add_program(PIO_BUSDIR, &msxaudio_busdir_program);

    pio_sm_set_enabled(PIO_BUSDIR, SM_BUSDIR, false);
    pio_sm_clear_fifos(PIO_BUSDIR, SM_BUSDIR);
    pio_sm_restart(PIO_BUSDIR, SM_BUSDIR);

    pio_sm_config cfg_busdir = msxaudio_busdir_program_get_default_config(offset_busdir);
    sm_config_set_in_pins(&cfg_busdir, PIN_RD);            // pin0=/RD, pin4=/WAIT
    sm_config_set_jmp_pin(&cfg_busdir, PIN_IORQ);
    sm_config_set_sideset_pins(&cfg_busdir, PIN_BUSSDIR);
    sm_config_set_clkdiv(&cfg_busdir, 1.0f);
    pio_sm_init(PIO_BUSDIR, SM_BUSDIR, offset_busdir, &cfg_busdir);

    // /BUSDIR is push-pull: preload the value HIGH (released) and make the pin
    // a permanent output, so the MSX data-bus buffer direction is never left
    // floating.
    pio_sm_set_pins_with_mask64(PIO_BUSDIR, SM_BUSDIR,
                                (1ull << PIN_BUSSDIR), (1ull << PIN_BUSSDIR));
    pio_gpio_init(PIO_BUSDIR, PIN_BUSSDIR);
    pio_sm_set_consecutive_pindirs(PIO_BUSDIR, SM_BUSDIR, PIN_BUSSDIR, 1, true);

    // Prime the port match value consumed by the two decoding programs.
    pio_sm_put(PIO_READ, SM_READ, MSXAUDIO_PORT_MATCH);
    pio_sm_put(PIO_WRITE, SM_IO_WRITE, MSXAUDIO_PORT_MATCH);

    pio_sm_set_enabled(PIO_READ,   SM_READ,      true);
    pio_sm_set_enabled(PIO_WRITE,  SM_MEM_WRITE, true);
    pio_sm_set_enabled(PIO_WRITE,  SM_IO_WRITE,  true);
    pio_sm_set_enabled(PIO_BUSDIR, SM_BUSDIR,    true);
}

// -----------------------------------------------------------------------
// Bus service helpers (Core 0)
// -----------------------------------------------------------------------

// Response token for the read state machine:
//   bits[7:0]  = data byte
//   bits[15:8] = pindirs (0xFF = drive the bus, 0x00 = stay tri-stated)
static inline uint32_t __not_in_flash_func(build_token)(bool drive, uint8_t data)
{
    return drive ? (0xFF00u | data) : (uint32_t)data;
}

static inline void __not_in_flash_func(drain_memory_writes)(void)
{
    while (!pio_sm_is_rx_fifo_empty(PIO_WRITE, SM_MEM_WRITE))
    {
        uint32_t sample = pio_sm_get(PIO_WRITE, SM_MEM_WRITE);
        uint16_t addr = (uint16_t)(sample & 0xFFFFu);
        uint8_t *block = msxaudio_write_map[addr >> 12];
        if (block)
            block[addr & (MSXAUDIO_BLOCK_SIZE - 1u)] = (uint8_t)((sample >> 16) & 0xFFu);
        DBG_INC(dbg_mem_writes);
    }
}

static inline void __not_in_flash_func(drain_io_writes)(void)
{
    while (!pio_sm_is_rx_fifo_empty(PIO_WRITE, SM_IO_WRITE))
    {
        uint32_t sample = pio_sm_get(PIO_WRITE, SM_IO_WRITE);
        uint8_t data = (uint8_t)((sample >> 16) & 0xFFu);
        // A0 selects the Y8950 register-select port (0xC0) or data port (0xC1).
        Y8950_Write(sample & 1u, data);
        DBG_SET(dbg_last_reg, (sample & 1u) ? dbg_last_reg : data);
        DBG_INC(dbg_io_writes);
    }
}

static inline void __not_in_flash_func(service_read)(void)
{
    if (pio_sm_is_rx_fifo_empty(PIO_READ, SM_READ))
        return;

    uint32_t captured = pio_sm_get(PIO_READ, SM_READ);

    // The Z80 is frozen by /WAIT, so the cycle's control lines are still valid:
    // /SLTSL low means this is a memory read, otherwise it is a Y8950 read.
    if (gpio_get(PIN_SLTSL) == 0)
    {
        // Apply any preceding memory write before answering a read-back.
        drain_memory_writes();

        uint16_t addr = (uint16_t)(captured & 0xFFFFu);
        const uint8_t *block = msxaudio_read_map[addr >> 12];
        uint32_t token = block
            ? build_token(true, block[addr & (MSXAUDIO_BLOCK_SIZE - 1u)])
            : build_token(false, 0xFFu);
        pio_sm_put(PIO_READ, SM_READ, token);
        DBG_INC(dbg_mem_reads);
    }
    else
    {
        // Apply any preceding OUT before answering the IN (register select,
        // ADPCM data streaming, status polling all depend on that ordering).
        drain_io_writes();

        uint8_t data = Y8950_Read(captured & 1u);
        pio_sm_put(PIO_READ, SM_READ, build_token(true, data));
        DBG_SET(dbg_last_status, ((captured & 1u) == 0u) ? data : dbg_last_status);
        DBG_INC(dbg_io_reads);
    }
}

static inline void __not_in_flash_func(generate_audio_sample)(void)
{
    uint32_t head = msxaudio_ring_head;
    uint32_t next = (head + 1u) & MSXAUDIO_RING_MASK;
    if (next == msxaudio_ring_tail)
        return;                       // ring full: Core 1 has not caught up yet

    msxaudio_ring[head] = (int16_t)Y8950_Clock();
    msxaudio_ring_head = next;
    DBG_INC(dbg_samples);
}

// -----------------------------------------------------------------------
// Optional USB CDC diagnostics
// -----------------------------------------------------------------------
#if MSXAUDIO_USB_DEBUG
static void msxaudio_debug_usb_boot_wait(void)
{
    absolute_time_t deadline = make_timeout_time_ms(5000);
    uint32_t next_print_ms = 0;
    while (!time_reached(deadline))
    {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now_ms - next_print_ms) >= 0)
        {
            next_print_ms = now_ms + 250u;
            printf("MSXAUDDBG usb-boot %lu\r\n", (unsigned long)now_ms);
        }
        sleep_ms(10);
    }
}

static void __not_in_flash_func(msxaudio_debug_dump)(void)
{
    static uint32_t last_us = 0;
    static uint32_t last_mr = 0, last_mw = 0, last_ir = 0, last_iw = 0, last_smp = 0, last_un = 0;

    uint32_t now_us = time_us_32();
    if ((now_us - last_us) < 1000000u)
        return;
    last_us = now_us;

    uint32_t mr = dbg_mem_reads,  mr_d = mr - last_mr;   last_mr = mr;
    uint32_t mw = dbg_mem_writes, mw_d = mw - last_mw;   last_mw = mw;
    uint32_t ir = dbg_io_reads,   ir_d = ir - last_ir;   last_ir = ir;
    uint32_t iw = dbg_io_writes,  iw_d = iw - last_iw;   last_iw = iw;
    uint32_t sp = dbg_samples,    sp_d = sp - last_smp;  last_smp = sp;
    uint32_t un = dbg_underruns,  un_d = un - last_un;   last_un = un;

    uint32_t fill = (msxaudio_ring_head - msxaudio_ring_tail) & MSXAUDIO_RING_MASK;

    printf("MSXAUDDBG memR/s=%lu memW/s=%lu ioR/s=%lu ioW/s=%lu gen/s=%lu under/s=%lu ring=%lu st=%02X reg=%02X int=%u rate=%lu\r\n",
           (unsigned long)mr_d, (unsigned long)mw_d,
           (unsigned long)ir_d, (unsigned long)iw_d,
           (unsigned long)sp_d, (unsigned long)un_d,
           (unsigned long)fill,
           dbg_last_status, dbg_last_reg, dbg_int_level,
           (unsigned long)msxaudio_sample_rate);
}
#else
#define msxaudio_debug_usb_boot_wait() ((void)0)
#define msxaudio_debug_dump()          ((void)0)
#endif

// Migrate the BIOS image from flash into SRAM a chunk at a time so that no
// single bus cycle is ever stretched by more than a few microseconds. Once the
// whole image is resident the decode tables switch to the SRAM copy and MSX
// memory reads no longer depend on the XIP cache (which the ADPCM sample RAM in
// PSRAM also shares).
static inline void __not_in_flash_func(migrate_rom_to_sram)(void)
{
    if (msxaudio_rom_copied >= MSXAUDIO_BIOS_ROM_SIZE)
        return;

    memcpy(msxaudio_rom + msxaudio_rom_copied,
           msxaudio_rom_source + msxaudio_rom_copied,
           MSXAUDIO_ROM_COPY_CHUNK);
    msxaudio_rom_copied += MSXAUDIO_ROM_COPY_CHUNK;

    if (msxaudio_rom_copied >= MSXAUDIO_BIOS_ROM_SIZE)
        msxaudio_build_memory_map(msxaudio_rom);
}

// -----------------------------------------------------------------------
// Core 0 main bus loop: bus service first, then exactly one audio sample.
// -----------------------------------------------------------------------
static void __no_inline_not_in_flash_func(msxaudio_bus_loop)(void)
{
    while (true)
    {
        drain_memory_writes();
        drain_io_writes();
        service_read();
        generate_audio_sample();
        migrate_rom_to_sram();
        msxaudio_debug_dump();
    }
}

// -----------------------------------------------------------------------
// I2S audio init (PIO1 SM2) - called on Core 1 so the DMA IRQ lives there
// -----------------------------------------------------------------------
static void i2s_audio_init(void)
{
    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 0);   // active-high mute: low = unmuted

    // The output rate IS the emulated Y8950 clock: one sample per 72 master
    // clocks, so 49716 Hz for the standard 3.579545 MHz and 55555 Hz for the
    // optional 4 MHz build. Resolved on Core 0 in msxaudio_init() before this
    // core is launched.
    static audio_format_t audio_format = {
        .sample_freq = MSXAUDIO_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };
    audio_format.sample_freq = msxaudio_sample_rate;

    static struct audio_buffer_format producer_format = {
        .format = &audio_format,
        .sample_stride = 4,
    };

    audio_pool = audio_new_producer_pool(&producer_format, 3, MSXAUDIO_AUDIO_BUFFER_SAMPLES);

    for (int ch = 0; ch < NUM_DMA_CHANNELS; ++ch)
    {
        if (!dma_channel_is_claimed((uint)ch))
        {
            audio_dma_channel = ch;
            break;
        }
    }
    if (audio_dma_channel < 0)
    {
        audio_pool = NULL;
        return;
    }

    static struct audio_i2s_config i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_BCLK_PIN,
        .dma_channel = 0,
        .pio_sm = SM_I2S,
    };
    i2s_config.dma_channel = (uint)audio_dma_channel;

    audio_i2s_setup(&audio_format, &i2s_config);
    audio_i2s_connect(audio_pool);
    audio_i2s_set_enabled(true);
}

// -----------------------------------------------------------------------
// Core 1 - drain the ring into the I2S producer pool (mono -> stereo)
// -----------------------------------------------------------------------
static void __no_inline_not_in_flash_func(core1_i2s)(void)
{
    i2s_audio_init();

    if (!audio_pool)
    {
        while (true) { tight_loop_contents(); }
    }

    int16_t last_sample = 0;

    while (true)
    {
        struct audio_buffer *buffer = take_audio_buffer(audio_pool, true);
        int16_t *samples = (int16_t *)buffer->buffer->bytes;

        for (uint32_t i = 0; i < MSXAUDIO_AUDIO_BUFFER_SAMPLES; ++i)
        {
            uint32_t tail = msxaudio_ring_tail;
            if (tail != msxaudio_ring_head)
            {
                last_sample = msxaudio_ring[tail];
                msxaudio_ring_tail = (tail + 1u) & MSXAUDIO_RING_MASK;
            }
            else
            {
                // Core 0 fell behind: hold the last sample instead of injecting
                // a click, and never block the I2S DMA.
                DBG_INC(dbg_underruns);
            }

            samples[i * 2]     = last_sample;
            samples[i * 2 + 1] = last_sample;
        }

        buffer->sample_count = MSXAUDIO_AUDIO_BUFFER_SAMPLES;
        give_audio_buffer(audio_pool, buffer);
    }
}

// -----------------------------------------------------------------------
// main (Core 0)
// -----------------------------------------------------------------------
int __no_inline_not_in_flash_func(main)(void)
{
    // Flash timing + 300 MHz system clock (flash divider 3 keeps XIP at
    // 100 MHz). The whole firmware runs from SRAM, so this only affects the
    // boot-time copy and the appended BIOS read.
    qmi_hw->m[0].timing = 0x40000203;
    set_sys_clock_khz(300000, true);

#if MSXAUDIO_USB_DEBUG
    stdio_init_all();
    msxaudio_debug_usb_boot_wait();
#endif

    setup_gpio();

    // External PSRAM provides the 256 kB ADPCM sample RAM.
    if (!psram_init())
    {
        // Release /WAIT so the MSX can still boot without the cartridge instead
        // of being frozen forever.
        gpio_set_dir(PIN_WAIT, GPIO_IN);
        while (true) { tight_loop_contents(); }
    }

    msxaudio_init();

    // Bring the bus responder live; this also releases the boot /WAIT hold.
    io_bus_init();

    multicore_launch_core1(core1_i2s);

    msxaudio_bus_loop();
    return 0;
}
