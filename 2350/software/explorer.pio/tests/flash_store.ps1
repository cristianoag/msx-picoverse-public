param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"

# Runs the production flash ROM store (F = copy a microSD ROM to flash,
# D = delete a flash entry) against a simulated 16 MB NOR flash that is loaded
# from a real UF2 built by the packaged Explorer tool.
$root = Split-Path $PSScriptRoot
$firmware = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw
$cmake = Get-Content (Join-Path $root "pico\explorer\CMakeLists.txt") -Raw

function Get-CFunction([string]$source, [string]$name) {
    $match = [regex]::Match($source, "(?m)^(?:static[^\r\n]*|void[^\r\n]*)\b$name\b\)?\s*\([^;{]*\)\s*\{")
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

$flashSize = [regex]::Match($cmake, '(?m)^set\(EXPLORER_FLASH_SIZE_BYTES (\d+)\)')
if (!$flashSize.Success) { throw "Cannot find EXPLORER_FLASH_SIZE_BYTES in CMakeLists.txt" }
if ($cmake -notmatch 'PICO_FLASH_SIZE_BYTES=\$\{EXPLORER_FLASH_SIZE_BYTES\}') {
    throw "The firmware no longer passes the board flash size to the SDK flash routines"
}

# Layout constants and the record type come straight from the firmware.
$names = 'ROM_NAME_MAX|MAX_FLASH_RECORDS|ROM_RECORD_SIZE|MENU_ROM_SIZE|CONFIG_AREA_SIZE|WIFI_(?:CONFIG|BIOS)_(?:FLASH_OFFSET|ROM_SIZE)|(?:FMPAC|SFG)_BIOS_(?:FLASH_OFFSET|ROM_SIZE)|NEXTOR_DSK_(?:FLASH_OFFSET|ROM_SIZE)|SD_PATH_MAX|MIN_ROM_SIZE|RENAME_NAME_MAX'
$types = (([regex]::Matches($firmware, "(?m)^#define (?:$names)\b[^\r\n]*") | ForEach-Object { $_.Value }) -join "`n") + "`n"
$record = [regex]::Match($firmware, "typedef struct \{[^}]*\}\s*ROMRecord;")
if (!$record.Success) { throw "Cannot find ROMRecord" }
$types += $record.Value + "`n"

# Flash store state: everything from its defines up to the first helper.
$block = [regex]::Match($firmware, "(?s)#define FLASH_STORE_SIZE.*?(?=static inline uint32_t flash_store_align_up)")
if (!$block.Success) { throw "Cannot find the flash store definitions" }
$code = $block.Value
foreach ($name in @("write_u32_le", "trim_name_copy", "flash_store_align_up", "flash_store_base",
    "flash_store_config_base", "flash_store_serialize", "flash_store_load_records", "flash_names_equal",
    "flash_store_find_entry",
    "flash_store_patch", "flash_store_compact", "flash_store_find_space", "flash_store_tombstone",
    "flash_store_append", "flash_store_delete_entry", "flash_job_write_step", "flash_job_commit",
    "flash_job_finish", "flash_store_rename_entry")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}

# Structural guards for the parts that cannot run on the host.
$op = [regex]::Match($firmware, "(?s)static void __no_inline_not_in_flash_func\(flash_store_op\)\(.*?\n\}")
if (!$op.Success) { throw "flash_store_op must stay a RAM-resident (__no_inline_not_in_flash_func) function" }
foreach ($pattern in @("save_and_disable_interrupts\(\)", "qmi_hw->m\[0\]\.timing = m0_timing", "restore_interrupts\(irq_state\)")) {
    if ($op.Value -notmatch $pattern) { throw "flash_store_op lost: $pattern" }
}
if ((Get-CFunction $firmware "flash_job_start") -notmatch "quiesce_mp3_core1_before_sd_work\(\)") {
    throw "flash_job_start must stop Core 1 before programming the flash"
}
if ((Get-CFunction $firmware "core1_bg_work") -notmatch "(?s)^[^\n]*\n\s*if \(flash_job_state != FLASH_JOB_IDLE\)") {
    throw "core1_bg_work must give the flash copy job priority over other background work"
}
# Without a time slice after each step the MSX gets one bus read per step and
# cannot draw the progress until the copy ends.
if ((Get-CFunction $firmware "flash_job_background_work") -notmatch "(?s)time_us_32\(\) - flash_job_resume_us\) < 0\).*flash_job_resume_us = time_us_32\(\) \+ FLASH_JOB_YIELD_US;") {
    throw "flash_job_background_work must yield the bus to the MSX between steps"
}
$menuLoader = [regex]::Match($firmware, "(?s)int __no_inline_not_in_flash_func\(loadrom_msx_menu\)\(uint32_t offset\)\s*\{.*?\r?\n\}\r?\n")
if (!$menuLoader.Success -or $menuLoader.Value -notmatch "flash_store_load_records\(\)") {
    throw "The menu no longer reads the flash entries through flash_store_load_records()"
}
# The config area must own its sectors: the tool pads the firmware to 4 KB and
# main() rounds __flash_binary_end up the same way.
$tool = Get-Content (Join-Path $root "tool\src\explorer.c") -Raw
if ($tool -notmatch "#define FIRMWARE_ALIGN\s+4096u" -or $tool -notmatch "offset \+= firmware_span;") {
    throw "The tool no longer starts the menu ROM on a 4 KB boundary"
}
$main = [regex]::Match($firmware, "(?s)int __no_inline_not_in_flash_func\(main\)\(\)\s*\{(.{0,400})")
if (!$main.Success -or $main.Groups[1].Value -notmatch "flash_rom = \(const uint8_t \*\)\(\(\(uintptr_t\)__flash_binary_end \+ FLASH_PAYLOAD_ALIGN - 1u\)" -or
    $firmware -notmatch "#define FLASH_PAYLOAD_ALIGN 4096u") {
    throw "main() no longer aligns flash_rom like the tool"
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("explorer-flash-store-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $work | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $work "flash-store-types.h"), $types)
    [IO.File]::WriteAllText((Join-Path $work "flash-store-production.h"), $code)
    $exe = Join-Path $work "flash-store-test.exe"
    & $CC -std=c11 -Wall -Wextra -Werror "-DPICO_FLASH_SIZE_BYTES=$($flashSize.Groups[1].Value)u" -I $work (Join-Path $PSScriptRoot "flash_store.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Flash store host test compilation failed" }

    $creator = Join-Path $root "tool\dist\explorer.exe"
    if (!(Test-Path $creator)) { throw "Build and package the Explorer utility before running this test" }
    $embedded = Get-Content (Join-Path $root "tool\src\explorer.h") -Raw
    $length = [regex]::Match($embedded, 'unsigned int ___pico_explorer_build_explorer_bin_len = (\d+);')
    if (!$length.Success) { throw "Cannot determine the packaged firmware size" }

    $roms = Join-Path $work "roms"
    New-Item -ItemType Directory $roms | Out-Null
    & $exe make-roms $roms
    if ($LASTEXITCODE -ne 0) { throw "ROM fixture generation failed" }
    $uf2 = Join-Path $work "flash-store.uf2"
    Push-Location $roms
    try {
        & $creator -s1 -r --output $uf2 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Explorer image generation failed" }
    } finally {
        Pop-Location
    }
    & $exe check $uf2 $length.Groups[1].Value
    if ($LASTEXITCODE -ne 0) { throw "Flash store host tests failed" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
