param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"

# Compares the menu ROM's minimal printf/sprintf with the C library for every
# format string the menu uses.
$work = Join-Path ([IO.Path]::GetTempPath()) ("explorer-miniprintf-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $work | Out-Null
try {
    $exe = Join-Path $work "mini-printf-test.exe"
    & $CC -std=c11 -Wall -Wextra -Werror -Wno-format-truncation -Wno-format-nonliteral -Wno-format-zero-length -Wno-format-security `
        (Join-Path $PSScriptRoot "mini_printf.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "mini printf host test compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "mini printf host tests failed" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
