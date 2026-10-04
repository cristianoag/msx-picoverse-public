# MSX PicoVerse 2040 — MultiROM in openMSX

This document describes how to run a PicoVerse 2040 MultiROM UF2 in the [openMSX](https://openmsx.org/) emulator, as if the cartridge were plugged into the emulated MSX. It covers installation, choosing the UF2, using the menu, attaching the emulated USB drive to the Nextor entries, the console commands and settings, and how the implementation works.

The openMSX support is part of the MultiROM project and lives in [2040/software/multirom.pio/openmsx](../2040/software/multirom.pio/openmsx). It is versioned and built together with the rest of the MultiROM project (v2.64 and later).

## 1. Overview

You point openMSX at the same `.uf2` file you would flash to the Pico. The emulated MSX then boots the PicoVerse MultiROM menu and lists every entry in that UF2:

- the game and application ROMs, with their PicoVerse mappers;
- the embedded **Nextor Sunrise IDE** entry (UF2 built with `-s`) or **Nextor Sunrise IDE + 192KB Mapper** entry (UF2 built with `-m`), with a disk image as the USB drive.

The Raspberry Pi Pico is not emulated. An openMSX Tcl script does the Pico firmware's job instead, and a normal openMSX extension makes the cartridge appear in the openMSX menus. You don't need to change or rebuild openMSX.

Typical uses:

- try a MultiROM UF2 before flashing it;
- check that every ROM in a compilation starts with the mapper the tool detected;
- prepare and test a Nextor USB drive (as a disk image) without the cartridge.

## 2. Requirements

- openMSX 20.0 or newer (tested with 20.0 and 21.0). It needs the `NEO-8`, `NEO-16`, `ASCII16-X` and `Manbow2` ROM types.
- A MultiROM UF2 built with the PicoVerse 2040 MultiROM tool (`multirom.exe`), see the [MultiROM tool manual](./msx-picoverse-2040-multirom-tool-manual.en-us.md).
- Any MSX1, MSX2, MSX2+ or turbo R machine. The bundled C-BIOS machines are enough for plain ROMs. The Nextor entries need a machine with a real MSX BIOS (MSX-BASIC) and its system ROMs installed in openMSX.

## 3. Files

| File | Purpose |
| --- | --- |
| [openmsx/share/scripts/picoverse2040.tcl](../2040/software/multirom.pio/openmsx/share/scripts/picoverse2040.tcl) | The openMSX script. It turns a UF2 into the PicoVerse cartridge and adds the `picoverse2040` console command and three settings. |
| [openmsx/share/extensions/PicoVerse_2040/](../2040/software/multirom.pio/openmsx/share/extensions/PicoVerse_2040/hardwareconfig.xml) | The **PicoVerse 2040 MultiROM** extension shown in the openMSX extension lists. |
| [openmsx/src/placeholder.asm](../2040/software/multirom.pio/openmsx/src/placeholder.asm) | Source of the extension's placeholder ROM (`picoverse2040.rom`). |
| [openmsx/Makefile](../2040/software/multirom.pio/openmsx/Makefile) | Builds the placeholder ROM. Called by the MultiROM aggregate Makefile. |
| [openmsx/install.ps1](../2040/software/multirom.pio/openmsx/install.ps1) / [openmsx/install.sh](../2040/software/multirom.pio/openmsx/install.sh) | Install the script and the extension for the current user. |

The `openmsx/share` folder uses the same layout as the openMSX `share` folder. openMSX loads every `*.tcl` file in `share/scripts` at startup and lists every extension in `share/extensions`. An extension XML can only describe hardware; it can't read a UF2 or react to the menu. So the extension holds a small placeholder ROM, and the script replaces it with the menu of your UF2 when you insert it.

The PicoVerse 2350 Explorer add-on ([MSX PicoVerse 2350 — Explorer in openMSX](./msx-picoverse-2350-openmsx.md)) can be installed next to this one: `picoverse2040.tcl` only takes over RP2040 UF2s, and leaves RP2350 (Explorer) UF2s to `picoverse2350.tcl`.

## 4. Building

The openMSX add-on is built by the MultiROM aggregate Makefile, together with the MSX menu, the Pico firmware and the tool:

```sh
cd 2040/software/multirom.pio
make            # msx -> pico -> tool, and openmsx
make openmsx    # only the openMSX add-on
```

The `openmsx` stage assembles `src/placeholder.asm` with SDCC (`sdasz80`, `sdldz80`, `hex2bin`) into `share/extensions/PicoVerse_2040/picoverse2040.rom`. The aggregate Makefile passes `VERSION`, and the stage fails if the `variable version` line in `picoverse2040.tcl` doesn't match it. Bump both when the MultiROM version changes. Running `make` inside `openmsx/` without `VERSION` skips the check.

The placeholder ROM is committed, so you only need SDCC if you change `placeholder.asm`.

## 5. Installation

Run the installer for your system. It copies `share/scripts/picoverse2040.tcl` and `share/extensions/PicoVerse_2040` into the `share` folder of your openMSX user directory:

```powershell
# Windows (installs to Documents\openMSX\share, or to $env:OPENMSX_HOME\share)
powershell -ExecutionPolicy Bypass -File 2040\software\multirom.pio\openmsx\install.ps1
```

```sh
# Linux / macOS (installs to ~/.openMSX/share, or to $OPENMSX_HOME/share)
sh 2040/software/multirom.pio/openmsx/install.sh
```

Both installers also accept the openMSX user directory as an argument (`install.ps1 -OpenMSXHome <dir>`, `install.sh <dir>`).

Restart openMSX afterwards. **PicoVerse 2040 MultiROM** then shows up in the extension lists, and `help picoverse2040` works in the console (F10). Run the installer again after updating the repository to install the new script.

## 6. Quick start

1. Start openMSX with any MSX machine.
2. Open **Media > Cartridge Slot A** and choose **ROM image**.
3. Browse to your `.uf2` file. Switch the file type filter to **All files (\*)**, because openMSX only lists ROM files by default.
4. Insert it. The MSX resets and shows the PicoVerse MultiROM menu.
5. Choose an entry with the cursor keys and press Enter, as on real hardware.

Reset the MSX to go back to the menu. From now on, inserting the **PicoVerse 2040 MultiROM** extension loads the same UF2.

## 7. Choosing the UF2

The script needs to know which UF2 to use. You can give it in any of these ways; each one also stores the UF2 in the `picoverse2040_uf2` setting, so the extension reloads it later.

### 7.1 As a cartridge ROM image (GUI)

Open **Media > Cartridge Slot A** (or B), choose **ROM image**, switch the filter to **All files (\*)** and select the `.uf2`. openMSX inserts the file as a ROM; the script notices the `.uf2` in the slot within a quarter of a second, ejects it, inserts the PicoVerse cartridge in the same slot and resets the MSX. The UF2 also shows up in openMSX's recent cartridge list.

### 7.2 With the PicoVerse 2040 MultiROM extension

Insert **PicoVerse 2040 MultiROM** from the extensions list of the Media menu, or select it with the **Extension** option of a Cartridge Slot window. The script replaces the extension with the menu of the UF2 stored in `picoverse2040_uf2` (the last UF2 used) in the same slot.

If no UF2 was chosen yet, the placeholder ROM stays and the MSX screen shows how to select one, and openMSX shows a warning message.

### 7.3 From the command line

Start openMSX with the UF2 as a cartridge, or with the extension to load the remembered UF2:

```text
openmsx -machine Philips_NMS_8245 -carta C:/PicoVerse/multirom.uf2
openmsx -machine Philips_NMS_8245 -ext PicoVerse_2040
```

### 7.4 From the console

Open the console (F10) and run:

```text
picoverse2040 insert C:/PicoVerse/multirom.uf2
picoverse2040 insert C:/PicoVerse/multirom.uf2 -slot b
```

`picoverse2040 insert` without a file inserts the last UF2 again. The same works with `carta C:/PicoVerse/multirom.uf2`, which goes through the take-over described in 7.1.

### 7.5 With the setting

```text
set picoverse2040_uf2 C:/PicoVerse/multirom.uf2
```

This only selects the UF2 used by the next insertion of the extension (or by `picoverse2040 insert` without a file). openMSX saves the setting with its other settings.

> **Windows paths in the console:** the console is a Tcl shell, where a backslash is an escape character. `C:\temp\multirom.uf2` reaches the script as `C:<TAB>empmultirom.uf2`. Use forward slashes (`C:/temp/multirom.uf2`) or put the path in braces (`{C:\temp\multirom.uf2}`). Tab completion inserts forward slashes. File dialogs and the command line are not affected. When a path is not found, the script explains this in its error message.

### 7.6 Rebuilt UF2 files

The script extracts the menu and the selected ROMs to a cache folder named after the UF2 file, its size and its modification time. The UF2 is read when it is inserted, so after rebuilding it, insert it again (`picoverse2040 insert`, the extension, or the ROM image) to use the new contents; a reset keeps the UF2 that is already loaded. Caches of older builds of the same UF2 are deleted.

## 8. Using the menu

The menu is the real PicoVerse MultiROM menu ROM from the UF2, so it looks and works as on the cartridge: cursor keys to move, left/right to change page, Enter to start an entry.

When you start an entry, the script inserts it with the matching openMSX mapper and resets the MSX, which then boots the entry. After that:

| Action | Result |
| --- | --- |
| **Reset** (Machine menu, hotkey or `reset` command) | The menu comes back. |
| **Power off and on** | The menu comes back. |
| **Restart openMSX** with the entry still in the slot (for example a setup saved at exit and restored at startup) | The menu of the last UF2 comes back. |
| **Insert the PicoVerse 2040 MultiROM extension again** | The menu comes back. |
| **Load a savestate** | The entry that was running in the savestate keeps running. The next reset brings the menu back. |
| `picoverse2040 menu` | The menu comes back. |
| `picoverse2040 boot <n>` | Entry `<n>` starts without going through the menu. |

Only the reset that the script issues to start the selected entry keeps the entry. Every other reset or power-up returns to the menu.

## 9. Emulated USB storage (Nextor entries)

On the cartridge, the Nextor entries use a USB thumb drive connected to the Pico's USB-C port. In openMSX, a **hard disk image file** takes its place. It is attached as the master disk of an emulated Sunrise IDE interface, and Nextor sees it as the `PicoVerse USB drive`.

The image is only attached while a Nextor entry is running; the menu and the game ROMs don't use it.

### 9.1 Which image is used

The script picks the image when a Nextor entry boots, in this order:

1. the `-hd` option of the last `picoverse2040 insert` command;
2. the `picoverse2040_hd` setting;
3. the default image `persistent/picoverse2040/hd.dsk` in the openMSX user directory (`Documents\openMSX` on Windows, `~/.openMSX` on Linux and macOS).

`picoverse2040 info` shows the image that the next Nextor boot will use. If the image file doesn't exist, openMSX creates a blank (unformatted) 100MB image there.

### 9.2 Attaching an image

To use your own image for every Nextor boot, set the setting (openMSX saves it):

```text
set picoverse2040_hd C:/PicoVerse/usb.dsk
```

The setting is read every time a Nextor entry boots. If a Nextor entry is running, reset the MSX and start the Nextor entry again from the menu.

To use an image only for the current session, give it when inserting the UF2. It stays active until the next insertion:

```text
picoverse2040 insert C:/PicoVerse/multirom.uf2 -hd C:/PicoVerse/test.dsk
```

When the Nextor entry is running, the `hda` console command shows the attached image.

### 9.3 Creating and filling an image

The easiest way is the openMSX `diskmanipulator` command, which creates Nextor-formatted images and copies files between the PC and the image. Run these commands in the console, for example while the PicoVerse menu is on screen (no Nextor entry running):

```text
diskmanipulator create C:/PicoVerse/usb.dsk -nextor 100M
virtual_drive C:/PicoVerse/usb.dsk
diskmanipulator import virtual_drive C:/PicoVerse/files/
diskmanipulator dir virtual_drive
virtual_drive eject
set picoverse2040_hd C:/PicoVerse/usb.dsk
```

- `create ... -nextor 100M` creates a 100MB image with a single Nextor FAT16 volume (FAT16 is used above 32MB; the maximum is 4GB).
- `virtual_drive` attaches the image to openMSX's virtual drive, which `diskmanipulator` can work on without any emulated disk interface.
- `import` copies a file, or every file and subfolder of a PC folder, to the image. Use `mkdir` and `chdir` to create and select MSX folders first, and `export virtual_drive <PC folder>` to copy files back to the PC.
- `virtual_drive eject` releases the image before Nextor uses it.

To create a partitioned image, give several sizes: `diskmanipulator create C:/PicoVerse/usb.dsk -nextor 100M 32M`. The partitions are then addressed as `virtual_drive1`, `virtual_drive2`, and so on (`diskmanipulator import virtual_drive1 C:/PicoVerse/files/`).

> **Long file names:** Nextor uses 8.3 file names, and `diskmanipulator import` shortens longer names by truncating them: `Depeche Mode - Strangelove.mp3` becomes `DEPECHE_.MP3`. When two names in a folder shorten to the same 8.3 name, only the first file is imported and the console shows `Warning: preserving entry <name>`. For folders with long names, copy them first to a staging folder with unique 8.3 names (for example `DEPECH~1.MP3`, `DEPECH~2.MP3`, as Windows does), and import the staging folder.

Change the image only while no Nextor entry is running. Nextor caches disk sectors, so files imported while it runs may not show up, and the MSX could overwrite them.

Alternatively, let openMSX create the blank default image, boot the Nextor entry, and partition it with `CALL FDISK` from Nextor BASIC, as you would with a new thumb drive. You can also use a raw image of a FAT16 thumb drive (4GB or smaller) taken with a disk imaging tool.

### 9.4 Nextor system files

Without system files on the image, Nextor boots into Nextor BASIC (`Nextor BASIC version 2.10`). `CALL DRVINFO` lists the drives; the image is the drive assigned to `Sunrise IDE`, normally `A:`. To boot into the Nextor command prompt, copy `NEXTOR.SYS` and `COMMAND2.COM` to the root of the image, as for a real thumb drive. See [How to prepare a thumb drive for Nextor](./msx-picoverse-2040-multirom-tool-manual.en-us.md#how-to-prepare-a-thumb-drive-for-nextor) for the download links, ROM launchers such as SofaRun, and the RAM requirements.

The **Nextor Sunrise IDE + 192KB Mapper** entry adds a 192KB memory mapper, as on the cartridge, so Nextor also works on machines with less than 128KB of RAM.

## 10. Console command reference

| Command | Description |
| --- | --- |
| `picoverse2040 insert <file.uf2> [-slot a\|b] [-hd <image>]` | Insert the cartridge built into the UF2 and reset the MSX. `-slot` picks the cartridge slot (default: setting `picoverse2040_slot`). `-hd` picks the USB drive image of the Nextor entries until the next insert. |
| `picoverse2040 insert` | Insert the last UF2 again (setting `picoverse2040_uf2`). |
| `picoverse2040 <file.uf2>` | Same as `picoverse2040 insert <file.uf2>`. |
| `picoverse2040 menu` | Go back to the menu (same as resetting the MSX). |
| `picoverse2040 boot <n>` | Skip the menu and start entry `<n>`. |
| `picoverse2040 list` | List the UF2 entries with mapper and size. `*` marks the running one. |
| `picoverse2040 info` | Show the UF2, slot, state, USB drive image and cache folder. Same as `picoverse2040` without arguments. |
| `picoverse2040 eject` | Remove the cartridge. |

`help picoverse2040` shows the same information in the console. Sub-commands, slots, files and entry numbers can be tab completed.

## 11. Settings

These are normal openMSX settings. Change them in the console with `set <name> <value>`; openMSX saves them with its other settings.

| Setting | Default | Description |
| --- | --- | --- |
| `picoverse2040_uf2` | *(empty)* | UF2 loaded by the **PicoVerse 2040 MultiROM** extension. Updated every time you insert a UF2. |
| `picoverse2040_slot` | `a` | Cartridge slot used by `picoverse2040 insert` when `-slot` is not given. |
| `picoverse2040_hd` | *(empty)* | USB drive image of the Nextor entries, read every time a Nextor entry boots. If empty, `persistent/picoverse2040/hd.dsk` in the openMSX user directory is used. |

## 12. How it works

### 12.1 Insertion and slot watching

openMSX has no Tcl event for inserting a cartridge, so the script checks the external cartridge slots four times a second (`machine_info external_slot`). It takes over a slot when it holds:

- a `.uf2` file inserted as a ROM image;
- the `PicoVerse_2040` extension;
- a ROM from the script's cache or a Nextor configuration left over from an earlier session (for example restored from an openMSX setup at startup).

Taking over means ejecting what is in the slot, loading the UF2 and inserting the menu in the same slot, then resetting the MSX.

Extensions restored from a setup or savestate are not always reported in their cartridge slot, so the script also looks for the `PicoVerse_2040` extension and its own Nextor configurations in `list_extensions`. It then uses the slot in the `picoverse2040_slot` setting.

### 12.2 UF2 decoding

The UF2 blocks are put back together into the Pico flash image. Blocks marked as not-for-flash, blocks for other chip families (the RP2040 family ID is `0xE48BFF56`) and blocks below the flash base `0x10000000` are skipped; gaps are filled with `0xFF`.

### 12.3 Finding the menu

The MultiROM tool lays out flash as:

```text
[firmware][menu ROM 16KB][config area 16KB][ROM payloads...]
```

All config offsets are relative to the end of the firmware (`__flash_binary_end`), and the firmware size changes from build to build. The script therefore looks, in the first 4MB, for the menu's `AB` header with an INIT address in `0x4000-0xBFFF`, followed by a config area whose first record has a printable name, a known mapper code and points to offset `0x8000`.

Each config record is 59 bytes: name (50 bytes), mapper code (1 byte), size (4 bytes) and offset (4 bytes), up to 128 records. A record of 59 `0xFF` bytes ends the list.

### 12.4 Menu and selection

The 32KB menu and config area are inserted as a `Page12` cartridge (`0x4000-0xBFFF`), exactly as `loadrom_msx_menu()` serves them on the cartridge.

When you pick an entry, the menu writes its index to the ROM select register at `0x9D81` (`ROM_SELECT_REGISTER` in `msx/src/menu.h`) and reboots. A write watchpoint on `0x9D81` catches that write; it only fires while page 2 has the PicoVerse slot (and sub-slot) selected. Outside the CPU emulation loop, the script extracts the entry, inserts it with the matching openMSX mapper and resets the MSX.

### 12.5 Mapper mapping

| PicoVerse code | Menu label | openMSX ROM type | Notes |
| --- | --- | --- | --- |
| 1 | PLA-16 | `Page12` | Padded with `0xFF` to 32KB. The firmware returns `0xFF` above the ROM size. |
| 2 | PLA-32 | `Page12` | |
| 3 | KonSCC | `KonamiSCC` | |
| 4 | PLN-48 | `Page012` | Padded to 48KB. |
| 5 | ASC-08 | `ASCII8` | |
| 6 | ASC-16 | `ASCII16` | |
| 7 | Konami | `Konami` | |
| 8 | NEO-8 | `NEO-8` | |
| 9 | NEO-16 | `NEO-16` | |
| 10 | SYSTEM | generated `PicoVerse2040_Nextor` configuration | Nextor Sunrise IDE. |
| 11 | SYSTEM | generated `PicoVerse2040_Nextor_Mapper` configuration | Nextor Sunrise IDE plus a 192KB memory mapper. |
| 12 | ASC16X | `ASCII16-X` | |
| 13 | PLN-64 | `Page0123` | Padded to 64KB. |
| 14 | MANBW2 | `Manbow2` | |

Banked ROMs are padded to a whole number of banks.

### 12.6 Nextor entries

The Nextor entries are not separate cartridges: like on the hardware, they are options of the PicoVerse menu. openMSX can only build a Sunrise IDE and a memory mapper from an extension configuration, though, so when a Nextor entry boots, the script writes a standard openMSX extension configuration and inserts it into the PicoVerse slot:

- **Nextor Sunrise IDE** (mapper code 10): a `SunriseIDE` device with the Nextor ROM taken from the UF2 and an `IDEHD` master disk.
- **Nextor Sunrise IDE + 192KB Mapper** (mapper code 11): an expanded slot, like `loadrom_sunrise_mapper()` on the cartridge. Sub-slot 0 holds the `SunriseIDE` device and sub-slot 1 a 192KB `MemoryMapper`.

The `IDEHD` master disk is the USB drive image (section 9), with a default size of 100MB when openMSX has to create it.

openMSX lists every configuration in `share/extensions` in its extension menus, so the script keeps these internal configurations out of that folder. It writes them to `persistent/picoverse2040/extensions/PicoVerse2040_Nextor.xml` (or `PicoVerse2040_Nextor_Mapper.xml`) in the openMSX user directory. openMSX resolves an extension name as `share/extensions/<name>.xml`, so the script inserts them with the relative name `../../persistent/picoverse2040/extensions/<name>`. That is also the name that `list_extensions`, setups and savestates record, so they reload the configuration from there. The configurations are rewritten each time a Nextor entry boots. Only **PicoVerse 2040 MultiROM** appears in the extension lists. Earlier versions of the script wrote these configurations to `share/extensions`; the script deletes those generated copies when openMSX starts.

### 12.7 Returning to the menu

The script listens to the openMSX boot event (`after boot`), which fires on every reset and power-up, and to the `power` setting. The reset it issues to start the selected entry is flagged and ignored. Any other boot while an entry is running puts the menu back in the slot and resets the MSX again. If the entry was restored from an earlier session, the script first loads the UF2 in `picoverse2040_uf2`.

While a reverse/replay history is being replayed, boot events are ignored, so replays run as they were recorded.

### 12.8 Machine switches and savestates

After a machine switch or a savestate load, the script looks at the cartridge slots and extensions of the new machine. If the menu ROM is there, it re-arms the `0x9D81` watchpoint. If an extracted ROM or a Nextor configuration is there, it keeps it running and treats it as the selected entry, so the next reset brings the menu back.

### 12.9 Cache files

The menu and the selected ROMs are extracted to `persistent/picoverse2040/cache/<uf2 name>_<size>_<mtime>/` in the openMSX user directory, for example `007_Circus Charlie - Konami (1984) _RC-712_.rom`. openMSX's Tcl has no `zlib`, so the folder is keyed by the UF2 size and modification time, and existing files are compared before reuse. Files that openMSX still keeps open are written under a new name instead of being replaced.

## 13. Limitations

- The emulation is functional, not cycle-exact. openMSX uses its own mapper implementations, so Pico-specific timing such as `/WAIT` stretching is not reproduced.
- openMSX's `KonamiSCC` and `Manbow2` ROM types include the SCC sound chip. The PicoVerse 2040 hardware does not output SCC audio.
- Savestates and replays refer to the extracted files in the cache folder. Keep that folder if you want to reload them.
- Machine switches drop the cartridge, the same as openMSX does for normal cartridges. Insert the **PicoVerse 2040 MultiROM** extension again, or run `picoverse2040 insert`.
- When a `.uf2` is inserted as a ROM image, openMSX briefly runs it as a plain ROM (for up to a quarter of a second) before the script takes over the slot and resets the MSX.
- The USB drive is emulated as an IDE hard disk image; USB-specific behaviour of real thumb drives is not reproduced.

## 14. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| The extension shows "No UF2 image selected yet" | No UF2 was chosen yet. Choose the `.uf2` as a ROM image, or run `picoverse2040 insert <file.uf2>`. |
| The extension keeps showing the placeholder text after a UF2 was chosen | `picoverse2040.tcl` is not in `share/scripts` of the openMSX user directory. Run the installer and restart openMSX. |
| `file not found: C:<TAB>emp...` | Backslashes in the console. Use forward slashes or braces (section 7). |
| `no PicoVerse 2040 MultiROM menu found in the UF2 image` | The UF2 is not a MultiROM image (for example a LoadROM or Explorer UF2). |
| A game shows a black screen or crashes | Check the mapper with `picoverse2040 list`. Force the right mapper with a tag in the ROM file name when building the UF2 (see the [MultiROM tool manual](./msx-picoverse-2040-multirom-tool-manual.en-us.md)). |
| Nextor shows no files | The image is blank or was changed while Nextor was running. Fill it as in section 9.3 and start the Nextor entry again. |
| A Nextor entry doesn't start on a C-BIOS machine | Nextor needs a real MSX BIOS with MSX-BASIC. Use such a machine and install its system ROMs in openMSX. |
