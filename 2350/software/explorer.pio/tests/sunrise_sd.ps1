param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$storage = Join-Path $root "pico\explorer\storage"
$firmware = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw
$menu = Get-Content (Join-Path $root "msx\src\menu.h") -Raw
$screen = Get-Content (Join-Path $root "msx\src\screen_rom.c") -Raw
$sdHeader = Get-Content (Join-Path $storage "sunrise_sd.h") -Raw

function Get-CFunction([string]$source, [string]$name) {
    $match = [regex]::Match($source, "(?m)^(?:static|void)[^\r\n]*\b$name\b\)?\s*\([^;{]*\)\s*\{")
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

$sdValue = [regex]::Match($sdHeader, "(?m)^#define SD_PART_WHOLE_DISK\s+(\d+)u?")
$menuValue = [regex]::Match($menu, "(?m)^#define SD_PART_WHOLE_DISK\s+(\d+)u?")
if (!$sdValue.Success -or !$menuValue.Success -or $sdValue.Groups[1].Value -ne $menuValue.Groups[1].Value) {
    throw "Firmware and menu disagree on Whole Disk selection"
}
$names = 'PVC_OPTIONS_\w+|MAPPER_\w+|SOURCE_SD_FLAG|FOLDER_FLAG|MP3_FLAG|AUDIO_VOLUME_\w+|VDP_FREQ_\w+|CPU_MODE_\w+|AUDIO_PROFILE_NONE|SD_BROWSE_PARTITION_MAX|SD_PATH_MAX|CTRL_SD_PARTITION_INFO_SIZE|SOURCE_MODE_SD|BOOT_STUB_\w+|BOOT_PATCH_TRIGGER_OFF'
$types = (([regex]::Matches($firmware, "(?m)^#define (?:$names)\b[^\r\n]*") |
           Where-Object { $_.Value -notmatch 'MAPPER_DETECT_MAX_ROM_SIZE' } |
           ForEach-Object { $_.Value }) -join "`n") + "`n"
$descriptions = [regex]::Match($firmware, "(?s)static const char \*MAPPER_DESCRIPTIONS\[\] = \{.*?\};")
if (!$descriptions.Success) { throw "Cannot find mapper descriptions" }
$types += $descriptions.Value + "`n"
$bootState = [regex]::Match($firmware, "(?s)static volatile uint32_t boot_patch_trigger_rel = .*?static uint32_t boot_patch_offset = [^;]*;")
if (!$bootState.Success) { throw "Cannot find boot patch state" }
$code = $bootState.Value + "`n"
foreach ($name in @("build_boot_stub", "boot_patch_disarm", "boot_patch_restore", "apply_boot_patches", "read_rom_byte")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}
foreach ($name in @("is_system_mapper", "is_sunrise_sd_mapper", "mapper_code_from_record_byte", "is_dsk_record",
                    "set_sunrise_partition_text", "update_sunrise_partition_text", "refresh_sunrise_partition_info",
                    "process_load_options_request", "process_save_options_request",
                    "sd_partition_supported", "sd_find_partition_index", "sd_unmount_card", "sd_mount_partition",
                    "process_cycle_sd_partition_request")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}
$code += (Get-CFunction $screen "next_sd_partition") + "`n"
$write = [regex]::Match($firmware, "(?s)if \(addr == CTRL_SD_PARTITION\) \{([^}]*)\}")
if (!$write.Success) { throw "Cannot find the SD partition control handler" }
$code += "static void write_partition_control(uint8_t data) { $($write.Groups[1].Value) }`n"
if ((Get-CFunction $screen "show_rom_screen") -notmatch 'sd_partition = next_sd_partition\(') {
    throw "The SD Part key handler must use the tested partition cycle"
}
if ((Get-CFunction $screen "show_rom_screen") -notmatch 'allow_freq = \(!record_is_system_rom\(record\) \|\| record_mapper_code\(record->Mapper\) == 23\) && \(Peek\(0x002D\) != 0\)' -or
    $firmware -notmatch 'vdp_freq_launch = \(!system_mapper \|\| is_dsk\) \? ctrl_vdp_frequency : VDP_FREQ_DEFAULT;' -or
    $firmware -notmatch 'cpu_mode_launch = ctrl_cpu_mode;') {
    throw "DSKs must offer frequency on MSX2+, preserve it at launch and apply the CPU speed like every other entry"
}
$cache = Get-CFunction $firmware "prepare_sunrise_mapper_rom_source"
if ($cache -notmatch 'boot_patch_disarm\(\);' -or
    $cache -notmatch 'rom_cached_size = bytes_to_cache;[\s\S]*apply_boot_patches\(rom_sram, bytes_to_cache\);') {
    throw "Every DSK Nextor loader must apply the self-restoring boot patch to its cache"
}

$work = Join-Path $PSScriptRoot (".sunrise-sd-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory (Join-Path $work "pico") | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $work "pico\stdlib.h"), "#pragma once`n")
    [IO.File]::WriteAllText((Join-Path $work "pico\sync.h"), "#pragma once`n#define __dmb() ((void)0)`n")
    [IO.File]::WriteAllText((Join-Path $work "hw_config.h"), "#pragma once`n")
    [IO.File]::WriteAllText((Join-Path $work "diskio.h"), @"
#pragma once
#include <stdint.h>
typedef uint8_t BYTE;
typedef unsigned int UINT;
typedef uint32_t LBA_t;
typedef BYTE DSTATUS;
typedef enum { RES_OK = 0, RES_ERROR, RES_WRPRT, RES_NOTRDY, RES_PARERR } DRESULT;
DSTATUS disk_initialize(BYTE pdrv);
DRESULT disk_read_raw(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);
DRESULT disk_write_raw(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);
void disk_set_partition_window(LBA_t base, LBA_t count);
"@)
    [IO.File]::WriteAllText((Join-Path $work "sd_card.h"), @"
#pragma once
#include <stdint.h>
typedef struct { unsigned unused; } FATFS;
typedef struct sd_card_t {
    struct { uint8_t CID[16]; FATFS fatfs; } state;
    uint32_t (*get_num_sectors)(struct sd_card_t *);
} sd_card_t;
sd_card_t *sd_get_by_num(unsigned num);
"@)
    [IO.File]::WriteAllText((Join-Path $work "util.h"), @"
#pragma once
#include <stddef.h>
#include <stdint.h>
void ext_str(size_t len, const uint8_t *cid, unsigned high, unsigned low, size_t size, char *out);
uint32_t ext_bits16(const uint8_t *cid, unsigned high, unsigned low);
"@)
    [IO.File]::WriteAllText((Join-Path $work "sunrise-sd-types.h"), $types)
    [IO.File]::WriteAllText((Join-Path $work "sunrise-sd-options.h"), $code)
    $exe = Join-Path $work "sunrise-sd.exe"
    & $CC -std=c11 -O2 -Wall -Wextra -Werror -I $work -I $storage (Join-Path $PSScriptRoot "sunrise_sd_test.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Sunrise SD host compilation failed" }
    & $exe (Join-Path $root "resources\Nextor-2.1.4.SunriseIDE.MasterOnly.ROM")
    if ($LASTEXITCODE -ne 0) { throw "Sunrise SD host tests failed" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
