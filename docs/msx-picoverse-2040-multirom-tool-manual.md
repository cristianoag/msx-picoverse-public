# MSX PicoVerse 2040 Project

|PicoVerse Front|PicoVerse Back|
|---|---|
|![PicoVerse Front](/images/2025-12-02_20-05.png)|![PicoVerse Back](/images/2025-12-02_20-06.png)|


The PicoVerse 2040 is a Raspberry Pi Pico–based cartridge for MSX that uses replaceable firmware to extend the computer’s capabilities. By loading different firmware images, the cartridge can run MSX games and applications and emulate additional hardware devices (such as ROM mappers, extra RAM, or storage interfaces), effectively adding virtual peripherals to the MSX. One such firmware is the MultiROM system, which provides an on‑screen menu for browsing and launching multiple ROM titles stored in the cartridge.

The cartridge can also expose the Pico’s USB‑C port as a mass‑storage device, allowing you to copy ROMs, DSKs, and other files directly from a computer to the cartridge.

Additionally, you can use a Nextor firmware variant with +240 KB memory mapper support to take full advantage of the PicoVerse 2040 hardware on MSX systems with limited memory.

Those are the features available in the current version of the PicoVerse 2040 cartridge:

* MultiROM menu system for selecting and launching MSX ROMs.
* Nextor OS support with optional +240 KB memory mapper.
* USB mass-storage device support for loading ROMs and DSKs.
* Support for various MSX ROM mappers (PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16).
* Compatibility with MSX, MSX2, and MSX2+ systems.

## MultiROM UF2 Creator Manual

This section documents the `multirom` console tool used to generate UF2 images (`multirom.uf2`) that program the PicoVerse 2040 cartridge.

In it default behavior, the `multirom` tool scans MSX ROM files in a directory and packages them into a single UF2 image that can be flashed to the Raspberry Pi Pico. The resulting image typically contains the Pico firmware, the MultiROM MSX menu ROM, a configuration area describing each ROM entry, and the ROM payloads themselves.

Depending on the options provided, `multirom` can also produce UF2 images that boot directly into a custom firmware instead of the MultiROM menu. For example, options can be used to produce UF2 files with a firmware that implements the Nextor OS. In this mode, the Pico’s USB‑C port can be used as a mass‑storage device for loading ROMs, DSKs, and other files from Nextor (for example, via SofaRun).

## Overview

If executed without any options, the `multirom.exe` tool scans the current working directory for MSX ROM files (`.ROM` or `.rom`), analyses each ROM to guess the mapper type, builds a configuration table describing each ROM (name, mapper byte, size, offset) and embeds this table into an MSX menu ROM slice. The tool then concatenates the Pico firmware blob, the menu slice, the configuration area and the ROM payloads and serializes the whole image into a UF2 file named `multirom.uf2`.

![alt text](/images/2025-11-29_20-49.png)

The UF2 file (usually multirom.uf2) can then be copied to a Pico's USB mass storage device to flash the combined image. You need to connect the Pico while pushing the BOOTSEL button to enter UF2 flashing mode. Then a new USB drive named `RPI-RP2` appears, and you can copy `multirom.uf2` to it. After the copy completes, you can disconnect the Pico and insert the cartridge into your MSX to boot the MultiROM menu.

ROM file names are used to name the entries in the MSX menu. There is a limit of 50 characters per name. A rolling effect is used to show longer names on the MSX menu, but if the name exceeds 50 characters it will be truncated.

![alt text](/images/multirom_2040_menu.png)

If you want to use Nextor with your PicoVerse 2040 cartridge you need to run the tool with the `-s1` or `-s2` option to include the embedded Nextor ROM in the image. The `-s1` option includes the standard Nextor ROM without memory mapper support, while `-s2` includes a Nextor ROM with +240 KB memory mapper support. The embedded Nextor ROM will be the single firmwware loaded at boot, and the MultiROM menu will not be available. You can then use SofaRun to load ROMs and DSKs from the Pico's USB mass storage.

**Important Note:** To use a USB thumdrive you may need a OTG adapter or cable. That can be used to convert the USB-C port to a standard USB-A female port.

## Constants and limits
- Maximum number of ROM files processed: `MAX_ROM_FILES = 128`.
- ROM filename length used in config: `MAX_FILE_NAME_LENGTH = 50`.
- Number of ROMS displayed in menu: `MAX_MENU_ENTRIES = 19`.
- Minimum ROM size accepted: `MIN_ROM_SIZE = 8192` bytes (8 KB).
- Maximum ROM size accepted: `MAX_ROM_SIZE = 10*1024*1024` bytes.
- The tool uses a fixed `TARGET_FILE_SIZE = 32768` bytes for the combined MSX menu ROM + configuration area and reserves `MENU_COPY_SIZE = 16 * 1024` (16KB) of the menu ROM to copy unchanged before the configuration payload.

## Command-line usage

Only Microsoft Windows executables are provided for now (`multirom.exe`).

### Basic usage:

```
multirom.exe [options]
```

### Options:
- `-n`, `--nextor` : Includes the beta embedded NEXTOR ROM from the configuration and outputs. This option is still experimental and at this moment only works on specific MSX2 models.
- `-h`, `--help`   : Show usage help and exit.
- `-o <filename>`, `--output <filename>` : Set UF2 output filename (default is `multirom.uf2`).
- `-s1`            : Cartridge boots with Sunrise IDE Nextor (no memory mapper).
- `-s2`            : Cartridge boots with Sunrise IDE Nextor (+240 KB memory mapper).
- If neither `-s1` nor `-s2` is specified, the tool produces a MultiROM image with the menu.
- If you need to force a specific mapper type for a ROM file, you can append a mapper tag before the `.ROM` extension in the filename. The tag is case-insensitive. For example, naming a file `Knight Mare.PL-32.ROM` forces the use of the PL-32 mapper for that ROM. Tags like `SYSTEM` are ignored.

### Examples
- Produces the multirom.uf2 file with the MultiROM menu and all `.ROM` files in the current directory. You can run the tool using the command prompt or just by double-clicking the executable:
  ```
  multirom.exe
  ```

- Produces the multirom.uf2 file with the Sunrise IDE Nextor ROM (no memory mapper):
  ```
  multirom.exe -s1
  ```
- Produces the multirom.uf2 file with the Sunrise IDE Nextor ROM (+240 KB memory mapper):
  ```
  multirom.exe -s2
  ```

## How it works (high level)

1. The tool scans the current working directory for files ending with `.ROM` or `.rom`. For each file:
   - It extracts a display-name (filename without extension, truncated to 50 chars).
   - It obtains the file size and validates it is between `MIN_ROM_SIZE` and `MAX_ROM_SIZE`.
   - It calls `detect_rom_type()` to heuristically determine the mapper byte to use in the configuration entry. If a mapper tag is present in the filename, it overrides the detection.
   - If mapper detection fails, the file is skipped.
   - It serializes the per-ROM configuration record (50-byte name, 1-byte mapper, 4-byte size LE, 4-byte flash-offset LE) into the configuration area.
2. After scanning, the tool concatenates (in order): embedded Pico firmware binary, a leading slice of the MSX menu ROM (`MENU_COPY_SIZE` bytes), the full configuration area (`CONFIG_AREA_SIZE` bytes), optional NEXTOR ROM, and then the discovered ROM payloads in discovery order.
3. The combined payload is written as a UF2 file named `multirom.uf2` using `create_uf2_file()` which produces 256-byte payload UF2 blocks targeted to the Pico flash address `0x10000000`.

## Mapper detection heuristics
- `detect_rom_type()` implements a combination of signature checks ("AB" header, `ROM_NEO8` / `ROM_NE16` tags) and heuristic scanning of opcodes and addresses to pick common MSX mappers, including (but not limited to):
  - Plain 16KB (mapper byte 1) — 16KB AB header check
  - Plain 32KB (mapper byte 2) — 32KB AB header check
  - Linear0 mapper (mapper byte 4) — special AB layout check
  - NEO8 (mapper byte 8) and NEO16 (mapper byte 9)
  - Konami, Konami SCC, ASCII8, ASCII16 and others via weighted scoring
- If no mapper can be reliably detected, the tool skips the ROM and reports "unsupported mapper". Remember you can force a mapper via filename tag. The tags are case-insensitive and are listed below. 
- Only the following mappers are supported in the configuration area and menu: `PL-16,  PL-32,  KonSCC,  Linear,  ASC-08,  ASC-16,  Konami,  NEO-8,  NEO-16`

## Using the MSX ROM Selector menu

When you power on the MSX with the PicoVerse 2040 cartridge inserted, the MultiROM menu appears, showing the list of available ROMs. You can navigate the menu using the keyboard arrow keys.

Use the Up and Down arrow keys to move the selection cursor through the list of ROMs. If you have more than 19 ROMs, use the lateral arrow keys (Left and Right) to scroll through pages of entries.

Press the Enter or Space key to launch the selected ROM. The MSX will attempt to boot the ROM using the appropriate mapper settings.

At any time while in the menu, you can press H key read the help screen with basic instructions. Press any key to return to the main menu.

## Using Nextor with the PicoVerse 2040 cartridge

If you flashed the cartridge with a Nextor firmware (using the `-s1` or `-s2` options), the cartridge will boot directly into Nextor instead of the MultiROM menu.

You may need a USB OTG adapter or cable to connect standard USB-A thumb drives to the cartridge's USB-C port.

In MSX2 or MSX2+ systems with the +240 KB memory version of Nextor, the cartridge will add the extra memory to the system and the BIOS sequence will reflect the increased RAM. Some MSX models display the total expanded memory during boot.

### How to prepare a thumb drive for Nextor

To prepare a USB thumb drive for use with Nextor on the PicoVerse 2040 cartridge, follow these steps:

1. Connect the USB thumb drive to your PC.
2. Create a 4GB maximum FAT16 partition on the thumb drive. You can use built-in OS tools or third-party partitioning software to do this.
3. Copy the Nextor system files to the root directory of the FAT16 partition. You can obtain the Nextor system files from the official Nextor distribution or repository. You also need the COMMAND2 file for the command shell:
   1.  [Nextor Download Page](https://github.com/Konamiman/Nextor/releases) 
   2.  [Command2 Download Page](http://www.tni.nl/products/command2.html)
4. Copy any MSX ROMs (`.ROM` files) or disk images (`.DSK` files) you want to use with Nextor to the root directory of the thumb drive.
5. Install SofaRun or any other Nextor-compatible launcher on the thumb drive if you plan to use it for launching ROMs and DSKs. You can download SofaRun from its official source here: [SofaRun](https://www.louthrax.net/mgr/sofarun.html)
6. Safely eject the thumb drive from your PC.

## Improvements ideas
- Improve the ROM type detection heuristics to cover more mappers and edge cases.
- Implement configuration screen for each ROM entry (name, mapper override, etc).
- Add support for more ROM mappers.
- Implement a graphical menu with better navigation and ROM information display.
- Add support for saving/loading menu configuration to preserve user preferences.
- Support DSK files as well, with proper configuration entries.
- Improve mapper detection heuristics to cover more mappers.
- Add support for custom menu ROM slices or themes.
- Allow downloading ROMs from Internet URLs and embedding them directly.
- Wireless (wifi) network support via external ESP-01 module.
- Allow the use of the joystick to navigate the menu.
- Support for zipping/compressing ROMs to save space.

## Known issues

- The embedded Nextor ROM inclusion is still experimental and may not work on all MSX2 models.
- Some ROMs with uncommon mappers may not be detected correctly and will be skipped unless a valid mapper tag is used to force detection.
- The tool currently only supports Windows executables. Linux and macOS versions are not available yet.
- The tool does not currently validate the integrity of ROM files beyond size and basic header checks. Corrupted ROMs may lead to unexpected behavior.
- The MultiROM menu does not support DSK files; only ROM files are listed and launched.
- The tool does not currently support subdirectories; only ROM files in the current working directory are processed.
- The pico flash memory can wear out after many write cycles. Avoid excessive re-flashing.

## Tested MSX models

The PicoVerse 2040 cartridge with MultiROM firmware has been tested on the following MSX models:

| Model | Type | Status | Comments |
| --- | --- | --- | --- |
| Adermir Carchano Expert 4 | MSX2+ | OK | Verified operation |
| Gradiente Expert | MSX1 | OK | Verified operation |
| JFF MSX | MSX1 | OK | Verified operation |
| MSX Book | MSX2+ (FPGA clone) | OK | Verified operation |
| MSX One | MSX1 | Not OK | Cartridge not recognized |
| National FS-4500 | MSX1 | OK | Verified operation |
| Panasonic FS-A1GT | TurboR | OK | Verified operation |
| Panasonic FS-A1ST | TurboR | OK | Verified operation |
| Panasonic FS-A1WX | MSX2+ | OK | Verified operation |
| Panasonic FS-A1WSX | MSX2+ | OK | Verified operation |
| Sharp HotBit HB8000 | MSX1 | OK | Verified operation |
| SMX-HB | MSX2+ (FPGA clone) | OK | Verified operation |
| Sony HB-F1XD | MSX2 | OK | Verified operation |
| Sony HB-F1XDJ | MSX2 | OK | Verified operation |
| Sony HB-F1XV | MSX2+ | OK | Verified operation |
| TRHMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| uMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| Yamaha YIS604 | MSX1 | OK | Verified operation |

The PicoVerse 2040 cartridge with Sunrise Nextor +240K firmware has been tested on the following MSX models:

| Model | Status | Comments |
| --- | --- | --- |
|MSX1|||
| Sharp HotBit HB8000 (MSX1) | Not OK | Memory mapper not working then Nextor not functional |
|MSX2+|||
| TRHMSX (MSX2+, FPGA clone) | OK | Verified operation |
| uMSX (MSX2+, FPGA clone) | OK | Verified operation |



Author: Cristiano Almeida Goncalves
Last updated: December 2, 2025