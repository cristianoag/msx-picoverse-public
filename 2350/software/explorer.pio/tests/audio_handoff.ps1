param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$source = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw
$start = $source.IndexOf("static void prepare_rom_audio_handoff_pool(void)")
$end = $source.IndexOf("static struct audio_buffer_pool *claim_rom_audio_handoff_pool(void)", $start)
if ($start -lt 0 -or $end -le $start) { throw "Cannot locate production handoff function" }
$samples = [regex]::Match($source, '(?m)^#define SCC_AUDIO_BUFFER_SAMPLES\s+(\d+)')
if (!$samples.Success) { throw "Missing game buffer size" }
$mp3 = Get-Content (Join-Path $root "pico\explorer\audio\mp3.c") -Raw
$count = [regex]::Match($mp3, '(?m)^#define MP3_I2S_BUFFER_COUNT\s+(\d+)')
if (!$count.Success) { throw "Missing MP3 producer buffer count" }
if ($source -notmatch 'if \(!wavegame_active && \(cartridge_audio \|\| psg_emulation\)\)\s+prepare_rom_audio_handoff_pool\(\);') {
    throw "Handoff must preserve WAVEGAME and cover all game audio profiles"
}
$call = $source.LastIndexOf("prepare_rom_audio_handoff_pool();")
if ($call -gt $source.LastIndexOf("start_msx_music_audio();")) {
    throw "Pool must be compacted before OPLL allocation"
}
foreach ($profile in @("msx_music", "scc", "ym2151", "dual_psg", "main_psg")) {
    if ($source -notmatch "$profile`_audio_pool = handoff_pool;") {
        throw "$profile does not adopt the existing pool"
    }
}
$temp = Join-Path $PSScriptRoot (".audio-handoff-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $temp | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $temp "handoff-production.h"),
        "#define MP3_I2S_BUFFER_COUNT $($count.Groups[1].Value)`n#define SCC_AUDIO_BUFFER_SAMPLES $($samples.Groups[1].Value)`n" +
        $source.Substring($start, $end - $start))
    $obj = Join-Path $temp "emu2413.o"
    & $CC -std=c11 -O0 -include (Join-Path $PSScriptRoot "handoff_allocator.h") `
        -Dmalloc=handoff_malloc -Dcalloc=handoff_calloc -Dfree=handoff_free `
        -c (Join-Path $root "pico\explorer\audio\emu2413.c") -o $obj
    if ($LASTEXITCODE -ne 0) { throw "OPLL test compilation failed" }
    $exe = Join-Path $temp "handoff.exe"
    & $CC -std=c11 -Wall -Wextra -Werror -I $temp -I (Join-Path $root "pico\explorer\audio") `
        (Join-Path $PSScriptRoot "audio_handoff.c") $obj -lm -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Handoff test compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Handoff regression failed" }
} finally {
    foreach ($name in @("handoff-production.h", "emu2413.o", "handoff.exe")) {
        $file = Join-Path $temp $name
        if (Test-Path $file) { Remove-Item -LiteralPath $file }
    }
    Remove-Item -LiteralPath $temp
}
