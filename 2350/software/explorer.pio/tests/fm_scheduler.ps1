param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot
$firmware = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw

function Get-CFunction([string]$source, [string]$name) {
    $match = [regex]::Match($source, "(?m)^(?:static[^\r\n]*|void )\b$name\b\)?\s*\([^;{]*\)\s*\{")
    if (!$match.Success) {
        $match = [regex]::Match($source, "(?m)^void __not_in_flash_func\($name\)\(void\)\s*\{")
    }
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

$definitions = @(
    "MSX_MUSIC_SHARED_SLICE_SAMPLES", "MSX_MUSIC_SHARED_IO_BUDGET",
    "MSX_MUSIC_WRITE_RING_SIZE", "MSX_MUSIC_WRITE_RING_MASK",
    "MAIN_PSG_WRITE_RING_SIZE", "MAIN_PSG_WRITE_RING_MASK",
    "SCC_AUDIO_BUFFER_SAMPLES", "MSX_MUSIC_DC_R", "MSX_MUSIC_LP_A",
    "MSX_MUSIC_GAIN_NUM", "MSX_MUSIC_GAIN_SHIFT",
    "MSX_MUSIC_CLOCK", "MSX_MUSIC_SAMPLE_RATE"
)
$code = ""
foreach ($name in $definitions) {
    $match = [regex]::Match($firmware, "(?m)^#define $name\b[^\r\n]*")
    if (!$match.Success) { throw "Cannot find definition $name" }
    $code += $match.Value + "`n"
}
$constants = $code
$code = ""
foreach ($name in @("msx_music_filter_sample", "msx_music_calc_sample",
    "msx_music_write_ring_push", "msx_music_drain_write_ring",
    "msx_music_drain_psg_write_ring", "msx_music_write_io",
    "msx_music_audio_service_buffer")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}

# Exercise the actual steady-state loops; device initialization is not simulated.
foreach ($backend in @("sd", "usb")) {
    $file = if ($backend -eq "sd") { "sunrise_sd.c" } else { "sunrise_ide.c" }
    $source = Get-Content (Join-Path $root "pico\explorer\storage\$file") -Raw
    $body = Get-CFunction $source "sunrise_${backend}_task"
    $loop = $body.IndexOf("while (true)")
    if ($loop -lt 0) { throw "Cannot find $backend service loop" }
    $code += "static void run_${backend}_loop(void) {`n" + $body.Substring($loop) + "`n"
}

$work = Join-Path $PSScriptRoot (".fm-scheduler-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $work | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $work "fm-constants.h"), $constants)
    [IO.File]::WriteAllText((Join-Path $work "fm-production.h"), $code)
    $object = Join-Path $work "emu2413.o"
    $exe = Join-Path $work "fm-scheduler.exe"
    # The vendor emulator has intentionally unused compatibility parameters.
    & $CC -std=c11 -O0 -c (Join-Path $root "pico\explorer\audio\emu2413.c") -o $object
    if ($LASTEXITCODE -ne 0) { throw "emu2413 host compilation failed" }
    & $CC -std=c11 -Wall -Wextra -Werror -I $work `
        -I (Join-Path $root "pico\explorer\audio") `
        -I (Join-Path $root "pico\explorer\storage") `
        (Join-Path $PSScriptRoot "fm_scheduler.c") $object -lm -o $exe
    if ($LASTEXITCODE -ne 0) { throw "FM scheduler host compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "FM scheduler host tests failed" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
