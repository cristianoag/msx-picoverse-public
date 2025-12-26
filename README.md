# PicoVerse for MSX

**The MSX experience driven by the Raspberry Pi Pico family of microcontrollers.**

PicoVerse is a community-driven effort to build versatile MSX cartridges powered by Raspberry Pi Pico development boards. The project pairs accessible hardware designs with ready-to-flash firmware so MSX users can jump straight into loading games, tools, and Nextor without having to compile sources.

![alt text](/images/multirom_2040_menu.png)

> **Note:** There are reports that the PicoVerse is a copy of other projects. To avoid speculation, I am now making the source code publicly available on the software folders of this repository.

The MultiROM tool and menu firmware were developed from scratch, with support to a wide number of mappers and features not found in other similar projects. The hardware designs, implementation strategy, documentation, and PC-side tooling are all original work by the author. The wiring was thought to ensure the best compatibility with the MSX bus and to allow easy assembly using widely available RP2040 and RP2350 boards. 

PicoVerse is designed as an open-source, independent, well-documented MSX cartridge platform. Compatibility with other projects is neither a goal nor guaranteed (I tested some without much success); running third‑party software on PicoVerse hardware, or PicoVerse firmware on other boards, is at your own risk. The source and design files are openly available so you can learn, experiment, and build on them for the MSX community, subject to the project license.

## Project Highlights
- Multi-ROM loader with an on-screen menu and mapper auto-detection.
- Ready-made Nextor builds with USB (RP2040) or microSD (RP2350) storage bridges.
- PC-side tooling that generates UF2 images locally for quick drag-and-drop flashing.
- Open hardware schematics, BOMs, and production-ready Gerbers.
- Active development roadmap covering RP2040 and RP2350-based cartridges.

## Documentation

- [PicoVerse 2040 MultiROM Guide, English version](/docs/msx-picoverse-2040-multirom-tool-manual.en-US.md)
- [Guia MultiROM do PicoVerse 2040, versão em português](/docs/msx-picoverse-2040-multirom-tool-manual.pt-BR.md)
- [PicoVerse 2040 マルチROMガイド（日本語版）](/docs/msx-picoverse-2040-multirom-tool-manual.ja-JP.md)
- [Guía MultiROM PicoVerse 2040, versión en español](/docs/msx-picoverse-2040-multirom-tool-manual.es-ES.md)
- [PicoVerse 2040 Features Overview](/docs/msx-picoverse-2040-features.md)
- [PicoVerse 2350 Features Overview](/docs/msx-picoverse-2350-features.md)
- [Nextor Pico Bridge Protocol](/docs/Nextor-Pico-Bridge-Protocol.md)

## Hardware Variants

### PicoVerse 2040 Cartridge

| Prototype PCB (front) | Prototype PCB (back) |
|---------|---------|
| ![Image 1](/images/20241230_001854885_iOS.jpg) | ![Image 2](/images/20241230_001901504_iOS.jpg) | 

- Based on RP2040 boards exposing 30 GPIO pins (not compatible with stock Raspberry Pi Pico pinout).
- Up to 16 MB of flash for MSX ROMs with support for Plain16/32, Linear0, Konami SCC, Konami, ASCII8/16, NEO-8, and NEO-16 mappers.
- USB-C port doubles as a bridge for Nextor mass storage.

#### Bill of Materials

![alt text](/images/2025-12-02_20-05.png)

Interactive BOM available at [PicoVerse 2040 BOM](https://htmlpreview.github.io/?https://raw.githubusercontent.com/cristianoag/msx-picoverse-public/refs/heads/main/2040/hardware/MSX_PicoVerse_2040_1.3_bom.html)

| Reference | Description | Quantity | Link |
| --- | --- | --- | --- |
| U1 | RP2040 Dev Board 30 GPIO pins exposed | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c4MuM9st) |
| C1 | 0603 0.1 µF Ceramic Capacitor | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c2w5e36V) |
| C2 | 0603 10 µF Ceramic Capacitor | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c2w5e36V)|
| R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, R13| 0603 10 kΩ Resistor | 12 | [AliExpress](https://s.click.aliexpress.com/e/_c3XBv4od)|
| R1 | 0603 2 KΩ Resistor | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c3XBv4od)|
| D1 | 1N5819 SOD-123 Diode | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c4WEKCuz) |
| Q1, Q2, Q3, Q4, Q5 | BSS138 SOT-23 Transistor | 5 | [AliExpress](https://s.click.aliexpress.com/e/_c2veWxcD)|


### PicoVerse 2350 Cartridge

| Prototype PCB (front) | Prototype PCB (back) |
|---------|---------|
| ![Image 1](/images/20250208_180923511_iOS.jpg) | ![Image 2](/images/20250208_181032059_iOS.jpg) |

- Targets RP2350 boards exposing all 48 GPIO pins (not compatible with standard Pico 2 boards).
- Adds microSD storage, ESP8266 WiFi header, and I2S audio expansion alongside 16 MB flash space.
- Extra RAM to support advanced emulation features in future firmware releases.
- Ships with a Nextor-first menu so you can boot straight into SofaRun or other disk-based tools.
- Shares the same ROM mapper support list as the 2040 build.

#### Bill of Materials
| Reference | Description | Quantity | Link |
| --- | --- | --- | --- |
| U1 | RP2350 Dev Board 48 GPIO pins exposed (modded for extra RAM) | 1 | |

## Repository Contents
- `hardware/` – Production-ready Gerbers, fabrication notes, and BOMs for each supported dev board.
- `software/` – MultiROM PC utilities (`multirom.exe`) and menu ROM assets for both cartridge families.
- `docs/` – Feature lists, usage walkthroughs, and revision history for each cartridge family.
- `images/` – Board renders and build photos for quick identification.

For design source files, firmware code, and ongoing development discussions, see the private engineering repository.

## Quick Start
1. **Pick your target board**: Select the hardware revision that matches the RP2040 or RP2350 carrier you own, then grab the corresponding Gerber/BOM pack.
2. **Manufacture or assemble**: Send the Gerbers to your PCB house or build from an ordered kit. Follow the assembly notes included in each hardware bundle.
3. **Generate the UF2 image**:
   - Place your `.rom` files beside the MultiROM tool for your cartridge family (`2040/software/multirom/multirom.exe` or `2350/software/multirom/multirom.exe`).
   - Run `multirom.exe` to build a new `multirom.uf2`; no prebuilt UF2 images are distributed.
4. **Flash the firmware**:
   - Hold BOOTSEL while connecting the cartridge to your PC via USB-C.
   - Copy the freshly generated `multirom.uf2` (or alternate UF2 you produced) to the RPI-RP2 drive that appears.
   - Eject the drive; the board reboots and stores the image in flash.
5. **Enjoy on MSX**: Insert the cartridge, power on the computer, pick a ROM from the menu, or launch Nextor to access USB or microSD storage.

## Compatibility & Requirements
- Works with MSX, MSX2, and MSX2+ systems. Mapper support covers the most common game and utility formats.
- Requires Windows or Linux to run the PC-side UF2 builder utilities.
- Ensure your development board matches the pinout documented for each hardware revision before soldering.

## License & Usage

![Creative Commons Attribution-NonCommercial-ShareAlike 4.0](/images/ccans.png)

All hardware and firmware binaries in this repository are released under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International license. Personal builds and community tinkering are encouraged, but commercial use or resale requires explicit authorization from the author.

## Feedback & Community
Questions, test reports, and build photos are welcome. Open an issue on the public repository or reach out through the MSX retro hardware forums where PicoVerse updates are posted.
