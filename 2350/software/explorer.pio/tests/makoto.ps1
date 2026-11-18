param([string]$CXX = "g++")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$audio = Join-Path $root "pico\explorer\audio"
$work = Join-Path $PSScriptRoot (".makoto-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $work | Out-Null
try {
    # The embedded rhythm ROM must match the resource file byte for byte.
    $rom = [IO.File]::ReadAllBytes((Join-Path $root "resources\YM2608_ADPCM.ROM"))
    if ($rom.Length -ne 8192) { throw "YM2608 rhythm ROM must be 8192 bytes (found $($rom.Length))" }
    $header = Get-Content (Join-Path $audio "ym2608_adpcm_rom.h") -Raw
    $embedded = [regex]::Matches($header, '0x([0-9a-fA-F]{2})') | ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) }
    if ($embedded.Count -ne $rom.Length -or (Compare-Object $embedded $rom -SyncWindow 0)) {
        throw "audio/ym2608_adpcm_rom.h does not match resources/YM2608_ADPCM.ROM"
    }
    $exe = Join-Path $work "makoto.exe"
    & $CXX -std=c++17 -O2 -DOPL4_OUTPUT_STEP=2 -DOPL4_SKIP_INACTIVE_FM=1 `
        -I $audio (Join-Path $PSScriptRoot "makoto.cpp") (Join-Path $audio "emu2608.cpp") `
        (Join-Path $audio "ymfm\ymfm_opn.cpp") (Join-Path $audio "ymfm\ymfm_ssg.cpp") `
        (Join-Path $audio "ymfm\ymfm_adpcm.cpp") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Makoto host compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Makoto emulation regressions failed" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
