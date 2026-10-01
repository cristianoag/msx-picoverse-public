# PicoVerse 2040 MultiROM for openMSX

Run a PicoVerse 2040 MultiROM UF2 in [openMSX](https://openmsx.org/) as if the cartridge were plugged into the emulated MSX. You point openMSX at the same `.uf2` file you would flash to the Pico. The MSX then boots the PicoVerse menu and shows every entry in that UF2: your ROMs, plus the Nextor Sunrise IDE entries if the UF2 was built with `-s` or `-m`.

The Raspberry Pi Pico is not emulated. An openMSX Tcl script does the Pico firmware's job instead, and a normal openMSX extension makes the cartridge appear in the openMSX menus. You don't need to change or rebuild openMSX.

## Contents

| File | Purpose |
| --- | --- |
| [share/scripts/picoverse2040.tcl](share/scripts/picoverse2040.tcl) | The openMSX script. It turns a UF2 into the PicoVerse cartridge and adds the `picoverse2040` console command and three settings. |
| [share/extensions/PicoVerse_2040/](share/extensions/PicoVerse_2040/hardwareconfig.xml) | The **PicoVerse 2040 MultiROM** extension shown in the openMSX Extensions menu. |
| [src/placeholder.asm](src/placeholder.asm) | Source of the extension's placeholder ROM. Build it with `make` (needs SDCC). |
| [install.ps1](install.ps1) / [install.sh](install.sh) | Install the script and the extension for the current user. |

The folder uses the same layout as the openMSX `share` folder. openMSX automatically loads every `*.tcl` file in `share/scripts` and lists every extension in `share/extensions`. An extension XML can only describe hardware; it can't read a UF2 or react to the menu. So the extension holds a small placeholder ROM, and the script replaces it with the menu of your UF2 when you insert it. The Nextor entries use extensions that the script generates when you boot them (see [Nextor entries](#nextor-entries)).

## Requirements

- openMSX 20.0 or newer (tested with 20.0). It needs the `NEO-8`, `NEO-16`, `ASCII16-X` and `Manbow2` ROM types.
- A MultiROM UF2 built with the PicoVerse 2040 MultiROM tool (`multirom.exe`, v2.63 layout).
- Any MSX1, MSX2, MSX2+ or turbo R machine. The bundled C-BIOS machines are enough for plain ROMs. Nextor entries need a machine with a real MSX BIOS (MSX-BASIC).

## Installation

Run the installer for your system. It copies `share/scripts/picoverse2040.tcl` and `share/extensions/PicoVerse_2040` into the `share` folder of your openMSX user directory:

```powershell
# Windows (installs to Documents\openMSX\share, or to $env:OPENMSX_HOME\share)
powershell -ExecutionPolicy Bypass -File install.ps1
```

```sh
# Linux / macOS (installs to ~/.openMSX/share, or to $OPENMSX_HOME/share)
sh install.sh
```

Restart openMSX afterwards. **PicoVerse 2040 MultiROM** then shows up in the Extensions menu, and `help picoverse2040` works in the console (F10).

## Usage

### From the openMSX menus

1. Open **Media > Cartridge Slot A** (or B) and choose **ROM image**.
2. Browse to your `.uf2` file. Switch the file type filter to **All files (\*)**, because openMSX only lists ROM files by default.
3. Insert it. The script sees the UF2 in the slot and replaces it with the PicoVerse cartridge. The MSX resets and shows the PicoVerse MultiROM menu.

Choose an entry with the cursor keys and press Enter, just as on real hardware.

The script remembers that UF2. After that, you can simply insert the **PicoVerse 2040 MultiROM** extension, from the Extensions menu or from the extension option in a Cartridge Slot window, and it loads the same UF2. If you insert the extension before any UF2 has been chosen, the MSX screen explains how to choose one. The UF2 also shows up in openMSX's recent cartridges list.

### From the command line

Start openMSX with the UF2 as a cartridge, or with the extension to load the remembered UF2:

```text
openmsx -machine Philips_NMS_8245 -carta C:/PicoVerse/multirom.uf2
openmsx -machine Philips_NMS_8245 -ext PicoVerse_2040
```

### From the console

Open the console (F10):

```text
picoverse2040 insert C:/PicoVerse/multirom.uf2
```

> **Windows paths:** the console is a Tcl shell, where a backslash is an escape character. `C:\temp\multirom.uf2` reaches the script as `C:<TAB>empmultirom.uf2`. Use forward slashes (`C:/temp/multirom.uf2`) or put the path in braces (`{C:\temp\multirom.uf2}`). Tab completion also inserts forward slashes. File dialogs and the command line are not affected.

| Command | Description |
| --- | --- |
| `picoverse2040 insert <file.uf2> [-slot a\|b] [-hd <image>]` | Insert the cartridge and reset the MSX. `-slot` picks the cartridge slot. `-hd` picks the disk image the Nextor entries use as the USB drive. |
| `picoverse2040 insert` | Insert the last UF2 again (setting `picoverse2040_uf2`). |
| `picoverse2040 menu` | Go back to the menu, like power cycling the MSX with the real cartridge. |
| `picoverse2040 boot <n>` | Skip the menu and boot entry `<n>`. |
| `picoverse2040 list` | List the UF2 entries. `*` marks the running one. |
| `picoverse2040 info` | Show the UF2, slot, state, disk image and cache folder. |
| `picoverse2040 eject` | Remove the cartridge. |

The cartridge behaves like the real one:

- **Reset** keeps the selected ROM running. On real hardware, the Pico keeps serving the ROM it selected.
- **Power off and on** brings the menu back. On real hardware, the Pico restarts with the MSX.

### Settings

These are normal openMSX settings. Change them in the console (for example `set picoverse2040_hd C:/PicoVerse/usb.dsk`). openMSX saves them with the rest of its settings.

| Setting | Default | Description |
| --- | --- | --- |
| `picoverse2040_uf2` | *(empty)* | UF2 loaded by the **PicoVerse 2040 MultiROM** extension. Updated automatically every time you insert a UF2. |
| `picoverse2040_slot` | `a` | Cartridge slot used by `picoverse2040 insert` when `-slot` is not given. |
| `picoverse2040_hd` | *(empty)* | Disk image for the Nextor entries. If empty, `persistent/picoverse2040/hd.dsk` in the openMSX user directory is used. |

## How it works

1. **Detection.** openMSX has no Tcl event for inserting a cartridge, so the script checks the cartridge slots four times a second (`machine_info external_slot`). When a slot holds a `.uf2` file or the `PicoVerse_2040` extension, the script takes over that slot.
2. **UF2 decoding.** The UF2 blocks are put back together into the Pico flash image. Blocks marked as not-for-flash and blocks for other chip families are skipped.
3. **Finding the menu.** The multirom tool lays out flash as `[firmware][menu ROM 16KB][config area 16KB][ROMs...]`. All config offsets are relative to the end of the firmware (`__flash_binary_end`). The firmware size changes from build to build, so the script looks for the menu's `AB` header followed by a valid config area whose first record points to offset `0x8000`.
4. **Menu.** The 32KB menu and config area are inserted as a `Page12` cartridge (`0x4000-0xBFFF`), exactly as `loadrom_msx_menu()` serves them.
5. **Selection.** When you pick an entry, the menu writes its index to the ROM select register at `0x9D81` (`ROM_SELECT_REGISTER` in `msx/src/menu.h`) and then reboots. A write watchpoint on `0x9D81` catches that write. It only fires while page 2 has the PicoVerse slot selected. The script then swaps in the selected ROM and resets the MSX.
6. **ROM files.** The selected ROM is extracted to `persistent/picoverse2040/cache/<uf2 name>_<size>_<mtime>/` in the openMSX user directory. When the UF2 changes, the extracted files are rebuilt automatically.

### Mapper mapping

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
| 10 | SYSTEM | generated `PicoVerse2040_Nextor` extension | Nextor Sunrise IDE. |
| 11 | SYSTEM | generated `PicoVerse2040_Nextor_Mapper` extension | Nextor Sunrise IDE plus a 192KB memory mapper. |
| 12 | ASC16X | `ASCII16-X` | |
| 13 | PLN-64 | `Page0123` | Padded to 64KB. |
| 14 | MANBW2 | `Manbow2` | |

### Nextor entries

openMSX only loads extensions from its `extensions` folders. When you boot a Nextor entry, the script therefore writes a standard openMSX extension to `share/extensions/PicoVerse2040_Nextor*/hardwareconfig.xml` in the user directory and inserts it into the PicoVerse slot:

- **Nextor Sunrise IDE** (`-s`): a `SunriseIDE` device that uses the Nextor ROM taken from the UF2.
- **Nextor Sunrise IDE + 192KB Mapper** (`-m`): the slot is expanded, like `loadrom_sunrise_mapper()` does. Sub-slot 0 holds the `SunriseIDE` device and sub-slot 1 a 192KB `MemoryMapper`.

The USB thumb drive becomes the IDE master hard disk image, which is the `-hd` option or the `picoverse2040_hd` setting. If the image doesn't exist, openMSX creates a blank 100MB one. Partition and format it with `CALL FDISK` from Nextor BASIC, then copy the Nextor system files onto it, the same as a thumb drive. See [the MultiROM manual](../../../../docs/msx-picoverse-2040-multirom-tool-manual.en-us.md#using-nextor-with-the-picoverse-2040-cartridge). You can also point `-hd` at an existing disk image.

The generated extensions are rewritten each time a Nextor entry boots, and they also show up in the Extensions menu. Don't edit or insert them yourself; insert **PicoVerse 2040 MultiROM** instead.

## Limitations

- The emulation is functional, not cycle-exact. openMSX uses its own mapper implementations, so Pico-specific timing such as `/WAIT` stretching is not reproduced.
- openMSX's `KonamiSCC` and `Manbow2` ROM types include the SCC sound chip. The PicoVerse 2040 hardware does not output SCC audio.
- Savestates and replays refer to the extracted files in the cache folder. Keep that folder if you want to reload them.
- Machine switches drop the cartridge, the same as openMSX does for normal cartridges. Insert the **PicoVerse 2040 MultiROM** extension again, or run `picoverse2040 insert`.
- When a `.uf2` is inserted as a ROM image, openMSX briefly runs it as a plain ROM (for up to a quarter of a second) before the script takes over the slot and resets the MSX.
