# Change Log

## PicoVerse 2040 Multirom v2.64

- Version bumped to v2.64 (top-level and tool Makefiles, and the `picoverse2040.tcl` script version).
- Fixed the MSX menu ROM list being two columns left of the header and footer on machines that boot with a narrower SCREEN 0 width, such as the Philips VG-8020 (WIDTH 37). The BIOS centers its text window when `LINLEN` < 40, but the list rows are written straight to VRAM at column 0. `displayMenu()` now finds the column where the BIOS printed the header (`detect_menu_col_offset()`), and `blit_row_vram()` adds that offset and writes only up to the end of the line.
- Added `openmsx/`, an openMSX add-on that runs MultiROM UF2 images without changing openMSX. `openmsx/share/scripts/picoverse2040.tcl` adds the `picoverse2040` console command (`insert`, `menu`, `boot`, `list`, `info`, `eject`) and the `picoverse2040_uf2`, `picoverse2040_slot` and `picoverse2040_hd` settings.
  - It rebuilds the Pico flash image from the UF2, finds the menu and config area after the firmware, and inserts the 32KB menu as a `Page12` cartridge.
  - A watchpoint on the menu's `0x9D81` ROM select register catches the selected entry. The script then inserts it with the matching openMSX mapper: `Page12`/`Page012`/`Page0123` for the planar ROMs (padded to their window), `KonamiSCC`, `Konami`, `ASCII8`, `ASCII16`, `NEO-8`, `NEO-16`, `ASCII16-X` and `Manbow2`.
  - The Nextor entries become generated openMSX extension configurations: `SunriseIDE`, or an expanded slot with `SunriseIDE` in sub-slot 0 and a 192KB `MemoryMapper` in sub-slot 1. They are written to `persistent/picoverse2040/extensions/` and inserted through a relative name (`../../persistent/picoverse2040/extensions/<name>`), so openMSX lists only `PicoVerse 2040 MultiROM` as an extension; copies left in `share/extensions` by earlier script builds are deleted at startup. A hard disk image stands in for the USB drive. The image comes from the `-hd` option of the last `picoverse2040 insert`, else the `picoverse2040_hd` setting (read every time a Nextor entry boots), else `persistent/picoverse2040/hd.dsk`.
  - Any reset (Machine menu, hotkey, `reset`), a power cycle, or restarting openMSX with an entry left in the slot (for example a setup restored at startup) brings the PicoVerse menu back. Only the reset that the script issues to start the selected entry keeps it. This uses the openMSX `after boot` event. The slot watcher replaces leftover cached ROMs and Nextor configurations with the menu of the last UF2. Extensions restored from setups and savestates are not always reported in their cartridge slot, so the script also checks `list_extensions` for `PicoVerse_2040` and its Nextor configurations. After a machine switch or savestate load, the menu watchpoint is re-armed, and a running ROM or Nextor entry keeps running until the next reset.
  - Added the `PicoVerse 2040 MultiROM` openMSX extension (`openmsx/share/extensions/PicoVerse_2040`), so the cartridge appears in the openMSX extension lists. Its placeholder ROM (`openmsx/src/placeholder.asm`) explains how to pick a UF2. The script replaces it with the menu of the last UF2 used (`picoverse2040_uf2`).
  - The script watches the cartridge slots, so a `.uf2` chosen as a ROM image (Media > Cartridge Slot, `-carta`, `carta`) is taken over and remembered.
  - The console explains how to write Windows paths when backslashes were eaten by Tcl (for example `C:\temp` arriving as `C:<TAB>emp`).
  - Extracted ROMs are cached per UF2 size and modification time (openMSX's Tcl has no `zlib`), and cached files are compared before reuse.
  - Includes `install.ps1`/`install.sh` installers for the script and the extension.
- The aggregate Makefile now builds the openMSX add-on (`make openmsx`, also part of `make` and `make clean`). `openmsx/Makefile` assembles the placeholder ROM with SDCC and, when `VERSION` is passed, fails if the script's `variable version` doesn't match.
- Documented the openMSX add-on in `docs/msx-picoverse-2040-openmsx.md`: installation, choosing the UF2, menu and reset behaviour, creating and attaching the Nextor USB drive image with `diskmanipulator` (including how long file names are shortened to 8.3), commands, settings and implementation. It replaces `openmsx/README.md`. The MultiROM manual and the docs index link to it.
- `docs/msx-picoverse-public-readme.md` now lists the openMSX emulated cartridge in the highlights, the documentation section and the MultiROM menu section. It also has a license note saying the add-on was created based on the public openMSX reference (manuals and source) and doesn't include or redistribute openMSX code.

## PicoVerse 2040 Multirom v2.63

- Version bumped to v2.63 (top-level and tool Makefiles).
- Fixed lost bank-switch writes in every banked mapper loop (`banked8_loop` for Konami/Konami-SCC/ASCII8, plus ASCII16, ASCII16-X, Neo-8 and Neo-16). Those loops blocked on `pio_sm_get_blocking()` for the next cartridge read and only drained the memory write captor FIFO before and after it, so game code executing from MSX RAM that issued a burst of segment switches without any intervening cartridge read overflowed the 8-entry RX FIFO. The write state machine then stalled on `push block` and the extra switches were silently dropped, leaving the mapper register on a stale segment (and, with an ASCII16-X/Neo cache miss, forcing the bank refill to run while `/WAIT` was already asserted). Added `pio_get_read_draining_writes()`, which drains the memory write FIFO while waiting for the next read, and switched all banked loops to it. Same fix as PicoVerse 2040 Loadrom v2.62 and PicoVerse 2350 Loadrom v2.70, reported with "Go Figure v1.2" (ASCII16-X), which alternates the page-1 segment twice per call from a page-3 RAM routine and hung on the palette cross-fade.
- Kept the new idle loop at the same read-response latency as the previous blocking wait by sampling `FSTAT` once per iteration and testing the read and write RX-empty flags from that single sample. Polling the two FIFOs with separate `pio_sm_is_rx_fifo_empty()` calls doubles the PIO register accesses in the loop, which on the Cortex-M0+ stretches `/WAIT` on every cartridge read enough to slow VDP transfer loops and trigger "VDP too slow" reports in timing-sensitive games.
- The planar 16/32/48/64KB loops keep the plain blocking wait: they have no mapper registers and therefore no write handler.

## PicoVerse 2040 Multirom v2.62

- Fixed the PC tool mapper-detection read path to reject truncated ROM reads before hashing or scanning the allocated ROM buffer.
- Version bumped to v2.62 (top-level and tool Makefiles).

## PicoVerse 2040 Multirom v2.61

- Bumped the multirom.pio version to v2.61.
- Improved MSX MultiROM menu list drawing by rendering each ROM row through a single VRAM block write instead of per-character BIOS output.
- Fixed the MultiROM tool mapper detector so 8KB ROMs do not read past the ROM buffer when building images with embedded Sunrise/mapper options.
- Restored selected-row name scrolling so long ROM names advance from the first visible character while using the VRAM row renderer.
- Reduced page-navigation redraws so LEFT/RIGHT and page-boundary moves update only the ROM rows and footer instead of repainting the static title and separator lines.
- Frogger - Konami (1983) [RC-704] rom on the MSX1 Konami compilation had a bug and was replaced by a working dump from File Hunter.

## PicoVerse 2040 Multirom v2.60

- Moved the shared MultiROM version declaration into the aggregate and tool Makefiles, removing the separate `version.mk` include.
- Refreshed the generated ROM mapper SHA1 database from the current openMSX `softwaredb.xml` and updated the generator to parse the new attribute-based XML format.
  
## PicoVerse 2040 Multirom v2.59

- Bumped the multirom.pio version to v2.59.
- Changed the memory-read `/WAIT` driver to open-drain behaviour: the firmware now asserts `/WAIT` by pulling it low and releases the shared line as hi-Z after stretched read cycles and ROM-cache setup.

## PicoVerse 2040 Multirom v2.58

- Bumped the multirom.pio version to v2.58.
- Set all multirom.pio firmware target system clocks to 230 MHz to improve stability with pico boards with bad quality flash memory.

## PicoVerse 2040 Multirom v2.57

- Updated the MSX MultiROM menu separator lines to use the same custom MSX glyph used by Explorer, copying character pattern `0x17` into printable slot `0x7E` and rendering that glyph instead of ASCII hyphens.
- Removed the unused MSX menu configuration screen and disabled the `C`/`c` key shortcut that opened it.
