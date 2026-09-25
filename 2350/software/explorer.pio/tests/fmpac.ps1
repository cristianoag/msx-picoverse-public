param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot
$firmware = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw
$menu = Get-Content (Join-Path $root "msx\src\screen_rom.c") -Raw

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

# Compile the production functions, not reimplementations of their decisions.
$code = "#define __not_in_flash_func(name) name`n"
$code += "#define __no_inline_not_in_flash_func(name) name`n"
$code += ([regex]::Matches($firmware, '(?m)^#define (?:AUDIO_PROFILE_|MAPPER_)\w+[^\r\n]*') |
    ForEach-Object { $_.Value }) -join "`n"
$layoutNames = 'ROM_NAME_MAX|ROM_RECORD_SIZE|MENU_ROM_SIZE|CONFIG_AREA_SIZE|WIFI_(?:CONFIG|BIOS)_(?:FLASH_OFFSET|ROM_SIZE)|(?:FMPAC|SFG)_BIOS_(?:FLASH_OFFSET|ROM_SIZE)|NEXTOR_DSK_(?:FLASH_OFFSET|ROM_SIZE)'
$code += "`n" + (([regex]::Matches($firmware, "(?m)^#define (?:$layoutNames)\b[^\r\n]*") |
    ForEach-Object { $_.Value }) -join "`n") + "`n"
$header = Get-Content (Join-Path $root "pico\explorer\explorer.h") -Raw
$code += "`n" + [regex]::Match($header, '(?m)^#define MAPPER_PAGES\b[^\r\n]*').Value + "`n"
$code += (Get-CType "audio_mode_t") + "`n"
$code += (Get-CType "fmpac_state_t") + "`nstatic fmpac_state_t system_fmpac;`n"
$code += (Get-CType "sunrise_fmpac_bus_t") + "`n"
$code += (Get-CType "bank8_ctx_t") + "`n"
$code += (Get-CType "bank16_ctx_t") + "`n"
foreach ($name in @("is_system_mapper", "is_megaram_mapper", "is_audio_system_mapper",
    "mapper_supports_scc_audio", "resolve_audio_mode", "mapper_page_from_reg",
    "fmpac_sram_enabled", "fmpac_handle_write", "fmpac_handle_read",
    "sunrise_fmpac_drain_writes", "c2_handle_memory_write")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}
foreach ($name in @("handle_konamiscc_write", "handle_konami_write", "handle_ascii8_write",
    "handle_ascii16_write", "handle_neo8_write", "handle_neo16_write", "handle_ascii16x_write_simple",
    "loadrom_fmpac")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}
foreach ($name in @("record_is_system_rom", "record_is_sunrise_system_rom",
    "record_supports_scc_audio", "record_supports_external_scc_audio",
    "record_supports_dual_psg", "record_supports_msx_music", "audio_profile_is_supported")) {
    $code += (Get-CFunction $menu $name) + "`n"
}

# These are structural guards, not a simulation of PIO or physical MSX timing.
foreach ($name in @("loadrom_sunrise_fmpac_common", "loadrom_c2_common")) {
    $body = Get-CFunction $firmware $name
    if ($body.IndexOf("system_audio_init_for_sunrise(false)") -gt $body.IndexOf("multicore_launch_core1")) {
        throw "$name launches the consumer before audio initialization"
    }
    $drain = if ($name -eq "loadrom_c2_common") { "c2_handle_memory_write" } else { "sunrise_fmpac_drain_writes" }
    if ([regex]::Matches($body, "$drain\(").Count -ne 2) { throw "$name must drain writes twice" }
    $read = $body.LastIndexOf("pio_sm_get(msx_bus.pio, msx_bus.sm_read)")
    if ($body.IndexOf("$drain(", $read) -lt $read) { throw "$name lacks a post-read write drain" }
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ("explorer-fmpac-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $temp | Out-Null
$generated = Join-Path $temp "fmpac-production.h"
$exe = Join-Path $temp "fmpac-test.exe"
$uf2 = Join-Path $temp "fmpac-test.uf2"
try {
    [IO.File]::WriteAllText($generated, $code)
    & $CC -std=c11 -Wall -Wextra -Werror -I $temp (Join-Path $PSScriptRoot "fmpac.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "FMPAC host test compilation failed" }
    $creator = Join-Path $root "tool\dist\explorer.exe"
    if (!(Test-Path $creator)) { throw "Build and package the Explorer utility before running this test" }
    $embedded = Get-Content (Join-Path $root "tool\src\explorer.h") -Raw
    $length = [regex]::Match($embedded, 'unsigned int ___pico_explorer_build_explorer_bin_len = (\d+);')
    if (!$length.Success) { throw "Cannot determine the packaged firmware size" }
    Push-Location $temp
    try {
        & $creator --allnextor --output $uf2
        if ($LASTEXITCODE -ne 0) { throw "Nextor image generation failed" }
    } finally {
        Pop-Location
    }
    & $exe (Join-Path $root "..\loadrom.pio\fmpac\FMPCCMFC.BIN") $uf2 $length.Groups[1].Value
    if ($LASTEXITCODE -ne 0) { throw "FMPAC host tests failed" }
} finally {
    foreach ($file in @($generated, $exe, $uf2)) {
        if (Test-Path $file) { Remove-Item -LiteralPath $file }
    }
    Remove-Item -LiteralPath $temp
}
