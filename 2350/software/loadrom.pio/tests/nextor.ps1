$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot
$creator = Join-Path $root "tool\dist\loadrom.exe"
if (!(Test-Path -LiteralPath $creator)) { throw "Build and package LoadROM before running this test" }
$firmware = [IO.File]::ReadAllBytes((Join-Path $root "pico\loadrom\dist\loadrom.bin"))
$wifi = [IO.File]::ReadAllBytes((Join-Path $root "..\wifi\bios\ESP8266P.rom"))
$cases = @(
    @{ Id = "nextor3"; Options = @("-s3"); Name = "Nextor Sunrise 3.0.0 (SD)"; Version = "3.0.0"; Wifi = $false },
    @{ Id = "nextor3-wifi"; Options = @("--sunrise3-sd", "-w"); Name = "Nextor Sunrise 3.0.0 (SD)"; Version = "3.0.0"; Wifi = $true },
    @{ Id = "nextor2"; Options = @("-s1"); Name = "Nextor Sunrise IDE (SD)"; Version = "2.1.4"; Wifi = $false }
)
$temp = Join-Path ([IO.Path]::GetTempPath()) ("loadrom-nextor-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $temp | Out-Null
$files = @()
$sha = [Security.Cryptography.SHA256]::Create()
try {
    foreach ($case in $cases) {
        $uf2 = Join-Path $temp ($case.Id + ".uf2")
        $files += $uf2
        $options = $case.Options
        $output = & $creator @options --output $uf2
        if ($LASTEXITCODE -ne 0) { throw "LoadROM generation failed: $($case.Id)`n$($output -join "`n")" }
        $kernel = [IO.File]::ReadAllBytes((Join-Path $root ("resources\Nextor-" + $case.Version + ".SunriseIDE.MasterOnly.ROM")))
        $blocks = [IO.File]::ReadAllBytes($uf2)
        if ($blocks.Length -eq 0 -or $blocks.Length % 512 -ne 0) { throw "Invalid UF2 length: $($case.Id)" }
        $image = New-Object byte[] ($blocks.Length / 2)
        $count = $blocks.Length / 512
        for ($i = 0; $i -lt $count; $i++) {
            $offset = $i * 512
            if ([BitConverter]::ToUInt32($blocks, $offset) -ne 0x0A324655 -or
                [BitConverter]::ToUInt32($blocks, $offset + 4) -ne [Convert]::ToUInt32("9E5D5157", 16) -or
                [BitConverter]::ToUInt32($blocks, $offset + 8) -ne 0x2000 -or
                [BitConverter]::ToUInt32($blocks, $offset + 12) -ne 0x10000000 + $i * 256 -or
                [BitConverter]::ToUInt32($blocks, $offset + 16) -ne 256 -or
                [BitConverter]::ToUInt32($blocks, $offset + 20) -ne $i -or
                [BitConverter]::ToUInt32($blocks, $offset + 24) -ne $count -or
                [BitConverter]::ToUInt32($blocks, $offset + 28) -ne [Convert]::ToUInt32("E48BFF59", 16) -or
                [BitConverter]::ToUInt32($blocks, $offset + 508) -ne 0x0AB16F30) {
                throw "Invalid UF2 block $i`: $($case.Id)"
            }
            [Array]::Copy($blocks, $offset + 32, $image, $i * 256, 256)
        }
        $extraSize = 0
        $mapper = 15
        if ($case.Wifi) { $extraSize = $wifi.Length; $mapper = $mapper -bor 0x20 }
        $size = $firmware.Length + 59 + $kernel.Length + $extraSize
        if ($image.Length -ne [Math]::Ceiling($size / 256) * 256) { throw "Unexpected UF2 layout: $($case.Id)" }
        $record = $firmware.Length
        $name = [Text.Encoding]::ASCII.GetBytes($case.Name)
        for ($i = 0; $i -lt 50; $i++) {
            $expected = 0
            if ($i -lt $name.Length) { $expected = $name[$i] }
            if ($image[$record + $i] -ne $expected) { throw "Incorrect record name/padding byte $i`: $($case.Id)" }
        }
        if ($image[$record + 50] -ne $mapper -or
            [BitConverter]::ToUInt32($image, $record + 51) -ne $kernel.Length -or
            [BitConverter]::ToUInt32($image, $record + 55) -ne 59) {
            throw "Incorrect Nextor record type/size/offset: $($case.Id)"
        }
        if ([BitConverter]::ToString($sha.ComputeHash($image, 0, $firmware.Length)) -ne
            [BitConverter]::ToString($sha.ComputeHash($firmware)) -or
            [BitConverter]::ToString($sha.ComputeHash($image, $record + 59, $kernel.Length)) -ne
            [BitConverter]::ToString($sha.ComputeHash($kernel))) {
            throw "Firmware or Nextor payload mismatch: $($case.Id)"
        }
        if ($case.Wifi -and [BitConverter]::ToString($sha.ComputeHash($image, $record + 59 + $kernel.Length, $wifi.Length)) -ne
            [BitConverter]::ToString($sha.ComputeHash($wifi))) {
            throw "WiFi payload mismatch: $($case.Id)"
        }
        Write-Output "PASS: $($case.Id) exact name/padding, record fields, firmware/kernel bytes and UF2/WiFi layout"
    }
} finally {
    $sha.Dispose()
    foreach ($file in $files) {
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
    }
    Remove-Item -LiteralPath $temp
}
