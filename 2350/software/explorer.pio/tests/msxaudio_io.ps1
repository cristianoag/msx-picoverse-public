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

# Compile the production MSX-AUDIO I/O service, not a reimplementation of it.
$code = ""
foreach ($name in @("msxaudio_handle_io_write", "msxaudio_handle_io_read", "msxaudio_service_io",
    "msxaudio_bios_read", "msxaudio_bios_write", "bios_slot_write", "bios_slot_read", "fm_game_write", "fm_game_read")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}

# Structural guards for the wiring that a host test cannot see.
$service = Get-CFunction $firmware "msxaudio_service_io"
if ($service -match 'pio_try_get_io_write\(&read_addr' -or $service -notmatch 'pio_try_get_io_read\(&read_addr\)') {
    throw "msxaudio_service_io must keep the read address apart from the write drain's address"
}
# SYSTEM entries start it through the shared system audio hooks.
foreach ($hook in @("msxaudio_start\(service_io_on_core1\)", "return msxaudio_handle_io_write\(port, data\)",
    "return msxaudio_handle_io_read\(port, data\)", "msxaudio_audio_service_buffer\(\);")) {
    if ($firmware -notmatch $hook) { throw "system audio hook missing for MSX-AUDIO: $hook" }
}
# The BIOS needs an expanded slot wherever the profile runs: the game-ROM loop
# restarts through the expanded-slot bootstrap, the Sunrise (Nextor/DSK) and C2
# loops host the BIOS in their free subslot through the shared BIOS-subslot calls.
if ((Get-CFunction $firmware "loadrom_external_fm") -notmatch 'if \(msxaudio_bios_slot\)\s*fmpac_wait_for_expanded_bootstrap\(\);') {
    throw "loadrom_external_fm must run the expanded-slot bootstrap before serving the MSX-AUDIO BIOS"
}
foreach ($name in @("loadrom_sunrise_sfg_common", "loadrom_c2_common")) {
    $body = Get-CFunction $firmware $name
    if ($body -match '\bsfg_(?:read|write)\(' -or $body -notmatch 'bios_slot_read\(') {
        throw "$name must reach the BIOS subslot through bios_slot_read/bios_slot_write"
    }
}
if ($firmware -notmatch 'bool sfg_slot = sfg_audio \|\| msxaudio_audio;\s*msxaudio_bios_slot = msxaudio_audio;') {
    throw "main() must route MSX-AUDIO to the BIOS-subslot loaders and set msxaudio_bios_slot per launch"
}
# The chip is only ready once the wait-reads responder exists.
$start = Get-CFunction $firmware "msxaudio_start"
if ($start.IndexOf("msxaudio_prepare()") -gt $start.IndexOf("msx_pio_io_bus_init()") -or
    $start.IndexOf("msx_pio_io_bus_init()") -gt $start.IndexOf("msxaudio_ready = true")) {
    throw "msxaudio_start must prepare the chip, then install the I/O responder, then mark the profile ready"
}
# The I2S stream follows the chip's native rate, never the 44.1 kHz default.
if ($firmware -notmatch '#define MSXAUDIO_SAMPLE_RATE\s+49716u' -or
    ([regex]::Matches($firmware, 'sample_freq = MSXAUDIO_SAMPLE_RATE').Count -ne 2)) {
    throw "MSX-AUDIO must retune the handed-over pool and create its own pool at 49716 Hz"
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ("explorer-msxaudio-io-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $temp | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $temp "msxaudio-production.h"), $code)
    $exe = Join-Path $temp "msxaudio-io.exe"
    & $CC -std=c11 -Wall -Wextra -Werror -I $temp (Join-Path $PSScriptRoot "msxaudio_io.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "MSX-AUDIO I/O host test compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "MSX-AUDIO I/O host test failed" }
} finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}
