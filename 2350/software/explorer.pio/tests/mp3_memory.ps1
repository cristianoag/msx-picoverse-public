param([string]$BuildDir = (Join-Path (Split-Path $PSScriptRoot) "pico\explorer\build"))
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot
$map = Get-Content (Join-Path $BuildDir "explorer.elf.map") -Raw
$cache = Get-Content (Join-Path $BuildDir "CMakeCache.txt") -Raw
$mp3 = Get-Content (Join-Path $root "pico\explorer\audio\mp3.c") -Raw

function Read-Number([string]$text, [string]$pattern, [int]$base = 10) {
    $match = [regex]::Match($text, $pattern)
    if (!$match.Success) { throw "Missing memory-budget input: $pattern" }
    return [Convert]::ToUInt32($match.Groups[1].Value, $base)
}

$heapStart = Read-Number $map '(?m)^\s*0x([0-9a-fA-F]+)\s+__end__\s*=' 16
$heapLimit = Read-Number $map '(?m)^\s*0x([0-9a-fA-F]+)\s+__HeapLimit\s*=' 16
$count = Read-Number $mp3 '(?m)^#define MP3_I2S_BUFFER_COUNT\s+(\d+)'
$samples = Read-Number $mp3 '(?m)^#define MP3_I2S_BUFFER_SAMPLES\s+(\d+)'
$stride = Read-Number $mp3 'producer_format\s*=\s*\{[^}]*\.sample_stride\s*=\s*(\d+)'
$extrasMatch = [regex]::Match($cache, '(?m)^PICO_EXTRAS_PATH:PATH=([^\r\n]+)')
if (!$extrasMatch.Success) { throw "Missing configured pico-extras path" }
$extras = $extrasMatch.Groups[1].Value.Replace('/', '\')
$i2s = Get-Content (Join-Path $extras "src\rp2_common\pico_audio_i2s\audio_i2s.c") -Raw
$consumer = [regex]::Match($i2s, 'return audio_i2s_connect_extra\(producer, false, (\d+), (\d+), connection\)')
if (!$consumer.Success) { throw "I2S connection changed; update the MP3 memory-budget test" }
$consumerStride = Read-Number $i2s 'pio_i2s_consumer_buffer_format\.sample_stride\s*=\s*(4)'
$producerBytes = $count * $samples * $stride
$consumerBytes = [int]$consumer.Groups[1].Value * [int]$consumer.Groups[2].Value * $consumerStride
# CDC measured 6,344 bytes already allocated before audio initialization.
# Allow 7KB for that menu state, 4KB for allocator growth/fragmentation, and
# 2KB for pool descriptors/bookkeeping. Total capacity alone is not proof
# that the allocator can satisfy each request; hardware testing is still needed.
$reserve = 13 * 1024
$required = $producerBytes + $consumerBytes + $reserve
$available = $heapLimit - $heapStart
Write-Output "MP3 heap: $available bytes; PCM: $producerBytes + $consumerBytes; reserve: $reserve; required: $required"
if ($available -lt $required) {
    throw "MP3 startup heap budget exceeded by $($required - $available) bytes"
}
$reserved = Read-Number $map '(?m)^\.heap\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)' 16
if ($reserved -lt $required) {
    throw "The linked minimum heap is smaller than the MP3 budget; update PICO_HEAP_SIZE"
}
Write-Output "PASS: linked firmware retains MP3 producer/I2S buffer capacity and allocation reserve"
