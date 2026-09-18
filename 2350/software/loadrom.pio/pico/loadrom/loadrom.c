// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// loadrom.c - PIO-based ROM loader for MSX PICOVERSE project - v2.0
//
// This program loads ROM images using the MSX PICOVERSE project with the RP2350 PIO
// (Programmable I/O) hardware to handle MSX bus timing deterministically.
// The PIO state machines monitor /SLTSL and /RD//WR signals, assert /WAIT to freeze
// the Z80, and exchange address/data with the CPU through FIFOs. This frees the CPU
// from tight bit-banging loops and guarantees timing via the /WAIT mechanism.
//
// You need to concatenate the ROM image to the end of this program binary in order to load it.
// The program will then act as a simple ROM cartridge that responds to memory read requests from the MSX.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/regs/qmi.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "loadrom.h"
#include "audio/emu2212.h"
#include "audio/emu2149.h"
#include "audio/emu2413.h"
#include "msx_bus.pio.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/audio_i2s.h"
#include "storage/sunrise_ide.h"
#include "storage/sunrise_sd.h"
#include "storage/flash_sd.h"
#include "memory/c2_emu.h"

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

static msx_pio_bus_t msx_bus;
static bool msx_bus_programs_loaded = false;

// I/O bus context (PIO1) for memory mapper port access
typedef struct {
    PIO pio_read;
    PIO pio_write;
    uint sm_io_read;
    uint sm_io_write;
    uint offset_io_read;
    uint offset_io_write;
} msx_pio_io_bus_t;

static msx_pio_io_bus_t msx_io_bus;
static bool msx_io_bus_programs_loaded = false;

// Tracks how many bytes of the ROM are cached in SRAM (0 = no cache)
static uint32_t rom_cached_size = 0;

// Effective SRAM cache capacity (may be reduced by mappers that reserve
// part of the SRAM pool for writable flash sectors, e.g. Manbow2).
static uint32_t rom_cache_capacity = CACHE_SIZE;

// -----------------------------------------------------------------------
// SCC emulation state + I2S audio
// -----------------------------------------------------------------------
#define SCC_VOLUME_SHIFT 2  // Left-shift SCC output for volume boost (4x)
#define SCC_AUDIO_BUFFER_SAMPLES 256

static SCC scc_instance;
static struct audio_buffer_pool *audio_pool;
static bool scc_audio_ready = false;
static int scc_dma_channel = -1;

// -----------------------------------------------------------------------
// Dual PSG emulation state
// -----------------------------------------------------------------------
static PSG psg_instance;
static bool dual_psg_enabled = false;
static bool dual_psg_ready = false;
static bool dual_psg_audio_started = false;
static volatile bool dual_psg_io_core1 = false;

// -----------------------------------------------------------------------
// MSX-MUSIC / YM2413 emulation state
// -----------------------------------------------------------------------
static OPLL *msx_music_instance = NULL;
static spin_lock_t *msx_music_lock = NULL;
static bool msx_music_enabled = false;
static bool msx_music_ready = false;
static bool msx_music_audio_started = false;
static volatile bool msx_music_io_core1 = false;

// -----------------------------------------------------------------------
// Cartridge audio mode
// -----------------------------------------------------------------------
// Exactly ONE audio option is active per ROM. Future sound chips (OPLL,
// MSX-AUDIO, etc.) plug in by adding a new enumerator and a case in the
// audio init switch in main(); they MUST remain mutually exclusive.
typedef enum {
    AUDIO_MODE_NONE = 0,    // No on-cartridge audio emulation
    AUDIO_MODE_SCC,         // Konami SCC (standard) via I2S
    AUDIO_MODE_SCC_PLUS,    // Konami SCC+ (enhanced) via I2S
    AUDIO_MODE_DUAL_PSG,    // Secondary AY-3-8910 on I/O 0x10/0x11 via I2S
    AUDIO_MODE_MSX_MUSIC,    // MSX-MUSIC YM2413 on I/O 0x7C/0x7D via I2S
} audio_mode_t;

// Resolve which audio engine to activate for this cartridge. Audio flags are
// mutually exclusive tool choices, and SCC-class mappers keep their dedicated paths.
static inline audio_mode_t resolve_audio_mode(uint8_t rom_type, uint8_t base_rom_type, bool system_mode)
{
    if (system_mode)
        return AUDIO_MODE_NONE;

    bool scc_capable    = (base_rom_type == 3u || base_rom_type == 14u);
    bool scc_flag       = (rom_type & SCC_FLAG)      != 0;
    bool scc_plus_flag  = (rom_type & SCC_PLUS_FLAG) != 0;
    bool dual_psg_flag  = (rom_type & DUAL_PSG_FLAG) != 0;
    bool msx_music_flag = (rom_type & MSX_MUSIC_FLAG) != 0;

    if (scc_capable && scc_plus_flag) return AUDIO_MODE_SCC_PLUS;
    if (scc_capable && scc_flag)      return AUDIO_MODE_SCC;
    if (dual_psg_flag)                return AUDIO_MODE_DUAL_PSG;
    if (msx_music_flag && !scc_capable) return AUDIO_MODE_MSX_MUSIC;
    return AUDIO_MODE_NONE;
}

static inline bool is_system_rom_type(uint8_t base_rom_type)
{
    return base_rom_type >= 10u && base_rom_type <= 20u &&
           base_rom_type != 12u && base_rom_type != 13u && base_rom_type != 14u;
}

static inline bool is_embedded_nextor_record(const char *rom_name)
{
    static const char prefix[] = "Nextor ";
    return memcmp(rom_name, prefix, sizeof(prefix) - 1u) == 0;
}

static inline uint8_t decode_base_rom_type(uint8_t rom_type, const char *rom_name)
{
    uint8_t system_candidate = rom_type & ~(SCC_FLAG | SCC_PLUS_FLAG | WIFI_FLAG);
    if (is_system_rom_type(system_candidate) && is_embedded_nextor_record(rom_name))
        return system_candidate;

    return rom_type & ~(SCC_FLAG | SCC_PLUS_FLAG | DUAL_PSG_FLAG | MSX_MUSIC_FLAG);
}

// -----------------------------------------------------------------------
// Sunrise WiFi memio backend
// -----------------------------------------------------------------------
#define WIFI_MEM_F2_ADDR      0x7F05u
#define WIFI_MEM_CMD_ADDR     0x7F06u
#define WIFI_MEM_DATA_ADDR    0x7F07u
#define WIFI_RX_FIFO_SIZE     2080u
#define WIFI_QUICK_WAIT_US    25000u
#define WIFI_UART_DEFAULT_BAUD 859372u
#define WIFI_STATUS_RX_READY  0x01u
#define WIFI_STATUS_TX_BUSY   0x02u
#define WIFI_STATUS_RX_FULL   0x04u
#define WIFI_STATUS_QUICK_RX  0x08u
#define WIFI_STATUS_UNDERRUN  0x10u
#define WIFI_STATUS_FREE_BITS 0x80u

// Hardware UART1 backs the ESP-01 link on Core2350B GPIO38/39.
// On RP2350B these are UART1 TX/RX on FUNCSEL=11 (GPIO_FUNC_UART_AUX).
// FUNCSEL=2 on these pins maps to UART1 CTS/RTS, NOT data.
#define WIFI_UART_INSTANCE uart1
static uint8_t wifi_rx_fifo[WIFI_RX_FIFO_SIZE];
static volatile uint16_t wifi_rx_head = 0u;
static volatile uint16_t wifi_rx_tail = 0u;
static volatile uint16_t wifi_rx_count = 0u;
static uint8_t wifi_f2_state = 0xFFu;
static bool wifi_uart_ready = false;
static uint32_t wifi_uart_baud = WIFI_UART_DEFAULT_BAUD;
static uint32_t wifi_tx_busy_deadline_us = 0u;
static bool wifi_rx_underrun = false;

static inline void __not_in_flash_func(wifi_reset_fifo)(void)
{
    uint32_t irq_state = save_and_disable_interrupts();
    wifi_rx_head = 0u;
    wifi_rx_tail = 0u;
    wifi_rx_count = 0u;
    wifi_rx_underrun = false;
    restore_interrupts(irq_state);
}

static inline void __not_in_flash_func(wifi_push_rx_byte)(uint8_t data)
{
    // Producer side: called from UART RX IRQ on core0. The pop side
    // disables IRQs while it mutates head/tail/count, so we don't race.
    if (wifi_rx_count >= WIFI_RX_FIFO_SIZE)
    {
        return;
    }

    wifi_rx_fifo[wifi_rx_head] = data;
    wifi_rx_head = (uint16_t)((wifi_rx_head + 1u) % WIFI_RX_FIFO_SIZE);
    ++wifi_rx_count;
}

static inline bool __not_in_flash_func(wifi_pop_rx_byte)(uint8_t *data_out)
{
    uint32_t irq_state = save_and_disable_interrupts();
    if (wifi_rx_count == 0u)
    {
        restore_interrupts(irq_state);
        return false;
    }

    *data_out = wifi_rx_fifo[wifi_rx_tail];
    wifi_rx_tail = (uint16_t)((wifi_rx_tail + 1u) % WIFI_RX_FIFO_SIZE);
    --wifi_rx_count;
    restore_interrupts(irq_state);
    return true;
}

static inline bool __not_in_flash_func(wifi_hw_tx_busy)(void)
{
    return (uart_get_hw(WIFI_UART_INSTANCE)->fr & UART_UARTFR_BUSY_BITS) != 0u;
}

static inline void __not_in_flash_func(wifi_hw_rx_drain)(void)
{
    while (uart_is_readable(WIFI_UART_INSTANCE))
    {
        (void)uart_getc(WIFI_UART_INSTANCE);
    }
}

static inline void __not_in_flash_func(wifi_pio_rx_reset)(void)
{
    // Drain any in-flight bytes; FIFO will refill from the wire.
    wifi_hw_rx_drain();
}

static inline void __not_in_flash_func(wifi_pio_tx_reset)(void)
{
    // Wait for the TX shifter to flush so we don't truncate a frame.
    while (wifi_hw_tx_busy())
    {
    }
    wifi_tx_busy_deadline_us = 0u;
}

static inline void __not_in_flash_func(wifi_service_rx)(void)
{
    if (!wifi_uart_ready)
    {
        return;
    }

    while (wifi_rx_count < WIFI_RX_FIFO_SIZE && uart_is_readable(WIFI_UART_INSTANCE))
    {
        wifi_push_rx_byte((uint8_t)uart_getc(WIFI_UART_INSTANCE));
    }
}

// IRQ handler: drains UART1 RX hardware FIFO into the SW FIFO. Runs on
// core0 (the core that calls wifi_uart_init_once). Must be in RAM so it
// is not blocked by XIP cache misses while the bus loop is busy.
static void __not_in_flash_func(wifi_uart_rx_irq)(void)
{
    while (uart_is_readable(WIFI_UART_INSTANCE))
    {
        uint8_t b = (uint8_t)uart_getc(WIFI_UART_INSTANCE);
        if (wifi_rx_count < WIFI_RX_FIFO_SIZE)
        {
            wifi_rx_fifo[wifi_rx_head] = b;
            wifi_rx_head = (uint16_t)((wifi_rx_head + 1u) % WIFI_RX_FIFO_SIZE);
            ++wifi_rx_count;
        }
    }
}

static inline uint32_t __not_in_flash_func(wifi_uart_frame_time_us)(void)
{
    return (10000000u + (wifi_uart_baud - 1u)) / wifi_uart_baud;
}

static inline void __not_in_flash_func(wifi_uart_init_once)(void)
{
    if (wifi_uart_ready)
    {
        return;
    }

    // Initialise hardware UART1 on GPIO38 (TX) / GPIO39 (RX). Both pins
    // need GPIO_FUNC_UART_AUX (FUNCSEL=11) on RP2350B; the default
    // GPIO_FUNC_UART (FUNCSEL=2) maps to UART1 CTS/RTS on these pins.
    (void)uart_init(WIFI_UART_INSTANCE, wifi_uart_baud);
    gpio_set_function(PIN_ESP_UART_TX, GPIO_FUNC_UART_AUX);
    gpio_set_function(PIN_ESP_UART_RX, GPIO_FUNC_UART_AUX);
    uart_set_format(WIFI_UART_INSTANCE, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(WIFI_UART_INSTANCE, false, false);
    uart_set_fifo_enabled(WIFI_UART_INSTANCE, true);

    // Drain any spurious bytes the line may have latched during init.
    wifi_hw_rx_drain();

    wifi_f2_state = 0xFFu;
    wifi_reset_fifo();
    wifi_tx_busy_deadline_us = 0u;
    wifi_uart_ready = true;

    // IRQ-driven RX: at 859372 baud the 32-byte HW FIFO fills in ~370us.
    // Polling from the bus service loop is not reliable when the loop
    // stalls on pio_sm_put_blocking, so route the RX/RT interrupts to a
    // dedicated handler that drains the FIFO immediately. RT (receive
    // timeout) ensures partial bursts don't sit in the FIFO.
    int uart_irq = (WIFI_UART_INSTANCE == uart0) ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(uart_irq, wifi_uart_rx_irq);
    irq_set_enabled(uart_irq, true);
    uart_set_irq_enables(WIFI_UART_INSTANCE, true, false);
}

static inline void __not_in_flash_func(wifi_handle_cmd_write)(uint8_t cmd)
{
    wifi_uart_init_once();

    if (cmd == 20u)
    {
        wifi_reset_fifo();
        wifi_pio_tx_reset();
        wifi_pio_rx_reset();
    }
}

static inline uint8_t __not_in_flash_func(wifi_status_read)(void)
{
    wifi_uart_init_once();
    wifi_service_rx();

    uint8_t status = WIFI_STATUS_QUICK_RX | WIFI_STATUS_FREE_BITS;
    if (wifi_rx_count != 0u)
    {
        status |= WIFI_STATUS_RX_READY;
    }
    if (wifi_rx_count >= WIFI_RX_FIFO_SIZE)
    {
        status |= WIFI_STATUS_RX_FULL;
    }
    if (wifi_hw_tx_busy() ||
        !uart_is_writable(WIFI_UART_INSTANCE) ||
        (int32_t)(time_us_32() - wifi_tx_busy_deadline_us) < 0)
    {
        status |= WIFI_STATUS_TX_BUSY;
    }
    if (wifi_rx_underrun)
    {
        status |= WIFI_STATUS_UNDERRUN;
        wifi_rx_underrun = false;
    }

    return status;
}

static inline uint8_t __not_in_flash_func(wifi_data_read)(void)
{
    wifi_uart_init_once();

    uint8_t data;
    wifi_service_rx();
    if (wifi_pop_rx_byte(&data))
    {
        return data;
    }

    uint32_t deadline = time_us_32() + WIFI_QUICK_WAIT_US;
    while ((int32_t)(time_us_32() - deadline) < 0)
    {
        wifi_service_rx();
        if (wifi_pop_rx_byte(&data))
        {
            return data;
        }
    }

    wifi_rx_underrun = true;
    return 0xFFu;
}

static inline bool __not_in_flash_func(wifi_handle_mem_write)(uint16_t addr, uint8_t data)
{
    if (addr == WIFI_MEM_F2_ADDR)
    {
        wifi_f2_state = data;
        return true;
    }
    if (addr == WIFI_MEM_CMD_ADDR)
    {
        wifi_handle_cmd_write(data);
        return true;
    }
    if (addr == WIFI_MEM_DATA_ADDR)
    {
        wifi_uart_init_once();
        uart_putc_raw(WIFI_UART_INSTANCE, (char)data);
        wifi_tx_busy_deadline_us = time_us_32() + wifi_uart_frame_time_us();
        return true;
    }

    return false;
}

static inline uint8_t __not_in_flash_func(wifi_read_rom_byte)(const uint8_t *wifi_rom_base, uint16_t addr)
{
    uint32_t rel = (uint32_t)(addr - 0x4000u);
    if (rel < WIFI_ROM_SIZE)
    {
        return wifi_rom_base[rel];
    }

    return 0xFFu;
}

static inline uint8_t __not_in_flash_func(wifi_handle_mem_read)(const uint8_t *wifi_rom_base, uint16_t addr)
{
    if (addr == WIFI_MEM_F2_ADDR)
    {
        return wifi_f2_state;
    }
    if (addr == WIFI_MEM_CMD_ADDR)
    {
        return wifi_data_read();
    }
    if (addr == WIFI_MEM_DATA_ADDR)
    {
        return wifi_status_read();
    }

    return wifi_read_rom_byte(wifi_rom_base, addr);
}

// -----------------------------------------------------------------------
// External PSRAM mapper backing (QMI CS1)
// -----------------------------------------------------------------------
static inline void __not_in_flash_func(psram_delay_cycles)(uint32_t cycles)
{
    for (volatile uint32_t cycle = 0; cycle < cycles; ++cycle)
    {
        __asm volatile ("nop");
    }
}

static inline void __not_in_flash_func(psram_wait_direct_done)(void)
{
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0)
    {
    }
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
    {
    }
}

static inline void __not_in_flash_func(psram_send_direct_cmd)(uint8_t cmd, bool quad_width)
{
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    if (quad_width)
    {
        qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                            (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                            cmd;
    }
    else
    {
        qmi_hw->direct_tx = cmd;
    }
    psram_wait_direct_done();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    (void)qmi_hw->direct_rx;
}

static bool __no_inline_not_in_flash_func(psram_init)(void)
{
    gpio_set_function(PIN_PSRAM, GPIO_FUNC_XIP_CS1);

    uint32_t irq_state = save_and_disable_interrupts();

    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
    {
    }

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

    // PSRAM QMI timing — tuned for 210 MHz system clock.
    // CLKDIV=2 → SCK = 52.5 MHz (safe for all APS6404L/IPS6404L grades).
    // SELECT_HOLD=1 → 4.76 ns ≥ 3 ns tCSS.
    // MIN_DESELECT=5 → 23.8 ns ≥ 18 ns tCPH.
    // RXDELAY=2 → sample at 9.52 ns after SCK rise (mid-cycle at CLKDIV=2).
    // Performance comes from the XIP cache (PSRAM_BASE_ADDR = 0x11000000,
    // cached write-through window) rather than raw QSPI throughput.
    qmi_hw->m[1].timing =
        (QMI_M0_TIMING_PAGEBREAK_VALUE_1024 << QMI_M0_TIMING_PAGEBREAK_LSB) |
        (1u << QMI_M0_TIMING_SELECT_HOLD_LSB) |
        (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
        (2u << QMI_M0_TIMING_RXDELAY_LSB) |
        (26u << QMI_M0_TIMING_MAX_SELECT_LSB) |
        (5u << QMI_M0_TIMING_MIN_DESELECT_LSB) |
        (2u << QMI_M0_TIMING_CLKDIV_LSB);
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

    // Verify PSRAM is functional via uncached window (bypass XIP cache
    // to guarantee the write reaches the device and the read comes back
    // from the device, not from a stale cache line).
    volatile uint32_t *psram_test = (volatile uint32_t *)0x15000000u;
    psram_test[0] = 0x12345678u;
    return psram_test[0] == 0x12345678u;
}

// -----------------------------------------------------------------------
// PSRAM Memory Manager — bump allocator for 8MB external PSRAM
// -----------------------------------------------------------------------
static psram_region_t mapper_region;
static psram_region_t c2_rom_region;
static psram_region_t megaram_region;
static psram_region_t flash_region;

static struct {
    uint32_t next_free;
} psram_mgr;

static void psram_mem_init(void)
{
    psram_mgr.next_free = 0;
    memset(&mapper_region, 0, sizeof(mapper_region));
    memset(&c2_rom_region, 0, sizeof(c2_rom_region));
    memset(&megaram_region, 0, sizeof(megaram_region));
    memset(&flash_region, 0, sizeof(flash_region));
}

static bool psram_alloc(uint32_t size, psram_region_t *region)
{
    if (psram_mgr.next_free + size > PSRAM_TOTAL_SIZE)
        return false;
    region->offset = psram_mgr.next_free;
    region->size = size;
    region->ptr = (uint8_t *)(PSRAM_BASE_ADDR + psram_mgr.next_free);
    psram_mgr.next_free += size;
    return true;
}

static void psram_free_all(void)
{
    psram_mgr.next_free = 0;
    memset(&mapper_region, 0, sizeof(mapper_region));
    memset(&c2_rom_region, 0, sizeof(c2_rom_region));
    memset(&megaram_region, 0, sizeof(megaram_region));
    memset(&flash_region, 0, sizeof(flash_region));
}

// -----------------------------------------------------------------------
// PSRAM region helpers — mapper RAM
// -----------------------------------------------------------------------
static void __not_in_flash_func(mapper_fill_ff)(void)
{
    uint32_t *words = (uint32_t *)mapper_region.ptr;
    for (uint32_t index = 0; index < (mapper_region.size / sizeof(uint32_t)); ++index)
    {
        words[index] = 0xFFFFFFFFu;
    }
}

static inline void __not_in_flash_func(mapper_write_byte)(uint32_t offset, uint8_t data)
{
    mapper_region.ptr[offset] = data;
}

static inline uint8_t __not_in_flash_func(mapper_read_byte)(uint32_t offset)
{
    return mapper_region.ptr[offset];
}

static void __not_in_flash_func(megaram_fill_ff)(void)
{
    uint32_t *words = (uint32_t *)megaram_region.ptr;
    for (uint32_t index = 0; index < (megaram_region.size / sizeof(uint32_t)); ++index)
    {
        words[index] = 0xFFFFFFFFu;
    }
}

static inline uint8_t __not_in_flash_func(megaram_page_from_addr)(uint16_t addr)
{
    return (uint8_t)((addr >> 13) & 0x03u);
}

static inline bool __not_in_flash_func(megaram_bank_switch_write)(uint16_t addr, uint8_t data, uint8_t bank_reg[4])
{
    bank_reg[megaram_page_from_addr(addr)] = data & 0x7Fu;
    return true;
}

static inline void __not_in_flash_func(megaram_write_byte)(uint16_t addr, uint8_t data, const uint8_t bank_reg[4])
{
    uint8_t page = megaram_page_from_addr(addr);
    uint32_t offset = ((uint32_t)(bank_reg[page] & 0x7Fu) << 13) | (addr & 0x1FFFu);
    megaram_region.ptr[offset] = data;
}

static inline uint8_t __not_in_flash_func(megaram_read_byte)(uint16_t addr, const uint8_t bank_reg[4])
{
    uint8_t page = megaram_page_from_addr(addr);
    uint32_t offset = ((uint32_t)(bank_reg[page] & 0x7Fu) << 13) | (addr & 0x1FFFu);
    return megaram_region.ptr[offset];
}

// -----------------------------------------------------------------------
// ROM source preparation (cache to SRAM, flash fallback for large ROMs)
// -----------------------------------------------------------------------
// For ROMs that fit in the 192KB SRAM cache, the entire ROM is copied to
// SRAM and rom_base is redirected there. For ROMs larger than the cache,
// the first 192KB is cached in SRAM and rom_base stays pointing to flash.
// read_rom_byte() transparently serves from SRAM for offsets within the
// cached region and falls back to flash XIP for the rest.
static inline void __not_in_flash_func(prepare_rom_source)(
    uint32_t offset,
    bool cache_enable,
    uint32_t preferred_size,
    const uint8_t **rom_base_out,
    uint32_t *available_length_out)
{
    const uint8_t *rom_base = rom + offset;
    uint32_t available_length = active_rom_size;

    if (preferred_size != 0u && (available_length == 0u || available_length > preferred_size))
    {
        available_length = preferred_size;
    }

    if (cache_enable && available_length > 0u)
    {
        uint32_t bytes_to_cache = (available_length > rom_cache_capacity)
                                  ? rom_cache_capacity
                                  : available_length;

        gpio_init(PIN_WAIT);
        gpio_set_dir(PIN_WAIT, GPIO_OUT);
        gpio_put(PIN_WAIT, 0);

        // DMA bulk copy from flash/PSRAM XIP to SRAM.
        // Byte transfers are used because rom_base may not be 4-byte aligned
        // (ROM data starts at __flash_binary_end + 55-byte header).
        int dma_chan = dma_claim_unused_channel(true);
        dma_channel_config dma_cfg = dma_channel_get_default_config(dma_chan);
        channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
        channel_config_set_read_increment(&dma_cfg, true);
        channel_config_set_write_increment(&dma_cfg, true);
        dma_channel_configure(dma_chan, &dma_cfg,
            rom_sram,                        // write address (SRAM)
            rom_base,                        // read address (flash/PSRAM XIP)
            bytes_to_cache,                  // transfer count (bytes)
            true);                           // start immediately
        dma_channel_wait_for_finish_blocking(dma_chan);
        dma_channel_unclaim(dma_chan);
        // WAIT stays asserted; msx_pio_bus_init() will release it
        // atomically when the read SM starts (side-set 1 on first
        // instruction), guaranteeing PIO is ready before the MSX resumes.

        rom_cached_size = bytes_to_cache;

        if (available_length <= rom_cache_capacity)
        {
            // Entire ROM fits in SRAM cache
            rom_base = rom_sram;
        }
    }
    else
    {
        rom_cached_size = 0;
    }

    *rom_base_out = rom_base;
    *available_length_out = available_length;
}

static inline void __not_in_flash_func(prepare_sunrise_mapper_rom_source)(
    uint32_t offset,
    bool cache_enable,
    const uint8_t **rom_base_out,
    uint32_t *available_length_out)
{
    // On RP2350 the mapper RAM lives in external PSRAM, so the internal
    // 256KB SRAM cache can be used for the embedded Nextor ROM as well.
    // The caller has already frozen the MSX bus with /WAIT asserted, so
    // this path must not release /WAIT while the cache DMA is running.
    const uint8_t *rom_base = rom + offset;
    uint32_t available_length = active_rom_size;

    rom_cache_capacity = CACHE_SIZE;

    if (cache_enable && available_length > 0u)
    {
        uint32_t bytes_to_cache = (available_length > rom_cache_capacity)
                                  ? rom_cache_capacity
                                  : available_length;

        int dma_chan = dma_claim_unused_channel(true);
        dma_channel_config dma_cfg = dma_channel_get_default_config(dma_chan);
        channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
        channel_config_set_read_increment(&dma_cfg, true);
        channel_config_set_write_increment(&dma_cfg, true);
        dma_channel_configure(dma_chan, &dma_cfg,
            rom_sram,
            rom_base,
            bytes_to_cache,
            true);
        dma_channel_wait_for_finish_blocking(dma_chan);
        dma_channel_unclaim(dma_chan);

        rom_cached_size = bytes_to_cache;

        if (available_length <= rom_cache_capacity)
        {
            rom_base = rom_sram;
        }
    }
    else
    {
        rom_cached_size = 0;
    }

    *rom_base_out = rom_base;
    *available_length_out = available_length;
}

// -----------------------------------------------------------------------
// GPIO initialisation (address, data, control pins)
// -----------------------------------------------------------------------
static inline void setup_gpio(void)
{
    // Address pins A0-A15 as inputs
    for (uint pin = PIN_A0; pin <= PIN_A15; ++pin)
    {
        gpio_init(pin);
        gpio_set_input_hysteresis_enabled(pin, true);
        gpio_set_dir(pin, GPIO_IN);
    }

    // Data pins D0-D7 (will be managed by PIO)
    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
    {
        gpio_init(pin);
        gpio_set_input_hysteresis_enabled(pin, true);
    }

    // Control signals as inputs
    gpio_init(PIN_RD);      gpio_set_dir(PIN_RD, GPIO_IN);
    gpio_init(PIN_WR);      gpio_set_dir(PIN_WR, GPIO_IN);
    gpio_init(PIN_IORQ);    gpio_set_dir(PIN_IORQ, GPIO_IN);
    gpio_init(PIN_SLTSL);   gpio_set_dir(PIN_SLTSL, GPIO_IN);
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN);
    gpio_init(PIN_PSRAM);   gpio_set_dir(PIN_PSRAM, GPIO_IN);
}

// -----------------------------------------------------------------------
// PIO bus initialisation
// -----------------------------------------------------------------------
static void msx_pio_bus_init(void)
{
    msx_bus.pio = pio0;
    msx_bus.sm_read  = 0;
    msx_bus.sm_write = 1;

    if (!msx_bus_programs_loaded)
    {
        msx_bus.offset_read  = pio_add_program(msx_bus.pio, &msx_read_responder_program);
        msx_bus.offset_write = pio_add_program(msx_bus.pio, &msx_write_captor_program);
        msx_bus_programs_loaded = true;
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    pio_sm_clear_fifos(msx_bus.pio, msx_bus.sm_read);
    pio_sm_clear_fifos(msx_bus.pio, msx_bus.sm_write);
    pio_sm_restart(msx_bus.pio, msx_bus.sm_read);
    pio_sm_restart(msx_bus.pio, msx_bus.sm_write);

    // ----- Read responder SM (SM0) -----
    pio_sm_config cfg_read = msx_read_responder_program_get_default_config(msx_bus.offset_read);
    sm_config_set_in_pins(&cfg_read, PIN_A0);                // in base = GPIO 0
    sm_config_set_in_shift(&cfg_read, false, false, 16);     // shift left, no autopush, 16 bits
    sm_config_set_out_pins(&cfg_read, PIN_D0, 8);            // out base = GPIO 16, 8 pins
    sm_config_set_out_shift(&cfg_read, true, false, 32);     // shift right (LSB first), no autopull
    sm_config_set_sideset_pins(&cfg_read, PIN_WAIT);         // side-set = GPIO 28
    sm_config_set_jmp_pin(&cfg_read, PIN_RD);                // jmp pin = /RD for polling
    sm_config_set_clkdiv(&cfg_read, 1.0f);                   // Run at full system clock
    pio_sm_init(msx_bus.pio, msx_bus.sm_read, msx_bus.offset_read, &cfg_read);

    // ----- Write captor SM (SM1) -----
    pio_sm_config cfg_write = msx_write_captor_program_get_default_config(msx_bus.offset_write);
    sm_config_set_in_pins(&cfg_write, PIN_A0);               // in base = GPIO 0
    sm_config_set_in_shift(&cfg_write, false, false, 32);    // shift left, no autopush, 32 bits
    sm_config_set_fifo_join(&cfg_write, PIO_FIFO_JOIN_RX);   // Join FIFOs for 8-deep RX buffer
    sm_config_set_jmp_pin(&cfg_write, PIN_WR);               // jmp pin = /WR for polling
    sm_config_set_clkdiv(&cfg_write, 1.0f);
    pio_sm_init(msx_bus.pio, msx_bus.sm_write, msx_bus.offset_write, &cfg_write);

    // ----- Pin configuration for PIO -----
    // /WAIT pin: PIO side-set output, initially deasserted (high)
    pio_gpio_init(msx_bus.pio, PIN_WAIT);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_read, PIN_WAIT, 1, true);

    // Data pins: hand over to PIO, initially tri-stated (input)
    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
    {
        pio_gpio_init(msx_bus.pio, pin);
    }
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_read, PIN_D0, 8, false);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_write, PIN_D0, 8, false);

    // Ensure /WAIT starts high before enabling state machines
    gpio_put(PIN_WAIT, 1);

    // Enable both state machines
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, true);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, true);
}

// -----------------------------------------------------------------------
// PIO I/O bus initialisation (for memory mapper port access on PIO1)
// -----------------------------------------------------------------------
static void msx_pio_io_bus_init(void)
{
    msx_io_bus.pio_read = pio1;
    msx_io_bus.pio_write = pio1;
    msx_io_bus.sm_io_read  = 0;
    msx_io_bus.sm_io_write = 1;

    if (!msx_io_bus_programs_loaded)
    {
        msx_io_bus.offset_io_read = pio_add_program(msx_io_bus.pio_read, &msx_io_read_responder_program);
        msx_io_bus.offset_io_write = pio_add_program(msx_io_bus.pio_write, &msx_io_write_captor_program);
        msx_io_bus_programs_loaded = true;
    }

    pio_sm_set_enabled(msx_io_bus.pio_read, msx_io_bus.sm_io_read, false);
    pio_sm_set_enabled(msx_io_bus.pio_write, msx_io_bus.sm_io_write, false);
    pio_sm_clear_fifos(msx_io_bus.pio_read, msx_io_bus.sm_io_read);
    pio_sm_clear_fifos(msx_io_bus.pio_write, msx_io_bus.sm_io_write);
    pio_sm_restart(msx_io_bus.pio_read, msx_io_bus.sm_io_read);
    pio_sm_restart(msx_io_bus.pio_write, msx_io_bus.sm_io_write);

    pio_sm_config cfg_io_read = msx_io_read_responder_program_get_default_config(msx_io_bus.offset_io_read);
    sm_config_set_in_pins(&cfg_io_read, PIN_A0);
    sm_config_set_in_shift(&cfg_io_read, false, false, 16);
    sm_config_set_out_pins(&cfg_io_read, PIN_D0, 8);
    sm_config_set_out_shift(&cfg_io_read, true, false, 32);
    sm_config_set_jmp_pin(&cfg_io_read, PIN_RD);
    sm_config_set_clkdiv(&cfg_io_read, 1.0f);
    pio_sm_init(msx_io_bus.pio_read, msx_io_bus.sm_io_read, msx_io_bus.offset_io_read, &cfg_io_read);

    pio_sm_config cfg_io_write = msx_io_write_captor_program_get_default_config(msx_io_bus.offset_io_write);
    sm_config_set_in_pins(&cfg_io_write, PIN_A0);
    sm_config_set_in_shift(&cfg_io_write, false, false, 32);
    sm_config_set_fifo_join(&cfg_io_write, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_io_write, PIN_WR);
    sm_config_set_clkdiv(&cfg_io_write, 1.0f);
    pio_sm_init(msx_io_bus.pio_write, msx_io_bus.sm_io_write, msx_io_bus.offset_io_write, &cfg_io_write);

    pio_sm_set_consecutive_pindirs(msx_io_bus.pio_read, msx_io_bus.sm_io_read, PIN_D0, 8, false);

    pio_sm_set_enabled(msx_io_bus.pio_read, msx_io_bus.sm_io_read, true);
    pio_sm_set_enabled(msx_io_bus.pio_write, msx_io_bus.sm_io_write, true);
}

// -----------------------------------------------------------------------
// Token helpers
// -----------------------------------------------------------------------

// Read a byte from the ROM, using SRAM cache when possible, flash otherwise.
static inline uint8_t __not_in_flash_func(read_rom_byte)(const uint8_t *rom_base, uint32_t rel)
{
    return (rel < rom_cached_size) ? rom_sram[rel] : rom_base[rel];
}

// Build a 16-bit token to send back to the read SM via TX FIFO.
//   bits[7:0]  = data byte
//   bits[15:8] = pindirs mask (0xFF = drive bus, 0x00 = tri-state)
static inline uint16_t __not_in_flash_func(pio_build_token)(bool drive, uint8_t data)
{
    uint8_t dir_mask = drive ? 0xFFu : 0x00u;
    return (uint16_t)data | ((uint16_t)dir_mask << 8);
}

// Try to consume a write event from the write captor FIFO.
// Returns false if FIFO is empty.
static inline bool __not_in_flash_func(pio_try_get_write)(uint16_t *addr_out, uint8_t *data_out)
{
    if (pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_write))
        return false;

    uint32_t sample = pio_sm_get(msx_bus.pio, msx_bus.sm_write);
    *addr_out = (uint16_t)(sample & 0xFFFFu);
    *data_out = (uint8_t)((sample >> 16) & 0xFFu);
    return true;
}

// Drain all pending write events, invoking a handler for each.
static inline void __not_in_flash_func(pio_drain_writes)(void (*handler)(uint16_t addr, uint8_t data, void *ctx), void *ctx)
{
    uint16_t addr;
    uint8_t data;
    while (pio_try_get_write(&addr, &data))
    {
        handler(addr, data, ctx);
    }
}

// Try to consume an I/O write event from the I/O write captor FIFO (PIO1).
// Returns false if FIFO is empty.
static inline bool __not_in_flash_func(pio_try_get_io_write)(uint16_t *addr_out, uint8_t *data_out)
{
    if (pio_sm_is_rx_fifo_empty(msx_io_bus.pio_write, msx_io_bus.sm_io_write))
        return false;

    uint32_t sample = pio_sm_get(msx_io_bus.pio_write, msx_io_bus.sm_io_write);
    *addr_out = (uint16_t)(sample & 0xFFFFu);
    *data_out = (uint8_t)((sample >> 16) & 0xFFu);
    return true;
}

// Map an 8-bit mapper register value to a valid mapper page index.
static inline uint8_t __not_in_flash_func(mapper_page_from_reg)(uint8_t reg)
{
    return (uint8_t)(reg % MAPPER_PAGES);
}

// Try to consume an I/O read event from the I/O read responder FIFO (PIO1).
// Returns false if FIFO is empty.
static inline bool __not_in_flash_func(pio_try_get_io_read)(uint16_t *addr_out)
{
    if (pio_sm_is_rx_fifo_empty(msx_io_bus.pio_read, msx_io_bus.sm_io_read))
        return false;

    *addr_out = (uint16_t)pio_sm_get(msx_io_bus.pio_read, msx_io_bus.sm_io_read);
    return true;
}

static inline int16_t __not_in_flash_func(clamp_i16)(int32_t sample)
{
    if (sample > 32767) return 32767;
    if (sample < -32768) return -32768;
    return (int16_t)sample;
}

static void __not_in_flash_func(dual_psg_init)(void)
{
    if (!dual_psg_enabled || dual_psg_ready)
        return;

    memset(&psg_instance, 0, sizeof(psg_instance));
    psg_instance.rate = PSG_SAMPLE_RATE;
    PSG_setVolumeMode(&psg_instance, 2);
    PSG_setClock(&psg_instance, PSG_CLOCK);
    PSG_setQuality(&psg_instance, 1);
    PSG_reset(&psg_instance);
    msx_pio_io_bus_init();
    dual_psg_ready = true;
}

static inline int16_t __not_in_flash_func(dual_psg_calc_sample)(void)
{
    if (!dual_psg_ready)
        return 0;

    return clamp_i16((int32_t)PSG_calc(&psg_instance) << PSG_VOLUME_SHIFT);
}

static void __no_inline_not_in_flash_func(core1_dual_psg_audio)(void);
static void __no_inline_not_in_flash_func(core1_msx_music_audio)(void);
static void i2s_audio_init(void);
static void __not_in_flash_func(dual_psg_start_audio)(void);
static void __not_in_flash_func(msx_music_start_audio)(void);
static void __not_in_flash_func(sunrise_wait_for_expanded_bootstrap)(void);

static inline void __not_in_flash_func(dual_psg_service_io)(void)
{
    if (!dual_psg_ready)
        return;
    if (dual_psg_io_core1 && get_core_num() == 0)
        return;

    uint16_t io_addr;
    uint8_t io_data;
    while (pio_try_get_io_write(&io_addr, &io_data))
    {
        uint8_t port = io_addr & 0xFFu;
        if (port == PSG_PORT_REG || port == PSG_PORT_DATA)
            PSG_writeIO(&psg_instance, port, io_data);
    }

    while (pio_try_get_io_read(&io_addr))
    {
        pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read,
                            pio_build_token(false, 0xFFu));
    }
}

static inline struct audio_buffer *__not_in_flash_func(take_audio_buffer_with_dual_psg_io)(void)
{
    if (!dual_psg_ready)
        return take_audio_buffer(audio_pool, true);

    while (true)
    {
        dual_psg_service_io();
        struct audio_buffer *buffer = take_audio_buffer(audio_pool, false);
        if (buffer)
            return buffer;
        tight_loop_contents();
    }
}

static inline uint16_t __not_in_flash_func(pio_get_read_servicing_dual_psg)(void)
{
    dual_psg_service_io();
    while (pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
    {
        dual_psg_service_io();
        tight_loop_contents();
    }
    return (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
}

// Same as pio_get_read_servicing_dual_psg(), but keeps draining the memory
// write captor FIFO while waiting for the next read.  Mandatory for every
// banked mapper: game code running from MSX RAM can issue long bursts of
// bank-switch writes without a single cartridge read in between, and the
// write captor RX FIFO is only 8 entries deep.  Once it fills, the PIO
// stalls on "push block" and every further bank switch is silently lost,
// leaving the mapper registers on a stale segment.
//
// The idle loop reads FSTAT exactly once per iteration and tests both FIFO
// flags from that single sample.  A PIO register access dominates the loop
// cost, so polling both FIFOs separately would roughly double the /WAIT
// stretch on every cartridge read and slow down VDP transfer loops.  One
// FSTAT read keeps the read-response latency the same as a plain blocking
// wait on the read FIFO.
static inline uint16_t __not_in_flash_func(pio_get_read_draining_writes)(
    void (*handler)(uint16_t addr, uint8_t data, void *ctx), void *ctx)
{
    PIO pio = msx_bus.pio;
    const uint32_t read_empty  = 1u << (PIO_FSTAT_RXEMPTY_LSB + msx_bus.sm_read);
    const uint32_t write_empty = 1u << (PIO_FSTAT_RXEMPTY_LSB + msx_bus.sm_write);

    while (true)
    {
        dual_psg_service_io();

        uint32_t fstat = pio->fstat;

        if (!(fstat & read_empty))
            return (uint16_t)pio_sm_get(pio, msx_bus.sm_read);

        if (!(fstat & write_empty))
            pio_drain_writes(handler, ctx);
    }
}

static void __not_in_flash_func(msx_music_init)(void)
{
    if (!msx_music_enabled || msx_music_ready)
        return;

    msx_music_instance = OPLL_new(MSX_MUSIC_CLOCK, MSX_MUSIC_SAMPLE_RATE);
    if (!msx_music_instance)
        return;

    if (!msx_music_lock)
        msx_music_lock = spin_lock_instance(spin_lock_claim_unused(true));

    OPLL_reset(msx_music_instance);
    OPLL_setChipType(msx_music_instance, OPLL_2413_TONE);
    OPLL_resetPatch(msx_music_instance, OPLL_2413_TONE);
    msx_pio_io_bus_init();
    msx_music_ready = true;
}

static inline int16_t __not_in_flash_func(msx_music_calc_sample)(void)
{
    if (!msx_music_ready)
        return 0;

    uint32_t save = spin_lock_blocking(msx_music_lock);
    int16_t sample = OPLL_calc(msx_music_instance);
    spin_unlock(msx_music_lock, save);
    return clamp_i16((int32_t)sample << MSX_MUSIC_VOLUME_SHIFT);
}

static inline void __not_in_flash_func(msx_music_write_io)(uint8_t port, uint8_t data)
{
    if (!msx_music_ready)
        return;

    uint32_t save = spin_lock_blocking(msx_music_lock);
    OPLL_writeIO(msx_music_instance, port, data);
    spin_unlock(msx_music_lock, save);
}

static inline void __not_in_flash_func(msx_music_service_io)(void)
{
    if (!msx_music_ready)
        return;
    if (msx_music_io_core1 && get_core_num() == 0)
        return;

    uint16_t io_addr;
    uint8_t io_data;
    while (pio_try_get_io_write(&io_addr, &io_data))
    {
        uint8_t port = io_addr & 0xFFu;
        if (port == MSX_MUSIC_PORT_REG || port == MSX_MUSIC_PORT_DATA)
            msx_music_write_io(port, io_data);
    }

    while (pio_try_get_io_read(&io_addr))
    {
        pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read,
                            pio_build_token(false, 0xFFu));
    }
}

static inline struct audio_buffer *__not_in_flash_func(take_audio_buffer_with_msx_music_io)(void)
{
    if (!msx_music_ready)
        return take_audio_buffer(audio_pool, true);

    while (true)
    {
        msx_music_service_io();
        struct audio_buffer *buffer = take_audio_buffer(audio_pool, false);
        if (buffer)
            return buffer;
        tight_loop_contents();
    }
}

// -----------------------------------------------------------------------
// Bank switching write handlers (used by mapper ROM types)
// -----------------------------------------------------------------------

typedef struct {
    uint8_t *bank_regs;
} bank8_ctx_t;

typedef struct {
    uint16_t *bank_regs;
} bank16_ctx_t;

typedef struct {
    uint8_t page;
    uint8_t control;
    uint8_t sram_key_5ffe;
    uint8_t sram_key_5fff;
    uint8_t sram[8192];
} fmpac_state_t;

static inline bool __not_in_flash_func(fmpac_sram_enabled)(const fmpac_state_t *fmpac)
{
    return fmpac->sram_key_5ffe == 0x4Du && fmpac->sram_key_5fff == 0x69u;
}

static inline void __not_in_flash_func(fmpac_handle_write)(fmpac_state_t *fmpac, uint16_t addr, uint8_t data)
{
    if (addr == 0x7FF4u)
    {
        msx_music_write_io(MSX_MUSIC_PORT_REG, data);
    }
    else if (addr == 0x7FF5u)
    {
        msx_music_write_io(MSX_MUSIC_PORT_DATA, data);
    }
    else if (addr == 0x7FF6u)
    {
        fmpac->control = data;
    }
    else if (addr == 0x7FF7u)
    {
        fmpac->page = data & 0x03u;
    }
    else if (addr == 0x5FFEu)
    {
        fmpac->sram_key_5ffe = data;
    }
    else if (addr == 0x5FFFu)
    {
        fmpac->sram_key_5fff = data;
    }
    else if (fmpac_sram_enabled(fmpac) && addr >= 0x4000u && addr <= 0x5FFFu)
    {
        fmpac->sram[addr - 0x4000u] = data;
    }
}

static inline uint8_t __not_in_flash_func(fmpac_handle_read)(const fmpac_state_t *fmpac, const uint8_t *bios_base, uint16_t addr)
{
    if (addr == 0x7FF6u)
        return fmpac->control;
    if (addr == 0x7FF7u)
        return fmpac->page;
    if (fmpac_sram_enabled(fmpac) && addr >= 0x4000u && addr <= 0x5FFFu)
        return fmpac->sram[addr - 0x4000u];

    if (addr >= 0x4000u && addr <= 0x7FFFu)
    {
        uint32_t rel = ((uint32_t)(fmpac->page & 0x03u) << 14) | (addr & 0x3FFFu);
        if (rel < FMPAC_BIOS_SIZE)
            return bios_base[rel];
    }
    return 0xFFu;
}

static inline void __not_in_flash_func(handle_konamiscc_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint8_t *regs = ((bank8_ctx_t *)ctx)->bank_regs;
    if      (addr >= 0x5000u && addr <= 0x57FFu) regs[0] = data;
    else if (addr >= 0x7000u && addr <= 0x77FFu) regs[1] = data;
    else if (addr >= 0x9000u && addr <= 0x97FFu) regs[2] = data;
    else if (addr >= 0xB000u && addr <= 0xB7FFu) regs[3] = data;
}

static inline void __not_in_flash_func(handle_konami_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint8_t *regs = ((bank8_ctx_t *)ctx)->bank_regs;
    if      (addr >= 0x6000u && addr <= 0x67FFu) regs[1] = data;
    else if (addr >= 0x8000u && addr <= 0x87FFu) regs[2] = data;
    else if (addr >= 0xA000u && addr <= 0xA7FFu) regs[3] = data;
}

static inline void __not_in_flash_func(handle_ascii8_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint8_t *regs = ((bank8_ctx_t *)ctx)->bank_regs;
    if      (addr >= 0x6000u && addr <= 0x67FFu) regs[0] = data;
    else if (addr >= 0x6800u && addr <= 0x6FFFu) regs[1] = data;
    else if (addr >= 0x7000u && addr <= 0x77FFu) regs[2] = data;
    else if (addr >= 0x7800u && addr <= 0x7FFFu) regs[3] = data;
}

static inline void __not_in_flash_func(handle_ascii16_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint8_t *regs = ((bank8_ctx_t *)ctx)->bank_regs;
    if      (addr >= 0x6000u && addr <= 0x67FFu) regs[0] = data;
    else if (addr >= 0x7000u && addr <= 0x77FFu) regs[1] = data;
}

static inline void __not_in_flash_func(handle_neo8_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint16_t *regs = ((bank16_ctx_t *)ctx)->bank_regs;
    uint16_t base_addr = addr & 0xF800u;
    uint8_t bank_index = 6;

    switch (base_addr)
    {
        case 0x5000: case 0x1000: case 0x9000: case 0xD000: bank_index = 0; break;
        case 0x5800: case 0x1800: case 0x9800: case 0xD800: bank_index = 1; break;
        case 0x6000: case 0x2000: case 0xA000: case 0xE000: bank_index = 2; break;
        case 0x6800: case 0x2800: case 0xA800: case 0xE800: bank_index = 3; break;
        case 0x7000: case 0x3000: case 0xB000: case 0xF000: bank_index = 4; break;
        case 0x7800: case 0x3800: case 0xB800: case 0xF800: bank_index = 5; break;
    }

    if (bank_index < 6u)
    {
        if (addr & 0x01u)
            regs[bank_index] = (regs[bank_index] & 0x00FFu) | ((uint16_t)data << 8);
        else
            regs[bank_index] = (regs[bank_index] & 0xFF00u) | data;
        regs[bank_index] &= 0x0FFFu;
    }
}

static inline void __not_in_flash_func(handle_neo16_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint16_t *regs = ((bank16_ctx_t *)ctx)->bank_regs;
    uint16_t base_addr = addr & 0xF800u;
    uint8_t bank_index = 3;

    switch (base_addr)
    {
        case 0x5000: case 0x1000: case 0x9000: case 0xD000: bank_index = 0; break;
        case 0x6000: case 0x2000: case 0xA000: case 0xE000: bank_index = 1; break;
        case 0x7000: case 0x3000: case 0xB000: case 0xF000: bank_index = 2; break;
    }

    if (bank_index < 3u)
    {
        if (addr & 0x01u)
            regs[bank_index] = (regs[bank_index] & 0x00FFu) | ((uint16_t)data << 8);
        else
            regs[bank_index] = (regs[bank_index] & 0xFF00u) | data;
        regs[bank_index] &= 0x0FFFu;
    }
}

// -----------------------------------------------------------------------
// Generic banked ROM loop (8KB banks, 8-bit bank registers)
// -----------------------------------------------------------------------
static void __no_inline_not_in_flash_func(banked8_loop)(
    const uint8_t *rom_base,
    uint32_t available_length,
    uint8_t *bank_regs,
    void (*write_handler)(uint16_t, uint8_t, void *))
{
    bank8_ctx_t ctx = { .bank_regs = bank_regs };

    while (true)
    {
        uint16_t addr = pio_get_read_draining_writes(write_handler, &ctx);

        pio_drain_writes(write_handler, &ctx);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint32_t rel = ((uint32_t)bank_regs[(addr - 0x4000u) >> 13] * 0x2000u) + (addr & 0x1FFFu);
            if (available_length == 0u || rel < available_length)
                data = read_rom_byte(rom_base, rel);
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// Forward declarations for functions called from the RAM-target launch dispatch
void __no_inline_not_in_flash_func(loadrom_planar64)(uint32_t offset, bool cache_enable);

void __no_inline_not_in_flash_func(loadrom_plain32)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 32768u, &rom_base, &available_length);

    msx_pio_bus_init();

    while (true)
    {
        uint16_t addr = pio_get_read_servicing_dual_psg();

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = in_window ? rom_base[addr - 0x4000u] : 0xFFu;

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

void __no_inline_not_in_flash_func(loadrom_linear48)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 49152u, &rom_base, &available_length);

    msx_pio_bus_init();

    while (true)
    {
        uint16_t addr = pio_get_read_servicing_dual_psg();

        bool in_window = (addr <= 0xBFFFu);
        uint8_t data = in_window ? rom_base[addr] : 0xFFu;

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

void __no_inline_not_in_flash_func(loadrom_konamiscc)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();
    banked8_loop(rom_base, available_length, bank_registers, handle_konamiscc_write);
}

// -----------------------------------------------------------------------
// Core 1 audio entry: generates SCC samples and pushes to I2S
// -----------------------------------------------------------------------
static void __no_inline_not_in_flash_func(core1_scc_audio)(void)
{
    while (true)
    {
        struct audio_buffer *buffer = take_audio_buffer(audio_pool, true);
        int16_t *samples = (int16_t *)buffer->buffer->bytes;
        for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
        {
            int16_t raw = SCC_calc(&scc_instance);
            int32_t boosted = (int32_t)raw << SCC_VOLUME_SHIFT;
            int16_t s = clamp_i16(boosted);
            samples[i * 2]     = s;  // left
            samples[i * 2 + 1] = s;  // right
        }
        buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
        give_audio_buffer(audio_pool, buffer);
    }
}

void __not_in_flash_func(service_scc_audio)(void)
{
    if (!scc_audio_ready || !audio_pool)
        return;

    struct audio_buffer *buffer = take_audio_buffer(audio_pool, false);
    if (!buffer)
        return;

    int16_t *samples = (int16_t *)buffer->buffer->bytes;
    for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
    {
        int16_t raw = SCC_calc(&scc_instance);
        int32_t boosted = (int32_t)raw << SCC_VOLUME_SHIFT;
        int16_t s = clamp_i16(boosted);
        samples[i * 2]     = s;
        samples[i * 2 + 1] = s;
    }
    buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
    give_audio_buffer(audio_pool, buffer);
}

static void __no_inline_not_in_flash_func(core1_dual_psg_audio)(void)
{
    while (true)
    {
        struct audio_buffer *buffer = take_audio_buffer_with_dual_psg_io();
        int16_t *samples = (int16_t *)buffer->buffer->bytes;
        for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
        {
            dual_psg_service_io();
            int16_t s = dual_psg_calc_sample();
            samples[i * 2]     = s;
            samples[i * 2 + 1] = s;
        }
        buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
        give_audio_buffer(audio_pool, buffer);
    }
}

static void __no_inline_not_in_flash_func(core1_msx_music_audio)(void)
{
    while (true)
    {
        struct audio_buffer *buffer = take_audio_buffer_with_msx_music_io();
        int16_t *samples = (int16_t *)buffer->buffer->bytes;
        for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
        {
            msx_music_service_io();
            int16_t s = msx_music_calc_sample();
            samples[i * 2]     = s;
            samples[i * 2 + 1] = s;
        }
        buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
        give_audio_buffer(audio_pool, buffer);
    }
}

// -----------------------------------------------------------------------
// I2S audio initialisation. The mapper/C2 modes keep their I/O responder on
// PIO1, so audio must stay on PIO0 and avoid SM0/SM1 used by the MSX bus.
// -----------------------------------------------------------------------
static void i2s_audio_init(void)
{
    if (scc_audio_ready)
        return;

    // Unmute DAC (active-high mute: low = unmuted)
    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 0);

    static audio_format_t audio_format = {
        .sample_freq = SCC_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };

    static struct audio_buffer_format producer_format = {
        .format = &audio_format,
        .sample_stride = 4,  // 2 channels * 2 bytes
    };

    audio_pool = audio_new_producer_pool(&producer_format, 3, SCC_AUDIO_BUFFER_SAMPLES);

    if (scc_dma_channel < 0)
    {
        for (int ch = 0; ch < NUM_DMA_CHANNELS; ++ch)
        {
            if (!dma_channel_is_claimed((uint)ch))
            {
                scc_dma_channel = ch;
                break;
            }
        }
    }
    if (scc_dma_channel < 0)
    {
        audio_pool = NULL;
        return;
    }

    static struct audio_i2s_config i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_BCLK_PIN,
        .dma_channel = 0,
        .pio_sm = 2,
    };

    i2s_config.dma_channel = (uint)scc_dma_channel;

    audio_i2s_setup(&audio_format, &i2s_config);
    audio_i2s_connect(audio_pool);
    audio_i2s_set_enabled(true);
    scc_audio_ready = true;
}

static void __not_in_flash_func(dual_psg_start_audio)(void)
{
    if (!dual_psg_ready || dual_psg_audio_started)
        return;

    i2s_audio_init();
    if (!audio_pool)
        return;

    dual_psg_io_core1 = true;
    dual_psg_audio_started = true;
    multicore_launch_core1(core1_dual_psg_audio);
}

static void __not_in_flash_func(msx_music_start_audio)(void)
{
    if (!msx_music_ready || msx_music_audio_started)
        return;

    i2s_audio_init();
    if (!audio_pool)
        return;

    msx_music_io_core1 = true;
    msx_music_audio_started = true;
    multicore_launch_core1(core1_msx_music_audio);
}

// -----------------------------------------------------------------------
// Konami SCC mapper with SCC sound emulation via I2S
// -----------------------------------------------------------------------
void __no_inline_not_in_flash_func(loadrom_konamiscc_scc)(uint32_t offset, bool cache_enable, uint32_t scc_type)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    // Initialise SCC emulator (static allocation, no malloc)
    memset(&scc_instance, 0, sizeof(SCC));
    scc_instance.clk  = SCC_CLOCK;
    scc_instance.rate = SCC_SAMPLE_RATE;
    SCC_set_quality(&scc_instance, 1);
    scc_instance.type = scc_type;
    SCC_reset(&scc_instance);

    // Start I2S audio subsystem, then launch audio on core 1
    i2s_audio_init();
    multicore_launch_core1(core1_scc_audio);

    msx_pio_bus_init();

    while (true)
    {
        // --- drain pending writes ---
        uint16_t waddr;
        uint8_t  wdata;
        while (pio_try_get_write(&waddr, &wdata))
        {
            if      (waddr >= 0x5000u && waddr <= 0x57FFu) bank_registers[0] = wdata;
            else if (waddr >= 0x7000u && waddr <= 0x77FFu) bank_registers[1] = wdata;
            else if (waddr >= 0x9000u && waddr <= 0x97FFu) bank_registers[2] = wdata;
            else if (waddr >= 0xB000u && waddr <= 0xB7FFu) bank_registers[3] = wdata;

            // Forward to SCC emulator (handles enable + register writes)
            SCC_write(&scc_instance, waddr, wdata);
        }

        // --- handle read request ---
        uint16_t addr = pio_get_read_servicing_dual_psg();

        // drain writes that arrived while waiting
        while (pio_try_get_write(&waddr, &wdata))
        {
            if      (waddr >= 0x5000u && waddr <= 0x57FFu) bank_registers[0] = wdata;
            else if (waddr >= 0x7000u && waddr <= 0x77FFu) bank_registers[1] = wdata;
            else if (waddr >= 0x9000u && waddr <= 0x97FFu) bank_registers[2] = wdata;
            else if (waddr >= 0xB000u && waddr <= 0xB7FFu) bank_registers[3] = wdata;
            SCC_write(&scc_instance, waddr, wdata);
        }

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            // Check if address falls in active SCC register read region.
            // Standard SCC: 0x9800-0x98FF (base_adr = 0x9000)
            // Enhanced SCC+: 0xB800-0xB8FF (base_adr = 0xB000) or 0x9800-0x98FF
            bool is_scc_read = false;
            if (scc_instance.active)
            {
                uint32_t scc_reg_start = scc_instance.base_adr + 0x800u;
                if (addr >= scc_reg_start && addr <= (scc_reg_start + 0xFFu))
                    is_scc_read = true;
            }
            // SCC+ mode register at 0xBFFE-0xBFFF
            if (scc_type == SCC_ENHANCED && (addr & 0xFFFEu) == 0xBFFEu)
                is_scc_read = true;

            if (is_scc_read)
            {
                data = (uint8_t)SCC_read(&scc_instance, addr);
            }
            else
            {
                uint32_t rel = ((uint32_t)bank_registers[(addr - 0x4000u) >> 13] * 0x2000u)
                             + (addr & 0x1FFFu);
                if (available_length == 0u || rel < available_length)
                    data = read_rom_byte(rom_base, rel);
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

void __no_inline_not_in_flash_func(loadrom_konami)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();
    banked8_loop(rom_base, available_length, bank_registers, handle_konami_write);
}

void __no_inline_not_in_flash_func(loadrom_ascii8)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();
    banked8_loop(rom_base, available_length, bank_registers, handle_ascii8_write);
}

void __no_inline_not_in_flash_func(loadrom_ascii16)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[2] = {0, 1};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();

    bank8_ctx_t ctx = { .bank_regs = bank_registers };

    while (true)
    {
        uint16_t addr = pio_get_read_draining_writes(handle_ascii16_write, &ctx);

        pio_drain_writes(handle_ascii16_write, &ctx);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t bank = (addr >> 15) & 1;
            uint32_t rel = ((uint32_t)bank_registers[bank] << 14) + (addr & 0x3FFFu);
            if (available_length == 0u || rel < available_length)
                data = read_rom_byte(rom_base, rel);
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// Sunrise IDE write handler (cReg mapper at 0x4104 + IDE registers)
// -----------------------------------------------------------------------
typedef struct {
    sunrise_ide_t *ide;
} sunrise_ctx_t;

static inline void __not_in_flash_func(handle_sunrise_write)(uint16_t addr, uint8_t data, void *ctx)
{
    sunrise_ctx_t *sctx = (sunrise_ctx_t *)ctx;

    // IDE register / control writes (0x4104, 0x7C00-0x7DFF, 0x7E00-0x7EFF)
    if (addr >= 0x4000u && addr <= 0x7FFFu)
    {
        sunrise_ide_handle_write(sctx->ide, addr, data);
    }
}

// -----------------------------------------------------------------------
// loadrom_sunrise - Sunrise IDE Nextor ROM with emulated IDE interface
// -----------------------------------------------------------------------
// Uses the Sunrise IDE mapper: a single 16KB ROM window at 0x4000-0x7FFF
// with page selection via the control register at 0x4104 (bits 7:5 = page).
// No ROM at 0x8000-0xBFFF — the MSX provides its own RAM there.
// IDE register overlay when bit 0 of the control register is set:
//   - 0x7C00-0x7DFF = 16-bit data register (low/high byte latch)
//   - 0x7E00-0x7EFF = ATA task-file registers (mirrored every 16 bytes)
//   - 0x7F00-0x7FFF = ROM data (excluded from IDE space)
// ATA commands are translated to USB MSC operations on Core 1.
void __no_inline_not_in_flash_func(loadrom_sunrise)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    // Initialise Sunrise IDE state
    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    // Share IDE context with Core 1 and launch USB host task
    sunrise_usb_set_ide_ctx(&ide);
    multicore_launch_core1(sunrise_usb_task);

    // Initialise PIO bus engine
    msx_pio_bus_init();

    sunrise_ctx_t ctx = { .ide = &ide };

    // Main loop: service PIO read/write events
    // Continuous write draining needed — Sunrise IDE generates heavy
    // write bursts (bank switch + IDE_ON + task-file registers + command)
    // that can overflow the 8-entry PIO FIFO.
    while (true)
    {
        uint16_t addr;

        // Poll: drain write FIFO while waiting for a read event
        while (true)
        {
            pio_drain_writes(handle_sunrise_write, &ctx);
            if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
            {
                addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
                break;
            }
        }

        // Drain any writes that arrived alongside the read
        pio_drain_writes(handle_sunrise_write, &ctx);

        // Sunrise IDE ROM is only at 0x4000-0x7FFF (one 16KB window)
        bool in_window = (addr >= 0x4000u) && (addr <= 0x7FFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            // Check if IDE intercepts this read (0x7C00-0x7EFF when enabled)
            uint8_t ide_data;
            if (sunrise_ide_handle_read(&ide, addr, &ide_data))
            {
                data = ide_data;
            }
            else
            {
                // Sunrise mapper: page selected by ide.segment (cReg bits 7:5)
                uint8_t seg = ide.segment;
                uint32_t rel = ((uint32_t)seg << 14) + (addr & 0x3FFFu);
                if (available_length == 0u || rel < available_length)
                    data = read_rom_byte(rom_base, rel);
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// loadrom_sunrise_mapper - Sunrise IDE Nextor + 1MB Memory Mapper
// -----------------------------------------------------------------------
// Implements expanded slot with two sub-slots:
//   Sub-slot 0: Nextor ROM (Sunrise IDE) — 16KB window at 0x4000-0x7FFF
//   Sub-slot 1: 1MB Memory Mapper RAM — all 4 pages (0x0000-0xFFFF)
//
// The sub-slot register at 0xFFFF controls which sub-slot is selected
// for each 16KB page (bits 1:0 = page 0, bits 3:2 = page 1, etc.).
// Reading 0xFFFF returns the bitwise NOT of the sub-slot register.
//
// Memory mapper page registers (I/O ports FC-FF) select which 16KB page
// of mapper RAM appears in each address range:
//   Port FC → page at 0x0000-0x3FFF
//   Port FD → page at 0x4000-0x7FFF
//   Port FE → page at 0x8000-0xBFFF
//   Port FF → page at 0xC000-0xFFFF
//
// Reset values per BIOS convention: FC=3, FD=2, FE=1, FF=0
//
// The mapper RAM is 1MB = 64 pages of 16KB. Page registers are treated
// as 8-bit values and normalized to 0..63 when accessing RAM.
void __no_inline_not_in_flash_func(loadrom_sunrise_mapper)(uint32_t offset, bool cache_enable)
{
    (void)cache_enable;

    // ---------------------------------------------------------------
    // Phase 1 — Bootstrap: serve a tiny ROM that restarts the MSX.
    //
    // On cold power-on the Pico and MSX start simultaneously.  The MSX2
    // BIOS probes slots very early, and if the Pico is not yet ready
    // the BIOS records the slot as empty/simple.  Releasing WAIT later
    // with an expanded-slot mapper active leaves the BIOS in an
    // inconsistent state, causing a freeze (blue screen on MSX2).
    //
    // To guarantee a clean boot we first serve a minimal ROM whose INIT
    // routine executes JP 0x0000 (cold restart).  Once the restart is
    // detected we freeze the CPU and set up the full mapper before the
    // BIOS probes slots again.
    // ---------------------------------------------------------------

    // Minimal MSX ROM:  AB header with INIT that sets port F4 bit 7
    // (forces cold-boot on MSX2+, showing RAM count) then restarts.
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42,            // 'AB' ROM header
        0x0A, 0x40,            // INIT = 0x400A
        0x00, 0x00,            // STATEMENT = none
        0x00, 0x00,            // DEVICE = none
        0x00, 0x00,            // TEXT = none
        0xF3,                  // DI
        0xDB, 0xF4,            // IN A, (0xF4)
        0xF6, 0x80,            // OR 0x80       (set bit 7 → cold boot)
        0xD3, 0xF4,            // OUT (0xF4), A
        0xC7                   // RST 0x00      (cold restart)
    };

    // Start PIO memory bus so the MSX can discover the bootstrap ROM.
    msx_pio_bus_init();

    // Serve bootstrap ROM reads until the restart is detected.
    bool restart_detected = false;
    bool init_called = false;

    while (!restart_detected)
    {
        // Drain writes (ignored during bootstrap)
        {
            uint16_t waddr;
            uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata)) { }
        }

        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);

            if (init_called && addr == 0x0000u)
            {
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read,
                                    pio_build_token(false, 0xFFu));
                restart_detected = true;
            }
            else
            {
                if (addr >= 0x400Au && addr <= 0x4011u)
                    init_called = true;

                bool in_window = (addr >= 0x4000u && addr <= 0x7FFFu);
                uint8_t data = 0xFFu;
                if (in_window)
                {
                    uint32_t rel = addr - 0x4000u;
                    if (rel < sizeof(bootstrap_rom))
                        data = bootstrap_rom[rel];
                }
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read,
                                    pio_build_token(in_window, data));
            }
        }
        else
        {
            // MSX1 path: detect restart on raw address bus
            if (init_called && !gpio_get(PIN_RD) &&
                ((gpio_get_all() & 0xFFFFu) == 0x0000u))
            {
                restart_detected = true;
            }
        }
    }

    // ---------------------------------------------------------------
    // Phase 2 — Initialise the expanded-slot mapper.
    // The MSX just restarted — freeze it before the BIOS probes slots.
    // ---------------------------------------------------------------
    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_set_enabled(pio0, 1, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);            // Assert WAIT — freeze MSX bus

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);

    // Initialise Sunrise IDE state
    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    // Share IDE context with Core 1 and launch USB host task
    sunrise_usb_set_ide_ctx(&ide);
    multicore_launch_core1(sunrise_usb_task);

    // Initialise mapper page registers (BIOS convention)
    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };  // FC, FD, FE, FF

    // Sub-slot register: bits [1:0]=page0, [3:2]=page1, [5:4]=page2, [7:6]=page3
    // Boot mapping for Nextor MAP_INIT compatibility:
    //   page0 -> sub-slot0
    //   page1 -> sub-slot0 (Sunrise ROM/IDE)
    //   page2 -> sub-slot1 (mapper RAM, probe target at 0x8000)
    //   page3 -> sub-slot0
    // Value: 0b00010000 = 0x10
    uint8_t subslot_reg = 0x10;

    // Clear mapper RAM in external PSRAM
    mapper_fill_ff();

    // Initialise PIO I/O bus FIRST (mapper port handlers must be ready
    // before the memory bus releases WAIT and the BIOS starts probing).
    msx_pio_io_bus_init();

    // Initialise PIO memory bus — this hands PIN_WAIT back to the PIO
    // read SM whose first instruction uses "side 1" (WAIT released),
    // so the MSX resumes execution with everything fully initialised.
    msx_pio_bus_init();

    sunrise_ctx_t ctx = { .ide = &ide };

    // Main loop: service memory reads/writes and I/O reads/writes
    while (true)
    {
        // --- Drain memory writes (Sunrise IDE + mapper RAM + sub-slot) ---
        {
            uint16_t waddr;
            uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata))
            {
                // Sub-slot register write at 0xFFFF
                if (waddr == 0xFFFFu)
                {
                    subslot_reg = wdata;
                }
                else
                {
                    uint8_t page = (waddr >> 14) & 0x03u;
                    uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;

                    if (active_subslot == 0)
                    {
                        // Sub-slot 0: Sunrise IDE (0x4000-0x7FFF only)
                        if (waddr >= 0x4000u && waddr <= 0x7FFFu)
                        {
                            sunrise_ide_handle_write(&ide, waddr, wdata);
                        }
                    }
                    else if (active_subslot == 1)
                    {
                        // Sub-slot 1: Memory mapper RAM — write to mapped page
                        uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                        uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (waddr & 0x3FFFu);
                        mapper_write_byte(mapper_offset, wdata);
                    }
                    // Sub-slots 2 and 3: unused, writes are ignored
                }
            }
        }

        // --- Drain I/O writes (mapper page registers FC-FF) ---
        {
            uint16_t io_addr;
            uint8_t io_data;
            while (pio_try_get_io_write(&io_addr, &io_data))
            {
                uint8_t port = io_addr & 0xFFu;
                if (port >= 0xFCu && port <= 0xFFu)
                {
                    mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
                }
            }
        }

        // --- Handle I/O reads (mapper page registers FC-FF) ---
        {
            uint16_t io_addr;
            while (pio_try_get_io_read(&io_addr))
            {
                uint8_t port = io_addr & 0xFFu;
                bool in_window = false;
                uint8_t data = 0xFFu;

                if (port >= 0xFCu && port <= 0xFFu)
                {
                    in_window = true;
                    data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
                }

                pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read, pio_build_token(in_window, data));
            }
        }

        // --- Handle memory reads ---
        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
            // Always drive the bus when our primary slot is selected (the
            // PIO only queues reads while SLTSL is active for our slot).
            // Real MSX tolerates a floating bus (pull-ups -> 0xFF), but
            // several FPGA cores require an explicit ack on every read,
            // including reads to addresses inside an empty/unmapped
            // sub-slot. Default to 0xFF and override below.
            uint8_t data = 0xFFu;
            bool in_window = true;

            // Sub-slot register read at 0xFFFF: return ~subslot_reg
            if (addr == 0xFFFFu)
            {
                data = ~subslot_reg;
            }
            else
            {
                uint8_t page = (addr >> 14) & 0x03u;
                uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;

                if (active_subslot == 0)
                {
                    // Sub-slot 0: Nextor ROM (0x4000-0x7FFF only)
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        // Check if IDE intercepts this read
                        uint8_t ide_data;
                        if (sunrise_ide_handle_read(&ide, addr, &ide_data))
                        {
                            data = ide_data;
                        }
                        else
                        {
                            // Sunrise mapper: page selected by ide.segment
                            uint8_t seg = ide.segment;
                            uint32_t rel = ((uint32_t)seg << 14) + (addr & 0x3FFFu);
                            if (available_length == 0u || rel < available_length)
                                data = read_rom_byte(rom_base, rel);
                        }
                    }
                }
                else if (active_subslot == 1)
                {
                    // Sub-slot 1: Memory mapper RAM — all 4 pages
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (addr & 0x3FFFu);
                    data = mapper_read_byte(mapper_offset);
                }
                // Sub-slots 2 and 3: unused — keep default 0xFF (in window)
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

// -----------------------------------------------------------------------
// Case 15: Sunrise IDE via microSD card (no mapper)
// -----------------------------------------------------------------------
// Identical to loadrom_sunrise (case 10) but launches the SD backend
// on Core 1 instead of the USB backend.
void __no_inline_not_in_flash_func(loadrom_sunrise_sd)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    sunrise_sd_set_ide_ctx(&ide);
    multicore_launch_core1(sunrise_sd_task);

    msx_pio_bus_init();

    sunrise_ctx_t ctx = { .ide = &ide };

    while (true)
    {
        uint16_t addr;
        while (true)
        {
            pio_drain_writes(handle_sunrise_write, &ctx);
            if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
            {
                addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
                break;
            }
        }

        pio_drain_writes(handle_sunrise_write, &ctx);

        bool in_window = (addr >= 0x4000u) && (addr <= 0x7FFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t ide_data;
            if (sunrise_ide_handle_read(&ide, addr, &ide_data))
            {
                data = ide_data;
            }
            else
            {
                uint8_t seg = ide.segment;
                uint32_t rel = ((uint32_t)seg << 14) + (addr & 0x3FFFu);
                if (available_length == 0u || rel < available_length)
                    data = read_rom_byte(rom_base, rel);
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// Case 16: Sunrise IDE + 1MB memory mapper via microSD card
// -----------------------------------------------------------------------
// Identical to loadrom_sunrise_mapper (case 11) but launches the SD
// backend on Core 1 instead of the USB backend.
void __no_inline_not_in_flash_func(loadrom_sunrise_mapper_sd)(uint32_t offset, bool cache_enable)
{
    (void)cache_enable;

    // Phase 1 — Bootstrap: minimal ROM that restarts the MSX
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42,            // 'AB' ROM header
        0x0A, 0x40,            // INIT = 0x400A
        0x00, 0x00,            // STATEMENT = none
        0x00, 0x00,            // DEVICE = none
        0x00, 0x00,            // TEXT = none
        0xF3,                  // DI
        0xDB, 0xF4,            // IN A, (0xF4)
        0xF6, 0x80,            // OR 0x80
        0xD3, 0xF4,            // OUT (0xF4), A
        0xC7                   // RST 0x00
    };

    msx_pio_bus_init();

    bool restart_detected = false;
    bool init_called = false;

    while (!restart_detected)
    {
        { uint16_t waddr; uint8_t wdata; while (pio_try_get_write(&waddr, &wdata)) { } }

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
                if (in_window)
                {
                    uint32_t rel = addr - 0x4000u;
                    if (rel < sizeof(bootstrap_rom)) data = bootstrap_rom[rel];
                }
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
            }
        }
        else
        {
            if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
                restart_detected = true;
        }
    }

    // Phase 2 — Initialise expanded-slot mapper
    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_set_enabled(pio0, 1, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);
    const uint8_t *wifi_rom_base = rom + offset + available_length;

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    sunrise_sd_set_ide_ctx(&ide);
    multicore_launch_core1(sunrise_sd_task);

    // Allocate 1MB mapper RAM from the PSRAM memory manager
    psram_mem_init();
    if (!psram_alloc(MAPPER_SIZE, &mapper_region))
    {
        while (true) { tight_loop_contents(); }
    }

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t subslot_reg = 0x10;

    mapper_fill_ff();

    msx_pio_io_bus_init();
    msx_pio_bus_init();

    sunrise_ctx_t ctx = { .ide = &ide };

    while (true)
    {
        // Drain memory writes
        {
            uint16_t waddr; uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata))
            {
                if (waddr == 0xFFFFu)
                    subslot_reg = wdata;
                else
                {
                    uint8_t page = (waddr >> 14) & 0x03u;
                    uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;
                    if (active_subslot == 0)
                    {
                        if (waddr >= 0x4000u && waddr <= 0x7FFFu)
                            sunrise_ide_handle_write(&ide, waddr, wdata);
                    }
                    else if (active_subslot == 1)
                    {
                        uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                        uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (waddr & 0x3FFFu);
                        mapper_write_byte(mapper_offset, wdata);
                    }
                }
            }
        }

        // Drain I/O writes (mapper page registers FC-FF)
        {
            uint16_t io_addr; uint8_t io_data;
            while (pio_try_get_io_write(&io_addr, &io_data))
            {
                uint8_t port = io_addr & 0xFFu;
                if (port >= 0xFCu && port <= 0xFFu)
                    mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
            }
        }

        // Handle I/O reads (mapper page registers FC-FF)
        {
            uint16_t io_addr;
            while (pio_try_get_io_read(&io_addr))
            {
                uint8_t port = io_addr & 0xFFu;
                bool in_window = false;
                uint8_t data = 0xFFu;
                if (port >= 0xFCu && port <= 0xFFu)
                {
                    in_window = true;
                    data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
                }
                pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read, pio_build_token(in_window, data));
            }
        }

        // Handle memory reads
        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
            uint8_t data = 0xFFu;
            bool in_window = false;

            if (addr == 0xFFFFu)
            {
                in_window = true;
                data = ~subslot_reg;
            }
            else
            {
                uint8_t page = (addr >> 14) & 0x03u;
                uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;
                if (active_subslot == 0)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        in_window = true;
                        uint8_t ide_data;
                        if (sunrise_ide_handle_read(&ide, addr, &ide_data))
                            data = ide_data;
                        else
                        {
                            uint8_t seg = ide.segment;
                            uint32_t rel = ((uint32_t)seg << 14) + (addr & 0x3FFFu);
                            if (available_length == 0u || rel < available_length)
                                data = read_rom_byte(rom_base, rel);
                        }
                    }
                }
                else if (active_subslot == 1)
                {
                    in_window = true;
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (addr & 0x3FFFu);
                    data = mapper_read_byte(mapper_offset);
                }
                // Sub-slots 2 and 3: unused, return 0xFF (not in window)
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

void __no_inline_not_in_flash_func(loadrom_neo8)(uint32_t offset)
{
    uint16_t bank_registers[6] = {0};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, false, 0u, &rom_base, &available_length);

    msx_pio_bus_init();

    bank16_ctx_t ctx = { .bank_regs = bank_registers };

    while (true)
    {
        uint16_t addr = pio_get_read_draining_writes(handle_neo8_write, &ctx);

        pio_drain_writes(handle_neo8_write, &ctx);

        bool in_window = (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t bank_index = addr >> 13;
            if (bank_index < 6u)
            {
                uint32_t segment = bank_registers[bank_index] & 0x0FFFu;
                uint32_t rel = (segment << 13) + (addr & 0x1FFFu);
                if (available_length == 0u || rel < available_length)
                    data = read_rom_byte(rom_base, rel);
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

void __no_inline_not_in_flash_func(loadrom_neo16)(uint32_t offset)
{
    uint16_t bank_registers[3] = {0};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, false, 0u, &rom_base, &available_length);

    msx_pio_bus_init();

    bank16_ctx_t ctx = { .bank_regs = bank_registers };

    while (true)
    {
        uint16_t addr = pio_get_read_draining_writes(handle_neo16_write, &ctx);

        pio_drain_writes(handle_neo16_write, &ctx);

        bool in_window = (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t bank_index = addr >> 14;
            if (bank_index < 3u)
            {
                uint32_t segment = bank_registers[bank_index] & 0x0FFFu;
                uint32_t rel = (segment << 14) + (addr & 0x3FFFu);
                if (available_length == 0u || rel < available_length)
                    data = read_rom_byte(rom_base, rel);
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// loadrom_planar64 - 64KB Planar ROM (no mapper)
// -----------------------------------------------------------------------
// Four 16KB pages covering the full 64KB address space.
void __no_inline_not_in_flash_func(loadrom_planar64)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 65536u, &rom_base, &available_length);

    msx_pio_bus_init();

    while (true)
    {
        uint16_t addr = pio_get_read_servicing_dual_psg();

        bool in_window = true;
        uint8_t data = 0xFFu;

        if (available_length == 0u || (uint32_t)addr < available_length)
            data = read_rom_byte(rom_base, (uint32_t)addr);

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// Bank cache infrastructure for mapper-aware LRU caching
// -----------------------------------------------------------------------
// Provides a slot-based SRAM cache where each slot holds one bank of ROM
// data.  Used by mappers with large address spaces (ASCII16-X, NEO8/16
// cached variants) to serve all reads from SRAM.  Flash is only accessed
// on cache misses during bank switches.

#define BANK_CACHE_MAX_SLOTS 32   // max slots (256KB / 8KB)
#define BANK_CACHE_MAX_PINS   6   // max simultaneously pinned pages
#define BANK_EMPTY           0xFFFFu

typedef struct {
    uint16_t      slot_bank[BANK_CACHE_MAX_SLOTS]; // bank loaded per slot
    uint8_t       slot_lru[BANK_CACHE_MAX_SLOTS];  // LRU counter (0 = MRU)
    int8_t        page_slot[BANK_CACHE_MAX_PINS];  // slot pinned per page
    const uint8_t *flash_base;
    uint32_t      rom_length;
    uint8_t       num_slots;    // actual slot count
    uint8_t       num_pins;     // active pinned pages
    uint16_t      slot_size;    // bytes per slot (8192 or 16384)
    uint8_t       slot_shift;   // log2(slot_size): 13 or 14
} bank_cache_t;

static inline void __not_in_flash_func(bcache_init)(
    bank_cache_t *c, uint16_t slot_size, uint8_t num_pins,
    const uint8_t *flash_base, uint32_t rom_length)
{
    uint8_t num_slots = (uint8_t)(CACHE_SIZE / slot_size);
    c->num_slots  = num_slots;
    c->num_pins   = num_pins;
    c->slot_size  = slot_size;
    c->slot_shift = (slot_size == 8192u) ? 13 : 14;
    c->flash_base = flash_base;
    c->rom_length = rom_length;
    for (int i = 0; i < BANK_CACHE_MAX_SLOTS; i++)
    {
        c->slot_bank[i] = BANK_EMPTY;
        c->slot_lru[i]  = (uint8_t)i;
    }
    for (int i = 0; i < BANK_CACHE_MAX_PINS; i++)
        c->page_slot[i] = -1;
}

// Promote a slot to MRU.
static inline void __not_in_flash_func(bcache_touch)(bank_cache_t *c, int8_t slot)
{
    uint8_t prev = c->slot_lru[slot];
    if (prev == 0) return;
    uint8_t n = c->num_slots;
    for (uint8_t i = 0; i < n; i++)
        if (c->slot_lru[i] < prev) c->slot_lru[i]++;
    c->slot_lru[slot] = 0;
}

// Find the slot holding a given bank, or -1.
static inline int8_t __not_in_flash_func(bcache_find)(bank_cache_t *c, uint16_t bank)
{
    uint8_t n = c->num_slots;
    for (uint8_t i = 0; i < n; i++)
        if (c->slot_bank[i] == bank) return (int8_t)i;
    return -1;
}

// Pick a victim slot (highest LRU counter, not pinned).
static inline int8_t __not_in_flash_func(bcache_evict)(bank_cache_t *c)
{
    int8_t best = -1;
    uint8_t best_lru = 0;
    uint8_t n = c->num_slots;
    uint8_t np = c->num_pins;
    for (uint8_t i = 0; i < n; i++)
    {
        bool pinned = false;
        for (uint8_t p = 0; p < np; p++)
            if ((int8_t)i == c->page_slot[p]) { pinned = true; break; }
        if (pinned) continue;
        if (best < 0 || c->slot_lru[i] >= best_lru)
        {
            best = (int8_t)i;
            best_lru = c->slot_lru[i];
        }
    }
    return best;
}

// Normalize a bank number modulo the backing array size so out-of-range
// banks wrap around, matching real hardware mirror behaviour.
static inline uint16_t __not_in_flash_func(bcache_norm_bank)(bank_cache_t *c, uint16_t bank)
{
    if (c->rom_length > 0u)
    {
        uint16_t total_banks = (uint16_t)(c->rom_length >> c->slot_shift);
        if (total_banks > 0u)
            bank = bank % total_banks;
    }
    return bank;
}

// Ensure a bank is resident; returns its slot index.
static inline int8_t __not_in_flash_func(bcache_ensure)(bank_cache_t *c, uint16_t bank)
{
    bank = bcache_norm_bank(c, bank);
    int8_t slot = bcache_find(c, bank);
    if (slot >= 0) { bcache_touch(c, slot); return slot; }

    slot = bcache_evict(c);
    uint32_t src_off = (uint32_t)(bank & 0x0FFFu) << c->slot_shift;
    uint8_t *dst = &rom_sram[(uint32_t)slot * c->slot_size];
    uint16_t ss = c->slot_size;

    if (c->rom_length == 0u || src_off < c->rom_length)
    {
        uint32_t n = ss;
        if (c->rom_length > 0u && src_off + n > c->rom_length)
            n = c->rom_length - src_off;
        memcpy(dst, c->flash_base + src_off, n);
        if (n < ss) memset(dst + n, 0xFFu, ss - n);
    }
    else
    {
        memset(dst, 0xFFu, ss);
    }

    c->slot_bank[slot] = bank;
    bcache_touch(c, slot);
    return slot;
}

// Pre-fill the cache with the first N banks of the ROM.
static inline void __not_in_flash_func(bcache_prefill)(bank_cache_t *c)
{
    uint16_t max_banks = c->num_slots;
    if (c->rom_length > 0u)
    {
        uint16_t rom_banks = (uint16_t)((c->rom_length + c->slot_size - 1u) / c->slot_size);
        if (rom_banks < max_banks) max_banks = rom_banks;
    }
    for (uint16_t b = 0; b < max_banks; b++)
        bcache_ensure(c, b);
}

// -----------------------------------------------------------------------
// ASCII16-X mapper
// -----------------------------------------------------------------------
// Two 16KB banks mirrored to all four 16KB address quadrants:
//   page 1 bank at 4000-7FFF and C000-FFFF
//   page 2 bank at 0000-3FFF and 8000-BFFF
//
// Bank register mirrors:
//   page 1: 2000-2FFF, 6000-6FFF, A000-AFFF, E000-EFFF
//   page 2: 3000-3FFF, 7000-7FFF, B000-BFFF, F000-FFFF
//
// Bank number is 12-bit:
//   bits 0-7 from data bus (D0-D7)
//   bits 8-11 from address lines A8-A11
//
// Two implementations are provided.  The plain one below treats the
// cartridge as read-only and ignores everything except bank switching.
// The FlashROM one further down adds the writable flash device and is
// selected with the tool's -fr option.

typedef struct {
    uint16_t     bank_regs[2];
    bank_cache_t cache;
} ascii16x_bank_ctx_t;

static inline void __not_in_flash_func(handle_ascii16x_bank_write)(uint16_t addr, uint8_t data, void *ctx)
{
    ascii16x_bank_ctx_t *st = (ascii16x_bank_ctx_t *)ctx;

    uint8_t high_nibble = (uint8_t)((addr >> 8) & 0x0Fu);
    uint16_t bank = ((uint16_t)high_nibble << 8) | data;
    uint8_t page;

    switch (addr & 0xF000u)
    {
        case 0x2000u: case 0x6000u: case 0xA000u: case 0xE000u: page = 0; break;
        case 0x3000u: case 0x7000u: case 0xB000u: case 0xF000u: page = 1; break;
        default: return;
    }

    st->bank_regs[page] = bank;
    st->cache.page_slot[page] = bcache_ensure(&st->cache, bank);
}

// -----------------------------------------------------------------------
// loadrom_ascii16x_plain - read-only ASCII16-X
// -----------------------------------------------------------------------
// Uses a mapper-aware LRU cache so every read is served from SRAM.
// Firmware flash is only accessed during bank-switch cache misses, and
// writes only ever move the bank registers.
static void __no_inline_not_in_flash_func(loadrom_ascii16x_plain)(uint32_t offset)
{
    ascii16x_bank_ctx_t state;
    memset(&state, 0, sizeof(state));

    bcache_init(&state.cache, 16384u, 2, rom + offset, active_rom_size);
    bcache_prefill(&state.cache);

    // Both pages start at bank 0 after reset
    state.cache.page_slot[0] = bcache_find(&state.cache, 0);
    state.cache.page_slot[1] = state.cache.page_slot[0];

    msx_pio_bus_init();

    while (true)
    {
        uint16_t addr = pio_get_read_draining_writes(handle_ascii16x_bank_write, &state);

        pio_drain_writes(handle_ascii16x_bank_write, &state);

        // ASCII16-X mirrors ROM across all 4 quadrants.
        // Bit 14 selects the page: 1 = page 1 (regs[0]), 0 = page 2 (regs[1]).
        uint8_t data = 0xFFu;
        uint8_t page_idx = ((addr >> 14) & 0x01u) ? 0u : 1u;
        int8_t slot = state.cache.page_slot[page_idx];

        if (slot >= 0)
            data = rom_sram[(uint32_t)slot * state.cache.slot_size + (addr & 0x3FFFu)];

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(true, data));
    }
}

// -----------------------------------------------------------------------
// AMD-compatible FlashROM emulation for ASCII16-X
// -----------------------------------------------------------------------
// ASCII16-X cartridges are built around an AMD/Spansion-compatible
// FlashROM (the ASCII-X XL 8 MB uses an Infineon S29GL064S), and the
// mapper specification exposes it to the MSX: software can erase sectors
// and program bytes to keep save games, high scores or user created
// levels inside the cartridge.
//
// The emulation implements the command set the specification lists as the
// guaranteed minimum - autoselect, CFI query, chip erase, sector erase and
// byte program - plus the software reset.  Sector geometry follows the
// bottom-boot layout the specification documents: eight 8 KB sectors
// followed by 64 KB sectors.
//
// The flash array itself lives in external PSRAM, with the existing LRU
// bank cache in front of it so MSX reads are still served from internal
// SRAM at full speed.  Programmed data therefore survives bank switches
// (a cache refill reads back the modified array), and storage/flash_sd.c
// mirrors the array into a byte exact image file on the microSD card so
// it also survives a power cycle.
//
// Erases are carried out in small chunks between bus cycles, and while one
// is running every read returns a busy status word instead of array data,
// exactly as the real chip does.  The specification already requires the
// erase/program routine to run from RAM and to poll for completion.
//
// Addressing note: the S29GL064S is a x16 device wired in byte mode, which
// is what the MSX sees.  Commands are therefore decoded on *word*
// addresses (A-1 is ignored), so the unlock cycles land on byte addresses
// XAAAh/XAABh and X554h/X555h, and the CFI query on X0AAh/X0ABh.  The
// autoselect identifiers and the CFI records likewise sit at word offsets,
// so the MSX reads them at even byte addresses with the word's high byte
// in between.  Array data itself is plain byte addressed.

#define FLASH_BOOT_SECTORS      8u        // Number of small boot sectors
#define FLASH_BOOT_SECTOR_SIZE  0x2000u   // 8 KB each
#define FLASH_MAIN_SECTOR_SIZE  0x10000u  // 64 KB for the rest of the device
#define FLASH_ERASE_CHUNK       256u      // Bytes cleared per bus iteration
#define FLASH_CFI_WORDS         0x52u     // CFI records span word 10h-51h

// Command cycle word addresses (byte address >> 1).
#define FLASH_CMD_WORD_AAA      0x555u    // Unlock cycle 1 and command cycle
#define FLASH_CMD_WORD_555      0x2AAu    // Unlock cycle 2
#define FLASH_CMD_WORD_CFI      0x55u     // CFI query (low 8 bits only)

typedef enum {
    FLASH_IDLE = 0,       // Normal read mode
    FLASH_UNLOCK1,        // Received AAh at word 555h
    FLASH_UNLOCK2,        // Received 55h at word 2AAh
    FLASH_BYTE_PGM,       // Received A0h at word 555h – next write programs
    FLASH_ERASE_SETUP,    // Received 80h at word 555h – awaiting second unlock
    FLASH_ERASE_UNLOCK1,  // Received AAh at word 555h (second unlock cycle)
    FLASH_ERASE_UNLOCK2,  // Received 55h at word 2AAh (second unlock cycle)
    FLASH_AUTOSELECT,     // Received 90h – reads return device identifiers
    FLASH_CFI,            // Received 98h – reads return the CFI records
} flash_cmd_state_t;

typedef struct {
    uint16_t         bank_regs[2];
    bank_cache_t     cache;
    flash_cmd_state_t flash_state;

    uint8_t         *array;       // Flash array in PSRAM (NULL = read-only ROM)
    uint32_t         array_size;  // Flash array size in bytes
    bool             persist;     // microSD image file is active

    bool             erase_active; // Chunked erase in progress
    uint32_t         erase_pos;
    uint32_t         erase_end;
    uint8_t          status_toggle;

    uint16_t         cfi[FLASH_CFI_WORDS];
} ascii16x_state_t;

// Resolve the flash offset a bus address maps to through the mapper.
static inline uint32_t __not_in_flash_func(flash_bus_offset)(
    ascii16x_state_t *st, uint16_t addr)
{
    uint8_t page_idx = ((addr >> 14) & 0x01u) ? 0u : 1u;
    uint32_t bank = bcache_norm_bank(&st->cache, st->bank_regs[page_idx]);
    return (bank << 14) | (uint32_t)(addr & 0x3FFFu);
}

// Sector containing a given flash offset, clipped to the array.
static inline void __not_in_flash_func(flash_sector_bounds)(
    ascii16x_state_t *st, uint32_t off, uint32_t *base, uint32_t *end)
{
    uint32_t boot_span = FLASH_BOOT_SECTORS * FLASH_BOOT_SECTOR_SIZE;

    if (off < boot_span)
        *base = off & ~(FLASH_BOOT_SECTOR_SIZE - 1u);
    else
        *base = off & ~(FLASH_MAIN_SECTOR_SIZE - 1u);

    uint32_t size = (off < boot_span) ? FLASH_BOOT_SECTOR_SIZE : FLASH_MAIN_SECTOR_SIZE;
    uint32_t stop = *base + size;
    *end = (stop > st->array_size) ? st->array_size : stop;
}

// Program one byte.  Flash can only clear bits, so the value is ANDed in.
// The backing array and any cached copy of the bank are both updated.
static inline void __not_in_flash_func(flash_program_byte)(
    ascii16x_state_t *st, uint32_t off, uint8_t data)
{
    if (st->array != NULL)
    {
        if (off >= st->array_size)
            return;

        st->array[off] &= data;
        if (st->persist)
            flash_sd_mark_dirty(off, 1u);

        int8_t slot = bcache_find(&st->cache, (uint16_t)(off >> 14));
        if (slot >= 0)
            rom_sram[(uint32_t)slot * st->cache.slot_size + (off & 0x3FFFu)] &= data;
    }
    else
    {
        // No PSRAM: the cache is the only writable copy, so a programmed
        // byte lives only for as long as its bank stays resident.
        int8_t slot = bcache_find(&st->cache, (uint16_t)(off >> 14));
        if (slot >= 0)
            rom_sram[(uint32_t)slot * st->cache.slot_size + (off & 0x3FFFu)] &= data;
    }
}

// Advance a running erase by one chunk.  Called between bus cycles so the
// MSX keeps getting serviced while the "chip" is busy.
static inline void __not_in_flash_func(flash_service_erase)(ascii16x_state_t *st)
{
    if (!st->erase_active)
        return;

    uint32_t pos = st->erase_pos;
    uint32_t in_bank = pos & 0x3FFFu;
    uint32_t n = 0x4000u - in_bank;          // never cross a 16 KB bank
    if (n > FLASH_ERASE_CHUNK)
        n = FLASH_ERASE_CHUNK;
    if (pos + n > st->erase_end)
        n = st->erase_end - pos;

    if (st->array != NULL)
    {
        memset(st->array + pos, 0xFFu, n);
        if (st->persist)
            flash_sd_mark_dirty(pos, n);
    }

    int8_t slot = bcache_find(&st->cache, (uint16_t)(pos >> 14));
    if (slot >= 0)
        memset(&rom_sram[(uint32_t)slot * st->cache.slot_size + in_bank], 0xFFu, n);

    st->erase_pos = pos + n;
    if (st->erase_pos >= st->erase_end)
        st->erase_active = false;
}

static inline void __not_in_flash_func(flash_start_erase)(
    ascii16x_state_t *st, uint32_t base, uint32_t end)
{
    if (end <= base)
        return;
    st->erase_pos = base;
    st->erase_end = end;
    st->erase_active = true;
}

// Autoselect data.  Byte-mode identifiers of the S29GL064S the ASCII-X
// cartridges are built with; every sector reports as unprotected.  The
// device is x16, so each identifier is a word that the MSX reads as two
// consecutive byte addresses.
static inline uint8_t __not_in_flash_func(flash_autoselect_byte)(uint16_t addr)
{
    uint16_t word;

    switch ((addr >> 1) & 0x0Fu)
    {
        case 0x00u: word = 0x0001u; break;  // Manufacturer: AMD / Spansion / Infineon
        case 0x01u: word = 0x227Eu; break;  // Device ID cycle 1
        case 0x02u: word = 0x0000u; break;  // Sector protection: unprotected
        case 0x0Eu: word = 0x2210u; break;  // Device ID cycle 2: 64 Mbit density
        case 0x0Fu: word = 0x2201u; break;  // Device ID cycle 3: bottom boot
        default:    word = 0x0000u; break;
    }

    return (addr & 1u) ? (uint8_t)(word >> 8) : (uint8_t)word;
}

// Read one byte of the CFI records.  The records are 8-bit values held in
// the low half of each word, so odd byte addresses read back as zero.
static inline uint8_t __not_in_flash_func(flash_cfi_byte)(ascii16x_state_t *st, uint16_t addr)
{
    if (addr & 1u)
        return 0x00u;

    uint16_t index = (addr >> 1) & 0x00FFu;
    return (index < FLASH_CFI_WORDS) ? (uint8_t)st->cfi[index] : 0x00u;
}

// Build the CFI records for the emulated device size.  Indices are word
// offsets, which is how the CFI specification numbers them.
static void __not_in_flash_func(flash_build_cfi)(ascii16x_state_t *st)
{
    uint16_t *cfi = st->cfi;
    memset(cfi, 0, sizeof(st->cfi));

    cfi[0x10] = 'Q'; cfi[0x11] = 'R'; cfi[0x12] = 'Y';
    cfi[0x13] = 0x02;                      // Primary command set: AMD/Fujitsu
    cfi[0x15] = 0x40;                      // Primary extended table at 40h
    cfi[0x1B] = 0x27;                      // Vcc min 2.7 V
    cfi[0x1C] = 0x36;                      // Vcc max 3.6 V
    cfi[0x1F] = 0x06;                      // Typical byte program: 2^6 us
    cfi[0x20] = 0x00;                      // No buffered programming
    cfi[0x21] = 0x09;                      // Typical sector erase: 2^9 ms
    cfi[0x22] = 0x13;                      // Typical chip erase: 2^19 ms
    cfi[0x23] = 0x03;                      // Max byte program: 2^3 x typical
    cfi[0x24] = 0x00;                      // No buffered programming
    cfi[0x25] = 0x03;                      // Max sector erase: 2^3 x typical
    cfi[0x26] = 0x02;                      // Max chip erase: 2^2 x typical

    uint8_t size_log2 = 0;
    while ((1u << size_log2) < st->array_size)
        size_log2++;
    cfi[0x27] = size_log2;                 // Device size: 2^n bytes
    cfi[0x28] = 0x00; cfi[0x29] = 0x00;    // Interface: x8 only
    // Byte program is the only write command implemented, so no write
    // buffer is advertised and software cannot pick a buffered algorithm.
    cfi[0x2A] = 0x00;

    uint32_t boot_span = FLASH_BOOT_SECTORS * FLASH_BOOT_SECTOR_SIZE;
    if (st->array_size > boot_span)
    {
        uint32_t main_blocks = (st->array_size - boot_span) / FLASH_MAIN_SECTOR_SIZE;
        cfi[0x2C] = 0x02;                                    // Two erase regions
        cfi[0x2D] = FLASH_BOOT_SECTORS - 1u;                 // 8 x 8 KB
        cfi[0x2F] = (uint8_t)(FLASH_BOOT_SECTOR_SIZE >> 8);  // Size in 256-byte units
        cfi[0x31] = (uint8_t)((main_blocks - 1u) & 0xFFu);   // n x 64 KB
        cfi[0x32] = (uint8_t)((main_blocks - 1u) >> 8);
        cfi[0x34] = (uint8_t)(FLASH_MAIN_SECTOR_SIZE >> 16);
    }
    else
    {
        uint32_t boot_blocks = st->array_size / FLASH_BOOT_SECTOR_SIZE;
        cfi[0x2C] = 0x01;
        cfi[0x2D] = (uint8_t)((boot_blocks - 1u) & 0xFFu);
        cfi[0x2E] = (uint8_t)((boot_blocks - 1u) >> 8);
        cfi[0x2F] = (uint8_t)(FLASH_BOOT_SECTOR_SIZE >> 8);
    }

    cfi[0x40] = 'P'; cfi[0x41] = 'R'; cfi[0x42] = 'I';
    cfi[0x43] = '1'; cfi[0x44] = '3';      // Extended table revision 1.3
    // Everything else in the primary extended table stays zero: no erase
    // suspend, no sector protection and no burst or page mode, none of
    // which this emulation implements.
    cfi[0x4F] = 0x02;                      // Bottom boot sector device
}

// Process a bus write through the AMD flash command state machine.
static inline void __not_in_flash_func(flash_process_write)(
    ascii16x_state_t *st, uint16_t addr, uint8_t data)
{
    // A real device ignores commands while an embedded erase is running.
    // Accepting them here would let a program corrupt the sector being
    // erased, or let a second erase abandon the one in progress.
    if (st->erase_active)
        return;

    // Command cycles are decoded on the word address, so the lowest byte
    // address bit is a don't-care (byte mode A-1).
    uint16_t cmd_word = (uint16_t)((addr >> 1) & 0x07FFu);

    // F0h always returns the device to read mode, and a CFI query needs no
    // unlock sequence, so both are accepted from any read-mode state.
    if (data == 0xF0u &&
        (st->flash_state == FLASH_IDLE || st->flash_state == FLASH_AUTOSELECT ||
         st->flash_state == FLASH_CFI))
    {
        st->flash_state = FLASH_IDLE;
        return;
    }

    if (data == 0x98u && (cmd_word & 0x00FFu) == FLASH_CMD_WORD_CFI &&
        (st->flash_state == FLASH_IDLE || st->flash_state == FLASH_AUTOSELECT))
    {
        st->flash_state = FLASH_CFI;
        return;
    }

    switch (st->flash_state)
    {
        case FLASH_IDLE:
        case FLASH_AUTOSELECT:
        case FLASH_CFI:
            if (cmd_word == FLASH_CMD_WORD_AAA && data == 0xAAu)
                st->flash_state = FLASH_UNLOCK1;
            break;

        case FLASH_UNLOCK1:
            if (cmd_word == FLASH_CMD_WORD_555 && data == 0x55u)
                st->flash_state = FLASH_UNLOCK2;
            else
                st->flash_state = FLASH_IDLE;
            break;

        case FLASH_UNLOCK2:
            if (cmd_word == FLASH_CMD_WORD_AAA)
            {
                switch (data)
                {
                    case 0xA0u: st->flash_state = FLASH_BYTE_PGM;     break;
                    case 0x80u: st->flash_state = FLASH_ERASE_SETUP;  break;
                    case 0x90u: st->flash_state = FLASH_AUTOSELECT;   break;
                    case 0xF0u: st->flash_state = FLASH_IDLE;         break;
                    default:    st->flash_state = FLASH_IDLE;         break;
                }
            }
            else
                st->flash_state = FLASH_IDLE;
            break;

        case FLASH_BYTE_PGM:
            flash_program_byte(st, flash_bus_offset(st, addr), data);
            st->flash_state = FLASH_IDLE;
            break;

        case FLASH_ERASE_SETUP:
            if (cmd_word == FLASH_CMD_WORD_AAA && data == 0xAAu)
                st->flash_state = FLASH_ERASE_UNLOCK1;
            else
                st->flash_state = FLASH_IDLE;
            break;

        case FLASH_ERASE_UNLOCK1:
            if (cmd_word == FLASH_CMD_WORD_555 && data == 0x55u)
                st->flash_state = FLASH_ERASE_UNLOCK2;
            else
                st->flash_state = FLASH_IDLE;
            break;

        case FLASH_ERASE_UNLOCK2:
            if (data == 0x30u)
            {
                uint32_t base, end;
                flash_sector_bounds(st, flash_bus_offset(st, addr), &base, &end);
                flash_start_erase(st, base, end);
            }
            else if (data == 0x10u && cmd_word == FLASH_CMD_WORD_AAA)
            {
                flash_start_erase(st, 0u, st->array_size);
            }
            st->flash_state = FLASH_IDLE;
            break;
    }
}

// Cached ASCII16-X write handler.
static inline void __not_in_flash_func(handle_ascii16x_write_cached)(uint16_t addr, uint8_t data, void *ctx)
{
    ascii16x_state_t *st = (ascii16x_state_t *)ctx;

    // Feed every write to the flash command state machine.
    flash_process_write(st, addr, data);

    // Bank register processing.
    uint8_t high_nibble = (uint8_t)((addr >> 8) & 0x0Fu);
    uint16_t bank = ((uint16_t)high_nibble << 8) | data;
    uint8_t page;

    switch (addr & 0xF000u)
    {
        case 0x2000u: case 0x6000u: case 0xA000u: case 0xE000u: page = 0; break;
        case 0x3000u: case 0x7000u: case 0xB000u: case 0xF000u: page = 1; break;
        default: return;
    }

    st->bank_regs[page] = bank;
    st->cache.page_slot[page] = bcache_ensure(&st->cache, bank);
}

// -----------------------------------------------------------------------
// loadrom_ascii16x_flash - ASCII16-X with FlashROM emulation
// -----------------------------------------------------------------------
// The cartridge ROM is staged into external PSRAM, which becomes the
// writable FlashROM array, and a mapper-aware LRU cache in internal SRAM
// serves every MSX read.  Core 1 mirrors the array to a .FLA image file on
// the microSD card so flash writes survive a power cycle.
static void __no_inline_not_in_flash_func(loadrom_ascii16x_flash)(uint32_t offset)
{    // Freeze the MSX for the whole of the PSRAM staging and card access.
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    static ascii16x_state_t state;
    memset(&state, 0, sizeof(state));

    // Size the emulated device like the real thing: a power of two, at
    // least as large as the cartridge ROM, with anything past the end of
    // the ROM reading as erased (FFh). This also makes the mapper wrap
    // out-of-range bank numbers at the device size, as hardware does.
    uint32_t array_size = FLASH_MAIN_SECTOR_SIZE;
    while (array_size < active_rom_size && array_size < PSRAM_TOTAL_SIZE)
        array_size <<= 1;
    state.array_size = array_size;

    // Stage the cartridge ROM into PSRAM. Without working PSRAM the mapper
    // still runs, but the flash array is limited to whatever the bank cache
    // happens to hold and nothing is persisted.
    if (psram_init())
    {
        psram_mem_init();
        if (psram_alloc(array_size, &flash_region))
        {
            state.array = flash_region.ptr;

            uint32_t copied = (active_rom_size < array_size) ? active_rom_size : array_size;
            memcpy(state.array, rom + offset, copied);
            if (copied < array_size)
                memset(state.array + copied, 0xFFu, array_size - copied);
        }
    }

    // Restore the saved flash image from the card, or create it. Core 1 is
    // only free when no on-cartridge audio engine has claimed it.
    if (state.array != NULL && !dual_psg_audio_started && !scc_audio_ready)
    {
        flash_sd_configure((const char *)rom, state.array, state.array_size,
                           rom + offset, active_rom_size);
        multicore_launch_core1(flash_sd_task);

        // Wait for the card pass to finish before touching the array again.
        // Core 1 is still rewriting it, so starting the bus (and filling the
        // bank cache from it) here would serve partially restored data and
        // leave pinned cache pages permanently stale. The card driver has
        // its own timeouts, so a missing or unresponsive card still gets
        // here quickly; it just reports no backing file.
        while (!flash_sd_ready())
            tight_loop_contents();

        state.persist = flash_sd_backed();
    }

    flash_build_cfi(&state);

    if (state.array != NULL)
        bcache_init(&state.cache, 16384u, 2, state.array, state.array_size);
    else
        bcache_init(&state.cache, 16384u, 2, rom + offset, active_rom_size);
    bcache_prefill(&state.cache);

    // Both pages start at bank 0 after reset
    state.cache.page_slot[0] = bcache_find(&state.cache, 0);
    state.cache.page_slot[1] = state.cache.page_slot[0];

    msx_pio_bus_init();

    const uint32_t read_empty  = 1u << (PIO_FSTAT_RXEMPTY_LSB + msx_bus.sm_read);
    const uint32_t write_empty = 1u << (PIO_FSTAT_RXEMPTY_LSB + msx_bus.sm_write);

    while (true)
    {
        uint16_t addr;

        // Wait for the next read, draining bank-switch/flash writes and
        // advancing any running erase while the bus is idle.
        while (true)
        {
            dual_psg_service_io();

            uint32_t fstat = msx_bus.pio->fstat;

            if (!(fstat & read_empty))
            {
                addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
                break;
            }

            if (!(fstat & write_empty))
                pio_drain_writes(handle_ascii16x_write_cached, &state);
            else
                flash_service_erase(&state);
        }

        pio_drain_writes(handle_ascii16x_write_cached, &state);

        // ASCII16-X mirrors ROM across all 4 quadrants.
        // Bit 14 selects the page: 1 = page 1 (regs[0]), 0 = page 2 (regs[1]).
        uint8_t data = 0xFFu;

        if (state.erase_active)
        {
            // Busy status: DQ7 low, DQ6 toggling, DQ3 set (erase timer
            // expired). Any read of the device reports it while erasing.
            state.status_toggle ^= 0x40u;
            data = 0x08u | state.status_toggle;
        }
        else if (state.flash_state == FLASH_AUTOSELECT)
        {
            data = flash_autoselect_byte(addr);
        }
        else if (state.flash_state == FLASH_CFI)
        {
            data = flash_cfi_byte(&state, addr);
        }
        else
        {
            uint8_t page_idx = ((addr >> 14) & 0x01u) ? 0u : 1u;
            int8_t slot = state.cache.page_slot[page_idx];

            if (slot >= 0)
                data = rom_sram[(uint32_t)slot * state.cache.slot_size + (addr & 0x3FFFu)];
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(true, data));

        // Keep an erase moving even while the MSX polls the cartridge
        // without ever leaving the bus idle.
        flash_service_erase(&state);
    }
}

// FlashROM emulation is opt-in (the tool's -fr option). Without it the
// cartridge behaves as a read-only ASCII16-X and flash command sequences
// are ignored, which is what almost every ASCII16-X ROM expects.
void __no_inline_not_in_flash_func(loadrom_ascii16x)(uint32_t offset, bool cache_enable, bool flashrom)
{
    (void)cache_enable;

    if (flashrom)
        loadrom_ascii16x_flash(offset);
    else
        loadrom_ascii16x_plain(offset);
}

// -----------------------------------------------------------------------
// Manbow2 mapper — Konami SCC banking + AMD AM29F040B flash emulation
// -----------------------------------------------------------------------
// The Manbow2 cartridge uses standard Konami SCC bank switching (4×8KB
// windows at 0x4000-0xBFFF) backed by an AMD AM29F040B 512KB flash chip.
// The last 64KB sector (0x70000-0x7FFFF) is writable and used for save
// data.  All writes in the 0x4000-0xBFFF window pass through both the
// bank-switch decoder and the flash command state machine.
//
// Flash protocol (simplified — only the commands the game actually uses):
//   Program byte:  AA→[x555] 55→[x2AA] A0→[x555] data→[target]
//   Sector erase:  AA→[x555] 55→[x2AA] 80→[x555] AA→[x555] 55→[x2AA] 30→[sector]
//   Auto-select:   AA→[x555] 55→[x2AA] 90→[x555]
//   Reset:         F0→[any]
//
// Save data is backed by SRAM (volatile — lost on power off).  The last
// 64KB of the SRAM pool is reserved for the writable sector.

// Flash state machine states
enum manbow2_flash_state {
    MBW2_READ = 0,    // Normal read mode
    MBW2_CMD1,        // Received AA at x555, waiting for 55 at x2AA
    MBW2_CMD2,        // Received 55 at x2AA, waiting for command byte at x555
    MBW2_AUTOSELECT,  // Auto-select / ID mode
    MBW2_PROGRAM,     // Waiting for single data byte to program
    MBW2_ERASE_CMD1,  // Received 80, waiting for AA at x555
    MBW2_ERASE_CMD2,  // Received AA, waiting for 55 at x2AA
    MBW2_ERASE_CMD3   // Received 55, waiting for 30 + sector address
};

// Context for the Manbow2 mapper write handler and main loop
typedef struct {
    uint8_t bank_regs[4];         // Bank registers (Konami SCC style)
    enum manbow2_flash_state state;
    uint8_t *writable_sram;       // Pointer to 64KB SRAM backing the writable sector
    uint32_t writable_offset;     // First flash byte offset of writable sector (0x70000)
    uint32_t writable_size;       // Size of writable sector (0x10000 = 64KB)
    uint32_t rom_size_mask;       // ROM size - 1 (for bank wrapping)
} manbow2_ctx_t;

// Manbow2 write handler — processes both bank switching and flash commands.
// Every write in 0x4000-0xBFFF is routed here.
static inline void __not_in_flash_func(handle_manbow2_write)(uint16_t addr, uint8_t data, void *ctx)
{
    manbow2_ctx_t *mb = (manbow2_ctx_t *)ctx;

    if (addr < 0x4000u || addr > 0xBFFFu)
        return;

    uint8_t page = (uint8_t)((addr - 0x4000u) >> 13);

    // --- Flash command state machine ---
    // Compute the absolute flash address BEFORE updating the bank register.
    // This matches openMSX: flash.write(addr, value) uses the OLD bank,
    // then setRom(page, value) updates it afterward.
    uint32_t flash_addr = (uint32_t)mb->bank_regs[page] * 0x2000u + (addr & 0x1FFFu);

    // --- Bank switching (Konami SCC style) ---
    // Writes where (addr & 0x1800) == 0x1000 within each 8KB page select banks.
    // Updated AFTER flash_addr is computed (matches real hardware ordering).
    if ((addr & 0x1800u) == 0x1000u)
    {
        mb->bank_regs[page] = data & 0x3Fu;  // mask to 64 blocks (512KB / 8KB)
    }

    // Reset command (F0) from any state
    if (data == 0xF0u)
    {
        mb->state = MBW2_READ;
        return;
    }

    switch (mb->state)
    {
        case MBW2_READ:
            if ((flash_addr & 0x7FFu) == 0x555u && data == 0xAAu)
                mb->state = MBW2_CMD1;
            break;

        case MBW2_CMD1:
            if ((flash_addr & 0x7FFu) == 0x2AAu && data == 0x55u)
                mb->state = MBW2_CMD2;
            else
                mb->state = MBW2_READ;
            break;

        case MBW2_CMD2:
            if (data == 0xA0u)
                mb->state = MBW2_PROGRAM;
            else if (data == 0x90u)
                mb->state = MBW2_AUTOSELECT;
            else if (data == 0x80u)
                mb->state = MBW2_ERASE_CMD1;
            else
                mb->state = MBW2_READ;
            break;

        case MBW2_PROGRAM:
        {
            // Program a single byte: flash can only clear bits (AND with existing data).
            // Only the writable sector accepts programming.
            if (flash_addr >= mb->writable_offset &&
                flash_addr < mb->writable_offset + mb->writable_size)
            {
                uint32_t sram_off = flash_addr - mb->writable_offset;
                mb->writable_sram[sram_off] &= data;  // Flash program = AND
            }
            mb->state = MBW2_READ;
            break;
        }

        case MBW2_ERASE_CMD1:
            if ((flash_addr & 0x7FFu) == 0x555u && data == 0xAAu)
                mb->state = MBW2_ERASE_CMD2;
            else
                mb->state = MBW2_READ;
            break;

        case MBW2_ERASE_CMD2:
            if ((flash_addr & 0x7FFu) == 0x2AAu && data == 0x55u)
                mb->state = MBW2_ERASE_CMD3;
            else
                mb->state = MBW2_READ;
            break;

        case MBW2_ERASE_CMD3:
            if (data == 0x30u)
            {
                // Erase the sector containing flash_addr, if writable.
                if (flash_addr >= mb->writable_offset &&
                    flash_addr < mb->writable_offset + mb->writable_size)
                {
                    memset(mb->writable_sram, 0xFF, mb->writable_size);
                }
            }
            mb->state = MBW2_READ;
            break;

        case MBW2_AUTOSELECT:
            // Only F0 (handled above) exits auto-select mode.
            // Any other write is ignored while in this state.
            break;
    }
}

// -----------------------------------------------------------------------
// loadrom_manbow2 - Manbow2 (Konami SCC + AMD flash) mapper
// -----------------------------------------------------------------------
// 8KB banks: 4000-5FFF, 6000-7FFF, 8000-9FFF, A000-BFFF
// Same bank select addresses as Konami SCC.
// Flash sector 7 (last 64KB of 512KB ROM) is writable via SRAM.
void __no_inline_not_in_flash_func(loadrom_manbow2)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;

    // Reserve the last 64KB of the SRAM pool for the writable flash sector.
    // This leaves the rest for the ROM cache.
    static const uint32_t WRITABLE_SECTOR_SIZE  = 0x10000u;  // 64KB
    static const uint32_t WRITABLE_SECTOR_OFFSET = 0x70000u; // Last sector of 512KB
    uint32_t reduced_cache = CACHE_SIZE - WRITABLE_SECTOR_SIZE;

    // Temporarily reduce cache capacity so prepare_rom_source only uses
    // the first portion of SRAM, leaving 64KB for the writable sector.
    rom_cache_capacity = reduced_cache;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    // Set up the writable SRAM area (last 64KB of rom_sram)
    uint8_t *writable_sram = &rom_sram[reduced_cache];

    // Initialise writable sector with ROM content (the original save data area).
    // If the ROM is large enough to contain sector 7, copy it; otherwise fill 0xFF.
    if (available_length >= WRITABLE_SECTOR_OFFSET + WRITABLE_SECTOR_SIZE)
    {
        // rom_base may point to flash XIP (ROM > cache).  For the writable
        // sector we must always read from flash, not the SRAM cache, because
        // the cache only covers the first portion of the ROM.
        const uint8_t *flash_rom = rom + offset;
        memcpy(writable_sram, flash_rom + WRITABLE_SECTOR_OFFSET, WRITABLE_SECTOR_SIZE);
    }
    else if (available_length > WRITABLE_SECTOR_OFFSET)
    {
        const uint8_t *flash_rom = rom + offset;
        uint32_t partial = available_length - WRITABLE_SECTOR_OFFSET;
        memcpy(writable_sram, flash_rom + WRITABLE_SECTOR_OFFSET, partial);
        memset(writable_sram + partial, 0xFF, WRITABLE_SECTOR_SIZE - partial);
    }
    else
    {
        memset(writable_sram, 0xFF, WRITABLE_SECTOR_SIZE);
    }

    // Initialize Manbow2 context
    manbow2_ctx_t mb = {
        .bank_regs = {0, 1, 2, 3},
        .state = MBW2_READ,
        .writable_sram = writable_sram,
        .writable_offset = WRITABLE_SECTOR_OFFSET,
        .writable_size = WRITABLE_SECTOR_SIZE,
        .rom_size_mask = 0
    };

    msx_pio_bus_init();

    // Main loop: service PIO read/write events.
    // Unlike the generic banked8_loop, this loop continuously drains the
    // write FIFO even while waiting for reads.  Manbow2 generates heavy
    // write traffic (flash data written through 0x5000-0x57FF etc.) that
    // can overflow the 8-entry PIO FIFO if writes are only drained around
    // read events.  Lost bank-switch writes cause the Z80 to read from
    // the wrong bank, crashing the game.  Same pattern as loadrom_sunrise.
    while (true)
    {
        uint16_t addr;

        // Poll: drain write FIFO continuously while waiting for a read
        while (true)
        {
            dual_psg_service_io();
            uint16_t waddr;
            uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata))
            {
                handle_manbow2_write(waddr, wdata, &mb);
            }
            if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
            {
                addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
                break;
            }
        }

        // Drain any writes that arrived alongside the read
        {
            uint16_t waddr;
            uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata))
            {
                handle_manbow2_write(waddr, wdata, &mb);
            }
        }

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t page = (uint8_t)((addr - 0x4000u) >> 13);
            uint32_t rel = (uint32_t)mb.bank_regs[page] * 0x2000u + (addr & 0x1FFFu);

            if (mb.state == MBW2_AUTOSELECT)
            {
                // AMD auto-select: manufacturer ID at offset 0, device ID at offset 1
                // Sector protection at offset 2, extra code at offset 3
                uint32_t id_addr = rel & 0x03u;
                if (id_addr == 0x00u)
                    data = 0x01u;  // AMD manufacturer ID
                else if (id_addr == 0x01u)
                    data = 0xA4u;  // AM29F040B device ID
                else if (id_addr == 0x02u)
                {
                    // Sector write-protect status: 0 = writable, 1 = protected
                    // Only sector 7 (0x70000-0x7FFFF) is writable
                    data = (rel >= mb.writable_offset && rel < mb.writable_offset + mb.writable_size)
                           ? 0x00u : 0x01u;
                }
                else
                    data = 0x00u;
            }
            else
            {
                // Normal read: check writable sector first, then ROM
                if (rel >= mb.writable_offset &&
                    rel < mb.writable_offset + mb.writable_size)
                {
                    data = writable_sram[rel - mb.writable_offset];
                }
                else if (available_length == 0u || rel < available_length)
                {
                    data = read_rom_byte(rom_base, rel);
                }
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

static inline void __not_in_flash_func(handle_ascii16x_write_simple)(uint16_t addr, uint8_t data, uint16_t *bank_regs)
{
    if (addr >= 0x6000u && addr <= 0x67FFu)
        bank_regs[0] = data;
    else if (addr >= 0x7000u && addr <= 0x77FFu)
        bank_regs[1] = data;
}

void __no_inline_not_in_flash_func(loadrom_fmpac)(uint32_t offset, bool cache_enable, uint8_t base_rom_type)
{
    sunrise_wait_for_expanded_bootstrap();

    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_set_enabled(pio0, 1, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    uint8_t bank8_regs[4] = {0, 1, 2, 3};
    uint8_t ascii16_regs[2] = {0, 1};
    // Neo-8/Neo-16 power-on with every 8 KB / 16 KB window mapped to segment 0,
    // matching the standalone loadrom_neo8/loadrom_neo16 loaders. The boot "AB"
    // header lives at segment 0, so all windows must reset to 0 for the BIOS to
    // find it at 0x4000 during the expanded-slot scan. Initialising these to
    // {0,1,2,...} exposed segment 1 at 0x4000, hiding the header and leaving the
    // MSX in BASIC even though the same ROM boots fine without FM-PAC.
    uint16_t neo8_regs[6] = {0, 0, 0, 0, 0, 0};
    uint16_t neo16_regs[3] = {0, 0, 0};
    uint16_t ascii16x_regs[2] = {0, 0};

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);
    const uint8_t *fmpac_bios_base = rom + offset + active_rom_size;

    uint8_t subslot_reg = 0x00u;
    static fmpac_state_t fmpac;
    memset(&fmpac, 0, sizeof(fmpac));
    fmpac.control = 0x10u;

    bank8_ctx_t bank8_ctx = { .bank_regs = bank8_regs };
    bank8_ctx_t ascii16_ctx = { .bank_regs = ascii16_regs };
    bank16_ctx_t neo8_ctx = { .bank_regs = neo8_regs };
    bank16_ctx_t neo16_ctx = { .bank_regs = neo16_regs };

    msx_pio_bus_init();

    // Launch the MSX-MUSIC Core 1 + I2S audio output only now that the
    // expanded-slot bootstrap has finished and the FM-PAC bus responder is
    // live. Starting it earlier (in main()) let the audio core/DMA perturb
    // the cold-boot expanded-slot handshake, which left the MSX dropping to
    // BASIC instead of launching the game.
    if (msx_music_enabled)
        msx_music_start_audio();

    while (true)
    {
        uint16_t waddr;
        uint8_t wdata;
        while (pio_try_get_write(&waddr, &wdata))
        {
            if (waddr == 0xFFFFu)
            {
                subslot_reg = wdata;
                continue;
            }

            uint8_t page = (waddr >> 14) & 0x03u;
            uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;
            if (active_subslot == 0)
            {
                switch (base_rom_type)
                {
                    case 5:  handle_ascii8_write(waddr, wdata, &bank8_ctx); break;
                    case 6:  handle_ascii16_write(waddr, wdata, &ascii16_ctx); break;
                    case 7:  handle_konami_write(waddr, wdata, &bank8_ctx); break;
                    case 8:  handle_neo8_write(waddr, wdata, &neo8_ctx); break;
                    case 9:  handle_neo16_write(waddr, wdata, &neo16_ctx); break;
                    case 12: handle_ascii16x_write_simple(waddr, wdata, ascii16x_regs); break;
                }
            }
            else if (active_subslot == 3)
            {
                fmpac_handle_write(&fmpac, waddr, wdata);
            }
        }

        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
            uint8_t data = 0xFFu;
            bool in_window = true;

            if (addr == 0xFFFFu)
            {
                data = (uint8_t)~subslot_reg;
            }
            else
            {
                uint8_t page = (addr >> 14) & 0x03u;
                uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;
                if (active_subslot == 0)
                {
                    uint32_t rel = 0;
                    bool mapped = false;

                    switch (base_rom_type)
                    {
                        case 1:
                        case 2:
                            mapped = (addr >= 0x4000u && addr <= 0xBFFFu);
                            rel = addr - 0x4000u;
                            break;
                        case 5:
                        case 7:
                            mapped = (addr >= 0x4000u && addr <= 0xBFFFu);
                            if (mapped)
                                rel = ((uint32_t)bank8_regs[(addr - 0x4000u) >> 13] << 13) | (addr & 0x1FFFu);
                            break;
                        case 4:
                            mapped = (addr <= 0xBFFFu);
                            rel = addr;
                            break;
                        case 6:
                            mapped = (addr >= 0x4000u && addr <= 0xBFFFu);
                            if (mapped)
                                rel = ((uint32_t)ascii16_regs[(addr >> 15) & 1u] << 14) | (addr & 0x3FFFu);
                            break;
                        case 8:
                            mapped = (addr <= 0xBFFFu);
                            if (mapped && (addr >> 13) < 6u)
                                rel = ((uint32_t)(neo8_regs[addr >> 13] & 0x0FFFu) << 13) | (addr & 0x1FFFu);
                            break;
                        case 9:
                            mapped = (addr <= 0xBFFFu);
                            if (mapped && (addr >> 14) < 3u)
                                rel = ((uint32_t)(neo16_regs[addr >> 14] & 0x0FFFu) << 14) | (addr & 0x3FFFu);
                            break;
                        case 12:
                            mapped = true;
                            rel = ((uint32_t)ascii16x_regs[((addr >> 14) & 1u) ? 0u : 1u] << 14) | (addr & 0x3FFFu);
                            break;
                        case 13:
                            mapped = true;
                            rel = addr;
                            break;
                    }

                    if (mapped && (available_length == 0u || rel < available_length))
                        data = read_rom_byte(rom_base, rel);
                }
                else if (active_subslot == 3 && addr >= 0x4000u && addr <= 0x7FFFu)
                {
                    data = fmpac_handle_read(&fmpac, fmpac_bios_base, addr);
                }
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }

        tight_loop_contents();
    }
}

// -----------------------------------------------------------------------
// loadrom_manbow2_scc - Manbow2 mapper with SCC/SCC+ sound emulation
// -----------------------------------------------------------------------
// Combines Manbow2 flash emulation (AMD AM29F040B) with SCC audio output
// via emu2212 + I2S on Core 1.  All writes pass through both the Manbow2
// flash/bank state machine AND the SCC emulator.  Reads check the SCC
// register window first, then autoselect, writable sector, and ROM.
void __no_inline_not_in_flash_func(loadrom_manbow2_scc)(uint32_t offset, bool cache_enable, uint32_t scc_type)
{
    const uint8_t *rom_base;
    uint32_t available_length;

    static const uint32_t WRITABLE_SECTOR_SIZE  = 0x10000u;
    static const uint32_t WRITABLE_SECTOR_OFFSET = 0x70000u;
    uint32_t reduced_cache = CACHE_SIZE - WRITABLE_SECTOR_SIZE;

    rom_cache_capacity = reduced_cache;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    uint8_t *writable_sram = &rom_sram[reduced_cache];

    if (available_length >= WRITABLE_SECTOR_OFFSET + WRITABLE_SECTOR_SIZE)
    {
        const uint8_t *flash_rom = rom + offset;
        memcpy(writable_sram, flash_rom + WRITABLE_SECTOR_OFFSET, WRITABLE_SECTOR_SIZE);
    }
    else if (available_length > WRITABLE_SECTOR_OFFSET)
    {
        const uint8_t *flash_rom = rom + offset;
        uint32_t partial = available_length - WRITABLE_SECTOR_OFFSET;
        memcpy(writable_sram, flash_rom + WRITABLE_SECTOR_OFFSET, partial);
        memset(writable_sram + partial, 0xFF, WRITABLE_SECTOR_SIZE - partial);
    }
    else
    {
        memset(writable_sram, 0xFF, WRITABLE_SECTOR_SIZE);
    }

    manbow2_ctx_t mb = {
        .bank_regs = {0, 1, 2, 3},
        .state = MBW2_READ,
        .writable_sram = writable_sram,
        .writable_offset = WRITABLE_SECTOR_OFFSET,
        .writable_size = WRITABLE_SECTOR_SIZE,
        .rom_size_mask = 0
    };

    // Initialise SCC emulator
    memset(&scc_instance, 0, sizeof(SCC));
    scc_instance.clk  = SCC_CLOCK;
    scc_instance.rate = SCC_SAMPLE_RATE;
    SCC_set_quality(&scc_instance, 1);
    scc_instance.type = scc_type;
    SCC_reset(&scc_instance);

    // Start I2S audio subsystem, then launch audio on Core 1
    i2s_audio_init();
    if (dual_psg_ready)
        dual_psg_io_core1 = true;
    multicore_launch_core1(core1_scc_audio);

    msx_pio_bus_init();

    while (true)
    {
        uint16_t addr;

        // Poll: drain write FIFO continuously while waiting for a read
        while (true)
        {
            dual_psg_service_io();
            uint16_t waddr;
            uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata))
            {
                handle_manbow2_write(waddr, wdata, &mb);
                SCC_write(&scc_instance, waddr, wdata);
            }
            if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
            {
                addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
                break;
            }
        }

        // Drain any writes that arrived alongside the read
        {
            uint16_t waddr;
            uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata))
            {
                handle_manbow2_write(waddr, wdata, &mb);
                SCC_write(&scc_instance, waddr, wdata);
            }
        }

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            // Check SCC register read region first
            bool is_scc_read = false;
            if (scc_instance.active)
            {
                uint32_t scc_reg_start = scc_instance.base_adr + 0x800u;
                if (addr >= scc_reg_start && addr <= (scc_reg_start + 0xFFu))
                    is_scc_read = true;
            }
            if (scc_type == SCC_ENHANCED && (addr & 0xFFFEu) == 0xBFFEu)
                is_scc_read = true;

            if (is_scc_read)
            {
                data = (uint8_t)SCC_read(&scc_instance, addr);
            }
            else
            {
                uint8_t page = (uint8_t)((addr - 0x4000u) >> 13);
                uint32_t rel = (uint32_t)mb.bank_regs[page] * 0x2000u + (addr & 0x1FFFu);

                if (mb.state == MBW2_AUTOSELECT)
                {
                    uint32_t id_addr = rel & 0x03u;
                    if (id_addr == 0x00u)
                        data = 0x01u;
                    else if (id_addr == 0x01u)
                        data = 0xA4u;
                    else if (id_addr == 0x02u)
                    {
                        data = (rel >= mb.writable_offset && rel < mb.writable_offset + mb.writable_size)
                               ? 0x00u : 0x01u;
                    }
                    else
                        data = 0x00u;
                }
                else
                {
                    if (rel >= mb.writable_offset &&
                        rel < mb.writable_offset + mb.writable_size)
                    {
                        data = writable_sram[rel - mb.writable_offset];
                    }
                    else if (available_length == 0u || rel < available_length)
                    {
                        data = read_rom_byte(rom_base, rel);
                    }
                }
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// Cases 17/18: Sunrise IDE + 1MB memory mapper + Carnivore2 RAM emulation
// -----------------------------------------------------------------------
// Targets Nextor's SROM /D15 path. Structurally identical to case 16
// (sunrise_mapper_sd) / case 11 (sunrise_mapper_usb): bootstrap warm-
// restart, then expanded-slot main loop with Nextor on sub-slot 0,
// 1 MB mapper RAM on sub-slot 1. On top of that the main loop:
//   * decodes I/O writes/reads on port 0xF0 + PFXN for SROM detection
//   * decodes memory accesses in the 64-byte Carnivore2 register window
//     (default 0x4F80-0x4FBF) and the four programmable bank windows,
//     routing reads/writes to a separate 1 MB PSRAM region (c2_rom_region)
//     that backs the emulated Carnivore2 RAM.
// Writes only touch c2 RAM when the owning bank has R*Mult[5]=1 (RAM select)
// and R*Mult[4]=1 (write enable). This mirrors the VHDL bank decode exactly
// enough for SROM to upload and launch a ROM image.

typedef void (*c2_core1_task_t)(void);

static void __no_inline_not_in_flash_func(loadrom_c2_common)(uint32_t offset,
                                                             bool cache_enable,
                                                             c2_core1_task_t core1_task,
                                                             void (*attach_ctx)(sunrise_ide_t *))
{
    (void)cache_enable;

    uint8_t rom_type = rom[ROM_NAME_MAX];
    bool scc_emulation = (rom_type & SCC_FLAG) != 0;
    bool scc_plus = (rom_type & SCC_PLUS_FLAG) != 0;
    bool c2_scc_enabled = scc_emulation || scc_plus;

    // ---------------------------------------------------------------
    // Phase 1 — Bootstrap: serve a tiny ROM that restarts the MSX so
    // the BIOS sees the final expanded-slot layout on its next probe.
    // ---------------------------------------------------------------
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42,            // 'AB' ROM header
        0x0A, 0x40,            // INIT = 0x400A
        0x00, 0x00,            // STATEMENT = none
        0x00, 0x00,            // DEVICE = none
        0x00, 0x00,            // TEXT = none
        0xF3,                  // DI
        0xDB, 0xF4,            // IN A, (0xF4)
        0xF6, 0x80,            // OR 0x80
        0xD3, 0xF4,            // OUT (0xF4), A
        0xC7                   // RST 0x00
    };

    msx_pio_bus_init();

    bool restart_detected = false;
    bool init_called = false;

    while (!restart_detected)
    {
        { uint16_t waddr; uint8_t wdata; while (pio_try_get_write(&waddr, &wdata)) { } }

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
                if (in_window)
                {
                    uint32_t rel = addr - 0x4000u;
                    if (rel < sizeof(bootstrap_rom)) data = bootstrap_rom[rel];
                }
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
            }
        }
        else
        {
            if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
                restart_detected = true;
        }
    }

    // ---------------------------------------------------------------
    // Phase 2 — freeze MSX before it probes slots again, then wire
    // expanded slot + Nextor + mapper RAM + Carnivore2 emulation.
    // ---------------------------------------------------------------
    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_set_enabled(pio0, 1, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, true, &rom_base, &available_length);

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    attach_ctx(&ide);
    multicore_launch_core1(core1_task);

    // PSRAM regions: 1 MB mapper RAM + 1 MB Carnivore2 RAM.
    psram_mem_init();
    if (!psram_alloc(MAPPER_SIZE, &mapper_region) ||
        !psram_alloc(MAPPER_SIZE, &c2_rom_region))
    {
        while (true) { tight_loop_contents(); }
    }

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t subslot_reg = 0x10;

    mapper_fill_ff();

    // Initialise Carnivore2 state. We report primary slot 1 by default
    // (matches the typical factory layout; SROM only uses this value to
    // verify the port 0xF0 handshake, not to perform bus routing).
    static c2_state_t c2;
    c2_init(&c2, c2_rom_region.ptr, c2_rom_region.size, 0x01u);

    static const uint8_t c2_descr[8] = {
        'C', 'M', 'F', 'C', 'C', 'F', 'R', 'C'
    };
    bool c2_signature_overlay_armed = false;
    uint8_t c2_signature_overlay_index = 0u;

    if (c2_scc_enabled)
    {
        memset(&scc_instance, 0, sizeof(SCC));
        scc_instance.clk = SCC_CLOCK;
        scc_instance.rate = SCC_SAMPLE_RATE;
        SCC_set_quality(&scc_instance, 1);
        scc_instance.type = scc_plus ? SCC_ENHANCED : SCC_STANDARD;
        SCC_reset(&scc_instance);
        i2s_audio_init();
    }

    msx_pio_io_bus_init();
    msx_pio_bus_init();

    // Main loop: service memory + I/O traffic for Nextor, mapper RAM,
    // mapper page registers (FC-FF), Carnivore2 register window, and
    // Carnivore2 bank windows.
    while (true)
    {
        // --- Drain memory writes ---
        {
            uint16_t waddr; uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata))
            {
                // Expanded sub-slot register write at 0xFFFF.
                if (waddr == 0xFFFFu)
                {
                    subslot_reg = wdata;
                    continue;
                }

                uint8_t page = (waddr >> 14) & 0x03u;
                uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;

                if (active_subslot == 1)
                {
                    // Sub-slot 1 = Nextor (IDE BIOS, matches real C2 Sltsl_D).
                    if (waddr >= 0x4000u && waddr <= 0x7FFFu)
                        sunrise_ide_handle_write(&ide, waddr, wdata);
                }
                else if (active_subslot == 0)
                {
                    // Sub-slot 0 = Carnivore2 main (reg window + banks,
                    // matches real C2 Sltsl_C). Addresses not claimed by
                    // the register window or an enabled bank are ignored,
                    // exactly like the real card (no fallback to mapper
                    // RAM — that lives on sub-slot 2).
                    if (c2_addr_is_regwin(&c2, waddr))
                    {
                        c2_reg_write(&c2, waddr, wdata);
                    }
                    else
                    {
                        if (c2_scc_enabled)
                            SCC_write(&scc_instance, waddr, wdata);

                        // Konami-style mapper-latch: BxMask/BxAddr match
                        // updates RxReg on every bank. This is a write
                        // side-channel independent of the data-window
                        // decode below.
                        c2_bank_switch_write(&c2, waddr, wdata);

                        uint8_t bank_idx;
                        uint32_t linear;
                        if (c2_decode_addr(&c2, waddr, &bank_idx, &linear))
                        {
                            bool is_ram = c2_bank_is_ram(&c2, bank_idx);
                            bool is_we  = c2_bank_is_we (&c2, bank_idx);
                            if (is_ram && is_we)
                            {
                                if (linear < c2.ram_size)
                                {
                                    c2.ram_ptr[linear] = wdata;
                                    if (linear + 1u > c2.max_written) c2.max_written = linear + 1u;
                                }
                            }
                            else if (is_we && !is_ram)
                            {
                                // Flash-mode bank: feed the AMD autoselect
                                // state machine so c2ramldr's chip-ID
                                // probe succeeds. Absorbed writes never
                                // reach RAM (flash window is read-only
                                // from the PicoVerse side).
                                c2_flash_cmd_write(&c2, waddr, wdata);
                            }
                        }
                    }
                }
                else if (active_subslot == 2)
                {
                    // Sub-slot 2 = MSX memory mapper RAM
                    // (matches real C2 Sltsl_M).
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (waddr & 0x3FFFu);
                    mapper_write_byte(mapper_offset, wdata);
                }
            }
        }

        // --- Drain I/O writes (mapper page registers FC-FF + Carnivore2 port F0+PFXN) ---
        {
            uint16_t io_addr; uint8_t io_data;
            while (pio_try_get_io_write(&io_addr, &io_data))
            {
                uint8_t port = io_addr & 0xFFu;
                if (port >= 0xFCu && port <= 0xFFu)
                {
                    mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
                }
                else if ((port & 0xFCu) == 0xF0u &&
                         (port & 0x03u) == (c2.pfxn & 0x03u))
                {
                    // SROM's C2 detection path probes PF0 first and then
                    // reads 0x4010+index sequentially. Arm the signature
                    // overlay from that handshake instead of exposing it
                    // unconditionally in page 1.
                    if (io_data == 'C' || io_data == 'S')
                    {
                        c2_signature_overlay_armed = true;
                        c2_signature_overlay_index = 0u;
                    }
                    c2_port_write(&c2, io_data);
                }
            }
        }

        // --- Handle I/O reads (mapper page registers FC-FF + Carnivore2 port F0+PFXN) ---
        {
            uint16_t io_addr;
            while (pio_try_get_io_read(&io_addr))
            {
                uint8_t port = io_addr & 0xFFu;
                bool in_window = false;
                uint8_t data = 0xFFu;
                if (port >= 0xFCu && port <= 0xFFu)
                {
                    in_window = true;
                    data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
                }
                else if ((port & 0xFCu) == 0xF0u &&
                         (port & 0x03u) == (c2.pfxn & 0x03u) &&
                         c2.pf0_state != C2_PF0_IDLE)
                {
                    // Only drive the bus when the detection latch is armed,
                    // matching the VHDL `not(PF0_RV = "00")` gate so we do
                    // not conflict with other devices on the same port.
                    in_window = true;
                    data = c2_port_read(&c2);
                }
                pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read, pio_build_token(in_window, data));
            }
        }

        // --- Handle memory reads ---
        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
            uint8_t data = 0xFFu;
            bool in_window = false;

            // Sub-slot register read.
            if (addr == 0xFFFFu)
            {
                in_window = true;
                data = (uint8_t)~subslot_reg;
            }
            else
            {
                uint8_t page = (addr >> 14) & 0x03u;
                uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;

                // Expose the Carnivore2 firmware signature on any active
                // page-1 ROM view while the register block is enabled.
                // Real C2 serves these bytes from ordinary cartridge ROM;
                // our split Nextor/C2 sub-slot layout means SROM can reach
                // either sub-slot depending on how it ENASLTs the cart, so
                // keep the signature slot-independent for detection.
                if (c2_signature_overlay_armed &&
                    c2_signature_overlay_index < sizeof(c2_descr) &&
                    addr == (uint16_t)(0x4010u + c2_signature_overlay_index) &&
                    (c2.cardmdr & C2_CARDMDR_REGS_DISABLE) == 0u)
                {
                    in_window = true;
                    data = c2_descr[c2_signature_overlay_index++];
                    if (c2_signature_overlay_index >= sizeof(c2_descr))
                    {
                        c2_signature_overlay_armed = false;
                    }
                    pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read,
                                        pio_build_token(in_window, data));
                    continue;
                }
                else if (c2_signature_overlay_armed &&
                         addr >= 0x4010u && addr <= 0x4017u &&
                         (c2.cardmdr & C2_CARDMDR_REGS_DISABLE) == 0u)
                {
                    // Any out-of-sequence access means this is not the
                    // SROM detector loop anymore, so stop overlaying.
                    c2_signature_overlay_armed = false;
                    c2_signature_overlay_index = 0u;
                }

                if (active_subslot == 1)
                {
                    // Sub-slot 1 = Nextor (IDE BIOS, matches real C2 Sltsl_D).
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        in_window = true;
                        uint8_t ide_data;
                        if (sunrise_ide_handle_read(&ide, addr, &ide_data))
                            data = ide_data;
                        else
                        {
                            uint8_t seg = ide.segment;
                            uint32_t rel = ((uint32_t)seg << 14) + (addr & 0x3FFFu);
                            if (available_length == 0u || rel < available_length)
                                data = read_rom_byte(rom_base, rel);
                        }
                    }
                }
                else if (active_subslot == 0)
                {
                    // Sub-slot 0 = Carnivore2 (reg window + banks,
                    // matches real C2 Sltsl_C). Read priority order:
                    //   1. Register window reads (regs enabled AND reg
                    //      readback enabled) — highest priority so SROM
                    //      can read back CardMDR / Rx registers during
                    //      detection / setup.
                    //   2. Bank decode — serves uploaded ROM bytes from
                    //      PSRAM via R1..R4 configs. Takes over once
                    //      SROM has programmed any bank.
                    //   3. Carnivore2 firmware signature "CMFCCFRC" at
                    //      0x4010-0x4017 — SROM's cartridge-detect
                    //      routine (at 0x2051) copies 7 bytes from
                    //      0x4010 via LD HL,0x4010; ADD HL,DE. At
                    //      detection time no banks are armed yet and
                    //      CardMDR=0x30 (bit0=0), so the fallback runs.
                    //      Once banks are live the ROM shadows it.
                    //   4. Open bus (in_window=false).
                    bool regwin_read_active =
                        ((c2.cardmdr & C2_CARDMDR_REGS_DISABLE) == 0u) &&
                        ((c2.cardmdr & C2_CARDMDR_REG_RD_OFF) == 0u) &&
                        c2_addr_is_regwin(&c2, addr);

                    if (regwin_read_active)
                    {
                        in_window = true;
                        data = c2_reg_read(&c2, addr);
                    }
                    else
                    {
                        bool is_scc_read = false;
                        if (c2_scc_enabled)
                        {
                            if (scc_instance.active)
                            {
                                uint32_t scc_reg_start = scc_instance.base_adr + 0x800u;
                                if (addr >= scc_reg_start && addr <= (scc_reg_start + 0xFFu))
                                    is_scc_read = true;
                            }
                            if (scc_plus && (addr & 0xFFFEu) == 0xBFFEu)
                                is_scc_read = true;
                        }

                        if (is_scc_read)
                        {
                            in_window = true;
                            data = (uint8_t)SCC_read(&scc_instance, addr);
                        }
                        else
                        {
                            uint8_t bank_idx;
                            uint32_t linear;
                            if (c2_decode_addr(&c2, addr, &bank_idx, &linear))
                            {
                                in_window = true;
                                if (!c2_bank_is_ram(&c2, bank_idx))
                                {
                                    data = c2_flash_read(&c2, addr, linear);
                                }
                                else
                                {
                                    data = (linear < c2.ram_size) ? c2.ram_ptr[linear] : 0xFFu;
                                }
                            }
                        }
                    }
                }
                else if (active_subslot == 2)
                {
                    // Sub-slot 2 = MSX memory mapper RAM
                    // (matches real C2 Sltsl_M).
                    in_window = true;
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (addr & 0x3FFFu);
                    data = mapper_read_byte(mapper_offset);
                }
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

void __no_inline_not_in_flash_func(loadrom_c2_sd)(uint32_t offset, bool cache_enable)
{
    loadrom_c2_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx);
}

typedef void (*sunrise_backend_task_fn_t)(void);
typedef void (*sunrise_backend_attach_fn_t)(sunrise_ide_t *ide);

static void __not_in_flash_func(sunrise_wait_for_expanded_bootstrap)(void)
{
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42,
        0x0A, 0x40,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0xF3,
        0xDB, 0xF4,
        0xF6, 0x80,
        0xD3, 0xF4,
        0xC7
    };

    msx_pio_bus_init();

    bool restart_detected = false;
    bool init_called = false;

    while (!restart_detected)
    {
        uint16_t waddr;
        uint8_t wdata;
        while (pio_try_get_write(&waddr, &wdata))
        {
        }

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
                if (addr >= 0x400Au && addr <= 0x4011u)
                {
                    init_called = true;
                }

                bool in_window = (addr >= 0x4000u) && (addr <= 0x7FFFu);
                uint8_t data = 0xFFu;
                if (in_window)
                {
                    uint32_t rel = addr - 0x4000u;
                    if (rel < sizeof(bootstrap_rom))
                    {
                        data = bootstrap_rom[rel];
                    }
                }
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
            }
        }
        else if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
        {
            restart_detected = true;
        }
    }
}

static void __not_in_flash_func(loadrom_sunrise_wifi_common)(
    uint32_t offset,
    bool cache_enable,
    sunrise_backend_task_fn_t core1_task,
    sunrise_backend_attach_fn_t attach_ctx,
    bool mapper_enable)
{
    sunrise_wait_for_expanded_bootstrap();

    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_set_enabled(pio0, 1, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);
    const uint8_t *wifi_rom_base = rom + offset + available_length;

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    attach_ctx(&ide);
    multicore_launch_core1(core1_task);
    wifi_uart_init_once();

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t subslot_reg = mapper_enable ? 0x20u : 0x00u;

    if (mapper_enable)
    {
        psram_mem_init();
        if (!psram_alloc(MAPPER_SIZE, &mapper_region))
        {
            while (true) { tight_loop_contents(); }
        }
        mapper_fill_ff();
        msx_pio_io_bus_init();
    }

    msx_pio_bus_init();

    while (true)
    {
        wifi_service_rx();

        uint16_t waddr;
        uint8_t wdata;
        while (pio_try_get_write(&waddr, &wdata))
        {
            if (waddr == 0xFFFFu)
            {
                subslot_reg = wdata;
                continue;
            }

            uint8_t page = (waddr >> 14) & 0x03u;
            uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;

            // Sub-slot 0: WiFi BIOS (initializes first via BIOS slot scan)
            // Sub-slot 1: Sunrise Nextor IDE (initializes after WiFi BIOS)
            // Sub-slot 2: 1 MB mapper RAM (probed at 0x8000 during boot)
            if (active_subslot == 0)
            {
                (void)wifi_handle_mem_write(waddr, wdata);
            }
            else if (active_subslot == 1)
            {
                if (waddr >= 0x4000u && waddr <= 0x7FFFu)
                {
                    sunrise_ide_handle_write(&ide, waddr, wdata);
                }
            }
            else if (mapper_enable && active_subslot == 2)
            {
                uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (waddr & 0x3FFFu);
                mapper_write_byte(mapper_offset, wdata);
            }
        }

        if (mapper_enable)
        {
            uint16_t io_addr;
            uint8_t io_data;
            while (pio_try_get_io_write(&io_addr, &io_data))
            {
                uint8_t port = io_addr & 0xFFu;
                if (port >= 0xFCu && port <= 0xFFu)
                {
                    mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
                }
            }

            while (pio_try_get_io_read(&io_addr))
            {
                uint8_t port = io_addr & 0xFFu;
                bool in_window = false;
                uint8_t data = 0xFFu;
                if (port >= 0xFCu && port <= 0xFFu)
                {
                    in_window = true;
                    data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
                }
                pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read, pio_build_token(in_window, data));
            }
        }

        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
            // Always drive the bus when our slot is selected (FPGA MSX
            // cores require an explicit ack on every read, including
            // reads inside empty sub-slots).
            uint8_t data = 0xFFu;
            bool in_window = true;

            if (addr == 0xFFFFu)
            {
                data = (uint8_t)~subslot_reg;
            }
            else
            {
                uint8_t page = (addr >> 14) & 0x03u;
                uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;

                // Sub-slot 0: WiFi BIOS (initializes first)
                // Sub-slot 1: Sunrise Nextor IDE (initializes after)
                // Sub-slot 2: 1 MB mapper RAM
                if (active_subslot == 0)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        data = wifi_handle_mem_read(wifi_rom_base, addr);
                    }
                }
                else if (active_subslot == 1)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        uint8_t ide_data;
                        if (sunrise_ide_handle_read(&ide, addr, &ide_data))
                        {
                            data = ide_data;
                        }
                        else
                        {
                            uint8_t seg = ide.segment;
                            uint32_t rel = ((uint32_t)seg << 14) + (addr & 0x3FFFu);
                            if (available_length == 0u || rel < available_length)
                            {
                                data = read_rom_byte(rom_base, rel);
                            }
                        }
                    }
                }
                else if (mapper_enable && active_subslot == 2)
                {
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (addr & 0x3FFFu);
                    data = mapper_read_byte(mapper_offset);
                }
                // Other sub-slots: keep default 0xFF (in window)
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

void __no_inline_not_in_flash_func(loadrom_sunrise_wifi)(uint32_t offset, bool cache_enable)
{
    loadrom_sunrise_wifi_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx, false);
}

void __no_inline_not_in_flash_func(loadrom_sunrise_wifi_sd)(uint32_t offset, bool cache_enable)
{
    loadrom_sunrise_wifi_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx, false);
}

void __no_inline_not_in_flash_func(loadrom_sunrise_mapper_wifi)(uint32_t offset, bool cache_enable)
{
    loadrom_sunrise_wifi_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx, true);
}

void __no_inline_not_in_flash_func(loadrom_sunrise_mapper_wifi_sd)(uint32_t offset, bool cache_enable)
{
    loadrom_sunrise_wifi_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx, true);
}

void __no_inline_not_in_flash_func(loadrom_c2_usb)(uint32_t offset, bool cache_enable)
{
    loadrom_c2_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx);
}

static void __not_in_flash_func(loadrom_sunrise_megaram_common)(
    uint32_t offset,
    bool cache_enable,
    sunrise_backend_task_fn_t core1_task,
    sunrise_backend_attach_fn_t attach_ctx)
{
    sunrise_wait_for_expanded_bootstrap();

    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_set_enabled(pio0, 1, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    attach_ctx(&ide);
    multicore_launch_core1(core1_task);

    psram_mem_init();
    if (!psram_alloc(MAPPER_SIZE, &mapper_region) ||
        !psram_alloc(MEGARAM_SIZE, &megaram_region))
    {
        while (true) { tight_loop_contents(); }
    }

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t megaram_bank_reg[4] = { 0, 1, 2, 3 };
    bool megaram_write_enabled = false;
    uint8_t subslot_reg = 0x10u;

    mapper_fill_ff();
    megaram_fill_ff();

    msx_pio_io_bus_init();
    msx_pio_bus_init();

    while (true)
    {
        uint16_t waddr;
        uint8_t wdata;
        while (pio_try_get_write(&waddr, &wdata))
        {
            if (waddr == 0xFFFFu)
            {
                subslot_reg = wdata;
                continue;
            }

            uint8_t page = (waddr >> 14) & 0x03u;
            uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;

            if (active_subslot == 0)
            {
                if (waddr >= 0x4000u && waddr <= 0x7FFFu)
                    sunrise_ide_handle_write(&ide, waddr, wdata);
            }
            else if (active_subslot == 1)
            {
                uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (waddr & 0x3FFFu);
                mapper_write_byte(mapper_offset, wdata);
            }
            else if (active_subslot == 3 && waddr >= 0x4000u && waddr <= 0xBFFFu)
            {
                if (megaram_write_enabled)
                    megaram_write_byte(waddr, wdata, megaram_bank_reg);
                else
                    megaram_bank_switch_write(waddr, wdata, megaram_bank_reg);
            }
        }

        uint16_t io_addr;
        uint8_t io_data;
        while (pio_try_get_io_write(&io_addr, &io_data))
        {
            uint8_t port = io_addr & 0xFFu;
            if (port >= 0xFCu && port <= 0xFFu)
                mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
            else if (port == 0x8Eu || port == 0x8Fu)
                megaram_write_enabled = false;
        }

        while (pio_try_get_io_read(&io_addr))
        {
            uint8_t port = io_addr & 0xFFu;
            bool in_window = false;
            uint8_t data = 0xFFu;
            if (port >= 0xFCu && port <= 0xFFu)
            {
                in_window = true;
                data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
            }
            else if (port == 0x8Eu || port == 0x8Fu)
            {
                megaram_write_enabled = true;
            }
            pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read, pio_build_token(in_window, data));
        }

        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
            uint8_t data = 0xFFu;
            bool in_window = false;

            if (addr == 0xFFFFu)
            {
                in_window = true;
                data = (uint8_t)~subslot_reg;
            }
            else
            {
                uint8_t page = (addr >> 14) & 0x03u;
                uint8_t active_subslot = (subslot_reg >> (page * 2)) & 0x03u;

                if (active_subslot == 0)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        in_window = true;
                        uint8_t ide_data;
                        if (sunrise_ide_handle_read(&ide, addr, &ide_data))
                            data = ide_data;
                        else
                        {
                            uint8_t seg = ide.segment;
                            uint32_t rel = ((uint32_t)seg << 14) + (addr & 0x3FFFu);
                            if (available_length == 0u || rel < available_length)
                                data = read_rom_byte(rom_base, rel);
                        }
                    }
                }
                else if (active_subslot == 1)
                {
                    in_window = true;
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (addr & 0x3FFFu);
                    data = mapper_read_byte(mapper_offset);
                }
                else if (active_subslot == 3 && addr >= 0x4000u && addr <= 0xBFFFu)
                {
                    in_window = true;
                    data = megaram_read_byte(addr, megaram_bank_reg);
                }
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

void __no_inline_not_in_flash_func(loadrom_sunrise_megaram_sd)(uint32_t offset, bool cache_enable)
{
    loadrom_sunrise_megaram_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx);
}

void __no_inline_not_in_flash_func(loadrom_sunrise_megaram_usb)(uint32_t offset, bool cache_enable)
{
    loadrom_sunrise_megaram_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx);
}

int __no_inline_not_in_flash_func(main)()
{
    // Keep existing RP2350 flash timing setup.
    qmi_hw->m[0].timing = 0x40000202;
    set_sys_clock_khz(210000, true);

    setup_gpio();

    char rom_name[ROM_NAME_MAX];
    memcpy(rom_name, rom, ROM_NAME_MAX);
    uint8_t rom_type = rom[ROM_NAME_MAX];

    bool wifi_enabled = (rom_type & WIFI_FLAG) != 0;
    uint8_t base_rom_type = decode_base_rom_type(rom_type, rom_name);

        if ((base_rom_type == 11u || base_rom_type == 16u
            || base_rom_type == 17u || base_rom_type == 18u
            || base_rom_type == 19u || base_rom_type == 20u) && !psram_init())
    {
        while (true)
        {
            tight_loop_contents();
        }
    }

    uint32_t rom_size;
    memcpy(&rom_size, rom + ROM_NAME_MAX + 1, sizeof(uint32_t));
    active_rom_size = rom_size;

    // Resolve which (single) on-cartridge audio engine to enable. The
    // resolver enforces mutual exclusion: only one of SCC / SCC+ / dual
    // PSG / MSX-MUSIC (or any future chip) can be active per ROM.
    bool system_mode = is_system_rom_type(base_rom_type);
    audio_mode_t audio_mode = resolve_audio_mode(rom_type, base_rom_type, system_mode);
    bool scc_emulation = (audio_mode == AUDIO_MODE_SCC);
    bool scc_plus      = (audio_mode == AUDIO_MODE_SCC_PLUS);

    switch (audio_mode)
    {
        case AUDIO_MODE_MSX_MUSIC:
            // Only initialise the YM2413 emulator here. The Core 1 + I2S
            // audio output is launched later, inside loadrom_fmpac() after
            // the expanded-slot bootstrap releases /WAIT, so the running
            // audio core/DMA cannot disturb the timing-critical cold-boot
            // FM-PAC bus handshake (mirrors the Explorer ordering).
            msx_music_enabled = true;
            msx_music_init();
            break;

        case AUDIO_MODE_DUAL_PSG:
            // Standalone PSG-only engine; the tool rejects Dual PSG for
            // SCC-capable mappers before this configuration is generated.
            dual_psg_enabled = true;
            dual_psg_init();
            dual_psg_start_audio();
            break;

        case AUDIO_MODE_SCC:
        case AUDIO_MODE_SCC_PLUS:
            // The Konami-SCC / Manbow2 mapper handlers initialise the SCC
            // engine and launch its audio core1 themselves so they have
            // access to their own bank state.
            break;

        case AUDIO_MODE_NONE:
        default:
            break;
    }

    if (audio_mode == AUDIO_MODE_MSX_MUSIC)
    {
        loadrom_fmpac(ROM_RECORD_SIZE, true, base_rom_type);
        return 0;
    }

    switch (base_rom_type)
    {
        case 1:
        case 2:
            loadrom_plain32(ROM_RECORD_SIZE, true);
            break;
        case 3:
            if (scc_emulation || scc_plus)
                loadrom_konamiscc_scc(ROM_RECORD_SIZE, true, scc_plus ? SCC_ENHANCED : SCC_STANDARD);
            else
                loadrom_konamiscc(ROM_RECORD_SIZE, true);
            break;
        case 4:
            loadrom_linear48(ROM_RECORD_SIZE, true);
            break;
        case 5:
            loadrom_ascii8(ROM_RECORD_SIZE, true);
            break;
        case 6:
            loadrom_ascii16(ROM_RECORD_SIZE, true);
            break;
        case 7:
            loadrom_konami(ROM_RECORD_SIZE, true);
            break;
        case 8:
            loadrom_neo8(ROM_RECORD_SIZE);
            break;
        case 9:
            loadrom_neo16(ROM_RECORD_SIZE);
            break;
        case 10:
            if (wifi_enabled)
                loadrom_sunrise_wifi(ROM_RECORD_SIZE, true);
            else
                loadrom_sunrise(ROM_RECORD_SIZE, true);
            break;
        case 11:
            if (wifi_enabled)
                loadrom_sunrise_mapper_wifi(ROM_RECORD_SIZE, true);
            else
                loadrom_sunrise_mapper(ROM_RECORD_SIZE, true);
            break;
        case 12:
            loadrom_ascii16x(ROM_RECORD_SIZE, true, (rom_type & FLASHROM_FLAG) != 0);
            break;
        case 13:
            loadrom_planar64(ROM_RECORD_SIZE, true);
            break;
        case 14:
            if (scc_emulation || scc_plus)
                loadrom_manbow2_scc(ROM_RECORD_SIZE, true, scc_plus ? SCC_ENHANCED : SCC_STANDARD);
            else
                loadrom_manbow2(ROM_RECORD_SIZE, true);
            break;
        case 15:
            if (wifi_enabled)
                loadrom_sunrise_wifi_sd(ROM_RECORD_SIZE, true);
            else
                loadrom_sunrise_sd(ROM_RECORD_SIZE, true);
            break;
        case 16:
            if (wifi_enabled)
                loadrom_sunrise_mapper_wifi_sd(ROM_RECORD_SIZE, true);
            else
                loadrom_sunrise_mapper_sd(ROM_RECORD_SIZE, true);
            break;
        case 17:
            loadrom_c2_sd(ROM_RECORD_SIZE, true);
            break;
        case 18:
            loadrom_c2_usb(ROM_RECORD_SIZE, true);
            break;
        case 19:
            loadrom_sunrise_megaram_sd(ROM_RECORD_SIZE, true);
            break;
        case 20:
            loadrom_sunrise_megaram_usb(ROM_RECORD_SIZE, true);
            break;
        default:
            break;
    }

    return 0;
}
