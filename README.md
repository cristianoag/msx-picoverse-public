# PicoVerse for MSX

**The MSX experience driven by the Raspberry Pi Pico family of microcontrollers.**

PicoVerse is a community-driven effort to build versatile MSX cartridges powered by Raspberry Pi Pico development boards. The project pairs accessible hardware designs with ready-to-flash firmware so MSX users can jump straight into loading games, tools, and Nextor without having to compile sources.

| Cartridge Label - 2040 Variant | Cartridge Label - 2350 Variant |
|---------|---------|
|![PicoVerse 2040 Label](/labels/PicoVerse_2040_Label_1.png) | ![PicoVerse 2350 Label](/labels/PicoVerse_2350_Label_1.png) |

PicoVerse is designed as an open-source, independent, and documented MSX cartridge platform. Compatibility with other projects is neither a goal nor guaranteed (I tested some without much success); running third‑party software on PicoVerse hardware, or PicoVerse firmware on other boards, is at your own risk. The source and manufacturing files are openly available so you can learn, experiment, and build on them for the MSX community, subject to the project license.

If you find any issues, have questions, or want to contribute, please open an issue or reach out via the contact information in the repository.

## Project Highlights

- Single-ROM LoadROM workflow for instant and easy booting of one title.
- Multi-ROM loader with an on-screen menu and mapper auto-detection.
- Explorer firmware (PicoVerse 2350) merges flash ROMs, optional embedded Sunrise Nextor SYSTEM entries, and microSD ROMs, labels the source (FL/SD), adds microSD MP3/WAV playback, supports WAVEGAME microSD ROMs with port `0x92` controlled WAV streaming, supports on-device search, can cycle supported microSD partitions with `P`, can delete selected SD files with confirmed `D`, includes ROM detail audio profiles with native and external SCC/SCC+, Dual PSG, MSX-MUSIC, and YM2151/SFG support, adds an independent primary PSG DAC mirror option, and includes an integrated File Hunter browser for downloading ROMs over ESP-01 WiFi.
- Ready-made Nextor builds with USB pendrive (`-s2`/`-m2`) or FAT16 up-to-4GB microSD partitions (`-s1`/`-m1`) via Sunrise IDE emulation on PicoVerse 2350, and USB (`-s`/`-m`) on PicoVerse 2040.
- Sunrise IDE + 192KB memory mapper on PicoVerse 2040 (`loadrom.exe -m`), or Sunrise IDE standalone (`loadrom.exe -s`).
- Sunrise IDE + 1MB PSRAM memory mapper on PicoVerse 2350 (`loadrom.exe -m1` for microSD, `loadrom.exe -m2` for USB), or Sunrise IDE standalone (`loadrom.exe -s1` / `loadrom.exe -s2`).
- Sunrise IDE + 1MB memory mapper + 1MB MegaRAM on PicoVerse 2350 (`loadrom.exe -r1` / `explorer.exe -r1` for microSD, `loadrom.exe -r2` / `explorer.exe -r2` for USB), exposing 128 × 8KB MegaRAM banks in a dedicated expanded subslot.
- Carnivore2-compatible RAM-mode loader on PicoVerse 2350 (`loadrom.exe -c1` / `loadrom.exe -c2`) for `SROM.COM /D15` uploads into the 1MB PSRAM mapper.
- ESP-01 WiFi support for PicoVerse 2350 Sunrise IDE LoadROM/MultiROM builds (`loadrom.exe -s1 -w`, `-m1 -w`, `-s2 -w`, `-m2 -w`) and for the Explorer File Hunter browser. Compatible with both real MSX hardware and FPGA-based MSX cores.
- SCC/SCC+, Dual PSG, MSX-MUSIC/YM2413, and YM2151/SFG emulation on the PicoVerse 2350, with per-ROM audio profile selection where supported.
- OPL4 / YMF278B / MoonSound emulation on the PicoVerse 2350 (`loadrom.exe -4`) — a standalone sound cartridge with OPL3 FM (18 channels), the 24-voice PCM wavetable, the embedded 2 MB YRW801-M wave ROM, and 2 MB of PCM sample RAM on the standard MoonSound ports.
- MSX-AUDIO / Yamaha Y8950 emulation on the PicoVerse 2350 (`loadrom.exe -a`) — a standalone MSX-AUDIO card with OPL1 FM plus ADPCM, 256 KB of ADPCM sample RAM, and the MSX-Audio BIOS mapped in the cartridge slot.
- Yamanooto flash-cartridge emulation on the PicoVerse 2350 (`yamanooto.exe`) — a Konami-SCC compatible 8 MB flash cartridge with SCC/SCC+, a secondary (dual) PSG, a primary PSG mirror, and MSX-MUSIC/FM-PAC, all always available with the firmware selecting SCC, FM, or pure PSG on the fly so one image can mix SCC, FM, and PSG games.
- PC-side tooling that generates UF2 images locally for quick drag-and-drop flashing.
- USB keyboard support on PicoVerse 2040 — use a standard USB keyboard as the MSX keyboard via the cartridge slot.
- MSX-MIDI support on PicoVerse 2040 — use a USB-MIDI cable as a standard MSX-MIDI interface via the cartridge slot. 
- MIDI-PAC support on PicoVerse 2040 — passively convert live MSX PSG music and effects into high-quality MIDI for external synths or sound modules (SC-55 optimized). Features automatic bass detection, tone+noise coexistence, smooth pitch bending on fast passages, and improved percussion mapping. 
- USB joystick support on PicoVerse 2040 — plug a standard USB gamepad (HID, Xbox 360, Xbox One, or Xbox Series X|S) into the USB-C port and it appears as one or two native MSX joysticks with no MSX-side software needed. Uses open-drain bus driving to coexist with the real PSG.
- BOMs, and production-ready Gerbers.
- Active development roadmap covering RP2040 and RP2350-based cartridges.

## Documentation

**LoadROM Guides:** Use the LoadROM tool to create a UF2 image that boots directly into a single ROM, skipping the menu. Mapper type is auto-detected with filename tag overrides, and the tool reports the detected configuration before flashing.
- [MSX PicoVerse 2040 LoadROM Tool Manual (English)](/docs/msx-picoverse-2040-loadrom-tool-manual.en-us.md) 
- [MSX PicoVerse 2350 LoadROM Tool Manual (English)](/docs/msx-picoverse-2350-loadrom-tool-manual.en-us.md) 
  
**MultiROM Guides:** Use the MultiROM tool to create a UF2 image that allows selecting from multiple ROMs at boot. Mapper type is auto-detected with filename tag overrides, and the tool reports the detected configuration before flashing.
- [PicoVerse 2040 MultiROM Guide Manual (English)](/docs/msx-picoverse-2040-multirom-tool-manual.en-us.md)
- [PicoVerse 2350 MultiROM Tool Manual (English)](/docs/msx-picoverse-2350-multirom-tool-manual.en-us.md)

**Explorer Guides:** Use the Explorer tool to manage flash, embedded Sunrise Nextor, and microSD ROMs, cycle supported microSD partitions, delete selected SD files, play MP3/WAV files from the microSD card, browse File Hunter over ESP-01 WiFi, select ROM audio/PSG options, and search for titles on the device.
- [MSX PicoVerse 2350 Explorer Tool Manual (English)](/docs/msx-picoverse-2350-explorer-tool-manual.en-us.md)

**Yamanooto Guides:** Use the Yamanooto tool to create a UF2 that emulates a Yamanooto flash cartridge (Konami-SCC compatible, SCC/SCC+, dual PSG, primary PSG mirror, and MSX-MUSIC/FM-PAC) from a single ROM image, with the firmware choosing SCC/FM/PSG automatically at runtime.
- [MSX PicoVerse 2350 Yamanooto Tool Manual (English)](/docs/msx-picoverse-2350-yamanooto-tool-manual.en-us.md)

**Reference Material** 
- [PicoVerse 2040 Features Overview](/docs/msx-picoverse-2040-features.md)
- [PicoVerse 2350 Features Overview](/docs/msx-picoverse-2350-features.md)
- [MSX PicoVerse 2350 Dual PSG Implementation](/docs/msx-picoverse-2350-dualpsg.md)
- [MSX PicoVerse 2350 MSX-MUSIC/FM-PAC Implementation](/docs/msx-picoverse-2350-fmpac.md)
- [MSX PicoVerse 2350 50/60Hz CPU Speed Implementation](/docs/msx-picoverse-2350-50-60hz.md) 
- [MSX PicoVerse 2350 CPU Speed Implementation](/docs/msx-picoverse-2350-cpu-speed.md)
- [MSX PicoVerse 2350 Yamanooto Implementation](/docs/msx-picoverse-2350-yamanooto.md)
- [MSX PicoVerse 2350 MegaRAM Implementation](/docs/msx-picoverse-2350-megaram.md)
- [MSX PicoVerse 2350 WAVEGAME Protocol and Support](/docs/msx-picoverse-2350-wavegame.md)
- [MSX PicoVerse 2350 WiFi Support](/docs/msx-picoverse-2350-wifi.md)
- [MSX PicoVerse 2040 PIO Strategy](/docs/msx-picoverse-2040-pio.md) 
- [MSX PicoVerse 2350 PIO Strategy](/docs/msx-picoverse-2350-pio.md) 
- [MSX PicoVerse 2350 SCC Emulation](/docs/msx-picoverse-2350-scc.md)
- [MSX PicoVerse 2040 Sunrise IDE Emulation for Nextor](/docs/msx-picoverse-2040-sunrise-nextor.md) 
- [MSX PicoVerse 2350 Sunrise IDE Emulation for Nextor](/docs/msx-picoverse-2350-sunrise-nextor.md) 
- [MSX PicoVerse 2040 Mapper Implementation (Sunrise + Nextor)](/docs/msx-picoverse-2040-mapper.md) 
- [MSX PicoVerse 2040 USB Keyboard](/docs/msx-picoverse-2040-keyboard.md)
- [MSX PicoVerse 2040 MSX-MIDI](/docs/msx-picoverse-2040-msx-midi.md) 
- [MSX PicoVerse 2040 MIDI-PAC](/docs/msx-picoverse-2040-midipac.md)
- [MSX PicoVerse 2040 MegaROMs](/docs/msx-picoverse-2040-megaroms.md)
- [MSX PicoVerse 2040 USB Joystick](/docs/msx-picoverse-2040-joystick.md)


## Hardware Variants

### PicoVerse 2040 Cartridge

| Prototype PCB (front) | Prototype PCB (back) |
|---------|---------|
| ![Image 1](/images/20241230_001854885_iOS.jpg) | ![Image 2](/images/20241230_001901504_iOS.jpg) | 

- Based on RP2040 boards exposing 30 GPIO pins (not compatible with stock Raspberry Pi Pico pinout).
- Up to 16 MB of flash for MSX ROMs with support for Plain16/32 (`PLA-16`/`PLA-32`), Planar48/64 (`PLN-48`/`PLN-64`), Konami SCC, Konami, ASCII8/16/16X, NEO-8, NEO-16, and Manbow2 (`MANBW2`) mappers.
- USB-C port doubles as a bridge for Nextor mass storage via Sunrise IDE emulation (`loadrom.exe -s`).
- Sunrise IDE + 192KB memory mapper mode provides both Nextor disk access and expanded RAM (`loadrom.exe -m`).
- Standalone USB keyboard firmware — use a standard USB keyboard as the MSX keyboard via the cartridge slot (`loadrom.exe -k`). Not compatible with FPGA-based MSX systems or MSX computers with an integrated PPI (e.g. T9769 MSX-ENGINE).
- Standalone MSX-MIDI firmware — use a USB-MIDI cable as a standard MSX-MIDI interface for MIDI players and sequencers (`loadrom.exe -i`).
- Standalone MIDI-PAC firmware — passively listen to the MSX PSG and convert music and effects to USB MIDI for an external synth or sound module (`loadrom.exe -p`).
- Standalone USB joystick firmware — use a standard USB gamepad as an MSX joystick via the cartridge slot (`loadrom.exe -j`). Supports Xbox 360, Xbox One, Xbox Series X|S, and generic USB HID gamepads, with up to 2 gamepads mapped to MSX joystick ports 1 and 2.  Not compatible with FPGA-based MSX systems or MSX computers with an integrated PSG (e.g. T9769 MSX-ENGINE).

#### Bill of Materials

![alt text](/images/2026-03-28_20-28.png)

Interactive BOM available at [PicoVerse 2040 BOM](https://htmlpreview.github.io/?https://raw.githubusercontent.com/cristianoag/msx-picoverse-public/refs/heads/main/2040/hardware/MSX_PicoVerse_2040_1.3_bom.html)

| Reference | Description | Quantity | Link |
| --- | --- | --- | --- |
| U1 | RP2040 Dev Board 30 GPIO pins exposed | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c2un4bmn) |
| C1 | 0603 0.1 µF Ceramic Capacitor | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c2w5e36V) |
| C2 | 0603 10 µF Ceramic Capacitor | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c2w5e36V)|
| R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, R13| 0603 10 kΩ Resistor | 12 | [AliExpress](https://s.click.aliexpress.com/e/_c3XBv4od)|
| R1 | 0603 2 KΩ Resistor | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c3XBv4od)|
| D1 | 1N5819 SOD-123 Diode | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c4WEKCuz) |
| Q1, Q2, Q3, Q4, Q5 | BSS138 SOT-23 Transistor | 5 | [AliExpress](https://s.click.aliexpress.com/e/_c3IHdO5t)|

### PicoVerse 2350 Cartridge

| Prototype PCB (front) | Prototype PCB (back) |
|---------|---------|
| ![Image 1](/images/20250208_180923511_iOS.jpg) | ![Image 2](/images/20250208_181032059_iOS.jpg) |

- Targets RP2350 boards exposing all 48 GPIO pins (not compatible with standard Pico 2 boards).
- Adds microSD storage, ESP8266 WiFi header, and I2S audio expansion alongside 16 MB flash space.
- Extra RAM (PSRAM) now backs the 1MB mapper modes and the Carnivore2-compatible RAM loader, with room for additional advanced firmware features.
- Explorer firmware can load ROMs from flash, optional embedded Sunrise Nextor SYSTEM entries, and microSD, cycle FAT16/FAT32/exFAT microSD browsing partitions with `P`, delete selected SD files after confirmation with `D`, play MP3/WAV files from microSD, browse File Hunter over ESP-01 WiFi, save downloaded File Hunter ROMs to the microSD root, search local title lists, select native or external SCC/SCC+, Dual PSG, MSX-MUSIC, or YM2151/SFG audio profiles per ROM where supported, and optionally mirror primary PSG audio through the cartridge DAC.
- USB-C port doubles as a bridge for Nextor mass storage via Sunrise IDE emulation (`-s2`).
- microSD card slot provides Nextor mass storage via Sunrise IDE emulation (`-s1`) when using FAT16 partitions up to 4 GB.
- Sunrise IDE + 1MB PSRAM memory mapper mode provides both Nextor disk access and expanded RAM (`-m1` for microSD, `-m2` for USB).
- Sunrise IDE + 1MB mapper + 1MB MegaRAM mode provides Nextor disk access, standard mapper RAM, and a separate writable MegaRAM cartridge surface (`-r1` for microSD, `-r2` for USB).
- Carnivore2-compatible RAM-mode loading lets `SROM.COM /D15` upload ROMs into the 1MB PSRAM mapper (`-c1` for microSD, `-c2` for USB).
- Yamanooto flash-cartridge emulation (`yamanooto.exe`) reproduces the  Yamanooto: a Konami-SCC compatible 8 MB flash cartridge with SCC/SCC+, a secondary (dual) PSG on ports `0x10`/`0x11`, a primary PSG mirror (`0xA0`/`0xA1`), and MSX-MUSIC/FM-PAC via an expanded subslot. All engines are always available and the firmware selects SCC, FM, or pure PSG on the fly, so a single image can hold a mix of SCC, FM, and PSG games.
- Standalone OPL4 / YMF278B / MoonSound cartridge firmware (`loadrom.exe -4`): full YMF278B emulation (OPL3 FM with 18 channels plus 24 PCM voices), the 2 MB YRW801-M wave ROM embedded in the UF2, 2 MB of PCM sample RAM in PSRAM, MoonSound ports `0xC4`-`0xC7` and `0x7E`/`0x7F`, FM timer interrupts on `/INT`, and 16-bit stereo 44.1 kHz output through the I2S DAC. Optional `--opl4-limit` adaptive PCM voice limiter and `--lowclock` 282 MHz build.
- Standalone MSX-AUDIO / Yamaha Y8950 cartridge firmware (`loadrom.exe -a`): OPL1 FM (9 channels) plus ADPCM-B, 256 KB of ADPCM sample RAM in PSRAM, the 48 KB MSX-Audio BIOS v1.3 mapped in the slot (openMSX `Boosted_audio.xml` layout, with base and expanded work RAM), ports `0xC0`/`0xC1`, FM timer interrupts on `/INT`, and 16-bit 49716 Hz output through the I2S DAC. Optional `--4mhz` Y8950 clock.
- LoadROM Sunrise builds can also expose ESP-01 WiFi support with `-w` on top of the `-s1`/`-m1`/`-s2`/`-m2` modes.
- LoadROM, MultiROM, and Explorer support the Sunrise IDE options. MultiROM and Explorer allow combining them so multiple Nextor modes appear as selectable SYSTEM entries in the menu; Explorer also provides `-a` / `--allnextor` to add all embedded Nextor entries while still appending folder ROMs.
- Shares the same ROM mapper support list as the 2040 build.

#### Bill of Materials

![alt text](/images/2026-07-26_17-50.png)

Interactive BOM available at [PicoVerse 2350 BOM](https://htmlpreview.github.io/?https://raw.githubusercontent.com/cristianoag/msx-picoverse-public/refs/heads/main/2350/hardware/MSX_PicoVerse_2350_1.2.html) 

| Reference | Description | Quantity | Link |
| --- | --- | --- | --- |
| U1 | WaveShare Core2350B Dev Board 48 GPIO pins exposed (8Mb PSRAM)| 1 | [AliExpress](https://pt.aliexpress.com/item/1005009578742534.html?spm=a2g0o.order_list.order_list_main.112.62b91802b6L8HW&gatewayAdapt=glo2bra) |
|U2| UDA1334A I2S Stereo DAC| 1 | [AliExpress](https://s.click.aliexpress.com/e/_c3gam5lH) |
|U3| ESP-01 ESP8266 WiFi Module| 1 | [AliExpress](https://s.click.aliexpress.com/e/_c3cPt2OL) |
|C1| 0603 0.1 µF Ceramic Capacitor | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c3tkUxHz) |
|C2,C3| 0603 10 µF Ceramic Capacitor | 2 | [AliExpress](https://s.click.aliexpress.com/e/_c3tkUxHz)|
|R1| 0603 2 KΩ Resistor | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c43uKcEj)|
|R2, R3, R6, R7, R8| 0603 10 kΩ Resistor | 4 | [AliExpress](https://s.click.aliexpress.com/e/_c43uKcEj)|
|R4, R5| 0603 5.1 kΩ Resistor | 2 | [AliExpress](https://s.click.aliexpress.com/e/_c43uKcEj)|
|R9, R10| 0603 330 Ω Resistor | 2 | [AliExpress](https://s.click.aliexpress.com/e/_c43uKcEj)|
|D1| 1N5819 SOD-123 Diode | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c4WEKCuz) |
|SW1| Tactile Push Button Switch | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c2IPGkzv) |
|J1| USB-C 16 Pin Connector | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c4PtBc51) |
|J2| microSD Card Slot | 1 | [AliExpress](https://s.click.aliexpress.com/e/_c4Pzbd7Z) |

## Repository Contents
- `hardware/` – Production-ready Gerbers, fabrication notes, and BOMs for each supported dev board.
- `software/` – MultiROM PC utilities (`multirom.exe`) and menu ROM assets for both cartridge families.
- `docs/` – Feature lists, usage walkthroughs, and revision history for each cartridge family.
- `images/` – Board renders and build photos for quick identification.
- `labels/` – Printable Patola style cartridge label designs for both hardware variants.

## Quick Start
1. **Pick your target board**: Select the hardware revision that matches the RP2040 or RP2350 carrier you own, then grab the corresponding Gerber/BOM pack.
2. **Manufacture or assemble**: Send the Gerbers to your PCB house or build from an ordered kit. Follow the assembly notes included in each hardware bundle.
3. **Generate the UF2 image**:
   - For multiple titles, place your `.rom` files beside the MultiROM tool for your cartridge family (`2040/software/multirom.pio/tool/multirom.exe` or `2350/software/multirom.pio/tool/multirom.exe`) and run `multirom.exe` to create `multirom.uf2`.
   - For an instant boot into a single game, place the desired `.rom` next to the LoadROM tool for your cartridge family and run `loadrom.exe <file> [-o custom.uf2]` to create a dedicated UF2 that skips the menu.
   - For a combined flash + microSD menu (on PicoVerse 2350 only), use the Explorer tool (`2350/software/explorer.pio/tool/explorer.exe`) to create `explorer.uf2`. Add options such as `-s1`, `-m1`, `-r1`, `-s2`, `-m2`, `-r2`, or `-a` / `--allnextor` when you want embedded Nextor SYSTEM entries in the same Explorer menu, then copy extra `.rom` and `.mp3` files to the microSD card. With an ESP-01 module and configured WiFi, Explorer can also browse File Hunter and save downloaded ROMs directly to the microSD root.
4. **Flash the firmware**:
   - Hold BOOTSEL while connecting the cartridge to your PC via USB-C.
   - Copy the freshly generated UF2 (`multirom.uf2`, `loadrom.uf2`, or `explorer.uf2`) to the RPI-RP2 drive that appears.
   - Eject the drive; the board reboots and stores the image in flash.
5. **Enjoy on MSX**: Insert the cartridge, power on the computer, pick a ROM from the menu, or launch Nextor to access USB or microSD storage.

## MultiROM Menu

| PicoVerse 2040 MultiRom | PicoVerse 2350 MultiRom |
|---------|---------|
|<img src="/images/2026-05-17_21-15.png" alt="PicoVerse 2040 MultiROM menu" width="420" height="315">|<img src="/images/2026-05-17_21-20.png" alt="PicoVerse 2350 MultiROM menu" width="420" height="315">|

To use the MultiROM menu, insert the PicoVerse cartridge into your MSX and power it on. The system will boot directly into the MultiROM menu, which displays a list of all ROMs embedded in the cartridge. Each entry is named after the ROM filename (up to 50 characters), and longer names are shown using a scrolling effect.

Navigate the menu using the keyboard arrow keys. Use the Up and Down keys to move through the list of ROMs, and if more than 19 ROMs are present, use the Left and Right keys to switch between pages. To start a game or application, select it and press Enter or Space; the MSX will boot the ROM using the appropriate mapper configuration automatically.

While in the menu, pressing the H key opens a help screen with basic instructions; press any key to return to the main menu. Once a ROM is launched, control is handed over entirely to the selected software, just as if it were a physical cartridge inserted into the MSX.

Check the detailed MultiROM guide in the documentation folder for advanced features, troubleshooting tips, and mapper support details.

## LoadROM Tool

The LoadROM tool targets situations where you want the PicoVerse to behave like a traditional single-game cartridge or as a dedicated standalone firmware image. Instead of showing the MultiROM menu, the Pico boots straight into one ROM embedded in the UF2 image, or into a selected standalone mode such as keyboard, MSX-MIDI, MIDI-PAC, or USB joystick.

- **Input**: exactly one `.ROM` file. Mapper type is auto-detected with the same heuristics as MultiROM, and you can still force a mapper via filename tags such as `.KonSCC.ROM` or `.PLA-32.ROM`.
- **Output**: `loadrom.uf2` by default, or any filename you pass via `-o`. The UF2 contains the firmware, a 59-byte configuration record (title, mapper, size, flash offset), and the ROM payload.
- **Workflow**:
   1. Open a Command Prompt or PowerShell window in your target package folder:
      - `2040/software/loadrom.pio/tool`, or
      - `2350/software/loadrom.pio/tool`.
   2. Run `loadrom.exe -o mygame.uf2 \\path\\to\\Game.ROM` (the tool also accepts drag-and-drop onto the EXE).
      - MSX-MIDI standalone mode: `loadrom.exe -i -o midi.uf2`
      - MIDI-PAC standalone mode: `loadrom.exe -p -o midipac.uf2`
      - USB joystick standalone mode: `loadrom.exe -j -o joystick.uf2`
      - SCC standard emulation (only PicoVerse 2350): `loadrom.exe -scc \\path\\to\\Game.ROM`
      - SCC+ enhanced emulation (only PicoVerse 2350): `loadrom.exe -sccplus \\path\\to\\Game.ROM`
      - MSX-MUSIC/FM-PAC emulation with the bundled English FM-PAC BIOS (only PicoVerse 2350, non-SYSTEM ROMs): `loadrom.exe -f \\path\\to\\Game.ROM`, `loadrom.exe -fmpac \\path\\to\\Game.ROM`, or `loadrom.exe --fmpac -o mygame-fmpac.uf2 \\path\\to\\Game.ROM`
      - Dual PSG emulation on secondary PSG ports `0x10`/`0x11` (only PicoVerse 2350, supported non-SYSTEM ROMs): `loadrom.exe -d \\path\\to\\Game.ROM` or `loadrom.exe --dual-psg -o mygame-dualpsg.uf2 \\path\\to\\Game.ROM`
      - `-scc` and `-sccplus` are mutually exclusive.
      - `-scc`, `-sccplus`, `-f` / `-fmpac`, and `-d` / `--dual-psg` are cartridge audio modes; use only one of them per UF2.
      - `-f` / `-fmpac` is not valid with Sunrise, Carnivore2, or MegaRAM system modes (`-s1`, `-m1`, `-s2`, `-m2`, `-c1`, `-c2`, `-r1`, or `-r2`) or with Konami SCC / Manbow2 mapper ROMs.
      - `-d` / `--dual-psg` is intended for software that writes to a second PSG. It is not available for Konami SCC or Manbow2 mapper ROMs, SCC/SCC+ audio builds, Sunrise/Nextor system modes, Carnivore2 system modes, or MegaRAM system modes.
      - Sunrise IDE standalone (PicoVerse 2040): `loadrom.exe -s`
      - Sunrise IDE + 192KB mapper (PicoVerse 2040): `loadrom.exe -m`
      - Sunrise IDE standalone (PicoVerse 2350): `loadrom.exe -s1` or `loadrom.exe -s2`
      - Sunrise IDE + 1MB PSRAM mapper (PicoVerse 2350): `loadrom.exe -m1` or `loadrom.exe -m2`
      - Sunrise IDE + 1MB PSRAM mapper + 1MB MegaRAM (PicoVerse 2350): `loadrom.exe -r1` or `loadrom.exe -r2`
      - Sunrise IDE + WiFi (PicoVerse 2350): `loadrom.exe -s1 -w`, `loadrom.exe -s2 -w`, `loadrom.exe -m1 -w`, or `loadrom.exe -m2 -w`
      - Carnivore2-compatible RAM loader (PicoVerse 2350): `loadrom.exe -c1` or `loadrom.exe -c2`
      - OPL4 / MoonSound standalone cartridge (PicoVerse 2350, no ROM file): `loadrom.exe -4`, optionally with `--opl4-limit` or `--lowclock`
      - MSX-AUDIO / Y8950 standalone cartridge (PicoVerse 2350, no ROM file): `loadrom.exe -a`, optionally with `--4mhz`
      - `-4` and `-a` are standalone builds: they take no ROM file and cannot be combined with each other or with any other mode or audio flag.
      - `-w` is currently supported only with `-s1`, `-m1`, `-s2`, or `-m2`.
   3. Observe the reported ROM name, size, mapper status (auto vs forced), and Pico offset before the UF2 is written.
   4. Put the Pico into BOOTSEL mode and copy the generated UF2 to the `RPI-RP2` drive.
   5. Insert the cartridge into your MSX—on power-up the embedded game launches immediately.

Consult the LoadROM manuals linked above for screenshots, troubleshooting, and in-depth explanations of mapper forcing, UF2 structure, and limitations.

## Explorer Firmware (RP2350)

|Explorer Flash Menu|Explorer MicroSD Menu|
|---|---|
|<img src="/images/2026-05-17_21-26.png" alt="Explorer flash menu" width="420" height="315">|<img src="/images/2026-05-17_21-28.png" alt="Explorer microSD menu" width="420" height="315">|
|<center>**File Hunter Explorer Menu**|<center>**WiFi Configuration Screen**|
|<img src="/images/2026-05-17_21-30.png" alt="File Hunter Explorer menu" width="420" height="315">|<img src="/images/2026-05-17_21-31.png" alt="WiFi configuration screen" width="420" height="315">|


Explorer is a PicoVerse 2350-only firmware that merges ROMs stored in flash, optional embedded Sunrise Nextor SYSTEM entries, and additional ROMs and MP3/WAV files on the microSD card. ROMs are labeled with source tags (FL/SD), MP3 and WAV entries open a player screen with Play/Stop and Pause/Resume controls, the list supports paging, and you can search by name directly in the menu. Explorer can add individual Nextor entries with `-s1`, `-m1`, `-c1`, `-r1`, `-s2`, `-m2`, `-c2`, or `-r2`, or add all of them with `-a` / `--allnextor`, while still appending supported folder ROMs to the UF2. In the `F2` microSD screen, `P` cycles supported FAT16, FAT32, and exFAT browsing partitions, while `D` deletes the selected file after confirmation; folders are protected. With an ESP-01 / ESP8266 module installed and WiFi configured, pressing `F3` opens the integrated File Hunter browser. File Hunter results show the ROM name and size, can be searched from the MSX, and selected ROMs are downloaded through the Pico into PSRAM before being saved as `.ROM` files in the microSD root. microSD ROMs up to 4 MB can be executed directly from there. Use the Explorer tool to build the UF2 and copy extra ROMs and audio files to the microSD card. See the Explorer manual for limits, File Hunter requirements, and supported formats.

For Sunrise Nextor SYSTEM entries in Explorer, SD storage is limited to FAT16 microSD partitions up to 4 GB. If more than one compatible partition exists, choose the desired one from the ROM detail screen's `SD Part` option; Explorer saves that selection in the ROM's `.PVC` options file. FAT32 and exFAT partitions remain supported for normal Explorer file browsing but are not offered to Nextor.

You can have up to 1024 entries per folder view (folders + ROMs + MP3s; the root view can also include flash entries). The menu auto-detects whether the MSX supports 80-column text mode and boots accordingly; you can also press `C` at any time to toggle between 40- and 80-column layouts.

Explorer ROM entries open a detail screen where you can inspect mapper detection, choose a cartridge audio profile, and toggle **PSG** mirroring. Konami SCC and Manbow2 ROMs can use SCC/SCC+ profiles. For other supported non-SYSTEM ROMs, select **Dual PSG** to enable the second cartridge-side PSG on ports `0x10` and `0x11`, **MSX-MUSIC** to enable YM2413/FM-PAC audio, external **SCC/SCC+** to expose a virtual SCC cartridge in another subslot, or **YM2151 (SFG05/SFG01)** to expose a virtual Yamaha SFG-style YM2151 cartridge in a secondary subslot while the game mapper remains active. Supported Sunrise Nextor SYSTEM entries can also select external **SCC/SCC+** or **YM2151 (SFG05/SFG01)**; Explorer keeps Nextor storage and mapper RAM availability intact and places the added cartridge surface in a free expanded subslot. MegaRAM Nextor entries reserve their expanded layout for Nextor, mapper RAM, and MegaRAM, so PSG Mirror, MSX-MUSIC, WiFi, and external audio profiles are not offered for those entries. Set **PSG: Yes** to also mirror the normal primary PSG ports `0xA0`/`0xA1` through the cartridge DAC; this primary PSG mirror can be mixed with SCC/SCC+, Dual PSG, MSX-MUSIC, YM2151/SFG, or WAVEGAME WAV playback. When **Dual PSG** and **PSG: Yes** are enabled together, Explorer routes Dual PSG to the left channel and the mirrored primary PSG to the right channel. Explorer hides cartridge audio profiles where the cartridge audio slot is reserved, including folders and File Hunter folder records.

WAVEGAME support is available for microSD-launched game ROMs that keep their audio sidecars in the same folder. The running MSX game writes one-byte commands to I/O port `0x92`; Explorer captures those writes while serving the ROM and streams `NN.wav`, `multi.wav`, or `pause.wav` files from the microSD card through the I2S DAC. The protocol supports stop/fade-out, play once, loop, pause toggle, pause music, previous-song resume with fade-in, deferred next-command playback, optional per-song `.cfg` sample offsets, and `multi.wav` fallback offsets. WAVEGAME titles should use 48 kHz mono 16-bit PCM WAV files. When Explorer detects WAVEGAME sidecars next to a ROM, cartridge audio profiles are forced to `None` because they are incompatible with the WAVEGAME streaming path; the separate **PSG Mirror** option remains available and is mixed into the WAVEGAME WAV output.

A search function is available by pressing `/` in the menu. Type part of a ROM name and press Enter to jump to the first matching entry. Press `H` to view the help screen.

The File Hunter browser was implemented with reference to NataliaPC's public MSX File Hunter Browser project: https://github.com/nataliapc/msx_filehunterbrowser

## Compatibility & Requirements

- Works with MSX, MSX2, MSX2+, and MSX TurboR systems. Mapper support covers the most common game and utility formats.
- Requires Windows OS to run the PC-side UF2 builder utilities.
- Ensure your development board matches the pinout documented for each hardware revision before soldering.

## License, Copyright notes & Usage

![Creative Commons Attribution-NonCommercial-ShareAlike 4.0](/images/ccans.png)

All hardware and firmware binaries in this repository are released under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International license. Personal builds and community tinkering are encouraged, but **commercial use or resale requires explicit authorization from the author**.

**bios.h** is used on all MSX menus and was adapted from http://www.konamiman.com/msx/msx2th/th-ap.txt by Danilo Angelo, 2020. The original file is licensed under CC0 1.0 Universal (public domain). The adapted bios.h file in this repository is also released under CC0 1.0 Universal, allowing for free use and modification without restrictions.

**msxromcrt0.s** is used on Nextor C implementation and was adapted by S0urceror, 2022, from the crt0.s file available in FusionC v2.0. The original crt0.s file is licensed under the MIT License. The adapted msxromcrt0.s file in this repository is also released under the MIT License, permitting free use, modification, and distribution with proper attribution.

**uf2format.h** is used in all UF2 builder tools and was adapted from the UF2 specification and reference implementation available at https://github.com/Microsoft/uf2/blob/master/uf2.h. The original uf2.h is copyright by Microsoft Corp and the file is licensed under the MIT License. The adapted uf2format.h file in this repository is also released under the MIT License, allowing for free use, modification, and distribution with proper attribution.

**sha1.h** provides the minimal SHA-1 implementation used by the PicoVerse PC tools. It is based on Steve Reid's public-domain SHA-1 implementation <sreid@sea-to-sky.net>.

**emu2212.c** and **emu2212.h** used to implement SCC and SCC+ emulation are copyright by Mitsutaka Okazaki 2014 and licensed under the MIT License, allowing for free use, modification, and distribution with proper attribution. [emu2212 @ Digital Sound Antiques](https://github.com/digital-sound-antiques/emu2212)

**emu2149.c** and **emu2149.h** used to implement AY-3-8910 emulation/dual PSG feature are copyright by Mitsutaka Okazaki 2014 and licensed under the MIT License, allowing for free use, modification, and distribution with proper attribution. [emu2149 @ Digital Sound Antiques](https://github.com/digital-sound-antiques/emu2149)

**emu2413.c** and **emu2413.h** used to implement YM2413 / MSX-MUSIC emulation are copyright by Mitsutaka Okazaki 2020 and licensed under the MIT License, allowing for free use, modification, and distribution with proper attribution. [emu2413 @ Digital Sound Antiques](https://github.com/digital-sound-antiques/emu2413)

**The YM2151 / Yamaha SFG audio backend** uses the `ymfm` Yamaha FM sound cores by Aaron Giles, licensed under the BSD 3-Clause License. PicoVerse vendors the OPM/YM2151 subset under `2350/software/explorer.pio/pico/explorer/ymfm/` and wraps it with `emu2151.cpp` / `emu2151.h` so the Explorer SFG register implementation can use the same internal `OPM_*` API. [ymfm](https://github.com/aaronsgiles/ymfm)

**The Yamaha SFG01/SFG05 cartridge** behavior was implemented using public technical references including the RBSC SFG_Cartridge project and openMSX's Yamaha SFG implementation. PicoVerse exposes an SFG-like memory-mapped register window in a secondary expanded subslot, queues complete YM2151 register/value writes for the audio core. `SFG_64K.ROM` is bundled as a hidden Explorer flash payload for the YM2151/SFG profiles; the first 32K image is exposed for SFG05 and the second 32K image is exposed for SFG01. The SFG BIOS ROMs were created by Yamaha Corporation and must be treated as third-party copyrighted BIOS payloads. 

**WAVEGAME support on PicoVerse 2350 Explorer** implements the WAVEGAME idea and protocol authored by Jeroen Taverne and Mauricio Braga. PicoVerse keeps its own RP2350 Explorer implementation for ROM serving, port `0x92` command capture, WAV streaming, and optional PSG Mirror mixing. Implementation details were informed by the openMSX pull request [Add Wave Game audio cartridge support by MBilderbeek · Pull Request #2126 · openMSX/openMSX](https://github.com/openMSX/openMSX/pull/2126).

**MegaRAM support on PicoVerse 2350 LoadROM and Explorer** is a PicoVerse PSRAM-backed implementation of the MSX MegaRAM concept created by Ademir Carchano in Brazil in 1987. The implemented banking behavior follows the Cartucho II / SDMapper-compatible MegaRAM model: four 8KB windows in `0x4000`-`0xBFFF`, bank latches selected by address bits A14:A13, volatile RAM contents, and loader-controlled write enable through `IN (0x8E/0x8F)` with write disable through `OUT (0x8E/0x8F)`. The PicoVerse implementation is original firmware code and is provided as a tribute to Ademir Carchano's contribution to the MSX community.

**FMPCCMFC.BIN** is the English FM-PAC BIOS used by PicoVerse 2350 LoadROM when `-f` / `-fmpac` is selected. The FM-PAC BIOS ROM is copyrighted by Matsushita Corp.; the English translation is credited to 232, Max Iwamoto, and GDX.

**The Yamanooto flash-cartridge emulation on PicoVerse 2350** reproduces the *Yamanooto* cartridge (a Konami-SCC compatible 8 MB flash-ROM with SCC/SCC+ and a secondary PSG). The register map, mapper/offset behaviour, SCC/SCC+ activation rules, and FPGA ID handshake were implemented with reference to openMSX's Yamanooto device implementation ([Yamanooto.cc](https://github.com/openMSX/openMSX/blob/master/src/memory/Yamanooto.cc)) and public Yamanooto programming notes ([Programming the Yamanooto](https://genami.shop/blogs/news/programming-the-yamanooto)). PicoVerse's SCC/SCC+ uses **emu2212**, the dual/mirror PSG uses **emu2149**, and the added MSX-MUSIC engine uses **emu2413** (all © Mitsutaka Okazaki, MIT License). The embedded FM-PAC BIOS (`FMPCCMFC.BIN`) is credited as above. The RP2350 bus/PIO handling, the runtime SCC/FM/PSG engine selector, and the audio pipeline are original PicoVerse work licensed under CC BY-NC-SA 4.0. The physical Yamanooto cartridge is a product of the SCC Alliance; PicoVerse is an independent emulation and is not affiliated with or endorsed in any way.

**MIDI-PAC PSG-to-MIDI Conversion:**  
The MIDI-PAC firmware and its quality improvements were developed against public technical references and behavior studies. The PSG-to-MIDI mapping combines hardware-correct AY-3-8910 / YM2149 interpretation with pragmatic General MIDI reinterpretation for musical output on modules like the Roland SC-55. Key references used include:  
- The `berarma/aymidi` project by berarma (AY-3-8910 to MIDI conversion reference).  
- The `ayumi` AY/YM2149 emulator by Peter Sovietov (Peter Sovietov, 2022; used for envelope generation verification).  
- The ESEMSX3 FPGA implementation `midi.vhd` (serves as a minimal alternative PSG-to-MIDI transport reference).  
- AY-3-8910 and YM2149 datasheets and hardware behavior documentation (General Instruments and Yamaha specifications).  
- MSX-MIDI specification and USB MIDI 1.0 Event Packet format (USB Implementers Forum).  
- The Sunrise IDE specification and Nextor protocol documentation.  

**The OPL4 / YMF278B / MoonSound implementation on PicoVerse 2350 LoadROM** is an original PicoVerse standalone cartridge firmware written for the RP2350. PicoVerse implements the MSX/MoonSound host side itself: PIO bus capture for ports `0xC4`-`0xC7` and `0x7E`/`0x7F`, `/WAIT`-gated reads, `/BUSDIR` drive, register/status shadows, FM timer `/INT` delivery, YRW801-M wave-ROM exposure, and 2 MB PCM sample RAM.

**YMF278B synthesis** uses Aaron Giles' `ymfm` Yamaha sound cores, licensed under the BSD 3-Clause License, including the OPL/FM and PCM support wrapped by PicoVerse's `emu278b` interface for timer, IRQ, ROM, and sample-RAM behavior. The MoonSound/WozBlaster-compatible port map, `/BUSDIR` behavior, 2 MB YRW801-M ROM plus 2 MB RAM model, and compatibility work were informed by public technical references including the Wozblaster OPL4_Cartridge project and openMSX's OPL4 implementation. The bundled `YRW801-M` instrument ROM is a Yamaha 1993 third-party ROM payload and must be treated as copyrighted material. PicoVerse's bus firmware, cartridge integration, synchronization model, and RP2350 audio pipeline are original PicoVerse work and are licensed under CC BY-NC-SA 4.0, allowing for non-commercial use and modification with proper attribution; commercial use or resale requires explicit authorization from the author.

**The MSX-AUDIO / Yamaha Y8950 implementation on PicoVerse 2350 LoadROM** (`loadrom.exe -a`) is an original PicoVerse standalone cartridge firmware written for the RP2350. Unlike the OPL4 build it answers both memory and I/O cycles: PicoVerse implements the slot memory decode for the MSX-Audio BIOS plus base and expanded work RAM, the `0xC0`/`0xC1` port handling with `/WAIT`-gated reads and `/BUSDIR` drive, Y8950 FM timer `/INT` delivery, and 256 KB of ADPCM sample RAM in PSRAM, all in a single-owner core loop feeding the I2S DAC at the Y8950 native 49716 Hz rate.

**Y8950 synthesis** uses Aaron Giles' `ymfm` Yamaha sound cores (BSD 3-Clause License), specifically the `y8950` OPL1 + ADPCM-B core, timers, IRQ, busy behavior, and I/O pins. The cartridge model \u2014 slot memory map, `Normal0000` BIOS layout, work-RAM mirrors, status-bit behavior, and the 256 KB SampleRAM configuration \u2014 reproduces the openMSX extension `Boosted_audio.xml` ("MSX-Audio" by Philips/MSXPro) and openMSX's `MSXAudio` / `Y8950Periphery` implementations, used as a hardware-behavior reference only (openMSX is GPLv2+). The bundled `MSXAUDIO_NMS1205.ROM` is the MSX-Audio BIOS v1.3 for the Philips NMS-1205 and is a third-party copyrighted BIOS payload. PicoVerse's bus firmware, decode tables, synchronization model, and RP2350 audio pipeline are original PicoVerse work licensed under CC BY-NC-SA 4.0.

**MSX-MIDI Interface:**  
The MSX-MIDI firmware was developed using the MSX-MIDI specification, USB MIDI 1.0 specification, TinyUSB host API, and reference behavior from ESEMSX3 and similar implementations. The 8251 USART emulation and standard MIDI wire protocol handling are original PicoVerse implementations.

**The Sunrise IDE driver for Nextor** used on PicoVerse is copyright by Konamiman, Piter Punk, and FRS, and is licensed under the special terms by MSX Licensing Corporation. The original Sunrise IDE code is available at https://github.com/Konamiman/Nextor/blob/v2.1/source/kernel/drivers/SunriseIDE/sunride.asm

The algorithm to emulate ATA devices is original and based on the implementation for the Carnivore2 cartridge, Copyright (c) 2017-2024 by the RBSC group. Portions (c) Mitsutaka Okazaki and (c) Kazuhiro Tsujikawa. Available at https://github.com/RBSC/Carnivore2/tree/master/Firmware/Sources

**WiFi support on PicoVerse 2350** was implemented with direct reference to the ESP8266 / MSX WiFi work by Oduvaldo Pavan Junior (`ducasp`). The PicoVerse WiFi ROM integration, memio register layout, fixed-`859372` UART behavior, FIFO clear command handling, quick-receive behavior, and ESP-side protocol compatibility were based on public source and documentation from Oduvaldo's projects. Important references used include:
- `MSXPICO/UNAPI_BIOS_CUSTOM_ESP_MSXPICO/ESP8266_memio.asm` from `ducasp/MSX-Development` for the memory-mapped ROM and register model.
- `MSX-SM/WiFi/UNAPI_BIOS_CUSTOM_ESP_FIRMWARE` and related WiFi utilities from `ducasp/MSX-Development` for the ROM/driver lineage and behavior.
- `ducasp/ESP8266-UNAPI-Firmware` and its documentation for the serial protocol, command/response structure, and ESP firmware behavior.

Those projects remain copyright by Oduvaldo Pavan Junior and their respective contributors under their original terms. PicoVerse reuses the public technical design references and keeps its own RP2350 cartridge-side implementation.

**File Hunter browser integration on PicoVerse 2350 Explorer** was implemented with reference to NataliaPC's MSX File Hunter Browser project and the public File Hunter service behavior. PicoVerse keeps its own Explorer MSX menu and RP2350/ESP8266 transport implementation, while acknowledging the original MSX File Hunter Browser code and workflow by NataliaPC. Reference repository: https://github.com/nataliapc/msx_filehunterbrowser

**Mapper detection heuristics and filename tag forcing schemes** in the LoadROM and MultiROM tools are original implementations by the OpenMSX developers for the Romfactory module and licensed under the GNU Public License (GPL). Available at https://github.com/openMSX/openMSX/blob/6f8aa9865eeccdb0b31043d8851b822538440204/src/memory/RomFactory.cc

**The softwaredb database** used for ROM mapper detection is built upon contributions from across the MSX community, including the initial work by Nicolas Beyaert, later expanded by the BlueMSX Team (2004–2013) and continuously maintained by the openMSX Team (2005–present). It also incorporates MSX ID data generated by Generation MSX (www.generation-msx.nl). The latest updates are based on the awesome work by Vampier, with the database available at https://romdb.vampier.net/. Special thanks go to the Generation MSX / Sylvester project for its extensive reference data, as well as to contributors such as p_gimeno and diedel for ROM additions and validation, and GDX for further ROM information, corrections, and verification efforts. Part of openMSX effort. Available at https://github.com/openMSX

**The per-ROM 50/60 Hz (VDP frequency) feature** in PicoVerse 2350 Explorer was implemented with reference to the `50-60hz` project by sdsnatcher73, which demonstrates how to switch the V9938/V9958 VDP line frequency (Register #9, PAL/NTSC bit) reliably on MSX without corrupting the screen. That project is licensed under the Apache License 2.0 and could not have been created without the help of gdx, Grauw, and NYRIKKI, as credited by its author. The application technique was further refined after studying the strategy used by the **Carnivore2** cartridge boot menu (`BOOTCMFC.ASM`, by the RBSC group): using the BIOS `WRTVDP` routine (`0x0047`) so the R9 change also updates the `RG9SAV` shadow at `0xFFE8` for better game compatibility, and restricting the frequency change to MSX2 and later machines. Only the publicly available Carnivore2 technique was used as a reference. PicoVerse keeps its own RP2350 cartridge-side implementation (a per-ROM option persisted in `.PVC` files and applied by injecting a small VDP R9 write into the launched game's cartridge INIT), while gratefully acknowledging sdsnatcher73's and RBSC's reference work. Reference repositories: https://github.com/sdsnatcher73/50-60hz and https://github.com/RBSC/Carnivore2

**The per-ROM CPU speed feature** in PicoVerse 2350 Explorer was implemented with reference to two public MSX projects. `Z80-R800` by GDX (https://github.com/gdx-msx/Z80-R800) provides MSX-DOS/BASIC commands that switch the MSX turbo R CPU mode and demonstrates the `CHGCPU` (`#0180`) call convention along with the MSXVER check used to detect a turbo R. `msx-turbo` by Papipapito (https://github.com/Papipapito/msx-turbo), licensed under the MIT License, is a command-line 3.58/5.37 MHz switch for Panasonic MSX2+ machines and documents the switched-I/O device 8 protocol on ports `#40`/`#41` and its polarity. The port and BIOS definitions were cross-checked against the MSX Assembly Page (https://map.grauw.nl). PicoVerse keeps its own RP2350 cartridge-side implementation (a per-ROM option persisted in `.PVC` files and applied by injecting the switch into the launched game's cartridge INIT), while gratefully acknowledging GDX's and Papipapito's reference work.

## Feedback & Community

Questions, test reports, and build photos are welcome. Open an issue on the public repository or reach out through the MSX retro hardware forums where PicoVerse updates are posted.

Author: Cristiano Goncalves
Last updated: 08/22/2026
