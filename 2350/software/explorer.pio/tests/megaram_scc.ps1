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

# Compile the production MegaRAM/SCC decode against the real SCC emulator.
$code = "#define __not_in_flash_func(name) name`n"
$code += "#define __no_inline_not_in_flash_func(name) name`n"
$header = Get-Content (Join-Path $root "pico\explorer\explorer.h") -Raw
$code += [regex]::Match($header, '(?m)^#define MAPPER_PAGES\b[^\r\n]*').Value + "`n"
$code += [regex]::Match($firmware, 'typedef struct \{[^}]*\}\s*sunrise_megaram_bus_t;').Value + "`n"
foreach ($name in @("mapper_page_from_reg", "megaram_page_from_addr", "megaram_bank_switch_write",
    "megaram_write_byte", "megaram_read_byte", "megaram_scc_write", "megaram_scc_read",
    "megaram_handle_write", "megaram_handle_read", "megaram_drain_writes",
    "sunrise_megaram_drain_writes")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}

# Structural guards; these do not simulate PIO or physical bus timing.
$body = Get-CFunction $firmware "loadrom_sunrise_megaram_common"
$launch = $body.IndexOf("multicore_launch_core1")
if ($body.IndexOf("system_audio_init_for_sunrise(false)") -gt $launch -or
    $body.IndexOf("psram_prepare_megaram_region") -gt $launch) {
    throw "loadrom_sunrise_megaram_common launches Core 1 before PSRAM/audio setup"
}
foreach ($pair in @(@("loadrom_sunrise_megaram_common", "sunrise_megaram_drain_writes"),
                    @("loadrom_megaram", "megaram_drain_writes"))) {
    $body = Get-CFunction $firmware $pair[0]
    $drain = $pair[1]
    if ([regex]::Matches($body, "(?<!\w)$drain\(").Count -ne 2) { throw "$($pair[0]) must drain writes twice" }
    $read = $body.LastIndexOf("pio_sm_get(msx_bus.pio, msx_bus.sm_read)")
    if ([regex]::Match($body.Substring($read), "(?<!\w)$drain\(").Success -ne $true) {
        throw "$($pair[0]) lacks a post-read write drain"
    }
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ("explorer-megaram-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $temp | Out-Null
$generated = Join-Path $temp "megaram-production.h"
$exe = Join-Path $temp "megaram-test.exe"
try {
    [IO.File]::WriteAllText($generated, $code)
    $audio = Join-Path $root "pico\explorer\audio"
    & $CC -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I $temp -I $audio `
        (Join-Path $PSScriptRoot "megaram_scc.c") (Join-Path $audio "emu2212.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "MegaRAM SCC host test compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "MegaRAM SCC host tests failed" }
} finally {
    foreach ($file in @($generated, $exe)) {
        if (Test-Path $file) { Remove-Item -LiteralPath $file }
    }
    Remove-Item -LiteralPath $temp
}
