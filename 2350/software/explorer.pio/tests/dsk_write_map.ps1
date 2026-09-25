param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"

# Runs the production dsk_build_write_map() against the real FatFs (ff15) on a
# RAM disk. The firmware's ffconf.h is used unchanged except for FF_USE_CHMOD,
# which the test needs to mark an image read-only.
$root = Split-Path $PSScriptRoot
$firmware = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw
$fatfs = Join-Path $root "pico\explorer\lib\no-OS-FatFS-SD-SDIO-SPI-RPi-Pico\src"
$storage = Join-Path $root "pico\explorer\storage"

function Get-CFunction([string]$source, [string]$name) {
    $match = [regex]::Match($source, "(?m)^(?:static[^\r\n]*|void )\b$name\b\)?\s*\([^;{]*\)\s*\{")
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

$work = Join-Path $PSScriptRoot (".dsk-write-map-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $work | Out-Null
try {
    $conf = Get-Content (Join-Path $fatfs "include\ffconf.h") -Raw
    $conf = [regex]::Replace($conf, '(?m)^#define FF_USE_CHMOD\s+0', '#define FF_USE_CHMOD 1')
    [IO.File]::WriteAllText((Join-Path $work "ffconf.h"), $conf)
    [IO.File]::WriteAllText((Join-Path $work "dsk-map-production.h"), (Get-CFunction $firmware "dsk_build_write_map") + "`n")

    $includes = @("-I", $work, "-I", (Join-Path $fatfs "ff15\source"), "-I", $storage)
    $objects = @()
    foreach ($name in @("ff", "ffunicode", "ffsystem")) {
        $object = Join-Path $work "$name.o"
        & $CC -std=c11 -O1 @includes -c (Join-Path $fatfs "ff15\source\$name.c") -o $object
        if ($LASTEXITCODE -ne 0) { throw "FatFs $name host compilation failed" }
        $objects += $object
    }
    $exe = Join-Path $work "dsk-write-map.exe"
    & $CC -std=c11 -Wall -Wextra -Werror @includes (Join-Path $PSScriptRoot "dsk_write_map.c") @objects -o $exe
    if ($LASTEXITCODE -ne 0) { throw "DSK write map host compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "DSK write map host tests failed" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
