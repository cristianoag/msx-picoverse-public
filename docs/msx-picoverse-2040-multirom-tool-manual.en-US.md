# MSX PicoVerse 2040 Project

|PicoVerse Front|PicoVerse Back|
|---|---|
|![PicoVerse Front](/images/2025-12-02_20-05.png)|![PicoVerse Back](/images/2025-12-02_20-06.png)|

The PicoVerse 2040 is a Raspberry Pi Pico based cartridge for MSX that uses replaceable firmware to extend the computer’s capabilities. By loading different firmware images, the cartridge can run MSX games and applications and emulate additional hardware devices (such as ROM mappers, extra RAM, or storage interfaces), effectively adding virtual peripherals to the MSX. One such firmware is the MultiROM system, which provides an on‑screen menu for browsing and launching multiple ROM titles stored in the cartridge.

The cartridge can also expose the Pico’s USB‑C port as a mass‑storage device, allowing you to copy ROMs, DSKs, and other files directly from a PC with Windows or Linux to the cartridge.

Those are the features available in the current version of the PicoVerse 2040 cartridge:

* MultiROM menu system for selecting and launching MSX ROMs.
* Inline Menu option with Nextor OS support.
* USB mass-storage device support for loading ROMs and DSKs.
* Support for various MSX ROM mappers (PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16).
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

> **Note:** The `-n` option includes an experimental embedded Nextor ROM that works on specific MSX2 models. This version uses a different communication method (IO port based) and may not work on all systems.

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
- If you need to force a specific mapper type for a ROM file, you can append a mapper tag before the `.ROM` extension in the filename. The tag is case-insensitive. For example, naming a file `Knight Mare.PL-32.ROM` forces the use of the PL-32 mapper for that ROM. Tags like `SYSTEM` are ignored. The list of possible tags that can be used is: `PL-16,  PL-32,  KonSCC,  Linear,  ASC-08,  ASC-16,  Konami,  NEO-8,  NEO-16`

### Examples
- Produces the multirom.uf2 file with the MultiROM menu and all `.ROM` files in the current directory. You can run the tool using the command prompt or just by double-clicking the executable:
  ```
  multirom.exe
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

![alt text](/images/multirom_2040_menu.png)

Use the Up and Down arrow keys to move the selection cursor through the list of ROMs. If you have more than 19 ROMs, use the lateral arrow keys (Left and Right) to scroll through pages of entries.

Press the Enter or Space key to launch the selected ROM. The MSX will attempt to boot the ROM using the appropriate mapper settings.

At any time while in the menu, you can press H key read the help screen with basic instructions. Press any key to return to the main menu.

## Using Nextor with the PicoVerse 2040 cartridge

The -n option of the multirom tool includes an experimental embedded Nextor ROM that can be used on specific MSX2 models. However, this option is still under development and may not work on all systems. This version uses a different method (IO port based) to communicate with the MSX, allowing it to work on systems where the cartridge slot is not primary, for example with slot expanders. 

More details on the protocol used to communicate with the MSX computer can be found in the Nextor-Pico-Bridge-Protocol.md document. 

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
> **Note:** Remember Nextor needs a minimum of 128 KB of RAM to operate. 

## Improvements ideas
- Improve the ROM type detection heuristics to cover more mappers and edge cases.
- Implement configuration screen for each ROM entry (name, mapper override, etc).
- Add support for more ROM mappers.
- Implement a graphical menu with better navigation and ROM information display.
- Add support for saving/loading menu configuration to preserve user preferences.
- Support DSK files as well, with proper configuration entries.
- Add support for custom themes for the menu.
- Allow downloading ROMs from Internet URLs and embedding them directly.
- Allow the use of the joystick to navigate the menu.

## Known issues

- The embedded Nextor ROM inclusion is still experimental and may not work on all MSX2 models.
- Some ROMs with uncommon mappers may not be detected correctly and will be skipped unless a valid mapper tag is used to force detection.
- The MultiROM tool currently only supports Windows. Linux and macOS versions are not available yet.
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
| Sharp HotBit HB8000 | MSX1 | OK | Verified operation |
| SMX-HB | MSX2+ (FPGA clone) | OK | Verified operation |
| Sony HB-F1XD | MSX2 | OK | Verified operation |
| Sony HB-F1XDJ | MSX2 | OK | Verified operation |
| Sony HB-F1XV | MSX2+ | OK | Verified operation |
| TRHMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| uMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| Yamaha YIS604 | MSX1 | OK | Verified operation |

Experimental embedded Nextor ROM (option `-n`) has been tested on the following MSX models:

| Model | Type | Status | Comments |
| --- | --- | --- | --- |
| Omega MSX | MSX2+ | Not OK | USB not detected |
| Sony HB-F1XD | MSX2 | Not OK | Error reading disk sector |
| TRHMSX | MSX2+ (FPGA clone) | OK | Verified operation |
| uMSX | MSX2+ (FPGA clone) | OK | Verified operation |

Author: Cristiano Almeida Goncalves
Last updated: 12/25/2025