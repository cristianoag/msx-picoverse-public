param([string]$CC = "gcc")
$ErrorActionPreference = "Stop"

# Runs the production microSD rename helpers (R) on the host and checks how the
# MSX menu and the Pico firmware are wired together for the rename command.
$root = Split-Path $PSScriptRoot
$firmware = Get-Content (Join-Path $root "pico\explorer\explorer.c") -Raw
$menuHeader = Get-Content (Join-Path $root "msx\src\menu.h") -Raw
$menu = Get-Content (Join-Path $root "msx\src\menu.c") -Raw

function Get-CFunction([string]$source, [string]$name) {
    $match = [regex]::Match($source, "(?m)^(?:static[^\r\n]*|void[^\r\n]*)\b$name\b\)?\s*\([^;{]*\)\s*\{")
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

function Get-Define([string]$source, [string]$name) {
    $match = [regex]::Match($source, "(?m)^#define $name\s+(\S+)")
    if (!$match.Success) { throw "Cannot find #define $name" }
    return $match.Groups[1].Value
}

# The menu and the firmware must agree on the command and on where the name goes.
if ([Convert]::ToInt32((Get-Define $menuHeader "CMD_RENAME_ENTRY"), 16) -ne [Convert]::ToInt32((Get-Define $firmware "CMD_RENAME_ENTRY"), 16)) {
    throw "CMD_RENAME_ENTRY differs between the menu and the firmware"
}
if ([Convert]::ToInt32((Get-Define $menuHeader "MEMORY_START"), 16) -ne [Convert]::ToInt32((Get-Define $firmware "DATA_BASE_ADDR"), 16)) {
    throw "The menu writes the new name outside the firmware's capture window"
}
if ($menu -notmatch "memcpy\(\(void \*\)MEMORY_START, name, strlen\(name\) \+ 1\);") {
    throw "The menu no longer writes the NUL terminated name to MEMORY_START"
}
$handler = [regex]::Match($firmware, "(?s)static inline void __not_in_flash_func\(handle_menu_write_explorer\)\(.*?\n\}")
if (!$handler.Success -or $handler.Value -notmatch "addr >= DATA_BASE_ADDR && addr < \(DATA_BASE_ADDR \+ sizeof\(rename_buf\)\)" -or
    $handler.Value -notmatch "data == CMD_RENAME_ENTRY\)\s*\{\s*process_rename_request\(\);" -or
    $handler.Value -notmatch "data != CMD_COPY_TO_FLASH && data != CMD_RENAME_ENTRY\)") {
    throw "handle_menu_write_explorer no longer captures the name, dispatches CMD_RENAME_ENTRY or keeps it busy until the refresh"
}
# The menu write handler runs from SRAM; an inlined rename handler overflowed it.
if ($firmware -notmatch "static void __noinline process_rename_request\(void\)") {
    throw "process_rename_request must stay __noinline (out of the SRAM-resident write handler)"
}
$finalize = [regex]::Match($firmware, "(?s)case REFRESH_FINALIZE: \{.*?return true;")
if (!$finalize.Success -or $finalize.Value -notmatch "resolve_rename_match\(\);") {
    throw "The list refresh no longer reports the renamed entry"
}
# Name-keyed sidecars (.FLA saves, /<name>.flash.PVC) may belong to another ROM:
# they must never be deleted to make room, and a save collision refuses the rename.
if ((Get-CFunction $firmware "rename_fla_image") -notmatch "sd_rename_sidecar\(scratch, flash_image_path, false\);" -or
    (Get-CFunction $firmware "rename_flash_sidecars") -notmatch "sd_rename_sidecar\(from, to, false\);") {
    throw "A rename may overwrite another ROM's .FLA save or flash options"
}
$sdRename = Get-CFunction $firmware "sd_rename_entry"
if ($sdRename -notmatch "(?s)fla_rename_allowed\(.*f_rename\(old_path, new_path\)") {
    throw "sd_rename_entry must check the FlashROM save before renaming the file"
}
if ((Get-CFunction $firmware "flash_store_rename_entry") -notmatch "(?s)flash_names_equal\(.*flash_rename_sidecars_allowed\(.*flash_store_append\(") {
    throw "flash_store_rename_entry must check names case-insensitively and the save before writing"
}
# Enter without an edit must not rename: the field can shorten a long name.
if ($menu -notmatch 'menu_ui_read_line\("Name: ", name, MAX_FILE_NAME_LENGTH - 1\) != 1') {
    throw "The menu sends a rename for an unedited (possibly shortened) name"
}

$code = "#define ROM_NAME_MAX $(Get-Define $firmware 'ROM_NAME_MAX')`n"
$code += "#define SD_PATH_MAX $(Get-Define $firmware 'SD_PATH_MAX')`n"
$code += [regex]::Match($firmware, '(?m)^#define RENAME_NAME_MAX\b[^\r\n]*').Value + "`n"
$descriptions = [regex]::Match($firmware, "(?s)static const char \*MAPPER_DESCRIPTIONS\[\] = \{.*?\};")
if (!$descriptions.Success) { throw "Cannot find MAPPER_DESCRIPTIONS" }
$code += $descriptions.Value + "`n"
$code += [regex]::Match($firmware, '(?m)^#define MAPPER_DESCRIPTION_COUNT\b[^\r\n]*').Value + "`n"
foreach ($name in @("mapper_tag_is_placeholder", "equals_ignore_case", "basename_from_path", "display_name_length",
    "build_display_name", "sd_pvc_path_from_rom", "rename_clean_name", "build_renamed_path")) {
    $code += (Get-CFunction $firmware $name) + "`n"
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("explorer-rename-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $work | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $work "rename-production.h"), $code)
    $exe = Join-Path $work "rename-test.exe"
    & $CC -std=c11 -Wall -Wextra -Werror -Wno-format-truncation -I $work (Join-Path $PSScriptRoot "rename.c") -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Rename host test compilation failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Rename host tests failed" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
