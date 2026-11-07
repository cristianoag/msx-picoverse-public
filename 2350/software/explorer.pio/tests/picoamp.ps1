param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$pico = Join-Path $root "pico\explorer"
$engine = Join-Path $pico "picoamp\pvaudio"
$fatfs = Join-Path $pico "lib\no-OS-FatFS-SD-SDIO-SPI-RPi-Pico\src"
$mp3 = Join-Path $pico "lib\picomp3lib\src"
$rom = Join-Path $root "resources\PICOAMP.ROM"
$creator = Join-Path $root "tool\dist\explorer.exe"
$work = Join-Path $PSScriptRoot (".picoamp-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null

function Assert-Bytes([byte[]]$expected, [byte[]]$actual, [int]$offset) {
    if ($offset -lt 0 -or $offset + $expected.Length -gt $actual.Length) { throw "Payload outside image" }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $a = [Convert]::ToBase64String($sha.ComputeHash($expected))
        $b = [Convert]::ToBase64String($sha.ComputeHash($actual, $offset, $expected.Length))
        if ($a -ne $b) { throw "Payload differs at offset $offset" }
    } finally { $sha.Dispose() }
}

function Read-Uf2([string]$path) {
    $uf2 = [IO.File]::ReadAllBytes($path)
    if ($uf2.Length % 512) { throw "Truncated UF2" }
    $image = New-Object byte[] ($uf2.Length / 2)
    for ($i = 0; $i -lt $uf2.Length; $i += 512) {
        if ([BitConverter]::ToUInt32($uf2, $i) -ne 0x0A324655 -or
            [BitConverter]::ToUInt32($uf2, $i + 4) -ne [Convert]::ToUInt32('9E5D5157', 16) -or
            [BitConverter]::ToUInt32($uf2, $i + 16) -ne 256 -or
            [BitConverter]::ToUInt32($uf2, $i + 508) -ne 0x0AB16F30) { throw "Invalid UF2 block" }
        $target = [BitConverter]::ToUInt32($uf2, $i + 12) - 0x10000000
        if ($target -ne $i / 2) { throw "Noncontiguous UF2 payload" }
        [Buffer]::BlockCopy($uf2, $i + 32, $image, $target, 256)
    }
    return ,$image
}

function Get-CFunction([string]$source, [string]$name) {
    $match = [regex]::Match($source, "(?m)^(?:void|bool)[^\r\n]*\b$name\b\s*\([^;{]*\)\s*\{")
    if (!$match.Success) { throw "Cannot find production function $name" }
    $end = $match.Index + $match.Length
    $depth = 1
    while ($depth -gt 0 -and $end -lt $source.Length) {
        if ($source[$end] -eq '{') { $depth++ }
        if ($source[$end] -eq '}') { $depth-- }
        $end++
    }
    return $source.Substring($match.Index, $end - $match.Index)
}

try {
    $sources = @((Join-Path $PSScriptRoot "picoamp.c"),
        (Join-Path $pico "audio\emu2413.c"), (Join-Path $pico "picoamp\mp3_alloc_dispatch.c"),
        (Join-Path $engine "tests\host_hal.c"), (Join-Path $engine "tests\host_esp.c"),
        (Join-Path $fatfs "ff15\source\ff.c"), (Join-Path $fatfs "ff15\source\ffunicode.c"),
        (Join-Path $fatfs "ff15\source\ffsystem.c"))
    $sources += Get-ChildItem -LiteralPath $engine -Filter "*.c" |
        Where-Object { $_.Name -ne "pva_hal_pico.c" } | ForEach-Object { $_.FullName }
    $sources += Get-ChildItem -LiteralPath $mp3 -Filter "*.c" | ForEach-Object { $_.FullName }
    $exe = Join-Path $work "picoamp-test.exe"
    # Compile Helix's own portable arithmetic on the host instead of MSVC assembly.
    $assembly = (Get-Content -LiteralPath (Join-Path $mp3 "assembly.h") -Raw).Replace("`r`n", "`n")
    $start = $assembly.IndexOf("#else`n`n#include <stdint.h>")
    $end = $assembly.IndexOf("#endif`t/* platforms */", $start)
    if ($start -lt 0 -or $end -le $start) { throw "Cannot find Helix portable arithmetic" }
    $portable = Join-Path $work "helix-portable.h"
    [IO.File]::WriteAllText($portable, "#pragma once`n#define _ASSEMBLY_H`n" + $assembly.Substring($start + 6, $end - $start - 6))
    & $CC -std=gnu11 -O0 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -DPVA_HOST `
        -include $portable `
        -DMPDEC_ALLOCATOR=pv_mp3_alloc -include (Join-Path $pico "picoamp\mp3_alloc_dispatch.h") `
        -I $pico -I (Join-Path $fatfs "include") -I (Join-Path $fatfs "ff15\source") -I $mp3 `
        @sources -lm -o $exe
    if ($LASTEXITCODE -ne 0) { throw "PicoAmp engine test compilation failed" }
    Push-Location $work
    try {
        & $exe $rom
        if ($LASTEXITCODE -ne 0) { throw "PicoAmp engine regression failed" }
    } finally { Pop-Location }

    $hal = Get-Content -LiteralPath (Join-Path $engine "pva_hal_pico.c") -Raw
    $code = (([regex]::Matches($hal, '(?m)^#define PVA_I2S_\w+[^\r\n]*') |
        ForEach-Object { $_.Value }) -join "`n") + "`n"
    foreach ($name in @("picoamp_audio_adopt", "picoamp_audio_set_volume", "pva_hal_audio_init", "pva_hal_audio_commit")) {
        $code += (Get-CFunction $hal $name) + "`n"
    }
    [IO.File]::WriteAllText((Join-Path $work "picoamp-audio-production.h"), $code)
    $audioExe = Join-Path $work "picoamp-audio.exe"
    & $CC -std=c11 -Wall -Wextra -Werror -I $work (Join-Path $PSScriptRoot "picoamp_audio.c") -o $audioExe
    if ($LASTEXITCODE -ne 0) { throw "PicoAmp audio test compilation failed" }
    & $audioExe
    if ($LASTEXITCODE -ne 0) { throw "PicoAmp audio handoff regression failed" }

    $firmware = [IO.File]::ReadAllBytes((Join-Path $pico "build\explorer.bin"))
    $menu = [IO.File]::ReadAllBytes((Join-Path $root "msx\dist\menu.rom"))
    $player = [IO.File]::ReadAllBytes($rom)
    # PICOAMP.ROM of upstream release 1.3 (Jzzy-dev/msx-picoplay-picoamp, tag 1.3)
    $releaseSha = "2723BB79ABA4ED8A9F9E39815507AA8DD0F6D4FC5EE311395C5892E558B6F6AF"
    if ((Get-FileHash -LiteralPath $rom -Algorithm SHA256).Hash -ne $releaseSha) {
        throw "resources\PICOAMP.ROM is not the PicoAmp 1.3 release ROM"
    }
    $external = New-Object byte[] 8192
    $external[0] = 65; $external[1] = 66; $external[2] = 16; $external[3] = 64; $external[16] = 201
    $span = ($firmware.Length + 4095) -band (-bnot 4095)
    $cases = @(
        @{ Name="standalone"; Args=@("-pa"); PicoAmp=$true; Nextor=0 },
        @{ Name="alias"; Args=@("--picoamp", "-pa"); PicoAmp=$true; Nextor=0 },
        @{ Name="combined"; Args=@("-a", "-r", "-pa"); PicoAmp=$true; Nextor=10 },
        @{ Name="nextor"; Args=@("-m1"); PicoAmp=$false; Nextor=1 }
    )
    foreach ($case in $cases) {
        $path = Join-Path $work ($case.Name + ".uf2")
        $withExternal = $case.Name -eq "combined"
        $externalPath = Join-Path $work "External.PLA-16.ROM"
        if ($withExternal) { [IO.File]::WriteAllBytes($externalPath, $external) }
        Push-Location $work
        try {
            & $creator @($case.Args) -o $path | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "UF2 creation failed: $($case.Name)" }
        } finally { Pop-Location }
        $image = Read-Uf2 $path
        Assert-Bytes $firmware $image 0
        Assert-Bytes $menu $image $span
        $found = 0
        $nextor = 0
        $scanned = 0
        for ($entry = 0; $entry -lt 128; $entry++) {
            $pos = $span + 32768 + $entry * 80
            if ($image[$pos] -eq 255) { break }
            $mapper = $image[$pos + 71]
            $size = [BitConverter]::ToUInt32($image, $pos + 72)
            $offset = [BitConverter]::ToUInt32($image, $pos + 76)
            if ($mapper -eq 24) {
                $found++
                $label = "PicoAmp 1.3 by Jzzy"
                if ($size -ne 65536 -or [Text.Encoding]::ASCII.GetString($image, $pos, $label.Length) -ne $label -or $image[$pos + $label.Length] -ne 0) {
                    throw "Invalid PicoAmp record"
                }
                Assert-Bytes $player $image ($span + $offset)
            } elseif ($mapper -eq 1) {
                $scanned++
                if ($size -ne $external.Length) { throw "Scanned ROM size changed" }
                Assert-Bytes $external $image ($span + $offset)
            } elseif ($mapper -ne 21) {
                $nextor++
                if ($size -ne 131072) { throw "Nextor payload changed" }
            }
        }
        if ($found -ne [int]$case.PicoAmp -or $nextor -ne $case.Nextor -or $scanned -ne [int]$withExternal) {
            throw "Unexpected embedded records: $($case.Name)"
        }
        if ($withExternal) { Remove-Item -LiteralPath $externalPath }
    }
    Write-Output "PASS: -pa/--picoamp deduplication, standalone and combined UF2 records, exact firmware/menu/player bytes, opt-in omission"

    $map = Get-Content -LiteralPath (Join-Path $pico "build\explorer.elf.map") -Raw
    foreach ($symbol in @("picoamp_run", "pva_mem_read", "pva_mem_write", "records")) {
        $address = [regex]::Match($map, "(?m)^\s*0x([0-9a-fA-F]+)\s+$symbol\s*$")
        if (!$address.Success) { throw "Missing linked symbol $symbol" }
        $value = [Convert]::ToUInt32($address.Groups[1].Value, 16)
        if ($value -lt 0x20000000 -or $value -ge 0x20080000) { throw "$symbol is not in internal SRAM" }
    }
    $menuMap = Get-Content -LiteralPath (Join-Path $root "msx\build\menu.map") -Raw
    $start = [Convert]::ToUInt32([regex]::Match($menuMap, '(?m)^\s*([0-9A-Fa-f]+)\s+s__CODE\b').Groups[1].Value, 16)
    $length = [Convert]::ToUInt32([regex]::Match($menuMap, '(?m)^\s*([0-9A-Fa-f]+)\s+l__CODE\b').Groups[1].Value, 16)
    if ($start + $length -gt 0xB900 - 200) { throw "MSX menu leaves less than 200 bytes before the protocol window" }
    Write-Output ("PASS: SRAM-resident PicoAmp bus/window/ROM workspace; MSX _CODE ends at 0x{0:X} ({1} bytes free)" -f ($start + $length), (0xB900 - $start - $length))
} finally {
    Get-ChildItem -LiteralPath $work -File | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
    Remove-Item -LiteralPath $work
}
