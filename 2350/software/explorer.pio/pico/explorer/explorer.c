// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// explorer.c - PicoVerse 2350 Explorer firmware (ROM loader + SD support)
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include "ff.h"
#include "diskio.h"
#include "my_rtc.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/structs/qmi.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/regs/uart.h"
#include "hardware/irq.h"
#include "hw_config.h"
#include "explorer.h"
#include "mapper_detect.h"
#include "audio/mp3.h"
#include "memory/c2_emu.h"
#include "audio/emu2212.h"
#include "audio/emu2149.h"
#include "audio/emu2413.h"
#include "audio/emu2151.h"
#include "msx_bus.pio.h"
#include "pico/audio_i2s.h"
#include "storage/sunrise_ide.h"
#include "storage/sunrise_sd.h"

// config area and buffer for the ROM data
#define ROM_NAME_MAX    71          // Maximum size of the ROM name on the 80-column detail screen
#define MAX_ROM_RECORDS 1024        // Maximum ROM files supported (flash + SD)
#define MAX_FLASH_RECORDS 128       // Maximum ROM files stored in flash
#define ROM_RECORD_SIZE (ROM_NAME_MAX + 1 + (sizeof(uint32_t) * 2)) // Name + mapper + size + offset
#define MENU_ROM_SIZE   (32u * 1024u) // Full menu ROM size stored before config records
#define CONFIG_AREA_SIZE (16u * 1024u)
#define ROM_SELECT_MENU 0xFEu
#define ROM_SELECT_WIFI_CONFIG 0xFFF0u
#define WIFI_CONFIG_FLASH_OFFSET (MENU_ROM_SIZE + CONFIG_AREA_SIZE)
#define WIFI_CONFIG_ROM_SIZE (8u * 1024u)
#define WIFI_BIOS_FLASH_OFFSET (WIFI_CONFIG_FLASH_OFFSET + WIFI_CONFIG_ROM_SIZE)
#define WIFI_BIOS_ROM_SIZE (16u * 1024u)
#define FMPAC_BIOS_FLASH_OFFSET (WIFI_BIOS_FLASH_OFFSET + WIFI_BIOS_ROM_SIZE)
#define FMPAC_BIOS_ROM_SIZE (64u * 1024u)
#define SFG_BIOS_FLASH_OFFSET (FMPAC_BIOS_FLASH_OFFSET + FMPAC_BIOS_ROM_SIZE)
#define SFG_BIOS_ROM_SIZE (64u * 1024u)
#define WIFI_CONFIG_RETURN_MENU 0xE0u
#define MONITOR_ADDR    (0xBF7F)    // ROM select register just below the menu control window
#define CACHE_SIZE      (256u * 1024u)     // 256KB cache size for ROM data

#define SOURCE_SD_FLAG  0x80 // Flag in the mapper byte indicating the ROM is on SD
#define FOLDER_FLAG     0x40 // Flag in the mapper byte indicating the record is a folder
#define MP3_FLAG        0x20 // Flag in the mapper byte indicating the record is an MP3 file
#define OVERRIDE_FLAG   0x10 // Flag in the mapper byte indicating manual override
#define AUDIO_TYPE_MASK 0x0F // Low bits identify audio subtypes when MP3_FLAG is set
#define AUDIO_TYPE_WAV  0x01 // Audio subtype: PCM WAV file

#define EXPLORER_MP3_DISABLED 0

#define FILES_PER_PAGE  19 // Maximum files per page on the menu
#define CTRL_BASE_ADDR  0xBFF0 // Control registers base address
#define CTRL_COUNT_L    (CTRL_BASE_ADDR + 0) // Control: total record count low byte
#define CTRL_COUNT_H    (CTRL_BASE_ADDR + 1) // Control: total record count high byte
#define CTRL_PAGE       (CTRL_BASE_ADDR + 2) // Control: current page index
#define CTRL_STATUS     (CTRL_BASE_ADDR + 3) // Control: status register
#define CTRL_CMD        (CTRL_BASE_ADDR + 4) // Control: command register
#define CTRL_MATCH_L    (CTRL_BASE_ADDR + 5) // Control: match index low byte
#define CTRL_MATCH_H    (CTRL_BASE_ADDR + 6) // Control: match index high byte
#define CTRL_MAPPER     (CTRL_BASE_ADDR + 7) // Control: mapper value
#define CTRL_ACK        (CTRL_BASE_ADDR + 8) // Control: command ack
#define CTRL_MAGIC      0xA5 // Control: magic value to indicate valid command
#define CTRL_STATUS_SD_MISSING 0x5D // Control: selected source is microSD but no card is available
#define CTRL_QUERY_BASE 0xBFC0 // Control: query string base address
#define CTRL_QUERY_SIZE 32 // Control: query string size
#define CMD_APPLY_FILTER 0x01 // Command: apply filter
#define CMD_FIND_FIRST   0x02 // Command: find first match
#define CMD_ENTER_DIR    0x03 // Command: enter directory
#define CMD_DETECT_MAPPER 0x04 // Command: detect mapper on demand
#define CMD_SET_MAPPER    0x05 // Command: set mapper override
#define CMD_SET_SOURCE    0x06 // Command: switch visible source set
#define CMD_LOAD_OPTIONS  0x07 // Command: load ROM options from microSD .PVC file
#define CMD_SAVE_OPTIONS  0x08 // Command: save ROM options to microSD .PVC file
#define CMD_PREPARE_QUICK_RUN 0x09 // Command: load saved/default launch options for quick-run
#define CMD_CYCLE_SD_PARTITION 0x0A // Command: cycle Explorer microSD browse partition
#define CMD_DELETE_SD_FILE 0x0B // Command: delete selected Explorer microSD file
#define CMD_LOAD_LAST_SELECTION 0x0C // Command: load the last executed entry from microSD
#define CMD_SAVE_LAST_SELECTION 0x0D // Command: save the entry being executed to microSD
#define CMD_FH_LIST_PAGE   0x40 // Command: File Hunter list page from menu ROM
#define CMD_FH_DOWNLOAD    0x41 // Command: File Hunter download/save from menu ROM
#define CMD_FH_SEARCH      0x42 // Command: File Hunter search from menu ROM
#define CMD_FH_WIFI_STATUS 0x43 // Command: File Hunter WiFi status from menu ROM
#define CMD_FH_WIFI_CONFIG 0x44 // Command: File Hunter WiFi setup from menu ROM

#define FH_CTRL_BASE    0xBFF0
#define FH_COUNT_L      (FH_CTRL_BASE + 0)
#define FH_COUNT_H      (FH_CTRL_BASE + 1)
#define FH_PAGE         (FH_CTRL_BASE + 2)
#define FH_STATUS       (FH_CTRL_BASE + 3)
#define FH_CMD          (FH_CTRL_BASE + 4)
#define FH_SELECT_L     (FH_CTRL_BASE + 5)
#define FH_SELECT_H     (FH_CTRL_BASE + 6)
#define FH_PROGRESS_L   (FH_CTRL_BASE + 7)
#define FH_PROGRESS_H   (FH_CTRL_BASE + 8)
#define FH_RESULT       (FH_CTRL_BASE + 9)
#define FH_QUERY_BASE   0xBFC0
#define FH_QUERY_SIZE   32
#define FH_STATUS_TEXT_BASE 0xBF80
#define FH_STATUS_TEXT_SIZE 64
#define CTRL_CHIP_ID_BASE (FH_STATUS_TEXT_BASE + FH_STATUS_TEXT_SIZE - 17u) // Control: Pico unique board ID string
#define CTRL_CHIP_ID_SIZE 17u // Control: 16 hex digits plus terminator
#define CTRL_SD_PARTITION_INFO_BASE FH_STATUS_TEXT_BASE
#define CTRL_SD_PARTITION_INFO_SIZE 32u
#define CTRL_VDP_FREQ   0xBFA0 // Control: per-ROM VDP 50/60Hz load read-back (Pico -> MSX)
#define CTRL_NET_STATUS 0xBFA1 // Control: ESP-01 network link state read-back (Pico -> MSX)
#define NET_STATUS_OFFLINE 0u  // ESP-01 missing, not answering or not joined to an access point
#define NET_STATUS_ONLINE  1u  // ESP-01 answered and reports an active access point connection
#define VDP_FREQ_DEFAULT 0u
#define VDP_FREQ_60HZ    1u
#define VDP_FREQ_50HZ    2u
#define CTRL_CPU_MODE   0xBFA2 // Control: per-ROM CPU speed load read-back (Pico -> MSX)
#define CPU_MODE_DEFAULT 0u    // Leave the CPU as the BIOS boots it
#define CPU_MODE_TURBO   1u    // Panasonic MSX2+/turbo R switched-I/O 5.37MHz Z80 turbo
#define CPU_MODE_R800    2u    // MSX turbo R R800 ROM mode via the CHGCPU BIOS entry
#define FH_DATA_BASE    0xB900
#define FH_RECORD_FLAG_OFFSET ROM_NAME_MAX
#define FH_RECORD_SIZE_OFFSET (FH_RECORD_FLAG_OFFSET + 1u)
#define FH_RECORD_SIZE  (ROM_NAME_MAX + 3u)
#define FH_FILES_PER_PAGE 19
#define FH_MAX_RESULTS 384
#define FH_HTTP_BUFFER_SIZE (256u * 1024u)
#define FH_HTTP_CHUNK_SIZE 2048u
#define FH_HOST "msxpico.file-hunter.com"
#define FH_ENDPOINT "/picoverse.php"        // Name-filtered catalog, used by the search key
#define FH_LATEST_ENDPOINT "/new-pico.php"  // Latest releases (newest first), used when File Hunter opens
                                            // An empty query selects FH_LATEST_ENDPOINT; see fh_build_http_path()
#define FH_WIFI_CONNECT_WAIT_US 30000000u
#define FH_STATUS_READY 0xA5
#define FH_STATUS_BUSY  0x5A
#define FH_RESULT_SAVING 0x02
#define FH_RECORD_FLAG_MESSAGE 0x80u
#define FH_CMD_LIST_PAGE 0x01
#define FH_CMD_DOWNLOAD  0x02
#define FH_CMD_EXIT      0x03
#define FH_CMD_SEARCH    0x04
#define FH_CMD_WIFI_STATUS 0x05

#define SOURCE_MODE_ALL   0x00
#define SOURCE_MODE_FLASH 0x01
#define SOURCE_MODE_SD    0x02
#define SOURCE_MODE_FILEHUNTER 0x03

#define CTRL_AUDIO      (CTRL_BASE_ADDR + 9) // Control: audio profile selection
#define CTRL_WIFI_SUPPORT (CTRL_BASE_ADDR + 10) // Control: Sunrise WiFi support (0=No, 1=Yes)
#define CTRL_PSG_EMULATION (CTRL_BASE_ADDR + 11) // Control: primary PSG emulation (0=No, 1=Yes)
#define CTRL_WAVEGAME_ROM (CTRL_BASE_ADDR + 12) // Control: selected SD ROM has WAVEGAME sidecar audio
#define CTRL_SD_PARTITION (CTRL_BASE_ADDR + 13) // Control: selected Sunrise SD partition number
#define CTRL_SD_BROWSE_PARTITION (CTRL_BASE_ADDR + 14) // Control: selected Explorer microSD partition number
#define CTRL_AUDIO_VOLUME (CTRL_BASE_ADDR + 15) // Control: per-ROM audio volume percent

#define AUDIO_PROFILE_NONE       0u
#define AUDIO_PROFILE_SCC        1u
#define AUDIO_PROFILE_SCC_PLUS   2u
#define AUDIO_PROFILE_DUAL_PSG 3u
#define AUDIO_PROFILE_MSX_MUSIC 4u
#define AUDIO_PROFILE_SCC_EXTERNAL 5u
#define AUDIO_PROFILE_SCC_PLUS_EXTERNAL 6u
#define AUDIO_PROFILE_YM2151_SFG05 7u
#define AUDIO_PROFILE_YM2151_SFG01 8u
#define AUDIO_PROFILE_MEGARAM_SCC 9u
#define AUDIO_PROFILE_MEGARAM_SCC_PLUS 10u
// Same SFG05/SFG01 surfaces, but the emulated OPM/OPP core is clocked at 4 MHz
// instead of the MSX 3.579545 MHz, matching the arcade boards most YM2151 music
// was written for.
#define AUDIO_PROFILE_YM2151_SFG05_4MHZ 11u
#define AUDIO_PROFILE_YM2151_SFG01_4MHZ 12u

#define PSG_SAMPLE_RATE 44100
#define PSG_CLOCK       1789773
#define PSG_VOLUME_SHIFT 2
#define PSG_QUALITY_FAST 0
#define PSG_QUALITY_HIGH 1
#define PSG_PORT_REG    0x10u
#define PSG_PORT_DATA   0x11u
#define MAIN_PSG_PORT_REG  0xA0u
#define MAIN_PSG_PORT_DATA 0xA1u

#define MSX_MUSIC_SAMPLE_RATE 44100
#define MSX_MUSIC_CLOCK       3579545
#define MSX_MUSIC_VOLUME_SHIFT 2
#define MSX_MUSIC_PSG_VOLUME_SHIFT 0
/* Soft-knee limiter for the post-gain FM sample: below the knee the output is
 * a pure linear MSX_MUSIC_VOLUME_SHIFT gain (so normal games are unchanged);
 * above the knee, loud/dense OPLL content is smoothly compressed toward the
 * ceiling instead of hard-clipping (which produced a robotic, low-volume tone
 * on heavy arrangements like Aleste Gaiden). */
#define MSX_MUSIC_LIMIT_KNEE  24000
#define MSX_MUSIC_LIMIT_CEIL  32767
#define MSX_MUSIC_PORT_REG    0x7Cu
#define MSX_MUSIC_PORT_DATA   0x7Du
#define FMPAC_BIOS_SIZE       FMPAC_BIOS_ROM_SIZE
#define SFG_BIOS_SIZE         SFG_BIOS_ROM_SIZE
#define SFG_BIOS_VARIANT_SIZE (32u * 1024u)

#define AUDIO_VOLUME_DEFAULT 100u
#define AUDIO_VOLUME_MAX     200u

#define YM2151_SAMPLE_RATE    44100
#define YM2151_CLOCK          3579545u
#define YM2151_CLOCK_4MHZ     4000000u
#define YM2151_FRAME_DIVIDER  64u
#define YM2151_CYCLES_PER_FRAME 1u
#define YM2151_WRITE_APPLY_CYCLES 0u
#define YM2151_WRITE_SERVICE_BUDGET 4u
#define YM2151_WRITE_QUEUE_SIZE 512u
#define YM2151_WRITE_QUEUE_MASK (YM2151_WRITE_QUEUE_SIZE - 1u)
#define YM2151_PSG_VOLUME_SHIFT 0
#define YM2151_BASE_VOLUME_PERCENT 200u

#define WIFI_MEM_F2_ADDR      0x7F05u
#define WIFI_MEM_CMD_ADDR     0x7F06u
#define WIFI_MEM_DATA_ADDR    0x7F07u
#define WIFI_F2_FORCE_SETUP   0xF1u
#define WIFI_RX_FIFO_SIZE     2080u
#define WIFI_RX_SERVICE_BUDGET 32u
#define WIFI_QUICK_WAIT_US    25000u
#define WIFI_UART_DEFAULT_BAUD 859372u
#define WIFI_STATUS_RX_READY  0x01u
#define WIFI_STATUS_TX_BUSY   0x02u
#define WIFI_STATUS_RX_FULL   0x04u
#define WIFI_STATUS_QUICK_RX  0x08u
#define WIFI_STATUS_UNDERRUN  0x10u
#define WIFI_STATUS_FREE_BITS 0x80u
#define WIFI_UART_INSTANCE    uart1

#define DATA_BASE_ADDR   0xB900 // Data buffer base address
#define DATA_BUFFER_SIZE (FH_STATUS_TEXT_BASE - DATA_BASE_ADDR) // Data buffer size
#define DATA_MAGIC_0     'P' // Data header magic bytes
#define DATA_MAGIC_1     'V' // Data header magic bytes
#define DATA_MAGIC_2     'E' // Data header magic bytes
#define DATA_MAGIC_3     'X' // Data header magic bytes
#define DATA_VERSION     1 // Data header version
#define DATA_HEADER_SIZE 24 // Data header size in bytes
#define DATA_RECORD_TABLE_ENTRY_SIZE 4 // Size of each record table entry
#define DATA_RECORD_PAYLOAD_SIZE 13 // Minimum size of each record payload
#define TLV_NAME_OFFSET  0x01 // Type-Length-Value: Name offset
#define TLV_MAPPER       0x02 // Type-Length-Value: Mapper
#define TLV_SIZE         0x03 // Type-Length-Value: Size

#define PVC_OPTIONS_MAGIC_0 'P'
#define PVC_OPTIONS_MAGIC_1 'V'
#define PVC_OPTIONS_MAGIC_2 'C'
#define PVC_OPTIONS_MAGIC_3 '1'
#define PVC_OPTIONS_LEGACY_SIZE 6u
#define PVC_OPTIONS_MAPPER_SIZE 7u
#define PVC_OPTIONS_PARTITION_SIZE 8u
#define PVC_OPTIONS_VOLUME_SIZE 9u
#define PVC_OPTIONS_FREQ_SIZE 10u
#define PVC_OPTIONS_SIZE 11u

#define PV_CONFIG_MAGIC_0 'P'
#define PV_CONFIG_MAGIC_1 'V'
#define PV_CONFIG_MAGIC_2 'C'
#define PV_CONFIG_MAGIC_3 'F'
#define PV_CONFIG_SIZE 5u
#define PV_CONFIG_PATH "/PICOVERSE.PVC"

// Last executed Flash/microSD entry, remembered across power cycles.
#define PV_LAST_MAGIC_0 'P'
#define PV_LAST_MAGIC_1 'V'
#define PV_LAST_MAGIC_2 'L'
#define PV_LAST_MAGIC_3 'S'
#define PV_LAST_VERSION 1u
#define PV_LAST_HEADER_SIZE 8u
#define PV_LAST_PATH "/PICOVERSE.PVL"


// MP3 control registers
#define MP3_CTRL_BASE      0xBFE0 // MP3 control registers base address
#define MP3_CTRL_CMD       (MP3_CTRL_BASE + 0)
#define MP3_CTRL_STATUS    (MP3_CTRL_BASE + 1)
#define MP3_CTRL_INDEX_L   (MP3_CTRL_BASE + 2)
#define MP3_CTRL_INDEX_H   (MP3_CTRL_BASE + 3)
#define MP3_CTRL_ELAPSED_L (MP3_CTRL_BASE + 4)
#define MP3_CTRL_ELAPSED_H (MP3_CTRL_BASE + 5)
#define MP3_CTRL_TOTAL_L   (MP3_CTRL_BASE + 6)
#define MP3_CTRL_TOTAL_H   (MP3_CTRL_BASE + 7)
#define MP3_CTRL_MODE      (MP3_CTRL_BASE + 8)

// MSX protocol command code for MP3 file selection (local to explorer.c)
#define MP3_CMD_SELECT      0x01

typedef struct {
    PIO pio;
    uint sm_read;
    uint sm_write;
    uint offset_read;
    uint offset_write;
} msx_pio_bus_t;

typedef struct {
    PIO pio_read;
    PIO pio_write;
    uint sm_io_read;
    uint sm_io_write;
    uint offset_io_read;
    uint offset_io_write;
} msx_pio_io_bus_t;

static msx_pio_bus_t msx_bus;
static bool msx_bus_programs_loaded = false;
static msx_pio_io_bus_t msx_io_bus;
static bool msx_io_read_program_loaded = false;
static bool msx_io_write_program_loaded = false;

typedef struct {
    uint16_t rom_index;
    bool rom_selected;
} menu_select_ctx_t;

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

// This symbol marks the end of the main program in flash.
// Custom data starts right after it
extern const uint8_t __flash_binary_end[];

// PSRAM-backed buffer to cache ROM data
static uint8_t *rom_sram = NULL;
static uint32_t active_rom_size = 0;
static uint32_t rom_cached_size = 0; // Added for PIO implementation
// Effective cache capacity used by prepare_rom_source(). Some mappers (e.g.
// Manbow2) reduce this so the tail of rom_sram can be repurposed (writable
// flash sector emulation), matching multirom's behaviour.
static uint32_t rom_cache_capacity = CACHE_SIZE;
static uint8_t page_buffer[DATA_BUFFER_SIZE];

// pointer to the custom data
static const uint8_t *flash_rom = (const uint8_t *)&__flash_binary_end;
static const uint8_t *rom_data = (const uint8_t *)&__flash_binary_end;
static bool rom_data_in_ram = false;

static uint8_t wifi_rx_fifo[WIFI_RX_FIFO_SIZE];
static volatile uint16_t wifi_rx_head = 0u;
static volatile uint16_t wifi_rx_tail = 0u;
static volatile uint16_t wifi_rx_count = 0u;
static uint8_t wifi_f2_state = 0xFFu;
static bool wifi_uart_ready = false;
static uint32_t wifi_uart_baud = WIFI_UART_DEFAULT_BAUD;
static uint32_t wifi_tx_busy_deadline_us = 0u;
static bool wifi_rx_underrun = false;

static void debug_trace(const char *fmt, ...)
{
#if EXPLORER_USB_STDIO_DEBUG
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\r\n");
    fflush(stdout);
#else
    (void)fmt;
#endif
}

static inline void __not_in_flash_func(wifi_reset_fifo)(void)
{
    uint32_t irq_state = save_and_disable_interrupts();
    wifi_rx_head = 0u;
    wifi_rx_tail = 0u;
    wifi_rx_count = 0u;
    wifi_rx_underrun = false;
    restore_interrupts(irq_state);
}

static inline bool __not_in_flash_func(wifi_push_rx_byte_locked)(uint8_t data)
{
    if (wifi_rx_count >= WIFI_RX_FIFO_SIZE) return false;
    wifi_rx_fifo[wifi_rx_head] = data;
    wifi_rx_head = (uint16_t)((wifi_rx_head + 1u) % WIFI_RX_FIFO_SIZE);
    ++wifi_rx_count;
    return true;
}

static inline bool __not_in_flash_func(wifi_push_rx_byte)(uint8_t data)
{
    uint32_t irq_state = save_and_disable_interrupts();
    bool pushed = wifi_push_rx_byte_locked(data);
    restore_interrupts(irq_state);
    return pushed;
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

static inline uint16_t __not_in_flash_func(wifi_rx_count_snapshot)(void)
{
    uint32_t irq_state = save_and_disable_interrupts();
    uint16_t count = wifi_rx_count;
    restore_interrupts(irq_state);
    return count;
}

static inline bool __not_in_flash_func(wifi_hw_tx_busy)(void)
{
    return (uart_get_hw(WIFI_UART_INSTANCE)->fr & UART_UARTFR_BUSY_BITS) != 0u;
}

static inline void __not_in_flash_func(wifi_hw_rx_drain)(void)
{
    uint32_t irq_state = save_and_disable_interrupts();
    while (uart_is_readable(WIFI_UART_INSTANCE))
    {
        (void)uart_getc(WIFI_UART_INSTANCE);
    }
    restore_interrupts(irq_state);
}

static inline void __not_in_flash_func(wifi_pio_rx_reset)(void)
{
    wifi_hw_rx_drain();
}

static inline void __not_in_flash_func(wifi_pio_tx_reset)(void)
{
    while (wifi_hw_tx_busy()) { }
    wifi_tx_busy_deadline_us = 0u;
}

static inline void __not_in_flash_func(wifi_service_rx)(void)
{
    uint16_t budget = WIFI_RX_SERVICE_BUDGET;
    uint32_t irq_state;

    if (!wifi_uart_ready) return;
    irq_state = save_and_disable_interrupts();
    while (budget-- != 0u && uart_is_readable(WIFI_UART_INSTANCE))
    {
        if (!wifi_push_rx_byte_locked((uint8_t)uart_getc(WIFI_UART_INSTANCE))) break;
    }
    restore_interrupts(irq_state);
}

static void __not_in_flash_func(wifi_uart_rx_irq)(void)
{
    while (uart_is_readable(WIFI_UART_INSTANCE))
    {
        uint8_t data = (uint8_t)uart_getc(WIFI_UART_INSTANCE);
        (void)wifi_push_rx_byte_locked(data);
    }
}

static inline uint32_t __not_in_flash_func(wifi_uart_frame_time_us)(void)
{
    return (10000000u + (wifi_uart_baud - 1u)) / wifi_uart_baud;
}

static inline void __not_in_flash_func(wifi_uart_init_once)(void)
{
    if (wifi_uart_ready) return;

    (void)uart_init(WIFI_UART_INSTANCE, wifi_uart_baud);
    gpio_set_function(PIN_ESP_UART_TX, GPIO_FUNC_UART_AUX);
    gpio_set_function(PIN_ESP_UART_RX, GPIO_FUNC_UART_AUX);
    uart_set_format(WIFI_UART_INSTANCE, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(WIFI_UART_INSTANCE, false, false);
    uart_set_fifo_enabled(WIFI_UART_INSTANCE, true);

    wifi_hw_rx_drain();
    wifi_f2_state = 0xFFu;
    wifi_reset_fifo();
    wifi_tx_busy_deadline_us = 0u;
    wifi_uart_ready = true;

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
    uint8_t status = 0u;
    uint16_t rx_count = wifi_rx_count_snapshot();
    if (rx_count != 0u) status |= WIFI_STATUS_RX_READY;
    if (rx_count >= WIFI_RX_FIFO_SIZE) status |= WIFI_STATUS_RX_FULL;
    if (wifi_hw_tx_busy() || !uart_is_writable(WIFI_UART_INSTANCE) ||
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
    if (wifi_pop_rx_byte(&data)) return data;

    uint32_t deadline = time_us_32() + WIFI_QUICK_WAIT_US;
    while ((int32_t)(time_us_32() - deadline) < 0)
    {
        wifi_service_rx();
        if (wifi_pop_rx_byte(&data)) return data;
    }

    wifi_rx_underrun = true;
    return 0xFFu;
}

static inline bool __not_in_flash_func(wifi_handle_mem_write)(uint16_t addr, uint8_t data)
{
    if (addr == WIFI_MEM_F2_ADDR) { wifi_f2_state = data; return true; }
    if (addr == WIFI_MEM_CMD_ADDR) { wifi_handle_cmd_write(data); return true; }
    if (addr == WIFI_MEM_DATA_ADDR)
    {
        wifi_uart_init_once();
        uart_putc_raw(WIFI_UART_INSTANCE, (char)data);
        wifi_tx_busy_deadline_us = time_us_32() + wifi_uart_frame_time_us();
        return true;
    }
    return false;
}

static inline uint8_t __not_in_flash_func(wifi_handle_mem_read)(const uint8_t *wifi_rom_base, uint32_t wifi_rom_size, uint16_t addr)
{
    if (addr == WIFI_MEM_F2_ADDR) return wifi_f2_state;
    if (addr == WIFI_MEM_CMD_ADDR) return wifi_data_read();
    if (addr == WIFI_MEM_DATA_ADDR) return wifi_status_read();
    if (addr >= 0x4000u && addr <= 0x7FFFu)
    {
        uint32_t rel = (uint32_t)(addr - 0x4000u);
        if (rel < wifi_rom_size) return wifi_rom_base[rel];
    }
    return 0xFFu;
}

// External PSRAM region used to buffer SD-loaded ROMs. Defined later
// alongside the PSRAM bring-up code; declared here for use by earlier
// functions such as load_rom_from_sd().
static psram_region_t sd_rom_region;
static psram_region_t rom_cache_region;
static psram_region_t mapper_region;
static psram_region_t c2_rom_region;
static psram_region_t megaram_region;
static psram_region_t mp3_buffer_region;
static psram_region_t fh_list_region;
static psram_region_t fh_download_region;
static bool psram_bring_up_once(void);
static bool psram_prepare_mp3_buffer(void);

uint8_t ctr_val = 0xFF;    
BYTE const pdrv = 0;  // Physical drive number
DSTATUS ds = 1; // Disk status (1 = not initialized)

// Structure to represent a ROM record
// The ROM record will contain the name of the ROM, the mapper code, the size of the ROM and the offset in the flash memory
// Name: ROM_NAME_MAX bytes
// Mapper: 1 byte
// Size: 4 bytes
// Offset: 4 bytes
typedef struct {
    char Name[ROM_NAME_MAX];
    unsigned char Mapper;
    unsigned long Size;
    unsigned long Offset;
} ROMRecord;

ROMRecord records[MAX_ROM_RECORDS]; // Array to store the ROM records

#define SD_PATH_MAX        256
#define SD_PATH_BUFFER_SIZE 19000
#define MIN_ROM_SIZE       8192
#define MAX_ROM_SIZE       (15u * 1024u * 1024u)
#define SD_ROM_MAX_SIZE    (4u * 1024u * 1024u) // PSRAM region capacity for SD-loaded ROMs
#define SD_BROWSE_PARTITION_MAX 20u
#define MP3_PSRAM_BUFFER_SIZE 65536u
#define MAPPER_SUNRISE_USB        10
#define MAPPER_SUNRISE_MAPPER_USB 11
#define MAPPER_SYSTEM             MAPPER_SUNRISE_USB
#define MAPPER_SUNRISE_SD         15
#define MAPPER_SUNRISE_MAPPER_SD  16
#define MAPPER_C2_SD              17
#define MAPPER_C2_USB             18
#define MAPPER_MEGARAM_SD         19
#define MAPPER_MEGARAM_USB        20
#define MAPPER_MEGARAM            21

static const char *MAPPER_DESCRIPTIONS[] = {
    "PLA-16", "PLA-32", "KonSCC", "PLN-48", "ASC-08",
    "ASC-16", "Konami", "NEO-8", "NEO-16", "SYSTEM",
    "SYSTEM", "ASC16X", "PLN-64", "MANBW2"
};

#define MAPPER_DESCRIPTION_COUNT (sizeof(MAPPER_DESCRIPTIONS) / sizeof(MAPPER_DESCRIPTIONS[0]))

static char sd_path_buffer[SD_PATH_BUFFER_SIZE];
static uint16_t sd_path_offsets[MAX_ROM_RECORDS];
static uint16_t sd_path_buffer_used = 0;
static uint16_t sd_record_count = 0;
static bool sd_mounted = false;
static uint8_t sd_mounted_partition = 0;
static uint8_t sd_browse_partition = 0;
static bool sd_config_loaded = false;
static sd_card_t *sd_card = NULL;
static uint16_t total_record_count = 0;
static uint16_t full_record_count = 0;
static uint8_t current_page = 0;
static uint16_t filtered_indices[MAX_ROM_RECORDS];
static char filter_query[CTRL_QUERY_SIZE];
static volatile uint8_t ctrl_cmd_state = 0;
static volatile uint8_t ctrl_mapper_value = 0;
static volatile uint8_t ctrl_ack_value = 0;
static uint16_t match_index = 0xFFFF;
static ROMRecord flash_records[MAX_FLASH_RECORDS];
static uint16_t flash_record_count = 0;
static char sd_current_path[SD_PATH_MAX] = "/";
static uint8_t browse_source_mode = SOURCE_MODE_FLASH;
// Last executed entry: source, full microSD path (or flash display name) and the
// folder it lives in, so the menu can reopen on it after a power cycle.
static uint8_t last_selection_source = 0;
static char last_selection_path[SD_PATH_MAX];
static char last_selection_dir[SD_PATH_MAX] = "/";
static bool last_selection_restore_pending = false;
static uint8_t ctrl_status_value = CTRL_MAGIC;
static volatile bool refresh_requested = false;
static volatile bool refresh_in_progress = false;
static bool refresh_worker_started = false;
static volatile bool detect_mapper_pending = false;

// Chunked directory scan state machine (yields back to MP3 loop between chunks)
#define REFRESH_CHUNK_SIZE 8
typedef enum {
    REFRESH_IDLE = 0,
    REFRESH_INIT,
    REFRESH_SCAN_FOLDERS,
    REFRESH_SORT_FOLDERS,
    REFRESH_SCAN_FILES,
    REFRESH_FINALIZE
} refresh_state_t;
static refresh_state_t refresh_state = REFRESH_IDLE;
static DIR refresh_dir;
static uint16_t refresh_record_index = 0;
static uint16_t refresh_folder_count = 0;
static bool refresh_has_parent = false;
static volatile uint16_t detect_mapper_index = 0;
static volatile uint16_t mp3_selected_index = 0;
static volatile bool mp3_pending_select = false;
static volatile uint16_t mp3_pending_index = 0;
static volatile uint8_t mp3_pending_cmd = 0;
static volatile uint8_t mp3_play_mode = MP3_PLAY_MODE_SINGLE;
static uint16_t mp3_playing_filtered_index = 0xFFFF;
static volatile uint8_t ctrl_audio_selection = AUDIO_PROFILE_NONE;
static volatile uint8_t ctrl_wifi_support = 0; // 0=disabled, 1=enabled for Sunrise Nextor launches
static volatile uint8_t ctrl_psg_emulation = 0; // 0=disabled, 1=primary PSG over I2S
static volatile uint8_t ctrl_wavegame_rom = 0; // 0=normal ROM, 1=WAVEGAME sidecars detected
static volatile uint8_t ctrl_sd_partition = 0; // 0=auto/default, 1-4=MBR primary partition
static volatile uint8_t ctrl_audio_volume = AUDIO_VOLUME_DEFAULT; // per-ROM audio volume percent
static volatile uint8_t ctrl_vdp_frequency = VDP_FREQ_DEFAULT; // per-ROM VDP 50/60Hz option (0=default,1=60,2=50)
static volatile uint8_t vdp_freq_launch = VDP_FREQ_DEFAULT; // 50/60Hz to inject into the launched game's INIT (regular game mappers only)
static volatile uint8_t ctrl_cpu_mode = CPU_MODE_DEFAULT; // per-ROM CPU speed option (0=default,1=turbo,2=R800)
static volatile uint8_t cpu_mode_launch = CPU_MODE_DEFAULT; // CPU speed to inject into the launched game's INIT (regular game mappers only)
static volatile uint32_t boot_stub_search_limit = 8192u; // image bytes reachable at 0x4000 when the launched ROM's INIT runs
static uint8_t ctrl_sd_partition_info[CTRL_SD_PARTITION_INFO_SIZE];
static char pico_chip_id[CTRL_CHIP_ID_SIZE] = "0000000000000000";
static bool wavegame_active = false;
static bool wavegame_psg_mirror_active = false;
static char wavegame_dir[SD_PATH_MAX];
static bool wavegame_defer_next = false;
static bool wavegame_deferred_valid = false;
static uint8_t wavegame_deferred_data = 0;

static uint8_t fh_page_index = 0;
static uint8_t fh_status = FH_STATUS_READY;
static uint8_t fh_result = 0;
static uint16_t fh_progress_percent = 0;
static uint8_t fh_query[FH_QUERY_SIZE];
static uint16_t fh_selected_index = 0;
static char fh_wifi_status_text[FH_STATUS_TEXT_SIZE];
static uint8_t fh_tcp_last_error = 0;
static uint32_t fh_download_size = 0;
static char fh_download_name[ROM_NAME_MAX + 1];

#define FH_SAVE_IDLE 0u
#define FH_SAVE_RUNNING 1u
#define FH_SAVE_DONE 2u
#define FH_SAVE_FAILED 3u

static uint8_t fh_sd_save_chunk[1024];
static volatile uint8_t fh_save_state = FH_SAVE_IDLE;
static FIL fh_save_file;
static uint32_t fh_save_offset = 0;
static bool fh_save_file_open = false;
static bool fh_save_close_pending = false;
static FILINFO fh_download_timestamp;
static volatile bool fh_menu_window_active = false;
static volatile bool fh_service_menu_window = false;

typedef struct {
    char name[ROM_NAME_MAX + 1];
    uint16_t size_kb;
} fh_catalog_record_t;

static fh_catalog_record_t fh_catalog[FH_MAX_RESULTS];
static uint16_t fh_catalog_count = 0;
static bool fh_catalog_loaded = false;
static bool fh_catalog_message = false;
static uint8_t fh_tcp_buffer[FH_HTTP_CHUNK_SIZE + 2u];
static char fh_active_query[FH_QUERY_SIZE];

// ESP-01 link monitor -------------------------------------------------------
// The MSX File Hunter screen shows a Network Online/Offline indicator. All of
// the decision making happens on this side so the MSX only reads one byte
// (CTRL_NET_STATUS) instead of interpreting ESP-01 answers by itself.
#define ESP_STATION_CONNECTING   1u        // wifi_station_get_connect_status() values
#define ESP_STATION_GOT_IP       5u        // reported by the ESP-01 UNAPI firmware
#define ESP_PROBE_TIMEOUT_US     250000u   // per attempt; the ESP answers 'g' within a few ms
#define ESP_PROBE_ATTEMPTS       3u        // retries recover from a desynchronized ESP parser
#define ESP_PARSER_RESET_US      300000u   // ESP-01 RX parser drops partial frames after 250 ms
#define ESP_PROBE_QUIET_US       3000u     // UART must be idle before the probe is sent
#define ESP_PROBE_QUIET_MAX_US   50000u    // bound on the pre-probe drain
#define ESP_CONNECTING_WAIT_US   3000000u  // keep polling while the ESP is still joining an AP
#define ESP_CACHE_PRESENT_US     3000000u  // re-probe interval while the module answers
#define ESP_CACHE_ABSENT_US      30000000u // back-off when no module answered at all
#define ESP_TRAFFIC_STICKY_US    10000000u // recent transfer keeps the link reported as online

static volatile uint8_t esp_net_status = NET_STATUS_OFFLINE;
static bool esp_link_probed = false;
static bool esp_link_module_present = false;
static uint32_t esp_link_checked_us = 0;
static bool esp_link_traffic_valid = false;
static uint32_t esp_link_traffic_us = 0;
static char esp_link_ssid[33];

// Stores the outcome of a link check. "answered" means the module replied at
// all (used to pick the re-probe interval), "online" that it is joined to an
// access point. A transfer that completed moments ago outranks a status query
// the module was too busy to answer, which keeps the indicator from flapping.
static void esp_link_store(bool answered, bool online)
{
    if (!online && esp_link_traffic_valid &&
        (uint32_t)(time_us_32() - esp_link_traffic_us) < ESP_TRAFFIC_STICKY_US)
    {
        online = true;
    }
    esp_link_module_present = answered || online;
    esp_net_status = online ? NET_STATUS_ONLINE : NET_STATUS_OFFLINE;
    esp_link_probed = true;
    esp_link_checked_us = time_us_32();
}

// A completed HTTP transfer proves the link far better than any status query.
static void esp_link_note_traffic(void)
{
    esp_link_traffic_valid = true;
    esp_link_traffic_us = time_us_32();
    esp_link_store(true, true);
}

// SCC emulation state + I2S audio
#define SCC_VOLUME_SHIFT 2  // Left-shift SCC output for volume boost (4x)
#define SCC_AUDIO_BUFFER_SAMPLES 256
static SCC scc_instance;
static struct audio_buffer_pool *scc_audio_pool;
static bool scc_audio_started = false;
static struct audio_buffer_pool *rom_audio_handoff_pool;

static PSG psg_instance;
static spin_lock_t *dual_psg_lock = NULL;
static struct audio_buffer_pool *dual_psg_audio_pool;
static bool dual_psg_ready = false;
static bool dual_psg_audio_started = false;
static bool dual_psg_core1_services_io = true;

static PSG main_psg_instance;
static spin_lock_t *main_psg_lock = NULL;
static struct audio_buffer_pool *main_psg_audio_pool;
static bool main_psg_ready = false;
static bool main_psg_audio_started = false;
static bool main_psg_core1_services_io = true;

// Lock-free single-producer/single-consumer ring used when core0 owns the MSX
// I/O write FIFO (Sunrise mapper mode). core0 pushes primary-PSG port writes
// here without taking main_psg_lock so it can never stall and drop the mapper
// segment-register writes (0xFC-0xFF) that share the lossy I/O capture FIFO.
// core1 drains the ring and applies the PSG writes while producing audio.
#define MAIN_PSG_WRITE_RING_SIZE 256u
#define MAIN_PSG_WRITE_RING_MASK (MAIN_PSG_WRITE_RING_SIZE - 1u)
static volatile uint16_t main_psg_write_ring[MAIN_PSG_WRITE_RING_SIZE];
static volatile uint32_t main_psg_write_ring_head; // written by producer (core0)
static volatile uint32_t main_psg_write_ring_tail; // written by consumer (core1)

// Same decoupling for the secondary (Dual PSG) and MSX-MUSIC (YM2413) audio
// register writes: in Sunrise mapper mode core0 must never block on an audio
// spinlock while it owns the lossy MSX I/O write FIFO, otherwise queued mapper
// segment writes (0xFC-0xFF) get dropped. core0 enqueues these chip writes and
// core1 applies them next to audio generation.
#define DUAL_PSG_WRITE_RING_SIZE 256u
#define DUAL_PSG_WRITE_RING_MASK (DUAL_PSG_WRITE_RING_SIZE - 1u)
static volatile uint16_t dual_psg_write_ring[DUAL_PSG_WRITE_RING_SIZE];
static volatile uint32_t dual_psg_write_ring_head; // written by producer (core0)
static volatile uint32_t dual_psg_write_ring_tail; // written by consumer (core1)

#define MSX_MUSIC_WRITE_RING_SIZE 256u
#define MSX_MUSIC_WRITE_RING_MASK (MSX_MUSIC_WRITE_RING_SIZE - 1u)
static volatile uint16_t msx_music_write_ring[MSX_MUSIC_WRITE_RING_SIZE];
static volatile uint32_t msx_music_write_ring_head; // written by producer (core0)
static volatile uint32_t msx_music_write_ring_tail; // written by consumer (core1)

typedef enum {
    SYSTEM_AUDIO_PROFILE_NONE = 0,
    SYSTEM_AUDIO_PROFILE_MAIN_PSG,
    SYSTEM_AUDIO_PROFILE_DUAL_PSG,
    SYSTEM_AUDIO_PROFILE_MSX_MUSIC,
    SYSTEM_AUDIO_PROFILE_YM2151_SFG05,
    SYSTEM_AUDIO_PROFILE_YM2151_SFG01,
    SYSTEM_AUDIO_PROFILE_SCC_EXTERNAL,
    SYSTEM_AUDIO_PROFILE_SCC_PLUS_EXTERNAL,
} system_audio_profile_t;

static system_audio_profile_t system_audio_profile = SYSTEM_AUDIO_PROFILE_NONE;

static OPLL *msx_music_instance = NULL;
static spin_lock_t *msx_music_lock = NULL;
static struct audio_buffer_pool *msx_music_audio_pool;
static bool msx_music_ready = false;
static bool msx_music_audio_started = false;
static bool msx_music_core1_services_io = true;

typedef enum {
    YM2151_SFG05 = 0,
    YM2151_SFG01 = 1,
} ym2151_sfg_variant_t;

static opm_t ym2151_instance;
static spin_lock_t *ym2151_lock = NULL;
static struct audio_buffer_pool *ym2151_audio_pool;
static bool ym2151_ready = false;
static bool ym2151_audio_started = false;
static uint32_t ym2151_clock_hz = YM2151_CLOCK;
static uint32_t ym2151_clock_accum = 0;
static int32_t ym2151_last_output[2] = {0, 0};
static volatile uint8_t ym2151_status_latch = 0;
static uint8_t ym2151_address_latch = 0;
static uint8_t ym2151_write_queue_reg[YM2151_WRITE_QUEUE_SIZE];
static uint8_t ym2151_write_queue_data[YM2151_WRITE_QUEUE_SIZE];
static uint16_t ym2151_write_queue_head = 0;
static uint16_t ym2151_write_queue_tail = 0;
static bool ym2151_write_queue_overflow = false;
static uint8_t ym2151_irq_vector = 0xFFu;
static uint8_t ym2151_midi_irq_vector = 0xFFu;
static uint8_t ym2151_midi_status = 0x03u;

static void msx_music_init(void);
static void msx_music_audio_init(void);
static inline void __not_in_flash_func(msx_music_write_io)(uint8_t port, uint8_t data);
static void i2s_audio_init_scc(void);
static void scc_audio_init_for_type(uint32_t scc_type);
static inline void __not_in_flash_func(scc_audio_service_buffer)(void);
static inline void __not_in_flash_func(msx_music_service_io)(void);
static inline void __not_in_flash_func(msx_music_write_stereo_sample)(int16_t *samples, int index);
static inline void __not_in_flash_func(msx_music_audio_service_buffer)(void);
static void ym2151_init(ym2151_sfg_variant_t variant);
static void ym2151_audio_init(void);
static inline void __not_in_flash_func(ym2151_audio_service_buffer)(bool service_psg_io);
static void start_ym2151_audio_output(void);
static inline bool __not_in_flash_func(pio_try_get_io_write)(uint16_t *addr_out, uint8_t *data_out);
static inline bool __not_in_flash_func(external_scc_read)(uint16_t addr, uint8_t *data);
static inline bool __not_in_flash_func(megaram_scc_audio_selected)(void);
static inline uint32_t __not_in_flash_func(megaram_scc_type_selected)(void);
static inline bool __not_in_flash_func(megaram_scc_write)(uint16_t addr, uint8_t data);
static inline bool __not_in_flash_func(megaram_scc_read)(uint16_t addr, uint8_t *data);
static inline void __not_in_flash_func(sfg_write)(uint16_t addr, uint8_t data);
static inline bool __not_in_flash_func(sfg_read)(const uint8_t *bios_base, uint16_t addr, uint8_t *data);

static struct audio_buffer_pool *claim_rom_audio_handoff_pool(void)
{
    struct audio_buffer_pool *pool = rom_audio_handoff_pool;
    rom_audio_handoff_pool = NULL;
    debug_trace("DBG claim_pool pool=%p", (void *)pool);
    if (pool)
    {
        debug_trace("DBG claim_pool i2s off/on");
        audio_i2s_set_enabled(false);
        audio_i2s_set_enabled(true);
        gpio_put(I2S_MUTE_PIN, 0);
    }
    return pool;
}

typedef enum {
    AUDIO_MODE_NONE = 0,
    AUDIO_MODE_SCC,
    AUDIO_MODE_SCC_PLUS,
    AUDIO_MODE_SCC_EXTERNAL,
    AUDIO_MODE_SCC_PLUS_EXTERNAL,
    AUDIO_MODE_MEGARAM_SCC,
    AUDIO_MODE_MEGARAM_SCC_PLUS,
    AUDIO_MODE_DUAL_PSG,
    AUDIO_MODE_MSX_MUSIC,
    AUDIO_MODE_YM2151_SFG05,
    AUDIO_MODE_YM2151_SFG01,
} audio_mode_t;

static const char *EXCLUDED_SD_FOLDERS[] = {
    "System Volume Information"
};
static const size_t EXCLUDED_SD_FOLDER_COUNT = sizeof(EXCLUDED_SD_FOLDERS) / sizeof(EXCLUDED_SD_FOLDERS[0]);

static void write_u32_le(uint8_t *ptr, uint32_t value);
static void write_u16_le(uint8_t *ptr, uint16_t value);
static void build_page_buffer(uint8_t page_index);
static void refresh_records_for_current_path(void);
static bool sd_mount_card(void);
static void sd_unmount_card(void);
static void cancel_refresh_work(void);
static void process_cycle_sd_partition_request(void);
static void process_delete_sd_file_request(void);
static void process_detect_mapper_request(uint16_t filtered_index);
static void quiesce_mp3_core1_before_sd_work(void);

static bool is_system_mapper(uint8_t mapper) {
    return mapper == MAPPER_SUNRISE_USB ||
           mapper == MAPPER_SUNRISE_MAPPER_USB ||
           mapper == MAPPER_SUNRISE_SD ||
           mapper == MAPPER_SUNRISE_MAPPER_SD ||
           mapper == MAPPER_C2_SD ||
           mapper == MAPPER_C2_USB ||
           mapper == MAPPER_MEGARAM_SD ||
           mapper == MAPPER_MEGARAM_USB ||
           mapper == MAPPER_MEGARAM;
}

static bool is_sunrise_sd_mapper(uint8_t mapper) {
    return mapper == MAPPER_SUNRISE_SD || mapper == MAPPER_SUNRISE_MAPPER_SD ||
           mapper == MAPPER_C2_SD || mapper == MAPPER_MEGARAM_SD;
}

static bool is_megaram_mapper(uint8_t mapper) {
    return mapper == MAPPER_MEGARAM_SD || mapper == MAPPER_MEGARAM_USB || mapper == MAPPER_MEGARAM;
}

static bool is_audio_system_mapper(uint8_t mapper) {
    return mapper == 9u || is_system_mapper(mapper);
}

static bool mapper_supports_scc_audio(uint8_t mapper) {
    return mapper == 3u || mapper == 14u;
}

static audio_mode_t resolve_audio_mode(uint8_t mapper, uint8_t requested_profile) {
    if (is_system_mapper(mapper)) {
        if (is_megaram_mapper(mapper)) {
            if (requested_profile == AUDIO_PROFILE_MEGARAM_SCC_PLUS)
                return AUDIO_MODE_MEGARAM_SCC_PLUS;
            if (requested_profile == AUDIO_PROFILE_MEGARAM_SCC)
                return AUDIO_MODE_MEGARAM_SCC;
            return AUDIO_MODE_NONE;
        }
        if (requested_profile == AUDIO_PROFILE_SCC_PLUS_EXTERNAL)
            return AUDIO_MODE_SCC_PLUS_EXTERNAL;
        if (requested_profile == AUDIO_PROFILE_SCC_EXTERNAL)
            return AUDIO_MODE_SCC_EXTERNAL;
        if (requested_profile == AUDIO_PROFILE_DUAL_PSG)
            return AUDIO_MODE_DUAL_PSG;
        if (requested_profile == AUDIO_PROFILE_MSX_MUSIC &&
            mapper != MAPPER_SUNRISE_MAPPER_USB && mapper != MAPPER_SUNRISE_MAPPER_SD &&
            mapper != MAPPER_C2_SD && mapper != MAPPER_C2_USB &&
            mapper != MAPPER_MEGARAM_SD && mapper != MAPPER_MEGARAM_USB)
            return AUDIO_MODE_MSX_MUSIC; // YM2413/FM-PAC only for non-mapper Sunrise Nextor options
        if (requested_profile == AUDIO_PROFILE_YM2151_SFG05 || requested_profile == AUDIO_PROFILE_YM2151_SFG05_4MHZ)
            return AUDIO_MODE_YM2151_SFG05;
        if (requested_profile == AUDIO_PROFILE_YM2151_SFG01 || requested_profile == AUDIO_PROFILE_YM2151_SFG01_4MHZ)
            return AUDIO_MODE_YM2151_SFG01;
        return AUDIO_MODE_NONE;
    }
    if (requested_profile == AUDIO_PROFILE_SCC_PLUS_EXTERNAL) {
        return AUDIO_MODE_SCC_PLUS_EXTERNAL;
    }
    if (requested_profile == AUDIO_PROFILE_SCC_EXTERNAL) {
        return AUDIO_MODE_SCC_EXTERNAL;
    }
    if (requested_profile == AUDIO_PROFILE_YM2151_SFG05 || requested_profile == AUDIO_PROFILE_YM2151_SFG05_4MHZ) {
        return AUDIO_MODE_YM2151_SFG05;
    }
    if (requested_profile == AUDIO_PROFILE_YM2151_SFG01 || requested_profile == AUDIO_PROFILE_YM2151_SFG01_4MHZ) {
        return AUDIO_MODE_YM2151_SFG01;
    }
    if (is_audio_system_mapper(mapper)) {
        return AUDIO_MODE_NONE;
    }
    if (requested_profile == AUDIO_PROFILE_MSX_MUSIC) {
        return AUDIO_MODE_MSX_MUSIC;
    }
    if (mapper_supports_scc_audio(mapper)) {
        if (requested_profile == AUDIO_PROFILE_SCC_PLUS) {
            return AUDIO_MODE_SCC_PLUS;
        }
        if (requested_profile == AUDIO_PROFILE_SCC) {
            return AUDIO_MODE_SCC;
        }
        return AUDIO_MODE_NONE;
    }
    if (requested_profile == AUDIO_PROFILE_DUAL_PSG) {
        return AUDIO_MODE_DUAL_PSG;
    }
    return AUDIO_MODE_NONE;
}

static uint8_t mapper_code_from_record_byte(uint8_t mapper) {
    return (uint8_t)(mapper & ~(SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG));
}

static int compare_record_names(const ROMRecord *a, const ROMRecord *b) {
    return strncmp(a->Name, b->Name, ROM_NAME_MAX);
}

static bool is_system_record(const ROMRecord *record) {
    return is_system_mapper(mapper_code_from_record_byte(record->Mapper));
}

static bool is_folder_record(const ROMRecord *record) {
    return (record->Mapper & FOLDER_FLAG) != 0;
}

static bool record_matches_source_mode(const ROMRecord *record) {
    switch (browse_source_mode) {
        case SOURCE_MODE_FLASH:
            return !is_folder_record(record) && ((record->Mapper & SOURCE_SD_FLAG) == 0);
        case SOURCE_MODE_SD:
            return is_folder_record(record) || ((record->Mapper & SOURCE_SD_FLAG) != 0);
        default:
            return true;
    }
}

static size_t trim_name_copy(char *dest, const char *src) {
    size_t len = 0;
    for (size_t i = 0; i < ROM_NAME_MAX; i++) {
        dest[i] = src[i];
    }
    dest[ROM_NAME_MAX] = '\0';
    len = ROM_NAME_MAX;
    while (len > 0 && dest[len - 1] == ' ') {
        dest[len - 1] = '\0';
        len--;
    }
    return len;
}

static bool contains_ignore_case(const char *text, const char *query) {
    if (!query || query[0] == '\0') {
        return true;
    }
    size_t text_len = strlen(text);
    size_t query_len = strlen(query);
    if (query_len == 0 || query_len > text_len) {
        return false;
    }
    for (size_t i = 0; i + query_len <= text_len; i++) {
        size_t j = 0;
        while (j < query_len) {
            char a = text[i + j];
            char b = query[j];
            if (toupper((unsigned char)a) != toupper((unsigned char)b)) {
                break;
            }
            j++;
        }
        if (j == query_len) {
            return true;
        }
    }
    return false;
}

static void apply_filter(void) {
    total_record_count = 0;
    if (filter_query[0] == '\0') {
        for (uint16_t i = 0; i < full_record_count; i++) {
            if (record_matches_source_mode(&records[i])) {
                filtered_indices[total_record_count++] = i;
            }
        }
        return;
    }

    char name_buf[ROM_NAME_MAX + 1];
    for (uint16_t i = 0; i < full_record_count; i++) {
        if (!record_matches_source_mode(&records[i])) {
            continue;
        }
        trim_name_copy(name_buf, records[i].Name);
        if (contains_ignore_case(name_buf, filter_query)) {
            filtered_indices[total_record_count++] = i;
        }
    }
}

static void find_first_match(void) {
    match_index = 0xFFFF;
    if (filter_query[0] == '\0') {
        return;
    }

    char name_buf[ROM_NAME_MAX + 1];
    for (uint16_t i = 0; i < total_record_count; i++) {
        uint16_t record_index = filtered_indices[i];
        if (record_index >= full_record_count) {
            continue;
        }
        trim_name_copy(name_buf, records[record_index].Name);
        if (contains_ignore_case(name_buf, filter_query)) {
            match_index = i;
            return;
        }
    }
}

static void swap_records(uint16_t a, uint16_t b) {
    if (a == b) {
        return;
    }
    ROMRecord temp = records[a];
    records[a] = records[b];
    records[b] = temp;
    uint16_t path_temp = sd_path_offsets[a];
    sd_path_offsets[a] = sd_path_offsets[b];
    sd_path_offsets[b] = path_temp;
}

static void sort_records_range(uint16_t start, uint16_t count) {
    if (count < 2) {
        return;
    }
    uint16_t end = (uint16_t)(start + count);
    for (uint16_t i = start; i + 1 < end; i++) {
        for (uint16_t j = i + 1; j < end; j++) {
            if (compare_record_names(&records[i], &records[j]) > 0) {
                swap_records(i, j);
            }
        }
    }
}

static void write_config_area(uint8_t *config_area, const ROMRecord *list, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        uint8_t *entry = config_area + (i * ROM_RECORD_SIZE);
        memset(entry, 0xFF, ROM_RECORD_SIZE);
        memset(entry, ' ', ROM_NAME_MAX);
        size_t name_len = strlen(list[i].Name);
        if (name_len > ROM_NAME_MAX) {
            name_len = ROM_NAME_MAX;
        }
        memcpy(entry, list[i].Name, name_len);
        entry[ROM_NAME_MAX] = list[i].Mapper;
        write_u32_le(entry + ROM_NAME_MAX + 1, (uint32_t)list[i].Size);
        write_u32_le(entry + ROM_NAME_MAX + 1 + sizeof(uint32_t), (uint32_t)list[i].Offset);
    }

    if (count < MAX_ROM_RECORDS) {
        memset(config_area + (count * ROM_RECORD_SIZE), 0xFF,
               (MAX_ROM_RECORDS - count) * ROM_RECORD_SIZE);
    }
}

static void build_page_buffer(uint8_t page_index) {
    uint8_t *buffer = page_buffer;
    const uint16_t buffer_size = DATA_BUFFER_SIZE;
    uint16_t start_index = (uint16_t)page_index * FILES_PER_PAGE;
    uint16_t record_count = 0;

    if (start_index < total_record_count) {
        uint16_t remaining = (uint16_t)(total_record_count - start_index);
        record_count = (remaining > FILES_PER_PAGE) ? FILES_PER_PAGE : remaining;
    }

    memset(buffer, 0xFF, buffer_size);

    uint16_t table_offset = DATA_HEADER_SIZE;
    uint16_t table_size = (uint16_t)(record_count * DATA_RECORD_TABLE_ENTRY_SIZE);
    uint16_t payload_size = (uint16_t)(record_count * DATA_RECORD_PAYLOAD_SIZE);
    uint16_t string_pool_offset = (uint16_t)(table_offset + table_size);
    uint16_t max_string_pool = 0;
    if ((uint32_t)string_pool_offset + payload_size <= buffer_size) {
        max_string_pool = (uint16_t)(buffer_size - (string_pool_offset + payload_size));
    }

    uint16_t name_offsets[FILES_PER_PAGE];
    uint16_t string_cursor = 0;
    uint16_t max_name_bytes = record_count ? (uint16_t)(max_string_pool / record_count) : 0;
    for (uint16_t i = 0; i < record_count; i++) {
        uint16_t filtered_index = (uint16_t)(start_index + i);
        uint16_t record_index = filtered_indices[filtered_index];
        char name_buf[ROM_NAME_MAX + 1];
        size_t name_len = trim_name_copy(name_buf, records[record_index].Name);
        if (max_name_bytes != 0u && name_len + 1u > max_name_bytes) {
            name_len = max_name_bytes - 1u;
            name_buf[name_len] = '\0';
        }
        if (name_len + 1 <= (size_t)(max_string_pool - string_cursor)) {
            name_offsets[i] = string_cursor;
            memcpy(buffer + string_pool_offset + string_cursor, name_buf, name_len);
            buffer[string_pool_offset + string_cursor + name_len] = '\0';
            string_cursor = (uint16_t)(string_cursor + name_len + 1);
        } else {
            name_offsets[i] = 0xFFFF;
        }
    }

    uint16_t string_pool_size = string_cursor;
    uint16_t payload_offset = (uint16_t)(string_pool_offset + string_pool_size);
    uint16_t payload_cursor = 0;

    for (uint16_t i = 0; i < record_count; i++) {
        uint16_t filtered_index = (uint16_t)(start_index + i);
        uint16_t record_index = filtered_indices[filtered_index];
        uint8_t *entry = buffer + table_offset + (i * DATA_RECORD_TABLE_ENTRY_SIZE);
        uint16_t record_offset = (uint16_t)(payload_offset + payload_cursor);
        uint16_t record_length = DATA_RECORD_PAYLOAD_SIZE;

        write_u16_le(entry, record_offset);
        write_u16_le(entry + 2, record_length);

        uint8_t *rec = buffer + record_offset;
        rec[0] = TLV_NAME_OFFSET;
        rec[1] = 2;
        write_u16_le(rec + 2, name_offsets[i]);
        rec[4] = TLV_MAPPER;
        rec[5] = 1;
        rec[6] = records[record_index].Mapper;
        rec[7] = TLV_SIZE;
        rec[8] = 4;
        write_u32_le(rec + 9, (uint32_t)records[record_index].Size);

        payload_cursor = (uint16_t)(payload_cursor + record_length);
    }

    buffer[0] = DATA_MAGIC_0;
    buffer[1] = DATA_MAGIC_1;
    buffer[2] = DATA_MAGIC_2;
    buffer[3] = DATA_MAGIC_3;
    buffer[4] = DATA_VERSION;
    buffer[5] = 0;
    write_u16_le(buffer + 6, DATA_HEADER_SIZE);
    write_u16_le(buffer + 8, total_record_count);
    write_u16_le(buffer + 10, page_index);
    write_u16_le(buffer + 12, record_count);
    write_u16_le(buffer + 14, table_offset);
    write_u16_le(buffer + 16, table_size);
    write_u16_le(buffer + 18, string_pool_offset);
    write_u16_le(buffer + 20, string_pool_size);
    write_u16_le(buffer + 22, payload_offset);
}

static bool equals_ignore_case(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool is_excluded_folder(const char *name) {
    if (!name || name[0] == '\0') {
        return true;
    }
    if (equals_ignore_case(name, ".") || equals_ignore_case(name, "..")) {
        return true;
    }
    for (size_t i = 0; i < EXCLUDED_SD_FOLDER_COUNT; i++) {
        if (equals_ignore_case(name, EXCLUDED_SD_FOLDERS[i])) {
            return true;
        }
    }
    return false;
}

static bool is_root_path(const char *path) {
    if (!path || path[0] == '\0') {
        return true;
    }
    if (path[0] == '/' && path[1] == '\0') {
        return true;
    }
    return false;
}

static void set_root_path(void) {
    sd_current_path[0] = '/';
    sd_current_path[1] = '\0';
}

static void go_up_one_level(void) {
    if (is_root_path(sd_current_path)) {
        set_root_path();
        return;
    }

    size_t len = strlen(sd_current_path);
    if (len == 0) {
        set_root_path();
        return;
    }

    while (len > 1 && sd_current_path[len - 1] == '/') {
        sd_current_path[len - 1] = '\0';
        len--;
    }

    char *slash = strrchr(sd_current_path, '/');
    if (!slash || slash == sd_current_path) {
        set_root_path();
    } else {
        *slash = '\0';
    }
}

static void append_folder_to_path(const char *folder) {
    if (!folder || folder[0] == '\0') {
        return;
    }
    if (equals_ignore_case(folder, "..")) {
        go_up_one_level();
        return;
    }

    size_t base_len = strlen(sd_current_path);
    size_t folder_len = strlen(folder);
    if (base_len + folder_len + 2 >= sizeof(sd_current_path)) {
        return;
    }

    if (!is_root_path(sd_current_path)) {
        sd_current_path[base_len] = '/';
        base_len++;
    }
    memcpy(sd_current_path + base_len, folder, folder_len + 1);
}

static void write_u32_le(uint8_t *ptr, uint32_t value) {
    ptr[0] = (uint8_t)(value & 0xFFu);
    ptr[1] = (uint8_t)((value >> 8) & 0xFFu);
    ptr[2] = (uint8_t)((value >> 16) & 0xFFu);
    ptr[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void write_u16_le(uint8_t *ptr, uint16_t value) {
    ptr[0] = (uint8_t)(value & 0xFFu);
    ptr[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static const char *basename_from_path(const char *path) {
    if (!path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

static uint8_t mapper_number_from_filename(const char *filename) {
    char name_copy[SD_PATH_MAX];
    strncpy(name_copy, filename, sizeof(name_copy));
    name_copy[sizeof(name_copy) - 1] = '\0';

    char *dot = strrchr(name_copy, '.');
    if (dot) {
        *dot = '\0';
    }

    size_t name_len = strlen(name_copy);
    for (size_t i = 0; i + 1 < MAPPER_DESCRIPTION_COUNT; ++i) {
        const char *tag = MAPPER_DESCRIPTIONS[i];
        size_t tag_len = strlen(tag);
        if (name_len > tag_len + 1 && name_copy[name_len - tag_len - 1] == '.') {
            if (equals_ignore_case(name_copy + name_len - tag_len, tag)) {
                return (uint8_t)(i + 1);
            }
        }
    }
    return 0;
}

static void build_display_name(const char *filename, char *out, size_t out_size) {
    char name_copy[SD_PATH_MAX];
    strncpy(name_copy, filename, sizeof(name_copy));
    name_copy[sizeof(name_copy) - 1] = '\0';

    char *dot = strrchr(name_copy, '.');
    if (dot) {
        *dot = '\0';
    }

    size_t name_len = strlen(name_copy);
    for (size_t i = 0; i + 1 < MAPPER_DESCRIPTION_COUNT; ++i) {
        const char *tag = MAPPER_DESCRIPTIONS[i];
        size_t tag_len = strlen(tag);
        if (name_len > tag_len + 1 && name_copy[name_len - tag_len - 1] == '.') {
            if (equals_ignore_case(name_copy + name_len - tag_len, tag)) {
                name_copy[name_len - tag_len - 1] = '\0';
                break;
            }
        }
    }

    strncpy(out, name_copy, out_size);
    out[out_size - 1] = '\0';
}

static bool has_rom_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot[1] == '\0') {
        return false;
    }
    return equals_ignore_case(dot + 1, "ROM");
}

static bool has_mp3_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot[1] == '\0') {
        return false;
    }
    return equals_ignore_case(dot + 1, "MP3");
}

static bool has_wav_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot[1] == '\0') {
        return false;
    }
    return equals_ignore_case(dot + 1, "WAV");
}

static bool build_pvc_options_path(uint16_t record_index, char *out, size_t out_size) {
    if (!out || out_size == 0 || record_index >= full_record_count) {
        return false;
    }
    ROMRecord *rec = &records[record_index];
    if ((rec->Mapper & (FOLDER_FLAG | MP3_FLAG)) != 0) {
        return false;
    }

    if (mapper_code_from_record_byte(rec->Mapper) == MAPPER_MEGARAM) {
        int written = snprintf(out, out_size, "/%s.flash.PVC", rec->Name);
        return written > 0 && (size_t)written < out_size;
    }

    if ((rec->Mapper & SOURCE_SD_FLAG) != 0) {
        if (sd_path_offsets[record_index] == 0xFFFF) {
            return false;
        }
        const char *rom_path = sd_path_buffer + sd_path_offsets[record_index];
        size_t len = strlen(rom_path);
        if (len + 1 > out_size) {
            return false;
        }
        memcpy(out, rom_path, len + 1);
        char *slash = strrchr(out, '/');
        char *dot = strrchr(out, '.');
        if (!dot || (slash && dot < slash)) {
            dot = out + len;
        }
        if ((size_t)(dot - out) + 5u > out_size) {
            return false;
        }
        memcpy(dot, ".PVC", 5u);
        return true;
    }

    int written = snprintf(out, out_size, "/%s.flash.PVC", rec->Name);
    return written > 0 && (size_t)written < out_size;
}

static uint16_t query_filtered_index(void) {
    return (uint16_t)((uint8_t)filter_query[0]) |
           (uint16_t)(((uint8_t)filter_query[1]) << 8);
}

static void process_enter_dir_request(void) {
    filter_query[CTRL_QUERY_SIZE - 1] = '\0';
    if ((uint8_t)filter_query[2] == CTRL_MAGIC) {
        uint16_t filtered_index = query_filtered_index();
        if (filtered_index < total_record_count) {
            uint16_t record_index = filtered_indices[filtered_index];
            if (record_index < full_record_count && (records[record_index].Mapper & FOLDER_FLAG)) {
                append_folder_to_path(records[record_index].Name);
            }
        }
    } else if (filter_query[0] == '\0') {
        go_up_one_level();
    } else {
        append_folder_to_path(filter_query);
    }
    refresh_requested = true;
}

static void cancel_refresh_work(void) {
    if (refresh_state == REFRESH_SCAN_FOLDERS || refresh_state == REFRESH_SCAN_FILES) {
        f_closedir(&refresh_dir);
    }
    refresh_state = REFRESH_IDLE;
    refresh_in_progress = false;
    refresh_requested = false;
}

static bool wavegame_assets_detected_for_record(uint16_t record_index);

static void set_sunrise_partition_text(const char *text) {
    size_t len = text ? strlen(text) : 0;
    if (len > CTRL_SD_PARTITION_INFO_SIZE - 3u) {
        len = CTRL_SD_PARTITION_INFO_SIZE - 3u;
    }
    memset(ctrl_sd_partition_info + 2, 0, CTRL_SD_PARTITION_INFO_SIZE - 2u);
    if (len) memcpy(ctrl_sd_partition_info + 2, text, len);
}

static void update_sunrise_partition_text(const sunrise_sd_partition_t *parts, uint8_t part_count) {
    if (part_count == 0) {
        set_sunrise_partition_text("NO FAT16 PARTITION AVAILABLE");
        return;
    }
    for (uint8_t i = 0; i < part_count; i++) {
        if (parts[i].number == ctrl_sd_partition) {
            if (parts[i].label[0]) {
                set_sunrise_partition_text(parts[i].label);
            } else {
                char fallback[16];
                snprintf(fallback, sizeof(fallback), "PARTITION%u", parts[i].number);
                set_sunrise_partition_text(fallback);
            }
            return;
        }
    }
    set_sunrise_partition_text("PARTITION");
}

static bool read_mounted_partition_free_mb(uint8_t selected_partition, uint32_t *free_mb) {
    if (!free_mb || !sd_mounted || sd_mounted_partition != selected_partition) {
        return false;
    }

    DWORD free_clusters = 0;
    FATFS *fatfs = NULL;
    if (f_getfree("", &free_clusters, &fatfs) != FR_OK || !fatfs || fatfs->csize == 0) {
        return false;
    }

    uint64_t mb = ((uint64_t)free_clusters * (uint64_t)fatfs->csize) / 2048u;
    *free_mb = (mb > UINT32_MAX) ? UINT32_MAX : (uint32_t)mb;
    return true;
}

static void set_browse_partition_status_text(const char *label, uint8_t selected_partition) {
    uint32_t free_mb = 0;
    if (!read_mounted_partition_free_mb(selected_partition, &free_mb)) {
        set_sunrise_partition_text(label);
        return;
    }

    char suffix[24];
    char text[CTRL_SD_PARTITION_INFO_SIZE - 2u];
    snprintf(suffix, sizeof(suffix), " (%luMB FREE)", (unsigned long)free_mb);

    size_t max_len = CTRL_SD_PARTITION_INFO_SIZE - 3u;
    size_t label_len = strlen(label);
    size_t suffix_len = strlen(suffix);
    if (suffix_len < max_len && label_len > max_len - suffix_len) {
        label_len = max_len - suffix_len;
    }

    memcpy(text, label, label_len);
    memcpy(text + label_len, suffix, suffix_len + 1u);
    set_sunrise_partition_text(text);
}

static void update_browse_partition_text(const sunrise_sd_partition_t *parts, uint8_t part_count, uint8_t selected_partition) {
    if (part_count == 0) {
        set_sunrise_partition_text("");
        return;
    }
    for (uint8_t i = 0; i < part_count; i++) {
        if (parts[i].number == selected_partition) {
            if (parts[i].label[0]) {
                set_browse_partition_status_text(parts[i].label, selected_partition);
            } else {
                char fallback[16];
                snprintf(fallback, sizeof(fallback), "PARTITION%u", parts[i].number);
                set_browse_partition_status_text(fallback, selected_partition);
            }
            return;
        }
    }
    set_sunrise_partition_text("PARTITION");
}

static void refresh_browse_partition_text(void) {
    sunrise_sd_partition_t parts[SD_BROWSE_PARTITION_MAX];
    uint8_t count = sunrise_sd_list_supported_partitions(parts, SD_BROWSE_PARTITION_MAX);
    if (count) {
        update_browse_partition_text(parts, count, sd_browse_partition ? sd_browse_partition : sd_mounted_partition);
    }
}

static void refresh_sunrise_partition_info(uint8_t selected_partition) {
    memset(ctrl_sd_partition_info, 0, sizeof(ctrl_sd_partition_info));
    ctrl_sd_partition = 0;

    sunrise_sd_partition_t parts[4];
    uint8_t part_count = sunrise_sd_list_fat16_partitions(parts, 4);
    ctrl_sd_partition_info[0] = part_count;

    for (uint8_t i = 0; i < part_count; i++) {
        ctrl_sd_partition_info[1] |= (uint8_t)(1u << (parts[i].number - 1u));
        if (selected_partition == parts[i].number) {
            ctrl_sd_partition = selected_partition;
        }
    }

    if (ctrl_sd_partition == 0 && part_count != 0) {
        ctrl_sd_partition = parts[0].number;
    }
    update_sunrise_partition_text(parts, part_count);
}

static void process_load_options_request(void) {
    ctrl_ack_value = 0;
    ctrl_mapper_value = 0;
    ctrl_wavegame_rom = 0;
    ctrl_audio_selection = AUDIO_PROFILE_NONE;
    ctrl_psg_emulation = 1;
    ctrl_sd_partition = 0;
    ctrl_audio_volume = AUDIO_VOLUME_DEFAULT;
    ctrl_vdp_frequency = VDP_FREQ_DEFAULT;
    ctrl_cpu_mode = CPU_MODE_DEFAULT;
    memset(ctrl_sd_partition_info, 0, sizeof(ctrl_sd_partition_info));
    quiesce_mp3_core1_before_sd_work();
    if (!sd_mount_card()) {
        return;
    }

    uint16_t index = query_filtered_index();
    if (index >= total_record_count) {
        return;
    }
    uint16_t record_index = filtered_indices[index];
    ROMRecord *rec = &records[record_index];
    bool sunrise_sd_options = is_sunrise_sd_mapper(mapper_code_from_record_byte(rec->Mapper));
    ctrl_wavegame_rom = wavegame_assets_detected_for_record(record_index) ? 1u : 0u;
    if (sunrise_sd_options) {
        refresh_sunrise_partition_info(0);
    }
    char path[SD_PATH_MAX];
    if (!build_pvc_options_path(record_index, path, sizeof(path))) {
        if (sunrise_sd_options) {
            ctrl_ack_value = 1;
        }
        return;
    }

    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) {
        if (sunrise_sd_options) {
            ctrl_ack_value = 1;
        }
        return;
    }
    uint8_t data[PVC_OPTIONS_SIZE];
    UINT br = 0;
    FRESULT fr = f_read(&fil, data, sizeof(data), &br);
    f_close(&fil);
    if (fr != FR_OK || br < PVC_OPTIONS_LEGACY_SIZE) {
        return;
    }
    if (data[0] != PVC_OPTIONS_MAGIC_0 || data[1] != PVC_OPTIONS_MAGIC_1 ||
        data[2] != PVC_OPTIONS_MAGIC_2 || data[3] != PVC_OPTIONS_MAGIC_3) {
        return;
    }

    ctrl_audio_selection = data[4];
    ctrl_psg_emulation = data[5] ? 1u : 0u;
    if (ctrl_wavegame_rom) {
        ctrl_audio_selection = AUDIO_PROFILE_NONE;
    }
    if (br >= PVC_OPTIONS_MAPPER_SIZE && data[6] != 0 && !is_system_mapper(data[6]) && data[6] < MAPPER_DESCRIPTION_COUNT) {
        uint8_t flags = rec->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG);
        if ((flags & (FOLDER_FLAG | MP3_FLAG)) == 0) {
            rec->Mapper = (uint8_t)(flags | data[6]);
            ctrl_mapper_value = data[6];
        }
    }
    if (sunrise_sd_options && br >= PVC_OPTIONS_PARTITION_SIZE) {
        refresh_sunrise_partition_info(data[7]);
    }
    if (br >= PVC_OPTIONS_VOLUME_SIZE) {
        ctrl_audio_volume = data[8] <= AUDIO_VOLUME_MAX ? data[8] : AUDIO_VOLUME_DEFAULT;
    }
    if (br >= PVC_OPTIONS_FREQ_SIZE) {
        ctrl_vdp_frequency = data[9] <= VDP_FREQ_50HZ ? data[9] : VDP_FREQ_DEFAULT;
    }
    if (br >= PVC_OPTIONS_SIZE) {
        ctrl_cpu_mode = data[10] <= CPU_MODE_R800 ? data[10] : CPU_MODE_DEFAULT;
    }
    ctrl_ack_value = 1;
}

static void process_save_options_request(void) {
    ctrl_ack_value = 0;
    quiesce_mp3_core1_before_sd_work();
    if (!sd_mount_card()) {
        return;
    }

    uint16_t index = query_filtered_index();
    if (index >= total_record_count) {
        return;
    }
    uint16_t record_index = filtered_indices[index];
    uint8_t audio_selection = wavegame_assets_detected_for_record(record_index) ? AUDIO_PROFILE_NONE : (uint8_t)filter_query[2];
    uint8_t sd_partition = (uint8_t)filter_query[5];
    uint8_t audio_volume = (uint8_t)filter_query[6];
    if (audio_volume > AUDIO_VOLUME_MAX) {
        audio_volume = AUDIO_VOLUME_DEFAULT;
    }
    uint8_t vdp_frequency = (uint8_t)filter_query[7];
    if (vdp_frequency > VDP_FREQ_50HZ) {
        vdp_frequency = VDP_FREQ_DEFAULT;
    }
    uint8_t cpu_mode = (uint8_t)filter_query[8];
    if (cpu_mode > CPU_MODE_R800) {
        cpu_mode = CPU_MODE_DEFAULT;
    }
    char path[SD_PATH_MAX];
    if (!build_pvc_options_path(record_index, path, sizeof(path))) {
        return;
    }

    FIL fil;
    if (f_open(&fil, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        return;
    }
    uint8_t data[PVC_OPTIONS_SIZE] = {
        PVC_OPTIONS_MAGIC_0, PVC_OPTIONS_MAGIC_1, PVC_OPTIONS_MAGIC_2, PVC_OPTIONS_MAGIC_3,
        audio_selection, (uint8_t)(filter_query[3] ? 1u : 0u), (uint8_t)filter_query[4], sd_partition,
        audio_volume, vdp_frequency, cpu_mode
    };
    UINT written = 0;
    FRESULT fr = f_write(&fil, data, sizeof(data), &written);
    FRESULT close_fr = f_close(&fil);
    if (fr == FR_OK && close_fr == FR_OK && written == sizeof(data)) {
        ctrl_audio_selection = data[4];
        ctrl_psg_emulation = data[5];
        ctrl_mapper_value = data[6];
        ctrl_sd_partition = data[7];
        ctrl_audio_volume = data[8];
        ctrl_vdp_frequency = data[9];
        ctrl_cpu_mode = data[10];
        ctrl_ack_value = 1;
    }
}

// --- Last executed entry -----------------------------------------------------
// The menu asks the Pico to store the Flash/microSD entry it is about to run so
// the next boot can reopen the same source, folder and cursor position. The
// state lives in a small file on the root of the browse partition.

static void split_last_selection_dir(void) {
    last_selection_dir[0] = '/';
    last_selection_dir[1] = '\0';
    if (last_selection_source != SOURCE_MODE_SD) {
        return;
    }
    const char *slash = strrchr(last_selection_path, '/');
    if (!slash || slash == last_selection_path) {
        return;
    }
    size_t len = (size_t)(slash - last_selection_path);
    memcpy(last_selection_dir, last_selection_path, len);
    last_selection_dir[len] = '\0';
}

static void process_save_last_selection_request(void) {
    ctrl_ack_value = 0;

    uint16_t index = query_filtered_index();
    if (index >= total_record_count) {
        return;
    }
    uint16_t record_index = filtered_indices[index];
    if (record_index >= full_record_count) {
        return;
    }

    ROMRecord const *rec = &records[record_index];
    if ((rec->Mapper & FOLDER_FLAG) != 0) {
        return;
    }

    char path[SD_PATH_MAX];
    uint8_t source;
    if ((rec->Mapper & SOURCE_SD_FLAG) != 0) {
        if (sd_path_offsets[record_index] == 0xFFFF) {
            return;
        }
        source = SOURCE_MODE_SD;
        const char *rom_path = sd_path_buffer + sd_path_offsets[record_index];
        if (strlen(rom_path) >= sizeof(path)) {
            return;
        }
        strcpy(path, rom_path);
    } else {
        source = SOURCE_MODE_FLASH;
        trim_name_copy(path, rec->Name);
    }

    if (source == last_selection_source && strcmp(path, last_selection_path) == 0) {
        ctrl_ack_value = 1; // already stored, keep the card untouched
        return;
    }

    quiesce_mp3_core1_before_sd_work();
    if (!sd_mount_card()) {
        return;
    }

    FIL fil;
    if (f_open(&fil, PV_LAST_PATH, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        return;
    }
    uint8_t header[PV_LAST_HEADER_SIZE] = {
        PV_LAST_MAGIC_0, PV_LAST_MAGIC_1, PV_LAST_MAGIC_2, PV_LAST_MAGIC_3,
        PV_LAST_VERSION, source, 0, 0
    };
    UINT path_len = (UINT)(strlen(path) + 1u);
    UINT written = 0;
    bool ok = f_write(&fil, header, sizeof(header), &written) == FR_OK && written == sizeof(header);
    if (ok) {
        ok = f_write(&fil, path, path_len, &written) == FR_OK && written == path_len;
    }
    if (f_close(&fil) != FR_OK || !ok) {
        return;
    }

    last_selection_source = source;
    strcpy(last_selection_path, path);
    split_last_selection_dir();
    ctrl_ack_value = 1;
}

static void process_load_last_selection_request(void) {
    ctrl_ack_value = 0;
    ctrl_mapper_value = 0;
    last_selection_source = 0;
    last_selection_path[0] = '\0';
    last_selection_dir[0] = '/';
    last_selection_dir[1] = '\0';
    last_selection_restore_pending = false;

    quiesce_mp3_core1_before_sd_work();
    if (!sd_mount_card()) {
        return;
    }

    FIL fil;
    if (f_open(&fil, PV_LAST_PATH, FA_READ) != FR_OK) {
        return;
    }
    uint8_t header[PV_LAST_HEADER_SIZE];
    char path[SD_PATH_MAX];
    UINT br = 0;
    memset(path, 0, sizeof(path));
    bool ok = f_read(&fil, header, sizeof(header), &br) == FR_OK && br == sizeof(header);
    if (ok) {
        ok = f_read(&fil, path, sizeof(path) - 1u, &br) == FR_OK && br >= 2u;
    }
    f_close(&fil);
    if (!ok) {
        return;
    }
    if (header[0] != PV_LAST_MAGIC_0 || header[1] != PV_LAST_MAGIC_1 ||
        header[2] != PV_LAST_MAGIC_2 || header[3] != PV_LAST_MAGIC_3 ||
        header[4] != PV_LAST_VERSION) {
        return;
    }

    uint8_t source = header[5];
    if (source != SOURCE_MODE_FLASH && source != SOURCE_MODE_SD) {
        return;
    }
    if (source == SOURCE_MODE_SD) {
        FILINFO fno;
        if (f_stat(path, &fno) != FR_OK) {
            return; // file was deleted or the card changed: fall back to flash
        }
    }

    last_selection_source = source;
    strcpy(last_selection_path, path);
    split_last_selection_dir();
    last_selection_restore_pending = true;
    ctrl_mapper_value = source;
    ctrl_ack_value = 1;
}

// Point match_index at the last executed entry once the list it belongs to has
// been rebuilt, so the menu can place the cursor on it. ctrl_ack_value is set to
// CTRL_MAGIC to tell the menu the restore is resolved.
static void resolve_last_selection_match(void) {
    if (!last_selection_restore_pending) {
        return;
    }
    last_selection_restore_pending = false;
    match_index = 0xFFFF;

    char name_buf[ROM_NAME_MAX + 1];
    for (uint16_t i = 0; i < total_record_count; i++) {
        uint16_t record_index = filtered_indices[i];
        if (record_index >= full_record_count) {
            continue;
        }
        ROMRecord const *rec = &records[record_index];
        if ((rec->Mapper & FOLDER_FLAG) != 0) {
            continue;
        }
        if (last_selection_source == SOURCE_MODE_SD) {
            if ((rec->Mapper & SOURCE_SD_FLAG) == 0 || sd_path_offsets[record_index] == 0xFFFF) {
                continue;
            }
            if (equals_ignore_case(sd_path_buffer + sd_path_offsets[record_index], last_selection_path)) {
                match_index = i;
                break;
            }
        } else if ((rec->Mapper & SOURCE_SD_FLAG) == 0) {
            trim_name_copy(name_buf, rec->Name);
            if (strcmp(name_buf, last_selection_path) == 0) {
                match_index = i;
                break;
            }
        }
    }
    ctrl_ack_value = CTRL_MAGIC;
}

static void process_prepare_quick_run_request(void) {
    uint16_t index = query_filtered_index();
    ctrl_ack_value = 0;
    ctrl_mapper_value = 0;
    ctrl_audio_selection = AUDIO_PROFILE_NONE;
    ctrl_psg_emulation = 1;
    ctrl_wifi_support = 0;
    ctrl_wavegame_rom = 0;
    ctrl_audio_volume = AUDIO_VOLUME_DEFAULT;
    ctrl_vdp_frequency = VDP_FREQ_DEFAULT;
    ctrl_cpu_mode = CPU_MODE_DEFAULT;

    if (index >= total_record_count) {
        return;
    }

    uint16_t record_index = filtered_indices[index];
    if (record_index >= full_record_count) {
        return;
    }

    ROMRecord *rec = &records[record_index];
    uint8_t mapper = mapper_code_from_record_byte(rec->Mapper);
    if (mapper_supports_scc_audio(mapper)) {
        ctrl_audio_selection = AUDIO_PROFILE_SCC;
    }

    process_load_options_request();
    bool options_loaded = (ctrl_ack_value != 0);

    mapper = mapper_code_from_record_byte(rec->Mapper);
    if (mapper == 0) {
        process_detect_mapper_request(index);
        mapper = ctrl_mapper_value;
    } else {
        ctrl_mapper_value = mapper;
    }

    if (!options_loaded) {
        ctrl_audio_selection = mapper_supports_scc_audio(mapper) ? AUDIO_PROFILE_SCC : AUDIO_PROFILE_NONE;
        ctrl_psg_emulation = 1;
    }
    ctrl_wifi_support = 0;
    ctrl_ack_value = 1;
}

// FatFS-backed reader callback for the shared streaming detector.
typedef struct {
    FIL *fil;
    uint32_t cur_offset; // current file pointer (best-effort cache to avoid redundant f_lseek)
    bool failed;
} fatfs_reader_ctx_t;

static uint32_t fatfs_reader_cb(void *user, uint32_t offset, void *buf, uint32_t len) {
    fatfs_reader_ctx_t *ctx = (fatfs_reader_ctx_t *)user;
    if (ctx->failed || !ctx->fil) {
        return 0;
    }
    if (offset != ctx->cur_offset) {
        if (f_lseek(ctx->fil, offset) != FR_OK) {
            ctx->failed = true;
            return 0;
        }
        ctx->cur_offset = offset;
    }
    UINT br = 0;
    if (f_read(ctx->fil, buf, (UINT)len, &br) != FR_OK) {
        ctx->failed = true;
        return 0;
    }
    ctx->cur_offset += br;
    return (uint32_t)br;
}

// Stream a ROM file from the SD card through the shared mapper detector.
// Uses the same SHA1 + heuristic algorithm as the PC tool so that the firmware
// produces identical mapper guesses for ROMs executed straight from microSD.
static uint8_t detect_rom_type_from_file(const char *path, uint32_t size) {
    if (size > MAX_ROM_SIZE || size < MIN_ROM_SIZE) {
        return 0;
    }

    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        return 0;
    }

    fatfs_reader_ctx_t ctx = { .fil = &fil, .cur_offset = 0, .failed = false };
    uint8_t mapper = mapper_detect_stream(size, fatfs_reader_cb, &ctx);
    f_close(&fil);
    if (ctx.failed) {
        return 0;
    }
    return mapper;
}

static bool sd_prepare_card(void) {
    if (!sd_init_driver()) return false;
    sd_card = sd_get_by_num(0);
    if (!sd_card) return false;
    return disk_initialize(pdrv) == 0;
}

static bool sd_partition_supported(const sunrise_sd_partition_t *parts, uint8_t count, uint8_t number) {
    for (uint8_t i = 0; i < count; i++) {
        if (parts[i].number == number) return true;
    }
    return false;
}

static int sd_find_partition_index(const sunrise_sd_partition_t *parts, uint8_t count, uint8_t number) {
    for (uint8_t i = 0; i < count; i++) {
        if (parts[i].number == number) return (int)i;
    }
    return -1;
}

static void sd_unmount_card(void) {
    if (sd_mounted) {
        f_mount(NULL, "", 0);
    }
    sd_mounted = false;
    sd_mounted_partition = 0;
    disk_set_partition_window(0, 0);
}

static bool sd_mount_partition(const sunrise_sd_partition_t *parts, uint8_t count, uint8_t number) {
    int index = sd_find_partition_index(parts, count, number);
    if (index < 0) return false;

    sd_unmount_card();
    disk_set_partition_window((LBA_t)parts[index].start_lba, (LBA_t)parts[index].sector_count);
    FRESULT fr = f_mount(&sd_card->state.fatfs, "", 1);
    if (fr != FR_OK) {
        sd_unmount_card();
        return false;
    }
    sd_mounted = true;
    sd_mounted_partition = parts[index].number;
    return true;
}

static uint8_t sd_load_saved_browse_partition(const sunrise_sd_partition_t *parts, uint8_t count) {
    uint8_t selected = count ? parts[0].number : 0;
    if (!count || !sd_mount_partition(parts, count, parts[0].number)) return selected;

    FIL fil;
    if (f_open(&fil, PV_CONFIG_PATH, FA_READ) == FR_OK) {
        uint8_t data[PV_CONFIG_SIZE];
        UINT br = 0;
        if (f_read(&fil, data, sizeof(data), &br) == FR_OK && br == sizeof(data) &&
            data[0] == PV_CONFIG_MAGIC_0 && data[1] == PV_CONFIG_MAGIC_1 &&
            data[2] == PV_CONFIG_MAGIC_2 && data[3] == PV_CONFIG_MAGIC_3 &&
            sd_partition_supported(parts, count, data[4])) {
            selected = data[4];
        }
        f_close(&fil);
    }
    sd_unmount_card();
    return selected;
}

static void sd_save_browse_partition(uint8_t selected) {
    sunrise_sd_partition_t parts[SD_BROWSE_PARTITION_MAX];
    uint8_t count = sunrise_sd_list_supported_partitions(parts, SD_BROWSE_PARTITION_MAX);
    if (!count || !sd_mount_partition(parts, count, parts[0].number)) return;

    FIL fil;
    if (f_open(&fil, PV_CONFIG_PATH, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        uint8_t data[PV_CONFIG_SIZE] = {
            PV_CONFIG_MAGIC_0, PV_CONFIG_MAGIC_1, PV_CONFIG_MAGIC_2, PV_CONFIG_MAGIC_3, selected
        };
        UINT written = 0;
        (void)f_write(&fil, data, sizeof(data), &written);
        f_close(&fil);
    }
    sd_unmount_card();
}

static bool sd_mount_card(void) {
    if (sd_mounted) {
        refresh_browse_partition_text();
        return true;
    }
    if (!sd_prepare_card()) return false;

    sunrise_sd_partition_t parts[SD_BROWSE_PARTITION_MAX];
    uint8_t count = sunrise_sd_list_supported_partitions(parts, SD_BROWSE_PARTITION_MAX);
    if (!count) return false;

    if (!sd_config_loaded) {
        sd_browse_partition = sd_load_saved_browse_partition(parts, count);
        sd_config_loaded = true;
    }
    if (!sd_partition_supported(parts, count, sd_browse_partition)) {
        sd_browse_partition = parts[0].number;
    }
    if (!sd_mount_partition(parts, count, sd_browse_partition)) return false;
    sd_browse_partition = sd_mounted_partition;
    update_browse_partition_text(parts, count, sd_browse_partition);
    return true;
}

static void process_cycle_sd_partition_request(void) {
    ctrl_ack_value = 0;
    cancel_refresh_work();
    if (!sd_prepare_card()) return;

    sunrise_sd_partition_t parts[SD_BROWSE_PARTITION_MAX];
    uint8_t count = sunrise_sd_list_supported_partitions(parts, SD_BROWSE_PARTITION_MAX);
    if (!count) return;

    int index = sd_find_partition_index(parts, count, sd_browse_partition ? sd_browse_partition : sd_mounted_partition);
    index++;
    if ((uint8_t)index >= count) index = 0;

    sd_browse_partition = parts[index].number;
    sd_save_browse_partition(sd_browse_partition);
    sd_unmount_card();
    (void)sd_mount_card();
    set_root_path();
    browse_source_mode = SOURCE_MODE_SD;
    fh_menu_window_active = false;
    memset(filter_query, 0, sizeof(filter_query));
    refresh_requested = true;
    ctrl_ack_value = 1;
}

static void process_delete_sd_file_request(void) {
    ctrl_ack_value = 0;
    cancel_refresh_work();
    if (!sd_mount_card()) {
        ctrl_cmd_state = 0;
        return;
    }

    uint16_t index = query_filtered_index();
    if (index >= total_record_count) {
        ctrl_cmd_state = 0;
        return;
    }

    uint16_t record_index = filtered_indices[index];
    if (record_index >= full_record_count || sd_path_offsets[record_index] == 0xFFFF) {
        ctrl_cmd_state = 0;
        return;
    }

    ROMRecord *rec = &records[record_index];
    if ((rec->Mapper & SOURCE_SD_FLAG) == 0 || (rec->Mapper & FOLDER_FLAG) != 0) {
        ctrl_cmd_state = 0;
        return;
    }

    char pvc_path[SD_PATH_MAX];
    bool has_pvc_path = build_pvc_options_path(record_index, pvc_path, sizeof(pvc_path));
    if (f_unlink(sd_path_buffer + sd_path_offsets[record_index]) == FR_OK) {
        refresh_requested = true;
        FRESULT pvc_result = has_pvc_path ? f_unlink(pvc_path) : FR_NO_FILE;
        if (pvc_result == FR_OK || pvc_result == FR_NO_FILE || pvc_result == FR_NO_PATH) {
            ctrl_ack_value = 1;
        } else {
            ctrl_cmd_state = 0;
        }
    } else {
        ctrl_cmd_state = 0;
    }
}

static bool sd_is_mounted(void) {
    return sd_mounted && sd_card != NULL;
}

static uint16_t add_folder_record(uint16_t record_index, const char *name) {
    if (record_index >= MAX_ROM_RECORDS) {
        return record_index;
    }
    memset(records[record_index].Name, 0, sizeof(records[record_index].Name));
    strncpy(records[record_index].Name, name, sizeof(records[record_index].Name) - 1);
    records[record_index].Mapper = (unsigned char)(FOLDER_FLAG | SOURCE_SD_FLAG);
    records[record_index].Size = 0;
    records[record_index].Offset = 0;
    sd_path_offsets[record_index] = 0xFFFF;
    return (uint16_t)(record_index + 1);
}

static bool wavegame_asset_exists_in_dir(const char *dir, const char *name) {
    char path[SD_PATH_MAX];
    int written;
    if (!dir || !name || dir[0] == '\0') {
        return false;
    }
    if (strcmp(dir, "/") == 0) {
        written = snprintf(path, sizeof(path), "/%s", name);
    } else {
        written = snprintf(path, sizeof(path), "%s/%s", dir, name);
    }
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return false;
    }
    FILINFO info;
    return f_stat(path, &info) == FR_OK && !(info.fattrib & AM_DIR) && info.fsize > 0;
}

static bool wavegame_assets_detected_for_record(uint16_t record_index) {
    if (record_index >= full_record_count || sd_path_offsets[record_index] == 0xFFFF) {
        return false;
    }
    ROMRecord const *rec = &records[record_index];
    uint8_t flags = rec->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG);
    if (!(flags & SOURCE_SD_FLAG) || (flags & (FOLDER_FLAG | MP3_FLAG)) || is_system_record(rec)) {
        return false;
    }

    char dir[SD_PATH_MAX];
    const char *rom_path = sd_path_buffer + sd_path_offsets[record_index];
    size_t len = strlen(rom_path);
    if (len == 0 || len >= sizeof(dir)) {
        return false;
    }
    memcpy(dir, rom_path, len + 1);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        return false;
    }
    if (slash == dir) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    if (wavegame_asset_exists_in_dir(dir, "multi.wav") || wavegame_asset_exists_in_dir(dir, "pause.wav")) {
        return true;
    }
    for (uint8_t i = 0; i < 64; i++) {
        char name[7];
        snprintf(name, sizeof(name), "%02u.wav", (unsigned)i);
        if (wavegame_asset_exists_in_dir(dir, name)) {
            return true;
        }
    }
    return false;
}

static uint16_t add_sd_rom_record(uint16_t record_index, const char *path, const char *filename, uint32_t size, uint8_t mapper) {
    if (record_index >= MAX_ROM_RECORDS) {
        return record_index;
    }
    size_t path_len = strlen(path);
    if (sd_path_buffer_used + path_len + 1 > SD_PATH_BUFFER_SIZE) {
        return record_index;
    }
    sd_path_offsets[record_index] = sd_path_buffer_used;
    memcpy(sd_path_buffer + sd_path_buffer_used, path, path_len + 1);
    sd_path_buffer_used = (uint16_t)(sd_path_buffer_used + path_len + 1);

    char display_name[ROM_NAME_MAX + 1];
    build_display_name(filename, display_name, sizeof(display_name));

    memset(records[record_index].Name, 0, sizeof(records[record_index].Name));
    strncpy(records[record_index].Name, display_name, sizeof(records[record_index].Name) - 1);
    records[record_index].Mapper = (unsigned char)(mapper | SOURCE_SD_FLAG);
    records[record_index].Size = (unsigned long)size;
    records[record_index].Offset = (unsigned long)record_index;
    return (uint16_t)(record_index + 1);
}

static uint16_t add_sd_mp3_record(uint16_t record_index, const char *path, const char *filename, uint32_t size, bool is_wav) {
    if (record_index >= MAX_ROM_RECORDS) {
        return record_index;
    }
    size_t path_len = strlen(path);
    if (sd_path_buffer_used + path_len + 1 > SD_PATH_BUFFER_SIZE) {
        return record_index;
    }
    sd_path_offsets[record_index] = sd_path_buffer_used;
    memcpy(sd_path_buffer + sd_path_buffer_used, path, path_len + 1);
    sd_path_buffer_used = (uint16_t)(sd_path_buffer_used + path_len + 1);

    char display_name[ROM_NAME_MAX + 1];
    build_display_name(filename, display_name, sizeof(display_name));

    memset(records[record_index].Name, 0, sizeof(records[record_index].Name));
    strncpy(records[record_index].Name, display_name, sizeof(records[record_index].Name) - 1);
    records[record_index].Mapper = (unsigned char)(MP3_FLAG | SOURCE_SD_FLAG | (is_wav ? AUDIO_TYPE_WAV : 0u));
    records[record_index].Size = (unsigned long)size;
    records[record_index].Offset = (unsigned long)record_index;
    return (uint16_t)(record_index + 1);
}

static void refresh_records_for_current_path(void) {
    sd_record_count = 0;
    sd_path_buffer_used = 0;
    for (uint16_t i = 0; i < MAX_ROM_RECORDS; i++) {
        sd_path_offsets[i] = 0xFFFF;
    }

    uint16_t record_index = 0;
    bool has_parent = !is_root_path(sd_current_path);

    if (has_parent) {
        record_index = add_folder_record(record_index, "..");
    }

    uint16_t folder_count = record_index;
    if (sd_mount_card()) {
        DIR dir;
        FILINFO fno;
        FRESULT fr = f_opendir(&dir, sd_current_path);
        if (fr == FR_OK) {
            while (record_index < MAX_ROM_RECORDS) {
                fr = f_readdir(&dir, &fno);
                if (fr != FR_OK || fno.fname[0] == '\0') {
                    break;
                }
                if (!(fno.fattrib & AM_DIR)) {
                    continue;
                }
                if (is_excluded_folder(fno.fname)) {
                    continue;
                }
                record_index = add_folder_record(record_index, fno.fname);
            }
        }
        f_closedir(&dir);

        folder_count = record_index;
        uint16_t folder_sort_start = has_parent ? 1u : 0u;
        if (folder_count > folder_sort_start) {
            sort_records_range(folder_sort_start, (uint16_t)(folder_count - folder_sort_start));
        }

        fr = f_opendir(&dir, sd_current_path);
        if (fr == FR_OK) {
            while (record_index < MAX_ROM_RECORDS) {
                fr = f_readdir(&dir, &fno);
                if (fr != FR_OK || fno.fname[0] == '\0') {
                    break;
                }
                if (fno.fattrib & AM_DIR) {
                    continue;
                }
                bool is_mp3 = has_mp3_extension(fno.fname);
                bool is_wav = has_wav_extension(fno.fname);
                bool is_rom = has_rom_extension(fno.fname);
#if EXPLORER_MP3_DISABLED
                // MP3 player temporarily disabled — skip MP3 entries.
                if (is_mp3 || is_wav) continue;
#endif

                if (!is_mp3 && !is_wav && !is_rom) {
                    continue;
                }

                char path[SD_PATH_MAX];
                int written;
                if (is_root_path(sd_current_path)) {
                    written = snprintf(path, sizeof(path), "/%s", fno.fname);
                } else {
                    written = snprintf(path, sizeof(path), "%s/%s", sd_current_path, fno.fname);
                }
                if (written <= 0 || (size_t)written >= sizeof(path)) {
                    continue;
                }

                if (is_mp3 || is_wav) {
                    if (fno.fsize == 0) {
                        continue;
                    }
                    record_index = add_sd_mp3_record(record_index, path, fno.fname, (uint32_t)fno.fsize, is_wav);
                    sd_record_count++;
                    continue;
                }

                if (fno.fsize < MIN_ROM_SIZE || fno.fsize > MAX_ROM_SIZE) {
                    continue;
                }
                if (fno.fsize > SD_ROM_MAX_SIZE) {
                    continue;
                }

                uint8_t mapper = mapper_number_from_filename(fno.fname);
                record_index = add_sd_rom_record(record_index, path, fno.fname, (uint32_t)fno.fsize, mapper);
                sd_record_count++;
            }
        }
        f_closedir(&dir);
    }

    if (is_root_path(sd_current_path) && flash_record_count > 0) {
        for (uint16_t i = 0; i < flash_record_count && record_index < MAX_ROM_RECORDS; i++) {
            records[record_index] = flash_records[i];
            sd_path_offsets[record_index] = 0xFFFF;
            record_index++;
        }
    }

    uint16_t rom_count = (uint16_t)(record_index - folder_count);
    if (rom_count > 1) {
        uint16_t system_insert = folder_count;
        for (uint16_t i = folder_count; i < record_index; i++) {
            if (is_system_record(&records[i])) {
                swap_records(i, system_insert);
                system_insert++;
            }
        }

        uint16_t system_count = (uint16_t)(system_insert - folder_count);
        uint16_t non_system_count = (uint16_t)(record_index - system_insert);
        if (non_system_count > 1) {
            sort_records_range(system_insert, non_system_count);
        }
    }

    full_record_count = record_index;
    memset(filter_query, 0, sizeof(filter_query));
    apply_filter();
    current_page = 0;
    build_page_buffer(current_page);
}

// Chunked version of refresh_records_for_current_path.
// Processes up to REFRESH_CHUNK_SIZE SD reads per call, then returns false
// to yield back to the MP3 decode loop. Returns true when done (or idle).
static bool refresh_records_chunked(void) {
    FILINFO fno;
    FRESULT fr;

    switch (refresh_state) {
    case REFRESH_IDLE:
        return true;

    case REFRESH_INIT:
        sd_record_count = 0;
        sd_path_buffer_used = 0;
        for (uint16_t i = 0; i < MAX_ROM_RECORDS; i++) {
            sd_path_offsets[i] = 0xFFFF;
        }
        refresh_record_index = 0;
        refresh_has_parent = !is_root_path(sd_current_path);
        if (refresh_has_parent) {
            refresh_record_index = add_folder_record(refresh_record_index, "..");
        }
        refresh_folder_count = refresh_record_index;
        ctrl_status_value = CTRL_MAGIC;
        if (browse_source_mode == SOURCE_MODE_FLASH) {
            refresh_state = REFRESH_FINALIZE;
            return false;
        }
        if (!sd_mount_card()) {
            if (browse_source_mode == SOURCE_MODE_SD) {
                ctrl_status_value = CTRL_STATUS_SD_MISSING;
            }
            refresh_state = REFRESH_FINALIZE;
            return false;
        }
        fr = f_opendir(&refresh_dir, sd_current_path);
        if (fr != FR_OK) {
            if (browse_source_mode == SOURCE_MODE_SD) {
                ctrl_status_value = CTRL_STATUS_SD_MISSING;
            }
            refresh_state = REFRESH_FINALIZE;
            return false;
        }
        refresh_state = REFRESH_SCAN_FOLDERS;
        return false;

    case REFRESH_SCAN_FOLDERS: {
        int reads = 0;
        while (reads < REFRESH_CHUNK_SIZE && refresh_record_index < MAX_ROM_RECORDS) {
            fr = f_readdir(&refresh_dir, &fno);
            reads++;
            if (fr != FR_OK || fno.fname[0] == '\0') {
                f_closedir(&refresh_dir);
                refresh_state = REFRESH_SORT_FOLDERS;
                return false;
            }
            if (!(fno.fattrib & AM_DIR)) continue;
            if (is_excluded_folder(fno.fname)) continue;
            refresh_record_index = add_folder_record(refresh_record_index, fno.fname);
        }
        if (refresh_record_index >= MAX_ROM_RECORDS) {
            f_closedir(&refresh_dir);
            refresh_state = REFRESH_SORT_FOLDERS;
        }
        return false;
    }

    case REFRESH_SORT_FOLDERS: {
        refresh_folder_count = refresh_record_index;
        uint16_t folder_sort_start = refresh_has_parent ? 1u : 0u;
        if (refresh_folder_count > folder_sort_start) {
            sort_records_range(folder_sort_start, (uint16_t)(refresh_folder_count - folder_sort_start));
        }
        fr = f_opendir(&refresh_dir, sd_current_path);
        if (fr != FR_OK) {
            refresh_state = REFRESH_FINALIZE;
            return false;
        }
        refresh_state = REFRESH_SCAN_FILES;
        return false;
    }

    case REFRESH_SCAN_FILES: {
        int reads = 0;
        while (reads < REFRESH_CHUNK_SIZE && refresh_record_index < MAX_ROM_RECORDS) {
            fr = f_readdir(&refresh_dir, &fno);
            reads++;
            if (fr != FR_OK || fno.fname[0] == '\0') {
                f_closedir(&refresh_dir);
                refresh_state = REFRESH_FINALIZE;
                return false;
            }
            if (fno.fattrib & AM_DIR) continue;
            bool is_mp3 = has_mp3_extension(fno.fname);
            bool is_wav = has_wav_extension(fno.fname);
            bool is_rom = has_rom_extension(fno.fname);
#if EXPLORER_MP3_DISABLED
            // MP3 player temporarily disabled — skip MP3 entries.
            if (is_mp3 || is_wav) continue;
#endif
            if (!is_mp3 && !is_wav && !is_rom) continue;

            char path[SD_PATH_MAX];
            int written;
            if (is_root_path(sd_current_path)) {
                written = snprintf(path, sizeof(path), "/%s", fno.fname);
            } else {
                written = snprintf(path, sizeof(path), "%s/%s", sd_current_path, fno.fname);
            }
            if (written <= 0 || (size_t)written >= sizeof(path)) {
                continue;
            }

            if (is_mp3 || is_wav) {
                if (fno.fsize == 0) continue;
                refresh_record_index = add_sd_mp3_record(refresh_record_index, path, fno.fname, (uint32_t)fno.fsize, is_wav);
                sd_record_count++;
                continue;
            }

            if (fno.fsize < MIN_ROM_SIZE || fno.fsize > MAX_ROM_SIZE) continue;
            if (fno.fsize > SD_ROM_MAX_SIZE) continue;

            uint8_t mapper = mapper_number_from_filename(fno.fname);
            refresh_record_index = add_sd_rom_record(refresh_record_index, path, fno.fname, (uint32_t)fno.fsize, mapper);
            sd_record_count++;
        }
        if (refresh_record_index >= MAX_ROM_RECORDS) {
            f_closedir(&refresh_dir);
            refresh_state = REFRESH_FINALIZE;
        }
        return false;
    }

    case REFRESH_FINALIZE: {
        if (is_root_path(sd_current_path) && flash_record_count > 0) {
            for (uint16_t i = 0; i < flash_record_count && refresh_record_index < MAX_ROM_RECORDS; i++) {
                records[refresh_record_index] = flash_records[i];
                sd_path_offsets[refresh_record_index] = 0xFFFF;
                refresh_record_index++;
            }
        }

        uint16_t rom_count = (uint16_t)(refresh_record_index - refresh_folder_count);
        if (rom_count > 1) {
            uint16_t system_insert = refresh_folder_count;
            for (uint16_t i = refresh_folder_count; i < refresh_record_index; i++) {
                if (is_system_record(&records[i])) {
                    swap_records(i, system_insert);
                    system_insert++;
                }
            }
            uint16_t system_count = (uint16_t)(system_insert - refresh_folder_count);
            uint16_t non_system_count = (uint16_t)(refresh_record_index - system_insert);
            if (non_system_count > 1) {
                sort_records_range(system_insert, non_system_count);
            }
        }

        full_record_count = refresh_record_index;
        memset(filter_query, 0, sizeof(filter_query));
        apply_filter();
        resolve_last_selection_match();
        current_page = 0;
        build_page_buffer(current_page);
        refresh_state = REFRESH_IDLE;
        return true;
    }

    default:
        refresh_state = REFRESH_IDLE;
        return true;
    }
}

static void process_detect_mapper_request(uint16_t filtered_index) {
    ctrl_mapper_value = 0;
    quiesce_mp3_core1_before_sd_work();

    if (!sd_mount_card()) {
        ctrl_cmd_state = 0;
        return;
    }

    if (filtered_index >= total_record_count) {
        ctrl_cmd_state = 0;
        return;
    }

    uint16_t record_index = filtered_indices[filtered_index];
    if (record_index >= full_record_count) {
        ctrl_cmd_state = 0;
        return;
    }

    ROMRecord *rec = &records[record_index];
    uint8_t flags = rec->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG);

    if (!(flags & SOURCE_SD_FLAG) || (flags & (FOLDER_FLAG | MP3_FLAG)) || sd_path_offsets[record_index] == 0xFFFF) {
        ctrl_mapper_value = mapper_code_from_record_byte(rec->Mapper);
        ctrl_cmd_state = 0;
        return;
    }

    const char *path = sd_path_buffer + sd_path_offsets[record_index];
    const char *filename = basename_from_path(path);

    uint8_t mapper = mapper_number_from_filename(filename);
    if (mapper == 0) {
        mapper = detect_rom_type_from_file(path, (uint32_t)rec->Size);
    }

    ctrl_mapper_value = mapper;
    if (mapper != 0) {
        rec->Mapper = (uint8_t)(flags | mapper);
    } else {
        rec->Mapper = flags;
    }

    ctrl_cmd_state = 0;
}

static void init_pico_chip_id(void) {
    pico_get_unique_board_id_string(pico_chip_id, sizeof(pico_chip_id));
    pico_chip_id[CTRL_CHIP_ID_SIZE - 1u] = '\0';
}

static void ensure_mp3_core1_started(void);

// Find an audio filtered index for auto-advance and manual skip.
// mode: MP3_PLAY_MODE_ALL = sequential wrap, MP3_PLAY_MODE_RANDOM = random pick.
static uint16_t find_mp3_filtered_index(uint16_t current, uint8_t mode, int direction) {
    if (total_record_count == 0) return 0xFFFF;

    if (mode == MP3_PLAY_MODE_RANDOM) {
        uint16_t mp3_count = 0;
        for (uint16_t i = 0; i < total_record_count; i++) {
            uint16_t ri = filtered_indices[i];
            if (ri < full_record_count && (records[ri].Mapper & MP3_FLAG))
                mp3_count++;
        }
        if (mp3_count == 0) return 0xFFFF;

        // Use microsecond timer as entropy for random pick
        uint16_t target = (uint16_t)(time_us_32() % mp3_count);
        uint16_t ord = 0;
        for (uint16_t i = 0; i < total_record_count; i++) {
            uint16_t ri = filtered_indices[i];
            if (ri < full_record_count && (records[ri].Mapper & MP3_FLAG)) {
                if (ord == target) return i;
                ord++;
            }
        }
        return 0xFFFF;
    }

    if (direction < 0) {
        uint16_t start = (current != 0xFFFF && current > 0) ? (uint16_t)(current - 1u) : (uint16_t)(total_record_count - 1u);
        for (uint16_t count = 0; count < total_record_count; count++) {
            uint16_t i = (uint16_t)((start + total_record_count - count) % total_record_count);
            uint16_t ri = filtered_indices[i];
            if (ri < full_record_count && (records[ri].Mapper & MP3_FLAG)) {
                return i;
            }
        }
        return 0xFFFF;
    }

    uint16_t start = (current != 0xFFFF && current + 1 < total_record_count) ? (uint16_t)(current + 1u) : 0;
    for (uint16_t count = 0; count < total_record_count; count++) {
        uint16_t i = (start + count) % total_record_count;
        uint16_t ri = filtered_indices[i];
        if (ri < full_record_count && (records[ri].Mapper & MP3_FLAG)) {
            return i;
        }
    }
    return 0xFFFF;
}

static bool mp3_play_filtered_index(uint16_t filtered_index) {
    if (filtered_index >= total_record_count) {
        return false;
    }
    uint16_t record_index = filtered_indices[filtered_index];
    if (record_index >= full_record_count || !(records[record_index].Mapper & MP3_FLAG) || sd_path_offsets[record_index] == 0xFFFF) {
        return false;
    }
    const char *path = sd_path_buffer + sd_path_offsets[record_index];
    printf("MP3: play filtered=%u path=%s\n", (unsigned)filtered_index, path);
    ensure_mp3_core1_started();
    mp3_select_file(path, (uint32_t)records[record_index].Size);
    mp3_send_cmd(MP3_CMD_PLAY);
    mp3_playing_filtered_index = filtered_index;
    mp3_selected_index = filtered_index;
    return true;
}

// Lazy non-blocking launch of Core 1 + MP3 stack. Called from
// core1_bg_work() the first time the MSX dispatches an MP3 command.
// We deliberately do NOT spin Core 0 waiting for Core 1 to finish
// mp3_init() — Core 0 is servicing the MSX PIO bus and any block
// here freezes the cartridge. The cross-core command queue is a
// small ring buffer so SELECT and PLAY (issued ~10 ms apart) both
// fit while Core 1 is still initialising.
static bool mp3_core1_started = false;

static void hold_msx_wait(void) {
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);
}

static void ensure_mp3_core1_started(void) {
#if EXPLORER_MP3_DISABLED
    // MP3 player disabled: never launch Core 1 from the menu, so the
    // SD card stays single-owner on Core 0 with no risk of cross-core
    // FatFS / I2S / DMA contention.
    return;
#else
    if (mp3_core1_started) return;
    // Keep MP3 stream buffering out of SRAM so MSX-MUSIC can allocate its OPLL state later.
    psram_prepare_mp3_buffer();
    // If a prior File Hunter operation (or ROM launch) quiesced the audio core
    // and handed off the live I2S pool, re-adopt it so the relaunched core
    // reuses the pico_audio_i2s pipeline instead of re-running audio_i2s_setup()
    // (which leaks PIO/IRQ-handler slots on the vendored singleton and is
    // unreliable as a re-setup). claim_rom_audio_handoff_pool() also re-arms the
    // I2S DMA. ROM-launch paths consume the pool themselves before calling here,
    // so this is a no-op for them.
    {
        struct audio_buffer_pool *handoff = claim_rom_audio_handoff_pool();
        if (handoff)
            mp3_adopt_i2s_audio_pool(handoff);
    }
    mp3_core1_started = true;
    multicore_launch_core1(mp3_core1_loop);
#endif
}

#define WAVEGAME_PORT 0x92u

static inline bool __not_in_flash_func(main_psg_handle_io_write)(uint8_t port, uint8_t data);

static void wavegame_clear(void)
{
    wavegame_active = false;
    wavegame_psg_mirror_active = false;
    wavegame_dir[0] = '\0';
    wavegame_defer_next = false;
    wavegame_deferred_valid = false;
    wavegame_deferred_data = 0;
    mp3_wavegame_set_psg_callbacks(NULL, NULL);
}

static void wavegame_prepare_for_rom(uint16_t record_index, bool enabled)
{
    wavegame_clear();
    if (!enabled || record_index >= MAX_ROM_RECORDS || sd_path_offsets[record_index] == 0xFFFF)
        return;

    const char *rom_path = sd_path_buffer + sd_path_offsets[record_index];
    size_t len = strlen(rom_path);
    if (len == 0 || len >= sizeof(wavegame_dir))
        return;

    memcpy(wavegame_dir, rom_path, len + 1);
    char *slash = strrchr(wavegame_dir, '/');
    if (!slash)
        return;
    if (slash == wavegame_dir)
        slash[1] = '\0';
    else
        slash[0] = '\0';
    wavegame_active = true;
    printf("WAVEGAME: active dir=%s\n", wavegame_dir);
}

static bool wavegame_is_playing(void)
{
    if (!mp3_core1_started)
        return false;
    uint8_t status = mp3_get_status();
    return (status & (MP3_STATUS_PLAYING | MP3_STATUS_PAUSED)) != 0;
}

static bool wavegame_execute_command(uint8_t data, bool allow_defer)
{
    if ((data & 0xC0u) == 0x40u)
    {
        ensure_mp3_core1_started();
        mp3_wavegame_play_index(wavegame_dir, (uint8_t)(data & 0x3Fu), false);
        return true;
    }
    if ((data & 0xC0u) == 0x80u)
    {
        ensure_mp3_core1_started();
        mp3_wavegame_play_index(wavegame_dir, (uint8_t)(data & 0x3Fu), true);
        return true;
    }
    if ((data & 0xF0u) == 0x00u)
    {
        ensure_mp3_core1_started();
        mp3_wavegame_fade_out((uint8_t)(data & 0x0Fu));
        return true;
    }

    switch (data)
    {
        case 0xC0u:
            ensure_mp3_core1_started();
            mp3_wavegame_toggle_pause();
            return true;
        case 0xD0u:
            if (allow_defer)
                wavegame_defer_next = true;
            return true;
        case 0xE0u:
            ensure_mp3_core1_started();
            mp3_wavegame_resume_previous();
            return true;
        case 0xF0u:
            ensure_mp3_core1_started();
            mp3_wavegame_play_pause(wavegame_dir);
            return true;
        default:
            return true;
    }
}

static inline bool __not_in_flash_func(wavegame_handle_io_write)(uint8_t port, uint8_t data)
{
    if (!wavegame_active || port != WAVEGAME_PORT)
        return false;

    if (wavegame_defer_next)
    {
        wavegame_defer_next = false;
        wavegame_deferred_data = data;
        wavegame_deferred_valid = true;
        if (!wavegame_is_playing())
        {
            wavegame_deferred_valid = false;
            (void)wavegame_execute_command(wavegame_deferred_data, false);
        }
        return true;
    }

    printf("WAVEGAME: port=92 data=%02x\n", (unsigned)data);
    return wavegame_execute_command(data, true);
}

static inline void __not_in_flash_func(wavegame_service_io)(void)
{
    if (!wavegame_active)
        return;

    uint16_t io_addr;
    uint8_t io_data;
    while (pio_try_get_io_write(&io_addr, &io_data))
    {
        uint8_t port = (uint8_t)(io_addr & 0xFFu);
        if (wavegame_handle_io_write(port, io_data))
            continue;
        if (wavegame_psg_mirror_active)
            (void)main_psg_handle_io_write(port, io_data);
    }

    if (wavegame_deferred_valid && !wavegame_is_playing())
    {
        uint8_t data = wavegame_deferred_data;
        wavegame_deferred_valid = false;
        printf("WAVEGAME: deferred data=%02x\n", (unsigned)data);
        (void)wavegame_execute_command(data, false);
    }
}

static void shutdown_mp3_core1_before_rom_launch(void) {
#if EXPLORER_MP3_DISABLED
    return;
#else
    debug_trace("DBG shutdown_mp3 enter started=%u", mp3_core1_started ? 1u : 0u);
    if (!mp3_core1_started) {
        return;
    }

    debug_trace("DBG shutdown_mp3 request");
    mp3_request_shutdown();
    uint32_t deadline = time_us_32() + 2000000u;
    while (!mp3_core1_has_stopped() && (int32_t)(deadline - time_us_32()) > 0) {
        sleep_us(200);
    }

    debug_trace("DBG shutdown_mp3 stopped=%u", mp3_core1_has_stopped() ? 1u : 0u);
    if (!mp3_core1_has_stopped()) {
        debug_trace("DBG shutdown_mp3 reset/deinit");
        multicore_reset_core1();
        mp3_deinit();
    } else {
        debug_trace("DBG shutdown_mp3 take pool");
        rom_audio_handoff_pool = mp3_take_i2s_audio_pool();
        multicore_reset_core1();
    }
    mp3_core1_started = false;
    debug_trace("DBG shutdown_mp3 done pool=%p", (void *)rom_audio_handoff_pool);
#endif
}

static void force_mp3_core1_handoff_before_rom_launch(void) {
#if EXPLORER_MP3_DISABLED
    return;
#else
    debug_trace("DBG force_mp3 enter started=%u", mp3_core1_started ? 1u : 0u);
    if (!mp3_core1_started) {
        return;
    }

    debug_trace("DBG force_mp3 reset core1");
    multicore_reset_core1();
    debug_trace("DBG force_mp3 take pool");
    rom_audio_handoff_pool = mp3_force_i2s_handoff_from_core0();
    mp3_core1_started = false;
    debug_trace("DBG force_mp3 done pool=%p", (void *)rom_audio_handoff_pool);
#endif
}

static void quiesce_mp3_core1_before_sd_work(void) {
#if EXPLORER_MP3_DISABLED
    return;
#else
    if (!mp3_core1_started) {
        return;
    }

    mp3_pending_cmd = 0;
    mp3_pending_select = false;
    shutdown_mp3_core1_before_rom_launch();
#endif
}

// Background work pump. Runs on Core 0 from the menu loop idle branch
// (no PIO traffic from the MSX). Single-core ownership of the SD card
// avoids FatFS reentrancy issues and removes the freeze observed when
// folder navigation raced with MP3 work on Core 1.
static void fh_save_background_work(void);
static void core1_bg_work(void) {
    if (fh_save_state == FH_SAVE_RUNNING) {
        fh_save_background_work();
        return;
    }

    // Bridge MP3 commands queued by handle_menu_write_explorer() to the
    // mp3.c command queue. Lazy-launch Core 1 the first time an MP3
    // command actually arrives.
    if (mp3_pending_select) {
        uint16_t pending_index = mp3_pending_index;
        mp3_pending_select = false;
        if (pending_index < total_record_count) {
            uint16_t record_index = filtered_indices[pending_index];
            if (record_index < full_record_count) {
                ROMRecord const *rec = &records[record_index];
                if ((rec->Mapper & MP3_FLAG) && (sd_path_offsets[record_index] != 0xFFFF)) {
                    const char *path = sd_path_buffer + sd_path_offsets[record_index];
                    printf("MP3: select path=%s size=%lu\n", path, (unsigned long)rec->Size);
                    ensure_mp3_core1_started();
                    mp3_select_file(path, (uint32_t)rec->Size);
                    mp3_playing_filtered_index = pending_index;
                } else {
                    printf("MP3: select ignored (not audio or no path)\n");
                }
            } else {
                printf("MP3: select index out of range\n");
            }
        } else {
            printf("MP3: select index invalid (total=%u)\n", (unsigned int)total_record_count);
        }
    }
    if (mp3_pending_cmd != 0) {
        uint8_t cmd = mp3_pending_cmd;
        mp3_pending_cmd = 0;
        if (cmd == MP3_CMD_NEXT || cmd == MP3_CMD_PREVIOUS) {
            uint16_t next = find_mp3_filtered_index(mp3_playing_filtered_index, mp3_play_mode,
                                                    (cmd == MP3_CMD_PREVIOUS) ? -1 : 1);
            if (next != 0xFFFF) {
                (void)mp3_play_filtered_index(next);
            }
        } else if (mp3_core1_started || cmd == MP3_CMD_PLAY) {
            ensure_mp3_core1_started();
            mp3_send_cmd(cmd);
        }
    }

    // Auto-advance: when current track ends and mode is ALL or RANDOM,
    // automatically select and play the next track.
    if (mp3_play_mode != MP3_PLAY_MODE_SINGLE) {
        uint8_t st = mp3_get_status();
        if ((st & MP3_STATUS_EOF) && !(st & MP3_STATUS_PLAYING)) {
            uint16_t next = find_mp3_filtered_index(mp3_playing_filtered_index, mp3_play_mode, 1);
            if (next != 0xFFFF) {
                uint16_t ri = filtered_indices[next];
                const char *path = sd_path_buffer + sd_path_offsets[ri];
                printf("MP3: auto-advance to index %u path=%s\n", (unsigned)next, path);
                mp3_auto_play(path, (uint32_t)records[ri].Size);
                mp3_playing_filtered_index = next;
                mp3_selected_index = next;
            }
        }
    }

    // Defer SD-heavy work while MP3 is actively playing to avoid
    // SPI bus contention between directory reads and MP3 file reads.
    if (mp3_get_status() & MP3_STATUS_PLAYING) {
        return;
    }

    // Chunked directory refresh — process one chunk then yield
    if (refresh_state != REFRESH_IDLE) {
        bool done = refresh_records_chunked();
        if (done) {
            refresh_in_progress = false;
            ctrl_cmd_state = 0;
        }
        return;
    }

    if (refresh_requested) {
        refresh_requested = false;
        refresh_in_progress = true;
        refresh_state = REFRESH_INIT;
        return;
    }

    if (detect_mapper_pending && !refresh_in_progress) {
        uint16_t pending_index = detect_mapper_index;
        detect_mapper_pending = false;
        process_detect_mapper_request(pending_index);
    }
}

static bool load_rom_from_sd(uint16_t record_index, uint32_t size) {
    if (!sd_mount_card()) {
        printf("SDLOAD: mount failed\n");
        return false;
    }
    if (record_index >= full_record_count || record_index >= MAX_ROM_RECORDS) {
        printf("SDLOAD: bad record=%u full=%u\n", (unsigned)record_index, (unsigned)full_record_count);
        return false;
    }
    if (sd_path_offsets[record_index] == 0xFFFF) {
        printf("SDLOAD: missing path record=%u\n", (unsigned)record_index);
        return false;
    }

    // SD ROMs are streamed into the 4MB PSRAM region so rom_sram remains
    // available for flash-resident mapper caching. Bring PSRAM up lazily.
    if (!psram_bring_up_once()) {
        printf("SDLOAD: psram bring-up failed\n");
        return false;
    }
    if (size > sd_rom_region.size) {
        printf("SDLOAD: too large size=%lu region=%lu\n", (unsigned long)size, (unsigned long)sd_rom_region.size);
        return false;
    }

    const char *path = sd_path_buffer + sd_path_offsets[record_index];
    printf("SDLOAD: record=%u size=%lu path=%s\n", (unsigned)record_index, (unsigned long)size, path);

    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        printf("SDLOAD: open failed fr=%d\n", (int)fr);
        return false;
    }

    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    uint8_t *dst = sd_rom_region.ptr;
    memset(dst, 0, size);

    UINT total = 0;
    while (total < size) {
        UINT to_read = (UINT)((size - total) > 4096 ? 4096 : (size - total));
        UINT br = 0;
        fr = f_read(&fil, dst + total, to_read, &br);
        if (fr != FR_OK || br == 0) {
            printf("SDLOAD: read stopped fr=%d br=%u total=%u target=%lu\n", (int)fr, (unsigned)br, (unsigned)total, (unsigned long)size);
            break;
        }
        total += br;
    }

    f_close(&fil);
    gpio_set_dir(PIN_WAIT, GPIO_IN);

    if (total != size) {
        printf("SDLOAD: short read total=%u target=%lu\n", (unsigned)total, (unsigned long)size);
        return false;
    }
    printf("SDLOAD: loaded %u bytes\n", (unsigned)total);
    return true;
}

// Initialize GPIO pins
static inline void setup_gpio()
{

    for (int i = 0; i <= 23; i++) {
        gpio_init(i);
        gpio_set_input_hysteresis_enabled(i, true);
    }

    for (int i = 0; i <= 15; i++) {
        gpio_set_dir(i, GPIO_IN);
    }

    // Initialize control pins as input
    gpio_init(PIN_RD); gpio_set_dir(PIN_RD, GPIO_IN);    
    gpio_init(PIN_WR); gpio_set_dir(PIN_WR, GPIO_IN); 
    gpio_init(PIN_IORQ); gpio_set_dir(PIN_IORQ, GPIO_IN); 
    gpio_init(PIN_SLTSL); gpio_set_dir(PIN_SLTSL, GPIO_IN); 
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN); 

    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 1);
}

// read_ulong - Read a 4-byte value from the memory area
// This function will read a 4-byte value from the memory area pointed by ptr and return the value as an unsigned long
// Parameters:
//   ptr - Pointer to the memory area to read the value from
// Returns:
//   The 4-byte value as an unsigned long 
unsigned long __no_inline_not_in_flash_func(read_ulong)(const unsigned char *ptr) {
    return (unsigned long)ptr[0] |
           ((unsigned long)ptr[1] << 8) |
           ((unsigned long)ptr[2] << 16) |
           ((unsigned long)ptr[3] << 24);
}

// isEndOfData - Check if the memory area is the end of the data
// This function will check if the memory area pointed by memory is the end of the data. The end of the data is defined by all bytes being 0xFF.
// Parameters:
//   memory - Pointer to the memory area to check
// Returns:
//   1 if the memory area is the end of the data, 0 otherwise
int __no_inline_not_in_flash_func(isEndOfData)(const unsigned char *memory) {
    for (int i = 0; i < ROM_RECORD_SIZE; i++) {
        if (memory[i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}

// -----------------------------------------------------------------------
// External PSRAM (QMI CS1) - hardware init + bump allocator.
// Mirrors the implementation in multirom so the same code paths work
// across the two firmwares. Used for SD-cached ROM storage so that
// rom_sram remains fully available for flash-resident mapper caching.
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
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) { }
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) { }
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
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) { }

    // Warm restart path: if PSRAM remained powered it may still be in QPI mode.
    psram_send_direct_cmd(0xF5u, true);
    psram_delay_cycles(128u);

    psram_send_direct_cmd(0x66u, false);
    psram_delay_cycles(128u);
    psram_send_direct_cmd(0x99u, false);
    psram_delay_cycles(50000u);
    psram_send_direct_cmd(0x35u, false);
    psram_delay_cycles(128u);
    // LY68L6400S PSRAM compatibility: Ludovic Avot reported that this chip
    // does not tolerate the optional 0xC0 post-QPI initialization command.
    // psram_send_direct_cmd(0xC0u, false);
    // psram_delay_cycles(128u);

    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    // PSRAM QMI timing - tuned for 210 MHz system clock (CLKDIV=2 -> ~52.5 MHz).
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

    // Verify PSRAM via uncached window (bypass XIP cache).
    volatile uint32_t *psram_test = (volatile uint32_t *)0x15000000u;
    psram_test[0] = 0x12345678u;
    return psram_test[0] == 0x12345678u;
}

// -----------------------------------------------------------------------
// PSRAM Memory Manager - bump allocator for 8MB external PSRAM.
// -----------------------------------------------------------------------
static struct {
    uint32_t next_free;
    bool initialised;
} psram_mgr;

static void psram_mem_init(void)
{
    psram_mgr.next_free = 0;
    memset(&sd_rom_region, 0, sizeof(sd_rom_region));
    memset(&rom_cache_region, 0, sizeof(rom_cache_region));
    rom_sram = NULL;
    memset(&mapper_region, 0, sizeof(mapper_region));
    memset(&c2_rom_region, 0, sizeof(c2_rom_region));
    memset(&megaram_region, 0, sizeof(megaram_region));
    memset(&mp3_buffer_region, 0, sizeof(mp3_buffer_region));
    memset(&fh_list_region, 0, sizeof(fh_list_region));
    memset(&fh_download_region, 0, sizeof(fh_download_region));
    fh_download_size = 0;
    memset(fh_download_name, 0, sizeof(fh_download_name));
}

static bool psram_alloc(uint32_t size, psram_region_t *region)
{
    if (psram_mgr.next_free + size > PSRAM_TOTAL_SIZE)
        return false;
    region->offset = psram_mgr.next_free;
    region->size   = size;
    region->ptr    = (uint8_t *)(PSRAM_BASE_ADDR + psram_mgr.next_free);
    psram_mgr.next_free += size;
    return true;
}

// One-time PSRAM bring-up + region carve-out. Idempotent. Returns false if
// the hardware probe fails or any allocation cannot be satisfied.
static bool psram_bring_up_once(void)
{
    if (psram_mgr.initialised) return true;
    if (!psram_init()) return false;
    psram_mem_init();
    // 4MB region for SD-loaded ROMs.
    if (!psram_alloc(SD_ROM_MAX_SIZE, &sd_rom_region)) return false;
    psram_mgr.initialised = true;
    return true;
}

static bool psram_prepare_mapper_region(void)
{
    if (!psram_bring_up_once()) return false;
    if (mapper_region.size == MAPPER_SIZE) return true;
    return psram_alloc(MAPPER_SIZE, &mapper_region);
}

static bool psram_prepare_c2_region(void)
{
    if (!psram_bring_up_once()) return false;
    if (c2_rom_region.size == MAPPER_SIZE) return true;
    return psram_alloc(MAPPER_SIZE, &c2_rom_region);
}

static bool psram_prepare_megaram_region(void)
{
    if (!psram_bring_up_once()) return false;
    if (megaram_region.size == MEGARAM_SIZE) return true;
    return psram_alloc(MEGARAM_SIZE, &megaram_region);
}

static bool psram_prepare_mp3_buffer(void)
{
    if (!psram_bring_up_once()) return false;
    if (mp3_buffer_region.ptr && mp3_buffer_region.size >= MP3_PSRAM_BUFFER_SIZE) return true;
    if (!psram_alloc(MP3_PSRAM_BUFFER_SIZE, &mp3_buffer_region)) return false;
    mp3_set_external_buffer(mp3_buffer_region.ptr, mp3_buffer_region.size);
    return true;
}

static bool psram_prepare_rom_cache(void)
{
    if (!psram_bring_up_once()) return false;
    if (rom_cache_region.size == CACHE_SIZE)
    {
        rom_sram = rom_cache_region.ptr;
        return true;
    }
    if (!psram_alloc(CACHE_SIZE, &rom_cache_region)) return false;
    rom_sram = rom_cache_region.ptr;
    return true;
}

static bool fh_prepare_list_region(void)
{
    if (!psram_bring_up_once()) return false;
    if (fh_list_region.ptr && fh_list_region.size >= FH_HTTP_BUFFER_SIZE) return true;
    return psram_alloc(FH_HTTP_BUFFER_SIZE, &fh_list_region);
}

// Per-ROM boot patch: the launched game's cartridge INIT is redirected through a
// small stub that applies the 50/60Hz and CPU speed options before jumping to the
// original entry point. The menu cannot do this itself because the warm RST 00h
// reset that boots the ROM re-initialises VDP R9 and returns the turbo R to Z80
// mode. The stub runs as the cartridge INIT with the BIOS in page 0, so the one
// remaining main-ROM call below (CHGCPU) is valid.
//
//   50/60Hz : VDP register 9 written directly through port 0x99 (bit 1 selects
//             50/60Hz). The BIOS WRTVDP entry (0x0047) must NOT be used here: on
//             MSX2 it only writes the port itself for registers 0..7 and routes
//             every register >= 8 (so R9) to the SUB-ROM through an inter-slot
//             EXTROM/CALSLT call (push ix / ld ix,#012D / jp #0295). Calling the
//             SUB-ROM from a cartridge INIT, while the BIOS boot slot scan is
//             still running, depends on boot state that is not guaranteed yet
//             and is machine dependent -- it is what stopped patched games from
//             booting. The direct-write stub also mirrors the value into the
//             BIOS shadow RG9SAV (0xFFE8) so games that reload R9 from the
//             shadow via a BIOS screen call keep the requested mode. Cartridge
//             INIT runs inside the BIOS inter-slot call with interrupts disabled.
//   Turbo   : Panasonic switched-I/O device 8, port 0x41 bit 0 (0 = 5.37MHz).
//   R800    : CHGCPU (0x0180) with A=0x81 (R800 ROM mode + turbo LED). That one
//             is a plain main-ROM entry (no SUB-ROM), so it stays a direct call.
//
// Both CPU stubs re-verify the machine at run time (MSXVER for the turbo R, the
// port 0x40 read-back for Panasonic) because the quick-run path replays a stored
// option without the detail screen's machine check, and the microSD card may
// have been configured on a different computer.
#define BOOT_STUB_MAX 32u
// Where the relocated stub may live inside the cached image. When the BIOS calls
// a cartridge INIT it has the cartridge enabled in page 1 only (0x4000..0x7FFF);
// page 2 still holds RAM until the ROM enables its own slot there, which games
// do from their INIT (King's Valley II calls ENASLT with H=0x80 for exactly
// that). So the stub has to live in the 16 KB seen through page 1. ASCII8 resets
// all four 8 KB windows to block 0, so only the first 8 KB of its image is
// reachable; every other cached mapper (plain 16/32 KB, linear 48 KB,
// Konami/Konami-SCC {0,1,2,3} and ASCII16 {0,1}) starts with sequential blocks,
// so image 0x0000..0x3FFF is at 0x4000..0x7FFF.
#define BOOT_STUB_SEARCH_BLOCK0 8192u
#define BOOT_STUB_SEARCH_PAGE1  16384u

// The stub is emitted as a byte stream plus the offsets at which it may be cut
// into fragments. A fragment boundary is only legal at an instruction boundary,
// and never inside the CPU fragments, whose `jr nz` displacements would no
// longer reach their target if the code around them were relocated.
typedef struct {
    uint8_t bytes[BOOT_STUB_MAX];
    uint8_t len;
    uint8_t cuts[BOOT_STUB_MAX];
    uint8_t cut_count;
} boot_stub_t;

static void __not_in_flash_func(boot_stub_mark_cut)(boot_stub_t *s)
{
    if (s->cut_count < BOOT_STUB_MAX) s->cuts[s->cut_count++] = s->len;
}

// Builds the INIT stub. `compact` selects a shorter frequency form used when the
// ROM has too little padding for the full one: it writes R9 outright instead of
// preserving the register's other bits, and carries no CPU option.
static void __not_in_flash_func(build_boot_stub)(boot_stub_t *s, uint16_t orig_init, bool compact)
{
    s->len = 0;
    s->cut_count = 0;
    boot_stub_mark_cut(s);                                                   // a fragment may start at 0

    if (vdp_freq_launch != VDP_FREQ_DEFAULT)
    {
        if (compact)
        {
            uint8_t r9 = (vdp_freq_launch == VDP_FREQ_50HZ) ? 0x02u : 0x00u;
            s->bytes[s->len++] = 0x01u; s->bytes[s->len++] = 0x09u;
            s->bytes[s->len++] = r9;                                         // ld bc,r9*256+9
            boot_stub_mark_cut(s);
            s->bytes[s->len++] = 0xCDu; s->bytes[s->len++] = 0x47u;
            s->bytes[s->len++] = 0x00u;                                      // call 0047h WRTVDP
            boot_stub_mark_cut(s);
            s->bytes[s->len++] = 0xC3u;                                      // jp orig_init
            s->bytes[s->len++] = (uint8_t)(orig_init & 0xFFu);
            s->bytes[s->len++] = (uint8_t)(orig_init >> 8);
            return;
        }

        // Carnivore2's boot menu applies the refresh mode this way and is known to
        // work on real MSX2/MSX2+ hardware: take the current RG9SAV, flip only bit 1
        // (0 = 60Hz/NTSC, 1 = 50Hz/PAL) so every other R9 bit is preserved, and let
        // the BIOS WRTVDP entry apply it. On MSX2 WRTVDP routes register 9 through
        // the SUB-ROM, which performs the full machine-correct update of both the
        // VDP register and its RG9SAV shadow -- a raw port 0x99 write does not.
        uint8_t bitop = (vdp_freq_launch == VDP_FREQ_50HZ) ? 0xCFu : 0x8Fu;  // set/res 1,a
        s->bytes[s->len++] = 0x3Au; s->bytes[s->len++] = 0xE8u;
        s->bytes[s->len++] = 0xFFu;                                          // ld a,(0FFE8h) RG9SAV
        boot_stub_mark_cut(s);
        s->bytes[s->len++] = 0xCBu; s->bytes[s->len++] = bitop;              // set 1,a / res 1,a
        boot_stub_mark_cut(s);
        s->bytes[s->len++] = 0x47u;                                          // ld b,a
        boot_stub_mark_cut(s);
        s->bytes[s->len++] = 0x0Eu; s->bytes[s->len++] = 0x09u;              // ld c,9
        boot_stub_mark_cut(s);
        s->bytes[s->len++] = 0xCDu; s->bytes[s->len++] = 0x47u;
        s->bytes[s->len++] = 0x00u;                                          // call 0047h WRTVDP
        boot_stub_mark_cut(s);
    }

    // The CPU fragments below contain relative jumps, so no cut is marked inside
    // them: each is emitted as one indivisible block.
    if (cpu_mode_launch == CPU_MODE_R800)
    {
        s->bytes[s->len++] = 0x3Au; s->bytes[s->len++] = 0x2Du;
        s->bytes[s->len++] = 0x00u;                                          // ld a,(002Dh) MSXVER
        s->bytes[s->len++] = 0xFEu; s->bytes[s->len++] = 0x03u;              // cp 3 (turbo R)
        s->bytes[s->len++] = 0x20u; s->bytes[s->len++] = 0x05u;              // jr nz,+5
        s->bytes[s->len++] = 0x3Eu; s->bytes[s->len++] = 0x81u;              // ld a,81h (R800 ROM + LED)
        s->bytes[s->len++] = 0xCDu; s->bytes[s->len++] = 0x80u;
        s->bytes[s->len++] = 0x01u;                                          // call 0180h (CHGCPU)
        boot_stub_mark_cut(s);
    }
    else if (cpu_mode_launch == CPU_MODE_TURBO)
    {
        s->bytes[s->len++] = 0x3Eu; s->bytes[s->len++] = 0x08u;              // ld a,8 (Matsushita id)
        s->bytes[s->len++] = 0xD3u; s->bytes[s->len++] = 0x40u;              // out (40h),a
        s->bytes[s->len++] = 0xDBu; s->bytes[s->len++] = 0x40u;              // in a,(40h)
        s->bytes[s->len++] = 0xFEu; s->bytes[s->len++] = 0xF7u;              // cp ~8
        s->bytes[s->len++] = 0x20u; s->bytes[s->len++] = 0x04u;              // jr nz,+4
        s->bytes[s->len++] = 0x3Eu; s->bytes[s->len++] = 0x80u;              // ld a,80h (bit0=0 -> 5.37MHz)
        s->bytes[s->len++] = 0xD3u; s->bytes[s->len++] = 0x41u;              // out (41h),a
        boot_stub_mark_cut(s);
    }

    s->bytes[s->len++] = 0xC3u;                                              // jp orig_init
    s->bytes[s->len++] = (uint8_t)(orig_init & 0xFFu);
    s->bytes[s->len++] = (uint8_t)(orig_init >> 8);
}

// A single run of padding bytes chosen to host part of the stub.
typedef struct { uint32_t off; uint32_t len; } boot_cave_t;

// Up to this many padding runs may be chained together to host one stub. ROMs
// with a single large run need one; heavily packed ROMs (dsk2rom images in
// particular) only offer a handful of very short runs.
#define BOOT_STUB_MAX_CAVES 4u

// Finds the largest run of padding bytes (0x00 or 0xFF) inside the mapped
// window that is at least `min_len` long and does not overlap a run already
// chosen. The largest run wins because it is the one most likely to be genuine
// trailing padding rather than a zero-filled table the game reads. Offsets
// below 16 are never considered: those are the cartridge header.
static bool __not_in_flash_func(find_boot_cave)(
    const uint8_t *img,
    uint32_t limit,
    const boot_cave_t *used,
    uint8_t used_count,
    uint32_t min_len,
    boot_cave_t *out)
{
    if (limit <= 16u || min_len == 0u) return false;

    uint32_t best_off = 0;
    uint32_t best_len = 0;
    uint32_t i = 16u;

    while (i < limit)
    {
        uint8_t b = img[i];
        if (b != 0x00u && b != 0xFFu) { i++; continue; }

        uint32_t start = i;
        while (i < limit && img[i] == b) i++;
        uint32_t run = i - start;
        if (run < min_len || run <= best_len) continue;

        bool clash = false;
        for (uint8_t u = 0; u < used_count; u++)
        {
            if (start < used[u].off + used[u].len && used[u].off < start + run)
            {
                clash = true;
                break;
            }
        }
        if (clash) continue;

        best_off = start;
        best_len = run;
    }

    if (best_len == 0u) return false;
    out->off = best_off;
    out->len = best_len;
    return true;
}

// Largest legal cut offset that is greater than `pos` and no further than
// `pos + max_take`. Returns `pos` when the fragment could not hold even one
// whole instruction.
static uint8_t __not_in_flash_func(boot_stub_next_cut)(const boot_stub_t *s, uint8_t pos, uint8_t max_take)
{
    uint8_t best = pos;
    for (uint8_t i = 0; i < s->cut_count; i++)
    {
        uint8_t c = s->cuts[i];
        if (c > pos && (uint16_t)(c - pos) <= (uint16_t)max_take && c > best) best = c;
    }
    return best;
}

// Places the stub into the ROM image, splitting it across as many padding runs
// as necessary and chaining the pieces with jumps. Fragments are only ever cut
// at instruction boundaries. Nothing is written to the cartridge header: only
// the INIT word (bytes 2..3) is retouched by the caller, so STATEMENT / DEVICE /
// TEXT and the reserved bytes keep their original values. Putting executable
// bytes there makes TEXT nonzero, and some BIOSes then treat it as a tokenized
// BASIC program pointer once INIT returns. Returns the address to point INIT at,
// or 0 when the stub cannot be placed.
static uint16_t __not_in_flash_func(place_boot_stub)(
    uint8_t *img,
    uint32_t limit,
    const boot_stub_t *s)
{
    if (s->len == 0u) return 0u;

    boot_cave_t caves[BOOT_STUB_MAX_CAVES];
    uint8_t count = 0;

    // A chainable run needs at least one code byte plus its 3-byte jump.
    while (count < BOOT_STUB_MAX_CAVES)
    {
        boot_cave_t c;
        if (!find_boot_cave(img, limit, caves, count, 4u, &c)) break;
        caves[count++] = c;
    }
    if (count == 0u) return 0u;

    uint8_t take[BOOT_STUB_MAX_CAVES];
    uint32_t slot[BOOT_STUB_MAX_CAVES];
    uint8_t used = 0;
    uint8_t pos = 0;

    for (uint8_t i = 0; i < count && pos < s->len; i++)
    {
        uint8_t remaining = (uint8_t)(s->len - pos);

        if ((uint32_t)remaining <= caves[i].len)
        {
            // Final fragment: the rest of the stub fits in this run.
            slot[used] = caves[i].off + caves[i].len - (uint32_t)remaining;
            take[used] = remaining;
            pos = s->len;
            used++;
            break;
        }

        uint8_t max_take = (uint8_t)(caves[i].len - 3u);
        uint8_t cut = boot_stub_next_cut(s, pos, max_take);
        if (cut == pos) continue;                    // cannot hold a whole instruction

        uint8_t t = (uint8_t)(cut - pos);
        slot[used] = caves[i].off + caves[i].len - (uint32_t)(t + 3u);
        take[used] = t;
        pos = cut;
        used++;
    }

    if (pos != s->len) return 0u;

    uint8_t idx = 0;
    for (uint8_t i = 0; i < used; i++)
    {
        for (uint8_t k = 0; k < take[i]; k++) img[slot[i] + k] = s->bytes[idx + k];
        idx = (uint8_t)(idx + take[i]);

        if (i + 1u < used)
        {
            uint16_t next = (uint16_t)(0x4000u + slot[i + 1u]);
            img[slot[i] + take[i] + 0u] = 0xC3u;                     // jp next
            img[slot[i] + take[i] + 1u] = (uint8_t)(next & 0xFFu);
            img[slot[i] + take[i] + 2u] = (uint8_t)(next >> 8);
        }
    }

    return (uint16_t)(0x4000u + slot[0]);
}

static void __not_in_flash_func(apply_boot_patches)(uint8_t *img, uint32_t cached_len)
{
    if (vdp_freq_launch == VDP_FREQ_DEFAULT && cpu_mode_launch == CPU_MODE_DEFAULT) return;
    if (cached_len < 16u || img[0] != 'A' || img[1] != 'B') return;

    uint16_t orig_init = (uint16_t)img[2] | ((uint16_t)img[3] << 8);
    if (orig_init < 0x4000u || orig_init > 0xBFFFu) return;

    uint32_t window = boot_stub_search_limit;
    uint32_t limit = (cached_len < window) ? cached_len : window;

    boot_stub_t stub;
    build_boot_stub(&stub, orig_init, false);
    uint16_t entry = place_boot_stub(img, limit, &stub);

    if (entry == 0u && vdp_freq_launch != VDP_FREQ_DEFAULT)
    {
        // Not enough padding for the full stub. Fall back to the compact
        // frequency-only form, which writes R9 outright instead of preserving
        // the register's other bits. At cartridge-INIT time the BIOS still has
        // its boot defaults loaded, so those bits are the ones we would have
        // read back anyway. The CPU option has no compact form and is dropped.
        cpu_mode_launch = CPU_MODE_DEFAULT;
        build_boot_stub(&stub, orig_init, true);
        entry = place_boot_stub(img, limit, &stub);
    }

    // Leave the ROM completely untouched when nothing fits, so choosing a
    // frequency can never stop a game from booting.
    if (entry == 0u) return;

    img[2] = (uint8_t)(entry & 0xFFu);                                   // INIT vector -> stub
    img[3] = (uint8_t)(entry >> 8);
}

static inline void __not_in_flash_func(prepare_rom_source)(
    uint32_t offset,
    bool cache_enable,
    uint32_t preferred_size,
    const uint8_t **rom_base_out,
    uint32_t *available_length_out)
{
    const uint8_t *rom_base = rom_data + offset;
    uint32_t available_length = active_rom_size;

    // For SD-loaded ROMs rom_data points at PSRAM (sd_rom_region.ptr) with
    // offset 0; for flash-resident ROMs it points at QSPI flash. In both
    // cases we copy the leading rom_cache_capacity bytes into the PSRAM ROM
    // cache, leaving the tail to be served from the source via XIP.
    if (preferred_size != 0u && (available_length == 0u || available_length > preferred_size))
    {
        available_length = preferred_size;
    }

    if (cache_enable && available_length > 0u && psram_prepare_rom_cache())
    {
        uint32_t cap = rom_cache_capacity;
        if (cap > CACHE_SIZE) cap = CACHE_SIZE;
        uint32_t bytes_to_cache = (available_length > cap) ? cap : available_length;

        gpio_init(PIN_WAIT);
        gpio_set_dir(PIN_WAIT, GPIO_OUT);
        gpio_put(PIN_WAIT, 0);

        // DMA bulk copy from flash XIP to SRAM (matches multirom). A
        // CPU memcpy() can stall or get truncated for large transfers
        // when this function runs in the PSRAM/QMI-active path.
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
        // If we cached everything, we can just point to SRAM
        if (active_rom_size <= cap)
        {
            rom_base = rom_sram;
        }

        // Per-ROM 50/60Hz and CPU speed: patch the launched game's cartridge
        // INIT so the options are applied right after the BIOS boot, when the
        // ROM's INIT runs. vdp_freq_launch / cpu_mode_launch are zeroed for
        // system ROMs so their loaders are never touched; the menu also zeroes
        // the per-ROM frequency on MSX1 (no R9), so no MSX1 guard is needed.
        apply_boot_patches(rom_sram, bytes_to_cache);

        // Release /WAIT only once the cache bookkeeping and the boot patch are
        // in place. The MSX can be stalled mid-read of the cartridge header, so
        // releasing earlier lets that read complete against the unpatched image.
        gpio_set_dir(PIN_WAIT, GPIO_IN);
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
    const uint8_t *rom_base = rom_data + offset;
    uint32_t available_length = active_rom_size;

    if (cache_enable && available_length > 0u && psram_prepare_rom_cache())
    {
        uint32_t bytes_to_cache = (available_length > CACHE_SIZE)
                                  ? CACHE_SIZE
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
        if (available_length <= CACHE_SIZE)
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

    pio_sm_config cfg_read = msx_read_responder_program_get_default_config(msx_bus.offset_read);
    sm_config_set_in_pins(&cfg_read, PIN_A0);
    sm_config_set_in_shift(&cfg_read, false, false, 16);
    sm_config_set_out_pins(&cfg_read, PIN_D0, 8);
    sm_config_set_out_shift(&cfg_read, true, false, 32);
    sm_config_set_set_pins(&cfg_read, PIN_WAIT, 1);
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

    pio_sm_set_pins_with_mask(msx_bus.pio, msx_bus.sm_read, 0u, (1u << PIN_WAIT));
    pio_gpio_init(msx_bus.pio, PIN_WAIT);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_read, PIN_WAIT, 1, false);

    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
    {
        pio_gpio_init(msx_bus.pio, pin);
    }

    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_read, PIN_D0, 8, false);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_write, PIN_D0, 8, false);

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, true);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, true);
}

static void msx_pio_io_bus_init(void)
{
    msx_io_bus.pio_read = pio2;
    msx_io_bus.pio_write = pio2;
    msx_io_bus.sm_io_read  = 0;
    msx_io_bus.sm_io_write = 1;

    if (!msx_io_read_program_loaded)
    {
        msx_io_bus.offset_io_read = pio_add_program(msx_io_bus.pio_read, &msx_io_read_responder_program);
        msx_io_read_program_loaded = true;
    }
    if (!msx_io_write_program_loaded)
    {
        msx_io_bus.offset_io_write = pio_add_program(msx_io_bus.pio_write, &msx_io_write_captor_program);
        msx_io_write_program_loaded = true;
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

static void msx_pio_io_write_bus_init(void)
{
    msx_io_bus.pio_write = pio2;
    msx_io_bus.sm_io_write = 1;

    if (!msx_io_write_program_loaded)
    {
        msx_io_bus.offset_io_write = pio_add_program(msx_io_bus.pio_write, &msx_io_write_captor_program);
        msx_io_write_program_loaded = true;
    }

    pio_sm_set_enabled(msx_io_bus.pio_write, msx_io_bus.sm_io_write, false);
    pio_sm_clear_fifos(msx_io_bus.pio_write, msx_io_bus.sm_io_write);
    pio_sm_restart(msx_io_bus.pio_write, msx_io_bus.sm_io_write);

    pio_sm_config cfg_io_write = msx_io_write_captor_program_get_default_config(msx_io_bus.offset_io_write);
    sm_config_set_in_pins(&cfg_io_write, PIN_A0);
    sm_config_set_in_shift(&cfg_io_write, false, false, 32);
    sm_config_set_fifo_join(&cfg_io_write, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_io_write, PIN_WR);
    sm_config_set_clkdiv(&cfg_io_write, 1.0f);
    pio_sm_init(msx_io_bus.pio_write, msx_io_bus.sm_io_write, msx_io_bus.offset_io_write, &cfg_io_write);
    pio_sm_set_enabled(msx_io_bus.pio_write, msx_io_bus.sm_io_write, true);
}

static inline uint8_t __not_in_flash_func(read_rom_byte)(const uint8_t *rom_base, uint32_t rel)
{
    return (rel < rom_cached_size) ? rom_sram[rel] : rom_base[rel];
}

static inline uint16_t __not_in_flash_func(pio_build_token)(bool drive, uint8_t data)
{
    uint8_t dir_mask = drive ? 0xFFu : 0x00u;
    return (uint16_t)data | ((uint16_t)dir_mask << 8);
}

static inline bool __not_in_flash_func(pio_try_get_write)(uint16_t *addr_out, uint8_t *data_out)
{
    if (pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_write))
        return false;

    uint32_t sample = pio_sm_get(msx_bus.pio, msx_bus.sm_write);
    *addr_out = (uint16_t)(sample & 0xFFFFu);
    *data_out = (uint8_t)((sample >> 16) & 0xFFu);
    return true;
}

static inline bool __not_in_flash_func(pio_try_get_io_write)(uint16_t *addr_out, uint8_t *data_out)
{
    if (pio_sm_is_rx_fifo_empty(msx_io_bus.pio_write, msx_io_bus.sm_io_write))
        return false;

    uint32_t sample = pio_sm_get(msx_io_bus.pio_write, msx_io_bus.sm_io_write);
    *addr_out = (uint16_t)(sample & 0xFFFFu);
    *data_out = (uint8_t)((sample >> 16) & 0xFFu);
    return true;
}

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

static inline int32_t __not_in_flash_func(scale_sample_percent_i32)(int32_t sample, uint8_t percent)
{
    return (int32_t)(((int64_t)sample * percent) / 100);
}

static inline int16_t __not_in_flash_func(apply_audio_volume)(int32_t sample)
{
    return clamp_i16(scale_sample_percent_i32(sample, ctrl_audio_volume));
}

static void __not_in_flash_func(dual_psg_init)(void)
{
    if (dual_psg_ready)
        return;

    if (!dual_psg_lock)
        dual_psg_lock = spin_lock_init(spin_lock_claim_unused(true));

    memset(&psg_instance, 0, sizeof(psg_instance));
    psg_instance.rate = PSG_SAMPLE_RATE;
    PSG_setVolumeMode(&psg_instance, 2);
    PSG_setClock(&psg_instance, PSG_CLOCK);
    PSG_setQuality(&psg_instance, 1);
    PSG_reset(&psg_instance);
    msx_pio_io_bus_init();
    dual_psg_ready = true;
}

static inline void __not_in_flash_func(dual_psg_write_ring_push)(uint8_t port, uint8_t data)
{
    uint32_t head = dual_psg_write_ring_head;
    uint32_t next = (head + 1u) & DUAL_PSG_WRITE_RING_MASK;
    if (next == dual_psg_write_ring_tail)
        return; // ring full: drop write (audio only; mapper state unaffected)
    dual_psg_write_ring[head] = (uint16_t)(((uint16_t)port << 8) | data);
    __dmb();
    dual_psg_write_ring_head = next;
}

static inline void __not_in_flash_func(dual_psg_drain_write_ring)(void)
{
    if (!dual_psg_ready)
        return;
    uint32_t tail = dual_psg_write_ring_tail;
    while (tail != dual_psg_write_ring_head)
    {
        __dmb();
        uint16_t entry = dual_psg_write_ring[tail];
        uint8_t port = (uint8_t)(entry >> 8);
        uint8_t data = (uint8_t)(entry & 0xFFu);
        uint32_t save = spin_lock_blocking(dual_psg_lock);
        PSG_writeIO(&psg_instance, port, data);
        spin_unlock(dual_psg_lock, save);
        tail = (tail + 1u) & DUAL_PSG_WRITE_RING_MASK;
    }
    dual_psg_write_ring_tail = tail;
}

static inline bool __not_in_flash_func(dual_psg_handle_io_write)(uint8_t port, uint8_t data)
{
    if (!dual_psg_ready)
        return false;
    if (port == PSG_PORT_REG || port == PSG_PORT_DATA)
    {
        if (!dual_psg_core1_services_io)
        {
            // core0 owns the MSX I/O write FIFO (mapper mode): defer to core1
            // through the lock-free ring instead of taking dual_psg_lock.
            dual_psg_write_ring_push(port, data);
            return true;
        }
        uint32_t save = spin_lock_blocking(dual_psg_lock);
        PSG_writeIO(&psg_instance, port, data);
        spin_unlock(dual_psg_lock, save);
        return true;
    }
    return false;
}

static void __not_in_flash_func(main_psg_init_with_io)(uint8_t quality, bool init_io_bus)
{
    if (main_psg_ready)
    {
        PSG_setQuality(&main_psg_instance, quality);
        return;
    }

    if (!main_psg_lock)
        main_psg_lock = spin_lock_init(spin_lock_claim_unused(true));

    memset(&main_psg_instance, 0, sizeof(main_psg_instance));
    main_psg_instance.rate = PSG_SAMPLE_RATE;
    PSG_setVolumeMode(&main_psg_instance, 2);
    PSG_setClock(&main_psg_instance, PSG_CLOCK);
    PSG_setQuality(&main_psg_instance, quality);
    PSG_reset(&main_psg_instance);
    if (init_io_bus)
        msx_pio_io_bus_init();
    main_psg_ready = true;
}

static void __not_in_flash_func(main_psg_init)(uint8_t quality)
{
    main_psg_init_with_io(quality, true);
}

static inline void __not_in_flash_func(main_psg_write_ring_push)(uint8_t port, uint8_t data)
{
    uint32_t head = main_psg_write_ring_head;
    uint32_t next = (head + 1u) & MAIN_PSG_WRITE_RING_MASK;
    if (next == main_psg_write_ring_tail)
        return; // ring full: drop PSG write (audio only; mapper state unaffected)
    main_psg_write_ring[head] = (uint16_t)(((uint16_t)port << 8) | data);
    __dmb();
    main_psg_write_ring_head = next;
}

static inline void __not_in_flash_func(main_psg_drain_write_ring)(void)
{
    uint32_t tail = main_psg_write_ring_tail;
    while (tail != main_psg_write_ring_head)
    {
        __dmb();
        uint16_t entry = main_psg_write_ring[tail];
        uint8_t port = (uint8_t)(entry >> 8);
        uint8_t data = (uint8_t)(entry & 0xFFu);
        uint32_t save = spin_lock_blocking(main_psg_lock);
        PSG_writeIO(&main_psg_instance, port, data);
        spin_unlock(main_psg_lock, save);
        tail = (tail + 1u) & MAIN_PSG_WRITE_RING_MASK;
    }
    main_psg_write_ring_tail = tail;
}

static inline bool __not_in_flash_func(main_psg_handle_io_write)(uint8_t port, uint8_t data)
{
    if (!main_psg_ready)
        return false;
    if (port == MAIN_PSG_PORT_REG || port == MAIN_PSG_PORT_DATA)
    {
        if (!main_psg_core1_services_io)
        {
            // Called from core0 while it owns the MSX I/O write FIFO (mapper mode).
            // Hand the write off to core1 through the lock-free ring instead of
            // taking main_psg_lock, so core0 never stalls and drops mapper writes.
            main_psg_write_ring_push(port, data);
            return true;
        }
        uint32_t save = spin_lock_blocking(main_psg_lock);
        PSG_writeIO(&main_psg_instance, port, data);
        spin_unlock(main_psg_lock, save);
        return true;
    }
    return false;
}

static inline void __not_in_flash_func(main_psg_service_io)(void)
{
    if (!main_psg_ready)
        return;

    if (!main_psg_core1_services_io)
    {
        // core0 owns the MSX I/O write FIFO (mapper mode); apply the PSG writes
        // it queued for us here on core1, next to audio generation.
        main_psg_drain_write_ring();
        return;
    }

    uint16_t io_addr;
    uint8_t io_data;
    while (pio_try_get_io_write(&io_addr, &io_data))
        main_psg_handle_io_write((uint8_t)(io_addr & 0xFFu), io_data);

    while (pio_try_get_io_read(&io_addr))
    {
        pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read,
                            pio_build_token(false, 0xFFu));
    }
}

static inline void __not_in_flash_func(dual_psg_service_io)(void)
{
    if (!dual_psg_ready)
        return;

    if (!dual_psg_core1_services_io)
    {
        // core0 owns the MSX I/O write FIFO (mapper mode); apply the writes it
        // queued for the mirrored PSG and the secondary PSG here on core1.
        main_psg_drain_write_ring();
        dual_psg_drain_write_ring();
        return;
    }

    uint16_t io_addr;
    uint8_t io_data;
    while (pio_try_get_io_write(&io_addr, &io_data))
    {
        uint8_t port = io_addr & 0xFFu;
        if (main_psg_handle_io_write(port, io_data))
            continue;
        (void)dual_psg_handle_io_write(port, io_data);
    }

    while (pio_try_get_io_read(&io_addr))
    {
        pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read,
                            pio_build_token(false, 0xFFu));
    }
}

static inline int16_t __not_in_flash_func(dual_psg_calc_sample)(void)
{
    if (!dual_psg_ready)
        return 0;

    uint32_t save = spin_lock_blocking(dual_psg_lock);
    int16_t sample = clamp_i16((int32_t)PSG_calc(&psg_instance) << PSG_VOLUME_SHIFT);
    spin_unlock(dual_psg_lock, save);
    return sample;
}

static inline int16_t __not_in_flash_func(main_psg_calc_sample_shifted)(uint8_t volume_shift)
{
    if (!main_psg_ready)
        return 0;

    uint32_t save = spin_lock_blocking(main_psg_lock);
    int16_t sample = clamp_i16((int32_t)PSG_calc(&main_psg_instance) << volume_shift);
    spin_unlock(main_psg_lock, save);
    return sample;
}

static inline int16_t __not_in_flash_func(main_psg_calc_sample)(void)
{
    return main_psg_calc_sample_shifted(PSG_VOLUME_SHIFT);
}

static int16_t __not_in_flash_func(wavegame_psg_calc_sample)(void)
{
    if (!wavegame_psg_mirror_active)
        return 0;
    main_psg_drain_write_ring();
    return apply_audio_volume(main_psg_calc_sample());
}

static void __not_in_flash_func(wavegame_psg_set_sample_rate)(uint32_t sample_rate)
{
    if (!wavegame_psg_mirror_active || !main_psg_ready || sample_rate == 0)
        return;
    uint32_t save = spin_lock_blocking(main_psg_lock);
    PSG_setRate(&main_psg_instance, sample_rate);
    spin_unlock(main_psg_lock, save);
}

static inline bool __not_in_flash_func(main_psg_has_audible_channels_unlocked)(void)
{
    uint8_t mixer = main_psg_instance.reg[7];
    for (uint8_t channel = 0; channel < 3; channel++)
    {
        uint8_t volume = main_psg_instance.reg[8 + channel] & 0x1Fu;
        if ((volume & 0x10u) != 0u)
        {
            if (main_psg_instance.env_freq == 0u)
                continue;
        }
        else if ((volume & 0x0Fu) == 0u)
        {
            continue;
        }

        bool tone_enabled = (mixer & (1u << channel)) == 0u;
        bool noise_enabled = (mixer & (1u << (channel + 3u))) == 0u;
        if (tone_enabled || noise_enabled)
            return true;
    }
    return false;
}

static inline bool __not_in_flash_func(main_psg_calc_audible_sample_shifted)(uint8_t volume_shift, int16_t *sample)
{
    if (!main_psg_ready)
        return false;

    uint32_t save = spin_lock_blocking(main_psg_lock);
    bool audible = main_psg_has_audible_channels_unlocked();
    if (audible)
        *sample = clamp_i16((int32_t)PSG_calc(&main_psg_instance) << volume_shift);
    spin_unlock(main_psg_lock, save);
    return audible;
}

static inline void __not_in_flash_func(dual_psg_write_stereo_sample)(int16_t *samples, int index)
{
    int16_t secondary = dual_psg_calc_sample();
    if (ctrl_psg_emulation != 0u && main_psg_ready)
    {
        samples[index * 2] = apply_audio_volume(main_psg_calc_sample());
        samples[index * 2 + 1] = apply_audio_volume(secondary);
        return;
    }

    int16_t sample = clamp_i16((int32_t)secondary + main_psg_calc_sample());
    sample = apply_audio_volume(sample);
    samples[index * 2] = sample;
    samples[index * 2 + 1] = sample;
}

static void __no_inline_not_in_flash_func(core1_dual_psg_audio)(void)
{
    while (true)
    {
        while (true)
        {
            dual_psg_service_io();
            struct audio_buffer *buffer = take_audio_buffer(dual_psg_audio_pool, false);
            if (buffer)
            {
                int16_t *samples = (int16_t *)buffer->buffer->bytes;
                for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
                {
                    dual_psg_service_io();
                    dual_psg_write_stereo_sample(samples, i);
                }
                buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
                give_audio_buffer(dual_psg_audio_pool, buffer);
                break;
            }
            tight_loop_contents();
        }
    }
}

static void dual_psg_audio_init(void)
{
    if (dual_psg_audio_started)
        return;

    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 1);

    struct audio_buffer_pool *handoff_pool = claim_rom_audio_handoff_pool();
    if (handoff_pool)
    {
        dual_psg_audio_pool = handoff_pool;
        dual_psg_audio_started = true;
        return;
    }

    static audio_format_t dual_psg_audio_format = {
        .sample_freq = PSG_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };

    static struct audio_buffer_format dual_psg_producer_format = {
        .format = &dual_psg_audio_format,
        .sample_stride = 4,
    };

    dual_psg_audio_pool = audio_new_producer_pool(&dual_psg_producer_format, 3, SCC_AUDIO_BUFFER_SAMPLES);

    int dma_channel = -1;
    for (int ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (!dma_channel_is_claimed(ch)) {
            dma_channel = ch;
            break;
        }
    }
    if (dma_channel < 0) {
        dual_psg_audio_pool = NULL;
        return;
    }

    static struct audio_i2s_config dual_psg_i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_BCLK_PIN,
        .dma_channel = 0,
        .pio_sm = 2,
    };
    dual_psg_i2s_config.dma_channel = (uint)dma_channel;

    audio_i2s_setup(&dual_psg_audio_format, &dual_psg_i2s_config);
    audio_i2s_connect(dual_psg_audio_pool);
    audio_i2s_set_enabled(true);
    gpio_put(I2S_MUTE_PIN, 0);
    dual_psg_audio_started = true;
}

static void start_dual_psg_audio(void)
{
    dual_psg_init();
    dual_psg_core1_services_io = true;
    dual_psg_audio_init();
    if (dual_psg_audio_pool)
        multicore_launch_core1(core1_dual_psg_audio);
}

static void dual_psg_audio_init_for_system(bool service_io_on_core1)
{
    dual_psg_init();
    dual_psg_core1_services_io = service_io_on_core1;
    if (ctrl_psg_emulation != 0u)
    {
        main_psg_init(PSG_QUALITY_HIGH);
        main_psg_core1_services_io = service_io_on_core1;
    }
    dual_psg_audio_init();
}

static inline void __not_in_flash_func(dual_psg_audio_service_buffer)(void)
{
    if (!dual_psg_audio_started || !dual_psg_audio_pool)
        return;

    struct audio_buffer *buffer = take_audio_buffer(dual_psg_audio_pool, false);
    if (!buffer)
        return;

    int16_t *samples = (int16_t *)buffer->buffer->bytes;
    for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
    {
        dual_psg_service_io();
        dual_psg_write_stereo_sample(samples, i);
    }
    buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
    give_audio_buffer(dual_psg_audio_pool, buffer);
}

static void __no_inline_not_in_flash_func(core1_main_psg_audio)(void)
{
    while (true)
    {
        while (true)
        {
            main_psg_service_io();
            struct audio_buffer *buffer = take_audio_buffer(main_psg_audio_pool, false);
            if (buffer)
            {
                int16_t *samples = (int16_t *)buffer->buffer->bytes;
                for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
                {
                    main_psg_service_io();
                    int16_t sample = apply_audio_volume(main_psg_calc_sample());
                    samples[i * 2] = sample;
                    samples[i * 2 + 1] = sample;
                }
                buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
                give_audio_buffer(main_psg_audio_pool, buffer);
                break;
            }
            tight_loop_contents();
        }
    }
}

static void main_psg_audio_init(void)
{
    if (main_psg_audio_started)
        return;

    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 1);

    struct audio_buffer_pool *handoff_pool = claim_rom_audio_handoff_pool();
    if (handoff_pool)
    {
        main_psg_audio_pool = handoff_pool;
        main_psg_audio_started = true;
        return;
    }

    static audio_format_t main_psg_audio_format = {
        .sample_freq = PSG_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };

    static struct audio_buffer_format main_psg_producer_format = {
        .format = &main_psg_audio_format,
        .sample_stride = 4,
    };

    main_psg_audio_pool = audio_new_producer_pool(&main_psg_producer_format, 3, SCC_AUDIO_BUFFER_SAMPLES);

    int dma_channel = -1;
    for (int ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (!dma_channel_is_claimed(ch)) {
            dma_channel = ch;
            break;
        }
    }
    if (dma_channel < 0) {
        main_psg_audio_pool = NULL;
        return;
    }

    static struct audio_i2s_config main_psg_i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_BCLK_PIN,
        .dma_channel = 0,
        .pio_sm = 2,
    };
    main_psg_i2s_config.dma_channel = (uint)dma_channel;

    audio_i2s_setup(&main_psg_audio_format, &main_psg_i2s_config);
    audio_i2s_connect(main_psg_audio_pool);
    audio_i2s_set_enabled(true);
    gpio_put(I2S_MUTE_PIN, 0);
    main_psg_audio_started = true;
}

static void main_psg_audio_init_for_system(bool service_io_on_core1)
{
    main_psg_init(PSG_QUALITY_HIGH);
    main_psg_core1_services_io = service_io_on_core1;
    main_psg_audio_init();
}

static inline void __not_in_flash_func(main_psg_audio_service_buffer)(void)
{
    if (!main_psg_audio_started || !main_psg_audio_pool)
        return;

    struct audio_buffer *buffer = take_audio_buffer(main_psg_audio_pool, false);
    if (!buffer)
        return;

    int16_t *samples = (int16_t *)buffer->buffer->bytes;
    for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
    {
        main_psg_service_io();
        int16_t sample = apply_audio_volume(main_psg_calc_sample());
        samples[i * 2] = sample;
        samples[i * 2 + 1] = sample;
    }
    buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
    give_audio_buffer(main_psg_audio_pool, buffer);
}

static void start_main_psg_audio(bool service_io_on_core1)
{
    main_psg_audio_init_for_system(service_io_on_core1);
    if (main_psg_audio_pool)
        multicore_launch_core1(core1_main_psg_audio);
}

static system_audio_profile_t system_audio_resolve_profile(void)
{
    if (ctrl_audio_selection == AUDIO_PROFILE_MSX_MUSIC)
        return SYSTEM_AUDIO_PROFILE_MSX_MUSIC;
    if (ctrl_audio_selection == AUDIO_PROFILE_DUAL_PSG)
        return SYSTEM_AUDIO_PROFILE_DUAL_PSG;
    if (ctrl_audio_selection == AUDIO_PROFILE_YM2151_SFG05 || ctrl_audio_selection == AUDIO_PROFILE_YM2151_SFG05_4MHZ)
        return SYSTEM_AUDIO_PROFILE_YM2151_SFG05;
    if (ctrl_audio_selection == AUDIO_PROFILE_YM2151_SFG01 || ctrl_audio_selection == AUDIO_PROFILE_YM2151_SFG01_4MHZ)
        return SYSTEM_AUDIO_PROFILE_YM2151_SFG01;
    if (ctrl_audio_selection == AUDIO_PROFILE_SCC_EXTERNAL)
        return SYSTEM_AUDIO_PROFILE_SCC_EXTERNAL;
    if (ctrl_audio_selection == AUDIO_PROFILE_SCC_PLUS_EXTERNAL)
        return SYSTEM_AUDIO_PROFILE_SCC_PLUS_EXTERNAL;
    if (ctrl_audio_selection == AUDIO_PROFILE_MEGARAM_SCC)
        return SYSTEM_AUDIO_PROFILE_SCC_EXTERNAL;
    if (ctrl_audio_selection == AUDIO_PROFILE_MEGARAM_SCC_PLUS)
        return SYSTEM_AUDIO_PROFILE_SCC_PLUS_EXTERNAL;
    if (ctrl_psg_emulation != 0u)
        return SYSTEM_AUDIO_PROFILE_MAIN_PSG;
    return SYSTEM_AUDIO_PROFILE_NONE;
}

static void system_audio_init_for_sunrise(bool service_io_on_core1)
{
    system_audio_profile = system_audio_resolve_profile();
    switch (system_audio_profile)
    {
    case SYSTEM_AUDIO_PROFILE_MSX_MUSIC:
        msx_music_init();
        msx_music_core1_services_io = service_io_on_core1;
        if (ctrl_psg_emulation != 0u)
        {
            main_psg_init(PSG_QUALITY_FAST);
            main_psg_core1_services_io = service_io_on_core1;
        }
        msx_music_audio_init();
        break;
    case SYSTEM_AUDIO_PROFILE_DUAL_PSG:
        dual_psg_audio_init_for_system(service_io_on_core1);
        break;
    case SYSTEM_AUDIO_PROFILE_YM2151_SFG05:
    case SYSTEM_AUDIO_PROFILE_YM2151_SFG01:
        ym2151_init(system_audio_profile == SYSTEM_AUDIO_PROFILE_YM2151_SFG01 ? YM2151_SFG01 : YM2151_SFG05);
        if (ctrl_psg_emulation != 0u)
        {
            main_psg_init(PSG_QUALITY_HIGH);
            main_psg_core1_services_io = service_io_on_core1;
        }
        ym2151_audio_init();
        break;
    case SYSTEM_AUDIO_PROFILE_SCC_EXTERNAL:
    case SYSTEM_AUDIO_PROFILE_SCC_PLUS_EXTERNAL:
        scc_audio_init_for_type(system_audio_profile == SYSTEM_AUDIO_PROFILE_SCC_PLUS_EXTERNAL ? SCC_ENHANCED : SCC_STANDARD);
        if (ctrl_psg_emulation != 0u)
        {
            main_psg_init(PSG_QUALITY_HIGH);
            main_psg_core1_services_io = service_io_on_core1;
        }
        i2s_audio_init_scc();
        break;
    case SYSTEM_AUDIO_PROFILE_MAIN_PSG:
        main_psg_audio_init_for_system(service_io_on_core1);
        break;
    default:
        break;
    }
}

static inline bool __not_in_flash_func(system_audio_handle_io_write)(uint8_t port, uint8_t data)
{
    switch (system_audio_profile)
    {
    case SYSTEM_AUDIO_PROFILE_MSX_MUSIC:
        if (main_psg_handle_io_write(port, data))
            return true;
        if (port == MSX_MUSIC_PORT_REG || port == MSX_MUSIC_PORT_DATA)
        {
            msx_music_write_io(port, data);
            return true;
        }
        return false;
    case SYSTEM_AUDIO_PROFILE_DUAL_PSG:
        if (main_psg_handle_io_write(port, data))
            return true;
        return dual_psg_handle_io_write(port, data);
    case SYSTEM_AUDIO_PROFILE_MAIN_PSG:
        return main_psg_handle_io_write(port, data);
    case SYSTEM_AUDIO_PROFILE_YM2151_SFG05:
    case SYSTEM_AUDIO_PROFILE_YM2151_SFG01:
        return main_psg_handle_io_write(port, data);
    default:
        return false;
    }
}

static inline bool __not_in_flash_func(system_audio_handle_io_read)(uint8_t port, uint8_t *data)
{
    (void)port;
    (void)data;
    return false;
}

static inline void __not_in_flash_func(system_audio_service_core1)(void)
{
    switch (system_audio_profile)
    {
    case SYSTEM_AUDIO_PROFILE_MSX_MUSIC:
        msx_music_audio_service_buffer();
        break;
    case SYSTEM_AUDIO_PROFILE_DUAL_PSG:
        dual_psg_service_io();
        dual_psg_audio_service_buffer();
        break;
    case SYSTEM_AUDIO_PROFILE_MAIN_PSG:
        main_psg_service_io();
        main_psg_audio_service_buffer();
        break;
    case SYSTEM_AUDIO_PROFILE_YM2151_SFG05:
    case SYSTEM_AUDIO_PROFILE_YM2151_SFG01:
        // service_psg_io drains the primary-PSG ring core0 queued in mapper mode
        // (no-op when PSG Mirror is off), keeping mirrored PSG audio alive.
        ym2151_audio_service_buffer(true);
        break;
    case SYSTEM_AUDIO_PROFILE_SCC_EXTERNAL:
    case SYSTEM_AUDIO_PROFILE_SCC_PLUS_EXTERNAL:
        main_psg_service_io();
        scc_audio_service_buffer();
        break;
    default:
        break;
    }
}

static void __not_in_flash_func(msx_music_init)(void)
{
    debug_trace("DBG music_init enter ready=%u opll=%p lock=%p", msx_music_ready ? 1u : 0u,
                (void *)msx_music_instance, (void *)msx_music_lock);
    if (msx_music_ready)
        return;

    if (!msx_music_instance) {
        debug_trace("DBG music_init alloc");
        msx_music_instance = OPLL_new(MSX_MUSIC_CLOCK, MSX_MUSIC_SAMPLE_RATE);
    }
    debug_trace("DBG music_init allocated=%p", (void *)msx_music_instance);
    if (!msx_music_instance)
        return;

    if (!msx_music_lock) {
        debug_trace("DBG music_init lock claim");
        msx_music_lock = spin_lock_instance(spin_lock_claim_unused(true));
    }
    debug_trace("DBG music_init lock=%p", (void *)msx_music_lock);

    debug_trace("DBG music_init reset");
    OPLL_reset(msx_music_instance);
    OPLL_setChipType(msx_music_instance, OPLL_2413_TONE);
    OPLL_resetPatch(msx_music_instance, OPLL_2413_TONE);
    debug_trace("DBG music_init pio");
    msx_pio_io_bus_init();
    msx_music_ready = true;
    debug_trace("DBG music_init done");
}

/* Output conditioning to narrow the quality gap with openMSX, whose final mix
 * passes through a DC blocker plus output filtering. We do the same on the raw
 * OPLL sample (before gain/limiting): a ~35 Hz DC blocker removes any constant
 * offset that would otherwise add a steady hiss/thump through the DAC, and a
 * gentle 1-pole low-pass (~16 kHz) softens the YM2413 exponential-DAC
 * quantization noise without dulling the music. */
static float msx_music_dc_x1 = 0.0f;   /* DC blocker previous input  */
static float msx_music_dc_y1 = 0.0f;   /* DC blocker previous output */
static float msx_music_lp_y1 = 0.0f;   /* low-pass previous output   */
#define MSX_MUSIC_DC_R   0.995f         /* DC blocker pole (~35 Hz cutoff) */
#define MSX_MUSIC_LP_A   0.70f          /* 1-pole LPF coeff (~16 kHz cutoff) */

static inline void msx_music_reset_filters(void)
{
    msx_music_dc_x1 = 0.0f;
    msx_music_dc_y1 = 0.0f;
    msx_music_lp_y1 = 0.0f;
}

static inline int32_t __not_in_flash_func(msx_music_filter_sample)(int16_t in)
{
    float x = (float)in;
    /* DC blocker: y = x - x1 + R*y1 */
    float dc = x - msx_music_dc_x1 + MSX_MUSIC_DC_R * msx_music_dc_y1;
    msx_music_dc_x1 = x;
    msx_music_dc_y1 = dc;
    /* 1-pole low-pass: y += A*(x - y) */
    msx_music_lp_y1 += MSX_MUSIC_LP_A * (dc - msx_music_lp_y1);
    return (int32_t)msx_music_lp_y1;
}

static inline int16_t __not_in_flash_func(msx_music_soft_limit)(int32_t g)
{
    int32_t mag = (g < 0) ? -g : g;
    if (mag <= MSX_MUSIC_LIMIT_KNEE)
        return (int16_t)g;

    const int32_t range = MSX_MUSIC_LIMIT_CEIL - MSX_MUSIC_LIMIT_KNEE;
    int32_t excess = mag - MSX_MUSIC_LIMIT_KNEE;
    int32_t compressed = (int32_t)(((int64_t)range * excess) / (range + excess));
    int32_t out = MSX_MUSIC_LIMIT_KNEE + compressed;
    if (out > MSX_MUSIC_LIMIT_CEIL)
        out = MSX_MUSIC_LIMIT_CEIL;
    return (int16_t)((g < 0) ? -out : out);
}

static inline int16_t __not_in_flash_func(msx_music_calc_sample)(void)
{
    if (!msx_music_ready)
        return 0;

    uint32_t save = spin_lock_blocking(msx_music_lock);
    int16_t sample = OPLL_calc(msx_music_instance);
    spin_unlock(msx_music_lock, save);
    int32_t filtered = msx_music_filter_sample(sample);
    return msx_music_soft_limit(filtered << MSX_MUSIC_VOLUME_SHIFT);
}

static inline void __not_in_flash_func(msx_music_write_ring_push)(uint8_t port, uint8_t data)
{
    uint32_t head = msx_music_write_ring_head;
    uint32_t next = (head + 1u) & MSX_MUSIC_WRITE_RING_MASK;
    if (next == msx_music_write_ring_tail)
        return; // ring full: drop write (audio only; mapper state unaffected)
    msx_music_write_ring[head] = (uint16_t)(((uint16_t)port << 8) | data);
    __dmb();
    msx_music_write_ring_head = next;
}

static inline void __not_in_flash_func(msx_music_drain_write_ring)(void)
{
    if (!msx_music_ready)
        return;
    uint32_t tail = msx_music_write_ring_tail;
    while (tail != msx_music_write_ring_head)
    {
        __dmb();
        uint16_t entry = msx_music_write_ring[tail];
        uint8_t port = (uint8_t)(entry >> 8);
        uint8_t data = (uint8_t)(entry & 0xFFu);
        uint32_t save = spin_lock_blocking(msx_music_lock);
        OPLL_writeIO(msx_music_instance, port, data);
        spin_unlock(msx_music_lock, save);
        tail = (tail + 1u) & MSX_MUSIC_WRITE_RING_MASK;
    }
    msx_music_write_ring_tail = tail;
}

static inline void __not_in_flash_func(msx_music_write_io)(uint8_t port, uint8_t data)
{
    if (!msx_music_ready)
        return;

    if (!msx_music_core1_services_io)
    {
        // core0 owns the MSX I/O write FIFO (mapper mode): defer to core1 through
        // the lock-free ring instead of taking msx_music_lock. This also covers
        // the FM-PAC memory-mapped register writes (0x7FF4/0x7FF5) dispatched
        // from core0 in the Sunrise + FM-PAC loaders.
        msx_music_write_ring_push(port, data);
        return;
    }

    uint32_t save = spin_lock_blocking(msx_music_lock);
    OPLL_writeIO(msx_music_instance, port, data);
    spin_unlock(msx_music_lock, save);
}

static inline void __not_in_flash_func(msx_music_service_io)(void)
{
    if (!msx_music_ready)
        return;

    if (!msx_music_core1_services_io)
    {
        // core0 owns the MSX I/O write FIFO (mapper mode); apply the writes it
        // queued for the mirrored PSG and the YM2413 here on core1.
        main_psg_drain_write_ring();
        msx_music_drain_write_ring();
        return;
    }

    uint16_t io_addr;
    uint8_t io_data;
    while (pio_try_get_io_write(&io_addr, &io_data))
    {
        uint8_t port = io_addr & 0xFFu;
        if (main_psg_handle_io_write(port, io_data))
            continue;
        if (port == MSX_MUSIC_PORT_REG || port == MSX_MUSIC_PORT_DATA)
            msx_music_write_io(port, io_data);
    }

    while (pio_try_get_io_read(&io_addr))
    {
        pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read,
                            pio_build_token(false, 0xFFu));
    }
}

static inline bool __not_in_flash_func(msx_music_calc_psg_sample)(int16_t *sample)
{
    int16_t psg_sample = 0;
    if (!main_psg_calc_audible_sample_shifted(MSX_MUSIC_PSG_VOLUME_SHIFT, &psg_sample))
    {
        *sample = 0;
        return false;
    }

    *sample = psg_sample;
    return true;
}

static inline void __not_in_flash_func(msx_music_write_stereo_sample)(int16_t *samples, int index)
{
    int16_t music = msx_music_calc_sample();
    msx_music_service_io();
    int16_t psg = 0;
    if (msx_music_calc_psg_sample(&psg))
    {
        samples[index * 2] = apply_audio_volume(music);
        samples[index * 2 + 1] = apply_audio_volume(psg);
        return;
    }

    music = apply_audio_volume(music);
    samples[index * 2] = music;
    samples[index * 2 + 1] = music;
}

static void __no_inline_not_in_flash_func(core1_msx_music_audio)(void)
{
    msx_music_reset_filters();
    while (true)
    {
        while (true)
        {
            msx_music_service_io();
            struct audio_buffer *buffer = take_audio_buffer(msx_music_audio_pool, false);
            if (buffer)
            {
                int16_t *samples = (int16_t *)buffer->buffer->bytes;
                for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
                {
                    msx_music_service_io();
                    msx_music_write_stereo_sample(samples, i);
                }
                buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
                give_audio_buffer(msx_music_audio_pool, buffer);
                break;
            }
            tight_loop_contents();
        }
    }
}

static void msx_music_audio_init(void)
{
    debug_trace("DBG music_audio_init started=%u", msx_music_audio_started ? 1u : 0u);
    if (msx_music_audio_started)
        return;

    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 1);

    struct audio_buffer_pool *handoff_pool = claim_rom_audio_handoff_pool();
    if (handoff_pool)
    {
        debug_trace("DBG music_audio_init handoff pool");
        msx_music_audio_pool = handoff_pool;
        msx_music_audio_started = true;
        return;
    }

    static audio_format_t msx_music_audio_format = {
        .sample_freq = MSX_MUSIC_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };

    static struct audio_buffer_format msx_music_producer_format = {
        .format = &msx_music_audio_format,
        .sample_stride = 4,
    };

    msx_music_audio_pool = audio_new_producer_pool(&msx_music_producer_format, 3, SCC_AUDIO_BUFFER_SAMPLES);

    int dma_channel = -1;
    for (int ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (!dma_channel_is_claimed(ch)) {
            dma_channel = ch;
            break;
        }
    }
    if (dma_channel < 0) {
        debug_trace("DBG music_audio_init no dma");
        msx_music_audio_pool = NULL;
        return;
    }

    static struct audio_i2s_config msx_music_i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_BCLK_PIN,
        .dma_channel = 0,
        .pio_sm = 2,
    };
    msx_music_i2s_config.dma_channel = (uint)dma_channel;

    debug_trace("DBG music_audio_init setup dma=%d", dma_channel);
    audio_i2s_setup(&msx_music_audio_format, &msx_music_i2s_config);
    audio_i2s_connect(msx_music_audio_pool);
    audio_i2s_set_enabled(true);
    gpio_put(I2S_MUTE_PIN, 0);
    msx_music_audio_started = true;
}

static void start_msx_music_audio(void)
{
    debug_trace("DBG start_music init");
    msx_music_init();
    debug_trace("DBG start_music ready=%u", msx_music_ready ? 1u : 0u);
}

static void start_msx_music_audio_output(void)
{
    debug_trace("DBG start_music_output init");
    msx_music_core1_services_io = true;
    msx_music_audio_init();
    debug_trace("DBG start_music_output pool=%p", (void *)msx_music_audio_pool);
    if (msx_music_audio_pool) {
        debug_trace("DBG start_music_output launch core1");
        multicore_launch_core1(core1_msx_music_audio);
    }
}

static inline void __not_in_flash_func(msx_music_audio_service_buffer)(void)
{
    if (!msx_music_audio_started || !msx_music_audio_pool)
        return;

    struct audio_buffer *buffer = take_audio_buffer(msx_music_audio_pool, false);
    if (!buffer)
        return;

    int16_t *samples = (int16_t *)buffer->buffer->bytes;
    for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
    {
        msx_music_service_io();
        msx_music_write_stereo_sample(samples, i);
    }
    buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
    give_audio_buffer(msx_music_audio_pool, buffer);
}

static void ym2151_init(ym2151_sfg_variant_t variant)
{
    if (!ym2151_lock)
        ym2151_lock = spin_lock_init(spin_lock_claim_unused(true));

    // The SFG cartridges feed the OPM/OPP the 3.579545 MHz MSX clock, but the
    // arcade boards most YM2151 music was written for run the chip at 4 MHz.
    // The whole chip (pitch, envelopes, LFO and timers) is derived from that
    // clock, so the 4 MHz profiles only need to raise the rate at which native
    // chip frames are generated from the fixed 44.1 kHz output stream.
    bool clock_4mhz = (ctrl_audio_selection == AUDIO_PROFILE_YM2151_SFG05_4MHZ ||
                       ctrl_audio_selection == AUDIO_PROFILE_YM2151_SFG01_4MHZ);

    uint32_t save = spin_lock_blocking(ym2151_lock);
    OPM_Reset(&ym2151_instance, variant == YM2151_SFG05 ? opm_flags_ym2164 : opm_flags_none);
    ym2151_clock_hz = clock_4mhz ? YM2151_CLOCK_4MHZ : YM2151_CLOCK;
    ym2151_clock_accum = 0;
    ym2151_last_output[0] = 0;
    ym2151_last_output[1] = 0;
    ym2151_status_latch = 0;
    ym2151_address_latch = 0;
    ym2151_write_queue_head = 0;
    ym2151_write_queue_tail = 0;
    ym2151_write_queue_overflow = false;
    ym2151_irq_vector = 0xFFu;
    ym2151_midi_irq_vector = 0xFFu;
    ym2151_midi_status = 0x03u;
    ym2151_ready = true;
    spin_unlock(ym2151_lock, save);
}

static inline void __not_in_flash_func(ym2151_clock_cycles_locked)(uint32_t cycles)
{
    for (uint32_t i = 0; i < cycles; ++i)
    {
        OPM_Clock(&ym2151_instance, ym2151_last_output, NULL, NULL, NULL);
    }
}

static inline void __not_in_flash_func(ym2151_clock_frame_locked)(void)
{
    ym2151_clock_cycles_locked(YM2151_CYCLES_PER_FRAME);
}

static inline void __not_in_flash_func(ym2151_queue_register_write_locked)(uint8_t reg, uint8_t data)
{
    uint16_t next_head = (uint16_t)((ym2151_write_queue_head + 1u) & YM2151_WRITE_QUEUE_MASK);
    if (next_head == ym2151_write_queue_tail)
    {
        ym2151_write_queue_tail = (uint16_t)((ym2151_write_queue_tail + 1u) & YM2151_WRITE_QUEUE_MASK);
        ym2151_write_queue_overflow = true;
    }

    ym2151_write_queue_reg[ym2151_write_queue_head] = reg;
    ym2151_write_queue_data[ym2151_write_queue_head] = data;
    ym2151_write_queue_head = next_head;
}

static inline void __not_in_flash_func(ym2151_service_writes_locked)(uint32_t budget)
{
    while (budget--)
    {
        uint8_t reg;
        uint8_t data;

        uint32_t save = spin_lock_blocking(ym2151_lock);
        if (ym2151_write_queue_tail == ym2151_write_queue_head)
        {
            spin_unlock(ym2151_lock, save);
            break;
        }

        reg = ym2151_write_queue_reg[ym2151_write_queue_tail];
        data = ym2151_write_queue_data[ym2151_write_queue_tail];
        ym2151_write_queue_tail = (uint16_t)((ym2151_write_queue_tail + 1u) & YM2151_WRITE_QUEUE_MASK);
        spin_unlock(ym2151_lock, save);

        OPM_Write(&ym2151_instance, 0u, reg);
        ym2151_clock_cycles_locked(YM2151_WRITE_APPLY_CYCLES);
        OPM_Write(&ym2151_instance, 1u, data);
        ym2151_clock_cycles_locked(YM2151_WRITE_APPLY_CYCLES);
        ym2151_status_latch = (uint8_t)(OPM_Read(&ym2151_instance, 1u) & 0x7Fu);
    }
}

static inline uint8_t __not_in_flash_func(ym2151_read_status)(void)
{
    if (!ym2151_ready)
        return 0xFFu;

    ym2151_write_queue_overflow = false;
    return ym2151_status_latch;
}

static inline void __not_in_flash_func(ym2151_select_register)(uint8_t reg)
{
    ym2151_address_latch = reg;
}

static inline void __not_in_flash_func(ym2151_write_register_data)(uint8_t data)
{
    if (!ym2151_ready)
        return;

    uint32_t save = spin_lock_blocking(ym2151_lock);
    ym2151_queue_register_write_locked(ym2151_address_latch, data);
    spin_unlock(ym2151_lock, save);
}

static inline void __not_in_flash_func(ym2151_calc_stereo_sample)(int16_t *samples, int index)
{
    int32_t opm_out[2] = {0, 0};

    if (ym2151_ready)
    {
        ym2151_service_writes_locked(YM2151_WRITE_SERVICE_BUDGET);
        ym2151_clock_accum += ym2151_clock_hz;
        while (ym2151_clock_accum >= (YM2151_SAMPLE_RATE * YM2151_FRAME_DIVIDER))
        {
            ym2151_clock_frame_locked();
            ym2151_clock_accum -= (YM2151_SAMPLE_RATE * YM2151_FRAME_DIVIDER);
        }
        ym2151_status_latch = (uint8_t)(OPM_Read(&ym2151_instance, 1u) & 0x7Fu);
        opm_out[0] = scale_sample_percent_i32(ym2151_last_output[0], YM2151_BASE_VOLUME_PERCENT);
        opm_out[1] = scale_sample_percent_i32(ym2151_last_output[1], YM2151_BASE_VOLUME_PERCENT);
    }

    int16_t psg = 0;
    (void)main_psg_calc_audible_sample_shifted(YM2151_PSG_VOLUME_SHIFT, &psg);
    samples[index * 2] = apply_audio_volume(opm_out[0] + psg);
    samples[index * 2 + 1] = apply_audio_volume(opm_out[1] + psg);
}

static inline void __not_in_flash_func(ym2151_audio_service_buffer)(bool service_psg_io)
{
    if (!ym2151_audio_started || !ym2151_audio_pool)
        return;

    if (service_psg_io)
        main_psg_service_io();

    struct audio_buffer *buffer = take_audio_buffer(ym2151_audio_pool, false);
    if (!buffer)
        return;

    int16_t *samples = (int16_t *)buffer->buffer->bytes;
    for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
    {
        if (service_psg_io)
            main_psg_service_io();
        ym2151_calc_stereo_sample(samples, i);
    }
    buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
    give_audio_buffer(ym2151_audio_pool, buffer);
}

static void __no_inline_not_in_flash_func(core1_ym2151_audio)(void)
{
    while (true)
    {
        ym2151_audio_service_buffer(true);
        tight_loop_contents();
    }
}

static void ym2151_audio_init(void)
{
    if (ym2151_audio_started)
        return;

    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 1);

    struct audio_buffer_pool *handoff_pool = claim_rom_audio_handoff_pool();
    if (handoff_pool)
    {
        ym2151_audio_pool = handoff_pool;
        ym2151_audio_started = true;
        return;
    }

    static audio_format_t ym2151_audio_format = {
        .sample_freq = YM2151_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };

    static struct audio_buffer_format ym2151_producer_format = {
        .format = &ym2151_audio_format,
        .sample_stride = 4,
    };

    ym2151_audio_pool = audio_new_producer_pool(&ym2151_producer_format, 3, SCC_AUDIO_BUFFER_SAMPLES);

    int dma_channel = -1;
    for (int ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (!dma_channel_is_claimed(ch)) {
            dma_channel = ch;
            break;
        }
    }
    if (dma_channel < 0) {
        ym2151_audio_pool = NULL;
        return;
    }

    static struct audio_i2s_config ym2151_i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_BCLK_PIN,
        .dma_channel = 0,
        .pio_sm = 2,
    };
    ym2151_i2s_config.dma_channel = (uint)dma_channel;

    audio_i2s_setup(&ym2151_audio_format, &ym2151_i2s_config);
    audio_i2s_connect(ym2151_audio_pool);
    audio_i2s_set_enabled(true);
    gpio_put(I2S_MUTE_PIN, 0);
    ym2151_audio_started = true;
}

static void start_ym2151_audio_output(void)
{
    ym2151_audio_init();
    if (ym2151_audio_pool)
        multicore_launch_core1(core1_ym2151_audio);
}

static inline uint8_t __not_in_flash_func(mapper_page_from_reg)(uint8_t reg)
{
    return (uint8_t)(reg % MAPPER_PAGES);
}

static inline void __not_in_flash_func(pio_drain_writes)(void (*handler)(uint16_t addr, uint8_t data, void *ctx), void *ctx)
{
    uint16_t addr;
    uint8_t data;
    wavegame_service_io();
    while (pio_try_get_write(&addr, &data))
    {
        handler(addr, data, ctx);
    }
    wavegame_service_io();
}

// Drain the write captor FIFO without touching the WaveGame I/O path.
// Used by the mappers that can never run a WaveGame (ASCII16-X, Neo-8,
// Neo-16, Konami SCC with SCC audio) so their bus loop stays as short as
// possible.
static inline void __not_in_flash_func(pio_drain_writes_only)(void (*handler)(uint16_t addr, uint8_t data, void *ctx), void *ctx)
{
    uint16_t addr;
    uint8_t data;
    while (pio_try_get_write(&addr, &data))
    {
        handler(addr, data, ctx);
    }
}

// Wait for the next memory read address, draining the write captor FIFO
// while idle.  Mandatory for every banked mapper: game code running from
// MSX RAM can issue long bursts of bank-switch writes without a single
// cartridge read in between, and the write captor RX FIFO is only 8
// entries deep.  Once it fills, the PIO stalls on "push block" and every
// further bank switch is silently lost, leaving the mapper registers on a
// stale segment.  Blocking on the read FIFO alone therefore drops writes.
//
// The idle loop reads FSTAT exactly once per iteration and tests both FIFO
// flags from that single sample, and does nothing else.  A PIO register
// access dominates the loop cost, so any extra work here (a second FIFO
// poll, or a WaveGame I/O service call) directly stretches /WAIT on every
// cartridge read and slows down VDP transfer loops.  WaveGame I/O is left
// to the pio_drain_writes() call that the WaveGame-capable mapper loops
// perform after taking the read address, which keeps it at its original
// rate.
static inline uint16_t __not_in_flash_func(pio_get_read_draining_writes)(
    void (*handler)(uint16_t addr, uint8_t data, void *ctx), void *ctx)
{
    PIO pio = msx_bus.pio;
    const uint32_t read_empty  = 1u << (PIO_FSTAT_RXEMPTY_LSB + msx_bus.sm_read);
    const uint32_t write_empty = 1u << (PIO_FSTAT_RXEMPTY_LSB + msx_bus.sm_write);

    while (true)
    {
        uint32_t fstat = pio->fstat;

        if (!(fstat & read_empty))
            return (uint16_t)pio_sm_get(pio, msx_bus.sm_read);

        if (!(fstat & write_empty))
            pio_drain_writes_only(handler, ctx);
    }
}

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

static inline void __not_in_flash_func(megaram_bank_switch_write)(uint16_t addr, uint8_t data, uint8_t bank_reg[4])
{
    bank_reg[megaram_page_from_addr(addr)] = data & 0x7Fu;
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

typedef struct {
    sunrise_ide_t *ide;
} sunrise_ctx_t;

static inline void __not_in_flash_func(handle_sunrise_write)(uint16_t addr, uint8_t data, void *ctx)
{
    sunrise_ctx_t *sctx = (sunrise_ctx_t *)ctx;
    if (addr >= 0x4000u && addr <= 0x7FFFu)
    {
        sunrise_ide_handle_write(sctx->ide, addr, data);
    }
}

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

static inline void __not_in_flash_func(handle_menu_write)(uint16_t addr, uint8_t data, void *ctx)
{
    menu_select_ctx_t *menu_ctx = (menu_select_ctx_t *)ctx;
    
    // Original explorer menu handling logic needs to be adapted here
    // However, the original menu loop was complex handling many registers.
    // The handle_menu_write in multirom only checks MONITOR_ADDR.
    // Explorer needs to handle CTRL registers and MP3 registers too.

    // For now, I'll only put the basic MONITOR_ADDR check here 
    // AND I will have to implement a more complex handler or just use the main loop 
    // inside loadrom_msx_menu which will be refactored anyway.
    
    if (addr == MONITOR_ADDR)
    {
        if (data == ROM_SELECT_MENU) {
            menu_ctx->rom_selected = false;
            return;
        }
        menu_ctx->rom_index = data;
        menu_ctx->rom_selected = true;
    }
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

static void __no_inline_not_in_flash_func(banked8_loop)(
    const uint8_t *rom_base,
    uint32_t available_length,
    uint8_t *bank_regs,
    void (*write_handler)(uint16_t, uint8_t, void *))
{
    bank8_ctx_t ctx = { .bank_regs = bank_regs };

    // openMSX-style block handling for non power-of-two ROMs (RomBlocks):
    // the image is treated as nrBlocks 8 KB blocks. A selected block that is
    // out of range wraps through blockMask (nrBlocks - 1) instead of reading
    // 0xFF, which lets odd-sized ASCII8 images (e.g. 264 KB / 33 blocks) run.
    uint32_t nrBlocks  = (available_length != 0u) ? ((available_length + 0x1FFFu) >> 13) : 0u;
    uint32_t blockMask = (nrBlocks != 0u) ? (nrBlocks - 1u) : 0u;

    while (true)
    {
        uint16_t addr = pio_get_read_draining_writes(write_handler, &ctx);

        pio_drain_writes(write_handler, &ctx);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint32_t block = bank_regs[(addr - 0x4000u) >> 13];
            if (nrBlocks != 0u && block >= nrBlocks)
                block &= blockMask;
            uint32_t rel = (block << 13) | (addr & 0x1FFFu);
            if (available_length == 0u || rel < available_length)
                data = read_rom_byte(rom_base, rel);
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

typedef struct {
    bool rom_selected;
    uint16_t rom_index;
} explorer_menu_ctx_t;

static void fh_copy_query_from_buffer(char *out, size_t out_size);
static bool fh_fetch_catalog(const char *query);
static void fh_set_catalog_message(const char *message);
static void fh_build_page_buffer(void);
static bool fh_download_selected(void);
static void fh_update_wifi_status_text(void);
static void fh_save_background_work(void);
static bool fh_start_save_job(void);
static void nexus_tracker_checkin_once(void);

static void __not_in_flash_func(handle_menu_fh_command)(uint8_t data, explorer_menu_ctx_t *menu_ctx)
{
    fh_menu_window_active = true;
    fh_service_menu_window = true;
    fh_status = FH_STATUS_BUSY;

    if (data == CMD_FH_LIST_PAGE || data == CMD_FH_SEARCH)
    {
        printf("[nexus] menu File Hunter command 0x%02X received, catalog_loaded=%u\n", data, fh_catalog_loaded ? 1u : 0u);
        fflush(stdout);
        if (data == CMD_FH_SEARCH || !fh_catalog_loaded)
        {
            fh_copy_query_from_buffer(fh_active_query, sizeof(fh_active_query));
            printf("[nexus] menu fetching File Hunter catalog, query='%s'\n", fh_active_query);
            fflush(stdout);
            if (!fh_fetch_catalog(fh_active_query))
            {
                printf("[nexus] menu File Hunter catalog fetch failed\n");
                fflush(stdout);
                fh_set_catalog_message("Offline or File Hunter unavailable");
            }
            else if (fh_catalog_count == 0u)
            {
                printf("[nexus] menu File Hunter catalog fetched, count=0\n");
                fflush(stdout);
                nexus_tracker_checkin_once();
                fh_set_catalog_message("No File Hunter ROMs found");
            }
            else
            {
                printf("[nexus] menu File Hunter catalog fetched, count=%u\n", fh_catalog_count);
                fflush(stdout);
                nexus_tracker_checkin_once();
            }
        }
        else
        {
            printf("[nexus] menu using cached File Hunter catalog, count=%u\n", fh_catalog_count);
            fflush(stdout);
            nexus_tracker_checkin_once();
        }
        fh_build_page_buffer();
        fh_result = fh_catalog_count ? 1u : 0u;
    }
    else if (data == CMD_FH_DOWNLOAD)
    {
        fh_selected_index = (uint16_t)((uint8_t)fh_query[0]) |
                            (uint16_t)(((uint8_t)fh_query[1]) << 8);
        fh_progress_percent = 0;
        fh_result = 0;
        if (!fh_download_selected())
            fh_result = 0u;
    }
    else if (data == CMD_FH_WIFI_STATUS)
    {
        fh_update_wifi_status_text();
        fh_result = 1u;
    }
    else if (data == CMD_FH_WIFI_CONFIG)
    {
        wifi_f2_state = WIFI_F2_FORCE_SETUP;
        fh_result = 1u;
        if (menu_ctx)
        {
            menu_ctx->rom_index = ROM_SELECT_WIFI_CONFIG;
            menu_ctx->rom_selected = true;
        }
    }

    fh_status = FH_STATUS_READY;
    fh_service_menu_window = false;
}

static inline void __not_in_flash_func(handle_menu_write_explorer)(uint16_t addr, uint8_t data, void *ctx)
{
    explorer_menu_ctx_t *menu_ctx = (explorer_menu_ctx_t *)ctx;

    if (addr >= CTRL_QUERY_BASE && addr < (CTRL_QUERY_BASE + CTRL_QUERY_SIZE))
    {
        uint16_t query_offset = addr - CTRL_QUERY_BASE;
        filter_query[query_offset] = (char)data;
        if (query_offset < FH_QUERY_SIZE)
            fh_query[query_offset] = data;
        return;
    }

    if (addr == CTRL_CMD)
    {
        ctrl_cmd_state = data;
        if (data == CMD_APPLY_FILTER)
        {
            filter_query[CTRL_QUERY_SIZE - 1] = '\0';
            apply_filter();
            current_page = 0;
            build_page_buffer(current_page);
        }
        else if (data == CMD_FIND_FIRST)
        {
            filter_query[CTRL_QUERY_SIZE - 1] = '\0';
            find_first_match();
        }
        else if (data == CMD_ENTER_DIR)
        {
            process_enter_dir_request();
            // Keep ctrl_cmd_state as CMD_ENTER_DIR to signal busy/done?
            // Original code: ctrl_cmd_state = CMD_ENTER_DIR;
            // Then it checked if (cmd != CMD_ENTER_DIR...) ctrl_cmd_state = 0;
            // Here data is CMD_ENTER_DIR.
        }
        else if (data == CMD_DETECT_MAPPER)
        {
             uint16_t index = (uint16_t)((uint8_t)filter_query[0]) |
                                         (uint16_t)(((uint8_t)filter_query[1]) << 8);
             detect_mapper_index = index;
             detect_mapper_pending = true;
             ctrl_mapper_value = 0;
        }
        else if (data == CMD_SET_MAPPER)
        {
             uint16_t index = (uint16_t)((uint8_t)filter_query[0]) |
                                         (uint16_t)(((uint8_t)filter_query[1]) << 8);
             uint8_t mapper = (uint8_t)filter_query[2];
             
             ctrl_ack_value = 0;
             if (index < total_record_count && mapper != 0 && !is_system_mapper(mapper) && mapper < MAPPER_DESCRIPTION_COUNT) {
                 uint16_t record_index = filtered_indices[index];
                 if (record_index < full_record_count) {
                    ROMRecord *rec = &records[record_index];
                    uint8_t flags = rec->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG);
                    if ((flags & (FOLDER_FLAG | MP3_FLAG)) == 0) {
                        rec->Mapper = (uint8_t)(flags | mapper);
                        ctrl_ack_value = 1;
                    }
                 }
             }
        }
        else if (data == CMD_SET_SOURCE)
        {
            uint8_t mode = (uint8_t)filter_query[0];
            if (mode > SOURCE_MODE_SD) {
                mode = SOURCE_MODE_ALL;
            }
            cancel_refresh_work();
            fh_menu_window_active = false;
            browse_source_mode = mode;
            sd_current_path[0] = '/';
            sd_current_path[1] = '\0';
            if (last_selection_restore_pending && mode == last_selection_source) {
                strcpy(sd_current_path, last_selection_dir);
            }
            memset(filter_query, 0, sizeof(filter_query));
            refresh_requested = true;
        }
        else if (data == CMD_LOAD_OPTIONS)
        {
            process_load_options_request();
        }
        else if (data == CMD_SAVE_OPTIONS)
        {
            process_save_options_request();
        }
        else if (data == CMD_PREPARE_QUICK_RUN)
        {
            process_prepare_quick_run_request();
        }
        else if (data == CMD_CYCLE_SD_PARTITION)
        {
            process_cycle_sd_partition_request();
        }
        else if (data == CMD_DELETE_SD_FILE)
        {
            process_delete_sd_file_request();
        }
        else if (data == CMD_LOAD_LAST_SELECTION)
        {
            process_load_last_selection_request();
        }
        else if (data == CMD_SAVE_LAST_SELECTION)
        {
            process_save_last_selection_request();
        }
        else if (data == CMD_FH_LIST_PAGE || data == CMD_FH_SEARCH ||
                 data == CMD_FH_DOWNLOAD || data == CMD_FH_WIFI_STATUS ||
                 data == CMD_FH_WIFI_CONFIG)
        {
            cancel_refresh_work();
            handle_menu_fh_command(data, menu_ctx);
        }
        
           if (data != CMD_ENTER_DIR && data != CMD_DETECT_MAPPER && data != CMD_DELETE_SD_FILE) {
             ctrl_cmd_state = 0;
        }
        return;
    }

    if (addr == MP3_CTRL_INDEX_L) {
        mp3_selected_index = (uint16_t)((mp3_selected_index & 0xFF00u) | data);
        return;
    }
    if (addr == MP3_CTRL_INDEX_H) {
        mp3_selected_index = (uint16_t)((mp3_selected_index & 0x00FFu) | ((uint16_t)data << 8));
        return;
    }

    if (addr == MP3_CTRL_CMD) {
        if (data == MP3_CMD_SELECT) {
             if (mp3_selected_index < total_record_count) {
                 mp3_pending_index = mp3_selected_index;
                 mp3_pending_select = true;
             }
         } else if (data == MP3_CMD_PLAY || data == MP3_CMD_STOP ||
                    data == MP3_CMD_PAUSE || data == MP3_CMD_RESUME ||
                data == MP3_CMD_TOGGLE_MUTE || data == MP3_CMD_NEXT ||
                data == MP3_CMD_PREVIOUS) {
             mp3_pending_cmd = data;
        }
        return;
    }

    if (addr == MP3_CTRL_MODE) {
        if (data <= MP3_PLAY_MODE_RANDOM) {
            mp3_play_mode = data;
        }
        return;
    }

    if (addr == CTRL_AUDIO) {
        ctrl_audio_selection = data;
        return;
    }

    if (addr == CTRL_WIFI_SUPPORT) {
        ctrl_wifi_support = data ? 1u : 0u;
        return;
    }

    if (addr == CTRL_PSG_EMULATION) {
        ctrl_psg_emulation = data ? 1u : 0u;
        return;
    }

    if (addr == CTRL_SD_PARTITION) {
        ctrl_sd_partition = (data >= 1u && data <= 4u) ? data : 0u;
        refresh_sunrise_partition_info(ctrl_sd_partition);
        return;
    }

    if (addr == CTRL_AUDIO_VOLUME) {
        ctrl_audio_volume = data <= AUDIO_VOLUME_MAX ? data : AUDIO_VOLUME_DEFAULT;
        return;
    }

    if (addr == CTRL_PAGE) {
        if (fh_menu_window_active) {
            fh_page_index = data;
        } else if (data != current_page) {
            current_page = data;
            build_page_buffer(current_page);
        }
        return;
    }

    if (addr == MONITOR_ADDR) {
        uint8_t selected_index = data;
        if (selected_index == ROM_SELECT_MENU) {
            menu_ctx->rom_selected = false;
            return;
        }
        if (selected_index < total_record_count) {
            menu_ctx->rom_index = (uint8_t)filtered_indices[selected_index];
            menu_ctx->rom_selected = true;
        }
        return;
    }
}

static uint16_t fh_read_le16(const uint8_t *ptr)
{
    return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static void fh_copy_status_prefix(const char *prefix);
static void fh_set_offline_status(void);
static bool fh_wifi_wait_byte(uint8_t *data_out, uint32_t deadline);
static bool fh_wifi_get_ap_connected(bool *connected);
static bool fh_wifi_wait_connected(uint32_t timeout_us);

static uint8_t __not_in_flash_func(fh_read_window_byte)(uint16_t addr, bool *in_window)
{
    uint8_t data = 0xFFu;
    *in_window = false;

    if (addr >= 0x4000u && addr <= 0xBFFFu)
    {
        *in_window = true;
        if (addr >= FH_CTRL_BASE && addr <= (FH_CTRL_BASE + 0x0Fu))
        {
            switch (addr)
            {
                case FH_COUNT_L: data = (uint8_t)(fh_catalog_count & 0xFFu); break;
                case FH_COUNT_H: data = (uint8_t)((fh_catalog_count >> 8) & 0xFFu); break;
                case FH_PAGE: data = fh_page_index; break;
                case FH_STATUS: data = fh_status; break;
                case FH_PROGRESS_L: data = (uint8_t)(fh_progress_percent & 0xFFu); break;
                case FH_PROGRESS_H: data = (uint8_t)((fh_progress_percent >> 8) & 0xFFu); break;
                case FH_RESULT: data = fh_result; break;
            }
        }
        else if (addr >= FH_DATA_BASE && addr < (FH_DATA_BASE + (FH_RECORD_SIZE * FH_FILES_PER_PAGE)))
        {
            data = page_buffer[addr - FH_DATA_BASE];
        }
        else if (addr >= CTRL_CHIP_ID_BASE && addr < (CTRL_CHIP_ID_BASE + CTRL_CHIP_ID_SIZE))
        {
            data = (uint8_t)pico_chip_id[addr - CTRL_CHIP_ID_BASE];
        }
        else if (addr == CTRL_NET_STATUS)
        {
            data = esp_net_status;
        }
        else if (addr >= FH_STATUS_TEXT_BASE && addr < (FH_STATUS_TEXT_BASE + FH_STATUS_TEXT_SIZE))
        {
            data = (uint8_t)fh_wifi_status_text[addr - FH_STATUS_TEXT_BASE];
        }
    }

    return data;
}

static uint8_t __not_in_flash_func(fh_read_menu_window_byte)(uint16_t addr, bool *in_window)
{
    uint8_t data = 0xFFu;
    *in_window = false;

    if (addr >= 0x4000u && addr <= 0xBFFFu)
    {
        *in_window = true;
        if (addr >= FH_CTRL_BASE && addr <= (FH_CTRL_BASE + 0x0Fu))
        {
            switch (addr)
            {
                case FH_COUNT_L: data = (uint8_t)(fh_catalog_count & 0xFFu); break;
                case FH_COUNT_H: data = (uint8_t)((fh_catalog_count >> 8) & 0xFFu); break;
                case FH_PAGE: data = fh_page_index; break;
                case FH_STATUS: data = fh_status; break;
                case FH_CMD: data = ctrl_cmd_state; break;
                case FH_PROGRESS_L: data = (uint8_t)(fh_progress_percent & 0xFFu); break;
                case FH_PROGRESS_H: data = (uint8_t)((fh_progress_percent >> 8) & 0xFFu); break;
                case FH_RESULT: data = fh_result; break;
            }
        }
        else if (addr >= CTRL_CHIP_ID_BASE && addr < (CTRL_CHIP_ID_BASE + CTRL_CHIP_ID_SIZE))
        {
            data = (uint8_t)pico_chip_id[addr - CTRL_CHIP_ID_BASE];
        }
        else if (addr == CTRL_NET_STATUS)
        {
            data = esp_net_status;
        }
        else if (addr >= FH_STATUS_TEXT_BASE && addr < (FH_STATUS_TEXT_BASE + FH_STATUS_TEXT_SIZE))
        {
            data = (uint8_t)fh_wifi_status_text[addr - FH_STATUS_TEXT_BASE];
        }
        else if (addr >= FH_DATA_BASE && addr < (FH_DATA_BASE + (FH_RECORD_SIZE * FH_FILES_PER_PAGE)))
        {
            data = page_buffer[addr - FH_DATA_BASE];
        }
        else
        {
            uint32_t rel = addr - 0x4000u;
            if (rel < MENU_ROM_SIZE)
                data = rom_sram[rel];
        }
    }

    return data;
}

static void __not_in_flash_func(fh_service_msx_reads)(void)
{
    while (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
    {
        uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
        bool in_window;
        uint8_t data = fh_service_menu_window ?
            fh_read_menu_window_byte(addr, &in_window) :
            fh_read_window_byte(addr, &in_window);
        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

static uint32_t fh_read_le32(const uint8_t *ptr)
{
    return (uint32_t)ptr[0] |
           ((uint32_t)ptr[1] << 8) |
           ((uint32_t)ptr[2] << 16) |
           ((uint32_t)ptr[3] << 24);
}

static void fh_set_catalog_message(const char *message)
{
    memset(fh_catalog, 0, sizeof(fh_catalog));
    fh_catalog_loaded = false;
    fh_catalog_message = message && *message;
    if (message && *message)
    {
        strncpy(fh_catalog[0].name, message, ROM_NAME_MAX);
        fh_catalog[0].name[ROM_NAME_MAX] = '\0';
        fh_catalog_count = 1;
    }
    else
    {
        fh_catalog_count = 0;
    }
}

static bool fh_wifi_wait_response(uint8_t command, uint8_t *response, uint16_t response_cap,
                                  uint16_t *response_len, uint32_t timeout_us)
{
    uint8_t data;
    uint8_t rc;
    uint8_t size_hi;
    uint8_t size_lo;
    uint16_t size;
    uint16_t keep;
    uint32_t deadline;

    deadline = time_us_32() + timeout_us;
    do {
        if (!fh_wifi_wait_byte(&data, deadline)) return false;
    } while (data != command);

    if (!fh_wifi_wait_byte(&rc, deadline) || rc != 0u) return false;
    if (!fh_wifi_wait_byte(&size_hi, deadline)) return false;
    if (!fh_wifi_wait_byte(&size_lo, deadline)) return false;
    size = (uint16_t)(((uint16_t)size_hi << 8) | size_lo);
    keep = size;
    if (keep > response_cap) keep = response_cap;
    for (uint16_t i = 0; i < size; ++i)
    {
        if (!fh_wifi_wait_byte(&data, deadline)) return false;
        if (i < keep && response) response[i] = data;
    }
    if (response_len) *response_len = keep;
    return true;
}

static bool fh_wifi_command(uint8_t command, const uint8_t *payload, uint16_t payload_len,
                            uint8_t *response, uint16_t response_cap, uint16_t *response_len,
                            uint32_t timeout_us)
{
    wifi_uart_init_once();
    wifi_hw_rx_drain();
    wifi_reset_fifo();

    uart_putc_raw(WIFI_UART_INSTANCE, (char)command);
    uart_putc_raw(WIFI_UART_INSTANCE, (char)(payload_len >> 8));
    uart_putc_raw(WIFI_UART_INSTANCE, (char)(payload_len & 0xFFu));
    for (uint16_t i = 0; i < payload_len; ++i)
        uart_putc_raw(WIFI_UART_INSTANCE, (char)payload[i]);
    wifi_tx_busy_deadline_us = time_us_32() + wifi_uart_frame_time_us();

    return fh_wifi_wait_response(command, response, response_cap, response_len, timeout_us);
}

static bool fh_wifi_command_report_error(uint8_t command, const uint8_t *payload, uint16_t payload_len,
                                         uint8_t *response, uint16_t response_cap, uint16_t *response_len,
                                         uint8_t *error_code, uint32_t timeout_us)
{
    uint8_t data;
    uint8_t rc;
    uint8_t size_hi;
    uint8_t size_lo;
    uint16_t size;
    uint16_t keep;
    uint32_t deadline;

    wifi_uart_init_once();
    wifi_hw_rx_drain();
    wifi_reset_fifo();

    uart_putc_raw(WIFI_UART_INSTANCE, (char)command);
    uart_putc_raw(WIFI_UART_INSTANCE, (char)(payload_len >> 8));
    uart_putc_raw(WIFI_UART_INSTANCE, (char)(payload_len & 0xFFu));
    for (uint16_t i = 0; i < payload_len; ++i)
        uart_putc_raw(WIFI_UART_INSTANCE, (char)payload[i]);
    wifi_tx_busy_deadline_us = time_us_32() + wifi_uart_frame_time_us();

    deadline = time_us_32() + timeout_us;
    do {
        if (!fh_wifi_wait_byte(&data, deadline)) return false;
    } while (data != command);

    if (!fh_wifi_wait_byte(&rc, deadline)) return false;
    if (error_code) *error_code = rc;
    if (!fh_wifi_wait_byte(&size_hi, deadline))
    {
        if (response_len) *response_len = 0;
        return rc == 0u;
    }
    if (!fh_wifi_wait_byte(&size_lo, deadline))
    {
        if (response_len) *response_len = 0;
        return rc == 0u;
    }
    size = (uint16_t)(((uint16_t)size_hi << 8) | size_lo);
    keep = size;
    if (keep > response_cap) keep = response_cap;
    for (uint16_t i = 0; i < size; ++i)
    {
        if (!fh_wifi_wait_byte(&data, deadline)) return false;
        if (i < keep && response) response[i] = data;
    }
    if (response_len) *response_len = keep;
    return rc == 0u;
}

static bool fh_parse_ipv4_literal(const char *host, uint8_t ip[4])
{
    const char *cursor = host;

    for (uint8_t part = 0; part < 4u; ++part)
    {
        unsigned long value;
        char *end;

        if (!isdigit((unsigned char)*cursor)) return false;
        value = strtoul(cursor, &end, 10);
        if (value > 255u) return false;
        ip[part] = (uint8_t)value;
        if (part == 3u)
            return *end == '\0';
        if (*end != '.') return false;
        cursor = end + 1;
    }

    return false;
}

static bool fh_wifi_dns(const char *host, uint8_t ip[4])
{
    uint8_t payload[1 + 64];
    uint16_t host_len = (uint16_t)strlen(host);
    uint16_t response_len = 0;

    if (fh_parse_ipv4_literal(host, ip))
    {
        printf("[fh] using IPv4 literal %s -> %u.%u.%u.%u\n", host, ip[0], ip[1], ip[2], ip[3]);
        fflush(stdout);
        return true;
    }
    if (host_len > 63u)
    {
        printf("[fh] DNS skipped: host too long '%s'\n", host);
        fflush(stdout);
        return false;
    }
    payload[0] = 0;
    memcpy(&payload[1], host, host_len);
    if (!fh_wifi_command(206u, payload, (uint16_t)(host_len + 1u), ip, 4, &response_len, 15000000u))
    {
        printf("[fh] DNS failed for '%s'\n", host);
        fflush(stdout);
        return false;
    }
    printf("[fh] DNS %s -> %u.%u.%u.%u, len=%u\n", host, ip[0], ip[1], ip[2], ip[3], response_len);
    fflush(stdout);
    return response_len == 4u;
}

static bool fh_wifi_get_ipinfo(uint8_t index, uint8_t ip[4])
{
    uint16_t response_len = 0;

    if (!fh_wifi_command(2u, &index, 1u, ip, 4u, &response_len, 2000000u))
        return false;
    return response_len == 4u;
}

static void fh_wifi_print_ipinfo(void)
{
    static const char *labels[] = { "", "local", "peer", "subnet", "gateway", "dns1", "dns2" };
    uint8_t ip[4];

    for (uint8_t index = 1u; index <= 6u; ++index)
    {
        if (fh_wifi_get_ipinfo(index, ip))
            printf("[fh] IPINFO %s=%u.%u.%u.%u\n", labels[index], ip[0], ip[1], ip[2], ip[3]);
        else
            printf("[fh] IPINFO %s=failed\n", labels[index]);
        fflush(stdout);
    }
}

static bool fh_wifi_tcp_open_ex(const char *host, uint16_t port, bool tls, uint8_t *connection)
{
    uint8_t ip[4];
    uint8_t params[11 + 64];
    uint16_t params_len = 11u;
    uint8_t response[1];
    uint16_t response_len = 0;
    size_t host_len = strlen(host);

    printf("[fh] TCP open request host=%s port=%u tls=%u\n", host, port, tls ? 1u : 0u);
    fflush(stdout);
    if (host_len > 63u)
    {
        printf("[fh] TCP open failed: host too long\n");
        fflush(stdout);
        return false;
    }
    if (!fh_wifi_dns(host, ip))
    {
        printf("[fh] TCP open failed: DNS/IP step\n");
        fflush(stdout);
        return false;
    }
    memset(params, 0, sizeof(params));
    memcpy(params, ip, 4);
    params[4] = (uint8_t)(port & 0xFFu);
    params[5] = (uint8_t)(port >> 8);
    params[6] = 0xFF;
    params[7] = 0xFF;
    params[8] = 120;
    params[9] = 0;
    params[10] = tls ? 0x04u : 0u;
    if (tls)
    {
        memcpy(params + params_len, host, host_len);
        params_len = (uint16_t)(params_len + host_len);
    }

    uint8_t error_code = 0xFFu;
    if (!fh_wifi_command_report_error(13u, params, params_len, response, sizeof(response), &response_len, &error_code, 60000000u))
    {
        printf("[fh] TCP open command failed host=%s port=%u err=%u len=%u", host, port, error_code, response_len);
        if (response_len >= 1u)
            printf(" close_reason=%u", response[0]);
        printf("\n");
        fflush(stdout);
        return false;
    }
    if (response_len != 1u)
    {
        printf("[fh] TCP open bad response len=%u\n", response_len);
        fflush(stdout);
        return false;
    }
    *connection = response[0];
    printf("[fh] TCP open ok connection=%u\n", *connection);
    fflush(stdout);
    return true;
}

static bool fh_wifi_tcp_open(const char *host, uint8_t *connection)
{
    return fh_wifi_tcp_open_ex(host, 80u, false, connection);
}

static bool fh_wifi_tcp_state(uint8_t connection, uint16_t *available, uint8_t *state)
{
    uint8_t response[16];
    uint16_t response_len = 0;

    if (!fh_wifi_command(16u, &connection, 1u, response, sizeof(response), &response_len, 2000000u))
        return false;
    if (response_len < 4u) return false;
    if (state) *state = response[1];
    if (available) *available = (uint16_t)response[2] | ((uint16_t)response[3] << 8);
    return true;
}

static bool fh_wifi_tcp_send(uint8_t connection, const char *data, uint16_t len)
{
    uint8_t command = 17u;
    uint16_t payload_len = (uint16_t)(len + 2u);
    uint8_t response_dummy;
    uint16_t response_len = 0;

    wifi_uart_init_once();
    wifi_hw_rx_drain();
    wifi_reset_fifo();
    uart_putc_raw(WIFI_UART_INSTANCE, (char)command);
    uart_putc_raw(WIFI_UART_INSTANCE, (char)(payload_len >> 8));
    uart_putc_raw(WIFI_UART_INSTANCE, (char)(payload_len & 0xFFu));
    uart_putc_raw(WIFI_UART_INSTANCE, (char)connection);
    uart_putc_raw(WIFI_UART_INSTANCE, 1);
    for (uint16_t i = 0; i < len; ++i)
        uart_putc_raw(WIFI_UART_INSTANCE, data[i]);
    wifi_tx_busy_deadline_us = time_us_32() + wifi_uart_frame_time_us();

    return fh_wifi_wait_response(command, &response_dummy, 0, &response_len, 10000000u);
}

static bool fh_wifi_tcp_receive(uint8_t connection, uint8_t *buffer, uint16_t cap, uint16_t *bytes_read)
{
    uint8_t command = 18u;
    uint8_t payload[3];
    uint8_t data;
    uint8_t rc;
    uint8_t size_hi;
    uint8_t size_lo;
    uint16_t response_size;
    uint16_t response_len = 0;
    uint16_t keep;
    uint32_t deadline;

    if (cap > FH_HTTP_CHUNK_SIZE) cap = FH_HTTP_CHUNK_SIZE;
    fh_tcp_last_error = 0;
    payload[0] = connection;
    payload[1] = (uint8_t)(cap & 0xFFu);
    payload[2] = (uint8_t)(cap >> 8);

    wifi_uart_init_once();
    wifi_hw_rx_drain();
    wifi_reset_fifo();
    uart_putc_raw(WIFI_UART_INSTANCE, (char)command);
    uart_putc_raw(WIFI_UART_INSTANCE, 0);
    uart_putc_raw(WIFI_UART_INSTANCE, (char)sizeof(payload));
    for (uint16_t i = 0; i < sizeof(payload); ++i)
        uart_putc_raw(WIFI_UART_INSTANCE, (char)payload[i]);
    wifi_tx_busy_deadline_us = time_us_32() + wifi_uart_frame_time_us();

    deadline = time_us_32() + 20000000u;
    do {
        if (!fh_wifi_wait_byte(&data, deadline)) {
            fh_tcp_last_error = 0xFEu;
            *bytes_read = 0;
            return false;
        }
    } while (data != command);

    if (!fh_wifi_wait_byte(&rc, deadline))
    {
        fh_tcp_last_error = 0xFEu;
        *bytes_read = 0;
        return false;
    }

    if (rc == 3u)
    {
        *bytes_read = 0;
        return true;
    }
    if (rc != 0u)
    {
        fh_tcp_last_error = rc;
        *bytes_read = 0;
        return false;
    }

    if (!fh_wifi_wait_byte(&size_hi, deadline) ||
        !fh_wifi_wait_byte(&size_lo, deadline))
    {
        fh_tcp_last_error = 0xFEu;
        *bytes_read = 0;
        return false;
    }

    response_size = (uint16_t)(((uint16_t)size_hi << 8) | size_lo);
    keep = response_size;
    if (keep > cap + 2u)
    {
        fh_tcp_last_error = 0xFDu;
        keep = 0u;
    }
    for (uint16_t i = 0; i < response_size; ++i)
    {
        if (!fh_wifi_wait_byte(&data, deadline)) {
            fh_tcp_last_error = 0xFEu;
            *bytes_read = 0;
            return false;
        }
        if (i < keep) fh_tcp_buffer[i] = data;
        if ((i & 0x1Fu) == 0x1Fu) fh_service_msx_reads();
    }
    if (fh_tcp_last_error == 0xFDu)
    {
        *bytes_read = 0;
        return false;
    }
    response_len = keep;

    if (response_len <= 2u)
    {
        uint16_t available = 0;
        uint8_t state = 0;

        *bytes_read = 0;
        if (fh_wifi_tcp_state(connection, &available, &state) && available == 0u && state == 0u)
            return false;
        return true;
    }
    *bytes_read = (uint16_t)(response_len - 2u);
    memcpy(buffer, &fh_tcp_buffer[2], *bytes_read);
    return true;
}

static void fh_wifi_tcp_close(uint8_t connection)
{
    uint8_t payload = connection;
    (void)fh_wifi_command(14u, &payload, 1, NULL, 0, NULL, 2000000u);
}

typedef bool (*fh_http_body_callback_t)(const uint8_t *data, uint16_t len, void *ctx);

typedef struct {
    bool chunked;
    bool done;
    bool has_content_length;
    uint32_t content_remaining;
    uint32_t remaining;
    char size_line[12];
    uint8_t size_len;
    uint8_t crlf_to_skip;
} fh_http_decoder_t;

typedef struct {
    char host[64];
    char path[192];
    uint16_t port;
    bool tls;
} fh_url_t;

static bool fh_starts_with(const char *text, const char *prefix)
{
    while (*prefix)
    {
        if (*text++ != *prefix++) return false;
    }
    return true;
}

static bool fh_parse_url(const char *url, fh_url_t *out)
{
    const char *host_start;
    const char *path_start;
    const char *host_end;
    const char *port_start = NULL;
    size_t host_len;
    size_t path_len;

    memset(out, 0, sizeof(*out));
    if (fh_starts_with(url, "https://"))
    {
        out->tls = true;
        out->port = 443u;
        host_start = url + 8;
    }
    else if (fh_starts_with(url, "http://"))
    {
        out->tls = false;
        out->port = 80u;
        host_start = url + 7;
    }
    else
    {
        out->tls = false;
        out->port = 80u;
        host_start = FH_HOST;
        strncpy(out->host, FH_HOST, sizeof(out->host) - 1u);
        strncpy(out->path, url && *url ? url : "/", sizeof(out->path) - 1u);
        return true;
    }

    path_start = strchr(host_start, '/');
    host_end = path_start ? path_start : (host_start + strlen(host_start));
    for (const char *scan = host_start; scan < host_end; ++scan)
    {
        if (*scan == ':')
        {
            port_start = scan + 1;
            host_end = scan;
            break;
        }
    }

    host_len = (size_t)(host_end - host_start);
    if (host_len == 0u || host_len >= sizeof(out->host)) return false;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if (port_start)
    {
        unsigned long parsed_port = strtoul(port_start, NULL, 10);
        if (parsed_port == 0u || parsed_port > 65535u) return false;
        out->port = (uint16_t)parsed_port;
    }

    if (!path_start) path_start = "/";
    path_len = strlen(path_start);
    if (path_len >= sizeof(out->path)) return false;
    memcpy(out->path, path_start, path_len + 1u);
    return true;
}

static const char *fh_find_header_value(const char *headers, const char *name)
{
    size_t name_len = strlen(name);
    const char *line = headers;

    while (line && *line)
    {
        const char *line_end = strstr(line, "\r\n");
        const char *value;
        if (!line_end) line_end = line + strlen(line);
        if ((size_t)(line_end - line) > name_len && line[name_len] == ':')
        {
            bool match = true;
            for (size_t i = 0; i < name_len; ++i)
            {
                if (tolower((unsigned char)line[i]) != tolower((unsigned char)name[i]))
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                value = line + name_len + 1u;
                while (*value == ' ' || *value == '\t') value++;
                return value;
            }
        }
        if (*line_end == '\0') break;
        line = line_end + 2;
    }
    return NULL;
}

static bool fh_header_value_is_chunked(const char *value)
{
    if (!value) return false;
    while (*value && *value != '\r' && *value != '\n')
    {
        if (tolower((unsigned char)value[0]) == 'c' &&
            tolower((unsigned char)value[1]) == 'h' &&
            tolower((unsigned char)value[2]) == 'u' &&
            tolower((unsigned char)value[3]) == 'n' &&
            tolower((unsigned char)value[4]) == 'k' &&
            tolower((unsigned char)value[5]) == 'e' &&
            tolower((unsigned char)value[6]) == 'd')
            return true;
        value++;
    }
    return false;
}

static bool fh_parse_header_u32(const char *value, uint32_t *out)
{
    unsigned long parsed;

    if (!value || !out) return false;
    while (*value == ' ' || *value == '\t') value++;
    if (!isdigit((unsigned char)*value)) return false;
    parsed = strtoul(value, NULL, 10);
    if (parsed > 0xFFFFFFFFul) return false;
    *out = (uint32_t)parsed;
    return true;
}

static int fh_http_month_number(const char *month)
{
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    for (int i = 0; i < 12; ++i)
    {
        if (tolower((unsigned char)month[0]) == tolower((unsigned char)months[i][0]) &&
            tolower((unsigned char)month[1]) == tolower((unsigned char)months[i][1]) &&
            tolower((unsigned char)month[2]) == tolower((unsigned char)months[i][2]))
            return i + 1;
    }
    return 0;
}

static bool fh_parse_http_timestamp(const char *value, FILINFO *timestamp)
{
    char weekday[5];
    char month_name[4];
    char zone[4];
    int day;
    int year;
    int hour;
    int minute;
    int second;
    int month;

    if (!value || !timestamp) return false;
    if (sscanf(value, "%4s %d %3s %d %d:%d:%d %3s",
               weekday, &day, month_name, &year, &hour, &minute, &second, zone) != 8)
        return false;

    month = fh_http_month_number(month_name);
    if (weekday[3] != ',' || month == 0 || day < 1 || day > 31 ||
        year < 1980 || year > 2107 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59 ||
        tolower((unsigned char)zone[0]) != 'g' ||
        tolower((unsigned char)zone[1]) != 'm' ||
        tolower((unsigned char)zone[2]) != 't')
        return false;

    memset(timestamp, 0, sizeof(*timestamp));
    timestamp->fdate = (WORD)(((year - 1980) << 9) | (month << 5) | day);
    timestamp->ftime = (WORD)((hour << 11) | (minute << 5) | (second / 2));
    return true;
}

static void fh_service_msx_reads_for(uint32_t duration_us)
{
    uint32_t start = time_us_32();
    while ((uint32_t)(time_us_32() - start) < duration_us)
    {
        fh_service_msx_reads();
        tight_loop_contents();
    }
}

static bool fh_deliver_http_body(const uint8_t *data, uint16_t len, fh_http_decoder_t *decoder,
                                 fh_http_body_callback_t callback, void *ctx)
{
    if (!decoder->chunked)
    {
        uint16_t take = len;
        if (decoder->has_content_length)
        {
            if (decoder->content_remaining == 0u)
            {
                decoder->done = true;
                return true;
            }
            if ((uint32_t)take > decoder->content_remaining)
                take = (uint16_t)decoder->content_remaining;
        }
        if (take != 0u && !callback(data, take, ctx))
            return false;
        if (decoder->has_content_length)
        {
            decoder->content_remaining -= take;
            if (decoder->content_remaining == 0u)
                decoder->done = true;
        }
        return true;
    }

    while (len > 0u && !decoder->done)
    {
        if (decoder->crlf_to_skip != 0u)
        {
            data++;
            len--;
            decoder->crlf_to_skip--;
            continue;
        }

        if (decoder->remaining == 0u)
        {
            uint8_t ch = *data++;
            len--;
            if (ch == '\n')
            {
                decoder->size_line[decoder->size_len] = '\0';
                decoder->remaining = (uint32_t)strtoul(decoder->size_line, NULL, 16);
                decoder->size_len = 0;
                if (decoder->remaining == 0u)
                    decoder->done = true;
            }
            else if (ch != '\r' && ch != ';' && decoder->size_len + 1u < sizeof(decoder->size_line))
            {
                decoder->size_line[decoder->size_len++] = (char)ch;
            }
            else if (ch == ';')
            {
                while (len > 0u && *data != '\n')
                {
                    data++;
                    len--;
                }
            }
            continue;
        }

        uint16_t take = len;
        if ((uint32_t)take > decoder->remaining)
            take = (uint16_t)decoder->remaining;
        if (take != 0u && !callback(data, take, ctx))
            return false;
        data += take;
        len = (uint16_t)(len - take);
        decoder->remaining -= take;
        if (decoder->remaining == 0u)
            decoder->crlf_to_skip = 2u;
    }

    return true;
}

// Builds the File Hunter request path. An empty query selects the latest
// releases endpoint (newest first, no name filter); any other query goes to the
// name-filtered catalog endpoint. Both return the same packed listing struct and
// take the same &download=<index> selector against the listing they produced.
static void fh_build_http_path(char *out, size_t out_size, const char *query, int download_index)
{
    size_t pos = 0;
    const char *suffix = "&download=";

    if (!query || !*query)
    {
        pos += snprintf(out + pos, out_size - pos, "%s", FH_LATEST_ENDPOINT "?base=1BA0&type=rom");
    }
    else
    {
        pos += snprintf(out + pos, out_size - pos, "%s", FH_ENDPOINT "?base=1BA0&type=rom&msx=&char=");
        for (; *query && pos + 4u < out_size; ++query)
        {
            unsigned char ch = (unsigned char)*query;
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
                out[pos++] = (char)ch;
            else if (ch == ' ')
                out[pos++] = '+';
        }
    }
    pos += snprintf(out + pos, out_size - pos, "%s", suffix);
    if (download_index >= 0)
        snprintf(out + pos, out_size - pos, "%d", download_index);
    else if (pos < out_size)
        out[pos] = '\0';
}

static bool fh_http_get_url(const char *url, fh_http_body_callback_t callback, void *ctx,
                            uint32_t idle_timeout_us, uint8_t redirect_depth, FILINFO *response_timestamp)
{
    fh_url_t parsed;
    uint8_t connection;
    char request[384];
    uint8_t chunk[FH_HTTP_CHUNK_SIZE];
    char headers[768];
    uint16_t header_len = 0;
    uint16_t bytes_read;
    uint32_t last_data;
    uint8_t header_state = 0;
    bool body_started = false;
    bool got_body = false;
    bool tcp_closed = false;
    fh_http_decoder_t decoder;
    int status_code = 0;

    memset(&decoder, 0, sizeof(decoder));
    if (!fh_parse_url(url, &parsed))
    {
        printf("[fh] HTTP parse failed: %s\n", url);
        fflush(stdout);
        return false;
    }

    printf("[fh] HTTP GET host=%s port=%u tls=%u path=%s\n", parsed.host, parsed.port, parsed.tls ? 1u : 0u, parsed.path);
    fflush(stdout);

    if (!fh_wifi_tcp_open_ex(parsed.host, parsed.port, parsed.tls, &connection))
    {
        printf("[fh] HTTP failed before send: TCP open\n");
        fflush(stdout);
        return false;
    }
    int request_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: FHBrowser (MSX PicoVerse)\r\nConnection: close\r\n\r\n",
        parsed.path, parsed.host);
    if (request_len <= 0 || request_len >= (int)sizeof(request))
    {
        fh_wifi_tcp_close(connection);
        return false;
    }
    if (!fh_wifi_tcp_send(connection, request, (uint16_t)request_len))
    {
        printf("[fh] HTTP send failed connection=%u\n", connection);
        fflush(stdout);
        fh_wifi_tcp_close(connection);
        return false;
    }
    printf("[fh] HTTP request sent, bytes=%d\n", request_len);
    fflush(stdout);

    last_data = time_us_32();
    while ((uint32_t)(time_us_32() - last_data) < idle_timeout_us)
    {
        if (!fh_wifi_tcp_receive(connection, chunk, sizeof(chunk), &bytes_read))
        {
            tcp_closed = true;
            if (got_body || body_started) break;
            continue;
        }
        if (bytes_read == 0)
        {
            fh_service_msx_reads_for(10000u);
            continue;
        }
        last_data = time_us_32();
        if (body_started)
        {
            if (!fh_deliver_http_body(chunk, bytes_read, &decoder, callback, ctx))
            {
                fh_wifi_tcp_close(connection);
                return false;
            }
            got_body = true;
            if (decoder.done) break;
            continue;
        }
        for (uint16_t i = 0; i < bytes_read; ++i)
        {
            uint8_t ch = chunk[i];
            if (!body_started)
            {
                if (header_len + 1u < sizeof(headers))
                    headers[header_len++] = (char)ch;
                if ((header_state == 0 && ch == '\r') ||
                    (header_state == 1 && ch == '\n') ||
                    (header_state == 2 && ch == '\r') ||
                    (header_state == 3 && ch == '\n'))
                {
                    header_state++;
                    if (header_state == 4)
                    {
                        headers[header_len] = '\0';
                        if (fh_starts_with(headers, "HTTP/"))
                        {
                            char *space = strchr(headers, ' ');
                            if (space) status_code = atoi(space + 1);
                        }
                        printf("[fh] HTTP status=%d\n", status_code);
                        fflush(stdout);
                        if (status_code >= 300 && status_code < 400 && redirect_depth < 4u)
                        {
                            const char *location = fh_find_header_value(headers, "Location");
                            if (location)
                            {
                                char redirect_url[256];
                                uint16_t pos = 0;
                                while (*location && *location != '\r' && *location != '\n' && pos + 1u < sizeof(redirect_url))
                                    redirect_url[pos++] = *location++;
                                redirect_url[pos] = '\0';
                                fh_wifi_tcp_close(connection);
                                if (redirect_url[0] == '/')
                                {
                                    char absolute_url[320];
                                    snprintf(absolute_url, sizeof(absolute_url), "%s://%s%s",
                                             parsed.tls ? "https" : "http", parsed.host, redirect_url);
                                    return fh_http_get_url(absolute_url, callback, ctx, idle_timeout_us,
                                                           (uint8_t)(redirect_depth + 1u), response_timestamp);
                                }
                                return fh_http_get_url(redirect_url, callback, ctx, idle_timeout_us,
                                                       (uint8_t)(redirect_depth + 1u), response_timestamp);
                            }
                        }
                        if (response_timestamp &&
                            !fh_parse_http_timestamp(fh_find_header_value(headers, "Last-Modified"), response_timestamp))
                            (void)fh_parse_http_timestamp(fh_find_header_value(headers, "Date"), response_timestamp);
                        decoder.chunked = fh_header_value_is_chunked(fh_find_header_value(headers, "Transfer-Encoding"));
                        if (!decoder.chunked && fh_parse_header_u32(fh_find_header_value(headers, "Content-Length"), &decoder.content_remaining))
                            decoder.has_content_length = true;
                        body_started = true;
                        if (i + 1u < bytes_read)
                        {
                            uint16_t body_len = (uint16_t)(bytes_read - i - 1u);
                            if (!fh_deliver_http_body(&chunk[i + 1u], body_len, &decoder, callback, ctx))
                            {
                                fh_wifi_tcp_close(connection);
                                return false;
                            }
                            got_body = true;
                        }
                        break;
                    }
                }
                else
                {
                    header_state = (ch == '\r') ? 1u : 0u;
                }
            }
        }
        if (body_started && !got_body)
            got_body = true;
        if (decoder.done) break;
    }

    fh_wifi_tcp_close(connection);
    if (!body_started)
    {
        printf("[fh] HTTP failed: no headers/body_started\n");
        fflush(stdout);
        return false;
    }
    if (decoder.chunked || decoder.has_content_length)
    {
        printf("[fh] HTTP done=%u chunked=%u content_length=%u remaining=%lu\n",
               decoder.done ? 1u : 0u,
               decoder.chunked ? 1u : 0u,
               decoder.has_content_length ? 1u : 0u,
               (unsigned long)decoder.content_remaining);
        fflush(stdout);
        return decoder.done;
    }
    printf("[fh] HTTP done=%u got_body=%u tcp_closed=%u no_length=1\n",
           (got_body && tcp_closed) ? 1u : 0u,
           got_body ? 1u : 0u,
           tcp_closed ? 1u : 0u);
    fflush(stdout);
    return got_body && tcp_closed;
}

// --- RP2350 QMI dual-chip-select lockup mitigation -------------------------
// File Hunter operations make Core 0 access PSRAM (the 256KB catalog buffer and
// the ROM download/save regions) through the QMI. If the audio Core 1 is
// simultaneously fetching code from flash-XIP (also via the QMI), the RP2350
// can permanently lock the QMI, which hangs Core 1 forever (audio dies and is
// unrecoverable in-session).
//
// Mitigation: before any File Hunter WiFi/PSRAM work, fully quiesce the audio
// Core 1. shutdown_mp3_core1_before_rom_launch() hands the live I2S pool to
// rom_audio_handoff_pool and resets Core 1. A halted Core 1 never fetches from
// flash, so Core 0 becomes the sole QMI user and cannot trip the dual-CS
// lockup. The audio pipeline is preserved (not torn down), so the next
// ensure_mp3_core1_started() re-adopts the pool without re-running
// audio_i2s_setup() (no PIO program / shared-IRQ-handler leak on the vendored
// pico_audio_i2s singleton).
//
// (An earlier multicore_lockout-based attempt parked Core 1 around each PSRAM
// burst, but repeated multicore_lockout_start_blocking() calls hung Core 0
// waiting for the victim to acknowledge - the 2nd lockout cycle never
// completed - which froze the MSX bus. Quiescing the core avoids the lockout
// entirely.)
static void fh_quiesce_audio_for_qmi(void)
{
    if (mp3_core1_started)
        shutdown_mp3_core1_before_rom_launch();
}

static bool fh_http_get_with_timestamp(const char *path, fh_http_body_callback_t callback, void *ctx,
                                       uint32_t idle_timeout_us, FILINFO *response_timestamp)
{
    char url[256];
    bool http_ok;

    // Halt the audio core before touching WiFi/PSRAM (see note above).
    fh_quiesce_audio_for_qmi();

    if (!fh_wifi_wait_connected(FH_WIFI_CONNECT_WAIT_US))
    {
        fh_set_offline_status();
        return false;
    }

    if (fh_starts_with(path, "http://") || fh_starts_with(path, "https://"))
        http_ok = fh_http_get_url(path, callback, ctx, idle_timeout_us, 0, response_timestamp);
    else
    {
        snprintf(url, sizeof(url), "http://" FH_HOST "%s", path);
        http_ok = fh_http_get_url(url, callback, ctx, idle_timeout_us, 0, response_timestamp);
    }
    if (http_ok) esp_link_note_traffic();
    return http_ok;
}

static bool fh_http_get(const char *path, fh_http_body_callback_t callback, void *ctx,
                        uint32_t idle_timeout_us)
{
    return fh_http_get_with_timestamp(path, callback, ctx, idle_timeout_us, NULL);
}

#if __has_include("../../../nexus/pico/nexus_tracker.inc")
#include "../../../nexus/pico/nexus_tracker.inc"
#else
static inline void nexus_tracker_checkin_once(void) { }
#endif

typedef struct {
    uint8_t *data;
    uint32_t len;
    uint32_t cap;
    bool overflowed;
} fh_list_download_t;

static bool fh_list_body_callback(const uint8_t *data, uint16_t len, void *ctx)
{
    fh_list_download_t *download = (fh_list_download_t *)ctx;
    if (download->len + len > download->cap)
    {
        download->overflowed = true;
        return false;
    }
    memcpy(download->data + download->len, data, len);
    download->len += len;
    return true;
}

static bool fh_parse_catalog(const uint8_t *data, uint32_t len)
{
    uint16_t raw_count = 0;
    uint32_t pos = 0;
    uint32_t base_ptr;
    uint32_t string_start;

    fh_catalog_count = 0;
    fh_catalog_message = false;
    if (len < 4u) return false;

    while (pos + 4u <= len)
    {
        uint32_t name_ptr = fh_read_le32(data + pos);
        if (name_ptr == 0u) break;
        if (pos + 7u > len) return false;
        raw_count++;
        pos += 7u;
    }
    if (pos + 4u > len) return false;
    if (raw_count == 0u)
    {
        fh_catalog_loaded = true;
        return true;
    }

    base_ptr = fh_read_le32(data);
    string_start = pos + 4u;
    pos = 0;
    for (uint16_t i = 0; i < raw_count && fh_catalog_count < FH_MAX_RESULTS; ++i, pos += 7u)
    {
        uint32_t name_ptr = fh_read_le32(data + pos);
        uint32_t name_offset;
        uint32_t source;
        uint16_t out = 0;

        if (name_ptr < base_ptr) continue;
        name_offset = string_start + (name_ptr - base_ptr);
        if (name_offset >= len) continue;

        memset(&fh_catalog[fh_catalog_count], 0, sizeof(fh_catalog[fh_catalog_count]));
        source = name_offset;
        while (source < len && data[source] != 0u && out < ROM_NAME_MAX)
            fh_catalog[fh_catalog_count].name[out++] = (char)data[source++];
        fh_catalog[fh_catalog_count].name[out] = '\0';
        fh_catalog[fh_catalog_count].size_kb = fh_read_le16(data + pos + 4u);
        if (fh_catalog[fh_catalog_count].name[0] != '\0')
            fh_catalog_count++;
    }

    fh_catalog_loaded = true;
    return true;
}

// Copies the MSX-supplied query out of the shared buffer. An empty buffer is a
// meaningful value here: it selects the latest-releases endpoint, so it must not
// be replaced with a name filter.
static void fh_copy_query_from_buffer(char *out, size_t out_size)
{
    size_t i;
    if (out_size == 0) return;
    for (i = 0; i + 1u < out_size && i < FH_QUERY_SIZE; ++i)
    {
        char ch = (char)fh_query[i];
        if (ch == '\0') break;
        out[i] = ch;
    }
    out[i] = '\0';
}

static bool fh_fetch_catalog(const char *query)
{
    char path[192];

    if (!fh_prepare_list_region()) return false;

    fh_list_download_t download = { fh_list_region.ptr, 0, fh_list_region.size, false };

    fh_build_http_path(path, sizeof(path), query, -1);
    if (!fh_http_get(path, fh_list_body_callback, &download, 3000000u))
    {
        if (download.overflowed)
        {
            fh_set_catalog_message("Too many results. Refine search");
            return true;
        }
        return false;
    }
    return fh_parse_catalog(download.data, download.len);
}

typedef struct {
    bool metadata_done;
    char metadata_line[128];
    uint8_t metadata_len;
    uint32_t expected_size;
    uint32_t written;
    uint16_t fallback_size_kb;
    bool failed;
} fh_psram_download_t;

static uint32_t fh_parse_download_size(const char *line, uint16_t fallback_size_kb)
{
    const char *ptr = strstr(line, "size:");
    uint32_t fallback_size = (uint32_t)fallback_size_kb * 1024u;
    uint32_t value = 0u;

    if (ptr)
    {
        value = (uint32_t)strtoul(ptr + 5, NULL, 10);
    }

    if (value == 0u)
    {
        ptr = line;
        while (*ptr == ' ' || *ptr == '\t') ptr++;
        if (tolower((unsigned char)ptr[0]) == 's' &&
            tolower((unsigned char)ptr[1]) == 'i' &&
            tolower((unsigned char)ptr[2]) == 'z' &&
            tolower((unsigned char)ptr[3]) == 'e' && ptr[4] == ':')
        {
            value = (uint32_t)strtoul(ptr + 5, NULL, 10);
        }
    }

    if (value < fallback_size) value = fallback_size;
    return value;
}

static bool fh_prepare_download_region(uint32_t size)
{
    if (size == 0u || size > SD_ROM_MAX_SIZE) return false;
    if (!psram_bring_up_once()) return false;

    if (size <= sd_rom_region.size)
    {
        fh_download_region = sd_rom_region;
        return true;
    }

    if (fh_download_region.ptr && fh_download_region.size >= size) return true;
    return psram_alloc(size, &fh_download_region);
}

static bool fh_psram_download_body_callback(const uint8_t *data, uint16_t len, void *ctx)
{
    fh_psram_download_t *download = (fh_psram_download_t *)ctx;
    const uint8_t *ptr = data;
    uint16_t remaining = len;

    while (!download->metadata_done && remaining > 0u)
    {
        uint8_t ch = *ptr++;
        remaining--;
        if (ch == '\n')
        {
            download->metadata_line[download->metadata_len] = '\0';
            download->expected_size = fh_parse_download_size(download->metadata_line, download->fallback_size_kb);
            if (!fh_prepare_download_region(download->expected_size))
            {
                download->failed = true;
                return false;
            }
            download->metadata_done = true;
            fh_progress_percent = 0;
            break;
        }
        if (ch != '\r' && download->metadata_len + 1u < sizeof(download->metadata_line))
            download->metadata_line[download->metadata_len++] = (char)ch;
    }

    if (!download->metadata_done)
        return true;

    if (remaining > 0u)
    {
        uint32_t to_copy = remaining;

        if (!fh_download_region.ptr || download->written > fh_download_region.size ||
            to_copy > (fh_download_region.size - download->written))
        {
            download->failed = true;
            return false;
        }

        memcpy(fh_download_region.ptr + download->written, ptr, to_copy);
        download->written += to_copy;
    }

    if (download->expected_size != 0u)
    {
        uint32_t percent = (download->written * 100u) / download->expected_size;
        if (percent > 100u) percent = 100u;
        fh_progress_percent = (uint16_t)percent;
    }

    fh_service_msx_reads();
    return true;
}

static bool fh_filename_has_rom_extension(const char *filename)
{
    size_t len = strlen(filename);
    const char *ext;
    if (len < 4u) return false;
    ext = filename + len - 4u;
    return ext[0] == '.' &&
           tolower((unsigned char)ext[1]) == 'r' &&
           tolower((unsigned char)ext[2]) == 'o' &&
           tolower((unsigned char)ext[3]) == 'm';
}

static bool fh_build_download_path(const char *name, char *path, size_t path_size)
{
    char filename[ROM_NAME_MAX + 5u];
    size_t out = 0;

    if (!path || path_size == 0u) return false;
    if (!name || *name == '\0') name = "FILEHUNT";

    for (size_t i = 0; name[i] != '\0' && out + 1u < sizeof(filename); ++i)
    {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 32u || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
        {
            ch = '_';
        }
        filename[out++] = (char)ch;
    }

    while (out > 0u && (filename[out - 1u] == ' ' || filename[out - 1u] == '.'))
        --out;

    if (out == 0u)
    {
        memcpy(filename, "FILEHUNT", 8u);
        out = 8u;
    }

    filename[out] = '\0';
    if (!fh_filename_has_rom_extension(filename) && out + 4u < sizeof(filename))
    {
        memcpy(filename + out, ".ROM", 5u);
    }

    return snprintf(path, path_size, "%s", filename) < (int)path_size;
}

static void fh_save_fail(const char *message)
{
    if (message)
        fh_copy_status_prefix(message);
    if (fh_save_file_open)
    {
        f_close(&fh_save_file);
        fh_save_file_open = false;
    }
    fatfs_set_fattime_override(0u);
    fh_save_close_pending = false;
    fh_result = 0u;
    fh_save_state = FH_SAVE_FAILED;
}

static void fh_save_background_work(void)
{
    FRESULT fr;

    if (fh_save_state != FH_SAVE_RUNNING) return;

    if (!fh_save_file_open && !fh_save_close_pending)
    {
        char path[SD_PATH_MAX];

        if (!fh_download_region.ptr || fh_download_size == 0u || fh_download_size > fh_download_region.size)
        {
            fh_save_fail("Save failed: buffer");
            return;
        }
        if (!sd_is_mounted())
        {
            fh_save_fail("Use F2 microSD first");
            return;
        }
        if (!fh_build_download_path(fh_download_name, path, sizeof(path)))
        {
            fh_save_fail("Save failed: path");
            return;
        }
        fatfs_set_fattime_override(((uint32_t)fh_download_timestamp.fdate << 16) |
                                   fh_download_timestamp.ftime);
        fr = f_open(&fh_save_file, path, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK)
        {
            fatfs_set_fattime_override(0u);
            snprintf(fh_wifi_status_text, sizeof(fh_wifi_status_text), "Save failed: open %u", (unsigned int)fr);
            fh_save_fail(NULL);
            return;
        }
        fh_save_file_open = true;
        return;
    }

    if (fh_save_close_pending)
    {
        fr = f_close(&fh_save_file);
        fatfs_set_fattime_override(0u);
        fh_save_file_open = false;
        fh_save_close_pending = false;
        if (fr != FR_OK)
        {
            snprintf(fh_wifi_status_text, sizeof(fh_wifi_status_text), "Save failed: close %u", (unsigned int)fr);
            fh_result = 0u;
            fh_save_state = FH_SAVE_FAILED;
            return;
        }
        fh_copy_status_prefix("Saved to microSD");
        fh_result = 1u;
        fh_save_state = FH_SAVE_DONE;
        set_root_path();
        refresh_requested = true;
        return;
    }

    if (fh_save_offset < fh_download_size)
    {
        UINT to_write = (UINT)((fh_download_size - fh_save_offset) > 512u ? 512u : (fh_download_size - fh_save_offset));
        UINT written = 0;

        memcpy(fh_sd_save_chunk, fh_download_region.ptr + fh_save_offset, to_write);
        fr = f_write(&fh_save_file, fh_sd_save_chunk, to_write, &written);
        if (fr != FR_OK || written != to_write)
        {
            snprintf(fh_wifi_status_text, sizeof(fh_wifi_status_text), "Save failed: write %u", (unsigned int)fr);
            fh_save_fail(NULL);
            return;
        }
        fh_save_offset += written;
        if (fh_save_offset >= fh_download_size)
            fh_save_close_pending = true;
    }
}

static bool fh_start_save_job(void)
{
    if (fh_save_state == FH_SAVE_RUNNING) return false;

    // The File Hunter save does FatFS on Core 0, so the live MP3/WAV core must
    // not touch the SD card during the save. Stop any active playback but keep
    // the core alive (while FH_SAVE_RUNNING, core1_bg_work() services only the
    // save and dispatches no MP3 commands, so the stopped core stays idle and
    // the save single-owns the SD card). NOTE: a File Hunter download currently
    // leaves the live audio pipeline unusable until a power cycle - reusing or
    // re-initialising pico_audio_i2s after a download is silent (see docs/log).
    if (mp3_core1_started &&
        (mp3_get_status() & (MP3_STATUS_PLAYING | MP3_STATUS_PAUSED)))
    {
        mp3_send_cmd(MP3_CMD_STOP);
        uint32_t deadline = time_us_32() + 500000u;
        while ((mp3_get_status() & (MP3_STATUS_PLAYING | MP3_STATUS_PAUSED)) &&
               (int32_t)(deadline - time_us_32()) > 0)
            sleep_us(500);
    }

    fh_save_offset = 0;
    fh_save_file_open = false;
    fh_save_close_pending = false;
    fh_result = FH_RESULT_SAVING;
    fh_save_state = FH_SAVE_RUNNING;
    return true;
}

static bool fh_download_selected(void)
{
    char path[192];
    fh_psram_download_t download;
    bool http_ok;
    bool ok;

    if (fh_selected_index >= fh_catalog_count) return false;

    if (((uint32_t)fh_catalog[fh_selected_index].size_kb * 1024u) > SD_ROM_MAX_SIZE)
    {
        fh_copy_status_prefix("Download blocked: >4MB");
        return false;
    }

    fh_build_http_path(path, sizeof(path), fh_active_query, (int)fh_selected_index);

    memset(&download, 0, sizeof(download));
    memset(&fh_download_timestamp, 0, sizeof(fh_download_timestamp));
    download.fallback_size_kb = fh_catalog[fh_selected_index].size_kb;
    fh_progress_percent = 0;
    http_ok = fh_http_get_with_timestamp(path, fh_psram_download_body_callback, &download, 20000000u,
                                         &fh_download_timestamp);
    ok = http_ok &&
         download.metadata_done && !download.failed &&
            download.written != 0u &&
            (download.expected_size == 0u || download.written >= download.expected_size);
    if (ok)
    {
        fh_progress_percent = 100u;
        fh_download_size = download.written;
        strncpy(fh_download_name, fh_catalog[fh_selected_index].name, sizeof(fh_download_name) - 1u);
        fh_download_name[sizeof(fh_download_name) - 1u] = '\0';
        fh_result = FH_RESULT_SAVING;
        fh_copy_status_prefix("Saving to microSD card...");
        fh_service_msx_reads_for(250000u);
        ok = fh_start_save_job();
    }
    else
    {
        uint32_t percent = 0;
        if (download.expected_size != 0u)
        {
            percent = (download.written * 100u) / download.expected_size;
            if (percent > 100u) percent = 100u;
        }
        if (!http_ok)
            snprintf(fh_wifi_status_text, sizeof(fh_wifi_status_text), "Download failed: TCP %u %lu%%", (unsigned int)fh_tcp_last_error, (unsigned long)percent);
        else if (!download.metadata_done)
            fh_copy_status_prefix("Download failed: no size");
        else if (download.failed)
            fh_copy_status_prefix("Download failed: data");
        else
            snprintf(fh_wifi_status_text, sizeof(fh_wifi_status_text), "Download failed: %lu%%", (unsigned long)percent);
    }
    return ok;
}

static void __not_in_flash_func(fh_build_page_buffer)(void)
{
    memset(page_buffer, 0, FH_RECORD_SIZE * FH_FILES_PER_PAGE);
    uint16_t start = (uint16_t)fh_page_index * FH_FILES_PER_PAGE;
    for (uint16_t row = 0; row < FH_FILES_PER_PAGE; row++)
    {
        uint16_t idx = start + row;
        if (idx >= fh_catalog_count)
            break;
        uint8_t *dst = &page_buffer[row * FH_RECORD_SIZE];
        strncpy((char *)dst, fh_catalog[idx].name, ROM_NAME_MAX);
        dst[FH_RECORD_FLAG_OFFSET] = fh_catalog_message ? FH_RECORD_FLAG_MESSAGE : 0u;
        uint16_t size_kb = fh_catalog[idx].size_kb;
        dst[FH_RECORD_SIZE_OFFSET] = (uint8_t)(size_kb & 0xFFu);
        dst[FH_RECORD_SIZE_OFFSET + 1u] = (uint8_t)((size_kb >> 8) & 0xFFu);
    }
}

static void __not_in_flash_func(fh_copy_status_prefix)(const char *prefix)
{
    uint16_t i = 0;
    while (i + 1u < FH_STATUS_TEXT_SIZE && prefix[i] != '\0')
    {
        fh_wifi_status_text[i] = prefix[i];
        ++i;
    }
    fh_wifi_status_text[i] = '\0';
}

static void __not_in_flash_func(fh_set_offline_status)(void)
{
    fh_copy_status_prefix("Offline");
}

static bool __not_in_flash_func(fh_wifi_wait_byte)(uint8_t *data_out, uint32_t deadline)
{
    while ((int32_t)(time_us_32() - deadline) < 0)
    {
        wifi_service_rx();
        fh_service_msx_reads();
        if (wifi_pop_rx_byte(data_out)) return true;
    }
    return false;
}

// Waits for the ESP-01 UART to fall silent before a query is sent. Leftover
// bytes from a previous transfer are the main reason a status query used to
// desynchronize and report a healthy module as offline.
static void __not_in_flash_func(esp_uart_settle)(void)
{
    uint32_t limit = time_us_32() + ESP_PROBE_QUIET_MAX_US;
    uint32_t quiet = time_us_32() + ESP_PROBE_QUIET_US;
    uint8_t drop;

    wifi_uart_init_once();
    while ((int32_t)(time_us_32() - limit) < 0)
    {
        wifi_service_rx();
        fh_service_msx_reads();
        if (wifi_pop_rx_byte(&drop))
        {
            quiet = time_us_32() + ESP_PROBE_QUIET_US;
            continue;
        }
        if ((int32_t)(time_us_32() - quiet) >= 0) break;
    }
    wifi_hw_rx_drain();
    wifi_reset_fifo();
}

// Reads one framed answer to the 'g' (get access point status) command. The
// reader keeps scanning for a plausible frame until the deadline instead of
// failing on the first mismatched byte, so a late reply from an earlier
// command cannot be mistaken for "no module".
static bool __not_in_flash_func(esp_read_ap_frame)(uint8_t *payload, uint16_t *payload_len,
                                                   uint32_t deadline)
{
    uint8_t data;

    while (fh_wifi_wait_byte(&data, deadline))
    {
        uint8_t size_hi;
        uint8_t size_lo;
        uint16_t size;
        uint16_t keep;
        bool complete = true;

        if (data != 'g') continue;                          // resync on the reply tag
        if (!fh_wifi_wait_byte(&data, deadline)) return false;
        if (data != 0u) continue;                           // not an OK answer
        if (!fh_wifi_wait_byte(&size_hi, deadline)) return false;
        if (!fh_wifi_wait_byte(&size_lo, deadline)) return false;

        size = (uint16_t)(((uint16_t)size_hi << 8) | size_lo);
        if (size < 2u || size > 34u) continue;              // implausible frame

        keep = size;
        if (keep > 33u) keep = 33u;
        for (uint16_t i = 0; i < size; ++i)
        {
            if (!fh_wifi_wait_byte(&data, deadline)) { complete = false; break; }
            if (i < keep) payload[i] = data;
        }
        if (!complete) return false;
        if (payload[0] > ESP_STATION_GOT_IP) continue;      // not a station status byte

        *payload_len = keep;
        return true;
    }
    return false;
}

static bool __not_in_flash_func(fh_wifi_query_ap_status)(uint8_t *payload, uint16_t *payload_len,
                                                         uint32_t timeout_us)
{
    esp_uart_settle();
    uart_putc_raw(WIFI_UART_INSTANCE, 'g');
    wifi_tx_busy_deadline_us = time_us_32() + wifi_uart_frame_time_us();
    return esp_read_ap_frame(payload, payload_len, time_us_32() + timeout_us);
}

// Probes the ESP-01 with retries. Returns true when the module answered at
// all; *online tells whether it is joined to an access point.
static bool __not_in_flash_func(esp_probe_link)(bool *online)
{
    uint32_t connect_deadline = time_us_32() + ESP_CONNECTING_WAIT_US;
    // Once a thorough probe has established that no module is fitted, keep the
    // later checks short so polling the indicator never stalls the MSX.
    uint8_t max_attempts = (esp_link_probed && !esp_link_module_present) ? 1u : ESP_PROBE_ATTEMPTS;
    bool answered = false;

    *online = false;
    esp_link_ssid[0] = '\0';

    for (;;)
    {
        for (uint8_t attempt = 0; attempt < max_attempts; ++attempt)
        {
            uint8_t payload[34];
            uint16_t payload_len = 0;

            if (fh_wifi_query_ap_status(payload, &payload_len, ESP_PROBE_TIMEOUT_US))
            {
                answered = true;
                // STATION_GOT_IP is the only state that means "joined". The
                // SSID that follows is informational: the firmware can return
                // it empty while the link is perfectly up, which the previous
                // check reported as offline.
                if (payload[0] == ESP_STATION_GOT_IP)
                {
                    uint16_t i = 1u;
                    while (i < payload_len && (i - 1u) < (sizeof(esp_link_ssid) - 1u) &&
                           payload[i] != 0u)
                    {
                        esp_link_ssid[i - 1u] = (char)payload[i];
                        ++i;
                    }
                    esp_link_ssid[i - 1u] = '\0';
                    *online = true;
                    return true;
                }
                if (payload[0] != ESP_STATION_CONNECTING) return true;
                break;  // still joining: wait below and probe again
            }
            // A truncated command leaves the ESP-01 receive parser waiting for
            // data; it only falls back to idle after 250 ms, so pause before
            // retrying instead of declaring the module missing.
            if ((attempt + 1u) < max_attempts)
                fh_service_msx_reads_for(ESP_PARSER_RESET_US);
        }

        if (!answered) return false;
        if ((int32_t)(time_us_32() - connect_deadline) >= 0) return true;
        fh_service_msx_reads_for(250000u);
    }
}

// Single link check used by callers that only need a yes/no answer (the Nexus
// tracker include). Returns false when the module did not answer at all.
static bool __not_in_flash_func(fh_wifi_get_ap_connected)(bool *connected)
{
    bool online = false;
    bool answered = esp_probe_link(&online);

    esp_link_store(answered, online);
    *connected = (esp_net_status == NET_STATUS_ONLINE);
    return answered || *connected;
}

static bool __not_in_flash_func(fh_wifi_wait_connected)(uint32_t timeout_us)
{
    uint32_t deadline = time_us_32() + timeout_us;

    for (;;)
    {
        bool online = false;
        bool answered = esp_probe_link(&online);

        esp_link_store(answered, online);
        if (esp_net_status == NET_STATUS_ONLINE) return true;
        if (!answered) return false;  // no module fitted: do not stall the transfer
        if ((int32_t)(time_us_32() - deadline) >= 0) return false;
        fh_copy_status_prefix("Waiting for Wi-Fi...");
        fh_service_msx_reads_for(250000u);
    }
}

// Refreshes the cached ESP-01 link state exposed through CTRL_NET_STATUS.
// Probing is rate limited so the MSX can poll the indicator freely, and the
// back-off is much longer when no module answered at all.
static void __not_in_flash_func(esp_link_refresh)(bool force)
{
    uint32_t ttl = esp_link_module_present ? ESP_CACHE_PRESENT_US : ESP_CACHE_ABSENT_US;
    bool online = false;
    bool answered;

    if (!force && esp_link_probed && (uint32_t)(time_us_32() - esp_link_checked_us) < ttl)
        return;

    answered = esp_probe_link(&online);
    esp_link_store(answered, online);
}

static void __not_in_flash_func(fh_update_wifi_status_text)(void)
{
    esp_link_refresh(false);

    if (esp_net_status != NET_STATUS_ONLINE)
    {
        fh_set_offline_status();
        return;
    }
    if (esp_link_ssid[0] == '\0')
    {
        fh_copy_status_prefix("Connected to network");
        return;
    }

    // Kept short so the text stays clear of the control bytes that share the
    // status window (CTRL_VDP_FREQ / CTRL_NET_STATUS at 0xBFA0).
    {
        const char *prefix = "Connected to ";
        uint16_t out = 0;
        uint16_t i;

        while (out < 31u && prefix[out] != '\0')
        {
            fh_wifi_status_text[out] = prefix[out];
            ++out;
        }
        for (i = 0; out < 31u && esp_link_ssid[i] != '\0'; ++i)
            fh_wifi_status_text[out++] = esp_link_ssid[i];
        fh_wifi_status_text[out] = '\0';
    }
}

static inline void __not_in_flash_func(handle_fh_write)(uint16_t addr, uint8_t data, void *ctx)
{
    bool *exit_requested = (bool *)ctx;

    if (addr == MONITOR_ADDR && data == ROM_SELECT_MENU)
    {
        *exit_requested = true;
        fh_result = 1;
        fh_status = FH_STATUS_READY;
        return;
    }

    if (addr >= FH_QUERY_BASE && addr < (FH_QUERY_BASE + FH_QUERY_SIZE))
    {
        fh_query[addr - FH_QUERY_BASE] = data;
        return;
    }

    if (addr == FH_PAGE)
    {
        fh_page_index = data;
        return;
    }
    if (addr == FH_SELECT_L)
    {
        fh_selected_index = (uint16_t)((fh_selected_index & 0xFF00u) | data);
        return;
    }
    if (addr == FH_SELECT_H)
    {
        fh_selected_index = (uint16_t)((fh_selected_index & 0x00FFu) | ((uint16_t)data << 8));
        return;
    }
    if (addr == FH_CMD)
    {
        fh_status = FH_STATUS_BUSY;
        if (data == FH_CMD_LIST_PAGE || data == FH_CMD_SEARCH)
        {
            if (data == FH_CMD_SEARCH || !fh_catalog_loaded)
            {
                fh_copy_query_from_buffer(fh_active_query, sizeof(fh_active_query));
                if (!fh_fetch_catalog(fh_active_query))
                    fh_set_catalog_message("Offline or File Hunter unavailable");
                else
                {
                    printf("[nexus] File Hunter catalog loaded, invoking tracker\n");
                    fflush(stdout);
                    nexus_tracker_checkin_once();
                    printf("[nexus] tracker call returned\n");
                    fflush(stdout);
                    if (fh_catalog_count == 0u)
                        fh_set_catalog_message("No File Hunter ROMs found");
                }
            }
            fh_build_page_buffer();
            fh_result = fh_catalog_count ? 1u : 0u;
        }
        else if (data == FH_CMD_DOWNLOAD)
        {
            fh_progress_percent = 0;
            fh_result = 0;
            if (!fh_download_selected())
                fh_result = 0u;
        }
        else if (data == FH_CMD_EXIT)
        {
            *exit_requested = true;
            fh_result = 1;
        }
        else if (data == FH_CMD_WIFI_STATUS)
        {
            fh_update_wifi_status_text();
            fh_result = 1;
        }
        fh_status = FH_STATUS_READY;
        return;
    }
}

static int __no_inline_not_in_flash_func(loadrom_filehunter)(void)
{
    msx_pio_bus_init();

    bool exit_requested = false;
    fh_page_index = 0;
    fh_result = 0;
    fh_status = FH_STATUS_READY;
    memset(fh_query, 0, sizeof(fh_query));
    memset(fh_active_query, 0, sizeof(fh_active_query));
    fh_catalog_count = 0;
    fh_catalog_loaded = false;
    fh_set_catalog_message("Retrieving File Hunter ROMs...");
    fh_set_offline_status();
    wifi_uart_init_once();

    while (true)
    {
        wifi_service_rx();

        uint16_t waddr;
        uint8_t wdata;
        while (pio_try_get_write(&waddr, &wdata))
        {
            handle_fh_write(waddr, wdata, &exit_requested);
        }

        if (pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            if (exit_requested &&
                !gpio_get(PIN_RD) &&
                ((gpio_get_all() & 0xFFFFu) == 0x0000u))
            {
                return ROM_SELECT_MENU;
            }
            tight_loop_contents();
            continue;
        }

        uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
        bool in_window = false;
        uint8_t data = 0xFFu;

        if (exit_requested && addr == 0x0000u)
        {
            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(false, 0xFFu));
            return ROM_SELECT_MENU;
        }

        data = fh_read_window_byte(addr, &in_window);

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }

    return ROM_SELECT_MENU;
}

//load the MSX Menu ROM into the MSX
int __no_inline_not_in_flash_func(loadrom_msx_menu)(uint32_t offset)
{
    if (!psram_prepare_rom_cache())
    {
        return 0xFFFF;
    }

    //setup the rom_sram buffer for the 32KB ROM
    gpio_init(PIN_WAIT); // Init wait signal pin
    gpio_set_dir(PIN_WAIT, GPIO_OUT); // Set the WAIT signal as output
    gpio_put(PIN_WAIT, 0); // Wait until we are ready to read the ROM
    memset(rom_sram, 0, MENU_ROM_SIZE); // Clear the SRAM buffer
    memcpy(rom_sram, flash_rom + offset, MENU_ROM_SIZE); // Load full 32KB menu ROM
    gpio_set_dir(PIN_WAIT, GPIO_IN); // Lets go!

    int record_count = 0; // Record count
    const uint8_t *record_ptr = flash_rom + offset + MENU_ROM_SIZE; // Pointer to the ROM records
    for (int i = 0; i < MAX_FLASH_RECORDS; i++)      // Read the ROMs from the configuration area
    {
        if (isEndOfData(record_ptr)) {
            break; // Stop if end of data is reached
        }
        char flash_name[ROM_NAME_MAX + 1];
        memcpy(flash_name, record_ptr, ROM_NAME_MAX);
        flash_name[ROM_NAME_MAX] = '\0';
        memset(flash_records[record_count].Name, 0, sizeof(flash_records[record_count].Name));
        trim_name_copy(flash_records[record_count].Name, flash_name);
        record_ptr += ROM_NAME_MAX; // Move the pointer to the next field
        flash_records[record_count].Mapper = *record_ptr++; // Read the mapper code
        flash_records[record_count].Size = read_ulong(record_ptr); // Read the ROM size
        record_ptr += sizeof(unsigned long); // Move the pointer to the next field
        flash_records[record_count].Offset = read_ulong(record_ptr); // Read the ROM offset
        record_ptr += sizeof(unsigned long); // Move the pointer to the next record
        record_count++; // Increment the record count
    }
    flash_record_count = (uint16_t)record_count;
    set_root_path();
    refresh_records_for_current_path();

    // Core 1 is intentionally NOT launched here. Folder navigation,
    // mapper detection, and SD enumeration all run on Core 0 from the
    // menu loop's idle branch, so the SD card has a single owner and
    // Core 1 cannot contend on shared buses. Core 1 is lazy-started
    // by ensure_mp3_core1_started() the first time an MP3 command is
    // dispatched. The cross-core command queue is a small FIFO so
    // SELECT+PLAY (issued ~10 ms apart by the menu) both fit while
    // mp3_init() is still running on Core 1.

    uint16_t rom_index = 0;
    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
    bool rom_selected = false; // ROM selected flag
    rom_cached_size = MENU_ROM_SIZE;
    msx_pio_bus_init();

    explorer_menu_ctx_t menu_ctx = {0};

    while (true)
    {
        pio_drain_writes(handle_menu_write_explorer, &menu_ctx);

        if (pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read)) {
            // PIO idle (SLTSL high).  On MSX1 the BIOS never selects
            // the cartridge at addr 0x0000 after rst 0x00, so detect
            // the reboot on the raw bus via GPIO.
            if (menu_ctx.rom_selected &&
                !gpio_get(PIN_RD) &&
                ((gpio_get_all() & 0xFFFFu) == 0x0000u))
            {
                return menu_ctx.rom_index;
            }
            // Background work runs on Core 0 during MSX bus idle: chunked
            // SD directory refresh, mapper detection, and bridging MP3
            // commands to Core 1 (lazy-launched on first MP3 command).
            core1_bg_work();
            tight_loop_contents();
            continue;
        }

        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        pio_drain_writes(handle_menu_write_explorer, &menu_ctx);

        bool in_window = false;
        uint8_t data = 0xFFu;
        bool drive = false;

        if (menu_ctx.rom_selected && addr == 0x0000u)
        {
            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(false, 0xFFu));
            return menu_ctx.rom_index;
        }

        if (addr >= 0x4000u && addr <= 0xBFFFu)
        {
            in_window = true;
            drive = true;

            if (addr >= CTRL_BASE_ADDR && addr <= (CTRL_BASE_ADDR + 0x0F))
            {
                if (fh_menu_window_active)
                {
                    switch (addr)
                    {
                        case CTRL_COUNT_L: data = (uint8_t)(fh_catalog_count & 0xFFu); break;
                        case CTRL_COUNT_H: data = (uint8_t)((fh_catalog_count >> 8) & 0xFFu); break;
                        case CTRL_PAGE:    data = fh_page_index; break;
                        case CTRL_STATUS:  data = fh_status; break;
                        case CTRL_CMD:     data = ctrl_cmd_state; break;
                        case CTRL_MATCH_L: data = (uint8_t)(fh_selected_index & 0xFFu); break;
                        case CTRL_MATCH_H: data = (uint8_t)((fh_selected_index >> 8) & 0xFFu); break;
                        case CTRL_MAPPER:  data = (uint8_t)(fh_progress_percent & 0xFFu); break;
                        case CTRL_ACK:     data = (uint8_t)((fh_progress_percent >> 8) & 0xFFu); break;
                        case CTRL_AUDIO:   data = fh_result; break;
                        case CTRL_WIFI_SUPPORT: data = 0; break;
                        case CTRL_PSG_EMULATION: data = 0; break;
                        case CTRL_WAVEGAME_ROM: data = 0; break;
                        case CTRL_SD_PARTITION: data = 0; break;
                        case CTRL_SD_BROWSE_PARTITION: data = 0; break;
                        case CTRL_AUDIO_VOLUME: data = 0; break;
                    }
                }
                else switch (addr)
                {
                    case CTRL_COUNT_L: data = (uint8_t)(total_record_count & 0xFFu); break;
                    case CTRL_COUNT_H: data = (uint8_t)((total_record_count >> 8) & 0xFFu); break;
                    case CTRL_PAGE:    data = current_page; break;
                    case CTRL_STATUS:  data = ctrl_status_value; break;
                    case CTRL_CMD:     data = ctrl_cmd_state; break;
                    case CTRL_MATCH_L: data = (uint8_t)(match_index & 0xFFu); break;
                    case CTRL_MATCH_H: data = (uint8_t)((match_index >> 8) & 0xFFu); break;
                    case CTRL_MAPPER:  data = ctrl_mapper_value; break;
                    case CTRL_ACK:     data = ctrl_ack_value; break;
                    case CTRL_AUDIO:   data = ctrl_audio_selection; break;
                    case CTRL_WIFI_SUPPORT: data = ctrl_wifi_support; break;
                    case CTRL_PSG_EMULATION: data = ctrl_psg_emulation; break;
                    case CTRL_WAVEGAME_ROM: data = ctrl_wavegame_rom; break;
                    case CTRL_SD_PARTITION: data = ctrl_sd_partition; break;
                    case CTRL_SD_BROWSE_PARTITION: data = sd_browse_partition; break;
                    case CTRL_AUDIO_VOLUME: data = ctrl_audio_volume; break;
                }
            }
            else if (addr >= MP3_CTRL_BASE && addr <= (MP3_CTRL_BASE + 0x0F))
            {
                uint16_t elapsed = mp3_get_elapsed_seconds();
                uint16_t total = mp3_get_total_seconds();
                switch (addr)
                {
                    case MP3_CTRL_STATUS:    data = mp3_get_status(); break;
                    case MP3_CTRL_ELAPSED_L: data = (uint8_t)(elapsed & 0xFFu); break;
                    case MP3_CTRL_ELAPSED_H: data = (uint8_t)((elapsed >> 8) & 0xFFu); break;
                    case MP3_CTRL_TOTAL_L:   data = (uint8_t)(total & 0xFFu); break;
                    case MP3_CTRL_TOTAL_H:   data = (uint8_t)((total >> 8) & 0xFFu); break;
                    case MP3_CTRL_INDEX_L:   data = (uint8_t)(mp3_selected_index & 0xFFu); break;
                    case MP3_CTRL_INDEX_H:   data = (uint8_t)((mp3_selected_index >> 8) & 0xFFu); break;
                    case MP3_CTRL_MODE:      data = mp3_play_mode; break;
                }
            }
            else if (addr >= CTRL_SD_PARTITION_INFO_BASE && addr < (CTRL_SD_PARTITION_INFO_BASE + CTRL_SD_PARTITION_INFO_SIZE))
            {
                data = ctrl_sd_partition_info[addr - CTRL_SD_PARTITION_INFO_BASE];
            }
            else if (addr >= CTRL_CHIP_ID_BASE && addr < (CTRL_CHIP_ID_BASE + CTRL_CHIP_ID_SIZE))
            {
                data = (uint8_t)pico_chip_id[addr - CTRL_CHIP_ID_BASE];
            }
            else if (!fh_menu_window_active && addr == CTRL_VDP_FREQ)
            {
                data = ctrl_vdp_frequency;
            }
            else if (!fh_menu_window_active && addr == CTRL_CPU_MODE)
            {
                data = ctrl_cpu_mode;
            }
            else if (addr == CTRL_NET_STATUS)
            {
                data = esp_net_status;
            }
            else if (fh_menu_window_active && addr >= FH_STATUS_TEXT_BASE && addr < (FH_STATUS_TEXT_BASE + FH_STATUS_TEXT_SIZE))
            {
                data = (uint8_t)fh_wifi_status_text[addr - FH_STATUS_TEXT_BASE];
            }
            else if (addr >= DATA_BASE_ADDR && addr < (DATA_BASE_ADDR + DATA_BUFFER_SIZE))
            {
                data = page_buffer[addr - DATA_BASE_ADDR];
            }
            else
            {
                uint32_t rel = addr - 0x4000u;
                if (rel < MENU_ROM_SIZE)
                {
                    data = rom_sram[rel];
                }
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(drive, data));
    }
    // Old implementation disabled
    /*
    while (true)
    {
        // Check control signals
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)

        uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
        //uint16_t addr = sio_hw->gpio_in & 0x00FFFF;
        if (sltsl) 
        {
            bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low, not used)

            if (wr && addr >= CTRL_QUERY_BASE && addr < (CTRL_QUERY_BASE + CTRL_QUERY_SIZE))
            {
                    uint8_t value = (gpio_get_all() >> 16) & 0xFF;
                    filter_query[addr - CTRL_QUERY_BASE] = (char)value;
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);
            }

            if (wr && addr == CTRL_CMD)
            {
                    uint8_t cmd = (gpio_get_all() >> 16) & 0xFF;
                    ctrl_cmd_state = cmd;
                    if (cmd == CMD_APPLY_FILTER)
                    {
                        filter_query[CTRL_QUERY_SIZE - 1] = '\0';
                        apply_filter();
                        current_page = 0;
                        build_page_buffer(current_page);
                    }
                    else if (cmd == CMD_FIND_FIRST)
                    {
                        filter_query[CTRL_QUERY_SIZE - 1] = '\0';
                        find_first_match();
                    }
                    else if (cmd == CMD_ENTER_DIR)
                    {
                        process_enter_dir_request();
                        ctrl_cmd_state = CMD_ENTER_DIR;
                    }
                    else if (cmd == CMD_DETECT_MAPPER)
                    {
                        uint16_t index = (uint16_t)((uint8_t)filter_query[0]) |
                                         (uint16_t)(((uint8_t)filter_query[1]) << 8);
                        detect_mapper_index = index;
                        detect_mapper_pending = true;
                        ctrl_mapper_value = 0;
                        ctrl_cmd_state = CMD_DETECT_MAPPER;
                    }
                    else if (cmd == CMD_SET_MAPPER)
                    {
                        uint16_t index = (uint16_t)((uint8_t)filter_query[0]) |
                                         (uint16_t)(((uint8_t)filter_query[1]) << 8);
                        uint8_t mapper = (uint8_t)filter_query[2];
                        ctrl_ack_value = 0;

                        if (index < total_record_count && mapper != 0 && !is_system_mapper(mapper) && mapper < MAPPER_DESCRIPTION_COUNT) {
                            uint16_t record_index = filtered_indices[index];
                            if (record_index < full_record_count) {
                                ROMRecord *rec = &records[record_index];
                                uint8_t flags = rec->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG | MP3_FLAG);
                                if ((flags & (FOLDER_FLAG | MP3_FLAG)) == 0) {
                                    rec->Mapper = (uint8_t)(flags | mapper);
                                    ctrl_ack_value = 1;
                                }
                            }
                        }
                        ctrl_cmd_state = 0;
                    }
                    else if (cmd == CMD_SET_SOURCE)
                    {
                        uint8_t mode = (uint8_t)filter_query[0];
                        if (mode > SOURCE_MODE_SD) {
                            mode = SOURCE_MODE_ALL;
                        }
                        cancel_refresh_work();
                        browse_source_mode = mode;
                        sd_current_path[0] = '/';
                        sd_current_path[1] = '\0';
                        memset(filter_query, 0, sizeof(filter_query));
                        refresh_requested = true;
                        ctrl_cmd_state = 0;
                    }
                    else if (cmd == CMD_PREPARE_QUICK_RUN)
                    {
                        process_prepare_quick_run_request();
                        ctrl_cmd_state = 0;
                    }
                    else if (cmd == CMD_DELETE_SD_FILE)
                    {
                        process_delete_sd_file_request();
                    }
                    if (cmd != CMD_ENTER_DIR && cmd != CMD_DETECT_MAPPER && cmd != CMD_DELETE_SD_FILE) {
                        ctrl_cmd_state = 0;
                    }
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);
            }

            if (wr && addr == MP3_CTRL_INDEX_L)
            {
                    uint8_t value = (gpio_get_all() >> 16) & 0xFF;
                    mp3_selected_index = (uint16_t)((mp3_selected_index & 0xFF00u) | value);
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);
            }

            if (wr && addr == MP3_CTRL_INDEX_H)
            {
                    uint8_t value = (gpio_get_all() >> 16) & 0xFF;
                    mp3_selected_index = (uint16_t)((mp3_selected_index & 0x00FFu) | ((uint16_t)value << 8));
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);
            }

            if (wr && addr == MP3_CTRL_CMD)
            {
                    uint8_t cmd = (gpio_get_all() >> 16) & 0xFF;

                    if (cmd == MP3_CMD_SELECT)
                    {
                        if (mp3_selected_index < total_record_count)
                        {
                            mp3_pending_index = mp3_selected_index;
                            mp3_pending_select = true;
                        }
                    }
                    else if (cmd == MP3_CMD_PLAY || cmd == MP3_CMD_STOP ||
                             cmd == MP3_CMD_PAUSE || cmd == MP3_CMD_RESUME ||
                             cmd == MP3_CMD_TOGGLE_MUTE || cmd == MP3_CMD_NEXT ||
                             cmd == MP3_CMD_PREVIOUS)
                    {
                        mp3_pending_cmd = cmd;
                    }

                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);
            }

            if (wr && addr == CTRL_PAGE)
            {
                    uint8_t page = (gpio_get_all() >> 16) & 0xFF;
                    if (page != current_page)
                    {
                        current_page = page;
                        build_page_buffer(current_page);
                    }
                    while (!(gpio_get(PIN_WR))) { // Wait until the write cycle completes (WR goes high)
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
            }

            if (wr && addr == MONITOR_ADDR) // Monitor ROM address to select the ROM
            {   
                    uint8_t selected_index = (gpio_get_all() >> 16) & 0xFF;
                    if (selected_index < total_record_count) {
                        rom_index = filtered_indices[selected_index];
                    } else {
                        rom_index = 0;
                    }
                    while (!(gpio_get(PIN_WR))) { // Wait until the write cycle completes (WR goes high){
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
                    rom_selected = true;    // ROM selected
            }

            if (addr >= 0x4000 && addr <= 0xBFFF) // Check if the address is within the ROM range
            {   
                if (rd)
                {
                    if (addr >= CTRL_BASE_ADDR && addr <= (CTRL_BASE_ADDR + 0x0F))
                    {
                        uint8_t ctrl_value = 0xFF;
                        switch (addr)
                        {
                            case CTRL_COUNT_L:
                                ctrl_value = (uint8_t)(total_record_count & 0xFFu);
                                break;
                            case CTRL_COUNT_H:
                                ctrl_value = (uint8_t)((total_record_count >> 8) & 0xFFu);
                                break;
                            case CTRL_PAGE:
                                ctrl_value = current_page;
                                break;
                            case CTRL_STATUS:
                                ctrl_value = ctrl_status_value;
                                break;
                            case CTRL_CMD:
                                ctrl_value = ctrl_cmd_state;
                                break;
                            case CTRL_MATCH_L:
                                ctrl_value = (uint8_t)(match_index & 0xFFu);
                                break;
                            case CTRL_MATCH_H:
                                ctrl_value = (uint8_t)((match_index >> 8) & 0xFFu);
                                break;
                            case CTRL_MAPPER:
                                ctrl_value = ctrl_mapper_value;
                                break;
                            case CTRL_ACK:
                                ctrl_value = ctrl_ack_value;
                                break;
                        }
                        gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                        gpio_put_masked(0xFF0000, (uint32_t)ctrl_value << 16);
                        while (!(gpio_get(PIN_RD))) {
                            tight_loop_contents();
                        }
                        gpio_set_dir_in_masked(0xFF << 16);
                        continue;
                    }

                    if (addr >= MP3_CTRL_BASE && addr <= (MP3_CTRL_BASE + 0x0F))
                    {
                        uint8_t ctrl_value = 0xFF;
                        uint16_t elapsed = mp3_get_elapsed_seconds();
                        uint16_t total = mp3_get_total_seconds();

                        switch (addr)
                        {
                            case MP3_CTRL_STATUS:
                                ctrl_value = mp3_get_status();
                                break;
                            case MP3_CTRL_ELAPSED_L:
                                ctrl_value = (uint8_t)(elapsed & 0xFFu);
                                break;
                            case MP3_CTRL_ELAPSED_H:
                                ctrl_value = (uint8_t)((elapsed >> 8) & 0xFFu);
                                break;
                            case MP3_CTRL_TOTAL_L:
                                ctrl_value = (uint8_t)(total & 0xFFu);
                                break;
                            case MP3_CTRL_TOTAL_H:
                                ctrl_value = (uint8_t)((total >> 8) & 0xFFu);
                                break;
                            case MP3_CTRL_INDEX_L:
                                ctrl_value = (uint8_t)(mp3_selected_index & 0xFFu);
                                break;
                            case MP3_CTRL_INDEX_H:
                                ctrl_value = (uint8_t)((mp3_selected_index >> 8) & 0xFFu);
                                break;
                        }
                        gpio_set_dir_out_masked(0xFF << 16);
                        gpio_put_masked(0xFF0000, (uint32_t)ctrl_value << 16);
                        while (!(gpio_get(PIN_RD))) {
                            tight_loop_contents();
                        }
                        gpio_set_dir_in_masked(0xFF << 16);
                        continue;
                    }

                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    uint8_t data;
                    if (addr >= DATA_BASE_ADDR && addr < (DATA_BASE_ADDR + DATA_BUFFER_SIZE)) {
                        data = page_buffer[addr - DATA_BASE_ADDR];
                    } else {
                        uint32_t rom_addr = (uint32_t)(addr - 0x4000); // Offset within 32KB menu ROM
                        data = rom_sram[rom_addr];
                    }
                    gpio_put_masked(0xFF0000, (uint32_t)data << 16); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD))) { // Wait until the read cycle completes (RD goes high)
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
                }
            } 
        }

        if (rd && addr == 0x0000 && rom_selected)   // lets return the rom_index and load the selected ROM
        {
            return rom_index;
        }
    }
    */
}

// loadrom_plain32 - Load a simple 32KB (or less) ROM into the MSX directly from the pico flash
// 32KB ROMS have two pages of 16Kb each in the following areas:
// 0x4000-0x7FFF and 0x8000-0xBFFF
// AB is on 0x0000, 0x0001
// 16KB ROMS have only one page in the 0x4000-0x7FFF area
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_plain32)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 32768u, &rom_base, &available_length);

    msx_pio_bus_init();

    while (true)
    {
        wavegame_service_io();
        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);
        wavegame_service_io();

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint32_t rel = addr - 0x4000u;
            if (available_length == 0u || rel < available_length)
                data = read_rom_byte(rom_base, rel);
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// loadrom_linear48 - Load a simple 48KB Linear0 ROM into the MSX directly from the pico flash
// Those ROMs have three pages of 16Kb each in the following areas:
// 0x0000-0x3FFF, 0x4000-0x7FFF and 0x8000-0xBFFF
// AB is on 0x4000, 0x4001
void __no_inline_not_in_flash_func(loadrom_linear48)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 49152u, &rom_base, &available_length);

    msx_pio_bus_init();

    while (true)
    {
        wavegame_service_io();
        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);
        wavegame_service_io();

        bool in_window = (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;
        if (in_window && (available_length == 0u || addr < available_length))
            data = read_rom_byte(rom_base, addr);

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// loadrom_konamiscc - Load a any Konami SCC ROM into the MSX directly from the pico flash
// The KonamiSCC ROMs are divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments 
// into the MSX's address space. Since the size of the mapper is 8Kb, the memory banks are:
// Bank 1: 4000h - 5FFFh , Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
// And the address to change banks are:
// Bank 1: 5000h - 57FFh (5000h used), Bank 2: 7000h - 77FFh (7000h used), Bank 3: 9000h - 97FFh (9000h used), Bank 4: B000h - B7FFh (B000h used)
// AB is on 0x0000, 0x0001
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
        struct audio_buffer *buffer = NULL;
        while (!buffer)
        {
            main_psg_service_io();
            buffer = take_audio_buffer(scc_audio_pool, false);
            if (!buffer)
                tight_loop_contents();
        }
        int16_t *samples = (int16_t *)buffer->buffer->bytes;
        for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
        {
            main_psg_service_io();
            int16_t raw = SCC_calc(&scc_instance);
            int32_t boosted = (int32_t)raw << SCC_VOLUME_SHIFT;
            if (boosted > 32767) boosted = 32767;
            else if (boosted < -32768) boosted = -32768;
            int16_t s = apply_audio_volume(boosted + main_psg_calc_sample());
            samples[i * 2]     = s;  // left
            samples[i * 2 + 1] = s;  // right
        }
        buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
        give_audio_buffer(scc_audio_pool, buffer);
    }
}

// -----------------------------------------------------------------------
// I2S audio initialisation for SCC emulation
// -----------------------------------------------------------------------
static void i2s_audio_init_scc(void)
{
    gpio_init(I2S_MUTE_PIN);
    gpio_set_dir(I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(I2S_MUTE_PIN, 1);

    struct audio_buffer_pool *handoff_pool = claim_rom_audio_handoff_pool();
    if (handoff_pool)
    {
        scc_audio_pool = handoff_pool;
        scc_audio_started = true;
        return;
    }

    static audio_format_t scc_audio_format = {
        .sample_freq = SCC_SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };

    static struct audio_buffer_format scc_producer_format = {
        .format = &scc_audio_format,
        .sample_stride = 4,
    };

    scc_audio_pool = audio_new_producer_pool(&scc_producer_format, 3, SCC_AUDIO_BUFFER_SAMPLES);

    // Find a free DMA channel (SD SPI driver claims channels at init)
    int scc_dma_channel = -1;
    for (int ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (!dma_channel_is_claimed(ch)) {
            scc_dma_channel = ch;
            break;
        }
    }
    if (scc_dma_channel < 0) {
        scc_audio_pool = NULL;
        return;
    }

    static struct audio_i2s_config scc_i2s_config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_BCLK_PIN,
        .dma_channel = 0,
        // Audio I2S lives on PIO1 SM2 so it never collides with the MSX
        // memory bus PIO programs (pio0 SM0/SM1) or the I/O bus
        // responder (pio2 SM0/SM1).
        .pio_sm = 2,
    };
    scc_i2s_config.dma_channel = (uint)scc_dma_channel;

    audio_i2s_setup(&scc_audio_format, &scc_i2s_config);
    audio_i2s_connect(scc_audio_pool);
    audio_i2s_set_enabled(true);
    gpio_put(I2S_MUTE_PIN, 0);
    scc_audio_started = true;
}

static void scc_audio_init_for_type(uint32_t scc_type)
{
    scc_audio_started = false;
    scc_audio_pool = NULL;
    memset(&scc_instance, 0, sizeof(SCC));
    scc_instance.clk  = SCC_CLOCK;
    scc_instance.rate = SCC_SAMPLE_RATE;
    SCC_set_quality(&scc_instance, 1);
    scc_instance.type = scc_type;
    SCC_reset(&scc_instance);
}

static inline void __not_in_flash_func(scc_audio_service_buffer)(void)
{
    if (!scc_audio_started || !scc_audio_pool)
        return;

    struct audio_buffer *buffer = take_audio_buffer(scc_audio_pool, false);
    if (!buffer)
        return;

    int16_t *samples = (int16_t *)buffer->buffer->bytes;
    for (int i = 0; i < SCC_AUDIO_BUFFER_SAMPLES; i++)
    {
        main_psg_service_io();
        int16_t raw = SCC_calc(&scc_instance);
        int32_t boosted = (int32_t)raw << SCC_VOLUME_SHIFT;
        if (boosted > 32767) boosted = 32767;
        else if (boosted < -32768) boosted = -32768;
        int16_t sample = apply_audio_volume(boosted + main_psg_calc_sample());
        samples[i * 2] = sample;
        samples[i * 2 + 1] = sample;
    }
    buffer->sample_count = SCC_AUDIO_BUFFER_SAMPLES;
    give_audio_buffer(scc_audio_pool, buffer);
}

// -----------------------------------------------------------------------
// Konami SCC mapper with SCC sound emulation via I2S
// -----------------------------------------------------------------------

// Bank switching + SCC register writes for loadrom_konamiscc_scc().
// ctx points at the 4-entry bank register array.
static inline void __not_in_flash_func(handle_konamiscc_scc_write)(uint16_t addr, uint8_t data, void *ctx)
{
    uint8_t *regs = (uint8_t *)ctx;

    if      (addr >= 0x5000u && addr <= 0x57FFu) regs[0] = data;
    else if (addr >= 0x7000u && addr <= 0x77FFu) regs[1] = data;
    else if (addr >= 0x9000u && addr <= 0x97FFu) regs[2] = data;
    else if (addr >= 0xB000u && addr <= 0xB7FFu) regs[3] = data;

    // Forward to SCC emulator (handles enable + register writes)
    SCC_write(&scc_instance, addr, data);
}

void __no_inline_not_in_flash_func(loadrom_konamiscc_scc)(uint32_t offset, bool cache_enable, uint32_t scc_type)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    // Hold WAIT to freeze Z80 during audio subsystem initialization,
    // preventing BIOS from scanning the slot before we are ready.
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    // Initialise SCC emulator (static allocation, no malloc)
    scc_audio_init_for_type(scc_type);

    // Start I2S audio subsystem, then launch audio on core 1
    i2s_audio_init_scc();
    multicore_launch_core1(core1_scc_audio);

    msx_pio_bus_init();

    while (true)
    {
        // --- wait for a read, draining bank/SCC writes while idle ---
        uint16_t addr = pio_get_read_draining_writes(handle_konamiscc_scc_write, bank_registers);

        // drain writes that arrived while waiting
        pio_drain_writes_only(handle_konamiscc_scc_write, bank_registers);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            // Check if address falls in active SCC register read region.
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

// loadrom_konami - Load a Konami (without SCC) ROM into the MSX directly from the pico flash
// The Konami (without SCC) ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 8Kb, the memory banks are:
//  Bank 1: 4000h - 5FFFh, Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
// And the addresses to change banks are:
//	Bank 1: <none>, Bank 2: 6000h - 67FFh (6000h used), Bank 3: 8000h - 87FFh (8000h used), Bank 4: A000h - A7FFh (A000h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_konami)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();
    banked8_loop(rom_base, available_length, bank_registers, handle_konami_write);
}

// loadrom_ascii8 - Load an ASCII8 ROM into the MSX directly from the pico flash
// The ASCII8 ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 8Kb, the memory banks are:
// Bank 1: 4000h - 5FFFh , Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
// And the address to change banks are:
// Bank 1: 6000h - 67FFh (6000h used), Bank 2: 6800h - 6FFFh (6800h used), Bank 3: 7000h - 77FFh (7000h used), Bank 4: 7800h - 7FFFh (7800h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_ascii8)(uint32_t offset, bool cache_enable)
{
    // openMSX RomAscii8kB::reset() maps all four switchable 8 KB windows
    // (0x4000-0xBFFF) to block 0 at startup, not to a 0,1,2,3 linear layout.
    uint8_t bank_registers[4] = {0, 0, 0, 0};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();
    banked8_loop(rom_base, available_length, bank_registers, handle_ascii8_write);
}

// loadrom_ascii16 - Load an ASCII16 ROM into the MSX directly from the pico flash
// The ASCII16 ROM is divided into 16KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 16Kb, the memory banks are:
// Bank 1: 4000h - 7FFFh , Bank 2: 8000h - BFFFh
// And the address to change banks are:
// Bank 1: 6000h - 67FFh (6000h used), Bank 2: 7000h - 77FFh (7000h and 77FFh used)
void __no_inline_not_in_flash_func(loadrom_ascii16)(uint32_t offset, bool cache_enable)
{
    uint8_t bank_registers[2] = {0, 1};
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    msx_pio_bus_init();

    bank8_ctx_t ctx = { .bank_regs = bank_registers };

    // openMSX-style block handling for non power-of-two 16 KB ROMs.
    uint32_t nrBlocks  = (available_length != 0u) ? ((available_length + 0x3FFFu) >> 14) : 0u;
    uint32_t blockMask = (nrBlocks != 0u) ? (nrBlocks - 1u) : 0u;

    while (true)
    {
        uint16_t addr = pio_get_read_draining_writes(handle_ascii16_write, &ctx);

        pio_drain_writes(handle_ascii16_write, &ctx);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t bank = (addr >> 15) & 1;
            uint32_t block = bank_registers[bank];
            if (nrBlocks != 0u && block >= nrBlocks)
                block &= blockMask;
            uint32_t rel = (block << 14) | (addr & 0x3FFFu);
            if (available_length == 0u || rel < available_length)
                data = read_rom_byte(rom_base, rel);
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}


// loadrom_neo8 - Load an NEO8 ROM into the MSX directly from the pico flash
// The NEO8 ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Size of a segment: 8 KB
// Segment switching addresses:
// Bank 0: 0000h~1FFFh, Bank 1: 2000h~3FFFh, Bank 2: 4000h~5FFFh, Bank 3: 6000h~7FFFh, Bank 4: 8000h~9FFFh, Bank 5: A000h~BFFFh
// Switching address: 
// 5000h (mirror at 1000h, 9000h and D000h), 
// 5800h (mirror at 1800h, 9800h and D800h), 
// 6000h (mirror at 2000h, A000h and E000h), 
// 6800h (mirror at 2800h, A800h and E800h), 
// 7000h (mirror at 3000h, B000h and F000h), 
// 7800h (mirror at 3800h, B800h and F800h)
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

        pio_drain_writes_only(handle_neo8_write, &ctx);

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

// loadrom_neo16 - Load an NEO16 ROM into the MSX directly from the pico flash
// The NEO16 ROM is divided into 16KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Size of a segment: 16 KB
// Segment switching addresses:
// Bank 0: 0000h~3FFFh, Bank 1: 4000h~7FFFh, Bank 2: 8000h~BFFFh
// Switching address:
// 5000h (mirror at 1000h, 9000h and D000h),
// 6000h (mirror at 2000h, A000h and E000h),
// 7000h (mirror at 3000h, B000h and F000h)
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

        pio_drain_writes_only(handle_neo16_write, &ctx);

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

void __not_in_flash_func(service_system_audio)(void)
{
    system_audio_service_core1();
}

void __no_inline_not_in_flash_func(loadrom_sunrise)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    sunrise_usb_set_ide_ctx(&ide);
    multicore_launch_core1(sunrise_usb_task);

    system_audio_init_for_sunrise(true);

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

void __no_inline_not_in_flash_func(loadrom_sunrise_mapper)(uint32_t offset, bool cache_enable)
{
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42, 0x0A, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xF3, 0xDB, 0xF4, 0xF6, 0x80, 0xD3, 0xF4, 0xC7
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
        else if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
        {
            restart_detected = true;
        }
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    sunrise_usb_set_ide_ctx(&ide);
    multicore_launch_core1(sunrise_usb_task);

    if (!psram_prepare_mapper_region())
    {
        while (true) { tight_loop_contents(); }
    }

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t subslot_reg = 0x10;

    mapper_fill_ff();

    msx_pio_io_bus_init();
    system_audio_init_for_sunrise(false);
    msx_pio_bus_init();

    while (true)
    {
        uint16_t waddr;
        uint8_t wdata;
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

        uint16_t io_addr;
        uint8_t io_data;
        while (pio_try_get_io_write(&io_addr, &io_data))
        {
            uint8_t port = io_addr & 0xFFu;
            if (system_audio_handle_io_write(port, io_data))
                continue;
            if (port >= 0xFCu && port <= 0xFFu)
                mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
        }

        while (pio_try_get_io_read(&io_addr))
        {
            uint8_t port = io_addr & 0xFFu;
            bool in_window = false;
            uint8_t data = 0xFFu;
            if (system_audio_handle_io_read(port, &data))
            {
                in_window = true;
            }
            else if (port >= 0xFCu && port <= 0xFFu)
            {
                in_window = true;
                data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
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
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

void __no_inline_not_in_flash_func(loadrom_sunrise_sd)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    sunrise_sd_set_ide_ctx(&ide);
    multicore_launch_core1(sunrise_sd_task);

    system_audio_init_for_sunrise(true);

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

void __no_inline_not_in_flash_func(loadrom_sunrise_mapper_sd)(uint32_t offset, bool cache_enable)
{
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42, 0x0A, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xF3, 0xDB, 0xF4, 0xF6, 0x80, 0xD3, 0xF4, 0xC7
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
        else if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
        {
            restart_detected = true;
        }
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);

    // Bring up PSRAM and pre-fill the 1MB mapper region BEFORE launching
    // Core 1. psram_init() runs an interrupts-disabled QMI direct-mode
    // sequence and the subsequent 1MB fill saturates QMI/XIP traffic; if
    // Core 1's SD initialisation runs concurrently from XIP, SPI timing
    // is disrupted and `disk_initialize` fails ("Master device: not
    // found").
    if (!psram_prepare_mapper_region())
    {
        while (true) { tight_loop_contents(); }
    }

    mapper_fill_ff();

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    sunrise_sd_set_ide_ctx(&ide);
    multicore_launch_core1(sunrise_sd_task);

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t subslot_reg = 0x10;

    msx_pio_io_bus_init();
    system_audio_init_for_sunrise(false);
    msx_pio_bus_init();

    while (true)
    {
        uint16_t waddr;
        uint8_t wdata;
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

        uint16_t io_addr;
        uint8_t io_data;
        while (pio_try_get_io_write(&io_addr, &io_data))
        {
            uint8_t port = io_addr & 0xFFu;
            if (system_audio_handle_io_write(port, io_data))
                continue;
            if (port >= 0xFCu && port <= 0xFFu)
                mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
        }

        while (pio_try_get_io_read(&io_addr))
        {
            uint8_t port = io_addr & 0xFFu;
            bool in_window = false;
            uint8_t data = 0xFFu;
            if (system_audio_handle_io_read(port, &data))
            {
                in_window = true;
            }
            else if (port >= 0xFCu && port <= 0xFFu)
            {
                in_window = true;
                data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
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
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

typedef void (*sunrise_backend_task_fn_t)(void);
typedef void (*sunrise_backend_attach_fn_t)(sunrise_ide_t *ide);

static inline void __not_in_flash_func(c2_handle_memory_write)(
    c2_state_t *c2,
    sunrise_ide_t *ide,
    uint8_t mapper_reg[4],
    uint8_t *subslot_reg,
    bool c2_scc_enabled,
    bool external_scc_audio,
    bool sfg_audio,
    uint16_t waddr,
    uint8_t wdata)
{
    if (waddr == 0xFFFFu)
    {
        *subslot_reg = wdata;
        return;
    }

    uint8_t page = (waddr >> 14) & 0x03u;
    uint8_t active_subslot = (*subslot_reg >> (page * 2)) & 0x03u;

    if (active_subslot == 1u)
    {
        if (waddr >= 0x4000u && waddr <= 0x7FFFu)
            sunrise_ide_handle_write(ide, waddr, wdata);
    }
    else if (active_subslot == 0u)
    {
        if (c2_addr_is_regwin(c2, waddr))
        {
            c2_reg_write(c2, waddr, wdata);
        }
        else
        {
            if (c2_scc_enabled)
                SCC_write(&scc_instance, waddr, wdata);

            c2_bank_switch_write(c2, waddr, wdata);

            uint8_t bank_idx;
            uint32_t linear;
            if (c2_decode_addr(c2, waddr, &bank_idx, &linear))
            {
                bool is_ram = c2_bank_is_ram(c2, bank_idx);
                bool is_we = c2_bank_is_we(c2, bank_idx);
                if (is_ram && is_we)
                {
                    if (linear < c2->ram_size)
                    {
                        c2->ram_ptr[linear] = wdata;
                        if (linear + 1u > c2->max_written) c2->max_written = linear + 1u;
                    }
                }
                else if (is_we && !is_ram)
                {
                    c2_flash_cmd_write(c2, waddr, wdata);
                }
            }
        }
    }
    else if (active_subslot == 2u)
    {
        uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
        uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (waddr & 0x3FFFu);
        mapper_write_byte(mapper_offset, wdata);
    }
    else if (active_subslot == 3u)
    {
        if (external_scc_audio)
            SCC_write(&scc_instance, waddr, wdata);
        else if (sfg_audio)
            sfg_write(waddr, wdata);
    }
}

static void __no_inline_not_in_flash_func(loadrom_c2_common)(
    uint32_t offset,
    bool cache_enable,
    sunrise_backend_task_fn_t core1_task,
    sunrise_backend_attach_fn_t attach_ctx)
{
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42, 0x0A, 0x40, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xF3, 0xDB, 0xF4, 0xF6, 0x80, 0xD3,
        0xF4, 0xC7
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
                if (in_window)
                {
                    uint32_t rel = addr - 0x4000u;
                    if (rel < sizeof(bootstrap_rom)) data = bootstrap_rom[rel];
                }
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
            }
        }
        else if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
        {
            restart_detected = true;
        }
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);

    if (!psram_prepare_mapper_region() || !psram_prepare_c2_region())
    {
        while (true) { tight_loop_contents(); }
    }
    mapper_fill_ff();

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    attach_ctx(&ide);
    multicore_launch_core1(core1_task);

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t subslot_reg = 0x10u;

    static c2_state_t c2;
    c2_init(&c2, c2_rom_region.ptr, c2_rom_region.size, 0x01u);

    static const uint8_t c2_descr[8] = { 'C', 'M', 'F', 'C', 'C', 'F', 'R', 'C' };
    bool c2_signature_overlay_armed = false;
    uint8_t c2_signature_overlay_index = 0u;

    msx_pio_io_bus_init();
    system_audio_init_for_sunrise(false);

    bool external_scc_audio = system_audio_profile == SYSTEM_AUDIO_PROFILE_SCC_EXTERNAL ||
                              system_audio_profile == SYSTEM_AUDIO_PROFILE_SCC_PLUS_EXTERNAL;
    bool sfg_audio = system_audio_profile == SYSTEM_AUDIO_PROFILE_YM2151_SFG05 ||
                     system_audio_profile == SYSTEM_AUDIO_PROFILE_YM2151_SFG01;
    bool c2_scc_enabled = external_scc_audio;
    ym2151_sfg_variant_t sfg_variant = system_audio_profile == SYSTEM_AUDIO_PROFILE_YM2151_SFG01 ? YM2151_SFG01 : YM2151_SFG05;
    const uint32_t sfg_variant_offset = (sfg_variant == YM2151_SFG01) ? SFG_BIOS_VARIANT_SIZE : 0u;
    const uint8_t *sfg_bios_base = flash_rom + SFG_BIOS_FLASH_OFFSET + sfg_variant_offset;

    msx_pio_bus_init();

    while (true)
    {
        uint16_t waddr;
        uint8_t wdata;
        while (pio_try_get_write(&waddr, &wdata))
            c2_handle_memory_write(&c2, &ide, mapper_reg, &subslot_reg, c2_scc_enabled, external_scc_audio, sfg_audio, waddr, wdata);

        uint16_t io_addr;
        uint8_t io_data;
        while (pio_try_get_io_write(&io_addr, &io_data))
        {
            uint8_t port = io_addr & 0xFFu;
            if (system_audio_handle_io_write(port, io_data))
                continue;
            if (port >= 0xFCu && port <= 0xFFu)
                mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
            else if ((port & 0xFCu) == 0xF0u && (port & 0x03u) == (c2.pfxn & 0x03u))
            {
                if (io_data == 'C' || io_data == 'S')
                {
                    c2_signature_overlay_armed = true;
                    c2_signature_overlay_index = 0u;
                }
                c2_port_write(&c2, io_data);
            }
        }

        while (pio_try_get_io_read(&io_addr))
        {
            uint8_t port = io_addr & 0xFFu;
            bool in_window = false;
            uint8_t data = 0xFFu;
            if (system_audio_handle_io_read(port, &data))
            {
                in_window = true;
            }
            else if (port >= 0xFCu && port <= 0xFFu)
            {
                in_window = true;
                data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
            }
            else if ((port & 0xFCu) == 0xF0u &&
                     (port & 0x03u) == (c2.pfxn & 0x03u) &&
                     c2.pf0_state != C2_PF0_IDLE)
            {
                in_window = true;
                data = c2_port_read(&c2);
            }
            pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read, pio_build_token(in_window, data));
        }

        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);

            while (pio_try_get_write(&waddr, &wdata))
                c2_handle_memory_write(&c2, &ide, mapper_reg, &subslot_reg, c2_scc_enabled, external_scc_audio, sfg_audio, waddr, wdata);

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

                if (c2_signature_overlay_armed &&
                    c2_signature_overlay_index < sizeof(c2_descr) &&
                    addr == (uint16_t)(0x4010u + c2_signature_overlay_index) &&
                    (c2.cardmdr & C2_CARDMDR_REGS_DISABLE) == 0u)
                {
                    in_window = true;
                    data = c2_descr[c2_signature_overlay_index++];
                    if (c2_signature_overlay_index >= sizeof(c2_descr))
                        c2_signature_overlay_armed = false;
                    pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
                    continue;
                }
                else if (c2_signature_overlay_armed &&
                         addr >= 0x4010u && addr <= 0x4017u &&
                         (c2.cardmdr & C2_CARDMDR_REGS_DISABLE) == 0u)
                {
                    c2_signature_overlay_armed = false;
                    c2_signature_overlay_index = 0u;
                }

                if (active_subslot == 1u)
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
                else if (active_subslot == 0u)
                {
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
                            if (system_audio_profile == SYSTEM_AUDIO_PROFILE_SCC_PLUS_EXTERNAL && (addr & 0xFFFEu) == 0xBFFEu)
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
                                    data = c2_flash_read(&c2, addr, linear);
                                else
                                    data = (linear < c2.ram_size) ? c2.ram_ptr[linear] : 0xFFu;
                            }
                        }
                    }
                }
                else if (active_subslot == 2u)
                {
                    in_window = true;
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (addr & 0x3FFFu);
                    data = mapper_read_byte(mapper_offset);
                }
                else if (active_subslot == 3u)
                {
                    if (external_scc_audio)
                        in_window = external_scc_read(addr, &data);
                    else if (sfg_audio)
                        in_window = sfg_read(sfg_bios_base, addr, &data);
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
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42, 0x0A, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xF3, 0xDB, 0xF4, 0xF6, 0x80, 0xD3, 0xF4, 0xC7
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
        else if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
        {
            restart_detected = true;
        }
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
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

    if (!psram_prepare_mapper_region() || !psram_prepare_megaram_region())
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
    system_audio_init_for_sunrise(false);
    bool megaram_scc_audio = megaram_scc_audio_selected();
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
                if (megaram_scc_audio && !megaram_write_enabled && megaram_scc_write(waddr, wdata))
                    continue;
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
            if (system_audio_handle_io_write(port, io_data))
                continue;
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
            if (system_audio_handle_io_read(port, &data))
            {
                in_window = true;
            }
            else if (port >= 0xFCu && port <= 0xFFu)
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
                    if (!megaram_scc_audio || !megaram_scc_read(addr, &data))
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

void __no_inline_not_in_flash_func(loadrom_megaram)(uint32_t offset, bool cache_enable)
{
    (void)offset;
    (void)cache_enable;

    msx_pio_bus_init();

    if (!psram_prepare_megaram_region())
    {
        while (true) { tight_loop_contents(); }
    }

    uint8_t megaram_bank_reg[4] = { 0, 1, 2, 3 };
    bool megaram_write_enabled = false;
    bool megaram_scc_audio = megaram_scc_audio_selected();
    megaram_fill_ff();

    msx_pio_io_bus_init();
    if (megaram_scc_audio)
    {
        scc_audio_init_for_type(megaram_scc_type_selected());
        i2s_audio_init_scc();
        multicore_launch_core1(core1_scc_audio);
    }

    while (true)
    {
        uint16_t waddr;
        uint8_t wdata;
        while (pio_try_get_write(&waddr, &wdata))
        {
            if (waddr >= 0x4000u && waddr <= 0xBFFFu)
            {
                if (megaram_scc_audio && !megaram_write_enabled && megaram_scc_write(waddr, wdata))
                    continue;
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
            if (port == 0x8Eu || port == 0x8Fu)
                megaram_write_enabled = false;
        }

        while (pio_try_get_io_read(&io_addr))
        {
            uint8_t port = io_addr & 0xFFu;
            if (port == 0x8Eu || port == 0x8Fu)
                megaram_write_enabled = true;
            pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read, pio_build_token(false, 0xFFu));
        }

        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
            bool in_window = addr >= 0x4000u && addr <= 0xBFFFu;
            uint8_t data = 0xFFu;
            if (in_window && (!megaram_scc_audio || !megaram_scc_read(addr, &data)))
                data = megaram_read_byte(addr, megaram_bank_reg);
            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_wifi_common)(
    uint32_t offset,
    bool cache_enable,
    sunrise_backend_task_fn_t core1_task,
    sunrise_backend_attach_fn_t attach_ctx,
    bool mapper_enable)
{
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42, 0x0A, 0x40, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xF3, 0xDB, 0xF4, 0xF6, 0x80, 0xD3,
        0xF4, 0xC7
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
                if (in_window)
                {
                    uint32_t rel = addr - 0x4000u;
                    if (rel < sizeof(bootstrap_rom)) data = bootstrap_rom[rel];
                }
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
            }
        }
        else if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
        {
            restart_detected = true;
        }
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);
    const uint8_t *wifi_rom_base = rom_data + WIFI_BIOS_FLASH_OFFSET;

    if (mapper_enable)
    {
        if (!psram_prepare_mapper_region())
        {
            while (true) { tight_loop_contents(); }
        }
        mapper_fill_ff();
    }

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    attach_ctx(&ide);
    multicore_launch_core1(core1_task);
    wifi_uart_init_once();

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t subslot_reg = mapper_enable ? 0x20u : 0x00u;

    if (mapper_enable)
    {
        msx_pio_io_bus_init();
    }

    system_audio_init_for_sunrise(!mapper_enable);

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

            if (active_subslot == 0)
            {
                (void)wifi_handle_mem_write(waddr, wdata);
            }
            else if (active_subslot == 1)
            {
                if (waddr >= 0x4000u && waddr <= 0x7FFFu)
                    sunrise_ide_handle_write(&ide, waddr, wdata);
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
                if (system_audio_handle_io_write(port, io_data))
                    continue;
                if (port >= 0xFCu && port <= 0xFFu)
                    mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
            }

            while (pio_try_get_io_read(&io_addr))
            {
                uint8_t port = io_addr & 0xFFu;
                bool in_window = false;
                uint8_t data = 0xFFu;
                if (system_audio_handle_io_read(port, &data))
                {
                    in_window = true;
                }
                else if (port >= 0xFCu && port <= 0xFFu)
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
                        data = wifi_handle_mem_read(wifi_rom_base, WIFI_BIOS_ROM_SIZE, addr);
                    }
                }
                else if (active_subslot == 1)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        in_window = true;
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
                }
                else if (mapper_enable && active_subslot == 2)
                {
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

static void __no_inline_not_in_flash_func(loadrom_sunrise_wifi)(uint32_t offset, bool cache_enable)
{
    loadrom_sunrise_wifi_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx, false);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_wifi_sd)(uint32_t offset, bool cache_enable)
{
    loadrom_sunrise_wifi_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx, false);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_mapper_wifi)(uint32_t offset, bool cache_enable)
{
    loadrom_sunrise_wifi_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx, true);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_mapper_wifi_sd)(uint32_t offset, bool cache_enable)
{
    loadrom_sunrise_wifi_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx, true);
}

// -----------------------------------------------------------------------
// Bank cache infrastructure for mapper-aware LRU caching
// Used by ASCII16-X to handle ROMs larger than the 128KB rom_sram cache.
// -----------------------------------------------------------------------
#define BANK_CACHE_MAX_SLOTS 16
#define BANK_CACHE_MAX_PINS   6
#define BANK_EMPTY           0xFFFFu

typedef struct {
    uint16_t      slot_bank[BANK_CACHE_MAX_SLOTS];
    uint8_t       slot_lru[BANK_CACHE_MAX_SLOTS];
    int8_t        page_slot[BANK_CACHE_MAX_PINS];
    const uint8_t *flash_base;
    uint8_t      *slot_base;    // backing store for the slots
    uint32_t      rom_length;
    uint8_t       num_slots;
    uint8_t       num_pins;
    uint16_t      slot_size;
    uint8_t       slot_shift;
} bank_cache_t;

static inline void __not_in_flash_func(bcache_init)(
    bank_cache_t *c, uint16_t slot_size, uint8_t num_pins,
    const uint8_t *flash_base, uint32_t rom_length,
    uint8_t *slot_base, uint8_t max_slots)
{
    uint8_t num_slots = max_slots;
    if (num_slots > BANK_CACHE_MAX_SLOTS) num_slots = BANK_CACHE_MAX_SLOTS;
    c->num_slots  = num_slots;
    c->num_pins   = num_pins;
    c->slot_size  = slot_size;
    c->slot_shift = (slot_size == 8192u) ? 13 : 14;
    c->flash_base = flash_base;
    c->slot_base  = slot_base;
    c->rom_length = rom_length;
    for (int i = 0; i < BANK_CACHE_MAX_SLOTS; i++)
    {
        c->slot_bank[i] = BANK_EMPTY;
        c->slot_lru[i]  = (uint8_t)i;
    }
    for (int i = 0; i < BANK_CACHE_MAX_PINS; i++)
        c->page_slot[i] = -1;
}

static inline void __not_in_flash_func(bcache_touch)(bank_cache_t *c, int8_t slot)
{
    uint8_t prev = c->slot_lru[slot];
    if (prev == 0) return;
    uint8_t n = c->num_slots;
    for (uint8_t i = 0; i < n; i++)
        if (c->slot_lru[i] < prev) c->slot_lru[i]++;
    c->slot_lru[slot] = 0;
}

static inline int8_t __not_in_flash_func(bcache_find)(bank_cache_t *c, uint16_t bank)
{
    uint8_t n = c->num_slots;
    for (uint8_t i = 0; i < n; i++)
        if (c->slot_bank[i] == bank) return (int8_t)i;
    return -1;
}

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

static inline int8_t __not_in_flash_func(bcache_ensure)(bank_cache_t *c, uint16_t bank)
{
    if (c->rom_length > 0u)
    {
        uint16_t total_banks = (uint16_t)(c->rom_length >> c->slot_shift);
        if (total_banks > 0u)
            bank = bank % total_banks;
    }

    int8_t slot = bcache_find(c, bank);
    if (slot >= 0) { bcache_touch(c, slot); return slot; }

    slot = bcache_evict(c);
    uint32_t src_off = (uint32_t)(bank & 0x0FFFu) << c->slot_shift;
    uint8_t *dst = &c->slot_base[(uint32_t)slot * c->slot_size];
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
// ASCII16-X mapper (Padial) - 16KB banks at 0x4000-0x7FFF / 0x8000-0xBFFF
// with extended addressing (12-bit bank number) and AMD-compatible flash
// command emulation (program/erase) on the cached SRAM banks.
// Bank-switch writes use Konami-like layout but the high nibble of the
// register address is OR'd into the bank number to allow >256 banks.
// -----------------------------------------------------------------------
typedef enum {
    FLASH_IDLE = 0,
    FLASH_UNLOCK1,
    FLASH_UNLOCK2,
    FLASH_BYTE_PGM,
    FLASH_ERASE_SETUP,
    FLASH_ERASE_UNLOCK1,
    FLASH_ERASE_UNLOCK2,
} flash_cmd_state_t;

typedef struct {
    uint16_t          bank_regs[2];
    bank_cache_t      cache;
    flash_cmd_state_t flash_state;
} ascii16x_state_t;

static inline void __not_in_flash_func(flash_process_write)(
    ascii16x_state_t *st, uint16_t addr, uint8_t data)
{
    uint16_t cmd_addr = addr & 0x0FFFu;

    switch (st->flash_state)
    {
        case FLASH_IDLE:
            if (cmd_addr == 0x0AAAu && data == 0xAAu)
                st->flash_state = FLASH_UNLOCK1;
            else if (data == 0xF0u)
                st->flash_state = FLASH_IDLE;
            break;
        case FLASH_UNLOCK1:
            if (cmd_addr == 0x0555u && data == 0x55u)
                st->flash_state = FLASH_UNLOCK2;
            else
                st->flash_state = FLASH_IDLE;
            break;
        case FLASH_UNLOCK2:
            if (cmd_addr == 0x0AAAu)
            {
                switch (data)
                {
                    case 0xA0u: st->flash_state = FLASH_BYTE_PGM;    break;
                    case 0x80u: st->flash_state = FLASH_ERASE_SETUP;  break;
                    default:    st->flash_state = FLASH_IDLE;         break;
                }
            }
            else
                st->flash_state = FLASH_IDLE;
            break;
        case FLASH_BYTE_PGM:
        {
            uint8_t page_idx = ((addr >> 14) & 0x01u) ? 0u : 1u;
            int8_t slot = st->cache.page_slot[page_idx];
            if (slot >= 0)
            {
                uint32_t off = (uint32_t)slot * st->cache.slot_size + (addr & 0x3FFFu);
                st->cache.slot_base[off] &= data;
            }
            st->flash_state = FLASH_IDLE;
            break;
        }
        case FLASH_ERASE_SETUP:
            if (cmd_addr == 0x0AAAu && data == 0xAAu)
                st->flash_state = FLASH_ERASE_UNLOCK1;
            else
                st->flash_state = FLASH_IDLE;
            break;
        case FLASH_ERASE_UNLOCK1:
            if (cmd_addr == 0x0555u && data == 0x55u)
                st->flash_state = FLASH_ERASE_UNLOCK2;
            else
                st->flash_state = FLASH_IDLE;
            break;
        case FLASH_ERASE_UNLOCK2:
            if (data == 0x30u)
            {
                uint8_t page_idx = ((addr >> 14) & 0x01u) ? 0u : 1u;
                int8_t slot = st->cache.page_slot[page_idx];
                if (slot >= 0)
                    memset(&st->cache.slot_base[(uint32_t)slot * st->cache.slot_size], 0xFFu, st->cache.slot_size);
            }
            st->flash_state = FLASH_IDLE;
            break;
    }
}

static inline void __not_in_flash_func(handle_ascii16x_write_cached)(uint16_t addr, uint8_t data, void *ctx)
{
    ascii16x_state_t *st = (ascii16x_state_t *)ctx;
    flash_process_write(st, addr, data);

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

// ASCII16-X bank cache backing store.
//
// Explorer's rom_sram is a PSRAM region, so serving mapper reads from it
// costs a QMI transaction per byte and stretches /WAIT on every cartridge
// access.  Borrow the OPLL tll_table (128KB of internal SRAM, meaningful
// only while an MSX-MUSIC instance exists) so ASCII16-X reads come from real
// internal SRAM, matching the standalone loadrom firmware.  When MSX-MUSIC
// is enabled the table is off limits and we fall back to the PSRAM path.
#define ASCII16X_SLOT_SIZE  16384u
#define ASCII16X_SRAM_SLOTS 8u

void __no_inline_not_in_flash_func(loadrom_ascii16x)(uint32_t offset, bool cache_enable)
{
    (void)cache_enable;

    ascii16x_state_t state;
    memset(&state, 0, sizeof(state));

    const uint8_t *rom_base;
    uint32_t available_length;

    uint8_t *sram_cache = NULL;
    if (msx_music_instance == NULL)
        sram_cache = (uint8_t *)OPLL_borrow_table_memory(ASCII16X_SRAM_SLOTS * ASCII16X_SLOT_SIZE);

    if (sram_cache == NULL && !psram_prepare_rom_cache())
    {
        return;
    }

    // Don't preload via prepare_rom_source — bcache manages the slots directly.
    prepare_rom_source(offset, false, 0u, &rom_base, &available_length);

    if (sram_cache != NULL)
    {
        bcache_init(&state.cache, ASCII16X_SLOT_SIZE, 2, rom_data + offset, active_rom_size,
                    sram_cache, (uint8_t)ASCII16X_SRAM_SLOTS);
    }
    else
    {
        bcache_init(&state.cache, ASCII16X_SLOT_SIZE, 2, rom_data + offset, active_rom_size,
                    rom_sram, (uint8_t)(CACHE_SIZE / ASCII16X_SLOT_SIZE));
    }

    bcache_prefill(&state.cache);

    // Initial bank 0 visible at both windows.
    state.cache.page_slot[0] = bcache_find(&state.cache, 0);
    state.cache.page_slot[1] = state.cache.page_slot[0];

    // bcache reads via slot index, bypassing read_rom_byte.
    rom_cached_size = 0;

    msx_pio_bus_init();

    while (true)
    {
        uint16_t addr = pio_get_read_draining_writes(handle_ascii16x_write_cached, &state);

        pio_drain_writes_only(handle_ascii16x_write_cached, &state);

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t page_idx = ((addr >> 14) & 0x01u) ? 0u : 1u;
            int8_t slot = state.cache.page_slot[page_idx];
            if (slot >= 0)
                data = state.cache.slot_base[(uint32_t)slot * state.cache.slot_size + (addr & 0x3FFFu)];
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// loadrom_planar64 - 64KB Planar ROM (no mapper)
// Linear 64KB ROM mapped at 0x0000-0xFFFF (typical MSX2 boot games like
// Game Master 2). AB header is at 0x4000.
// -----------------------------------------------------------------------
void __no_inline_not_in_flash_func(loadrom_planar64)(uint32_t offset, bool cache_enable)
{
    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 65536u, &rom_base, &available_length);

    msx_pio_bus_init();

    while (true)
    {
        wavegame_service_io();
        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);
        wavegame_service_io();

        bool in_window = true; // 64KB linear: drive all reads
        uint8_t data = 0xFFu;
        if (available_length == 0u || addr < available_length)
            data = read_rom_byte(rom_base, addr);

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// -----------------------------------------------------------------------
// Manbow2 mapper - Konami SCC banking + AMD AM29F040B flash emulation
// 8KB banks at 0x4000-0x5FFF / 0x6000-0x7FFF / 0x8000-0x9FFF / 0xA000-0xBFFF.
// Bank-switch writes land at 0x?000-0x?7FF for each page (Konami SCC layout).
// The last 64KB of the ROM (flash offset 0x70000-0x7FFFF) is treated as a
// writable sector emulating the AMD AM29F040B flash chip used by Manbow2 to
// persist save data via the standard JEDEC unlock+program/erase sequence.
// -----------------------------------------------------------------------
enum manbow2_flash_state {
    MBW2_READ = 0,
    MBW2_CMD1,
    MBW2_CMD2,
    MBW2_AUTOSELECT,
    MBW2_PROGRAM,
    MBW2_ERASE_CMD1,
    MBW2_ERASE_CMD2,
    MBW2_ERASE_CMD3
};

typedef struct {
    uint8_t bank_regs[4];
    enum manbow2_flash_state state;
    uint8_t *writable_sram;
    uint32_t writable_offset;
    uint32_t writable_size;
} manbow2_ctx_t;

static inline void __not_in_flash_func(handle_manbow2_write)(uint16_t addr, uint8_t data, void *ctx)
{
    manbow2_ctx_t *mb = (manbow2_ctx_t *)ctx;

    if (addr < 0x4000u || addr > 0xBFFFu)
        return;

    uint8_t page = (uint8_t)((addr - 0x4000u) >> 13);
    uint32_t flash_addr = (uint32_t)mb->bank_regs[page] * 0x2000u + (addr & 0x1FFFu);

    // Konami SCC-style bank latches at 0x?000-0x?7FF of each 8KB page.
    if ((addr & 0x1800u) == 0x1000u)
        mb->bank_regs[page] = data & 0x3Fu;

    // AMD reset
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
            if      (data == 0xA0u) mb->state = MBW2_PROGRAM;
            else if (data == 0x90u) mb->state = MBW2_AUTOSELECT;
            else if (data == 0x80u) mb->state = MBW2_ERASE_CMD1;
            else                     mb->state = MBW2_READ;
            break;
        case MBW2_PROGRAM:
            if (flash_addr >= mb->writable_offset &&
                flash_addr <  mb->writable_offset + mb->writable_size)
            {
                uint32_t sram_off = flash_addr - mb->writable_offset;
                mb->writable_sram[sram_off] &= data;
            }
            mb->state = MBW2_READ;
            break;
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
                if (flash_addr >= mb->writable_offset &&
                    flash_addr <  mb->writable_offset + mb->writable_size)
                    memset(mb->writable_sram, 0xFF, mb->writable_size);
            }
            mb->state = MBW2_READ;
            break;
        case MBW2_AUTOSELECT:
            break;
    }
}

// Common Manbow2 setup: caches the first (CACHE_SIZE - writable_size) bytes of
// the ROM into rom_sram (mirrors multirom's strategy) and reserves the tail of
// rom_sram for the emulated 64KB writable AMD flash sector. Returns rom_base /
// writable_sram / available_length to the caller.
static inline void __not_in_flash_func(loadrom_manbow2_setup)(
    uint32_t offset,
    const uint8_t **rom_base_out,
    uint32_t *available_length_out,
    uint8_t **writable_sram_out,
    uint32_t writable_offset,
    uint32_t writable_size)
{
    uint32_t reduced_cache = CACHE_SIZE - writable_size;

    // Limit prepare_rom_source's caching to `reduced_cache` so the tail of
    // rom_sram is left untouched for the writable flash sector.
    rom_cache_capacity = reduced_cache;
    prepare_rom_source(offset, true, 0u, rom_base_out, available_length_out);
    rom_cache_capacity = CACHE_SIZE; // restore default for next ROM type

    uint8_t *writable_sram = &rom_sram[reduced_cache];

    // Seed the writable AMD flash sector mirror from the ROM payload.
    // rom_data points to flash for flash-resident ROMs and to PSRAM for
    // SD-resident ROMs, so a single source path works for both.
    {
        const uint8_t *src = rom_data + offset;

        if (*available_length_out >= writable_offset + writable_size)
        {
            memcpy(writable_sram, src + writable_offset, writable_size);
        }
        else if (*available_length_out > writable_offset)
        {
            uint32_t partial = *available_length_out - writable_offset;
            memcpy(writable_sram, src + writable_offset, partial);
            memset(writable_sram + partial, 0xFF, writable_size - partial);
        }
        else
        {
            memset(writable_sram, 0xFF, writable_size);
        }
    }

    *writable_sram_out = writable_sram;
}

static inline void __not_in_flash_func(handle_ascii16x_write_simple)(uint16_t addr, uint8_t data, uint16_t *bank_regs)
{
    if (addr >= 0x6000u && addr <= 0x67FFu)
        bank_regs[0] = data;
    else if (addr >= 0x7000u && addr <= 0x77FFu)
        bank_regs[1] = data;
}

static void __not_in_flash_func(fmpac_wait_for_expanded_bootstrap)(void)
{
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42, 0x0A, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xF3, 0xDB, 0xF4, 0xF6, 0x80, 0xD3, 0xF4, 0xC7
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
        else if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
        {
            restart_detected = true;
        }
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);
}

void __no_inline_not_in_flash_func(loadrom_fmpac)(uint32_t offset, bool cache_enable, uint8_t mapper)
{
    debug_trace("DBG fmpac enter");
    fmpac_wait_for_expanded_bootstrap();
    debug_trace("DBG fmpac bootstrap done");

    uint8_t bank8_regs[4] = {0, 1, 2, 3};
    uint8_t ascii16_regs[2] = {0, 1};
    uint16_t neo8_regs[6] = {0, 1, 2, 3, 4, 5};
    uint16_t neo16_regs[3] = {0, 1, 2};
    uint16_t ascii16x_regs[2] = {0, 0};

    // ASCII8 (RomAscii8kB) resets all switchable 8 KB windows to block 0.
    if (mapper == 5u)
    {
        bank8_regs[0] = 0; bank8_regs[1] = 0; bank8_regs[2] = 0; bank8_regs[3] = 0;
    }

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_rom_source(offset, cache_enable, 0u, &rom_base, &available_length);
    const uint8_t *fmpac_bios_base = flash_rom + FMPAC_BIOS_FLASH_OFFSET;

    uint8_t subslot_reg = 0x00u;
    static fmpac_state_t fmpac;
    memset(&fmpac, 0, sizeof(fmpac));
    fmpac.control = 0x10u;

    bank8_ctx_t bank8_ctx = { .bank_regs = bank8_regs };
    bank8_ctx_t ascii16_ctx = { .bank_regs = ascii16_regs };
    bank16_ctx_t neo8_ctx = { .bank_regs = neo8_regs };
    bank16_ctx_t neo16_ctx = { .bank_regs = neo16_regs };

    debug_trace("DBG fmpac bus init");
    msx_pio_bus_init();
    debug_trace("DBG fmpac start audio out");
    start_msx_music_audio_output();
    debug_trace("DBG fmpac loop start");

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
                switch (mapper)
                {
                    case 3:  handle_konamiscc_write(waddr, wdata, &bank8_ctx); break;
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

                    switch (mapper)
                    {
                        case 1:
                        case 2:
                            mapped = (addr >= 0x4000u && addr <= 0xBFFFu);
                            rel = addr - 0x4000u;
                            break;
                        case 3:
                        case 5:
                        case 7:
                            mapped = (addr >= 0x4000u && addr <= 0xBFFFu);
                            if (mapped)
                            {
                                uint32_t block = bank8_regs[(addr - 0x4000u) >> 13];
                                if (available_length != 0u)
                                {
                                    uint32_t nrBlocks = (available_length + 0x1FFFu) >> 13;
                                    if (block >= nrBlocks)
                                        block &= (nrBlocks - 1u);
                                }
                                rel = (block << 13) | (addr & 0x1FFFu);
                            }
                            break;
                        case 4:
                            mapped = (addr <= 0xBFFFu);
                            rel = addr;
                            break;
                        case 6:
                            mapped = (addr >= 0x4000u && addr <= 0xBFFFu);
                            if (mapped)
                            {
                                uint32_t block = ascii16_regs[(addr >> 15) & 1u];
                                if (available_length != 0u)
                                {
                                    uint32_t nrBlocks = (available_length + 0x3FFFu) >> 14;
                                    if (block >= nrBlocks)
                                        block &= (nrBlocks - 1u);
                                }
                                rel = (block << 14) | (addr & 0x3FFFu);
                            }
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

typedef struct {
    uint8_t mapper;
    const uint8_t *rom_base;
    uint32_t available_length;
    uint8_t bank8_regs[4];
    uint8_t ascii16_regs[2];
    uint16_t neo8_regs[6];
    uint16_t neo16_regs[3];
    uint16_t ascii16x_regs[2];
    uint8_t *manbow2_writable_sram;
    manbow2_ctx_t manbow2;
} external_scc_game_t;

static inline void __not_in_flash_func(external_scc_game_init)(external_scc_game_t *game, uint8_t mapper)
{
    memset(game, 0, sizeof(*game));
    game->mapper = mapper;
    // ASCII8 (RomAscii8kB) resets all switchable 8 KB windows to block 0;
    // the bank-switched mappers (Konami/SCC) keep the linear 0,1,2,3 layout.
    if (mapper == 5u)
    {
        game->bank8_regs[0] = 0;
        game->bank8_regs[1] = 0;
        game->bank8_regs[2] = 0;
        game->bank8_regs[3] = 0;
    }
    else
    {
        game->bank8_regs[0] = 0;
        game->bank8_regs[1] = 1;
        game->bank8_regs[2] = 2;
        game->bank8_regs[3] = 3;
    }
    game->ascii16_regs[0] = 0;
    game->ascii16_regs[1] = 1;
    game->neo8_regs[0] = 0;
    game->neo8_regs[1] = 1;
    game->neo8_regs[2] = 2;
    game->neo8_regs[3] = 3;
    game->neo8_regs[4] = 4;
    game->neo8_regs[5] = 5;
    game->neo16_regs[0] = 0;
    game->neo16_regs[1] = 1;
    game->neo16_regs[2] = 2;
}

static inline void __not_in_flash_func(external_scc_game_write)(external_scc_game_t *game, uint16_t addr, uint8_t data)
{
    bank8_ctx_t bank8_ctx = { .bank_regs = game->bank8_regs };
    bank8_ctx_t ascii16_ctx = { .bank_regs = game->ascii16_regs };
    bank16_ctx_t neo8_ctx = { .bank_regs = game->neo8_regs };
    bank16_ctx_t neo16_ctx = { .bank_regs = game->neo16_regs };

    switch (game->mapper)
    {
        case 3:  handle_konamiscc_write(addr, data, &bank8_ctx); break;
        case 5:  handle_ascii8_write(addr, data, &bank8_ctx); break;
        case 6:  handle_ascii16_write(addr, data, &ascii16_ctx); break;
        case 7:  handle_konami_write(addr, data, &bank8_ctx); break;
        case 8:  handle_neo8_write(addr, data, &neo8_ctx); break;
        case 9:  handle_neo16_write(addr, data, &neo16_ctx); break;
        case 12: handle_ascii16x_write_simple(addr, data, game->ascii16x_regs); break;
        case 14: handle_manbow2_write(addr, data, &game->manbow2); break;
    }
}

static inline bool __not_in_flash_func(external_scc_game_read)(external_scc_game_t *game, uint16_t addr, uint8_t *data)
{
    uint32_t rel = 0;
    bool mapped = false;

    switch (game->mapper)
    {
        case 1:
        case 2:
            mapped = (addr >= 0x4000u && addr <= 0xBFFFu);
            rel = addr - 0x4000u;
            break;
        case 3:
        case 5:
        case 7:
            mapped = (addr >= 0x4000u && addr <= 0xBFFFu);
            if (mapped)
            {
                uint32_t block = game->bank8_regs[(addr - 0x4000u) >> 13];
                if (game->available_length != 0u)
                {
                    uint32_t nrBlocks = (game->available_length + 0x1FFFu) >> 13;
                    if (block >= nrBlocks)
                        block &= (nrBlocks - 1u);
                }
                rel = (block << 13) | (addr & 0x1FFFu);
            }
            break;
        case 4:
            mapped = (addr <= 0xBFFFu);
            rel = addr;
            break;
        case 6:
            mapped = (addr >= 0x4000u && addr <= 0xBFFFu);
            if (mapped)
            {
                uint32_t block = game->ascii16_regs[(addr >> 15) & 1u];
                if (game->available_length != 0u)
                {
                    uint32_t nrBlocks = (game->available_length + 0x3FFFu) >> 14;
                    if (block >= nrBlocks)
                        block &= (nrBlocks - 1u);
                }
                rel = (block << 14) | (addr & 0x3FFFu);
            }
            break;
        case 8:
            mapped = (addr <= 0xBFFFu);
            if (mapped && (addr >> 13) < 6u)
                rel = ((uint32_t)(game->neo8_regs[addr >> 13] & 0x0FFFu) << 13) | (addr & 0x1FFFu);
            break;
        case 9:
            mapped = (addr <= 0xBFFFu);
            if (mapped && (addr >> 14) < 3u)
                rel = ((uint32_t)(game->neo16_regs[addr >> 14] & 0x0FFFu) << 14) | (addr & 0x3FFFu);
            break;
        case 12:
            mapped = true;
            rel = ((uint32_t)game->ascii16x_regs[((addr >> 14) & 1u) ? 0u : 1u] << 14) | (addr & 0x3FFFu);
            break;
        case 13:
            mapped = true;
            rel = addr;
            break;
        case 14:
            mapped = (addr >= 0x4000u && addr <= 0xBFFFu);
            if (mapped)
            {
                uint8_t page = (uint8_t)((addr - 0x4000u) >> 13);
                rel = (uint32_t)game->manbow2.bank_regs[page] * 0x2000u + (addr & 0x1FFFu);
                if (game->manbow2.state == MBW2_AUTOSELECT)
                {
                    uint32_t id_addr = rel & 0x03u;
                    if      (id_addr == 0x00u) *data = 0x01u;
                    else if (id_addr == 0x01u) *data = 0xA4u;
                    else if (id_addr == 0x02u) *data = (rel >= game->manbow2.writable_offset && rel < game->manbow2.writable_offset + game->manbow2.writable_size) ? 0x00u : 0x01u;
                    else                       *data = 0x00u;
                    return true;
                }
                if (rel >= game->manbow2.writable_offset && rel < game->manbow2.writable_offset + game->manbow2.writable_size)
                {
                    *data = game->manbow2_writable_sram[rel - game->manbow2.writable_offset];
                    return true;
                }
            }
            break;
    }

    if (!mapped)
        return false;

    *data = 0xFFu;
    if (game->available_length == 0u || rel < game->available_length)
        *data = read_rom_byte(game->rom_base, rel);
    return true;
}

static inline bool __not_in_flash_func(external_scc_read)(uint16_t addr, uint8_t *data)
{
    uint32_t base = scc_instance.base_adr;
    if (addr == base ||
        (scc_instance.active && addr >= base + 0x800u && addr <= base + 0x8FFu) ||
        (scc_instance.type == SCC_ENHANCED && (addr & 0xFFFEu) == 0xBFFEu))
    {
        *data = (uint8_t)SCC_read(&scc_instance, addr);
        return true;
    }
    return false;
}

static inline bool __not_in_flash_func(megaram_scc_audio_selected)(void)
{
    return ctrl_audio_selection == AUDIO_PROFILE_MEGARAM_SCC ||
           ctrl_audio_selection == AUDIO_PROFILE_MEGARAM_SCC_PLUS;
}

static inline uint32_t __not_in_flash_func(megaram_scc_type_selected)(void)
{
    return ctrl_audio_selection == AUDIO_PROFILE_MEGARAM_SCC_PLUS ? SCC_ENHANCED : SCC_STANDARD;
}

static inline bool __not_in_flash_func(megaram_scc_write)(uint16_t addr, uint8_t data)
{
    uint32_t base = scc_instance.base_adr;
    bool consume = scc_instance.active && addr >= base + 0x800u && addr <= base + 0x8FFu;
    if (!consume && addr >= base && addr <= base + 0x7FFu)
        SCC_write(&scc_instance, base, data);
    else
        SCC_write(&scc_instance, addr, data);
    if (consume)
        return true;
    return false;
}

static inline bool __not_in_flash_func(megaram_scc_read)(uint16_t addr, uint8_t *data)
{
    uint32_t base = scc_instance.base_adr;
    if (scc_instance.active && addr >= base + 0x800u && addr <= base + 0x8FFu)
    {
        *data = (uint8_t)SCC_read(&scc_instance, addr);
        return true;
    }
    return false;
}

void __no_inline_not_in_flash_func(loadrom_external_scc)(uint32_t offset, bool cache_enable, uint8_t mapper, uint32_t scc_type)
{
    fmpac_wait_for_expanded_bootstrap();

    external_scc_game_t game;
    external_scc_game_init(&game, mapper);

    if (mapper == 14u)
    {
        static const uint32_t WRITABLE_SECTOR_SIZE   = 0x10000u;
        static const uint32_t WRITABLE_SECTOR_OFFSET = 0x70000u;
        loadrom_manbow2_setup(offset, &game.rom_base, &game.available_length, &game.manbow2_writable_sram,
                              WRITABLE_SECTOR_OFFSET, WRITABLE_SECTOR_SIZE);
        game.manbow2.bank_regs[0] = 0;
        game.manbow2.bank_regs[1] = 1;
        game.manbow2.bank_regs[2] = 2;
        game.manbow2.bank_regs[3] = 3;
        game.manbow2.state = MBW2_READ;
        game.manbow2.writable_sram = game.manbow2_writable_sram;
        game.manbow2.writable_offset = WRITABLE_SECTOR_OFFSET;
        game.manbow2.writable_size = WRITABLE_SECTOR_SIZE;
    }
    else
    {
        prepare_rom_source(offset, cache_enable, 0u, &game.rom_base, &game.available_length);
    }

    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    scc_audio_init_for_type(scc_type);

    i2s_audio_init_scc();
    multicore_launch_core1(core1_scc_audio);

    uint8_t subslot_reg = 0x00u;

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
                external_scc_game_write(&game, waddr, wdata);
            else if (active_subslot == 1)
                SCC_write(&scc_instance, waddr, wdata);
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
                    in_window = external_scc_game_read(&game, addr, &data);
                else if (active_subslot == 1)
                    in_window = external_scc_read(addr, &data);
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }

        tight_loop_contents();
    }
}

static inline void __not_in_flash_func(sfg_write)(uint16_t addr, uint8_t data)
{
    switch (addr & 0x3FFFu)
    {
        case 0x3FF0u:
            ym2151_select_register(data);
            break;
        case 0x3FF1u:
            ym2151_write_register_data(data);
            break;
        case 0x3FF3u:
            ym2151_midi_irq_vector = data;
            break;
        case 0x3FF4u:
            ym2151_irq_vector = data;
            break;
        case 0x3FF5u:
            ym2151_midi_status |= 0x01u;
            break;
        case 0x3FF6u:
            ym2151_midi_status = 0x03u;
            break;
    }
}

static inline bool __not_in_flash_func(sfg_read)(const uint8_t *bios_base, uint16_t addr, uint8_t *data)
{
    switch (addr & 0x3FFFu)
    {
        case 0x3FF0u:
            *data = 0xFFu;
            return true;
        case 0x3FF1u:
            *data = ym2151_read_status();
            return true;
        case 0x3FF2u:
            *data = 0xFFu;
            return true;
        case 0x3FF5u:
            *data = 0xFFu;
            return true;
        case 0x3FF6u:
            *data = ym2151_midi_status;
            return true;
    }

    if ((addr & 0x3F80u) == 0x3F80u)
    {
        *data = 0xFFu;
        return true;
    }

    *data = bios_base[addr & (SFG_BIOS_VARIANT_SIZE - 1u)];
    return true;
}

// Drain every pending MSX write from the capture FIFO and apply it to the
// active subslot (game mapper in subslot 0, SFG surface in subslot 1). This is
// called both before AND after dequeuing a read address so the read response
// always reflects the most recent bank/subslot state: a bank-switch or ENASLT
// (0xFFFF) write can land between the top-of-loop drain and the read dequeue,
// and servicing the read with the stale state routes the access to the wrong
// bank/device, returning a single corrupt byte. banked8_loop (the plain ASCII8
// path) already re-drains after dequeuing the read; the combined SFG loop must
// do the same or odd-paced ASCII8 games freeze after some time.
static inline void __not_in_flash_func(external_sfg_drain_writes)(external_scc_game_t *game, uint8_t *subslot_reg)
{
    uint16_t waddr;
    uint8_t wdata;
    while (pio_try_get_write(&waddr, &wdata))
    {
        if (waddr == 0xFFFFu)
        {
            *subslot_reg = wdata;
            continue;
        }

        uint8_t page = (waddr >> 14) & 0x03u;
        uint8_t active_subslot = (*subslot_reg >> (page * 2)) & 0x03u;
        if (active_subslot == 0)
            external_scc_game_write(game, waddr, wdata);
        else if (active_subslot == 1)
            sfg_write(waddr, wdata);
    }
}

void __no_inline_not_in_flash_func(loadrom_external_sfg)(uint32_t offset, bool cache_enable, uint8_t mapper, ym2151_sfg_variant_t variant)
{
    fmpac_wait_for_expanded_bootstrap();

    external_scc_game_t game;
    external_scc_game_init(&game, mapper);

    if (mapper == 14u)
    {
        static const uint32_t WRITABLE_SECTOR_SIZE   = 0x10000u;
        static const uint32_t WRITABLE_SECTOR_OFFSET = 0x70000u;
        loadrom_manbow2_setup(offset, &game.rom_base, &game.available_length, &game.manbow2_writable_sram,
                              WRITABLE_SECTOR_OFFSET, WRITABLE_SECTOR_SIZE);
        game.manbow2.bank_regs[0] = 0;
        game.manbow2.bank_regs[1] = 1;
        game.manbow2.bank_regs[2] = 2;
        game.manbow2.bank_regs[3] = 3;
        game.manbow2.state = MBW2_READ;
        game.manbow2.writable_sram = game.manbow2_writable_sram;
        game.manbow2.writable_offset = WRITABLE_SECTOR_OFFSET;
        game.manbow2.writable_size = WRITABLE_SECTOR_SIZE;
    }
    else
    {
        prepare_rom_source(offset, cache_enable, 0u, &game.rom_base, &game.available_length);
    }

    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    ym2151_init(variant);
    start_ym2151_audio_output();

    const uint32_t sfg_variant_offset = (variant == YM2151_SFG01) ? SFG_BIOS_VARIANT_SIZE : 0u;
    const uint8_t *sfg_bios_base = flash_rom + SFG_BIOS_FLASH_OFFSET + sfg_variant_offset;

    uint8_t subslot_reg = 0x00u;

    msx_pio_bus_init();

    while (true)
    {
        external_sfg_drain_writes(&game, &subslot_reg);

        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);

            // Re-drain: apply any write captured between the drain above and
            // dequeuing this read so the response uses up-to-date bank/subslot.
            external_sfg_drain_writes(&game, &subslot_reg);

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
                    in_window = external_scc_game_read(&game, addr, &data);
                }
                else if (active_subslot == 1)
                    in_window = sfg_read(sfg_bios_base, addr, &data);
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }

        tight_loop_contents();
    }
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_sfg_common)(
    uint32_t offset,
    bool cache_enable,
    sunrise_backend_task_fn_t core1_task,
    sunrise_backend_attach_fn_t attach_ctx,
    bool wifi_enable,
    bool mapper_enable,
    ym2151_sfg_variant_t variant)
{
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42, 0x0A, 0x40, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xF3, 0xDB, 0xF4, 0xF6, 0x80, 0xD3,
        0xF4, 0xC7
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
                if (in_window)
                {
                    uint32_t rel = addr - 0x4000u;
                    if (rel < sizeof(bootstrap_rom)) data = bootstrap_rom[rel];
                }
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
            }
        }
        else if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
        {
            restart_detected = true;
        }
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);
    const uint8_t *wifi_rom_base = rom_data + WIFI_BIOS_FLASH_OFFSET;

    if (mapper_enable)
    {
        if (!psram_prepare_mapper_region())
        {
            while (true) { tight_loop_contents(); }
        }
        mapper_fill_ff();
    }

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    attach_ctx(&ide);
    multicore_launch_core1(core1_task);
    if (wifi_enable)
        wifi_uart_init_once();

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t subslot_reg = wifi_enable ? 0x20u : 0x10u;
    const uint8_t nextor_subslot = wifi_enable ? 1u : 0u;
    const uint8_t mapper_subslot = wifi_enable ? 2u : 1u;
    const uint8_t sfg_subslot = wifi_enable ? 3u : 2u;
    const uint32_t sfg_variant_offset = (variant == YM2151_SFG01) ? SFG_BIOS_VARIANT_SIZE : 0u;
    const uint8_t *sfg_bios_base = flash_rom + SFG_BIOS_FLASH_OFFSET + sfg_variant_offset;

    msx_pio_io_bus_init();
    system_audio_init_for_sunrise(false);
    msx_pio_bus_init();

    while (true)
    {
        if (wifi_enable)
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

            if (wifi_enable && active_subslot == 0u)
            {
                (void)wifi_handle_mem_write(waddr, wdata);
            }
            else if (active_subslot == nextor_subslot)
            {
                if (waddr >= 0x4000u && waddr <= 0x7FFFu)
                    sunrise_ide_handle_write(&ide, waddr, wdata);
            }
            else if (mapper_enable && active_subslot == mapper_subslot)
            {
                uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (waddr & 0x3FFFu);
                mapper_write_byte(mapper_offset, wdata);
            }
            else if (active_subslot == sfg_subslot)
            {
                sfg_write(waddr, wdata);
            }
        }

        uint16_t io_addr;
        uint8_t io_data;
        while (pio_try_get_io_write(&io_addr, &io_data))
        {
            uint8_t port = io_addr & 0xFFu;
            if (system_audio_handle_io_write(port, io_data))
                continue;
            if (mapper_enable && port >= 0xFCu && port <= 0xFFu)
                mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
        }

        while (pio_try_get_io_read(&io_addr))
        {
            uint8_t port = io_addr & 0xFFu;
            bool in_window = false;
            uint8_t data = 0xFFu;
            if (system_audio_handle_io_read(port, &data))
            {
                in_window = true;
            }
            else if (mapper_enable && port >= 0xFCu && port <= 0xFFu)
            {
                in_window = true;
                data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
            }
            pio_sm_put_blocking(msx_io_bus.pio_read, msx_io_bus.sm_io_read, pio_build_token(in_window, data));
        }

        if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            uint16_t addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);

            // Re-drain memory writes captured between the drain above (and the
            // IO handling that follows it) and dequeuing this read, so the read
            // response reflects the latest bank/subslot state. A bank-switch or
            // ENASLT (0xFFFF) write landing in that gap would otherwise be
            // serviced after this read, returning a stale/corrupt byte and
            // freezing odd-paced ASCII8 games after some time.
            while (pio_try_get_write(&waddr, &wdata))
            {
                if (waddr == 0xFFFFu)
                {
                    subslot_reg = wdata;
                    continue;
                }

                uint8_t wpage = (waddr >> 14) & 0x03u;
                uint8_t wsub = (subslot_reg >> (wpage * 2)) & 0x03u;

                if (wifi_enable && wsub == 0u)
                {
                    (void)wifi_handle_mem_write(waddr, wdata);
                }
                else if (wsub == nextor_subslot)
                {
                    if (waddr >= 0x4000u && waddr <= 0x7FFFu)
                        sunrise_ide_handle_write(&ide, waddr, wdata);
                }
                else if (mapper_enable && wsub == mapper_subslot)
                {
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[wpage]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (waddr & 0x3FFFu);
                    mapper_write_byte(mapper_offset, wdata);
                }
                else if (wsub == sfg_subslot)
                {
                    sfg_write(waddr, wdata);
                }
            }

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

                if (wifi_enable && active_subslot == 0u)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        in_window = true;
                        data = wifi_handle_mem_read(wifi_rom_base, WIFI_BIOS_ROM_SIZE, addr);
                    }
                }
                else if (active_subslot == nextor_subslot)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        in_window = true;
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
                }
                else if (mapper_enable && active_subslot == mapper_subslot)
                {
                    in_window = true;
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (addr & 0x3FFFu);
                    data = mapper_read_byte(mapper_offset);
                }
                else if (active_subslot == sfg_subslot)
                {
                    in_window = sfg_read(sfg_bios_base, addr, &data);
                }
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_fmpac_common)(
    uint32_t offset,
    bool cache_enable,
    sunrise_backend_task_fn_t core1_task,
    sunrise_backend_attach_fn_t attach_ctx,
    bool wifi_enable,
    bool mapper_enable)
{
    static const uint8_t bootstrap_rom[] = {
        0x41, 0x42, 0x0A, 0x40, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xF3, 0xDB, 0xF4, 0xF6, 0x80, 0xD3,
        0xF4, 0xC7
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
                if (in_window)
                {
                    uint32_t rel = addr - 0x4000u;
                    if (rel < sizeof(bootstrap_rom)) data = bootstrap_rom[rel];
                }
                pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
            }
        }
        else if (init_called && !gpio_get(PIN_RD) && ((gpio_get_all() & 0xFFFFu) == 0x0000u))
        {
            restart_detected = true;
        }
    }

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);
    const uint8_t *wifi_rom_base = rom_data + WIFI_BIOS_FLASH_OFFSET;
    const uint8_t *fmpac_bios_base = flash_rom + FMPAC_BIOS_FLASH_OFFSET;

    if (mapper_enable)
    {
        if (!psram_prepare_mapper_region())
        {
            while (true) { tight_loop_contents(); }
        }
        mapper_fill_ff();
    }

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    attach_ctx(&ide);
    multicore_launch_core1(core1_task);
    if (wifi_enable)
        wifi_uart_init_once();

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t subslot_reg = wifi_enable ? 0x20u : 0x10u;
    const uint8_t nextor_subslot = wifi_enable ? 1u : 0u;
    const uint8_t mapper_subslot = wifi_enable ? 2u : 1u;
    const uint8_t fmpac_subslot = wifi_enable ? 3u : 2u;

    static fmpac_state_t fmpac;
    memset(&fmpac, 0, sizeof(fmpac));
    fmpac.control = 0x10u;

    msx_pio_io_bus_init();
    system_audio_init_for_sunrise(false);
    msx_pio_bus_init();

    while (true)
    {
        if (wifi_enable)
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

            if (wifi_enable && active_subslot == 0u)
            {
                (void)wifi_handle_mem_write(waddr, wdata);
            }
            else if (active_subslot == nextor_subslot)
            {
                if (waddr >= 0x4000u && waddr <= 0x7FFFu)
                    sunrise_ide_handle_write(&ide, waddr, wdata);
            }
            else if (mapper_enable && active_subslot == mapper_subslot)
            {
                uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (waddr & 0x3FFFu);
                mapper_write_byte(mapper_offset, wdata);
            }
            else if (active_subslot == fmpac_subslot)
            {
                fmpac_handle_write(&fmpac, waddr, wdata);
            }
        }

        uint16_t io_addr;
        uint8_t io_data;
        while (pio_try_get_io_write(&io_addr, &io_data))
        {
            uint8_t port = io_addr & 0xFFu;
            if (system_audio_handle_io_write(port, io_data))
                continue;
            if (mapper_enable && port >= 0xFCu && port <= 0xFFu)
                mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
        }

        while (pio_try_get_io_read(&io_addr))
        {
            uint8_t port = io_addr & 0xFFu;
            bool in_window = false;
            uint8_t data = 0xFFu;
            if (system_audio_handle_io_read(port, &data))
            {
                in_window = true;
            }
            else if (mapper_enable && port >= 0xFCu && port <= 0xFFu)
            {
                in_window = true;
                data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
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

                if (wifi_enable && active_subslot == 0u)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        in_window = true;
                        data = wifi_handle_mem_read(wifi_rom_base, WIFI_BIOS_ROM_SIZE, addr);
                    }
                }
                else if (active_subslot == nextor_subslot)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        in_window = true;
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
                }
                else if (mapper_enable && active_subslot == mapper_subslot)
                {
                    in_window = true;
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (addr & 0x3FFFu);
                    data = mapper_read_byte(mapper_offset);
                }
                else if (active_subslot == fmpac_subslot && addr >= 0x4000u && addr <= 0x7FFFu)
                {
                    in_window = true;
                    data = fmpac_handle_read(&fmpac, fmpac_bios_base, addr);
                }
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_fmpac)(uint32_t offset, bool cache_enable, bool mapper_enable)
{
    loadrom_sunrise_fmpac_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx, false, mapper_enable);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_fmpac_sd)(uint32_t offset, bool cache_enable, bool mapper_enable)
{
    loadrom_sunrise_fmpac_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx, false, mapper_enable);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_fmpac_wifi)(uint32_t offset, bool cache_enable, bool mapper_enable)
{
    loadrom_sunrise_fmpac_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx, true, mapper_enable);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_fmpac_wifi_sd)(uint32_t offset, bool cache_enable, bool mapper_enable)
{
    loadrom_sunrise_fmpac_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx, true, mapper_enable);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_scc_common)(
    uint32_t offset,
    bool cache_enable,
    sunrise_backend_task_fn_t core1_task,
    sunrise_backend_attach_fn_t attach_ctx,
    bool wifi_enable,
    bool mapper_enable)
{
    fmpac_wait_for_expanded_bootstrap();

    const uint8_t *rom_base;
    uint32_t available_length;
    prepare_sunrise_mapper_rom_source(offset, cache_enable, &rom_base, &available_length);
    const uint8_t *wifi_rom_base = rom_data + WIFI_BIOS_FLASH_OFFSET;

    if (mapper_enable)
    {
        if (!psram_prepare_mapper_region())
        {
            while (true) { tight_loop_contents(); }
        }
        mapper_fill_ff();
    }

    static sunrise_ide_t ide;
    sunrise_ide_init(&ide);

    attach_ctx(&ide);
    multicore_launch_core1(core1_task);
    if (wifi_enable)
        wifi_uart_init_once();

    uint8_t mapper_reg[4] = { 3, 2, 1, 0 };
    uint8_t subslot_reg = wifi_enable ? 0x20u : 0x10u;
    const uint8_t nextor_subslot = wifi_enable ? 1u : 0u;
    const uint8_t mapper_subslot = wifi_enable ? 2u : 1u;
    const uint8_t scc_subslot = wifi_enable ? 3u : 2u;

    msx_pio_io_bus_init();
    system_audio_init_for_sunrise(false);
    msx_pio_bus_init();

    while (true)
    {
        if (wifi_enable)
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

            if (wifi_enable && active_subslot == 0u)
            {
                (void)wifi_handle_mem_write(waddr, wdata);
            }
            else if (active_subslot == nextor_subslot)
            {
                if (waddr >= 0x4000u && waddr <= 0x7FFFu)
                    sunrise_ide_handle_write(&ide, waddr, wdata);
            }
            else if (mapper_enable && active_subslot == mapper_subslot)
            {
                uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (waddr & 0x3FFFu);
                mapper_write_byte(mapper_offset, wdata);
            }
            else if (active_subslot == scc_subslot)
            {
                SCC_write(&scc_instance, waddr, wdata);
            }
        }

        uint16_t io_addr;
        uint8_t io_data;
        while (pio_try_get_io_write(&io_addr, &io_data))
        {
            uint8_t port = io_addr & 0xFFu;
            if (system_audio_handle_io_write(port, io_data))
                continue;
            if (mapper_enable && port >= 0xFCu && port <= 0xFFu)
                mapper_reg[port - 0xFCu] = io_data & 0x3Fu;
        }

        while (pio_try_get_io_read(&io_addr))
        {
            uint8_t port = io_addr & 0xFFu;
            bool in_window = false;
            uint8_t data = 0xFFu;
            if (system_audio_handle_io_read(port, &data))
            {
                in_window = true;
            }
            else if (mapper_enable && port >= 0xFCu && port <= 0xFFu)
            {
                in_window = true;
                data = (uint8_t)(0xC0u | (mapper_reg[port - 0xFCu] & 0x3Fu));
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

                if (wifi_enable && active_subslot == 0u)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        in_window = true;
                        data = wifi_handle_mem_read(wifi_rom_base, WIFI_BIOS_ROM_SIZE, addr);
                    }
                }
                else if (active_subslot == nextor_subslot)
                {
                    if (addr >= 0x4000u && addr <= 0x7FFFu)
                    {
                        in_window = true;
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
                }
                else if (mapper_enable && active_subslot == mapper_subslot)
                {
                    in_window = true;
                    uint8_t mapper_page = mapper_page_from_reg(mapper_reg[page]);
                    uint32_t mapper_offset = ((uint32_t)mapper_page << 14) | (addr & 0x3FFFu);
                    data = mapper_read_byte(mapper_offset);
                }
                else if (active_subslot == scc_subslot)
                {
                    in_window = external_scc_read(addr, &data);
                }
            }

            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
        }
    }
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_scc)(uint32_t offset, bool cache_enable, bool mapper_enable)
{
    loadrom_sunrise_scc_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx, false, mapper_enable);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_scc_sd)(uint32_t offset, bool cache_enable, bool mapper_enable)
{
    loadrom_sunrise_scc_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx, false, mapper_enable);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_scc_wifi)(uint32_t offset, bool cache_enable, bool mapper_enable)
{
    loadrom_sunrise_scc_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx, true, mapper_enable);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_scc_wifi_sd)(uint32_t offset, bool cache_enable, bool mapper_enable)
{
    loadrom_sunrise_scc_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx, true, mapper_enable);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_sfg)(uint32_t offset, bool cache_enable, bool mapper_enable, ym2151_sfg_variant_t variant)
{
    loadrom_sunrise_sfg_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx, false, mapper_enable, variant);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_sfg_sd)(uint32_t offset, bool cache_enable, bool mapper_enable, ym2151_sfg_variant_t variant)
{
    loadrom_sunrise_sfg_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx, false, mapper_enable, variant);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_sfg_wifi)(uint32_t offset, bool cache_enable, bool mapper_enable, ym2151_sfg_variant_t variant)
{
    loadrom_sunrise_sfg_common(offset, cache_enable, sunrise_usb_task, sunrise_usb_set_ide_ctx, true, mapper_enable, variant);
}

static void __no_inline_not_in_flash_func(loadrom_sunrise_sfg_wifi_sd)(uint32_t offset, bool cache_enable, bool mapper_enable, ym2151_sfg_variant_t variant)
{
    loadrom_sunrise_sfg_common(offset, cache_enable, sunrise_sd_task, sunrise_sd_set_ide_ctx, true, mapper_enable, variant);
}

// loadrom_manbow2 - Manbow2 (Konami SCC-banked + AMD flash) without SCC audio
void __no_inline_not_in_flash_func(loadrom_manbow2)(uint32_t offset, bool cache_enable)
{
    (void)cache_enable;

    static const uint32_t WRITABLE_SECTOR_SIZE   = 0x10000u;  // 64KB
    static const uint32_t WRITABLE_SECTOR_OFFSET = 0x70000u;  // 448KB

    const uint8_t *rom_base;
    uint32_t available_length;
    uint8_t *writable_sram;
    loadrom_manbow2_setup(offset, &rom_base, &available_length, &writable_sram,
                           WRITABLE_SECTOR_OFFSET, WRITABLE_SECTOR_SIZE);

    manbow2_ctx_t mb = {
        .bank_regs       = {0, 1, 2, 3},
        .state           = MBW2_READ,
        .writable_sram   = writable_sram,
        .writable_offset = WRITABLE_SECTOR_OFFSET,
        .writable_size   = WRITABLE_SECTOR_SIZE,
    };

    msx_pio_bus_init();

    while (true)
    {
        // Busy-poll write FIFO + read FIFO together. Manbow2's AMD flash
        // command sequences burst multiple writes (0x555=0xAA, 0x2AA=0x55,
        // 0xA0/0x80, then data) without intervening reads — using a blocking
        // read here would let the write FIFO overflow and stall the bus.
        uint16_t addr;
        while (true)
        {
            uint16_t waddr; uint8_t wdata;
            while (pio_try_get_write(&waddr, &wdata))
                handle_manbow2_write(waddr, wdata, &mb);
            if (!pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
            {
                addr = (uint16_t)pio_sm_get(msx_bus.pio, msx_bus.sm_read);
                break;
            }
        }

        { uint16_t waddr; uint8_t wdata;
          while (pio_try_get_write(&waddr, &wdata))
              handle_manbow2_write(waddr, wdata, &mb); }

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            uint8_t page = (uint8_t)((addr - 0x4000u) >> 13);
            uint32_t rel = (uint32_t)mb.bank_regs[page] * 0x2000u + (addr & 0x1FFFu);

            if (mb.state == MBW2_AUTOSELECT)
            {
                // AM29F040B device IDs (manufacturer 0x01 AMD, device 0xA4)
                uint32_t id_addr = rel & 0x03u;
                if      (id_addr == 0x00u) data = 0x01u;
                else if (id_addr == 0x01u) data = 0xA4u;
                else if (id_addr == 0x02u)
                    data = (rel >= mb.writable_offset && rel < mb.writable_offset + mb.writable_size) ? 0x00u : 0x01u;
                else                       data = 0x00u;
            }
            else
            {
                if (rel >= mb.writable_offset && rel < mb.writable_offset + mb.writable_size)
                    data = writable_sram[rel - mb.writable_offset];
                else if (available_length == 0u || rel < available_length)
                    data = read_rom_byte(rom_base, rel); // SRAM cache up to rom_cached_size, then flash
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

// loadrom_manbow2_scc - Manbow2 mapper with SCC/SCC+ audio emulation
void __no_inline_not_in_flash_func(loadrom_manbow2_scc)(uint32_t offset, bool cache_enable, uint32_t scc_type)
{
    (void)cache_enable;

    static const uint32_t WRITABLE_SECTOR_SIZE   = 0x10000u;
    static const uint32_t WRITABLE_SECTOR_OFFSET = 0x70000u;

    const uint8_t *rom_base;
    uint32_t available_length;
    uint8_t *writable_sram;
    loadrom_manbow2_setup(offset, &rom_base, &available_length, &writable_sram,
                           WRITABLE_SECTOR_OFFSET, WRITABLE_SECTOR_SIZE);

    manbow2_ctx_t mb = {
        .bank_regs       = {0, 1, 2, 3},
        .state           = MBW2_READ,
        .writable_sram   = writable_sram,
        .writable_offset = WRITABLE_SECTOR_OFFSET,
        .writable_size   = WRITABLE_SECTOR_SIZE,
    };

    // Hold WAIT to freeze Z80 during audio subsystem init.
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    memset(&scc_instance, 0, sizeof(SCC));
    scc_instance.clk  = SCC_CLOCK;
    scc_instance.rate = SCC_SAMPLE_RATE;
    SCC_set_quality(&scc_instance, 1);
    scc_instance.type = scc_type;
    SCC_reset(&scc_instance);

    i2s_audio_init_scc();
    multicore_launch_core1(core1_scc_audio);

    msx_pio_bus_init();

    while (true)
    {
        // Busy-poll write FIFO + read FIFO together (see loadrom_manbow2 for
        // rationale — AMD flash command sequences burst writes without reads).
        uint16_t addr;
        while (true)
        {
            uint16_t waddr; uint8_t wdata;
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

        { uint16_t waddr; uint8_t wdata;
          while (pio_try_get_write(&waddr, &wdata))
          {
              handle_manbow2_write(waddr, wdata, &mb);
              SCC_write(&scc_instance, waddr, wdata);
          } }

        bool in_window = (addr >= 0x4000u) && (addr <= 0xBFFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            // SCC register read window
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
                    if      (id_addr == 0x00u) data = 0x01u;
                    else if (id_addr == 0x01u) data = 0xA4u;
                    else if (id_addr == 0x02u)
                        data = (rel >= mb.writable_offset && rel < mb.writable_offset + mb.writable_size) ? 0x00u : 0x01u;
                    else                       data = 0x00u;
                }
                else
                {
                    if (rel >= mb.writable_offset && rel < mb.writable_offset + mb.writable_size)
                        data = writable_sram[rel - mb.writable_offset];
                    else if (available_length == 0u || rel < available_length)
                        data = read_rom_byte(rom_base, rel);
                }
            }
        }

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

void __no_inline_not_in_flash_func(loadrom_wifi_config_setup)(uint32_t offset)
{
    const uint8_t *wifi_rom_base = flash_rom + offset;
    bool return_requested = false;

    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    wifi_uart_init_once();
    wifi_f2_state = WIFI_F2_FORCE_SETUP;
    msx_pio_bus_init();

    while (true)
    {
        wifi_service_rx();

        uint16_t waddr;
        uint8_t wdata;
        while (pio_try_get_write(&waddr, &wdata))
        {
            (void)wifi_handle_mem_write(waddr, wdata);
        }

        if (wifi_f2_state == WIFI_CONFIG_RETURN_MENU)
            return_requested = true;

        if (pio_sm_is_rx_fifo_empty(msx_bus.pio, msx_bus.sm_read))
        {
            if (return_requested &&
                !gpio_get(PIN_RD) &&
                ((gpio_get_all() & 0xFFFFu) == 0x0000u))
            {
                return;
            }
            tight_loop_contents();
            continue;
        }

        uint16_t addr = (uint16_t)pio_sm_get_blocking(msx_bus.pio, msx_bus.sm_read);

        while (pio_try_get_write(&waddr, &wdata))
        {
            (void)wifi_handle_mem_write(waddr, wdata);
        }

        if (wifi_f2_state == WIFI_CONFIG_RETURN_MENU)
            return_requested = true;

        if (return_requested && addr == 0x0000u)
        {
            pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(false, 0xFFu));
            return;
        }

        bool in_window = (addr >= 0x4000u && addr <= 0x7FFFu) ||
                         (addr >= WIFI_MEM_F2_ADDR && addr <= WIFI_MEM_DATA_ADDR);
        uint8_t data = wifi_handle_mem_read(wifi_rom_base, WIFI_CONFIG_ROM_SIZE, addr);

        pio_sm_put_blocking(msx_bus.pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}


// Main function running on core 0
int __no_inline_not_in_flash_func(main)()
{
    qmi_hw->m[0].timing = 0x40000202; // Set the QMI timing for the MSX bus
    set_sys_clock_khz(210000, true);     // Set system clock to 210Mhz

    stdio_init_all();     // Initialize stdio
    init_pico_chip_id();
    setup_gpio();     // Initialize GPIO

    while (true) {
    int rom_index = loadrom_msx_menu(0x0000); //load the first 32KB ROM into the MSX (The MSX PICOVERSE MENU)

    if ((uint16_t)rom_index == ROM_SELECT_WIFI_CONFIG) {
        active_rom_size = WIFI_CONFIG_ROM_SIZE;
        rom_data = flash_rom;
        rom_data_in_ram = false;
        loadrom_wifi_config_setup(WIFI_CONFIG_FLASH_OFFSET);
        continue;
    }

    ROMRecord const *selected = &records[rom_index];
    active_rom_size = selected->Size;

    uint8_t mapper = mapper_code_from_record_byte(selected->Mapper);
    audio_mode_t audio_mode = resolve_audio_mode(mapper, ctrl_audio_selection);
    // PSG Mirror is unstable on Sunrise Nextor + 1MB mapper: heavy disk I/O can
    // drop mapper segment-register writes, so force it off for those variants.
    if (mapper == MAPPER_SUNRISE_MAPPER_USB || mapper == MAPPER_SUNRISE_MAPPER_SD ||
        mapper == MAPPER_MEGARAM_SD || mapper == MAPPER_MEGARAM_USB || mapper == MAPPER_MEGARAM) {
        ctrl_psg_emulation = 0;
    }
    debug_trace("DBG launch rom=%d mapper=%u audio=%u mp3_started=%u", rom_index, mapper, (unsigned)audio_mode, mp3_core1_started ? 1u : 0u);
    if (audio_mode == AUDIO_MODE_MSX_MUSIC) {
        force_mp3_core1_handoff_before_rom_launch();
    } else {
        shutdown_mp3_core1_before_rom_launch();
    }
    debug_trace("DBG launch hold wait");
    hold_msx_wait();

    uint32_t rom_offset = selected->Offset;
    rom_data = flash_rom;
    rom_data_in_ram = false;

    bool is_sd_rom = (selected->Mapper & SOURCE_SD_FLAG) != 0;
    if (is_sd_rom) {
        debug_trace("DBG launch load sd");
        if (!load_rom_from_sd((uint16_t)rom_index, (uint32_t)selected->Size)) {
            printf("Debug: Failed to load ROM from SD card\n");
            while (true) { tight_loop_contents(); }
        }
        rom_data = sd_rom_region.ptr;
        rom_data_in_ram = true;
        rom_offset = 0;
        debug_trace("DBG launch sd loaded hold wait");
        hold_msx_wait();
    }

    // Cache the leading window into rom_sram for both flash- and PSRAM-resident
    // ROMs (SD ROMs are staged to PSRAM, so caching them into SRAM mirrors the
    // flash path and keeps the mapper hot loop fast).
    bool cache_enable = true;
    bool scc_audio = (audio_mode == AUDIO_MODE_SCC || audio_mode == AUDIO_MODE_SCC_PLUS ||
                      audio_mode == AUDIO_MODE_SCC_EXTERNAL || audio_mode == AUDIO_MODE_SCC_PLUS_EXTERNAL ||
                      audio_mode == AUDIO_MODE_MEGARAM_SCC || audio_mode == AUDIO_MODE_MEGARAM_SCC_PLUS);
    bool external_scc_audio = (audio_mode == AUDIO_MODE_SCC_EXTERNAL || audio_mode == AUDIO_MODE_SCC_PLUS_EXTERNAL);
    bool sfg_audio = (audio_mode == AUDIO_MODE_YM2151_SFG05 || audio_mode == AUDIO_MODE_YM2151_SFG01);
    bool cartridge_audio = scc_audio || sfg_audio || audio_mode == AUDIO_MODE_DUAL_PSG || audio_mode == AUDIO_MODE_MSX_MUSIC;
    bool psg_emulation = (ctrl_psg_emulation != 0u);
    bool system_mapper = is_system_mapper(mapper);
    // The 50/60Hz INIT patch only applies to regular game ROMs; the system ROMs
    // (Nextor/Sunrise/C2/MegaRAM) manage their own boot and must not be patched.
    vdp_freq_launch = system_mapper ? VDP_FREQ_DEFAULT : ctrl_vdp_frequency;
    cpu_mode_launch = system_mapper ? CPU_MODE_DEFAULT : ctrl_cpu_mode;
    // ASCII8 (mapper 5) maps block 0 into all four 8 KB windows at reset, so only
    // the first 8 KB of its image is reachable when INIT runs. Every other cached
    // game mapper starts with sequential blocks from 0x4000, so the stub may be
    // parked anywhere in the first 32 KB.
    boot_stub_search_limit = (mapper == 5u) ? BOOT_STUB_SEARCH_BLOCK0 : BOOT_STUB_SEARCH_PAGE1;
    bool wavegame_detected = wavegame_assets_detected_for_record((uint16_t)rom_index);
    wavegame_prepare_for_rom((uint16_t)rom_index,
        is_sd_rom && wavegame_detected && !system_mapper && audio_mode == AUDIO_MODE_NONE);
    if (wavegame_active) {
        msx_pio_io_write_bus_init();
    }
    if (psg_emulation && !system_mapper) {
        if (wavegame_active && audio_mode == AUDIO_MODE_NONE) {
            main_psg_init_with_io(PSG_QUALITY_HIGH, false);
            main_psg_core1_services_io = false;
            wavegame_psg_mirror_active = true;
            mp3_wavegame_set_psg_callbacks(wavegame_psg_calc_sample, wavegame_psg_set_sample_rate);
            printf("WAVEGAME: PSG mirror active\n");
        } else {
            main_psg_init(audio_mode == AUDIO_MODE_MSX_MUSIC ? PSG_QUALITY_FAST : PSG_QUALITY_HIGH);
        }
    }
    if (audio_mode == AUDIO_MODE_DUAL_PSG && !system_mapper) {
        start_dual_psg_audio();
    } else if (audio_mode == AUDIO_MODE_MSX_MUSIC) {
        start_msx_music_audio();
    } else if (psg_emulation && !cartridge_audio && !system_mapper && !wavegame_active) {
        start_main_psg_audio(true);
    }

    // Pre-start the WAVEGAME audio Core 1 here, during launch setup while
    // /WAIT is held, instead of lazily from inside the ROM bus-serving loop.
    // The intro's first OUT (0x92) write would otherwise trigger the blocking
    // multicore_launch_core1() + PSRAM buffer prep on Core 0 in the middle of a
    // bus cycle, starving MSX bus servicing long enough to hang the Z80. The
    // stall window depends on where the Z80 is when Core 0 blocks, which is why
    // the hang is intermittent and why warming the MP3 core up earlier (e.g. by
    // playing an MP3/WAV first) made it appear to work. Pre-warming here makes
    // the in-loop 0x92 handler a cheap, non-blocking command enqueue, matching
    // how every other audio profile launches Core 1 before its serving loop.
    if (wavegame_active) {
        // If a menu MP3/WAV played before this launch, shutdown_mp3_core1_*
        // handed its live I2S pool to rom_audio_handoff_pool. Reuse that pool
        // (and re-enable the DMA IRQ via the set_enabled toggle) so the
        // relaunched WAVEGAME audio core keeps the pico_audio_i2s pipeline
        // alive instead of re-initialising the singleton (which would leave the
        // WAVEGAME WAVs silent). On a cold launch the pool is NULL and the core
        // initialises I2S fresh as usual.
        struct audio_buffer_pool *wavegame_pool = claim_rom_audio_handoff_pool();
        if (wavegame_pool) {
            mp3_adopt_i2s_audio_pool(wavegame_pool);
        }
        ensure_mp3_core1_started();
    }

    bool wifi_support = (ctrl_wifi_support != 0u) && is_system_mapper(mapper);

    if (audio_mode == AUDIO_MODE_MSX_MUSIC && !system_mapper) {
        debug_trace("DBG launch load fmpac");
        loadrom_fmpac(rom_offset, cache_enable, mapper);
        continue;
    }

    if ((audio_mode == AUDIO_MODE_SCC_EXTERNAL || audio_mode == AUDIO_MODE_SCC_PLUS_EXTERNAL) && !system_mapper) {
        debug_trace("DBG launch load external scc");
        loadrom_external_scc(rom_offset, cache_enable, mapper, audio_mode == AUDIO_MODE_SCC_PLUS_EXTERNAL ? SCC_ENHANCED : SCC_STANDARD);
        continue;
    }

    if (sfg_audio && !system_mapper) {
        debug_trace("DBG launch load external sfg");
        loadrom_external_sfg(rom_offset, cache_enable, mapper,
                            audio_mode == AUDIO_MODE_YM2151_SFG01 ? YM2151_SFG01 : YM2151_SFG05);
        continue;
    }

    ym2151_sfg_variant_t sfg_variant = audio_mode == AUDIO_MODE_YM2151_SFG01 ? YM2151_SFG01 : YM2151_SFG05;

    if (is_sunrise_sd_mapper(mapper)) {
        sunrise_sd_select_partition(ctrl_sd_partition);
    }

    // Load the selected ROM into the MSX according to the mapper
    switch (mapper) {
       
        case 1:
        case 2:
            loadrom_plain32(rom_offset, cache_enable);
            break;
        case 3:
            if (scc_audio)
                loadrom_konamiscc_scc(rom_offset, cache_enable, audio_mode == AUDIO_MODE_SCC_PLUS ? SCC_ENHANCED : SCC_STANDARD);
            else
                loadrom_konamiscc(rom_offset, cache_enable);
            break;
        case 4:
            loadrom_linear48(rom_offset, cache_enable);
            break;
        case 5:
            loadrom_ascii8(rom_offset, cache_enable); 
            break;
        case 6:
            loadrom_ascii16(rom_offset, cache_enable); 
            break;
        case 7:
            loadrom_konami(rom_offset, cache_enable); 
            break;
        case 8:
            loadrom_neo8(rom_offset); 
            break;
        case 9:
            loadrom_neo16(rom_offset); 
            break;
        case MAPPER_SUNRISE_USB:
            if (external_scc_audio && wifi_support)
                loadrom_sunrise_scc_wifi(rom_offset, cache_enable, false);
            else if (external_scc_audio)
                loadrom_sunrise_scc(rom_offset, cache_enable, false);
            else if (audio_mode == AUDIO_MODE_MSX_MUSIC && wifi_support)
                loadrom_sunrise_fmpac_wifi(rom_offset, cache_enable, false);
            else if (audio_mode == AUDIO_MODE_MSX_MUSIC)
                loadrom_sunrise_fmpac(rom_offset, cache_enable, false);
            else if (sfg_audio && wifi_support)
                loadrom_sunrise_sfg_wifi(rom_offset, cache_enable, false, sfg_variant);
            else if (sfg_audio)
                loadrom_sunrise_sfg(rom_offset, cache_enable, false, sfg_variant);
            else if (wifi_support)
                loadrom_sunrise_wifi(rom_offset, cache_enable);
            else
                loadrom_sunrise(rom_offset, cache_enable);
            break;
        case MAPPER_SUNRISE_MAPPER_USB:
            if (external_scc_audio && wifi_support)
                loadrom_sunrise_scc_wifi(rom_offset, cache_enable, true);
            else if (external_scc_audio)
                loadrom_sunrise_scc(rom_offset, cache_enable, true);
            else if (audio_mode == AUDIO_MODE_MSX_MUSIC && wifi_support)
                loadrom_sunrise_fmpac_wifi(rom_offset, cache_enable, true);
            else if (audio_mode == AUDIO_MODE_MSX_MUSIC)
                loadrom_sunrise_fmpac(rom_offset, cache_enable, true);
            else if (sfg_audio && wifi_support)
                loadrom_sunrise_sfg_wifi(rom_offset, cache_enable, true, sfg_variant);
            else if (sfg_audio)
                loadrom_sunrise_sfg(rom_offset, cache_enable, true, sfg_variant);
            else if (wifi_support)
                loadrom_sunrise_mapper_wifi(rom_offset, cache_enable);
            else
                loadrom_sunrise_mapper(rom_offset, cache_enable);
            break;
        case MAPPER_SUNRISE_SD:
            if (external_scc_audio && wifi_support)
                loadrom_sunrise_scc_wifi_sd(rom_offset, cache_enable, false);
            else if (external_scc_audio)
                loadrom_sunrise_scc_sd(rom_offset, cache_enable, false);
            else if (audio_mode == AUDIO_MODE_MSX_MUSIC && wifi_support)
                loadrom_sunrise_fmpac_wifi_sd(rom_offset, cache_enable, false);
            else if (audio_mode == AUDIO_MODE_MSX_MUSIC)
                loadrom_sunrise_fmpac_sd(rom_offset, cache_enable, false);
            else if (sfg_audio && wifi_support)
                loadrom_sunrise_sfg_wifi_sd(rom_offset, cache_enable, false, sfg_variant);
            else if (sfg_audio)
                loadrom_sunrise_sfg_sd(rom_offset, cache_enable, false, sfg_variant);
            else if (wifi_support)
                loadrom_sunrise_wifi_sd(rom_offset, cache_enable);
            else
                loadrom_sunrise_sd(rom_offset, cache_enable);
            break;
        case MAPPER_SUNRISE_MAPPER_SD:
            if (external_scc_audio && wifi_support)
                loadrom_sunrise_scc_wifi_sd(rom_offset, cache_enable, true);
            else if (external_scc_audio)
                loadrom_sunrise_scc_sd(rom_offset, cache_enable, true);
            else if (audio_mode == AUDIO_MODE_MSX_MUSIC && wifi_support)
                loadrom_sunrise_fmpac_wifi_sd(rom_offset, cache_enable, true);
            else if (audio_mode == AUDIO_MODE_MSX_MUSIC)
                loadrom_sunrise_fmpac_sd(rom_offset, cache_enable, true);
            else if (sfg_audio && wifi_support)
                loadrom_sunrise_sfg_wifi_sd(rom_offset, cache_enable, true, sfg_variant);
            else if (sfg_audio)
                loadrom_sunrise_sfg_sd(rom_offset, cache_enable, true, sfg_variant);
            else if (wifi_support)
                loadrom_sunrise_mapper_wifi_sd(rom_offset, cache_enable);
            else
                loadrom_sunrise_mapper_sd(rom_offset, cache_enable);
            break;
        case MAPPER_C2_SD:
            loadrom_c2_sd(rom_offset, cache_enable);
            break;
        case MAPPER_C2_USB:
            loadrom_c2_usb(rom_offset, cache_enable);
            break;
        case MAPPER_MEGARAM_SD:
            loadrom_sunrise_megaram_sd(rom_offset, cache_enable);
            break;
        case MAPPER_MEGARAM_USB:
            loadrom_sunrise_megaram_usb(rom_offset, cache_enable);
            break;
        case MAPPER_MEGARAM:
            loadrom_megaram(rom_offset, cache_enable);
            break;
        case 12:
            loadrom_ascii16x(rom_offset, cache_enable);
            break;
        case 13:
            loadrom_planar64(rom_offset, cache_enable);
            break;
        case 14:
            if (scc_audio)
                loadrom_manbow2_scc(rom_offset, cache_enable, audio_mode == AUDIO_MODE_SCC_PLUS ? SCC_ENHANCED : SCC_STANDARD);
            else
                loadrom_manbow2(rom_offset, cache_enable);
            break;
        default:
                printf("Debug: Unsupported ROM mapper: %d\n", mapper);
            break;
    }
    }
    
}
