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

# Compile the production OPL4 I/O service, not a reimplementation of it.
$code = ""
foreach ($name in @("opl4_port_offset", "opl4_handle_io_write", "opl4_handle_io_read", "opl4_service_io")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}

# Structural guards for the wiring that a host test cannot see.
$service = Get-CFunction $firmware "opl4_service_io"
if ($service -match 'pio_try_get_io_write\(&read_addr' -or $service -notmatch 'pio_try_get_io_read\(&read_addr\)') {
    throw "opl4_service_io must keep the read address apart from the write drain's address"
}
# SYSTEM entries start the OPL4 through the shared system audio hooks.
foreach ($hook in @("opl4_start\(service_io_on_core1\)", "return opl4_handle_io_write\(port, data\)",
    "return opl4_handle_io_read\(port, data\)", "opl4_audio_service_buffer\(\)")) {
    if ($firmware -notmatch $hook) { throw "system audio hook missing for OPL4: $hook" }
}
# The chip is only ready once the wait-reads responder exists, and never before
# the chip itself was built.
$start = Get-CFunction $firmware "opl4_start"
if ($start.IndexOf("opl4_prepare()") -gt $start.IndexOf("msx_pio_io_bus_init()") -or
    $start.IndexOf("msx_pio_io_bus_init()") -gt $start.IndexOf("opl4_ready = true")) {
    throw "opl4_start must prepare the chip, then install the I/O responder, then mark the profile ready"
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ("explorer-opl4-io-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $temp | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $temp "opl4-production.h"), $code)
    $exe = Join-Path $temp "opl4-io.exe"
    & $CC -std=c11 -Wall -Wextra -Werror -I $temp (Join-Path $PSScriptRoot "opl4_io.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "OPL4 I/O host test compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "OPL4 I/O host test failed" }
} finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}
