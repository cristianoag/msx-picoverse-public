# MSX PICOVERSE PROJECT
# (c) 2026 Cristiano Goncalves
# The Retro Hacker
#
# make-flash-image.ps1 - builds a Carnivore2 FlashROM image (8 MB) for "carnivore2.exe -f"
# from the three RBSC component binaries.
#
# The layout is the one of the factory Carnivore2.rom (checked byte for byte):
#   0x000000  boot menu, 32 KB, stored as four 8 KB pieces at 0x0000, 0x2000, 0xC000 and 0xE000
#   0x004000  default configuration record (64 bytes, copied from the factory image)
#   0x010000  Nextor/IDE BIOS (BIDECMFC.BIN, 128 KB)
#   0x030000  FM-PAC BIOS (FMPCCMFC.BIN, 64 KB)
#   everything else 0xFF (empty ROM directory and free space)
#
# Usage:
#   .\make-flash-image.ps1 -BootMenu BOOTCMFC.BIN -IdeBios BIDECMFC.BIN -FmPacBios FMPCCMFC.BIN -Output flash.rom
#
# The binaries are in the RBSC repository (https://github.com/RBSC/Carnivore2):
# BootMenu\BOOTCMFC.BIN and BIOSes\BIDECMFC.BIN, BIOSes\FMPCCMFC.BIN.
#
# This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
# License". https://creativecommons.org/licenses/by-nc-sa/4.0/

param(
    [Parameter(Mandatory = $true)][string]$BootMenu,
    [Parameter(Mandatory = $true)][string]$IdeBios,
    [Parameter(Mandatory = $true)][string]$FmPacBios,
    [string]$Output = "flash.rom"
)

$ErrorActionPreference = "Stop"

function Read-Exact([string]$path, [int]$size, [string]$what)
{
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $path))
    if ($bytes.Length -ne $size) { throw "$what must be $size bytes (got $($bytes.Length)): $path" }
    return $bytes
}

$boot = Read-Exact $BootMenu 32768 "Boot menu"
$ide = Read-Exact $IdeBios 131072 "IDE BIOS"
$fm = Read-Exact $FmPacBios 65536 "FM-PAC BIOS"

$image = New-Object byte[] (8 * 1024 * 1024)
$blank = New-Object byte[] 65536
for ($i = 0; $i -lt $blank.Length; $i++) { $blank[$i] = 0xFF }
for ($o = 0; $o -lt $image.Length; $o += $blank.Length) { [Buffer]::BlockCopy($blank, 0, $image, $o, $blank.Length) }

[Array]::Copy($boot, 0x0000, $image, 0x0000, 0x2000)
[Array]::Copy($boot, 0x2000, $image, 0x2000, 0x2000)
[Array]::Copy($boot, 0x4000, $image, 0xC000, 0x2000)
[Array]::Copy($boot, 0x6000, $image, 0xE000, 0x2000)
# First ROM directory record at 0x4000: the "DefConfig: RAM+IDE+FMPAC+SCC" configuration of the factory image.
$defConfig = for ($i = 0; $i -lt 128; $i += 2) { [Convert]::ToByte("00FF000043446566436F6E6669673A2052414D2B4944452B464D5041432B5343432020F85000853F40F870018C3F60F890028C3F80F8B0038C3FA0FF380001FF".Substring($i, 2), 16) }
[Array]::Copy([byte[]]$defConfig, 0, $image, 0x4000, $defConfig.Length)

[Array]::Copy($ide, 0, $image, 0x10000, $ide.Length)
[Array]::Copy($fm, 0, $image, 0x30000, $fm.Length)

$target = if ([IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path (Get-Location).Path $Output }
[IO.File]::WriteAllBytes($target, $image)
Write-Host "Wrote $Output (8 MB). Use it with: carnivore2.exe -f $Output -o carnivore2.uf2"
