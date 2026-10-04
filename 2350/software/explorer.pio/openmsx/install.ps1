# Installs the PicoVerse 2350 Explorer openMSX script and extension for the current user.
# Usage: powershell -ExecutionPolicy Bypass -File install.ps1 [-OpenMSXHome <openMSX user directory>]
param(
    [string]$OpenMSXHome
)

$ErrorActionPreference = 'Stop'

if (-not $OpenMSXHome) {
    if ($env:OPENMSX_HOME) {
        $OpenMSXHome = $env:OPENMSX_HOME
    } else {
        $OpenMSXHome = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'openMSX'
    }
}

$source = Join-Path $PSScriptRoot 'share'
$destination = Join-Path $OpenMSXHome 'share'

foreach ($dir in 'scripts', 'extensions\PicoVerse_2350') {
    $target = Join-Path $destination $dir
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Copy-Item -Path (Join-Path $source "$dir\*") -Destination $target -Force
    Write-Host "Installed $dir to $target"
}
Write-Host "Restart openMSX: 'PicoVerse 2350 Explorer' is now in the Extensions menu and 'help picoverse2350' works in the console (F10)."
