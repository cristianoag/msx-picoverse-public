# Change Log

## PicoVerse 2350 Multirom v2.64
- Version bumped to v2.64 (top-level, MSX, and tool Makefiles).
- Renamed the `nextor/` folder to `resources/`: the Nextor 2.1.4 Sunrise IDE MasterOnly ROM moved from `nextor/kernel/` to `resources/` (via `git mv`) and the `kernel/` subfolder was removed. `tool/Makefile` now points `NEXTOR_SUNRISE` at `../resources/`, which renames the `xxd` embed symbol to `___resources_Nextor_2_1_4_SunriseIDE_MasterOnly_ROM`.
- Added the `-s3` / `--sunrise3-sd` tool option for testing Nextor 3, microSD only for now. It adds a `Nextor Sunrise 3.0.0 Beta 2 (SD)` SYSTEM entry that boots Konamiman's Nextor 3.0.0 beta 2 Sunrise IDE MasterOnly kernel (`resources/Nextor-3.0.0-beta2.SunriseIDE.MasterOnly.ROM`, 128 KB, SunriseIDE-Nextor-driver `v0.1.8-blueMSX-v0.1.5-Nextor-3.0-beta.2` release), embedded by `tool/Makefile` as `tool/src/nextor3.h`. The entry uses the existing Sunrise SD mapper (15), so the firmware and MSX menu are unchanged; the beta 2 driver sends the same ATA commands as 2.1.4. `-w` also applies to `-s3` (same loader as `-s1`), and the "-w requires" check now accepts it.
- Added `-a` / `--allnextor`, which enables every Nextor entry: `-s1`, `-m1`, `-s2`, `-m2`, `-c1`, `-c2` and `-s3`.
- Tool: as in Explorer, each Nextor entry now carries its own ROM pointer and size (`nextor_entry_rom[]`, sized by `MAX_NEXTOR_ENTRIES`), so 2.1.4 and 3.0 kernels can be mixed in one UF2. The tool refuses to build if the two embedded Nextor ROMs differ in size. Checked UF2s from `-a`, `-s3` and `-a -w`: every record points at the matching ROM bytes, and with `-w` the ESP8266P BIOS follows the `-s1`/`-m1`/`-s2`/`-m2`/`-s3` payloads only. Booting beta 2 on hardware is not yet validated.
- Docs: MultiROM tool manual (`-a`, `-s3`, `-w`, examples, mapper table, Nextor 3 section), public README and WiFi notes.
- The shared generator `../tools/gen_romdb.py` now downloads the latest database by default from Vampier's ROM DB (`https://romdb.vampier.net/Archive/xml-msxromsdb.zip`) and reads `softwaredb.xml` from the zip in memory, so a local openMSX install is no longer needed. A local `softwaredb.xml` or `.zip` can still be passed as an argument for offline use. The generated header now records the source and the database timestamp. The script also accepts the `ASCII16-X` and lowercase `konami` mapper names used by this database.
- Regenerated `tool/src/romdb.h` from the 2026-10-03 database: 3115 → 3218 entries (103 added, none removed or changed), including 31 ASCII16-X ROMs (mapper 12) that the old DB never matched. Rebuilt `tool/dist/multirom.exe`.
- Fixed `tool/Makefile` still defaulting to `VERSION ?= v2.63`; it now matches the top-level v2.64, so a standalone `make -C tool` stamps the correct version.
- The MSX menu help key is now F1 instead of H, as in PicoVerse 2040 MultiROM v2.65. `main()` clears the BIOS function key strings and sets F1's `FNKSTR` entry to `MENU_KEY_F1_HELP` (0x01, `msx/src/menu.h`), so `CHGET` returns it. The footer shows `[F1 - Help]` and the help screen was updated.
- Letter keys (A-Z, case-insensitive) now jump to the first ROM whose name starts with that letter (`find_first_by_letter()`), switching page when needed; other keys still do nothing. Rebuilt `msx/dist/menu.rom` (`_CODE` ends at 0x6086, below the 0x8000 record window) and `tool/dist/multirom.exe`. Updated the MultiROM tool manual.

## PicoVerse 2350 Multirom v2.63
- Version bumped to v2.63 (top-level, MSX, and tool Makefiles).
- Fixed banked mapper ROMs hanging when the game switches segments in bursts from MSX RAM without reading the cartridge in between. The bus loops blocked waiting for a read and let the 8-entry write FIFO overflow, silently dropping bank switches and leaving the mapper on a stale segment. All banked loops (Konami/Konami-SCC/ASCII8, Konami SCC with SCC audio, ASCII16, ASCII16-X, Neo-8, Neo-16) now drain writes while waiting for the next read, using a single `FSTAT` sample per idle iteration so the read-response latency is unchanged. Reported with "Go Figure v1.2"; same fix as PicoVerse 2350 Loadrom v2.70.

## PicoVerse 2350 Multirom v2.62
- Reorganized `pico/multirom` sources into per-type subfolders: `audio/` (`emu2212`), `memory/` (`c2_emu`, the Carnivore2-style mapper/RAM emulation), and `storage/` (`hw_config.c`, `sunrise_ide`, `sunrise_sd`), updating `CMakeLists.txt` and `multirom.c` includes accordingly. Verified with a full reconfigure and rebuild.
- Removed the unused `nextor.c`/`nextor.h` files, which were not referenced by `CMakeLists.txt` or included by any other source (dead code left over from an earlier Nextor bridge implementation superseded by `sunrise_ide.c`/`sunrise_sd.c`).
- Fixed the PC tool mapper-detection read path to reject truncated ROM reads before hashing or scanning the allocated ROM buffer.
- Version bumped to v2.62 (top-level, MSX, and tool Makefiles).

## PicoVerse 2350 Multirom v2.61
- Bumped the multirom.pio version to v2.61.
- Improved MSX MultiROM menu drawing by rendering ROM rows with a single VRAM block write, preserving long-name scrolling from the first visible character.
- Fixed short mapper labels in the optimized row renderer so they pad with spaces instead of leaking stray characters at the end of the line.
- Reduced page-navigation redraws so page changes update only the ROM rows and footer instead of repainting the static title and separator lines.
- Fixed the MultiROM tool mapper detector so 8KB ROMs do not read past the ROM buffer when building images with embedded Sunrise/mapper options.
- Frogger - Konami (1983) [RC-704] rom on the MSX1 Konami compilation had a bug and was replaced by a working dump from File Hunter.

## PicoVerse 2350 Multirom v2.59
- Refreshed the generated ROM mapper SHA1 database from the current openMSX `softwaredb.xml` and updated the shared generator to parse the new attribute-based XML format.

## PicoVerse 2350 Multirom v2.58

- Bumped the multirom.pio version to v2.58.
- Changed the memory-read `/WAIT` driver to open-drain behaviour: the firmware now asserts `/WAIT` by pulling it low and releases the shared line as hi-Z after stretched read cycles and ROM-cache setup.

## PicoVerse 2350 Multirom v2.57

- Updated the MSX MultiROM menu separator lines to use the same custom MSX glyph used by Explorer, copying character pattern `0x17` into printable slot `0x7E` and rendering that glyph instead of ASCII hyphens.
- Removed the unused MSX menu configuration screen and disabled the `C`/`c` key shortcut that opened it.
