# Change Log

## PicoVerse 2040 MultiROM v2.70

- Refreshed the ROM mapper SHA1 database (3363 entries, from the latest Vampier softwaredb) by re-running `gen_romdb.py`: `tool/src/romdb.h`.

- Bumped the aggregate, MSX menu and PC tool versions and openMSX add-on to v2.70; standalone MSX builds now default to the same project version.
- Replaced the Nextor 3 beta 2 resource with the official 128 KB `resources/Nextor-3.0.0.SunriseIDE.MasterOnly.ROM` from release `v0.1.8-blueMSX-v0.1.5-Nextor-3.0`, and updated the generated header symbol.
- Updated `-s3` / `--sunrise3` and `-m3` / `--mapper3` help, menu entry names and related manuals for released Nextor 3.0.0. Kept Nextor 2.1.4, combined entries, USB loaders, mapper types and UF2 layouts unchanged.
- Validation: MSX menu/tool builds, openMSX version check, official ROM size/SHA-256 and standalone/combined Nextor UF2s with scanned ROMs pass. Verified exact names, record fields and firmware/menu/kernel bytes. `_CODE` ends at `0x63EC`, below `0x8000`; 

## PicoVerse 2040 Multirom v2.69

- Version bumped to v2.69 (top-level and tool Makefiles, and the `picoverse2040.tcl` script version).
- Moved the RP2040 firmware build from Pico SDK 2.2.0 to Pico SDK 2.3.1 and from the Arm GNU Toolchain 14.2.Rel1 to 15.2.Rel1 (GCC 15.2.1). `pico/multirom/CMakeLists.txt` now sets `sdkVersion 2.3.1`, `toolchainVersion 15_2_Rel1` and `picotoolVersion 2.3.1`, and the `.vscode` files (`settings.json`, `c_cpp_properties.json`, `launch.json`, `tasks.json`) point to the SDK 2.3.1, toolchain 15_2_Rel1 and picotool 2.3.1 folders.
- The SDK 2.3.1 tag (with submodules, including its bundled TinyUSB), `pico-sdk-tools` 2.3.1 (pioasm), picotool 2.3.1 and the Arm GNU Toolchain 15.2.Rel1 are expected under `~/.pico-sdk/sdk/2.3.1`, `~/.pico-sdk/tools/2.3.1`, `~/.pico-sdk/picotool/2.3.1` and `~/.pico-sdk/toolchain/15_2_Rel1`. The old `pico/multirom/build` directory must be removed once so CMake reconfigures against the new SDK and compiler (`make clean` does this).
- Clean `make all` build with SDK 2.3.1 and toolchain 15.2.Rel1 completes without changes to the firmware sources; `multirom.bin` went from 55816 to 53240 bytes. Rebuilt `msx/dist/menu.rom` (header shows `[MultiROM v2.69]`), the embedded firmware/menu headers and `tool/dist/multirom.exe`. Not yet tested on hardware; because the MSX bus handling is timing-sensitive, the new compiler needs a hardware check before release.

## PicoVerse 2040 Multirom v2.68

- Version bumped to v2.68 (top-level and tool Makefiles, and the `picoverse2040.tcl` script version).
- The MSX menu can now be driven with a joystick on either MSX joystick port. The commented-out Fusion-C `JoystickRead`/`TriggerRead` code in `msx/src/menu.c` was replaced by `poll_joystick()`, which uses the BIOS `GTSTCK`/`GTTRIG` routines through new `bios_gtstck()`/`bios_gttrig()` wrappers. Directions map to the cursor keys with a first-press delay (`JOY_FIRST_DELAY`) and auto-repeat (`JOY_REPEAT_DELAY`) so the stick is no longer too fast; fire buttons A/B on both ports act as Enter, once per press (a button held at boot is ignored until released). The help screen mentions the joystick.
- Docs: 2040 MultiROM tool manual, `msx-picoverse-2040-menu.md` and the public README (MultiROM menu section now mentions joystick support).

## PicoVerse 2040 Multirom v2.67

- Version bumped to v2.67 (top-level and tool Makefiles, and the `picoverse2040.tcl` script version). Rebuilt the MSX menu (header shows `[MultiROM v2.67]`) and `tool/dist/multirom.exe`; the firmware binary is unchanged.
- Renamed `nextor/kernel/` to `resources/`, which now holds both `Nextor-2.1.4.SunriseIDE.MasterOnly.ROM` and Konamiman's `Nextor-3.0.0-beta2.SunriseIDE.MasterOnly.ROM` (128 KB, SunriseIDE-Nextor-driver `v0.1.8-blueMSX-v0.1.5-Nextor-3.0-beta.2` release, same ROM as the 2350 projects and 2040 LoadROM v2.64). `tool/Makefile` embeds them from there as `tool/src/nextor_sunrise.h` (symbol now `___resources_Nextor_2_1_4_...`) and the new `tool/src/nextor3.h`, and cleans both headers. Removed the unused `NEXTOR_DIR` variable from the top-level Makefile.
- Added the `-s3` / `--sunrise3` and `-m3` / `--mapper3` tool options for testing Nextor 3, following the 2350 MultiROM/Explorer `-s3` implementation. They add Nextor 3.0.0 Beta 2 Sunrise IDE entries on the existing Sunrise (10) and Sunrise + 192KB mapper (11) types, so the RP2040 firmware is unchanged; the beta 2 driver sends the same ATA commands as 2.1.4 and the firmware already boots each entry from its own record offset. Booting beta 2 on hardware is not yet validated.
- The Nextor menu entries now show the Nextor version: `Nextor 2.1.4 Sunrise IDE` (`-s`), `Nextor 2.1.4 Sunrise IDE + 192KB Mapper` (`-m`), `Nextor 3.0.0 Beta 2 Sunrise IDE` (`-s3`) and `Nextor 3.0.0 Beta 2 Sunrise IDE + 192KB Mapper` (`-m3`). Longer names scroll in the menu as before.
- Tool: the two hand-written Nextor blocks were replaced by a `nextor_entries[]` table in which each row carries its own kernel pointer and size, so `-s`, `-m`, `-s3` and `-m3` can be combined freely (in that order at the top of the menu) and Nextor versions can be mixed in one UF2. The tool refuses to build if an embedded kernel is not exactly 128 KB. The `-h` help text lists the new options.
- Checked `-s -m -s3 -m3`, `-s -m3` with a scanned ROM, and a ROM-only UF2: every record has the expected name, type, size and offset, each Nextor payload matches its 2.1.4 or 3.0.0 beta 2 kernel, and the scanned ROM follows the Nextor kernels.
- openMSX add-on: no script change needed beyond the version; the generated Sunrise IDE extension uses the kernel cached from the selected entry, so 2.1.4 and 3.0.0 entries each boot their own Nextor.
- Docs: 2040 MultiROM tool manual (options, entry-name table, example, known issues), 2040 openMSX notes and `openmsx/README.md` (entry names), 2040 Sunrise Nextor implementation notes (embedded kernels, names for both tools) and the public README.

## PicoVerse 2040 Multirom v2.66

- Version bumped to v2.66 (top-level and tool Makefiles).
- `../tools/gen_romdb.py` now downloads the latest database by default from Vampier's ROM DB (`https://romdb.vampier.net/Archive/xml-msxromsdb.zip`) and reads `softwaredb.xml` from the zip in memory, so a local openMSX install is no longer needed. A local `softwaredb.xml` or `.zip` can still be passed as an argument for offline use. The generated header now records the source and the database timestamp. The script also accepts the `ASCII16-X` and lowercase `konami` mapper names used by this database.
- Regenerated `tool/src/romdb.h` from the 2026-10-03 database: 3115 → 3218 entries (103 added, none removed or changed), including 31 ASCII16-X ROMs (mapper 12) that the old DB never matched. Rebuilt the MSX menu, firmware and `tool/dist/multirom.exe`.
- openMSX add-on: `picoverse2040.tcl` now only takes over `.uf2` files inserted as ROM images when they are RP2040 images (family `0xE48BFF56`, or no family ID), via the new `is_rp2040_uf2()`. PicoVerse 2350 Explorer UF2s are left to the new 2350 add-on (`2350/software/explorer.pio/openmsx`), so both add-ons can be installed together. Script version bumped to v2.66 to match the project, so the `make openmsx` version check passes again.

## PicoVerse 2040 Multirom v2.65

- Version bumped to v2.65 (top-level and tool Makefiles, and the `picoverse2040.tcl` script version).
- The openMSX add-on files (`openmsx/share/scripts/picoverse2040.tcl`, `openmsx/share/extensions/PicoVerse_2040/hardwareconfig.xml` and `openmsx/src/placeholder.asm`) are now licensed under the GNU GPL v2 or later instead of CC BY-NC-SA 4.0, so they can be contributed to openMSX (submitted as a draft PR to openMSX/openMSX). Only the license header changed; the rest of the project keeps its license.- Fixed the MSX menu ROM list being two columns left of the header and footer on machines that boot with a narrower SCREEN 0 width, such as the Philips VG-8020 (WIDTH 37). The BIOS centers its text window when `LINLEN` < 40, but the list rows are written straight to VRAM at column 0. `displayMenu()` now finds the column where the BIOS printed the header (`detect_menu_col_offset()`), and `blit_row_vram()` adds that offset and writes only up to the end of the line.
- The MSX menu help key is now F1 instead of H. `main()` clears the BIOS function key strings and sets F1's `FNKSTR` entry to `MENU_KEY_F1_HELP` (0x01), the same technique used by the 2350 Explorer. The footer shows `[F1 - Help]` and the help screen was updated.
- Letter keys (A-Z, case-insensitive) now jump to the first ROM whose name starts with that letter (`find_first_by_letter()`), switching page when needed. Updated the MultiROM manual, `msx-picoverse-2040-menu.md` and the public readme.

## PicoVerse 2040 Multirom v2.64

- Version bumped to v2.64 (top-level and tool Makefiles, and the `picoverse2040.tcl` script version).
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
