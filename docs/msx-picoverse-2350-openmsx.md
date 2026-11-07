# MSX PicoVerse 2350 — Explorer in openMSX

This document describes how to run a PicoVerse 2350 Explorer UF2 in the [openMSX](https://openmsx.org/) emulator, as if the cartridge were plugged into the emulated MSX. It covers installation, choosing the UF2 and the emulated microSD card, the Explorer features that work in the emulator, the Nextor, `.DSK` and audio options, the console commands and settings, and how the implementation works.

The openMSX support is part of the Explorer project and lives in [2350/software/explorer.pio/openmsx](../2350/software/explorer.pio/openmsx). It is versioned and built together with the rest of the Explorer project (v2.57 and later), and it runs the UF2s of Explorer v2.56 and later.

## 1. Overview

You point openMSX at the same `.uf2` file you would flash to the PicoVerse 2350. The emulated MSX then boots the real Explorer menu of that UF2 and behaves like the cartridge:

- **Flash list (F1):** the ROMs and SYSTEM entries in the UF2 (Nextor Sunrise 2.1.4 and 3.0.0, Nextor + 1MB mapper, Nextor + MegaRAM, standalone MegaRAM).
- **microSD list (F2):** a folder on your PC plays the microSD card. Its folders, `.ROM`, `.DSK`, `.MP3` and `.WAV` files are listed like on the cartridge.
- **Search, paging, 40/80 columns, help and the ROM detail screen** with mapper detection and the per-ROM options (mapper, audio profile, PSG mirror, volume, 1MB mapper for `.DSK`), saved to `.PVC` files on the emulated microSD card.
- **Copy to flash (F), rename (R) and delete (D)**, including the `Wait NN%` progress of the copy. Flash changes are kept in an editable copy of the flash image, so they survive openMSX restarts, like the real flash.
- **File Hunter (F3):** browse the online catalog of ROM and DSK files (`T` switches between them, `/` searches) and download them to the emulated microSD folder, with the real `Download: NN%` progress. The PC's network connection replaces the ESP-01 WiFi module.
- **Last selection:** the menu reopens on the entry you ran last (`PICOVERSE.PVL`).
- **Starting entries** with the matching openMSX hardware: the game mappers, the audio profiles (SCC/SCC+ in the game, external SCC/SCC+, MSX-MUSIC, Yamaha SFG-05/SFG-01, Dual PSG), the Nextor entries with a hard disk image, and `.DSK` images through the hidden Nextor 2.1.4 kernel of the UF2.

The Raspberry Pi Pico 2 is not emulated. An openMSX Tcl script does the Explorer firmware's job instead: it serves the menu ROM and answers the menu's commands in the same memory window the Pico uses. A normal openMSX extension makes the cartridge appear in the openMSX menus. You don't need to change or rebuild openMSX.

Explorer v2.61's optional **PicoAmp** (`-pa`) entry can be listed, including in a PicoAmp-only UF2, but cannot run in this add-on. Launching it reports that the RP2350 audio engine is not emulated; the add-on does not substitute a plain ROM mapper or Nextor device.

Typical uses:

- try an Explorer UF2 before flashing it;
- record videos and screenshots of the Explorer features;
- check the mappers the tool and the microSD detector pick;
- prepare a microSD folder (ROMs, `.DSK` images, `.PVC` options) before copying it to the card.

## 2. Requirements

- openMSX 20.0 or newer (tested with 21.0). It needs the `NEO-8`, `NEO-16`, `ASCII16-X` and `Manbow2` ROM types, the `YamahaSFG` device and the `sha1sum` command.
- The optional **SN76489 Franky** profile additionally requires an openMSX build with the `Franky` device; older releases may not provide it. There is no silent port-only fallback.
- An Explorer UF2 built with the PicoVerse 2350 Explorer tool (`explorer.exe`) v2.56 or later, see the [Explorer tool manual](./msx-picoverse-2350-explorer-tool-manual.en-us.md).
- An MSX1, MSX2, MSX2+ or turbo R machine with a real MSX BIOS (MSX-BASIC) and its system ROMs installed in openMSX. The FM-PAC, SFG and Nextor ROMs come from the UF2, so no extra system ROMs are needed for them.
- Python 3, only to regenerate the ROM database after the firmware's `romdb.h` changes (see section 4).

## 3. Files

| File | Purpose |
| --- | --- |
| [openmsx/share/scripts/picoverse2350.tcl](../2350/software/explorer.pio/openmsx/share/scripts/picoverse2350.tcl) | The openMSX script. It plays the Explorer firmware and adds the `picoverse2350` console command and four settings. |
| [openmsx/share/extensions/PicoVerse_2350/hardwareconfig.xml](../2350/software/explorer.pio/openmsx/share/extensions/PicoVerse_2350/hardwareconfig.xml) | The **PicoVerse 2350 Explorer** extension shown in the openMSX extension lists: the 32KB cartridge window the script serves. |
| [openmsx/share/extensions/PicoVerse_2350/romdb.txt](../2350/software/explorer.pio/openmsx/share/extensions/PicoVerse_2350/romdb.txt) | SHA1 → mapper table used to detect microSD ROMs, generated from the firmware's `pico/explorer/romdb.h`. |
| [openmsx/gen_romdb.py](../2350/software/explorer.pio/openmsx/gen_romdb.py) | Generates `romdb.txt`. |
| [openmsx/share/extensions/PicoVerse_2350/help.txt](../2350/software/explorer.pio/openmsx/share/extensions/PicoVerse_2350/help.txt) | The F5 help pages served to the menu, generated from the firmware's `pico/explorer/help_text.h`. |
| [openmsx/gen_help.py](../2350/software/explorer.pio/openmsx/gen_help.py) | Generates `help.txt`. |
| [openmsx/Makefile](../2350/software/explorer.pio/openmsx/Makefile) | Regenerates `romdb.txt` and `help.txt` and checks the script version. Called by the Explorer aggregate Makefile. |
| [openmsx/install.ps1](../2350/software/explorer.pio/openmsx/install.ps1) / [openmsx/install.sh](../2350/software/explorer.pio/openmsx/install.sh) | Install the script and the extension for the current user. |

The `openmsx/share` folder uses the same layout as the openMSX `share` folder. openMSX loads every `*.tcl` file in `share/scripts` at startup and lists every extension in `share/extensions`.

The PicoVerse 2040 MultiROM add-on ([MSX PicoVerse 2040 — MultiROM in openMSX](./msx-picoverse-2040-openmsx.md)) can be installed next to this one. Each script only takes over the UF2s of its own board (RP2040 or RP2350).

## 4. Building

The openMSX add-on is built by the Explorer aggregate Makefile, together with the MSX menu, the Pico firmware and the tool:

```sh
cd 2350/software/explorer.pio
make            # msx -> pico -> tool, and openmsx
make openmsx    # only the openMSX add-on
```

The `openmsx` stage regenerates `share/extensions/PicoVerse_2350/romdb.txt` with `gen_romdb.py` when `pico/explorer/romdb.h` is newer (set `PYTHON` if `python` is not your Python 3). The aggregate Makefile passes `VERSION`, and the stage fails if the `variable version` line in `picoverse2350.tcl` doesn't match it. Bump both when the Explorer version changes. Running `make` inside `openmsx/` without `VERSION` skips the check.

`romdb.txt` and `help.txt` are committed, so you only need Python when the ROM database or the help text changes.

## 5. Installation

Run the installer for your system. It copies `share/scripts/picoverse2350.tcl` and `share/extensions/PicoVerse_2350` into the `share` folder of your openMSX user directory:

```powershell
# Windows (installs to Documents\openMSX\share, or to $env:OPENMSX_HOME\share)
powershell -ExecutionPolicy Bypass -File 2350\software\explorer.pio\openmsx\install.ps1
```

```sh
# Linux / macOS (installs to ~/.openMSX/share, or to $OPENMSX_HOME/share)
sh 2350/software/explorer.pio/openmsx/install.sh
```

Both installers also accept the openMSX user directory as an argument (`install.ps1 -OpenMSXHome <dir>`, `install.sh <dir>`).

Restart openMSX afterwards. **PicoVerse 2350 Explorer** then shows up in the extension lists, and `help picoverse2350` works in the console (F10). Run the installer again after updating the repository.

## 6. Quick start

1. Start openMSX with any MSX machine.
2. Open the console (F10) and choose the folder that plays the microSD card:
   ```
   set picoverse2350_sd C:/MSX/SD
   ```
   Without it, the script uses an empty `picoverse2350/sd` folder in the openMSX persistent folder.
3. Open **Media > Cartridge Slot A** and choose **ROM image**. Browse to your Explorer `.uf2` file (switch the file type filter to **All files (\*)**, because openMSX only lists ROM files by default) and insert it.
4. The MSX resets and shows the Explorer menu. Use it as on the cartridge: cursor keys, F1 (flash), F2 (microSD), Enter (detail screen), Space (run), `/` (search), F5 (help).

The menu evenly distributes the gaps between F1-F5 options on line 23, with the selected source highlighted and one inverted space on each side of its label. Detail screens replace them with relevant controls and F5 Help; the 80-column ROM/player hints include more detailed option/action descriptions. F4 WiFi is available on lists and Help, not directly on detail screens. Line 24 centers its complete text and separates each item with ` | `: `Page XX/YY | Available: X.X MB | message` for flash/microSD, or the File Hunter ROM/DSK catalog and network state. Longer messages keep the page counter while temporarily replacing source information. Search input remains left-aligned on line 24; Esc restores the centered status.

The F2 summary shows the free space of the PC volume that holds the microSD folder, as the cartridge does for the card: megabytes to one decimal place, rounded down (for example `Available: 161245.3 MB`). The script asks the operating system (`dir` on Windows, `df -Pk` elsewhere) and caches the answer for a few seconds; if the volume can't be read, the summary falls back to `Available: unknown`.

Help retains all F1-F5 options with F5 highlighted and only the centered chip ID on line 24. Its source/configuration keys are active even when Help was opened from a detail screen. The compact 40-column bar stays inside the usable 38 columns.

Reset the MSX to go back to the menu. From now on, inserting the **PicoVerse 2350 Explorer** extension loads the same UF2.

## 7. Choosing the UF2

### 7.1 As a cartridge ROM image (GUI)

Select the `.uf2` as the ROM image of a cartridge slot (**Media > Cartridge Slot A/B > ROM image**). The script notices the UF2 within a fraction of a second, replaces it with the PicoVerse 2350 cartridge in the same slot and resets the MSX.

### 7.2 With the PicoVerse 2350 Explorer extension

Insert **PicoVerse 2350 Explorer** from **Media > Extensions > Insert** (or a Cartridge Slot window). It loads the UF2 in setting `picoverse2350_uf2`, which always holds the last UF2 used. Without a UF2, the MSX shows how to select one.

### 7.3 From the command line

```sh
openmsx -machine Philips_NMS_8250 -carta explorer.uf2
openmsx -machine Philips_NMS_8250 -ext PicoVerse_2350
```

### 7.4 From the console

```
picoverse2350 insert C:/MSX/explorer.uf2 -sd C:/MSX/SD -hd C:/MSX/nextor.dsk
```

In the openMSX console, backslashes are escape characters: use forward slashes, or put the path between braces (`{C:\MSX\explorer.uf2}`).

### 7.5 Rebuilt UF2 files and the emulated flash

The first time a UF2 is used, the script decodes it and keeps an editable copy of the flash in `persistent/picoverse2350/flash/<name>_<size>_<date>.img`. Copies, renames and deletes made from the menu change that copy, never the UF2. A rebuilt UF2 (other size or date) starts from a fresh copy, like flashing the cartridge again. `picoverse2350 reflash` discards the changes and starts again from the UF2.

## 8. The emulated microSD card

A folder on your PC plays the microSD card. It is chosen, in this order, by the `-sd` option of the last `picoverse2350 insert`, setting `picoverse2350_sd`, or the default `persistent/picoverse2350/sd` folder of your openMSX user directory.

Like the firmware, the menu lists:

- folders (except `System Volume Information`), sorted by name, with `..` in subfolders;
- `.ROM` files from 8KB up to the 32-bit menu size limit, with the mapper from a filename tag (`Game.Konami.ROM`) or, when needed, from the detector (SHA1 database, then heuristics, the same as the firmware);
- `.DSK` files whose size is a multiple of 360KB, up to 4MB;
- `.MP3` and `.WAV` files (listed, but not played, see section 13);
- in the root folder, the flash entries too (only shown in the combined list).

The script writes to that folder exactly like the firmware writes to the card, so keep a copy of anything you care about:

| File | Written when |
| --- | --- |
| `<rom>.PVC`, `<image>.DSK.PVC` | The options of a microSD ROM or `.DSK` are saved. |
| `/<flash entry>.flash.PVC` | The options of a flash entry are saved; renamed, copied and deleted together with the entry. |
| `/PICOVERSE.PVL` | An entry is started (last selection). |

Renaming (R) and deleting (D) a microSD entry renames or deletes the file in the folder, with its `.PVC`.

Selecting a ROM above 4MB with Enter or Space immediately offers the menu's line-24 `Y/N` flash-copy confirmation, before mapper detection or the options screen, just like the cartridge. After confirmation, an identical, same-name installed image is reused or a new persistent `F1` entry is created. Explorer switches to `F1` and opens that exact flash entry's ROM options screen, even when sorting places it on another page. Choose the options and select Run to execute; copying alone does not boot the ROM, and Esc keeps the copy and returns to the flash list. New copies inherit the SD options, while reused entries retain their existing flash options; edits and last selection use the actual flash identity. No suitable contiguous block or a full list reports the cleanup message; deleting flash entries with `D` makes the freed space reusable. The centered basic `F1` status is `Page XX/YY | Available: X.X MB` in both column modes; contiguous-space checks remain internal.

### 8.1 File Hunter (F3): download ROMs and DSKs

Press **F3** to browse [File Hunter](https://msxpico.file-hunter.com), the online catalog the cartridge reaches through its ESP-01 WiFi module. In openMSX the same requests are made with your PC's network connection, so there is no WiFi or serial emulation and nothing to configure: if the PC is online, the menu shows `Net: Online`.

- The list opens on the **latest releases**. **T** switches between the **ROM** and **DSK** catalogs (the line-24 status shows `Listing: ROM` or `Listing: DSK`), **/** searches by name (the search is kept when you press **T**), and the cursor keys, paging and 40/80 column keys work as on the cartridge.
- **Enter** or **Space** opens the detail screen and **Enter** downloads the file. The menu shows `Download: NN%` while the data arrives (the emulated MSX keeps running), then `Saved to microSD. Press key.`
- The file is saved in the **root of the emulated microSD folder** (section 8) as `<name>.ROM` or `<name>.DSK`, like on the card: characters Windows and FAT don't allow become `_`, trailing spaces and dots are dropped, and an existing file with the same name is replaced only once the new download is complete (it is written as `<name>.part` first). The file gets the date File Hunter reports for it (`Last-Modified`, else the server's clock). Press **F2** to see it, and run it from there. The `Available` space of the F2 list is updated.
- A `.DSK` must be a 360KB-multiple image of at most 4MB to be accepted, like the cartridge.
- A failed or interrupted download leaves no file and reports the reason on the status line, using the firmware's messages where they exist: `Download failed: ...` (`timeout`, `closed`, `network`, `HTTP 404`, `invalid size`, `incomplete`, `no metadata`), `Download exceeds file size`, `Bad DSK size`, `microSD full` and `No microSD card`. When File Hunter can't be reached, the list shows `Offline or File Hunter unavailable` and `Net: Offline`.
- Resetting the MSX or switching machine in the middle of a download cancels it and deletes the partial file.

Only plain `http://` is supported (File Hunter's own address works; `https` redirects are not). The script uses openMSX's non-blocking sockets from realtime events, so the emulator does not freeze while downloading; run openMSX at normal speed (`set throttle on`, the default) if you want the progress to be visible, as a fast download can finish in less than a second.

## 9. Copy to flash, rename and delete

These v2.57 features work as on the cartridge:

- **F** in the microSD list copies the selected ROM to flash after `To flash? Y/N`. The menu shows `Wait NN%` while the copy runs (about 200KB/s of emulated time, like the real flash), and the ROM then appears in the F1 list. The script applies the firmware rules: no folders, MP3/WAV, `.DSK`, SYSTEM mappers or duplicate names; at most 128 flash entries; the image goes to the lowest free 4KB-aligned gap after the hidden payloads; the entry is appended to the next free config slot, and the config area is compacted when all 204 slots have been used. The ROM's `.PVC` is copied to `/<name>.flash.PVC`.
- **R** renames a microSD ROM/`.DSK` (the folder, mapper tag and extension are kept) or a flash entry (appended under the new name, then the old slot is tombstoned). The list reopens on the renamed entry.
- **D** deletes a microSD file (with its `.PVC`) or a flash entry (its slot is tombstoned).

## 10. Starting entries

| Entry | openMSX hardware |
| --- | --- |
| Game ROM, no extra audio | The ROM in the cartridge slot with the openMSX mapper of the PicoVerse mapper code (`Page12`, `Page012`, `Page0123`, `KonamiSCC`, `Konami`, `ASCII8`, `ASCII16`, `NEO-8`, `NEO-16`, `ASCII16-X`, `Manbow2`). Planar ROMs are padded with 0xFF to the window the firmware serves. |
| Game ROM + External SCC / SCC+ | Expanded slot: the ROM in subslot 0, an SCC cartridge or an SCC+ (expanded mode) in subslot 2. |
| Game ROM + MSX-MUSIC | Expanded slot: the ROM in subslot 0, an FM-PAC with the FM-PAC BIOS of the UF2 in subslot 3. |
| Game ROM + YM2151 SFG-05 / SFG-01 | Expanded slot: the ROM in subslot 0, a Yamaha SFG (YM2164 or YM2151) with the SFG BIOS of the UF2 in subslot 2. |
| Game ROM + Dual PSG | A second PSG on ports 0x10-0x13, next to the ROM. |
| Game ROM or supported Nextor/DSK + SN76489 Franky | A native openMSX Franky device on sound/counter ports 0x48/0x49 and video ports 0x88/0x89. The hardware firmware exposes only the limited counter/control behavior needed by sound drivers, not a full Sega VDP. |
| Nextor Sunrise 2.1.4 / 3.0.0 (SD or USB) | Sunrise IDE with the entry's own Nextor kernel and the hard disk image (section 11). |
| Nextor + 1MB Mapper | Expanded slot: Sunrise IDE in subslot 0, a 1MB memory mapper in subslot 1. |
| Nextor + 1MB Mapper + 1MB MegaRAM | As above, plus a 1MB MegaRAM (port 0x8E) in subslot 3. |
| Nextor + 1MB Mapper + C2 RAM | Started as Nextor + 1MB mapper; the Carnivore2 RAM registers are not emulated. |
| Brazilian MegaRAM (1MB) | A 1MB MegaRAM in the cartridge slot; the MSX boots to BASIC with it. |
| `.DSK` image | Sunrise IDE with the hidden Nextor 2.1.4 kernel of the UF2 and the `.DSK` file itself as the disk, plus the 1MB mapper when **1MB Mapper** is **Yes**. The DSK audio profiles (External SCC/SCC+, MSX-MUSIC, SFG-05/SFG-01, Dual PSG) are added as above. Writes go straight to the `.DSK` file, like the firmware's write-through. |

The audio profile follows the firmware's `resolve_audio_mode()`: the option saved in the `.PVC` or chosen on the detail screen, with the same restrictions per mapper. The SCC/SCC+ profiles of Konami SCC and Manbow 2 games use the SCC built into the openMSX mapper. The 4 MHz SFG variants use the standard SFG clock.

The SN profile keeps saved ID 13 (Franky), without changing older IDs or the PVC file size. Former MMM selections (ID 14) are unsupported and do not create an audio device or mapper. Franky preserves the selected Nextor/DSK memory layout. See the [SN76489 instructions](./msx-picoverse-2350-explorer-tool-manual.en-us.md#sn76489-music-and-games) for entry restrictions and the VGMPlay workflow. openMSX supplies its own sound/device implementation; it does not validate Pico synthesis performance or PIO bus timing.

The selected entry is started like the firmware does it: the script swaps the cartridge and resets the MSX. Any later reset, a power cycle, or restarting openMSX with the entry still in the slot brings back the menu, which reopens on the last entry.

## 11. Hard disk image of the Nextor entries

On the cartridge, Nextor SD entries can expose either a primary FAT16 partition or the whole microSD card through `SD Part: WHOLE DISK`; USB entries use a USB drive. In openMSX, both use a hard disk image, chosen in this order: the `-hd` option of the last `picoverse2350 insert`, setting `picoverse2350_hd` (read every time a Nextor entry boots), or `persistent/picoverse2350/hd.dsk` (created empty, 100MB, the first time).

```
set picoverse2350_hd C:/MSX/nextor.dsk
```

A new empty image needs a partition: in Nextor BASIC, `CALL FDISK`. To create an image and copy a PC folder into it, use the openMSX `diskmanipulator`, as described in [MSX PicoVerse 2040 — MultiROM in openMSX](./msx-picoverse-2040-openmsx.md) (section 9.3); the steps are the same.

The microSD folder (section 8) and the Nextor image are separate: the menu does not see the files inside the image, and Nextor does not see the folder.

The script models the `SD Part` label/`WHOLE DISK` choices and saves/restores the selection in the existing `.PVC` file. It still presents the entire hard disk image to Nextor in either case; real-card partition-window isolation and multi-partition Explorer browsing are not simulated.

## 12. Console command and settings

```
picoverse2350 insert <file.uf2> [-slot <a|b>] [-sd <folder>] [-hd <image>]
picoverse2350 insert            re-insert the last UF2
picoverse2350 menu              back to the menu (same as a reset)
picoverse2350 list              list the flash entries
picoverse2350 info              current UF2, flash copy, slot, microSD folder, HD image, state
picoverse2350 reflash           discard the flash changes, start again from the UF2
picoverse2350 eject             remove the cartridge
```

| Setting | Meaning |
| --- | --- |
| `picoverse2350_uf2` | Last UF2 used, loaded by the extension. |
| `picoverse2350_slot` | Default cartridge slot (`a`). |
| `picoverse2350_sd` | Folder used as the microSD card (empty: `persistent/picoverse2350/sd`). |
| `picoverse2350_hd` | Hard disk image of the Nextor entries (empty: `persistent/picoverse2350/hd.dsk`). |

## 13. Limitations

- **MP3/WAV playback** is not emulated: the files are listed and the player reports an error.
- **WiFi setup (F4)** is not emulated: it resets to the menu. File Hunter (F3) works through the PC's network instead of the ESP-01 module, see section 8.1; only `http://` is supported.
- **VDP frequency (50/60 Hz) and CPU mode** options are saved, but not applied to the emulated MSX. Use the openMSX settings instead. The audio volume and PSG mirror options have no effect either.
- **Multi-disk `.DSK` images** (several disks joined into one file) boot the first disk only; the GRAPH + key disk switching is not emulated.
- **ASC16X-FR** ROMs run as ASCII16-X; FlashROM saves (`.FLA`) are not emulated.
- **Carnivore2** entries start as Nextor + 1MB mapper (section 10).
- **WaveGame** assets are not detected.
- The Nextor SD entries use a hard disk image, not the microSD folder (section 11), and only one partition is offered.
- Bus-level behaviour of the RP2350 (PIO timing, `/WAIT`) is not emulated: firmware fixes at that level can only be tested on hardware.

## 14. How it works

### 14.1 The cartridge window

The PicoVerse_2350 extension maps a 32KB RAM device at 0x4000-0xBFFF. The script loads the menu ROM of the UF2 into it and serves the Pico control area of `msx/src/menu.h` there: the page buffer at 0xB900-0xBF7F, the ROM select register at 0xBF7F, the partition info / status text at 0xBF80, `CTRL_VDP_FREQ`..`CTRL_DISK_ACT` at 0xBFA0, the 16-bit free/largest flash-capacity words at 0xBFA4/0xBFA6, the chip ID at 0xBFAF, the query buffer at 0xBFC0, the MP3 registers at 0xBFE0 and the control registers at 0xBFF0.

A write watchpoint on 0x4000-0xBFFF (only while the cartridge is selected in that page) runs `handle_menu_write_explorer()`'s logic for every MSX write: query and rename buffers, commands (`CTRL_CMD`), page requests, option registers and the ROM select register. The script answers synchronously, then puts back the byte the Pico serves at the written address. The MSX therefore never sees RAM in the window, which matters because the BIOS RAM probe at boot writes to the top of page 2. Writes made by BIOS code (PC below 0x4000) are only restored, never executed as commands.

The commands follow the firmware: the paged record list (`PVEX` header, record table, string pool, TLV records), the filter and find-first search, source switching, folders, mapper detection and override, `.PVC` load/save, quick run, last selection, partition cycling, delete, copy to flash, copy/reuse and select the flash ROM (`0x10`) and rename. Copy progress is simulated in 0.1 s steps, not used as a cartridge speed benchmark. The image is written and verified first; mapper validation then uses the copied flash bytes, with `CTRL_MAPPER = 0x7F` showing `Mapper...`. Saved/selected mapper overrides and filename tags still take precedence; unknown images use the SHA1/database and heuristic detector through a temporary copy of the flash bytes, removed after detection. Unsupported mappers leave no new entry. Command `0x10` stays busy until the refreshed flash list is ready, then reports the exact selected entry through `CTRL_MATCH` with `CTRL_ACK = 0xA5`; it does not write the ROM-select register or execute a reset.

### 14.2 UF2 decoding and the flash copy

The UF2 blocks of the RP2350 families are assembled into a flat flash image. The menu ROM is the first 4KB boundary after the firmware since v2.57 (`FIRMWARE_ALIGN`), and right after it before, so the script checks the aligned positions first and then searches for the `AB` header followed by a valid config area. The image from the menu ROM on is saved as the editable flash copy. Record offsets are relative to the menu ROM, as in the firmware (`flash_rom`).

### 14.3 Starting an entry

When the menu writes the ROM select register, the script takes the record (using the 16-bit index the menu just stored as last selection, so lists longer than 255 entries work), resolves the audio profile, writes an openMSX extension for it to `persistent/picoverse2350/extensions/PicoVerse2350_Boot.xml` (outside `share/extensions`, so openMSX does not list it), inserts it, or the plain ROM, in the cartridge slot and resets the MSX. ROM files and the FM-PAC, SFG and Nextor ROMs extracted from the UF2 are cached in `persistent/picoverse2350/cache`.

### 14.4 Returning to the menu, machine switches and savestates

Any reset, except the one issued to start an entry, a power cycle, or restarting openMSX with an entry in the slot (restored setup) brings back the menu. After a machine switch or a savestate load, the script re-attaches to the menu if it is in a slot, or lets the running entry continue until the next reset.

### 14.5 File Hunter

The File Hunter commands of the menu (`CMD_FH_SEARCH` 0x42, `CMD_FH_LIST_PAGE` 0x40, `CMD_FH_DOWNLOAD` 0x41 and `CMD_FH_WIFI_STATUS` 0x43) are answered like `handle_menu_fh_command()` does. While the File Hunter window is active the control registers change meaning: `CTRL_COUNT` is the number of results, `CTRL_MAPPER`/`CTRL_ACK` the download progress, `CTRL_AUDIO` the result (0 = failed, 1 = ok), 0xBF80 holds the status text shown after a download, and the page buffer holds 74-byte records (name, flags, size in KB). Leaving File Hunter (F1/F2/Esc) returns the window to the normal lists.

The requests are the firmware's: `/new-pico.php?base=1BA0&type=<rom|dsk>&download=` for the latest releases and `/picoverse.php?base=1BA0&type=<rom|dsk>&msx=&char=<query>&download=` for a search, with `&download=<index>` to download an entry. The listing is a packed table (a 4-byte name pointer, the size in KB and a flag, ending with a zero pointer, followed by the names; at most 384 results and 256KB); a download starts with a metadata line (`type:,start:,size:<bytes>,name:<name>`) followed by the file, which is saved to a `.part` file and renamed when the size matches.

The HTTP client (`http_get` and friends) is a small state machine over a non-blocking socket polled every 20ms of real time: connect (10s timeout), headers, redirects (up to 4), and `Content-Length`, chunked or until-close bodies, with the firmware's idle timeouts (3s for a listing, 20s for a download). Because the polling runs from realtime events and the command stays busy (`CTRL_CMD` is not cleared) until the transfer ends, the emulated MSX keeps running and polls the progress itself. A list-page request that follows a failed search is answered from the message row instead of connecting a second time. The ESP-01 serial protocol and WiFi setup are not involved.

## 15. Troubleshooting

| Problem | Solution |
| --- | --- |
| The extension is not in the list | Run the installer and restart openMSX. |
| The MSX shows "no UF2 selected" | Insert the `.uf2` as ROM image, or `picoverse2350 insert <file.uf2>`. |
| `File not found` with a Windows path in the console | Use forward slashes or braces around the path. |
| The microSD list says "No microSD card" | The microSD folder does not exist: check `picoverse2350 info` and setting `picoverse2350_sd`. |
| Copies/renames/deletes are gone | The UF2 was rebuilt (a new flash copy is used), or `picoverse2350 reflash` was run. |
| Nextor says it has no drive | The hard disk image has no partition: use `CALL FDISK`, or point `picoverse2350_hd` to a prepared image. |
| A 2040 UF2 is selected | Use the PicoVerse 2040 add-on (`picoverse2040 insert`). |
| File Hunter says `Offline or File Hunter unavailable` | The PC can't reach `msxpico.file-hunter.com` on port 80: check the internet connection, a firewall or proxy, and try again with **T** or **/**. |
| A File Hunter download fails with `HTTP` or `network` | The server answered with an error or the connection dropped; nothing is saved. Try again. |
| A downloaded file isn't in the F2 list | Press F2 again: downloads go to the root of the microSD folder (`picoverse2350 info` shows it). |
