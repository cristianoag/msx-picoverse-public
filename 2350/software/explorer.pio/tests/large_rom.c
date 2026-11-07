#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
static int mkdir_host(const char *path) { return _mkdir(path); }
#else
#include <sys/stat.h>
static int mkdir_host(const char *path) { return mkdir(path, 0755); }
#endif
#define __noinline
#include "large-rom-types.h"
#include "mapper_detect.h"
#include "help_text.h"

enum { HOST_DATA_BUFFER_SIZE = DATA_BUFFER_SIZE };
static unsigned char mock_page_data[HOST_DATA_BUFFER_SIZE];
#undef MEMORY_START
#define MEMORY_START ((uintptr_t)mock_page_data)
#undef DATA_BUFFER_SIZE
#define DATA_BUFFER_SIZE HOST_DATA_BUFFER_SIZE
#define MemCopy(dst, src, length) memcpy((dst), (src), (length))

typedef uint64_t FSIZE_t;
typedef unsigned int UINT;
typedef enum { FR_OK, FR_DISK_ERR, FR_NO_FILE, FR_NO_PATH, FR_EXIST } FRESULT;
typedef uint32_t DWORD;
typedef struct { uint32_t csize; } FATFS;
typedef struct { FILE *file; } FIL;
typedef struct { uint16_t fdate, ftime; } FILINFO;
typedef bool (*fh_http_body_callback_t)(const uint8_t *, uint16_t, void *);
static bool fh_deliver_http_body(const uint8_t *, uint16_t, fh_http_decoder_t *, fh_http_body_callback_t, void *);

#define FA_WRITE 2u
#define FA_CREATE_ALWAYS 8u

static const char *work_dir;
static FIL fh_download_file;
static FILINFO fh_download_timestamp;
static char fh_wifi_status_text[FH_STATUS_TEXT_SIZE];
static uint16_t fh_progress_percent, fh_selected_index, fh_catalog_count = 1;
static uint8_t fh_active_type, fh_result, fh_tcp_last_error;
static char fh_active_query[FH_QUERY_SIZE];
static struct { char name[ROM_NAME_MAX + 1]; uint16_t size_kb; } fh_catalog[1];
static bool refresh_requested, root_selected, mounted = true;
static char sd_current_path[SD_PATH_MAX] = "/";
static char mock_download_dir[FH_DOWNLOAD_DIR_MAX + 1u] = "/";
static bool mock_dir_missing;
static bool sd_mounted, have_free_fs = true;
static uint8_t sd_mounted_partition, ctrl_sd_partition_info[CTRL_SD_PARTITION_INFO_SIZE];
static DWORD free_clusters;
static FATFS free_fs;
static FRESULT free_result = FR_OK;
static unsigned quiesce_count, service_count, http_count;
static uint32_t fattime_override, opened_fattime;
static uint64_t transfer_size, metadata_size, fail_write_at;
static bool short_write, fail_publish, fail_backup_cleanup, transport_ok = true;
static unsigned fail_close;
static uint16_t previous_progress;
static unsigned metadata_mode;

static FRESULT f_getfree(const char *path, DWORD *clusters, FATFS **fs) {
    assert(strcmp(path, "") == 0);
    *clusters = free_clusters;
    *fs = have_free_fs ? &free_fs : NULL;
    return free_result;
}

enum { META_SIZE, META_FALLBACK, META_INVALID, META_OVERFLOW, META_LONG, META_NONE };

static void host_path(const char *path, char *out, size_t capacity) {
    int n = snprintf(out, capacity, "%s\\%s", work_dir, path[0] == '/' ? path + 1 : path);
    assert(n > 0 && (size_t)n < capacity);
}

static bool file_exists(const char *path) {
    char host[1024];
    host_path(path, host, sizeof(host));
    FILE *file = fopen(host, "rb");
    if (!file) {
        assert(errno == ENOENT);
        return false;
    }
    assert(fclose(file) == 0);
    return true;
}

static FRESULT f_open(FIL *file, const char *path, unsigned mode) {
    assert(mode == (FA_CREATE_ALWAYS | FA_WRITE) && quiesce_count > 0);
    char host[1024];
    host_path(path, host, sizeof(host));
    file->file = fopen(host, "wb");
    opened_fattime = fattime_override;
    return file->file ? FR_OK : FR_DISK_ERR;
}

static FRESULT f_write(FIL *file, const void *data, UINT count, UINT *written) {
    assert(file->file && count <= FH_HTTP_CHUNK_SIZE);
    if (fail_write_at && (uint64_t)ftell(file->file) + count > fail_write_at) {
        *written = 0;
        return FR_DISK_ERR;
    }
    UINT want = short_write ? count - 1u : count;
    *written = (UINT)fwrite(data, 1, want, file->file);
    return *written == want ? FR_OK : FR_DISK_ERR;
}

static FRESULT f_close(FIL *file) {
    assert(file->file);
    if (fail_close) {
        fail_close--;
        return FR_DISK_ERR;
    }
    int rc = fclose(file->file);
    file->file = NULL;
    return rc == 0 ? FR_OK : FR_DISK_ERR;
}

static bool has_suffix(const char *name, const char *suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n >= s && strcmp(name + n - s, suffix) == 0;
}

static FRESULT f_rename(const char *from, const char *to) {
    if (file_exists(to)) return FR_EXIST;
    if (fail_publish && has_suffix(from, ".part")) {
        char backup[128];
        snprintf(backup, sizeof(backup), "%s.bak", to);
        if (file_exists(backup)) return FR_DISK_ERR;
    }
    char source[1024], dest[1024];
    host_path(from, source, sizeof(source));
    host_path(to, dest, sizeof(dest));
    return rename(source, dest) == 0 ? FR_OK : FR_DISK_ERR;
}

static FRESULT f_unlink(const char *path) {
    if (fail_backup_cleanup && has_suffix(path, ".bak")) return FR_DISK_ERR;
    char host[1024];
    host_path(path, host, sizeof(host));
    if (remove(host) == 0) return FR_OK;
    return errno == ENOENT ? FR_NO_FILE : FR_DISK_ERR;
}

static void fatfs_set_fattime_override(uint32_t time) { fattime_override = time; }
static uint32_t file_download_fattime(uint32_t time) { return time; }
static void sd_activity_note(void) {}
static bool sd_mount_card(void) { return mounted; }
static void set_root_path(void) { root_selected = true; }
// The firmware resolves the saved folder (and checks it still exists) on the card.
static void fh_download_dir_resolve(char *out, bool *missing) {
    *missing = mock_dir_missing;
    strcpy(out, mock_dir_missing ? "/" : mock_download_dir);
}
static void fh_quiesce_audio_for_qmi(void) { quiesce_count++; }
static void fh_copy_status_prefix(const char *text) {
    snprintf(fh_wifi_status_text, sizeof(fh_wifi_status_text), "%s", text);
}
static void fh_service_msx_reads(void) {
    assert(fh_progress_percent <= 100 && fh_progress_percent >= previous_progress);
    previous_progress = fh_progress_percent;
    service_count++;
}
static void fh_service_msx_reads_for(uint32_t duration) {
    assert(duration > 0 && fh_result == FH_RESULT_SAVING);
    fh_service_msx_reads();
}
static void fh_build_http_path(char *out, size_t size, const char *query, uint8_t type, int index) {
    (void)query;
    (void)type;
    assert(index == 0);
    snprintf(out, size, "/synthetic-rom");
}

static uint8_t pattern(uint64_t offset) {
    return (uint8_t)(offset * 37u + (offset >> 9));
}

static bool fh_http_get_with_timestamp(const char *path, fh_http_body_callback_t callback, void *ctx,
                                       uint32_t timeout, FILINFO *timestamp) {
    (void)path;
    assert(timeout > 0);
    http_count++;
    timestamp->fdate = 0x5CEBu;
    timestamp->ftime = 0x1234u;
    char metadata[160];
    switch (metadata_mode) {
        case META_FALLBACK: strcpy(metadata, "name: Large\r\n"); break;
        case META_INVALID: strcpy(metadata, "size: -1\r\n"); break;
        case META_OVERFLOW: strcpy(metadata, "size:18446744073709551616\r\n"); break;
        case META_LONG:
            memset(metadata, 'x', 150);
            strcpy(metadata + 150, "\r\n");
            break;
        case META_NONE: return transport_ok;
        default: snprintf(metadata, sizeof(metadata), "size:%llu\r\n", (unsigned long long)metadata_size); break;
    }
    size_t n = strlen(metadata);
    fh_http_decoder_t decoder = {.has_content_length = true, .content_remaining = n + transfer_size};
    if (!fh_deliver_http_body((const uint8_t *)metadata, 1, &decoder, callback, ctx)) return false;
    if (!fh_deliver_http_body((const uint8_t *)metadata + 1, (uint16_t)(n - 3), &decoder, callback, ctx)) return false;
    uint8_t chunk[FH_HTTP_CHUNK_SIZE];
    chunk[0] = '\r';
    chunk[1] = '\n';
    uint16_t first = (uint16_t)(transfer_size < 17u ? transfer_size : 17u);
    for (uint16_t i = 0; i < first; i++) chunk[i + 2u] = pattern(i);
    if (!fh_deliver_http_body(chunk, first + 2u, &decoder, callback, ctx)) return false;
    uint64_t offset = first;
    while (offset < transfer_size) {
        uint16_t count = (uint16_t)((transfer_size - offset > sizeof(chunk)) ? sizeof(chunk) : transfer_size - offset);
        for (uint16_t i = 0; i < count; i++) chunk[i] = pattern(offset + i);
        if (!fh_deliver_http_body(chunk, count, &decoder, callback, ctx)) return false;
        offset += count;
    }
    return transport_ok && decoder.done;
}

static ROMRecord records[FILES_PER_PAGE];
static uint8_t control[65536];
static uint16_t mock_flash_free, mock_flash_max;
static uint8_t mock_jiffy, mock_partition[32];
static int use_80_columns;
static unsigned char menu_shortcut_selection;
static unsigned char menu_pending_key;
static unsigned char menu_move_pending;
static unsigned char menu_pick_download;
static unsigned char fh_catalog_type, menu_ui_listing;
static char menu_ui_sd_text[32];
static int currentPage, totalPages;
static unsigned int totalFiles;
static int paging_enabled;
static unsigned char menu_message_row;
static char rendered_status[81];
static unsigned char rendered_footer[81], mock_screen[24][80];
static size_t rendered_length;
static unsigned char ui_col, ui_row, ui_keys[128];
static size_t ui_key_count, ui_key_index;
static char ui_input_lines[128][81];
static unsigned char mock_fnkstr[160];
#define CTRL_FLASH_FREE_KB ((uintptr_t)&mock_flash_free)
#define CTRL_FLASH_LARGEST_KB ((uintptr_t)&mock_flash_max)
#define CTRL_SD_PARTITION_INFO_BASE ((uintptr_t)mock_partition)
#define JIFFY ((uintptr_t)&mock_jiffy)
#define CTRL_CHIP_ID_BASE ((uintptr_t)(control + 0xBFAF))
#define BIOS_FNKSTR ((uintptr_t)mock_fnkstr)
static char last_status[80], last_error[80];
static char permission_key;
static unsigned questions, resets, selections, saves, prepare_commands, busy_ticks;
static unsigned settings_prepared;
static unsigned mapper_status_messages;
static unsigned detail_screens, flash_options_loaded, menu_refreshes;
static int currentIndex;
static int frame_rendered;
static unsigned char restore_pending;
static uint8_t flash_detail_mapper, flash_detail_audio;
static unsigned int detail_index;
static unsigned long imported_size;
static uint8_t prepare_result;
static uint8_t settings_mapper;
static unsigned int saved_index;
int record_needs_flash(const ROMRecord *record);
void loadGame(int index, unsigned char quick_run);
static void menu_ui_render_last_line(const char *text);
static unsigned char bios_chget(void);

static uint8_t Peek(unsigned int addr) { return control[addr]; }
static void Poke(unsigned int addr, unsigned int value) {
    control[addr] = (uint8_t)value;
    if (addr == CTRL_CMD) {
        assert(value == CMD_PREPARE_FLASH_ROM);
        assert(settings_prepared == 1 && questions == 1);
        prepare_commands++;
        busy_ticks = 3;
        control[CTRL_ACK] = control[CTRL_MAPPER] = 0;
    }
    if (addr == ROM_SELECT_REGISTER) selections++;
}
static void delay_ms(uint16_t time) {
    assert(time > 0);
    if (busy_ticks == 2) control[CTRL_MAPPER] = FLASH_COPY_DETECTING;
    if (busy_ticks && --busy_ticks == 0) {
        control[CTRL_CMD] = 0;
        control[CTRL_ACK] = prepare_result == 1 ? CTRL_MAGIC : 0;
        control[CTRL_MAPPER] = prepare_result == 1 ? 100 : prepare_result;
    }
}
static char bios_chget_quiet(void) {
    if (ui_key_index < ui_key_count) return (char)bios_chget();
    assert(settings_prepared == 0 && prepare_commands == 0);
    assert(strstr(last_status, "Y/N"));
    assert(strcmp(last_status, "ROM>4MB: Copy to flash? Y/N") == 0);
    questions++;
    return permission_key;
}
static unsigned help_pages_requested, help_ends, help_last_page;
static uint8_t run_ctrl_cmd(uint8_t cmd) {
    if (cmd == CMD_HELP_PAGE) {
        // The firmware puts the requested page in the data window.
        const help_page_t *pages = control[CTRL_QUERY_BASE + 1] ? help_pages_80 : help_pages_40;
        unsigned page = control[CTRL_QUERY_BASE];
        assert(page < HELP_PAGE_COUNT);
        memset(mock_page_data, 0, sizeof(mock_page_data));
        mock_page_data[0] = HELP_PAGE_COUNT;
        memcpy(mock_page_data + 1, pages[page].text, pages[page].size);
        help_last_page = page;
        help_pages_requested++;
        // CTRL_ACK holds another value while File Hunter is open: the menu must not use it.
        control[CTRL_ACK] = 0;
        return 0;
    }
    if (cmd == CMD_HELP_END) {
        help_ends++;
        return control[CTRL_ACK] = 1;
    }
    assert(cmd == CMD_PREPARE_QUICK_RUN || cmd == CMD_LOAD_OPTIONS);
    unsigned int index = control[CTRL_QUERY_BASE] | ((unsigned int)control[CTRL_QUERY_BASE + 1] << 8);
    ROMRecord *record = &records[index % FILES_PER_PAGE];
    if (record_needs_flash(record)) {
        assert(cmd == CMD_LOAD_OPTIONS);
        assert(questions == 1 && (permission_key | 0x20) == 'y');
        imported_size = record->Size;
        settings_prepared++;
    } else if (cmd == CMD_LOAD_OPTIONS) {
        assert(detail_screens == 1 && menu_shortcut_selection == MENU_SHORTCUT_FLASH);
        assert(index == detail_index && (record->Mapper & SOURCE_SD_FLAG) == 0);
        flash_options_loaded++;
    } else {
        settings_prepared++;
    }
    control[CTRL_MAPPER] = cmd == CMD_LOAD_OPTIONS && (record->Mapper & SOURCE_SD_FLAG) == 0 ?
                          flash_detail_mapper : settings_mapper;
    control[CTRL_AUDIO] = flash_options_loaded ? flash_detail_audio : 3;
    control[CTRL_ACK] = 1;
    return 1;
}
static void menu_ui_print_last_line_text(const char *text) {
    assert(strlen(text) <= 38);
    snprintf(last_status, sizeof(last_status), "%s", text);
    if (strcmp(text, "Mapper...") == 0) mapper_status_messages++;
    menu_ui_render_last_line(text);
}
static void menu_ui_print_progress(const char *action, unsigned int percent) {
    char text[32];
    assert(strcmp(action, use_80_columns ? "Copying to flash" : "Flash") == 0);
    snprintf(text, sizeof(text), "%s: %u%%", action, percent);
    menu_ui_print_last_line_text(text);
}
static void menu_ui_wait_key_with_blinking_status(const char *text) {
    assert(strlen(text) <= 38);
    snprintf(last_error, sizeof(last_error), "%s", text);
    menu_ui_render_last_line(text);
}
static void menu_ui_clear_last_line(void) { menu_ui_render_last_line(NULL); }
static unsigned char menu_ui_row_width(void) { return use_80_columns ? 80 : 40; }
static void Locate(unsigned char x, unsigned char y) {
    assert(x < menu_ui_row_width() && y < 24);
    ui_col = x;
    ui_row = y;
}
static void PrintChar(unsigned char ch) {
    assert(ui_col < menu_ui_row_width() && ui_row < 24);
    mock_screen[ui_row][ui_col++] = ch;
    if (ui_col == menu_ui_row_width()) {
        ui_col = 0;
        assert(++ui_row < 24);
    }
}
static void blit_row_vram(unsigned char row, const char *src, unsigned char width) {
    assert((row == 22 || row == 23) && width == menu_ui_row_width());
    memcpy(mock_screen[row], src, width);
    if (row == 23) {
        memcpy(rendered_status, src, width);
        rendered_status[width] = '\0';
        rendered_length = width;
    } else {
        memcpy(rendered_footer, src, width);
        rendered_footer[width] = '\0';
    }
}
static void menu_ui_clear_rows(unsigned char first, unsigned char end) {
    assert(first < end && end <= 24);
    for (unsigned char row = first; row < end; row++) {
        memset(mock_screen[row], ' ', menu_ui_row_width());
    }
}
static void menu_ui_print_title_line(void) {}
static void menu_ui_print_delimiter_line(void) {}
static int ui_printf(const char *format, ...) {
    char text[128];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(text));
    for (int i = 0; i < length; i++) PrintChar((unsigned char)text[i]);
    return length;
}
static unsigned char bios_chget(void) {
    assert(ui_key_index < ui_key_count);
    memcpy(ui_input_lines[ui_key_index], mock_screen[23], menu_ui_row_width());
    ui_input_lines[ui_key_index][menu_ui_row_width()] = '\0';
    return ui_keys[ui_key_index++];
}
static void set_ui_keys(const unsigned char *keys, size_t count) {
    assert(count <= sizeof(ui_keys));
    memcpy(ui_keys, keys, count);
    ui_key_count = count;
    ui_key_index = 0;
}
static void clear_fkeys(void) { memset(mock_fnkstr, 0, sizeof(mock_fnkstr)); }
static bool record_is_folder(const ROMRecord *record) { return (record->Mapper & FOLDER_FLAG) != 0; }
static bool record_is_mp3(const ROMRecord *record) { return (record->Mapper & MP3_FLAG) != 0; }
static uint8_t record_mapper_code(uint8_t mapper) { return mapper & 0x1Fu; }
static void refresh_menu_state(const char *message) {
    assert(message == NULL && control[CTRL_ACK] == CTRL_MAGIC);
    assert(restore_pending && !frame_rendered && menu_shortcut_selection == MENU_SHORTCUT_FLASH);
    assert(resets == 0 && selections == 0 && saves == 0);
    currentIndex = 57; // The sorted flash entry is not on the original SD page.
    restore_pending = 0;
    records[currentIndex % FILES_PER_PAGE] = (ROMRecord){.Mapper = flash_detail_mapper, .Size = imported_size};
    menu_refreshes++;
}
static void show_rom_screen(unsigned int index) {
    assert(index == (unsigned int)currentIndex && index == 57);
    assert(!record_needs_flash(&records[index % FILES_PER_PAGE]));
    assert((records[index % FILES_PER_PAGE].Mapper & SOURCE_SD_FLAG) == 0);
    assert(resets == 0 && selections == 0 && saves == 0);
    detail_screens++;
    detail_index = index;
    control[CTRL_QUERY_BASE] = (uint8_t)index;
    control[CTRL_QUERY_BASE + 1] = (uint8_t)(index >> 8);
    assert(run_ctrl_cmd(CMD_LOAD_OPTIONS));
}
static void save_last_selection(unsigned int index) {
    assert(prepare_commands == 0 || control[CTRL_ACK] == 1);
    saved_index = index;
    saves++;
}
static void execute_rst00(void) { resets++; }
#undef POKE_U16
#define POKE_U16(addr, value) do { control[addr] = (uint8_t)(value); control[(addr) + 1] = (uint8_t)((value) >> 8); } while (0)

#include "large-rom-production.h"

static void cleanup_files(void) {
    const char *names[] = {"/Large.ROM", "/Large.ROM.part", "/Large.ROM.bak",
                          "/Disk.DSK", "/Disk.DSK.part", "/Disk.DSK.bak"};
    fail_backup_cleanup = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        FRESULT fr = f_unlink(names[i]);
        assert(fr == FR_OK || fr == FR_NO_FILE);
    }
}

static void reset_download(uint64_t size) {
    cleanup_files();
    transfer_size = metadata_size = size;
    fh_catalog[0].size_kb = 65535; // Catalog size is advisory, not a download limit.
    strcpy(fh_catalog[0].name, "Large");
    fh_selected_index = 0;
    fh_active_type = FH_TYPE_ROM;
    fh_result = fh_tcp_last_error = 0;
    fh_progress_percent = previous_progress = 0;
    refresh_requested = root_selected = false;
    strcpy(mock_download_dir, "/");
    mock_dir_missing = false;
    strcpy(sd_current_path, "/");
    mounted = transport_ok = true;
    fail_write_at = 0;
    short_write = fail_publish = false;
    fail_close = 0;
    service_count = quiesce_count = http_count = 0;
    metadata_mode = META_SIZE;
}

static void write_old_file(const char *path) {
    char host[1024];
    host_path(path, host, sizeof(host));
    FILE *file = fopen(host, "wb");
    assert(file && fwrite("OLD", 1, 3, file) == 3 && fclose(file) == 0);
}

static void assert_old_file(const char *path) {
    char host[1024], data[4] = {0};
    host_path(path, host, sizeof(host));
    FILE *file = fopen(host, "rb");
    assert(file && fread(data, 1, sizeof(data), file) == 3 && fclose(file) == 0);
    assert(strcmp(data, "OLD") == 0);
}

static void assert_rom_file(const char *path, uint64_t size) {
    char host[1024];
    host_path(path, host, sizeof(host));
    FILE *file = fopen(host, "rb");
    assert(file);
    uint8_t data[2048];
    uint64_t offset = 0;
    size_t n;
    while ((n = fread(data, 1, sizeof(data), file)) != 0) {
        for (size_t i = 0; i < n; i++) assert(data[i] == pattern(offset + i));
        offset += n;
    }
    assert(!ferror(file) && fclose(file) == 0 && offset == size);
    assert(fh_result == 1 && fh_progress_percent == 100 && refresh_requested && root_selected);
    assert(opened_fattime != 0 && fattime_override == 0 && service_count > 0);
}

static void assert_download_failed(void) {
    assert(fh_result == 0 && !refresh_requested && !root_selected && fattime_override == 0);
    assert(!file_exists("/Large.ROM.part") && fh_wifi_status_text[0]);
}

static void test_downloads(void) {
    const uint64_t sizes[] = {8192u, 4194304u, 4194304u + 37u, 8388608u, 20u * 1024u * 1024u + 17u};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        reset_download(sizes[i]);
        assert(fh_download_selected());
        assert_rom_file("/Large.ROM", sizes[i]);
        assert(!file_exists("/Large.ROM.part") && !file_exists("/Large.ROM.bak"));
        assert(strcmp(fh_wifi_status_text, "Saved to microSD. Press key.") == 0);
    }
    reset_download(4194304u + 37u);
    metadata_mode = META_FALLBACK;
    fh_catalog[0].size_kb = 4097;
    assert(fh_download_selected()); // Rounded catalog KB must not reject a complete file.
    assert_rom_file("/Large.ROM", transfer_size);

    reset_download(8388608u);
    write_old_file("/Large.ROM");
    assert(fh_download_selected());
    assert_rom_file("/Large.ROM", transfer_size);
    assert(!file_exists("/Large.ROM.bak"));

    for (unsigned failure = 0; failure < 9; failure++) {
        reset_download(8192);
        write_old_file("/Large.ROM");
        switch (failure) {
            case 0: transport_ok = false; break;
            case 1: metadata_size++; break;
            case 2: metadata_size--; break;
            case 3: fail_write_at = 4096; break;
            case 4: short_write = true; break;
            case 5: fail_close = 1; break;
            case 6: metadata_mode = META_INVALID; break;
            case 7: metadata_mode = META_OVERFLOW; break;
            case 8: metadata_mode = META_LONG; break;
        }
        assert(!fh_download_selected());
        assert_download_failed();
        assert_old_file("/Large.ROM");
    }
    reset_download(8192);
    metadata_mode = META_NONE;
    assert(!fh_download_selected());
    assert_download_failed();

    reset_download(8192);
    write_old_file("/Large.ROM");
    fail_publish = true;
    assert(!fh_download_selected());
    assert_download_failed();
    assert_old_file("/Large.ROM");
    assert(!file_exists("/Large.ROM.bak"));

    reset_download(8192);
    write_old_file("/Large.ROM");
    write_old_file("/Large.ROM.bak");
    assert(!fh_download_selected());
    assert_download_failed();
    assert_old_file("/Large.ROM");
    assert_old_file("/Large.ROM.bak");

    reset_download(8192);
    write_old_file("/Large.ROM");
    fail_backup_cleanup = true;
    assert(fh_download_selected());
    assert_rom_file("/Large.ROM", transfer_size);
    assert_old_file("/Large.ROM.bak");
    assert(strstr(fh_wifi_status_text, ".bak") != NULL);

    reset_download(8192);
    mounted = false;
    assert(!fh_download_selected() && http_count == 0);
    reset_download(8192);
    fh_selected_index = 1;
    assert(!fh_download_selected() && http_count == 0);

    reset_download(720u * 1024u);
    fh_active_type = FH_TYPE_DSK;
    strcpy(fh_catalog[0].name, "Disk");
    assert(fh_download_selected());
    assert_rom_file("/Disk.DSK", transfer_size);
    const uint32_t unsupported[] = {180u * 1024u, 12u * 360u * 1024u};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        reset_download(unsupported[i]);
        fh_active_type = FH_TYPE_DSK;
        strcpy(fh_catalog[0].name, "Disk");
        assert(!fh_download_selected() && fh_result == 0);
        assert(!file_exists("/Disk.DSK") && !file_exists("/Disk.DSK.part"));
    }
    cleanup_files();

    // File Hunter saves into the chosen download folder and opens the list there.
    {
        char host[1024], dir[1024];
        host_path("/TFH", dir, sizeof(dir));
        if (mkdir_host(dir) != 0) assert(errno == EEXIST);
        reset_download(8192);
        strcpy(mock_download_dir, "/TFH");
        assert(fh_download_selected());
        assert(file_exists("/TFH/Large.ROM") && !file_exists("/Large.ROM") && !file_exists("/TFH/Large.ROM.part"));
        assert(strcmp(sd_current_path, "/TFH") == 0 && !root_selected && refresh_requested && fh_result == 1);
        assert(strcmp(fh_wifi_status_text, "Saved to microSD. Press key.") == 0);
        host_path("/TFH/Large.ROM", host, sizeof(host));
        assert(remove(host) == 0);

        // A saved folder that no longer exists: the file goes to the root and the user is told.
        reset_download(8192);
        strcpy(mock_download_dir, "/TFH");
        mock_dir_missing = true;
        assert(fh_download_selected());
        assert(file_exists("/Large.ROM") && root_selected);
        assert(strcmp(fh_wifi_status_text, "Saved in root: folder gone. Press key.") == 0);

        // The longest folder plus the longest name still fits the save path.
        char longest[FH_DOWNLOAD_DIR_MAX + 1u], long_name[ROM_NAME_MAX + 1u], save_path[FH_DOWNLOAD_PATH_SIZE];
        longest[0] = '/';
        memset(longest + 1, 'd', FH_DOWNLOAD_DIR_MAX - 1u);
        longest[FH_DOWNLOAD_DIR_MAX] = '\0';
        memset(long_name, 'n', ROM_NAME_MAX);
        long_name[ROM_NAME_MAX] = '\0';
        assert(fh_build_download_path(long_name, FH_TYPE_ROM, longest, save_path, sizeof(save_path)));
        assert(strlen(save_path) == FH_DOWNLOAD_DIR_MAX + 1u + ROM_NAME_MAX + 4u);
        assert(fh_build_download_path("Game", FH_TYPE_DSK, "/", save_path, sizeof(save_path)) && strcmp(save_path, "/Game.DSK") == 0);
        assert(fh_build_download_path("Game", FH_TYPE_ROM, "/A/B", save_path, sizeof(save_path)) && strcmp(save_path, "/A/B/Game.ROM") == 0);
    }
    cleanup_files();
}

static void test_large_counters(void) {
    for (uint64_t size = 1; size < 1000; size++) {
        for (uint64_t written = 0; written <= size; written++) {
            assert(fh_download_percent(written, size) == written * 100u / size);
        }
    }
    assert(fh_download_percent(UINT64_MAX / 2u, UINT64_MAX) == 49);
    assert(fh_download_percent(4ull << 30, 5ull << 30) == 80);
    uint64_t size;
    bool exact;
    assert(fh_parse_download_size("size:5000000000", 1, &size, &exact) && exact && size == 5000000000ull);
    assert(fh_parse_download_size("x", 1, &size, &exact) && !exact && size == 1024);
    assert(!fh_parse_download_size("size:-1", 1, &size, &exact));
    reset_download(8192);
    fh_sd_download_t state = {.metadata_done = true, .file_open = true, .size_exact = true,
                              .expected_size = 5ull << 30, .written = 4ull << 30};
    fh_quiesce_audio_for_qmi();
    assert(f_open(&fh_download_file, "/Large.ROM.part", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    uint8_t data[17] = {0};
    assert(fh_sd_download_body_callback(data, sizeof(data), &state));
    assert(state.written == (4ull << 30) + sizeof(data) && fh_progress_percent == 80);
    assert(f_close(&fh_download_file) == FR_OK);
    cleanup_files();
}

static void reset_launch(unsigned long size, char permission, uint8_t result) {
    memset(records, 0, sizeof(records));
    memset(control, 0, sizeof(control));
    records[0].Mapper = SOURCE_SD_FLAG | 8u;
    records[0].Size = size;
    permission_key = permission;
    prepare_result = result;
    settings_mapper = 8;
    settings_prepared = 0;
    mapper_status_messages = 0;
    detail_screens = flash_options_loaded = menu_refreshes = 0;
    currentIndex = 0;
    frame_rendered = 1;
    restore_pending = 0;
    menu_shortcut_selection = MENU_SHORTCUT_MICROSD;
    flash_detail_mapper = 8;
    flash_detail_audio = 3;
    detail_index = 0xFFFF;
    imported_size = 0;
    questions = resets = selections = saves = prepare_commands = busy_ticks = 0;
    last_status[0] = last_error[0] = '\0';
}

static void test_launches(void) {
    reset_launch(4194304UL, 'n', 1);
    loadGame(0, 0);
    assert(questions == 0 && prepare_commands == 0 && resets == 2 && selections == 1 && saves == 1);
    reset_launch(4194305UL, 'n', 1);
    loadGame(0, 0);
    assert(questions == 1 && prepare_commands == 0 && resets == 0 && selections == 0 && saves == 0);
    assert(settings_prepared == 0);
    assert(strstr(last_status, "Y/N"));
    reset_launch(8388608UL, 'Y', 1);
    loadGame(0, 0);
    assert(questions == 1 && prepare_commands == 1 && resets == 0 && selections == 0 && saves == 0);
    assert(detail_screens == 1 && detail_index == 57 && menu_refreshes == 1 && flash_options_loaded == 1);
    assert(mapper_status_messages > 0);
    assert(menu_shortcut_selection == MENU_SHORTCUT_FLASH);
    assert(settings_prepared == 1 && control[CTRL_AUDIO] == 3);
    control[CTRL_AUDIO] = 5; // Choose another option, then explicitly run the flash entry.
    loadGame(currentIndex, 0);
    assert(resets == 2 && selections == 1 && saves == 1 && saved_index == 57);
    assert(prepare_commands == 1 && questions == 1 && control[CTRL_AUDIO] == 5);
    reset_launch(8388608UL, 'Y', 1);
    records[0].Mapper = SOURCE_SD_FLAG;
    loadGame(0, 0);
    assert(settings_prepared == 1 && records[0].Mapper == 8u && detail_screens == 1 && resets == 0);
    reset_launch(8388608UL, 'Y', 1);
    flash_detail_mapper = 12;
    flash_detail_audio = 5;
    loadGame(0, 0);
    assert(detail_screens == 1 && records[0].Mapper == 12 && control[CTRL_AUDIO] == 5 && resets == 0);
    reset_launch(8388608UL, 'n', 1);
    quick_run_rom(0);
    assert(questions == 1 && settings_prepared == 0 && prepare_commands == 0);
    reset_launch(8388608UL, 'y', 1);
    quick_run_rom(0);
    assert(questions == 1 && settings_prepared == 1 && prepare_commands == 1 && resets == 0 && detail_screens == 1);
    reset_launch(4194304UL, 'n', 1);
    quick_run_rom(0);
    assert(questions == 0 && settings_prepared == 1 && prepare_commands == 0 && resets == 2);
    const uint8_t failures[] = {FLASH_COPY_NO_SPACE, FLASH_COPY_TOO_LARGE, FLASH_COPY_DUPLICATE,
                                FLASH_COPY_MAPPER, FLASH_COPY_IO};
    for (size_t i = 0; i < sizeof(failures); i++) {
        reset_launch(8388608UL, 'y', failures[i]);
        loadGame(0, 0);
        assert(prepare_commands == 1 && resets == 0 && selections == 0 && saves == 0 && last_error[0]);
        assert(detail_screens == 0 && menu_refreshes == 0 && menu_shortcut_selection == MENU_SHORTCUT_MICROSD);
        if (failures[i] == FLASH_COPY_NO_SPACE) assert(strstr(last_error, "clean F1"));
    }
    reset_launch(8388608UL, 'n', 1);
    records[0].Mapper = 8; // F1 never asks to import an already-flashed ROM.
    loadGame(0, 0);
    assert(questions == 0 && prepare_commands == 0 && resets == 2);
    reset_launch(8388608UL, 'n', 1);
    records[0].Mapper = MAPPER_ASCII16X_FR;
    control[CTRL_AUDIO] = AUDIO_PROFILE_MSX_MUSIC;
    loadGame(0, 0);
    assert(resets == 0 && selections == 0 && saves == 0 && strstr(last_error, "Mapper/audio"));
    control[CTRL_AUDIO] = 0;
    loadGame(0, 0);
    assert(resets == 2 && questions == 0);
    reset_launch(8388609UL, 'n', 1);
    records[0].Mapper = MAPPER_ASCII16X_FR;
    loadGame(0, 0);
    assert(resets == 0 && strstr(last_error, "Mapper/audio"));
    records[0].Mapper = 12; // The options screen can choose the read-only mapper.
    loadGame(0, 0);
    assert(resets == 2 && questions == 0);
    reset_launch(8388608UL, 'n', 1);
    records[0].Mapper = SOURCE_SD_FLAG | MP3_FLAG;
    assert(!record_needs_flash(&records[0]));
    records[0].Mapper = SOURCE_SD_FLAG | FOLDER_FLAG;
    assert(!record_needs_flash(&records[0]));
    records[0].Mapper = SOURCE_SD_FLAG | 23u;
    assert(!record_needs_flash(&records[0]));
}

static void test_mapper_ceiling(void) {
    uint32_t size = 15u * 1024u * 1024u + 8192u;
    uint8_t *rom = calloc(size, 1);
    assert(rom);
    rom[0] = 'A';
    rom[1] = 'B';
    memcpy(rom + 0x10, "ROM_NEO8", 8);
    assert(mapper_detect_buffer(rom, size) == MAPPER_DETECT_NEO8);
    free(rom);
}

static bool count_body(const uint8_t *data, uint16_t len, void *ctx) {
    (void)data;
    *(uint64_t *)ctx += len;
    return true;
}

static void test_http_lengths(void) {
    uint64_t size;
    assert(fh_parse_header_u64(" 5000000000\r\n", &size) && size == 5000000000ull);
    assert(fh_parse_header_u64("18446744073709551615", &size) && size == UINT64_MAX);
    assert(!fh_parse_header_u64("18446744073709551616", &size));
    assert(!fh_parse_header_u64("-1", &size));
    assert(!fh_parse_header_u64("12broken", &size));
    uint8_t data[32] = {0};
    uint64_t count = 0;
    fh_http_decoder_t decoder = {.has_content_length = true, .content_remaining = (1ull << 32) + 7u};
    assert(fh_deliver_http_body(data, 17, &decoder, count_body, &count));
    assert(count == 17 && decoder.content_remaining == (1ull << 32) - 10u && !decoder.done);
    decoder.content_remaining = 13; // Simulate the final frame of the same large transfer.
    assert(fh_deliver_http_body(data, 17, &decoder, count_body, &count));
    assert(count == 30 && decoder.done);

    decoder = (fh_http_decoder_t){.chunked = true};
    count = 0;
    const char *line = "100000007;ext=f";
    assert(fh_deliver_http_body((const uint8_t *)line, (uint16_t)strlen(line), &decoder, count_body, &count));
    assert(fh_deliver_http_body((const uint8_t *)"ffff\r\n", 6, &decoder, count_body, &count));
    assert(decoder.remaining == (1ull << 32) + 7u && !decoder.done);
    assert(fh_deliver_http_body(data, 17, &decoder, count_body, &count));
    assert(count == 17 && decoder.remaining == (1ull << 32) - 10u);
    decoder.remaining = 13;
    memcpy(data + 13, "\r\n0\r\n\r\n", 7);
    assert(fh_deliver_http_body(data, 20, &decoder, count_body, &count));
    assert(count == 30 && decoder.done);
    decoder = (fh_http_decoder_t){.chunked = true};
    line = "10000000000000000\r\n";
    assert(!fh_deliver_http_body((const uint8_t *)line, (uint16_t)strlen(line), &decoder, count_body, &count));
}

static void assert_centered_status(const char *text) {
    size_t width = menu_ui_row_width();
    size_t length = strlen(text);
    assert(length <= width && rendered_length == width);
    size_t padding = (width - length) / 2;
    if (memcmp(rendered_status + padding, text, length) != 0) {
        fprintf(stderr, "Expected centered [%s], got [%s]\n", text, rendered_status);
        assert(false);
    }
    for (size_t col = 0; col < padding; col++) assert(rendered_status[col] == ' ');
    for (size_t col = padding + length; col < width; col++) assert(rendered_status[col] == ' ');
}

static void test_sd_space_precision(void) {
    sd_mounted = true;
    sd_mounted_partition = 1;
    free_fs.csize = 1;
    const DWORD sectors[] = {0, 204, 205, 2047, 2048, 95u * 2048u + 1536u};
    const uint32_t whole[] = {0, 0, 0, 0, 1, 95};
    const uint8_t tenths[] = {0, 0, 1, 9, 0, 7};
    for (size_t i = 0; i < sizeof(sectors) / sizeof(sectors[0]); i++) {
        uint32_t mb = UINT32_MAX;
        uint8_t tenth = 255;
        free_clusters = sectors[i];
        assert(read_mounted_partition_free_mb(1, &mb, &tenth));
        assert(mb == whole[i] && tenth == tenths[i]);
    }
    set_browse_partition_status_text("GAMES", 1);
    assert(strcmp((char *)ctrl_sd_partition_info + 2, "GAMES (95.7MB FREE)") == 0);
    set_browse_partition_status_text("THIS IS A LONG PARTITION LABEL", 1);
    assert(strlen((char *)ctrl_sd_partition_info + 2) == CTRL_SD_PARTITION_INFO_SIZE - 3);
    assert(strstr((char *)ctrl_sd_partition_info + 2, " (95.7MB FREE)"));
    uint32_t mb;
    uint8_t tenth;
    assert(!read_mounted_partition_free_mb(2, &mb, &tenth));
    assert(!read_mounted_partition_free_mb(1, NULL, &tenth));
    assert(!read_mounted_partition_free_mb(1, &mb, NULL));
    free_result = FR_DISK_ERR;
    set_browse_partition_status_text("GAMES", 1);
    assert(strcmp((char *)ctrl_sd_partition_info + 2, "GAMES") == 0);
    free_result = FR_OK;
    have_free_fs = false;
    assert(!read_mounted_partition_free_mb(1, &mb, &tenth));
    have_free_fs = true;
    free_fs.csize = 0;
    assert(!read_mounted_partition_free_mb(1, &mb, &tenth));
    free_fs.csize = UINT32_MAX;
    free_clusters = UINT32_MAX;
    assert(read_mounted_partition_free_mb(1, &mb, &tenth) && mb == UINT32_MAX && tenth == 0);
    sd_mounted = false;
    assert(!read_mounted_partition_free_mb(1, &mb, &tenth));
}

static void write_page_u16(unsigned offset, unsigned value) {
    mock_page_data[offset] = (unsigned char)value;
    mock_page_data[offset + 1] = (unsigned char)(value >> 8);
}

static void write_page_u32(unsigned offset, unsigned long value) {
    for (unsigned i = 0; i < 4; i++) mock_page_data[offset + i] = (unsigned char)(value >> (8 * i));
}

static void test_page_records(void) {
    assert(ROM_RECORD_SIZE == 80);
    for (use_80_columns = 0; use_80_columns <= 1; use_80_columns++) {
        memset(control, 0, sizeof(control));
        memset(records, 0, sizeof(records));
        memset(mock_page_data, 0xFF, sizeof(mock_page_data));
        for (unsigned i = 0; i < 3; i++) {
            unsigned offset = i * ROM_RECORD_SIZE;
            char name[16];
            snprintf(name, sizeof(name), "Legacy%02u", i);
            memset(mock_page_data + offset, 0, ROM_RECORD_SIZE);
            memcpy(mock_page_data + offset, name, strlen(name) + 1);
            mock_page_data[offset + MAX_FILE_NAME_LENGTH] = SOURCE_SD_FLAG | (unsigned char)(i + 1);
            write_page_u32(offset + MAX_FILE_NAME_LENGTH + 1, 0x12345678UL + i);
            write_page_u32(offset + MAX_FILE_NAME_LENGTH + 5, 0xDEADBEEFUL + i);
        }
        control[CTRL_COUNT_L] = control[CTRL_COUNT_H] = 0xFF;
        readROMData(&totalFiles);
        assert(totalFiles == 3 && !paging_enabled && !menu_message_row);
        assert(strncmp(records[0].Name, "Legacy00", 8) == 0);
        assert(strncmp(records[2].Name, "Legacy02", 8) == 0);
        for (unsigned i = 0; i < 3; i++) {
            assert(records[i].Mapper == (SOURCE_SD_FLAG | (i + 1)));
            assert(records[i].Size == 0x12345678UL + i && records[i].Name[MAX_FILE_NAME_LENGTH] == '\0');
        }
        memset(records, 0x55, sizeof(records));
        load_page_records(0);
        assert(strncmp(records[1].Name, "Legacy01", 8) == 0 && records[1].Size == 0x12345679UL);
        for (unsigned i = 3; i < FILES_PER_PAGE; i++) {
            assert(records[i].Name[0] == '\0' && records[i].Mapper == 0 && records[i].Size == 0);
        }

        memset(mock_page_data, 0, sizeof(mock_page_data));
        memcpy(mock_page_data, "PVEX", 4);
        mock_page_data[DATA_HDR_VERSION] = DATA_VERSION;
        write_page_u16(DATA_HDR_SIZE, DATA_HEADER_SIZE);
        write_page_u16(DATA_HDR_TOTAL_COUNT, 21);
        write_page_u16(DATA_HDR_PAGE_COUNT, 2);
        write_page_u16(DATA_HDR_TABLE_OFFSET, 24);
        write_page_u16(DATA_HDR_TABLE_SIZE, 8);
        write_page_u16(DATA_HDR_STRING_OFFSET, 80);
        write_page_u16(DATA_HDR_STRING_SIZE, 11);
        write_page_u16(24, 32); write_page_u16(26, 16);
        write_page_u16(28, 48); write_page_u16(30, 13);
        const unsigned char first[] = {TLV_NAME_OFFSET, 2, 0, 0, TLV_MAPPER, 1, 8,
            TLV_SIZE, 4, 0x34, 0x12, 0, 0, 0x77, 1, 0x55};
        const unsigned char second[] = {TLV_NAME_OFFSET, 2, 6, 0, TLV_MAPPER, 1, SOURCE_SD_FLAG | 23,
            TLV_SIZE, 4, 0, 0xA0, 5, 0};
        memcpy(mock_page_data + 32, first, sizeof(first));
        memcpy(mock_page_data + 48, second, sizeof(second));
        memcpy(mock_page_data + 80, "Alpha\0Beta", 11);
        control[CTRL_STATUS] = CTRL_MAGIC;
        control[CTRL_COUNT_L] = 21;
        control[CTRL_COUNT_H] = 0;
        readROMData(&totalFiles);
        assert(totalFiles == 21 && paging_enabled && !menu_message_row);
        assert(strcmp(records[0].Name, "Alpha") == 0 && records[0].Mapper == 8 && records[0].Size == 0x1234UL);
        assert(strcmp(records[1].Name, "Beta") == 0 && records[1].Mapper == (SOURCE_SD_FLAG | 23));
        assert(records[1].Size == 368640UL && records[2].Name[0] == '\0');

        control[CTRL_STATUS] = CTRL_STATUS_SD_MISSING;
        readROMData(&totalFiles);
        assert(totalFiles == 1 && !paging_enabled && menu_message_row);
        assert(strcmp(records[0].Name, use_80_columns ? "No microSD card inserted." : "No microSD card") == 0);
        assert(records[0].Mapper == SOURCE_SD_FLAG && records[0].Size == 0);
        control[CTRL_STATUS] = 0;
    }
}

static void test_status_line(void) {
    mock_flash_free = 8192;
    mock_flash_max = 4096;
    for (use_80_columns = 0; use_80_columns <= 1; use_80_columns++) {
        menu_shortcut_selection = MENU_SHORTCUT_FLASH;
        currentPage = 2;
        totalPages = 7;
        menu_ui_render_menu_frame();
        unsigned char footer[81];
        memcpy(footer, rendered_footer, sizeof(footer));
        assert_centered_status("Page 02/07 | Available: 8.0 MB");
        assert(!strstr(rendered_status, "Max:") && !strstr(rendered_status, "F4"));
        mock_jiffy = 0x80;
        menu_ui_clear_last_line();
        assert_centered_status("Page 02/07 | Available: 8.0 MB");
        menu_ui_render_last_line("Loading...");
        assert_centered_status(use_80_columns ? "Page 02/07 | Available: 8.0 MB | Loading..." :
            "Page 02/07 | Loading...");
        menu_ui_render_last_line("Wait");
        assert_centered_status("Page 02/07 | Available: 8.0 MB | Wait");
        menu_ui_render_last_line("");
        assert_centered_status("Page 02/07 | Available: 8.0 MB");
        menu_ui_render_last_line("ROM>4MB: Copy to flash? Y/N");
        assert_centered_status(use_80_columns ?
            "Page 02/07 | Available: 8.0 MB | ROM>4MB: Copy to flash? Y/N" :
            "Page 02/07 | ROM>4MB: Copy to flash? Y/N");
        assert(memcmp(footer, rendered_footer, sizeof(footer)) == 0);

        char message[81];
        size_t capacity = menu_ui_row_width() - strlen("Page 02/07 | Available: 8.0 MB") - 3;
        memset(message, 'X', capacity);
        message[capacity] = '\0';
        menu_ui_render_last_line(message);
        assert(strstr(rendered_status, "Available: 8.0 MB"));
        assert(rendered_status[menu_ui_row_width() - 1] == 'X');
        message[capacity++] = 'X';
        message[capacity] = '\0';
        menu_ui_render_last_line(message);
        assert(!strstr(rendered_status, "Available:"));
        assert(strstr(rendered_status, message));
        char combined[96];
        snprintf(combined, sizeof(combined), "Page 02/07 | %s", message);
        assert_centered_status(combined);

        const uint16_t flash_kb[] = {0, 102, 103, 1023, 1024, 8294, 8295, 9215, 9216};
        const char *const amounts[] = {"0.0", "0.0", "0.1", "0.9", "1.0", "8.0", "8.1", "8.9", "9.0"};
        for (size_t i = 0; i < sizeof(flash_kb) / sizeof(flash_kb[0]); i++) {
            mock_flash_free = flash_kb[i];
            menu_ui_clear_last_line();
            snprintf(combined, sizeof(combined), "Page 02/07 | Available: %s MB", amounts[i]);
            assert_centered_status(combined);
        }

        mock_flash_free = UINT16_MAX;
        menu_ui_clear_last_line();
        assert_centered_status("Page 02/07 | Flash list full");
        mock_flash_free = 8192;
        currentPage = 123;
        totalPages = 3449;
        menu_ui_update_list_footer();
        assert_centered_status("Page 123/3449 | Available: 8.0 MB");
        currentPage = totalPages = 1;
        menu_ui_clear_last_line();
        assert_centered_status("Page 01/01 | Available: 8.0 MB");

        menu_shortcut_selection = MENU_SHORTCUT_MICROSD;
        strcpy((char *)mock_partition + 2, "GAMES(A) (95.7MB FREE)");
        mock_jiffy = 0;
        menu_ui_capture_sd_status();
        menu_ui_render_menu_frame();
        assert_centered_status("Page 01/01 | Available: 95.7 MB");
        assert(!strstr(rendered_status, "GAMES") && !strchr(rendered_status, ')'));
        mock_jiffy = 0x80;
        menu_ui_clear_last_line();
        assert_centered_status("Page 01/01 | Available: 95.7 MB");
        menu_ui_render_detail_frame();
        strcpy((char *)mock_partition + 2, "NEXTOR PARTITION");
        menu_ui_render_menu_frame();
        assert_centered_status("Page 01/01 | Available: 95.7 MB");
        mock_partition[2] = '\0';
        control[CTRL_STATUS] = CTRL_STATUS_SD_MISSING;
        menu_ui_capture_sd_status();
        menu_ui_clear_last_line();
        assert_centered_status("Page 01/01 | No microSD card");
        control[CTRL_STATUS] = 0;
        strcpy((char *)mock_partition + 2, "GAMES");
        menu_ui_capture_sd_status();
        menu_ui_clear_last_line();
        assert_centered_status("Page 01/01 | Available: unknown");
        strcpy((char *)mock_partition + 2, "GAMES (123)");
        menu_ui_capture_sd_status();
        menu_ui_clear_last_line();
        assert_centered_status("Page 01/01 | Available: unknown");
        strcpy((char *)mock_partition + 2, "(4294967295.9MB FREE)");
        menu_ui_capture_sd_status();
        menu_ui_clear_last_line();
        assert_centered_status("Page 01/01 | Available: 4294967295.9 MB");
        strcpy((char *)mock_partition + 2, "(95MB FREE)");
        menu_ui_capture_sd_status();
        menu_ui_clear_last_line();
        assert_centered_status("Page 01/01 | Available: 95.0 MB");

        menu_shortcut_selection = MENU_SHORTCUT_FILEHUNTER;
        for (fh_catalog_type = CTRL_FH_TYPE_ROM; fh_catalog_type <= CTRL_FH_TYPE_DSK; fh_catalog_type++) {
            control[CTRL_NET_STATUS] = NET_STATUS_ONLINE;
            menu_ui_render_menu_frame();
            assert_centered_status(fh_catalog_type ? "Page 01/01 | Listing: DSK | Net: Online" :
                "Page 01/01 | Listing: ROM | Net: Online");
            control[CTRL_NET_STATUS] = 0;
            menu_ui_clear_last_line();
            assert_centered_status(fh_catalog_type ? "Page 01/01 | Listing: DSK | Net: Offline" :
                "Page 01/01 | Listing: ROM | Net: Offline");
            menu_ui_render_last_line("Retrieving...");
            assert_centered_status(use_80_columns ?
                (fh_catalog_type ? "Page 01/01 | Listing: DSK | Retrieving..." :
                    "Page 01/01 | Listing: ROM | Retrieving...") :
                "Page 01/01 | Retrieving...");
        }
        unsigned char visible = 1, tick = 0;
        menu_ui_blink_last_line("Loading...", &visible, &tick, 1);
        assert(!visible);
        assert_centered_status("Page 01/01 | Listing: ROM");
        menu_ui_blink_last_line("Loading...", &visible, &tick, 1);
        assert(visible && strstr(rendered_status, "Loading..."));
        currentPage = totalPages = 3449;
        fh_catalog_type = CTRL_FH_TYPE_DSK;
        menu_ui_clear_last_line();
        assert_centered_status(use_80_columns ? "Page 3449/3449 | Listing: DSK | Net: Offline" :
            "Page 3449/3449 | Listing: DSK");

        menu_ui_render_detail_frame();
        menu_ui_render_last_line("Saved to microSD. Press key.");
        assert_centered_status("Saved to microSD. Press key.");
        assert(!strstr(rendered_status, "Page ") && !strstr(rendered_status, "Listing:"));
        menu_ui_clear_last_line();
        for (size_t col = 0; col < rendered_length; col++) assert(rendered_status[col] == ' ');
    }
}

static void assert_footer_options(const char *const *labels, unsigned count, unsigned selected) {
    unsigned width = menu_ui_row_width();
    unsigned inset = use_80_columns ? 1 : 0;
    unsigned bracket_width = inset * 2;
    unsigned total = 0;
    for (unsigned i = 0; i < count; i++) total += (unsigned)strlen(labels[i]) + bracket_width;
    assert(count >= 2 && total + count + 1 <= width - 2);
    unsigned space = width - 4 - total;
    unsigned gap_width = space / (count - 1);
    unsigned extra = space % (count - 1);
    unsigned start = 1;
    unsigned min_gap = width, max_gap = 0;
    unsigned highlight_start = 0, highlight_end = 0;
    unsigned char expected[80];
    memset(expected, ' ', width);
    for (unsigned i = 0; i < count; i++) {
        unsigned len = (unsigned)strlen(labels[i]);
        assert(start + len + bracket_width + 1 <= width - 2);
        if (inset) {
            expected[start] = '[';
            expected[start + len + 1] = ']';
        }
        memcpy(expected + start + inset, labels[i], len);
        len += bracket_width;
        if (selected == i + 1) {
            highlight_start = start - 1;
            highlight_end = start + len + 1;
        }
        start += len;
        if (i + 1 < count) {
            unsigned gap = gap_width + (i < extra ? 1 : 0);
            if (gap < min_gap) min_gap = gap;
            if (gap > max_gap) max_gap = gap;
            start += gap;
        }
    }
    assert(min_gap >= 1 && max_gap - min_gap <= 1 && start == width - 3);
    for (unsigned col = highlight_start; col < highlight_end; col++) expected[col] += 96;
    assert(memcmp(rendered_footer, expected, width) == 0);
    unsigned left_brackets = 0, right_brackets = 0;
    for (unsigned col = 0; col < width; col++) {
        unsigned char ch = rendered_footer[col];
        if (ch >= 128) ch -= 96;
        if (ch == '[') left_brackets++;
        if (ch == ']') right_brackets++;
    }
    assert(left_brackets == (use_80_columns ? count : 0));
    assert(right_brackets == left_brackets);
    if (selected) {
        unsigned first = 0;
        while (first < width && rendered_footer[first] < 128) first++;
        unsigned end = first;
        while (end < width && rendered_footer[end] >= 128) end++;
        assert(first == highlight_start && end == highlight_end);
        assert(rendered_footer[first] == ' ' + 96 && rendered_footer[end - 1] == ' ' + 96);
        for (unsigned col = end; col < width; col++) assert(rendered_footer[col] < 128);
        if (count == 5 && strncmp(labels[0], "F1 ", 3) == 0) {
            const unsigned starts_40[] = {1, 8, 15, 22, 30};
            const unsigned starts_80[] = {1, 16, 33, 54, 68};
            const unsigned *starts = use_80_columns ? starts_80 : starts_40;
            unsigned index = selected - 1;
            assert(first == starts[index] - 1);
            assert(end == starts[index] + strlen(labels[index]) + bracket_width + 1);
            for (unsigned i = 0; i < count; i++) {
                for (unsigned j = 0; j < strlen(labels[i]); j++) {
                    unsigned char ch = rendered_footer[starts[i] + inset + j];
                    if (i == index) ch -= 96;
                    assert(ch == (unsigned char)labels[i][j]);
                }
            }
        }
    }
}

static void test_footer_options(void) {
    const char *const flash_40[] = {"F1 FL", "F2 SD", "F3 FH", "F4 WIFI", "F5 HELP"};
    const char *const flash_80[] = {"F1 FLASH", "F2 SD CARD", "F3 FILE HUNTER", "F4 WIFI", "F5 HELP"};
    const char *const rom[] = {"ESC Back", "L/R Chg", "U/D Opt", "F5 Help"};
    const char *const rom_80[] = {"ESC Back", "L/R Change value", "U/D Select option", "ENTER Run", "F5 Help"};
    const char *const download[] = {"ESC Back", "ENTER Save", "A Folder", "F5 Help"};
    const char *const mp3_single[] = {"ESC Back", "ENTER Action", "F5 Help"};
    const char *const mp3_multi[] = {"ESC Back", "ENTER", "N Next", "P Prev", "F5 Help"};
    const char *const mp3_single_80[] = {"ESC Back", "U/D Select option", "ENTER Action", "F5 Help"};
    const char *const mp3_multi_80[] = {"ESC Back", "U/D Options", "ENTER Action", "N Next", "P Previous", "F5 Help"};
    const char *const uneven_labels[] = {"One", "Two", "Three", "Four", "Five"};
    for (use_80_columns = 0; use_80_columns <= 1; use_80_columns++) {
        for (menu_shortcut_selection = 1; menu_shortcut_selection <= 5; menu_shortcut_selection++) {
            menu_ui_print_footer_line(menu_shortcut_selection);
            assert_footer_options(use_80_columns ? flash_80 : flash_40, 5, menu_shortcut_selection);
        }
        for (unsigned count = 2; count <= 5; count++) {
            for (unsigned selected = 0; selected <= count; selected++) {
                menu_ui_render_footer("One\0Two\0Three\0Four\0Five", (unsigned char)count, (unsigned char)selected);
                assert_footer_options(uneven_labels, count, selected);
            }
        }
        menu_ui_render_detail_frame();
        render_rom_footer_line();
        assert_footer_options(use_80_columns ? rom_80 : rom, use_80_columns ? 5 : 4, 0);
        fh_render_detail_footer();
        assert_footer_options(download, 4, 0);
        control[MP3_CTRL_MODE] = MP3_PLAY_MODE_SINGLE;
        render_mp3_footer_line();
        assert_footer_options(use_80_columns ? mp3_single_80 : mp3_single, use_80_columns ? 4 : 3, 0);
        control[MP3_CTRL_MODE] = MP3_PLAY_MODE_ALL;
        render_mp3_footer_line();
        assert_footer_options(use_80_columns ? mp3_multi_80 : mp3_multi, use_80_columns ? 6 : 5, 0);
        control[MP3_CTRL_MODE] = MP3_PLAY_MODE_RANDOM;
        render_mp3_footer_line();
        assert_footer_options(use_80_columns ? mp3_multi_80 : mp3_multi, use_80_columns ? 6 : 5, 0);

        memcpy(control + 0xBFAF, "0123456789ABCDEF", CTRL_CHIP_ID_SIZE);
        const unsigned char keys[] = {'1', '2', '3', MENU_KEY_F4_CONFIG, MENU_KEY_F5_HELP, 27};
        for (size_t i = 0; i < sizeof(keys); i++) {
            unsigned ends = help_ends;
            menu_pending_key = 0;
            set_ui_keys(keys + i, 1);
            helpMenu(HELP_PAGE_LIST);
            assert_footer_options(use_80_columns ? flash_80 : flash_40, 5, MENU_SHORTCUT_HELP);
            assert_centered_status("Help 1/6 - LEFT/RIGHT page, key closes");
            assert(help_ends == ends + 1);
            assert(!strstr(ui_input_lines[0], "Any key") && !strstr(ui_input_lines[0], "Page "));
            assert(frame_rendered == 0 && menu_pending_key == (i < 4 ? keys[i] : 0));
        }
        assert(rendered_footer[menu_ui_row_width() - 2] == ' ');
        assert(rendered_footer[menu_ui_row_width() - 1] == ' ');
    }
    memset(mock_fnkstr, 0x55, sizeof(mock_fnkstr));
    redefine_function_keys();
    assert(mock_fnkstr[0] == '1' && mock_fnkstr[16] == '2' && mock_fnkstr[32] == '3');
    assert(mock_fnkstr[48] == MENU_KEY_F4_CONFIG && mock_fnkstr[49] == 0);
    assert(mock_fnkstr[64] == MENU_KEY_F5_HELP && mock_fnkstr[65] == 0);
}

// The F5 pages come from the firmware: they must fit the screen of the mode they
// are written for, and the help must page, clamp and close as documented.
static void test_help_pages(void) {
    for (int columns = 0; columns <= 1; columns++) {
        const help_page_t *pages = columns ? help_pages_80 : help_pages_40;
        unsigned max_width = columns ? HELP_WIDTH_80 : HELP_WIDTH_40;
        for (unsigned page = 0; page < HELP_PAGE_COUNT; page++) {
            const char *line = pages[page].text;
            const char *end = pages[page].text + pages[page].size;
            unsigned rows = 0;
            assert(pages[page].size > 1 && pages[page].size + 1 + 200 < DATA_BUFFER_SIZE);
            assert(end[-1] == '\0' && line[0] == '\1');
            while (line < end) {
                size_t length = strlen(line);
                assert(length > 0 && length - (*line == '\1') <= max_width);
                for (size_t i = (*line == '\1'); i < length; i++) {
                    assert((unsigned char)line[i] >= 0x20 && (unsigned char)line[i] < 0x7F);
                }
                rows++;
                line += length + 1;
            }
            assert(rows <= HELP_MAX_ROWS);
        }
    }

    use_80_columns = 1;
    menu_shortcut_selection = MENU_SHORTCUT_FLASH;
    {
        // RIGHT twice, LEFT once, then a key that closes: pages 0, 1, 2, 1.
        const unsigned char keys[] = {28, 28, 29, 'x'};
        unsigned requested = help_pages_requested, ends = help_ends;
        set_ui_keys(keys, sizeof(keys));
        helpMenu(HELP_PAGE_LIST);
        assert(help_pages_requested == requested + 4 && help_last_page == 1 && help_ends == ends + 1);
        assert_centered_status("Help 2/6 - LEFT/RIGHT page, key closes");
        // The first row of the page is its inverted heading.
        assert(mock_screen[2][0] == (unsigned char)(help_pages_80[1].text[1] + 96));
    }
    {
        // LEFT on the first page and RIGHT on the last one stay where they are.
        const unsigned char keys[] = {29, 28, 28, 28, 28, 28, 28, 28, 27};
        unsigned requested = help_pages_requested;
        set_ui_keys(keys, sizeof(keys));
        helpMenu(HELP_PAGE_LIST);
        assert(help_pages_requested == requested + sizeof(keys) && help_last_page == HELP_PAGE_COUNT - 1);
        assert_centered_status("Help 6/6 - LEFT/RIGHT page, key closes");
    }
    {
        // Each screen opens the page that describes it.
        const unsigned char key = 27;
        const unsigned char pages[] = {HELP_PAGE_LIST, HELP_PAGE_ROM, HELP_PAGE_MP3, HELP_PAGE_FILEHUNTER};
        for (size_t i = 0; i < sizeof(pages); i++) {
            set_ui_keys(&key, 1);
            helpMenu(pages[i]);
            assert(help_last_page == pages[i]);
        }
    }
}

// The list rows are built without printf now. They must match what the old
// sprintf-based builder and the old selectable-line renderer produced.
static void reference_row(const ROMRecord *record, const char *name, char *out, unsigned char width, int selected) {
    // The row layouts the menu printed before the printf-free builder.
    const char *const format_80 = " %-61.61s %6s %-2s %-5s";
    const char *const format_40 = " %-21.21s %6s %-2s %-5s";
    const char *source = (record->Mapper & SOURCE_SD_FLAG) ? "SD" : "FL";
    const char *type_label = " ROM";
    char size_text[16] = "";
    char text[96];
    unsigned long size_kb = record->Size / 1024u;

    if (record_is_folder(record)) {
        type_label = "<DIR>";
        source = "SD";
    } else if (record_is_mp3(record) && ((record->Mapper & AUDIO_TYPE_MASK) == AUDIO_TYPE_WAV)) {
        type_label = " WAV";
        source = "SD";
    } else if (record_is_mp3(record)) {
        type_label = " MP3";
        source = "SD";
    } else if (record_mapper_code(record->Mapper) == 23) {
        type_label = " DSK";
    }
    if (!record_is_folder(record)) {
        if (size_kb <= 1024u) {
            snprintf(size_text, sizeof(size_text), "%luKB", size_kb);
        } else {
            snprintf(size_text, sizeof(size_text), "%luMB", (size_kb + 1023u) / 1024u);
        }
    }
    snprintf(text, sizeof(text), width == 80 ? format_80 : format_40, name, size_text, source, type_label);
    size_t length = strlen(text);
    for (size_t i = length; i < width; i++) text[i] = ' ';
    text[width] = '\0';

    // The old selectable-line renderer, fed with the text after its first space.
    const char *content = text + 1;
    unsigned char content_width = width - 1, selected_width = width - 3;
    size_t content_length = strlen(content);
    if (content_length > content_width) content_length = content_width;
    out[0] = selected ? '>' : ' ';
    if (selected) {
        for (unsigned i = 0; i < selected_width; i++) out[1 + i] = (char)((i < content_length ? content[i] : ' ') + 96);
        for (unsigned i = selected_width; i < content_width; i++) out[1 + i] = ' ';
    } else {
        for (unsigned i = 0; i < content_width; i++) out[1 + i] = i < content_length ? content[i] : ' ';
    }
}

static void test_list_rows(void) {
    const unsigned long sizes[] = {0, 1, 1023, 1024, 1025, 4096, 524288, 1048575, 1048576, 1048576 + 1023, 1048576 + 1024,
                                   1048576 + 1025, 2097152, 3145728 + 1, 33554432UL, 67108864UL + 5000, 4294967295UL};
    const unsigned char mappers[] = {SOURCE_SD_FLAG | 4, 7, FOLDER_FLAG | SOURCE_SD_FLAG, MP3_FLAG | SOURCE_SD_FLAG,
                                     MP3_FLAG | SOURCE_SD_FLAG | AUDIO_TYPE_WAV, SOURCE_SD_FLAG | 23, 23};
    const char *const names[] = {"", "A", "Game", "123456789012345678901", "1234567890123456789012",
                                 "1234567890123456789012345678901234567890123456789012345678901",
                                 "12345678901234567890123456789012345678901234567890123456789012",
                                 "12345678901234567890123456789012345678901234567890123456789012345678901"};
    char row[81], expected[81];

    for (use_80_columns = 0; use_80_columns <= 1; use_80_columns++) {
        unsigned char width = menu_ui_row_width();
        for (size_t m = 0; m < sizeof(mappers); m++) {
            for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
                for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
                    ROMRecord record = {.Mapper = mappers[m], .Size = sizes[s]};
                    snprintf(record.Name, sizeof(record.Name), "%s", names[n]);
                    for (int selected = 0; selected <= 1; selected++) {
                        memset(row, 0x55, sizeof(row));
                        build_record_row(&record, NULL, row, (unsigned char)selected);
                        reference_row(&record, record.Name, expected, width, selected);
                        assert(memcmp(row, expected, width) == 0 && row[width] == 0x55);
                        // A sliding name window replaces the name only.
                        build_record_row(&record, "sliding", row, (unsigned char)selected);
                        reference_row(&record, "sliding", expected, width, selected);
                        assert(memcmp(row, expected, width) == 0);
                    }
                }
            }
        }
    }
    // The catalog sizes use the same number formatter.
    for (unsigned value = 0; value < 65536; value += (value < 2000 ? 1 : 37)) {
        char text[16], expected_text[16];
        menu_ui_size_text(text, (uint16_t)value, 'M');
        snprintf(expected_text, sizeof(expected_text), "%uMB", value);
        assert(strcmp(text, expected_text) == 0);
    }
    use_80_columns = 0;
}

static void test_line_editor(void) {
    for (use_80_columns = 0; use_80_columns <= 1; use_80_columns++) {
        for (menu_shortcut_selection = MENU_SHORTCUT_FLASH;
             menu_shortcut_selection <= MENU_SHORTCUT_FILEHUNTER; menu_shortcut_selection++) {
            currentPage = 2;
            totalPages = 7;
            fh_catalog_type = CTRL_FH_TYPE_DSK;
            mock_flash_free = 8192;
            strcpy((char *)mock_partition + 2, "GAMES (95.7MB FREE)");
            menu_ui_capture_sd_status();
            menu_ui_render_menu_frame();
            char basic[81];
            unsigned char footer[81];
            memcpy(basic, rendered_status, sizeof(basic));
            memcpy(footer, rendered_footer, sizeof(footer));
            const unsigned char cancel_keys[] = {'K', 'i', 'n', 'g', 27};
            char query[25] = "";
            set_ui_keys(cancel_keys, sizeof(cancel_keys));
            assert(menu_ui_read_line("Search: ", query, 24) == 0);
            assert(strncmp(ui_input_lines[0], "Search: ", 8) == 0);
            assert(strncmp(ui_input_lines[4], "Search: King", 12) == 0);
            assert(strcmp(rendered_status, basic) == 0 && strcmp(query, "King") == 0);
            assert(memcmp(footer, rendered_footer, sizeof(footer)) == 0);

            const unsigned char edit_keys[] = {'A', 'B', 8, 'C', 13};
            query[0] = '\0';
            set_ui_keys(edit_keys, sizeof(edit_keys));
            assert(menu_ui_read_line("Search: ", query, 24) == 1 && strcmp(query, "AC") == 0);
            assert(strcmp(rendered_status, basic) == 0);
            unsigned char limit_keys[27];
            memset(limit_keys, 'A', 26);
            limit_keys[26] = 13;
            query[0] = '\0';
            set_ui_keys(limit_keys, sizeof(limit_keys));
            assert(menu_ui_read_line("Search: ", query, 24) == 1 && strlen(query) == 24);
            assert(strcmp(rendered_status, basic) == 0);

            const unsigned char function_keys[] = {MENU_KEY_F4_CONFIG, MENU_KEY_F5_HELP};
            for (size_t i = 0; i < sizeof(function_keys); i++) {
                query[0] = '\0';
                set_ui_keys(function_keys + i, 1);
                assert(menu_ui_read_line("Search: ", query, 24) == -(int)(i + 1));
                assert(strcmp(rendered_status, basic) == 0);
            }
            const unsigned char enter[] = {13};
            strcpy(query, "Original name");
            set_ui_keys(enter, sizeof(enter));
            assert(menu_ui_read_line("Name: ", query, 5) == 2 && strcmp(query, "Origi") == 0);
            assert(strcmp(rendered_status, basic) == 0);
        }
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    work_dir = argv[1];
    test_downloads();
    test_large_counters();
    for (use_80_columns = 0; use_80_columns <= 1; use_80_columns++) test_launches();
    test_mapper_ceiling();
    test_http_lengths();
    test_sd_space_precision();
    test_page_records();
    test_status_line();
    test_footer_options();
    test_help_pages();
    test_list_rows();
    test_line_editor();
    puts("PASS: legacy/PVEX decoding, streamed downloads, flash options, exact SD/flash MB tenths, centered status, contextual controls, F5 Help and search restoration in 40/80 columns");
    return 0;
}
