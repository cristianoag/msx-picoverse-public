// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// carnivore2.c - dedicated Carnivore2 (RBSC) firmware for the PicoVerse 2350.
//
// The firmware turns the cartridge into a software compatible Carnivore2:
// the original FlashROM image (boot menu, Nextor IDE BIOS, FM-PAC BIOS, ROM
// directory) lives in RP2350 flash, the 1 MB memory mapper and the 1 MB
// MegaRAM/shadow RAM live in PSRAM, and the FPGA logic of the original design
// is modelled by core/c2_core.c.
//
// Core 0 runs the MSX bus (PIO0 memory cycles, PIO1 I/O cycles). Core 1 runs
// the microSD IDE back-end and renders SCC/SCC+, FM-PAC, PSG and beeper audio.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "carnivore2.h"
#include "msx_bus.pio.h"
#include "core/c2_core.h"
#include "core/c2_flash.h"
#include "core/c2_eeprom.h"
#include "audio/c2_audio.h"
#include "storage/c2_store.h"
#include "storage/sunrise_ide.h"
#include "storage/sunrise_sd.h"

// -----------------------------------------------------------------------
// PIO bus context
// -----------------------------------------------------------------------
typedef struct {
    PIO pio;
    uint sm_read;
    uint sm_write;
    uint offset_read;
    uint offset_write;
} msx_pio_bus_t;

typedef struct {
    PIO pio;
    uint sm_read;
    uint sm_write;
    uint offset_read;
    uint offset_write;
} msx_pio_io_bus_t;

static msx_pio_bus_t msx_bus;
static msx_pio_io_bus_t msx_io_bus;
static bool msx_bus_programs_loaded;
static bool msx_io_programs_loaded;

// -----------------------------------------------------------------------
// Emulated cartridge
// -----------------------------------------------------------------------
static c2_t c2;
static c2_flash_t flash_chip;
static c2_eeprom_t eeprom;
static sunrise_ide_t ide;

// The MSX is "quiet" when none of these were touched for QUIET_US. The flash
// write-back only runs then, because a sector commit stalls the bus for tens
// of milliseconds.
#define QUIET_US            200000u
#define COMMIT_SPACING_US   25000u
static uint32_t last_activity_us;
static uint32_t next_commit_us;

static inline void __not_in_flash_func(touch_activity)(void)
{
    last_activity_us = time_us_32();
}

// ---- FlashROM backend --------------------------------------------------
static uint8_t __not_in_flash_func(be_flash_read)(void *ctx, uint32_t addr)
{
    (void)ctx;
    return c2s_flash_read(addr);
}

static void __not_in_flash_func(be_flash_program)(void *ctx, uint32_t addr, uint8_t data)
{
    (void)ctx;
    c2s_flash_program(addr, data);
}

static void __not_in_flash_func(be_flash_erase)(void *ctx, uint32_t start, uint32_t length)
{
    (void)ctx;
    c2s_flash_erase(start, length);
}

static const c2_flash_backend_t flash_backend = {
    NULL, be_flash_read, be_flash_program, be_flash_erase
};

// ---- c2_hw_t callbacks ---------------------------------------------------
static uint8_t __not_in_flash_func(hw_flash_read)(void *ctx, uint32_t addr)
{
    (void)ctx;
    return c2_flash_read(&flash_chip, addr);
}

static void __not_in_flash_func(hw_flash_write)(void *ctx, uint32_t addr, uint8_t data)
{
    (void)ctx;
    c2_flash_write(&flash_chip, addr, data);
    c2_set_flash_array(&c2, flash_chip.mode == C2F_MODE_ARRAY && !flash_chip.in_reset);
    touch_activity();
}

static uint8_t __not_in_flash_func(hw_ram_read)(void *ctx, uint32_t addr)
{
    (void)ctx;
    return c2s_ram_read(addr);
}

static void __not_in_flash_func(hw_ram_write)(void *ctx, uint32_t addr, uint8_t data)
{
    (void)ctx;
    c2s_ram_write(addr, data);
}

static bool __not_in_flash_func(hw_ide_read)(void *ctx, uint16_t addr, uint8_t *data)
{
    (void)ctx;
    return sunrise_ide_handle_read(&ide, addr, data);
}

static void __not_in_flash_func(hw_ide_write)(void *ctx, uint16_t addr, uint8_t data)
{
    (void)ctx;
    (void)sunrise_ide_handle_write(&ide, addr, data);
}

static void __not_in_flash_func(hw_flash_ctrl)(void *ctx, bool reset_n, bool vpp_wp)
{
    (void)ctx;
    c2_flash_control(&flash_chip, reset_n, vpp_wp);
    c2_set_flash_array(&c2, flash_chip.mode == C2F_MODE_ARRAY && !flash_chip.in_reset);
}

static uint8_t __not_in_flash_func(hw_scc_wave_read)(void *ctx, uint8_t offset)
{
    (void)ctx;
    return c2a_scc_wave_read(offset);
}

static void __not_in_flash_func(hw_scc_wave_write)(void *ctx, uint8_t offset, uint8_t data)
{
    (void)ctx;
    c2a_post(C2A_SCC_WAVE, offset, data);
}

static void __not_in_flash_func(hw_scc_reg_write)(void *ctx, uint8_t reg, uint8_t data)
{
    (void)ctx;
    c2a_post(C2A_SCC_REG, reg, data);
}

static void __not_in_flash_func(hw_scc_mode_write)(void *ctx, uint8_t data)
{
    (void)ctx;
    c2a_post(C2A_SCC_MODE, 0u, data);
}

static void __not_in_flash_func(hw_scc_plus)(void *ctx, bool enhanced)
{
    (void)ctx;
    c2a_post(C2A_SCC_PLUS, 0u, enhanced ? 1u : 0u);
}

static void __not_in_flash_func(hw_opll_write)(void *ctx, uint8_t a0, uint8_t data)
{
    (void)ctx;
    c2a_post(C2A_OPLL, a0, data);
}

static void __not_in_flash_func(hw_psg_write)(void *ctx, uint8_t a0, uint8_t data)
{
    (void)ctx;
    c2a_post(C2A_PSG, a0, data);
}

static void __not_in_flash_func(hw_beeper)(void *ctx, uint8_t level)
{
    (void)ctx;
    c2a_post(C2A_BEEPER, 0u, level);
}

static void __not_in_flash_func(hw_levels)(void *ctx, uint8_t lvl, uint8_t lvl1)
{
    (void)ctx;
    c2a_post(C2A_LEVELS, lvl, lvl1);
}

static void __not_in_flash_func(hw_eeprom_pins)(void *ctx, uint8_t cs, uint8_t ck, uint8_t di)
{
    (void)ctx;
    c2_eeprom_pins(&eeprom, cs, ck, di);
    if (eeprom.dirty)
    {
        eeprom.dirty = false;
        c2s_eeprom_changed(eeprom.data, C2_EEPROM_SIZE);
        touch_activity();
    }
}

static uint8_t __not_in_flash_func(hw_eeprom_do)(void *ctx)
{
    (void)ctx;
    return c2_eeprom_do(&eeprom);
}

static c2_hw_t c2_hw = {
    .ctx = NULL,
    .flash_read = hw_flash_read,
    .flash_write = hw_flash_write,
    .ram_read = hw_ram_read,
    .ram_write = hw_ram_write,
    .ide_read = hw_ide_read,
    .ide_write = hw_ide_write,
    .flash_ctrl = hw_flash_ctrl,
    .scc_wave_read = hw_scc_wave_read,
    .scc_wave_write = hw_scc_wave_write,
    .scc_reg_write = hw_scc_reg_write,
    .scc_mode_write = hw_scc_mode_write,
    .scc_plus = hw_scc_plus,
    .opll_write = hw_opll_write,
    .psg_write = hw_psg_write,
    .beeper = hw_beeper,
    .levels = hw_levels,
    .eeprom_pins = hw_eeprom_pins,
    .eeprom_do = hw_eeprom_do,
};

// -----------------------------------------------------------------------
// GPIO
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
    }
    gpio_init(PIN_RD);      gpio_set_dir(PIN_RD, GPIO_IN);
    gpio_init(PIN_WR);      gpio_set_dir(PIN_WR, GPIO_IN);
    gpio_init(PIN_IORQ);    gpio_set_dir(PIN_IORQ, GPIO_IN);
    gpio_init(PIN_SLTSL);   gpio_set_dir(PIN_SLTSL, GPIO_IN);
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN);
    gpio_init(PIN_PSRAM);   gpio_set_dir(PIN_PSRAM, GPIO_IN);
}

// -----------------------------------------------------------------------
// External PSRAM (QMI CS1)
// -----------------------------------------------------------------------
static inline void __not_in_flash_func(psram_delay_cycles)(uint32_t cycles)
{
    for (volatile uint32_t cycle = 0; cycle < cycles; ++cycle)
        __asm volatile ("nop");
}

static inline void __not_in_flash_func(psram_wait_direct_done)(void)
{
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) { }
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) { }
}

static inline void __not_in_flash_func(psram_send_direct_cmd)(uint8_t cmd, bool quad_width)
{
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = quad_width
        ? QMI_DIRECT_TX_OE_BITS | (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) | cmd
        : cmd;
    psram_wait_direct_done();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    (void)qmi_hw->direct_rx;
}

static bool __no_inline_not_in_flash_func(psram_init)(void)
{
    gpio_set_function(PIN_PSRAM, GPIO_FUNC_XIP_CS1);

    uint32_t irq_state = save_and_disable_interrupts();

    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) { }

    // Warm restart: the PSRAM may still be in QPI mode and keeps its contents.
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

    qmi_hw->m[1].timing =
        (QMI_M0_TIMING_PAGEBREAK_VALUE_1024 << QMI_M0_TIMING_PAGEBREAK_LSB) |
        (1u << QMI_M0_TIMING_SELECT_HOLD_LSB) |
        (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
        (C2_PSRAM_RXDELAY << QMI_M0_TIMING_RXDELAY_LSB) |
        (C2_PSRAM_MAX_SELECT << QMI_M0_TIMING_MAX_SELECT_LSB) |
        (C2_PSRAM_MIN_DESELECT << QMI_M0_TIMING_MIN_DESELECT_LSB) |
        (C2_PSRAM_CLKDIV << QMI_M0_TIMING_CLKDIV_LSB);
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

    // Verify through the uncached window.
    volatile uint32_t *psram_test = (volatile uint32_t *)0x15000000u;
    psram_test[0] = 0x12345678u;
    return psram_test[0] == 0x12345678u;
}

// -----------------------------------------------------------------------
// PIO bus
// -----------------------------------------------------------------------
static void msx_pio_bus_init(void)
{
    msx_bus.pio = pio0;
    msx_bus.sm_read = 0;
    msx_bus.sm_write = 1;

    if (!msx_bus_programs_loaded)
    {
        msx_bus.offset_read = pio_add_program(msx_bus.pio, &msx_read_responder_program);
        msx_bus.offset_write = pio_add_program(msx_bus.pio, &msx_write_captor_program);
        msx_bus_programs_loaded = true;
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    pio_sm_clear_fifos(msx_bus.pio, msx_bus.sm_read);
    pio_sm_clear_fifos(msx_bus.pio, msx_bus.sm_write);
    pio_sm_restart(msx_bus.pio, msx_bus.sm_read);
    pio_sm_restart(msx_bus.pio, msx_bus.sm_write);

    pio_sm_config cfg_read = msx_read_responder_program_get_default_config(msx_bus.offset_read);
    sm_config_set_in_pins(&cfg_read, PIN_A0);
    sm_config_set_in_shift(&cfg_read, false, false, 16);
    sm_config_set_out_pins(&cfg_read, PIN_D0, 8);
    sm_config_set_out_shift(&cfg_read, true, false, 32);
    sm_config_set_sideset_pins(&cfg_read, PIN_WAIT);
    sm_config_set_jmp_pin(&cfg_read, PIN_RD);
    sm_config_set_clkdiv(&cfg_read, 1.0f);
    pio_sm_init(msx_bus.pio, msx_bus.sm_read, msx_bus.offset_read, &cfg_read);

    pio_sm_config cfg_write = msx_write_captor_program_get_default_config(msx_bus.offset_write);
    sm_config_set_in_pins(&cfg_write, PIN_A0);
    sm_config_set_in_shift(&cfg_write, false, false, 32);
    sm_config_set_fifo_join(&cfg_write, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_write, PIN_WR);
    sm_config_set_clkdiv(&cfg_write, 1.0f);
    pio_sm_init(msx_bus.pio, msx_bus.sm_write, msx_bus.offset_write, &cfg_write);

    pio_gpio_init(msx_bus.pio, PIN_WAIT);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_read, PIN_WAIT, 1, true);

    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
        pio_gpio_init(msx_bus.pio, pin);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_read, PIN_D0, 8, false);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_write, PIN_D0, 8, false);

    gpio_put(PIN_WAIT, 1);

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, true);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, true);
}

static void msx_pio_io_bus_init(void)
{
    msx_io_bus.pio = pio1;
    msx_io_bus.sm_read = 0;
    msx_io_bus.sm_write = 1;

    if (!msx_io_programs_loaded)
    {
        msx_io_bus.offset_read = pio_add_program(msx_io_bus.pio, &msx_io_read_responder_program);
        msx_io_bus.offset_write = pio_add_program(msx_io_bus.pio, &msx_io_write_captor_program);
        msx_io_programs_loaded = true;
    }

    pio_sm_set_enabled(msx_io_bus.pio, msx_io_bus.sm_read, false);
    pio_sm_set_enabled(msx_io_bus.pio, msx_io_bus.sm_write, false);
    pio_sm_clear_fifos(msx_io_bus.pio, msx_io_bus.sm_read);
    pio_sm_clear_fifos(msx_io_bus.pio, msx_io_bus.sm_write);
    pio_sm_restart(msx_io_bus.pio, msx_io_bus.sm_read);
    pio_sm_restart(msx_io_bus.pio, msx_io_bus.sm_write);

    pio_sm_config cfg_read = msx_io_read_responder_program_get_default_config(msx_io_bus.offset_read);
    sm_config_set_in_pins(&cfg_read, PIN_A0);
    sm_config_set_in_shift(&cfg_read, false, false, 16);
    sm_config_set_out_pins(&cfg_read, PIN_D0, 8);
    sm_config_set_out_shift(&cfg_read, true, false, 32);
    sm_config_set_jmp_pin(&cfg_read, PIN_RD);
    sm_config_set_clkdiv(&cfg_read, 1.0f);
    pio_sm_init(msx_io_bus.pio, msx_io_bus.sm_read, msx_io_bus.offset_read, &cfg_read);

    pio_sm_config cfg_write = msx_io_write_captor_program_get_default_config(msx_io_bus.offset_write);
    sm_config_set_in_pins(&cfg_write, PIN_A0);
    sm_config_set_in_shift(&cfg_write, false, false, 32);
    sm_config_set_fifo_join(&cfg_write, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_write, PIN_WR);
    sm_config_set_clkdiv(&cfg_write, 1.0f);
    pio_sm_init(msx_io_bus.pio, msx_io_bus.sm_write, msx_io_bus.offset_write, &cfg_write);

    pio_sm_set_consecutive_pindirs(msx_io_bus.pio, msx_io_bus.sm_read, PIN_D0, 8, false);

    pio_sm_set_enabled(msx_io_bus.pio, msx_io_bus.sm_read, true);
    pio_sm_set_enabled(msx_io_bus.pio, msx_io_bus.sm_write, true);
}

// Token for the read state machines: bits[7:0] data, bits[15:8] pin directions.
static inline uint16_t __not_in_flash_func(pio_build_token)(bool drive, uint8_t data)
{
    return (uint16_t)data | (uint16_t)((drive ? 0xFFu : 0x00u) << 8);
}

static inline bool __not_in_flash_func(pio_try_get_write)(uint16_t *addr, uint8_t *data)
{
    if (pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_write)) return false;
    uint32_t sample = pio_sm_get(msx_bus.pio, msx_bus.sm_write);
    *addr = (uint16_t)(sample & 0xFFFFu);
    *data = (uint8_t)((sample >> 16) & 0xFFu);
    return true;
}

static inline bool __not_in_flash_func(pio_try_get_io_write)(uint8_t *port, uint8_t *data)
{
    if (pio_sm_is_rx_fifo_empty(msx_io_bus.pio, msx_io_bus.sm_write)) return false;
    uint32_t sample = pio_sm_get(msx_io_bus.pio, msx_io_bus.sm_write);
    *port = (uint8_t)(sample & 0xFFu);
    *data = (uint8_t)((sample >> 16) & 0xFFu);
    return true;
}

static inline bool __not_in_flash_func(pio_try_get_io_read)(uint8_t *port)
{
    if (pio_sm_is_rx_fifo_empty(msx_io_bus.pio, msx_io_bus.sm_read)) return false;
    *port = (uint8_t)(pio_sm_get(msx_io_bus.pio, msx_io_bus.sm_read) & 0xFFu);
    return true;
}

// -----------------------------------------------------------------------
// Cold boot handshake
// -----------------------------------------------------------------------
// The MSX BIOS probes the slots long before the firmware is ready. A tiny
// extension ROM sets the boot flag (port 0xF4 bit 7) and restarts the MSX, so
// the second boot finds the fully initialised cartridge (the original
// Carnivore2 also reboots twice after a cold start).
static void __no_inline_not_in_flash_func(wait_for_bootstrap)(void)
{
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42,            // 'AB'
        0x0A, 0x40,            // INIT = 0x400A
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xF3,                  // DI
        0xDB, 0xF4,            // IN A,(0xF4)
        0xF6, 0x80,            // OR 0x80
        0xD3, 0xF4,            // OUT (0xF4),A
        0xC7                   // RST 0
    };

    msx_pio_bus_init();

    bool restart_detected = false;
    bool init_called = false;

    while (!restart_detected)
    {
        uint16_t waddr;
        uint8_t wdata;
        while (pio_try_get_write(&waddr, &wdata)) { }

        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
            if (init_called && addr == 0x0000u)
            {
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(false, 0xFFu));
                restart_detected = true;
            }
            else
            {
                if (addr >= 0x400Au && addr <= 0x4011u) init_called = true;
                bool in_window = (addr >= 0x4000u && addr <= 0x7FFFu);
                uint8_t data = 0xFFu;
                if (in_window && (addr - 0x4000u) < sizeof(bootstrap_rom)) data = bootstrap_rom[addr - 0x4000u];
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
            }
        }
        else if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
        {
            restart_detected = true;
        }
    }
}

// -----------------------------------------------------------------------
// Core 1: microSD IDE back-end + audio rendering
// -----------------------------------------------------------------------
static void __no_inline_not_in_flash_func(core1_entry)(void)
{
    flash_safe_execute_core_init();
    sunrise_sd_task();
}

// -----------------------------------------------------------------------
// Bus loop (core 0)
// -----------------------------------------------------------------------
static inline void __not_in_flash_func(service_memory_writes)(void)
{
    uint16_t addr;
    uint8_t data;
    while (pio_try_get_write(&addr, &data))
        c2_mem_write(&c2, addr, data);
}

static inline void __not_in_flash_func(service_io)(void)
{
    uint8_t port, data;

    while (pio_try_get_io_write(&port, &data))
    {
        if (port >= 0xFCu) touch_activity();    // memory mapper page switching
        c2_io_write(&c2, port, data);
    }

    while (pio_try_get_io_read(&port))
    {
        uint8_t value = 0xFFu;
        bool drive = c2_io_read(&c2, port, &value);
        pio_sm_put_blocking(msx_io_bus.pio, msx_io_bus.sm_read, pio_build_token(drive, value));
    }
}

static inline void __not_in_flash_func(housekeeping)(void)
{
    // Delayed reconfiguration armed for the opcode fetch at address 0 (JP 0 / RST 0 / reset).
    if (c2_wants_fetch_zero(&c2) && !gpio_get(PIN_RD) && (gpio_get_all() & 0xFFFFu) == 0u)
        c2_fetch_zero(&c2);

    if (c2s_has_work())
    {
        uint32_t now = time_us_32();
        bool quiet = (now - last_activity_us) > QUIET_US && (int32_t)(now - next_commit_us) >= 0;
        if (quiet || c2s_free_slots() < 8u)
        {
            c2s_commit_step();
            next_commit_us = time_us_32() + COMMIT_SPACING_US;
        }
    }
}

static void __no_inline_not_in_flash_func(bus_loop)(void)
{
    PIO pio = msx_bus.pio;
    PIO io = msx_io_bus.pio;
    const uint sm_read = msx_bus.sm_read;
    const uint32_t read_empty = 1u << (PIO_FSTAT_RXEMPTY_LSB + msx_bus.sm_read);
    const uint32_t write_empty = 1u << (PIO_FSTAT_RXEMPTY_LSB + msx_bus.sm_write);
    const uint32_t io_empty = (1u << (PIO_FSTAT_RXEMPTY_LSB + msx_io_bus.sm_read)) |
                              (1u << (PIO_FSTAT_RXEMPTY_LSB + msx_io_bus.sm_write));

    while (true)
    {
        if ((pio->fstat & read_empty) == 0u)
        {
            uint16_t addr = (uint16_t)pio->rxf[sm_read];

            // Writes that were captured before this read must be visible to it.
            // The Z80 is held by /WAIT, so nothing new can arrive from here on.
            if ((pio->fstat & write_empty) == 0u) service_memory_writes();
            if ((io->fstat & io_empty) != io_empty) service_io();

            c2.last_page = (uint8_t)(addr >> 14);

            const c2_fast_t *page = c2_fast_get(&c2, addr);
            uint8_t data = 0xFFu;
            bool drive;

            if (page->kind == C2_FAST_FLASH)
            {
                data = c2s_flash_read(page->base + (addr & 0x1FFFu));
                drive = true;
            }
            else if (page->kind == C2_FAST_RAM)
            {
                data = c2s_ram_read(page->base + (addr & 0x1FFFu));
                drive = true;
            }
            else if (page->kind == C2_FAST_OPEN)
            {
                drive = false;
            }
            else
            {
                drive = c2_mem_read(&c2, addr, &data);
            }
            pio->txf[sm_read] = pio_build_token(drive, data);
        }
        else
        {
            if ((pio->fstat & write_empty) == 0u) service_memory_writes();
            if ((io->fstat & io_empty) != io_empty) service_io();
            housekeeping();
        }
    }
}

int __no_inline_not_in_flash_func(main)(void)
{

    // RP2350 flash timing + system clock (same as the other PicoVerse firmwares).
    qmi_hw->m[0].timing = C2_QMI_M0_TIMING;
    set_sys_clock_khz(C2_SYS_CLOCK_KHZ, true);

    setup_gpio();

    // The MSX may already be booting: answer with the restart ROM until the
    // slow initialisation below is done.
    wait_for_bootstrap();

    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_set_enabled(pio0, 1, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    psram_init();
    c2s_init();

    c2_eeprom_init(&eeprom);
    c2s_eeprom_load(eeprom.data, C2_EEPROM_SIZE);
    c2_flash_init(&flash_chip, &flash_backend);

    sunrise_ide_init(&ide);
    sunrise_sd_set_ide_ctx(&ide);

    c2a_init();
    c2_init(&c2, &c2_hw);

    msx_pio_bus_init();
    msx_pio_io_bus_init();

    c2a_start();
    multicore_launch_core1(core1_entry);

    last_activity_us = time_us_32();
    bus_loop();
    return 0;
}
