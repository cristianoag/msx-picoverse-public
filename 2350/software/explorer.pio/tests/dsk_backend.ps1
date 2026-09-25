param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"

# Compiles the production Sunrise .DSK backend against stub Pico/FatFs headers
# and exercises its Core 1 service loop. Card/bus timing is not simulated.
$root = Split-Path $PSScriptRoot
$storage = Join-Path $root "pico\explorer\storage"

$work = Join-Path $PSScriptRoot (".dsk-backend-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory (Join-Path $work "pico") | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $work "pico\stdlib.h"), "#pragma once`n")
    [IO.File]::WriteAllText((Join-Path $work "pico\sync.h"), "#pragma once`n#define __dmb() ((void)0)`n")
    $diskio = @"
#pragma once
#include <stdint.h>
typedef uint8_t BYTE;
typedef unsigned int UINT;
typedef uint32_t LBA_t;
typedef BYTE DSTATUS;
typedef enum { RES_OK = 0, RES_ERROR, RES_WRPRT, RES_NOTRDY, RES_PARERR } DRESULT;
#define STA_NOINIT 0x01
DSTATUS disk_initialize(BYTE pdrv);
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);
"@
    [IO.File]::WriteAllText((Join-Path $work "diskio.h"), $diskio)
    $exe = Join-Path $work "dsk-backend.exe"
    & $CC -std=c11 -Wall -Wextra -Werror -I $work -I $storage (Join-Path $PSScriptRoot "dsk_backend.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "DSK backend host compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "DSK backend host tests failed" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
