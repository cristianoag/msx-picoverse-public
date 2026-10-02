# MSX PicoVerse 2040 Project

The PicoVerse 2040 is a Raspberry Pi Pico based cartridge for MSX that uses replaceable firmware to extend the computer’s capabilities. By loading different firmware images, the cartridge can run MSX games and applications and emulate additional hardware devices (such as ROM mappers, extra RAM, or storage interfaces), effectively adding virtual peripherals to the MSX. One such firmware is the MultiROM system, which provides an on‑screen menu for browsing and launching multiple ROM titles stored in the cartridge.

The cartridge can also expose the Pico’s USB‑C port as a mass‑storage device, allowing you to copy ROMs, DSKs, and other files directly from a PC with Windows or Linux to the cartridge.

Those are the features available in the current version of the PicoVerse 2040 cartridge:

* MultiROM menu system for selecting and launching MSX ROMs.
* Inline Menu option with Nextor OS support.
* USB mass-storage device support for loading ROMs and DSKs.
* Support for various MSX ROM mappers (PLA-16, PLA-32, KonSCC, PLN-48, PLN-64, ASC-08, ASC-16, ASC-16X, Konami, NEO-8, NEO-16, MANBW2).
* Compatibility with MSX, MSX2, and MSX2+ systems.

## MultiROM UF2 Creator Manual

This section documents the `multirom` console tool used to generate UF2 images (`multirom.uf2`) that program the PicoVerse 2040 cartridge.

In its default behavior, the `multirom` tool scans MSX ROM files in a directory and packages them into a single UF2 image that can be flashed to the Raspberry Pi Pico. The resulting image typically contains the Pico firmware, the MultiROM MSX menu ROM, a configuration area describing each ROM entry, and the ROM payloads themselves.

> **Note:** A maximum of 128 ROMs can be included in a single image, with a total size limit of aproximately 16 MB.

Depending on the options provided, `multirom` can also produce UF2 images that boot directly into a custom firmware instead of the MultiROM menu. For example, options can be used to produce UF2 files with a firmware that implements the Nextor OS. In this mode, the Pico’s USB‑C port can be used as a mass‑storage device for loading ROMs, DSKs, and other files from Nextor (for example, via SofaRun).

## Overview

If executed without any options, the `multirom.exe` tool scans the current working directory for MSX ROM files (`.ROM` or `.rom`), analyses each ROM to guess the mapper type, builds a configuration table describing each ROM (name, mapper byte, size, offset) and embeds this table into an MSX menu ROM slice. The tool then concatenates the Pico firmware blob, the menu slice, the configuration area and the ROM payloads and serializes the whole image into a UF2 file named `multirom.uf2`.

![alt text](/images/2025-11-29_20-49.png)

The UF2 file (usually multirom.uf2) can then be copied to a Pico's USB mass storage device to flash the combined image. You need to connect the Pico while pushing the BOOTSEL button to enter UF2 flashing mode. Then a new USB drive named `RPI-RP2` appears, and you can copy `multirom.uf2` to it. After the copy completes, you can disconnect the Pico and insert the cartridge into your MSX to boot the MultiROM menu.

ROM file names are used to name the entries in the MSX menu. There is a limit of 50 characters per name. A rolling effect is used to show longer names on the MSX menu, but if the name exceeds 50 characters it will be truncated.

![alt text](/images/multirom_2040_menu.png)


> **Note:** To use a USB thumdrive you may need a OTG adapter or cable. That can be used to convert the USB-C port to a standard USB-A female port.

> **Note:** The `-s` option includes Sunrise IDE Nextor, and the `-m` option includes Sunrise IDE Nextor plus an additional 192KB mapper implementation. Both provide USB mass-storage support via the cartridge's USB-C port and work on MSX1, MSX2, and MSX2+ systems.

## Command-line usage

Only Microsoft Windows executables are provided for now (`multirom.exe`).

### Basic usage:

```
multirom.exe [options]
```

### Options:
- `-s`, `--sunrise` : Includes the embedded Sunrise IDE Nextor ROM for USB mass-storage support. The cartridge's USB-C port can be used as a block device with Nextor-compatible loaders like SofaRun.
- `-m`, `--mapper` : Includes the embedded Sunrise IDE Nextor ROM with 192KB mapper support (12 x 16KB pages). In the MSX menu this entry is still shown with mapper text `SYSTEM`.
- `-h`, `--help`   : Show usage help and exit.
- `-o <filename>`, `--output <filename>` : Set UF2 output filename (default is `multirom.uf2`).
- If you need to force a specific mapper type for a ROM file, you can append a mapper tag before the `.ROM` extension in the filename. The tag is case-insensitive. For example, naming a file `Knight Mare.PLA-32.ROM` forces the use of the PLA-32 mapper for that ROM. `SYSTEM` tags are ignored. The list of current tags is: `PLA-16, PLA-32, KonSCC, PLN-48, ASC-08, ASC-16, ASC-16X, Konami, NEO-8, NEO-16, PLN-64, MANBW2`.

### Examples
- Produces the multirom.uf2 file with the MultiROM menu and all `.ROM` files in the current directory. You can run the tool using the command prompt or just by double-clicking the executable:
  ```
  multirom.exe
  ```
- Produces a multirom UF2 that also includes a menu entry for Sunrise IDE Nextor + 192KB Mapper:
  ```
  multirom.exe -m -o multirom_mapper.uf2
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
- `detect_rom_type()` implements a combination of signature checks ("AB" header, `ROM_NEO8` / `ROM_NE16` / `ASCII16X` tags) and heuristic scanning of opcodes and addresses to pick common MSX mappers, including (but not limited to):
  - Plain 16KB (mapper byte 1) — 16KB AB header check
  - Plain 32KB (mapper byte 2) — 32KB AB header check
  - Planar48 mapper (mapper byte 4, tag `PLN-48`) — special AB layout check
  - Planar64 mapper (mapper byte 13, tag `PLN-64`) — 64KB planar layout
  - NEO8 (mapper byte 8) and NEO16 (mapper byte 9) — signature-based detection
  - ASCII16-X (mapper byte 12, tag `ASC-16X`) — `ASCII16X` signature at ROM offset 16
  - Manbow2 (mapper byte 14, tag `MANBW2`) — 512 KB Manbow 2 signature check
  - Konami, Konami SCC, ASCII8, ASCII16 and others via unit-weight scoring aligned with openMSX's `guessRomType()` algorithm
- Before applying heuristics, the tool computes the SHA-1 hash of the ROM and looks it up in an embedded database derived from the openMSX `softwaredb.xml`. When a match is found the database mapper type is used directly, bypassing the heuristic scanner. This improves accuracy for titles whose mapper cannot be reliably distinguished by opcode analysis alone.
- If no mapper can be reliably detected, the tool skips the ROM and reports "unsupported mapper". Remember you can force a mapper via filename tag. The tags are case-insensitive and are listed below. 
- Only the following mapper tags are supported in the configuration area and menu: `PLA-16, PLA-32, KonSCC, PLN-48, ASC-08, ASC-16, ASC-16X, Konami, NEO-8, NEO-16, PLN-64, MANBW2`.

## Using the MSX ROM Selector menu

When you power on the MSX with the PicoVerse 2040 cartridge inserted, the MultiROM menu appears, showing the list of available ROMs. You can navigate the menu using the keyboard arrow keys.

![alt text](/images/multirom_2040_menu.png)

Use the Up and Down arrow keys to move the selection cursor through the list of ROMs. If you have more than 19 ROMs, use the lateral arrow keys (Left and Right) to scroll through pages of entries.

Press the Enter or Space key to launch the selected ROM. The MSX will attempt to boot the ROM using the appropriate mapper settings.

Press a letter key (A to Z) to jump straight to the first ROM whose name starts with that letter; the menu switches to the page that contains it.

At any time while in the menu, you can press the F1 key to read the help screen with basic instructions. Press any key to return to the main menu.

## Using Nextor with the PicoVerse 2040 cartridge

The `-s` option of the multirom tool includes Sunrise IDE Nextor ROM, and the `-m` option includes Sunrise IDE Nextor ROM plus an additional 192KB mapper implementation. When the multirom UF2 is flashed with either option, the Pico's USB-C port exposes a block device that Nextor-compatible loaders (such as SofaRun) can use to load ROMs, DSK files, and other media from a connected USB thumb drive.

The Sunrise IDE implementation uses ATA register emulation, making it compatible with a wide range of MSX systems including MSX1, MSX2, and MSX2+ models. 

### How to prepare a thumb drive for Nextor

To prepare a USB thumb drive for use with Nextor on the PicoVerse 2040 cartridge, follow these steps:

1. Connect the USB thumb drive to your PC.
2. Create a 4GB maximum FAT16 partition on the thumb drive. You can use built-in Nextor OS tool (CALL FDISK while in MSX Basic) or third-party partitioning software to do this.
3. Copy the Nextor system files to the root directory of the FAT16 partition. You can obtain the Nextor system files from the official Nextor distribution or repository. You also need the COMMAND2 file for the command shell:
   1.  [Nextor Download Page](https://github.com/Konamiman/Nextor/releases) 
   2.  [Command2 Download Page](http://www.tni.nl/products/command2.html)
4. Copy any MSX ROMs (`.ROM` files) or disk images (`.DSK` files) you want to use with Nextor to the root directory of the thumb drive.
5. Install SofaRun or any other Nextor-compatible launcher on the thumb drive if you plan to use it for launching ROMs and DSKs. You can download SofaRun from its official source here: [SofaRun](https://www.louthrax.net/mgr/sofarun.html)
6. Safely eject the thumb drive from your PC.
7. Connect the thumb drive to the PicoVerse 2040 cartridge using a USB OTG adapter or cable if necessary.

> **Note:** Not all USB thumb drives are compatible with the PicoVerse 2040 cartridge. If you encounter issues, try using a different brand or model of thumb drive.
>
> **Note:** Nextor needs a minimum of 128 KB of RAM to operate; PicoVerse 2040 mapper mode provides 192KB mapper RAM (12 x 16KB pages).

## Testing a MultiROM UF2 in openMSX

You can try a MultiROM UF2 in the openMSX emulator before flashing it. Install the openMSX add-on from [2040/software/multirom.pio/openmsx](../2040/software/multirom.pio/openmsx) as described in [MSX PicoVerse 2040 — MultiROM in openMSX](./msx-picoverse-2040-openmsx.md). Then, in openMSX, choose your `.uf2` file as the ROM image of a cartridge slot (Media > Cartridge Slot, with the **All files** filter). The emulated MSX boots the same PicoVerse menu with every entry in the UF2, including the Nextor Sunrise IDE entries. A disk image takes the place of the USB drive. After that, the **PicoVerse 2040 MultiROM** extension reloads the same UF2.

## Known issues

- Some ROMs with uncommon mappers may not be detected correctly and will be skipped unless a valid mapper tag is used to force detection.
- The MultiROM tool currently only supports Windows. 
- The tool does not currently validate the integrity of ROM files beyond size and basic header checks. Corrupted ROMs may lead to unexpected behavior.
- Due to the nature of MultiROM tool (embedding multiple files into a single UF2), some antivirus software may flag the executable as suspicious. This is a false positive; ensure you download the tool from a trusted source.
- The MultiROM menu does not support DSK files; only ROM files are listed and launched.
- The tool does not currently support subdirectories; only ROM files in the current working directory are processed.
- The pico flash memory can wear out after many write cycles. Avoid excessive re-flashing of the cartridge.

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
| Omega MSX | MSX2+ | OK | Verified operation |
| Panasonic FS-A1GT | TurboR | OK | Verified operation |
| Panasonic FS-A1ST | TurboR | OK | Verified operation |
| Panasonic FS-A1WX | MSX2+ | OK | Verified operation |
| Panasonic FS-A1WSX | MSX2+ | OK | Verified operation |
| Sanyo Wavy 70FD | MSX2+ | OK | Verified operation |
| Sharp HotBit HB8000 | MSX1 | OK | Verified operation |
| SMX-HB | MSX2+ (FPGA clone) | OK | Verified operation |
| Sony HB-F1XD | MSX2 | OK | Verified operation |
| Sony HB-F1XDJ | MSX2 | OK | Verified operation |
| Sony HB-F1XV | MSX2+ | OK | Verified operation |
| Sony Hit-Bit 20HB | MSX1 | OK | Verified operation |
| TRHMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| uMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| Yamaha YIS604 | MSX1 | OK | Verified operation |

Sunrise IDE Nextor ROM (options `-s` and `-m`) has been tested on the following MSX models:

| Model | Type | Status | Comments |
| --- | --- | --- | --- |
| Adermir Carchano Expert 4 | MSX2+ | OK | Verified operation |
| Gradiente Expert | MSX1 | OK | Verified operation |
| JFF MSX | MSX1 | OK | Verified operation |
| MSX Book | MSX2+ (FPGA clone) | OK | Verified operation |
| National FS-4500 | MSX1 | OK | Verified operation |
| Omega MSX | MSX2+ | OK | Verified operation |
| Panasonic FS-A1GT | TurboR | OK | Verified operation |
| Panasonic FS-A1ST | TurboR | OK | Verified operation |
| Panasonic FS-A1WX | MSX2+ | OK | Verified operation |
| Panasonic FS-A1WSX | MSX2+ | OK | Verified operation |
| Sanyo Wavy 70FD | MSX2+ | OK | Verified operation |
| Sharp HotBit HB8000 | MSX1 | OK | Verified operation |
| SMX-HB | MSX2+ (FPGA clone) | OK | Verified operation |
| Sony HB-F1XD | MSX2 | OK | Verified operation |
| Sony HB-F1XDJ | MSX2 | OK | Verified operation |
| Sony HB-F1XV | MSX2+ | OK | Verified operation |
| Sony Hit-Bit 20HB | MSX1 | OK | Verified operation |
| TRHMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| uMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| Yamaha YIS604 | MSX1 | OK | Verified operation |

Author: Cristiano  Goncalves
Last updated: 03/29/2026