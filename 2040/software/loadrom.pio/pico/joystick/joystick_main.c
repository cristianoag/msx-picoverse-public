// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// joystick_main.c - Standalone USB Joystick → MSX PSG joystick firmware
//
// Turns the PicoVerse 2040 cartridge into a dedicated USB joystick adapter.
// Intercepts MSX I/O ports:
//   0xA0 (OUT) — PSG register address latch
//   0xA1 (OUT) — PSG register data write (R15 bit 6 = port select)
//   0xA2 (IN)  — PSG register data read  (R14 = joystick state)
//
// Architecture:
//   Core 0: PIO1 IRQ handler services I/O bus requests.
//           Tracks the PSG register latch and joystick port selection.
//           For R14 reads, responds with open-drain joystick data
//           (only pulls bits LOW for pressed buttons).
//           For all other register reads, tri-states the bus to let
//           the real PSG chip respond.
//   Core 1: TinyUSB host task processes USB HID gamepad reports and
//           updates the MSX joystick state for up to 2 ports.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-
// ShareAlike 4.0 International License".
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "joystick.h"
#include "hid_gamepad_parser.h"
#include "xinput_host.h"
#include "msx_joystick.pio.h"

// -----------------------------------------------------------------------
// TinyUSB custom class driver registration — enables XInput support
// -----------------------------------------------------------------------
usbh_class_driver_t const* usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &xinput_class_driver;
}

// -----------------------------------------------------------------------
// Joystick state — two ports, active-low (0xFF = idle)
// Written by Core 1, read by Core 0 (IRQ context)
// Also accessed by xinput_host.c via extern
// -----------------------------------------------------------------------
volatile uint8_t joystick_state[2] = { 0xFF, 0xFF };

// -----------------------------------------------------------------------
// PSG register tracking (updated by Core 0 from write captor)
// -----------------------------------------------------------------------
static volatile uint8_t psg_register_latch;   // Last value written to port 0xA0
static volatile uint8_t joystick_port_sel;    // 0 = port 1, 1 = port 2

// -----------------------------------------------------------------------
// Per-device gamepad context (up to CFG_TUH_HID = 4 HID interfaces)
// -----------------------------------------------------------------------
#define MAX_GAMEPADS 4

typedef struct {
    bool             active;       // Device is mounted and recognised
    uint8_t          dev_addr;     // TinyUSB device address
    uint8_t          instance;     // TinyUSB HID instance
    uint8_t          msx_port;     // Assigned MSX port (0 or 1), 0xFF = none
    gamepad_layout_t layout;       // Parsed HID report descriptor
} gamepad_dev_t;

static gamepad_dev_t gamepads[MAX_GAMEPADS];

// -----------------------------------------------------------------------
// PIO1 context
// -----------------------------------------------------------------------
#define IO_PIO      pio1
#define IO_SM_READ  0
#define IO_SM_WRITE 1

// -----------------------------------------------------------------------
// Build a 16-bit response token for the I/O read PIO:
//   bits[7:0]  = data byte
//   bits[15:8] = pindirs (0xFF = drive all, 0x00 = tri-state all,
//                          partial = open-drain)
// -----------------------------------------------------------------------
static inline uint16_t __not_in_flash_func(build_token)(uint8_t data,
                                                         uint8_t pindirs)
{
    return (uint16_t)(((uint16_t)pindirs << 8) | data);
}

// -----------------------------------------------------------------------
// Build an open-drain token from MSX joystick state byte.
//
// For pressed bits (0): drive LOW  → pindir=1, pin=0
// For idle bits   (1): tri-state   → pindir=0
//
// Only bits 0-5 are joystick signals; bits 6-7 are unused (always high).
// We tri-state all 8 data lines for unused bits so the PSG drives them.
//
// data    = 0x00 (always drive LOW when output is enabled)
// pindirs = bitmask of bits to drive = ~state & 0x3F
// -----------------------------------------------------------------------
static inline uint16_t __not_in_flash_func(build_opendrain_token)(uint8_t joy_state)
{
    uint8_t drive_mask = (~joy_state) & 0x3Fu;  // Only bits 0-5
    return build_token(0x00u, drive_mask);
}

// -----------------------------------------------------------------------
// PIO1 IRQ handler — services I/O reads and writes
// -----------------------------------------------------------------------
static void __not_in_flash_func(joystick_pio1_irq_handler)(void)
{
    // --- Handle I/O writes first (ports 0xA0, 0xA1) ---
    while (!pio_sm_is_rx_fifo_empty(IO_PIO, IO_SM_WRITE))
    {
        uint32_t sample = pio_sm_get(IO_PIO, IO_SM_WRITE);
        uint8_t port = (uint8_t)(sample & 0xFFu);
        uint8_t data = (uint8_t)((sample >> 16) & 0xFFu);

        if (port == PSG_ADDR_PORT)
        {
            // PSG register address latch
            psg_register_latch = data;
        }
        else if (port == PSG_WRITE_PORT)
        {
            // PSG data write — we only care about R15 (port select)
            if (psg_register_latch == PSG_REG_PORTB)
            {
                // Bit 6 selects joystick port: 0 = port 1, 1 = port 2
                joystick_port_sel = (data >> 6) & 0x01u;
            }
        }
    }

    // --- Handle I/O reads (port 0xA2) ---
    // The PIO asserts /WAIT and is stalled on pull — we MUST respond
    // to every read or the Z80 hangs.
    while (!pio_sm_is_rx_fifo_empty(IO_PIO, IO_SM_READ))
    {
        uint16_t addr = (uint16_t)pio_sm_get(IO_PIO, IO_SM_READ);
        uint8_t port = (uint8_t)(addr & 0xFFu);

        if (port == PSG_READ_PORT && psg_register_latch == PSG_REG_PORTA)
        {
            // R14 read — return joystick state using open-drain protocol
            uint8_t sel = joystick_port_sel;  // 0 or 1
            pio_sm_put(IO_PIO, IO_SM_READ,
                       build_opendrain_token(joystick_state[sel]));
        }
        else
        {
            // Not R14 — tri-state the bus entirely so the real PSG answers
            pio_sm_put(IO_PIO, IO_SM_READ, build_token(0xFFu, 0x00u));
        }
    }
}

// -----------------------------------------------------------------------
// GPIO initialisation — set every bus pin to a safe state before PIO
// -----------------------------------------------------------------------
static void setup_gpio(void)
{
    // Address bus A0-A15 as inputs
    for (uint pin = PIN_A0; pin <= PIN_A15; ++pin)
    {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // Data bus D0-D7 — managed by PIO later, start as input
    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
    {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // Control signals as inputs
    gpio_init(PIN_RD);      gpio_set_dir(PIN_RD, GPIO_IN);
    gpio_init(PIN_WR);      gpio_set_dir(PIN_WR, GPIO_IN);
    gpio_init(PIN_IORQ);    gpio_set_dir(PIN_IORQ, GPIO_IN);
    gpio_init(PIN_SLTSL);   gpio_set_dir(PIN_SLTSL, GPIO_IN);
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN);

    // /WAIT — open-drain: output latch low, released as input until PIO asserts it
    gpio_init(PIN_WAIT);
    gpio_put(PIN_WAIT, 0);
    gpio_set_dir(PIN_WAIT, GPIO_IN);
}

// -----------------------------------------------------------------------
// I/O bus PIO initialisation
// -----------------------------------------------------------------------
static void io_bus_init(void)
{
    // Load PIO programs
    uint offset_read  = pio_add_program(IO_PIO, &msx_joy_io_read_program);
    uint offset_write = pio_add_program(IO_PIO, &msx_joy_io_write_program);

    // --- SM0: I/O read responder (with /WAIT) ---
    pio_sm_set_enabled(IO_PIO, IO_SM_READ, false);
    pio_sm_clear_fifos(IO_PIO, IO_SM_READ);
    pio_sm_restart(IO_PIO, IO_SM_READ);

    pio_sm_config cfg_r = msx_joy_io_read_program_get_default_config(offset_read);
    sm_config_set_in_pins(&cfg_r, PIN_A0);
    sm_config_set_in_shift(&cfg_r, false, false, 16);
    sm_config_set_out_pins(&cfg_r, PIN_D0, 8);
    sm_config_set_out_shift(&cfg_r, true, false, 32);
    sm_config_set_set_pins(&cfg_r, PIN_WAIT, 1);
    sm_config_set_jmp_pin(&cfg_r, PIN_RD);
    sm_config_set_clkdiv(&cfg_r, 1.0f);
    pio_sm_init(IO_PIO, IO_SM_READ, offset_read, &cfg_r);

    // Preload /WAIT low; the PIO releases it by switching direction to input.
    pio_sm_set_pins_with_mask(IO_PIO, IO_SM_READ, 0u, (1u << PIN_WAIT));

    // Hand /WAIT to PIO1
    pio_gpio_init(IO_PIO, PIN_WAIT);
    pio_sm_set_consecutive_pindirs(IO_PIO, IO_SM_READ, PIN_WAIT, 1, false);

    // Data bus D0-D7: hand to PIO1 for bidirectional control
    for (int i = PIN_D0; i <= PIN_D7; i++)
        pio_gpio_init(IO_PIO, i);
    pio_sm_set_consecutive_pindirs(IO_PIO, IO_SM_READ, PIN_D0, 8, false);

    // --- SM1: I/O write captor ---
    pio_sm_set_enabled(IO_PIO, IO_SM_WRITE, false);
    pio_sm_clear_fifos(IO_PIO, IO_SM_WRITE);
    pio_sm_restart(IO_PIO, IO_SM_WRITE);

    pio_sm_config cfg_w = msx_joy_io_write_program_get_default_config(offset_write);
    sm_config_set_in_pins(&cfg_w, PIN_A0);
    sm_config_set_in_shift(&cfg_w, false, false, 32);
    sm_config_set_fifo_join(&cfg_w, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_w, PIN_WR);
    sm_config_set_clkdiv(&cfg_w, 1.0f);
    pio_sm_init(IO_PIO, IO_SM_WRITE, offset_write, &cfg_w);

    // --- Enable both state machines ---
    pio_sm_set_enabled(IO_PIO, IO_SM_READ,  true);
    pio_sm_set_enabled(IO_PIO, IO_SM_WRITE, true);

    // --- Route SM0+SM1 RX-not-empty to PIO1_IRQ_0 → Core 0 ---
    pio_set_irq0_source_enabled(IO_PIO, pis_sm0_rx_fifo_not_empty, true);
    pio_set_irq0_source_enabled(IO_PIO, pis_sm1_rx_fifo_not_empty, true);
    irq_set_exclusive_handler(PIO1_IRQ_0, joystick_pio1_irq_handler);
    irq_set_priority(PIO1_IRQ_0, 0);  // Highest priority
    irq_set_enabled(PIO1_IRQ_0, true);
}

// -----------------------------------------------------------------------
// Gamepad management helpers
// -----------------------------------------------------------------------

// True when a HID gamepad owns the given MSX port; shared with the Xbox driver.
bool hid_gamepad_port_in_use(uint8_t port)
{
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepads[i].active && gamepads[i].msx_port == port)
            return true;
    }
    return false;
}

static uint8_t allocate_msx_port(void)
{
    for (uint8_t port = 0; port < 2; port++) {
        if (!hid_gamepad_port_in_use(port) && !xinput_port_in_use(port))
            return port;
    }
    return 0xFF;
}

// Find the gamepad slot for a (dev_addr, instance) pair.  Returns -1 if not found.
static int find_gamepad(uint8_t dev_addr, uint8_t instance)
{
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepads[i].active &&
            gamepads[i].dev_addr == dev_addr &&
            gamepads[i].instance == instance)
            return i;
    }
    return -1;
}

// Find a free gamepad slot.  Returns -1 if all are used.
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (!gamepads[i].active)
            return i;
    }
    return -1;
}

// -----------------------------------------------------------------------
// TinyUSB HID host callbacks
// -----------------------------------------------------------------------
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len)
{
    int slot = find_free_slot();
    if (slot < 0) return;  // No free slots

    gamepad_dev_t *gp = &gamepads[slot];
    memset(gp, 0, sizeof(*gp));

    // Try to parse as a gamepad
    if (!gp_parse_descriptor(desc_report, desc_len, &gp->layout))
        return;  // Not a gamepad or unrecognised descriptor

    uint8_t port = allocate_msx_port();
    if (port == 0xFF) return;  // Both ports taken

    gp->active   = true;
    gp->dev_addr = dev_addr;
    gp->instance = instance;
    gp->msx_port = port;

    // Start receiving reports
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    int slot = find_gamepad(dev_addr, instance);
    if (slot < 0) return;

    // Release MSX port — set state to idle
    uint8_t port = gamepads[slot].msx_port;
    if (port < 2)
        joystick_state[port] = 0xFF;

    gamepads[slot].active = false;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len)
{
    int slot = find_gamepad(dev_addr, instance);
    if (slot >= 0) {
        gamepad_dev_t *gp = &gamepads[slot];
        const uint8_t *data = report;
        uint16_t data_len = len;

        // Skip report ID byte if present
        if (gp->layout.report_id != 0 && len > 0) {
            data++;
            data_len--;
        }

        uint8_t joy = gp_extract_joystick(data, data_len,
                                           &gp->layout, DEADZONE_PERCENT);

        if (gp->msx_port < 2) {
            __dmb();
            joystick_state[gp->msx_port] = joy;
            __dmb();
        }
    }

    // Re-arm for next report
    tuh_hid_receive_report(dev_addr, instance);
}

// -----------------------------------------------------------------------
// Core 1 — TinyUSB host task
// -----------------------------------------------------------------------
static void core1_entry(void)
{
    tusb_init();
    tuh_init(0);
    while (true)
        tuh_task();
}

// -----------------------------------------------------------------------
// Main — Core 0
// -----------------------------------------------------------------------
int main(void)
{
    set_sys_clock_khz(230000, true);

    // Initialise joystick state
    joystick_state[0] = 0xFF;
    joystick_state[1] = 0xFF;
    psg_register_latch = 0;
    joystick_port_sel = 0;
    memset(gamepads, 0, sizeof(gamepads));

    setup_gpio();
    io_bus_init();

    multicore_launch_core1(core1_entry);

    while (true)
        __wfi();

    return 0;
}
