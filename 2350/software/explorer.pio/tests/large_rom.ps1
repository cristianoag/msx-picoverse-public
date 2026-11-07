param([string]$CC = "gcc", [string]$Tcl = "tclsh", [switch]$HostOnly)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$firmware = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw
$menu = Get-Content (Join-Path $root "msx\src\menu.c") -Raw
$header = Get-Content (Join-Path $root "msx\src\menu.h") -Raw
$fh = Get-Content (Join-Path $root "msx\src\explorer_fh.c") -Raw
$ui = Get-Content (Join-Path $root "msx\src\menu_ui.c") -Raw
$screenRom = Get-Content (Join-Path $root "msx\src\screen_rom.c") -Raw
$menuState = Get-Content (Join-Path $root "msx\src\menu_state.h") -Raw
$dsk = Get-Content (Join-Path $root "pico\explorer\storage\sunrise_dsk.h") -Raw

function Get-CFunction([string]$source, [string]$name) {
    $match = [regex]::Match($source, "(?m)^(?:static[^\r\n]*|(?:void|int|unsigned char|const char \*)[^\r\n]*)\b$name\b\)?\s*\([^;{]*\)\s*\{")
    if (!$match.Success) { throw "Cannot find function $name" }
    $end = $match.Index + $match.Length
    $depth = 1
    while ($depth -gt 0 -and $end -lt $source.Length) {
        if ($source[$end] -eq '{') { $depth++ }
        if ($source[$end] -eq '}') { $depth-- }
        $end++
    }
    if ($depth -ne 0) { throw "Unclosed function $name" }
    return $source.Substring($match.Index, $end - $match.Index)
}

foreach ($name in @("CMD_PREPARE_FLASH_ROM", "CMD_HELP_PAGE", "CMD_HELP_END", "CMD_SET_DOWNLOAD_DIR", "CMD_FH_DOWNLOAD_DIR", "CTRL_FLASH_FREE_KB", "CTRL_FLASH_LARGEST_KB",
                    "FLASH_COPY_DETECTING",
                    "FLASH_COPY_NO_SPACE", "FLASH_COPY_TOO_LARGE", "FLASH_COPY_DUPLICATE",
                    "FLASH_COPY_MAPPER", "FLASH_COPY_IO")) {
    $picoValue = [regex]::Match($firmware, "(?m)^#define $name\s+(0x[0-9A-Fa-f]+)")
    $menuValue = [regex]::Match($header, "(?m)^#define $name\s+(0x[0-9A-Fa-f]+)")
    if (!$picoValue.Success -or !$menuValue.Success -or
        [Convert]::ToUInt32($picoValue.Groups[1].Value, 16) -ne [Convert]::ToUInt32($menuValue.Groups[1].Value, 16)) {
        throw "Firmware and menu disagree on $name"
    }
}
if ($fh -match "fh_record_fits_sd_cache|exceeds 4MB|Action: Too large|Locate\(0, 7\)") {
    throw "File Hunter still has a size block or uses the main screen for download status"
}
# The help pages come from the firmware, which must rebuild the list page they replace.
$helpHandler = Get-CFunction $firmware "handle_menu_write_explorer"
if ($helpHandler -notmatch "data == CMD_HELP_PAGE\)\s*\{\s*process_help_page_request\(\);" -or
    $helpHandler -notmatch "data == CMD_HELP_END\)\s*\{\s*process_help_end_request\(\);" -or
    $firmware -notmatch "static void __noinline process_help_page_request\(void\)" -or
    $firmware -notmatch "static void __noinline process_help_end_request\(void\)" -or
    (Get-CFunction $firmware "process_help_end_request") -notmatch "fh_build_page_buffer\(\);[\s\S]*build_page_buffer\(current_page\);" -or
    (Get-CFunction $menu "helpMenu") -notmatch "run_ctrl_cmd\(CMD_HELP_END\)") {
    throw "F5 help must be served by the firmware and close with CMD_HELP_END"
}
if ($menu -match "static const char help_text\[\]") {
    throw "The help text belongs in pico/explorer/help_text.h, not in the menu ROM"
}
# The download folder (A): the Pico answers CMD_SET_DOWNLOAD_DIR without a refresh, the menu
# reads the failure reason from CTRL_MAPPER, and downloads go to the resolved folder.
if ([regex]::Match($firmware, "(?m)^#define DOWNLOAD_DIR_ERR_LONG\s+(\d+)u").Groups[1].Value -ne
    [regex]::Match($header, "(?m)^#define DOWNLOAD_DIR_ERR_LONG\s+(\d+)").Groups[1].Value) {
    throw "Firmware and menu disagree on DOWNLOAD_DIR_ERR_LONG"
}
if ($helpHandler -notmatch "data == CMD_SET_DOWNLOAD_DIR\)\s*\{\s*process_set_download_dir_request\(\);" -or
    $firmware -notmatch "static void __noinline process_set_download_dir_request\(void\)" -or
    $firmware -notmatch "data == CMD_FH_WIFI_CONFIG \|\| data == CMD_FH_DOWNLOAD_DIR\)") {
    throw "The download folder commands must be dispatched by the menu write handler (and stay out of line)"
}
if ((Get-CFunction $firmware "fh_download_selected") -notmatch "(?s)fh_download_dir_resolve\(save_dir, &dir_missing\);.*fh_build_download_path\([^;]*save_dir,") {
    throw "File Hunter downloads must go to the resolved download folder"
}
if ((Get-CFunction $fh "fh_wait_download_ready") -notmatch "menu_ui_print_progress" -or
    (Get-CFunction $fh "fh_show_detail") -notmatch "menu_ui_wait_key_with_blinking_status\(fh_status_text\)") {
    throw "File Hunter download results must use line 24"
}
$statusRenderer = Get-CFunction $ui "menu_ui_render_last_line"
if ($statusRenderer -notmatch '"Page %02d/%02d"' -or
    $statusRenderer -notmatch 'blit_row_vram\(23, row, width\)' -or
    $statusRenderer -notmatch "CTRL_FLASH_FREE_KB" -or
    $statusRenderer -match 'CTRL_FLASH_LARGEST_KB|Max:|JIFFY') {
    throw "List status must retain its page prefix, use the full row and show stable available space"
}
if ((Get-CFunction $menu "redefine_function_keys") -notmatch 'fnkstr\[64\] = MENU_KEY_F5_HELP;' -or
    (Get-CFunction $menu "navigateMenu") -notmatch 'case MENU_KEY_F5_HELP:') {
    throw "F5 must activate Help from Explorer lists, including empty lists"
}
foreach ($screen in @((Get-CFunction $screenRom "show_rom_screen"),
                     (Get-CFunction $screenRom "show_mp3_screen"),
                     (Get-CFunction $fh "fh_show_detail"))) {
    if ($screen -notmatch 'MENU_KEY_F5_HELP' -or $screen -match 'MENU_KEY_F4_CONFIG|launch_wifi_config') {
        throw "Detail screens must support F5 Help, not F4 Wi-Fi setup"
    }
}
if ($menu -match "case 72:|case 104:" -or $fh -match "case 'h':|case 'H':" -or
    $screenRom -match "key == 'h'|key == 'H'") {
    throw "H must no longer be the Help shortcut"
}
if ((Get-CFunction $fh "explorer_fh_run") -notmatch 'menu_shortcut_selection = MENU_SHORTCUT_FILEHUNTER;' -or
    (Get-CFunction $fh "fh_render_frame") -match 'saved_page|saved_total|saved_selection') {
    throw "File Hunter must keep its source and page state active for status and search restoration"
}
$detail = Get-CFunction $screenRom "show_rom_screen"
$detailGuard = [regex]::Match($detail, "(?s)if \(record_needs_flash\(record\)\) \{\s*loadGame\(\(int\)index, 1\);\s*return;\s*\}")
if (!$detailGuard.Success) { throw "Large-ROM detail selection must route directly to the flash prompt" }
foreach ($operation in @("build_cpu_mode_list(", "render_rom_screen(", "send_detect_mapper(", "send_load_options(")) {
    if ($detail.IndexOf($operation) -le $detailGuard.Index) {
        throw "Large-ROM selection still runs $operation before asking permission"
    }
}
if ($firmware -match "fh_prepare_download_region|fh_psram_download_body_callback|fno\.fsize > SD_ROM_MAX_SIZE") {
    throw "Whole-ROM download buffering or large-ROM enumeration limits remain"
}
if ($firmware -notmatch "#define MAPPER_DETECT_MAX_ROM_SIZE PICO_FLASH_SIZE_BYTES" -or
    $firmware -notmatch "#define MAX_ROM_SIZE\s+MAPPER_DETECT_MAX_ROM_SIZE") {
    throw "Mapper detection must accept every ROM that can fit the physical flash"
}
if ((Get-CFunction $firmware "handle_menu_write_explorer") -notmatch
    "data != CMD_RENAME_ENTRY && data != CMD_PREPARE_FLASH_ROM") {
    throw "The flash preparation command must stay busy until it finishes"
}
if ($firmware -match "prepared_flash_record|prepared_flash_offset") {
    throw "Large-ROM copies must select the actual flash entry, not prepare an immediate SD launch"
}
if ((Get-CFunction $firmware "flash_job_finish") -notmatch "browse_source_mode = SOURCE_MODE_FLASH" -or
    $firmware -notmatch "resolve_flash_copy_match\(\);") {
    throw "Copy completion must rebuild and select the flash entry before reporting success"
}
$loadGame = Get-CFunction $menu "loadGame"
if ($loadGame -notmatch "(?s)refresh_menu_state\(0\);\s*show_rom_screen\(\(unsigned int\)currentIndex\);\s*return;" -or
    $loadGame.IndexOf("run_ctrl_cmd(CMD_LOAD_OPTIONS)") -ge $loadGame.IndexOf("CMD_PREPARE_FLASH_ROM")) {
    throw "Large-ROM copying must open flash options and return without resetting"
}

$firmwareNames = 'ROM_NAME_MAX|SD_PATH_MAX|FH_DOWNLOAD_DIR_MAX|FH_STATUS_TEXT_SIZE|FH_QUERY_SIZE|FH_TYPE_ROM|FH_TYPE_DSK|FH_RESULT_SAVING|FH_HTTP_CHUNK_SIZE|SD_ROM_MAX_SIZE|DSK_SIZE_360K|DSK_HEADER_BYTES'
$menuNames = 'FILES_PER_PAGE|AUDIO_TYPE_MASK|AUDIO_TYPE_WAV|MAX_FILE_NAME_LENGTH|ROM_RECORD_SIZE|MAX_ROM_RECORDS|MEMORY_START|DATA_\w+|TLV_\w+|SOURCE_SD_FLAG|FOLDER_FLAG|MP3_FLAG|MAPPER_ASCII16X_FR|MAPPER_PICOAMP|SD_ROM_MAX_SIZE_KB|MENU_KEY_F4_CONFIG|MENU_KEY_F5_HELP|CTRL_BASE_ADDR|CTRL_COUNT_[LH]|CTRL_PAGE|CTRL_CMD|CTRL_ACK|CTRL_MAPPER|CTRL_AUDIO|CTRL_MAGIC|CTRL_QUERY_BASE|CTRL_STATUS|CTRL_STATUS_SD_MISSING|CTRL_NET_STATUS|NET_STATUS_ONLINE|CTRL_FH_TYPE_ROM|CTRL_FH_TYPE_DSK|CTRL_CHIP_ID_SIZE|CTRL_FH_STATUS_TEXT_BASE|CTRL_SD_PARTITION_INFO_SIZE|CMD_LOAD_OPTIONS|CMD_PREPARE_QUICK_RUN|CMD_PREPARE_FLASH_ROM|CMD_HELP_PAGE|CMD_HELP_END|HELP_PAGE_(?:LIST|ROM|MP3|FILEHUNTER)|AUDIO_PROFILE_MSX_MUSIC|ROM_SELECT_REGISTER|MP3_CTRL_BASE|MP3_CTRL_CMD|MP3_CTRL_MODE|MP3_CMD_STOP|MP3_PLAY_MODE_\w+|FLASH_COPY_\w+'
$cmake = Get-Content (Join-Path $root "pico\explorer\CMakeLists.txt") -Raw
$flashSize = [regex]::Match($cmake, "(?m)^set\(EXPLORER_FLASH_SIZE_BYTES (\d+)\)")
if (!$flashSize.Success) { throw "Cannot find physical flash size" }
$types = "#define PICO_FLASH_SIZE_BYTES $($flashSize.Groups[1].Value)u`n#define MAPPER_DETECT_MAX_ROM_SIZE PICO_FLASH_SIZE_BYTES`n"
$types += (([regex]::Matches($firmware, "(?m)^#define (?:$firmwareNames)\b[^\r\n]*") | ForEach-Object { $_.Value }) -join "`n") + "`n"
$types += (([regex]::Matches($header, "(?m)^#define (?:$menuNames)\b[^\r\n]*") | ForEach-Object { $_.Value }) -join "`n") + "`n"
$types += (([regex]::Matches($menuState, "(?m)^#define MENU_SHORTCUT_\w+\b[^\r\n]*") | ForEach-Object { $_.Value }) -join "`n") + "`n"
$types += (([regex]::Matches($dsk, "(?m)^#define SUNRISE_DSK_(?:HEADER_SECTORS|SECTOR_SIZE)\b[^\r\n]*") | ForEach-Object { $_.Value }) -join "`n") + "`n"
$record = [regex]::Match($header, "typedef struct \{[^}]*\}\s*ROMRecord;")
if (!$record.Success) { throw "Cannot find the MSX ROM record type" }
$types += $record.Value + "`n"
$decoder = [regex]::Match($firmware, "typedef struct \{[^}]*\}\s*fh_http_decoder_t;")
if (!$decoder.Success) { throw "Cannot find HTTP decoder state" }
$types += $decoder.Value + "`n"
$state = [regex]::Match($firmware, "(?s)#define FH_DOWNLOAD_PATH_SIZE[^\r\n]*\s+typedef struct \{[^}]*\}\s*fh_sd_download_t;")
if (!$state.Success) { throw "Cannot find the streaming download state" }
$code = $state.Value + "`n"
foreach ($name in @("flash_mapper_error", "flash_progress_label")) {
    $text = [regex]::Match($menu, "(?m)^static const char $name\[\] = `"[^`"]*`";")
    if (!$text.Success) { throw "Cannot find shared flash status text: $name" }
    $code += $text.Value + "`n"
}
foreach ($name in @("fh_parse_header_u64", "fh_deliver_http_body", "dsk_size_supported", "fh_parse_download_size", "fh_download_percent",
                    "fh_sd_download_body_callback", "fh_filename_has_extension", "fh_build_download_path",
                    "fh_publish_download", "fh_download_selected", "set_sunrise_partition_text",
                    "read_mounted_partition_free_mb", "set_browse_partition_status_text")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}
$code += (Get-CFunction $menu "record_needs_flash") + "`n"
$code += (Get-CFunction $screenRom "quick_run_rom") + "`n"
$code += (Get-CFunction $menu "wait_flash_copy") + "`n" + (Get-CFunction $menu "loadGame") + "`n"
foreach ($name in @("menu_ui_status_text", "menu_ui_fh_type_label", "menu_ui_render_footer", "menu_ui_print_footer_line", "menu_ui_capture_sd_status",
                    "menu_ui_render_last_line", "menu_ui_render_frame", "menu_ui_render_menu_frame",
                    "menu_ui_render_detail_frame", "menu_ui_update_list_footer", "menu_ui_blink_last_line",
                    "menu_ui_read_line", "menu_ui_print_str_inverted_width", "menu_ui_size_text", "menu_ui_build_row")) {
    $function = Get-CFunction $ui $name
    if ($name -eq "menu_ui_render_last_line") {
        # SDCC uses 16-bit unsigned int for memory-mapped status words.
        $function = $function.Replace("unsigned int", "uint16_t")
    }
    $code += [regex]::Replace($function, '\bprintf\(', 'ui_printf(') + "`n"
}
foreach ($name in @("read_ulong", "isEndOfData", "read_u16_le", "buffer_has_magic",
                    "clear_record", "read_string_from_pool", "read_total_count", "parse_page_buffer",
                    "load_page_records", "readROMData")) {
    $function = Get-CFunction $menu $name
    # Route direct Z80 control-register reads through the host's mock memory.
    $function = [regex]::Replace($function, '\*\(\(unsigned char \*\)(CTRL_\w+)\)', 'Peek($1)')
    $code += $function + "`n"
}
if ((Get-CFunction $menu "refresh_menu_state") -notmatch 'menu_ui_capture_sd_status\(\);') {
    throw "A refreshed microSD list must capture its browsing free-space summary"
}
foreach ($name in @("build_record_size_text", "build_record_row")) {
    $code += (Get-CFunction $menu $name) + "`n"
}
foreach ($name in @("render_rom_footer_line", "render_mp3_footer_line")) {
    $code += (Get-CFunction $screenRom $name) + "`n"
}
$code += (Get-CFunction $fh "fh_render_detail_footer") + "`n"
foreach ($name in @("helpMenu", "redefine_function_keys")) {
    $code += [regex]::Replace((Get-CFunction $menu $name), '\bprintf\(', 'ui_printf(') + "`n"
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("explorer-large-rom-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $work | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $work "large-rom-types.h"), $types)
    [IO.File]::WriteAllText((Join-Path $work "large-rom-production.h"), $code)
    $exe = Join-Path $work "large-rom-test.exe"
    & $CC -std=c11 -Wall -Wextra -Werror -I $work -I (Join-Path $root "pico\explorer") (Join-Path $PSScriptRoot "large_rom.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Large ROM host test compilation failed" }
    & $exe $work
    if ($LASTEXITCODE -ne 0) { throw "Large ROM host tests failed" }
    if (!$HostOnly) {
        & $Tcl (Join-Path $PSScriptRoot "large_rom_openmsx.tcl") (Join-Path $root "openmsx\share\scripts\picoverse2350.tcl") $work
        if ($LASTEXITCODE -ne 0) { throw "Large ROM openMSX protocol tests failed" }
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
