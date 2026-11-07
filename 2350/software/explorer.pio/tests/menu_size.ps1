param([string]$Map = (Join-Path (Split-Path $PSScriptRoot) "msx\build\menu.map"))
$ErrorActionPreference = "Stop"

# The menu ROM's _CODE must end before the Pico page-buffer window
# (MEMORY_START in msx/src/menu.h): the Pico serves 0xB900 and up instead of ROM.
$limit = [Convert]::ToInt32((Select-String -Path (Join-Path (Split-Path $PSScriptRoot) "msx\src\menu.h") `
    -Pattern "^#define MEMORY_START (0x[0-9A-Fa-f]+)").Matches[0].Groups[1].Value, 16)
$start = $null
$length = $null
foreach ($line in Get-Content $Map) {
    if ($line -match "^\s+([0-9A-Fa-f]{8})\s+s__CODE\b") { $start = [Convert]::ToInt32($Matches[1], 16) }
    if ($line -match "^\s+([0-9A-Fa-f]{8})\s+l__CODE\b") { $length = [Convert]::ToInt32($Matches[1], 16) }
}
if ($null -eq $start -or $null -eq $length) { throw "s__CODE / l__CODE not found in $Map (build the menu first)" }
$end = $start + $length
if ($end -ge $limit) {
    throw ("Menu _CODE ends at 0x{0:X4}, which overlaps the page-buffer window at 0x{1:X4}" -f $end, $limit)
}
"PASS: menu _CODE ends at 0x{0:X4}, {1} bytes below 0x{2:X4}" -f $end, ($limit - $end), $limit
