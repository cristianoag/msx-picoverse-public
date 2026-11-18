param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot
$firmware = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw

function Get-CFunction([string]$source, [string]$name) {
    $match = [regex]::Match($source, "(?m)^(?:static[^\r\n]*|void[^\r\n]*)\b$name\b\)?\s*\([^;{]*\)\s*\{")
    if (!$match.Success) { throw "Cannot find function $name" }
    $start = $match.Index
    $end = $start + $match.Length
    $depth = 1
    while ($depth -gt 0 -and $end -lt $source.Length) {
        if ($source[$end] -eq '{') { $depth++ }
        if ($source[$end] -eq '}') { $depth-- }
        $end++
    }
    if ($depth -ne 0) { throw "Unclosed function $name" }
    return $source.Substring($start, $end - $start)
}

function Get-CType([string]$name) {
    $match = [regex]::Match($firmware, "typedef (?:struct|enum) \{[^}]*\}\s*$name;")
    if (!$match.Success) { throw "Cannot find type $name" }
    return $match.Value
}

# Compile the production FlashROM device, not a reimplementation of it.
$code = "#include <stdbool.h>`n#include <stdint.h>`n#include <string.h>`n"
$code += ([regex]::Matches($firmware, '(?m)^#define (?:FLASH_(?:BOOT|MAIN|ERASE|CFI|CMD|MAX|SD)_\w+|BANK_\w+|ASCII16X_\w+)[^\r\n]*') |
    ForEach-Object { $_.Value }) -join "`n"
$code += "`n"
foreach ($type in @("bank_cache_t", "flash_cmd_state_t", "flash_store_state_t", "ascii16x_flash_state_t")) {
    $code += (Get-CType $type) + "`n"
}
foreach ($name in @("bcache_init", "bcache_touch", "bcache_find", "bcache_norm_bank", "bcache_evict", "bcache_ensure",
    "bcache_prefill", "ascii16x_decode_bank_write", "flash_update_gates", "flash_bus_offset", "flash_mark_dirty",
    "flash_sector_bounds", "flash_program_byte", "flash_service_erase", "flash_start_erase",
    "flash_autoselect_byte", "flash_cfi_byte", "flash_build_cfi", "flash_process_write",
    "handle_ascii16x_flash_write", "fr_read_byte")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}

# Wiring a host test cannot see: the presented device is what plain ASCII16-X
# runs (writable = false), and only the writable one syncs the card image.
if ((Get-CFunction $firmware "ascii16x_flash_setup") -notmatch 'if \(writable && array_size >= active_rom_size\)') {
    throw "ascii16x_flash_setup must build a writable array only when asked and when the ROM fits"
}
if ((Get-CFunction $firmware "ascii16x_flash_setup") -notmatch 'if \(writable && st->array == NULL\)\s*return false;') {
    throw "ASC16X-FR must never degrade into a device that drops saves"
}
if ($firmware -notmatch 'case 12:\s*loadrom_ascii16x_flash\(rom_offset, cache_enable, selected->Name, false\);') {
    throw "plain ASCII16-X must run the presented FlashROM device"
}
if ($firmware -notmatch 'loadrom_ascii16x_flash\(rom_offset, cache_enable, selected->Name, true\);') {
    throw "ASC16X-FR must run the writable FlashROM device"
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ("explorer-ascii16x-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $temp | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $temp "ascii16x-production.h"), $code)
    $exe = Join-Path $temp "ascii16x-flash.exe"
    & $CC -std=c11 -Wall -Wextra -Werror -I $temp (Join-Path $PSScriptRoot "ascii16x_flash.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "ASCII16-X flash host test compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "ASCII16-X flash host test failed" }
} finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}
