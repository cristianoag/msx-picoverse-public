param([string]$CXX = "g++")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$audio = Join-Path $root "pico\explorer\audio"
$work = Join-Path $PSScriptRoot (".msxaudio-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $work | Out-Null
try {
    # ymfm_opl.cpp is built at half rate for the OPL4 (OPL4_OUTPUT_STEP=2): the
    # test compiles with the same definitions and checks that the Y8950 in it
    # still runs at its native rate.
    $exe = Join-Path $work "msxaudio.exe"
    & $CXX -std=c++17 -O2 -DOPL4_OUTPUT_STEP=2 -DOPL4_SKIP_INACTIVE_FM=1 -DOPL4_SKIP_INACTIVE_PCM=1 `
        -I $audio (Join-Path $PSScriptRoot "msxaudio.cpp") (Join-Path $audio "emuy8950.cpp") `
        (Join-Path $audio "emu278b.cpp") (Join-Path $audio "ymfm\ymfm_opl.cpp") `
        (Join-Path $audio "ymfm\ymfm_pcm.cpp") (Join-Path $audio "ymfm\ymfm_adpcm.cpp") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "MSX-AUDIO host compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "MSX-AUDIO emulation regressions failed" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
