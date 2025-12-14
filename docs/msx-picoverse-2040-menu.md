# MSX PicoVerse 2040 — Menu Documentation

Author: Cristiano Goncalves
Last updated: November 20, 2025

This document describes the MSX menu ROM used by the PicoVerse 2040 project. The implementation lives in `2040/software/multirom/msx/src/menu.c` and its interface/definitions are in `2040/software/multirom/msx/src/menu.h`.

## Purpose

- Present a navigable list of ROM images stored in the Pico flash configuration area and allow the user to load a selected ROM (or the NEXTOR ROM) from the MSX keyboard.

## Files and locations

- Implementation: `2040/software/multirom/msx/src/menu.c`
- Headers/definitions: `2040/software/multirom/msx/src/menu.h`
- Documentation: this file `docs/msx-picoverse-2040-menu.md`

## High-level behavior

- On boot the menu reads a contiguous configuration area starting at `MEMORY_START` (0x8000) and parses a sequence of fixed-size ROM records.
- Each record is `ROM_RECORD_SIZE` bytes and encodes: name, mapper byte, 4-byte size, 4-byte offset, and optional flags/metadata (see header for exact layout).
- The parsed records populate an array `records[MAX_ROM_RECORDS]` and `totalFiles` and `totalSize` are computed.
- The menu displays up to `FILES_PER_PAGE` entries per screen.

## ROM record format (on-flash)

- Total record size in flash: `ROM_RECORD_SIZE` bytes (see `menu.h`).
- Layout (byte offsets, relative to record start, as of current code):
  - 0..49 : Name (50 bytes copied into `records[].Name` and NUL-terminated in RAM).
  - 50    : Reserved/flags (implementation detail).
  - 51    : Mapper (1 byte).
  - 52..55: Size (32-bit little-endian).
  - 56..59: Offset (32-bit little-endian).
- The header defines `ROM_NAME_MAX` accordingly to hold the full 50‑character name plus terminator.

Note: The flash area is considered ended when a record's entire `ROM_RECORD_SIZE` bytes are `0xFF` (checked by `isEndOfData`).

## In-memory structures

- ROMRecord (C struct)
  - `char Name[ROM_NAME_MAX]`
  - `unsigned char Mapper` (1 byte)
  - `unsigned long Size` (4 bytes)
  - `unsigned long Offset` (4 bytes)
  - Optional extra fields as defined in `menu.h` (e.g., flags).
- Globals
  - `ROMRecord records[MAX_ROM_RECORDS]` — array of parsed records
  - `unsigned char totalFiles` — number of parsed entries
  - `unsigned long totalSize` — sum of all record sizes
  - `int currentPage`, `int totalPages`, `int currentIndex` — UI state

## Menu display

- Screen mode: `Screen(0)` (text mode). The code inverts character graphics for visual selection using `invert_chars(32, 126)`.
- Header: `"MSX PICOVERSE 2040 [MultiROM vX.XX]"` with a separating line.
- Each file line prints: `Name`, `Size` in KB, and mapper description (`mapper_description()`).
  - Display `printf` base format (as of current code):  
    `" %-24s %04lu %-7s"`  
    Long names (up to 50 chars) are shown using a horizontal scrolling (“rolling”) effect so the full name can be read over time.
- Footer: Page X/Y and quick help (e.g. `"[H - Help]"`).
- The currently selected entry is indicated by a `>` and printed with inverted characters via `print_str_inverted()`.

## Navigation and keys

Keys handled (Fusion-C key codes from `WaitKey()`):

- Up arrow: 30 — move selection up one (if > 0). If moving above start of page, moves to previous page and refreshes display.
- Down arrow: 31 — move selection down one (if < `totalFiles - 1`). If moving past page end, moves to next page and refreshes display.
- Right arrow: 28 — go to next page (if `currentPage < totalPages`). Selection set to first file on the page.
- Left arrow: 29 — go to previous page (if `currentPage > 1`). Selection set to first file on the page.
- Enter: 13, Space: 32 — load currently selected ROM (`loadGame(currentIndex)`).
- ESC: 27 — load the NEXTOR OS ROM (`loadGame(0)`).
- H/h: 72/104 — show help screen (`helpMenu()`).
- C/c: 99/67 — show config screen (`configMenu()`).

Note: Some keys may also be used in help/config screens; see the implementation for the latest behavior.

## Load behavior

- `loadGame(index)` checks `records[index].Mapper` and if non-zero writes the `index` to IO address `0x9D01` (monitor address) then executes `rst 0x00` twice to transfer control to the monitor/boot code.
- The monitor (Pico-side multirom firmware) reads the value at `MONITOR_ADDR` to determine which ROM to load.
- A mapper of 0 is treated as “non-loadable” by the menu.

## Helper functions and UI niceties

- `invert_chars(start, end)` — copies and bit-inverts character patterns from VRAM pattern table to an inverted pattern table to draw inverted text.
- `print_str_inverted()` and `print_str_normal()` — print strings mapped to inverted or normal pattern tables by shifting character codes.
- `clear_fkeys()` — clears function key strings in the BIOS area.
- `helpMenu()` — shows usage/help information and waits for a key before returning to the main menu.
- `configMenu()` — shows configuration options (e.g., video/boot/general settings) and returns to the main menu when done.
- Name scrolling — when a filename is longer than the visible area, the menu animates a horizontal scrolling window over the full 50‑character name for the currently visible entries.

## Mapper descriptions

- The code provides `mapper_description(int number)` which maps mapper codes to human-readable strings such as:
  - PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16, SYSTEM
- The header may contain a separate `rom_types[]` table; implementers should use the runtime `mapper_description()` provided by `menu.c` for displayed names, as it reflects the current mapping logic.

## Edge cases and behaviour notes

- Page calculation in the current code uses a formula that may overcount when `totalFiles` is an exact multiple of `FILES_PER_PAGE`. This behavior is preserved for compatibility unless intentionally changed.
- `readROMData()` stops when it sees an all-`0xFF` record or reaches `MAX_ROM_RECORDS`.
- `loadGame()` only triggers when `Mapper != 0`; ensure record mapper bytes for valid ROMs are non-zero.
- If `totalFiles` is 0, the menu still initializes the screen and shows a header/footer but does not allow ROM loading.
- Very long names (close to 50 chars) rely on the rolling effect for full visibility; the static portion of the line is still constrained by the 24‑column base layout.

## Code pointers

- `readROMData()` — parse configuration area and populate `records[]` (in `menu.c`).
- `displayMenu()` — builds and prints the screen content and selected entry (in `menu.c`), including the rolling name effect.
- `navigateMenu()` — main key loop handling navigation and dispatching to help/config/load (in `menu.c`).
- `loadGame()` — writes the index to `0x9D01` and triggers RST (in `menu.c`).
- `helpMenu()` — draws and handles the help screen.
- `configMenu()` — draws and handles the configuration screen.

## Examples

- Basic flow to display and load a ROM:
  1. Boot to menu ROM; `main()` calls `readROMData()` and `displayMenu()`.
  2. Use UP/DOWN to select a file; press ENTER to load the selected ROM.
  3. To boot NEXTOR OS ROM press ESC.
  4. Use H for help and C for configuration from the main menu.
