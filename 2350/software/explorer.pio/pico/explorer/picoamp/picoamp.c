// PicoAmp integration for Explorer. ROM and engine 0.12 from
// Jzzy-dev/msx-picoplay-picoamp, release 1.3 (tag 1.3, aad845d), plus the
// later news-catalogue commit 558f9c8.
// Licensed under CC BY-NC-SA 4.0, like the upstream PicoVerse Audio Engine.

#include <stdio.h>
#include <string.h>
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/regs/uart.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "../explorer.h"
#include "../audio/emu2413.h"
#include "picoamp.h"
#include "picoamp_bus.h"
#include "pvaudio/pva_internal.h"

#define ESP_RX_SIZE 4096u
static uint8_t esp_rx[ESP_RX_SIZE];
static volatile uint32_t esp_head, esp_tail;
static volatile bool esp_overflow;
static bool wifi_enabled;
static uint8_t *stream_ram;
static size_t stream_bytes;
static picoamp_bus_t cartridge;

static void __not_in_flash_func(esp_rx_irq)(void)
{
    while (uart_is_readable(uart1))
    {
        uint8_t data = (uint8_t)uart_getc(uart1);
        uint32_t next = (esp_head + 1u) & (ESP_RX_SIZE - 1u);
        if (next == esp_tail)
            esp_overflow = true;
        else
        {
            esp_rx[esp_head] = data;
            __dmb();
            esp_head = next;
        }
    }
    if (uart_get_hw(uart1)->rsr & UART_UARTRSR_OE_BITS)
    {
        uart_get_hw(uart1)->rsr = 0xFu;
        esp_overflow = true;
    }
}

bool pva_hal_net_present(void) { return wifi_enabled; }

bool pva_hal_net_open(void)
{
    if (!wifi_enabled)
        return false;
    esp_tail = esp_head;
    esp_overflow = false;
    return true;
}

void pva_hal_net_close(void)
{
    if (!wifi_enabled)
        return;
    uint32_t start = time_us_32();
    while ((uart_get_hw(uart1)->fr & UART_UARTFR_BUSY_BITS) &&
           time_us_32() - start < 5000u) { }
}

size_t pva_hal_net_write(const uint8_t *data, size_t size)
{
    size_t done = 0;
    while (wifi_enabled && done < size && uart_is_writable(uart1))
        uart_putc_raw(uart1, (char)data[done++]);
    return done;
}

size_t pva_hal_net_read(uint8_t *data, size_t size)
{
    if (esp_overflow)
    {
        esp_overflow = false;
        pva_log("ERROR: ESP UART receive overflow");
    }
    size_t done = 0;
    uint32_t tail = esp_tail;
    uint32_t head = esp_head;
    __dmb();
    while (done < size && tail != head)
    {
        data[done++] = esp_rx[tail];
        tail = (tail + 1u) & (ESP_RX_SIZE - 1u);
    }
    __dmb();
    esp_tail = tail;
    return done;
}

uint8_t *pva_hal_stream_ram(size_t *size)
{
    *size = stream_bytes;
    return stream_ram;
}

static void __not_in_flash_func(core1_player)(void)
{
    while (!pva_command_pending())
        sleep_us(50);
    pva_core1_init();
    while (true)
        if (!pva_core1_poll())
            sleep_us(50);
}

bool picoamp_prepare(const uint8_t *rom, size_t rom_size,
                     void *workspace, size_t workspace_size,
                     uint8_t *stream, size_t stream_size,
                     struct audio_buffer_pool *pool, bool wifi, uint8_t volume)
{
    if (!rom || !workspace || rom_size != PICOAMP_ROM_SIZE || rom[0] != 'A' || rom[1] != 'B' ||
        workspace_size < PICOAMP_ROM_SIZE + PICOAMP_STACK_SIZE ||
        ((uintptr_t)workspace & 7u) || !stream || stream_size < PICOAMP_STREAM_SIZE ||
        volume > 200u)
    {
        printf("PicoAmp: invalid ROM or workspace\n");
        return false;
    }
    uint8_t *arena = OPLL_borrow_table_memory(PICOAMP_ARENA_SIZE);
    if (!arena)
    {
        printf("PicoAmp: engine SRAM arena unavailable\n");
        return false;
    }
    memcpy(workspace, rom, PICOAMP_ROM_SIZE);
    cartridge.rom = workspace;
    cartridge.subslot = 0;
    cartridge.bank = 0;
    uint32_t *stack = (uint32_t *)((uint8_t *)workspace + PICOAMP_ROM_SIZE);
    memset(stack, 0xA5, PICOAMP_STACK_SIZE);
    stream_ram = stream;
    stream_bytes = PICOAMP_STREAM_SIZE;
    pva_init(arena, PICOAMP_ARENA_SIZE);
    pva_set_stack((const uint8_t *)stack, PICOAMP_STACK_SIZE);
    pva_allow_card_writes(true);
    picoamp_audio_set_volume(volume);
    picoamp_audio_adopt(pool);

    wifi_enabled = wifi;
    if (wifi)
    {
        uart_init(uart1, 859372u);
        gpio_set_function(PIN_ESP_UART_TX, GPIO_FUNC_UART_AUX);
        gpio_set_function(PIN_ESP_UART_RX, GPIO_FUNC_UART_AUX);
        uart_set_format(uart1, 8, 1, UART_PARITY_NONE);
        uart_set_hw_flow(uart1, false, false);
        uart_set_fifo_enabled(uart1, true);
        while (uart_is_readable(uart1))
            (void)uart_getc(uart1);
        uart_get_hw(uart1)->rsr = 0xFu;
        irq_set_exclusive_handler(UART1_IRQ, esp_rx_irq);
        irq_set_priority(UART1_IRQ, 0x40);
        irq_set_enabled(UART1_IRQ, true);
        uart_set_irq_enables(uart1, true, false);
    }
    multicore_launch_core1_with_stack(core1_player, stack, PICOAMP_STACK_SIZE);
    return true;
}

void __not_in_flash_func(picoamp_bootstrap)(PIO bus, uint sm_read, uint sm_write)
{
    static const uint8_t bootstrap[] = {
        0x41, 0x42, 0x0A, 0x40, 0, 0, 0, 0, 0, 0,
        0xF3, 0xDB, 0xF4, 0xF6, 0x80, 0xD3, 0xF4, 0xC7
    };
    bool init_called = false;
    while (true)
    {
        while (!pio_sm_is_rx_fifo_empty(bus, sm_write))
            (void)pio_sm_get(bus, sm_write);
        if (!pio_sm_is_rx_fifo_empty(bus, sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(bus, sm_read);
            if (init_called && addr == 0)
            {
                pio_sm_put_blocking(bus, sm_read, 0xFFu);
                return;
            }
            if (addr >= 0x400Au && addr <= 0x4011u)
                init_called = true;
            uint32_t rel = (uint16_t)(addr - 0x4000u);
            uint8_t data = rel < sizeof(bootstrap) ? bootstrap[rel] : 0xFFu;
            uint32_t drive = addr >= 0x4000u && addr <= 0x7FFFu ? 0xFF00u : 0u;
            pio_sm_put_blocking(bus, sm_read, drive | data);
        }
        else if (init_called && !gpio_get(PIN_RD) && (gpio_get_all() & 0xFFFFu) == 0)
            return;
    }
}

static inline void __not_in_flash_func(drain_writes)(PIO bus, uint sm_write)
{
    while (!pio_sm_is_rx_fifo_empty(bus, sm_write))
    {
        uint32_t sample = pio_sm_get(bus, sm_write);
        picoamp_bus_write(&cartridge, (uint16_t)sample, (uint8_t)(sample >> 16));
    }
}

void __not_in_flash_func(picoamp_run)(PIO bus, uint sm_read, uint sm_write)
{
    // All core-0 code and data below are SRAM-resident. Only core 1 uses QMI.
    while (true)
    {
        drain_writes(bus, sm_write);
        if (!pio_sm_is_rx_fifo_empty(bus, sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(bus, sm_read);
            drain_writes(bus, sm_write);
            pio_sm_put_blocking(bus, sm_read, 0xFF00u | picoamp_bus_read(&cartridge, addr));
        }
    }
}
