# MSX PicoVerse 2350 MultiROM Tool Manual (EN-US)

The PicoVerse 2350 MultiROM tool creates a UF2 image that flashes the PicoVerse 2350 cartridge with:
- The PicoVerse 2350 MultiROM firmware.
- The MSX MultiROM menu ROM.
- A configuration table describing each ROM entry.
- The ROM payloads themselves.

Use this tool when you want to bundle multiple MSX ROM files into a single UF2 for the PicoVerse 2350.

## Requirements

- A Windows PC (the distributed binary is a Windows console application).
- Your MSX ROM files in a single folder (no subdirectories).
- A PicoVerse 2350 cartridge and a USB-C cable.

## Limits

- Maximum ROM files per image: 128.
- Maximum combined ROM payload size: ~14 MB.
- Per-ROM name length in menu: 50 characters (longer names are truncated in the menu list).

## Basic workflow

1. Place all `.ROM` files you want to include in a single folder.
2. Run the tool in that folder.
3. Copy the generated UF2 file to the PicoVerse 2350 in BOOTSEL mode.
4. Insert the cartridge into the MSX and power on to access the MultiROM menu.

## Command-line usage

```
multirom.exe [options]
```

### Options

| Flag | Description |
|------|-------------|
| `-h`, `--help` | Show usage help and exit. |
| `-o <filename>`, `--output <filename>` | Set UF2 output filename (default `multirom.uf2`). |
| `-a`, `--allnextor` | Include every embedded Nextor entry below (`-s1`, `-m1`, `-s2`, `-m2`, `-c1`, `-c2` and `-s3`) while still scanning and appending `.ROM` files from the current folder. |
| `-s1`, `--sunrise-sd` | Include Sunrise IDE Nextor ROM (microSD card). |
| `-m1`, `--mapper-sd` | Include Sunrise IDE Nextor ROM + 1MB PSRAM mapper (microSD card). |
| `-s2`, `--sunrise-usb` | Include Sunrise IDE Nextor ROM (USB pendrive). |
| `-m2`, `--mapper-usb` | Include Sunrise IDE Nextor ROM + 1MB PSRAM mapper (USB pendrive). |
| `-c1`, `--carnivore2-sd` | Include Sunrise IDE Nextor ROM + 1MB PSRAM mapper + Carnivore2 RAM-mode emulation (microSD card). |
| `-c2`, `--carnivore2-usb` | Include Sunrise IDE Nextor ROM + 1MB PSRAM mapper + Carnivore2 RAM-mode emulation (USB pendrive). |
| `-s3`, `--sunrise3-sd` | Include Nextor Sunrise 3.0.0 Beta 2 (microSD card), for testing the upcoming Nextor 3. Listed as `Nextor Sunrise 3.0.0 Beta 2 (SD)`. |
| `-scc`, `--scc` | Enable SCC sound emulation for Konami SCC and Manbow2 ROMs, and for ROMs uploaded via SROM in `-c1`/`-c2` Carnivore2 modes. |
| `-sccplus`, `--sccplus` | Enable SCC+ (enhanced) sound emulation for Konami SCC and Manbow2 ROMs, and for ROMs uploaded via SROM in `-c1`/`-c2` Carnivore2 modes. |
| `-w`, `--wifi` | Add ESP-01 WiFi BIOS sub-slot to every `-s1`/`-m1`/`-s2`/`-m2`/`-s3` Nextor entry. Requires at least one of those flags (or `-a`); the WiFi sub-slot is not added to `-c1`/`-c2` Carnivore2 entries. |

The Sunrise / Carnivore2 options can be freely combined. Each adds a separate SYSTEM entry to the menu, so you can pick the desired Nextor / Carnivore2 mode on the MSX. When `-scc` or `-sccplus` is used together with `-c1`/`-c2`, the processing list shows the Carnivore2 entry with a trailing `+SCC` or `+SCC+` marker (the SCC flag is encoded in the entry's mapper byte and applies only to the Carnivore2 entries; other Sunrise modes are unaffected). When `-w` is combined with `-s1`/`-m1`/`-s2`/`-m2`/`-s3`, those entries are listed with a trailing `+WiFi` marker and the embedded ESP8266P system ROM is appended right after each entry's Sunrise ROM payload in flash.

### Examples

Scan for ROMs with no Nextor entries:

```
multirom.exe
```

Include both Sunrise IDE SD and Sunrise IDE USB entries alongside scanned ROMs:

```
multirom.exe -s1 -s2
```

Include Sunrise IDE + mapper over microSD, with SCC emulation enabled for compatible ROMs:

```
multirom.exe -m1 -scc
```

Include Carnivore2 RAM-mode (USB) with SCC+ emulation for SROM-uploaded Konami SCC ROMs, alongside scanned ROMs:

```
multirom.exe -c2 -sccplus
```

Include Sunrise IDE + mapper over USB with ESP-01 WiFi support, alongside scanned ROMs:

```
multirom.exe -m2 -w
```

Include every Nextor entry (Nextor 2.1.4 on microSD and USB, the Carnivore2 modes, and the Nextor 3.0.0 Beta 2 microSD entry), alongside scanned ROMs:

```
multirom.exe -a
```

Test Nextor 3 next to Nextor 2.1.4 on the same microSD card:

```
multirom.exe -s1 -s3
```

## Flashing the UF2

1. Hold BOOTSEL on the PicoVerse 2350 while connecting USB.
2. The `RPI-RP2` drive appears.
3. Copy `multirom.uf2` to the drive.
4. The drive disconnects automatically after flashing.

## ROM mapper detection

The tool analyzes each ROM to determine the correct mapper type, applying the following steps in order:

1. **SHA-1 database lookup** — The ROM's SHA-1 hash is checked against an embedded database derived from the openMSX `softwaredb.xml`. A match overrides the heuristic and returns the definitive mapper type.

2. **Signature checks** — Special byte signatures are checked:
   - 16 KB ROM with `AB` header → Plain16.
   - ≤ 32 KB ROM with `AB` header → Plain32 (or Planar48 if `AB` also at 0x4000).
   - `ASCII16X` at offset 16 → ASCII16-X.
   - `ROM_NEO8` / `ROM_NE16` at offset 16 → NEO-8 / NEO-16.
   - 512 KB ROM with `Manbow 2` at offset 0x28000 → Manbow2.
   - ≤ 48 KB ROM with `AB` at 0x4000 → Planar48.
   - 64 KB ROM with `AB` at 0x4000 → Planar64.

3. **Heuristic scoring** — For ROMs larger than 32 KB, the tool scans for Z80 `ld (nnnn),a` instructions referencing mapper register addresses. Each candidate address increments the score for its associated mapper type. The mapper with the highest score wins (ties broken by later type, matching openMSX).

4. **Fallback** — 64 KB ROMs with an `AB` header and no detected mapper writes are classified as Planar64. Larger ROMs with no detected writes use raw address density as a secondary hint.

## Mapper tag forcing

If auto-detection fails or you want to override it, add a mapper tag to the filename before `.ROM`. Tags are case-insensitive.

Forceable tags:

| Tag | Mapper | Aliases |
|-----|--------|---------|
| `PLA-16` | Plain 16 KB | `PL-16` |
| `PLA-32` | Plain 32 KB | `PL-32` |
| `KonSCC` | Konami SCC | |
| `PLN-48` | Planar 48 KB | `PL-48`, `LINEAR`, `LINEAR0`, `PLANAR48`, `PLN-32`, `PLANAR32` |
| `ASC-08` | ASCII 8 KB | |
| `ASC-16` | ASCII 16 KB | |
| `ASC16X` | ASCII16-X (flash) | |
| `Konami` | Konami (no SCC) | |
| `NEO-8` | NEO 8 KB | |
| `NEO-16` | NEO 16 KB | |
| `PLN-64` | Planar 64 KB | `PL-64`, `PLANAR64` |
| `MANBW2` | Manbow2 | `MANBOW2`, `MBW-2` |

`SYSTEM` tags are ignored (Sunrise IDE entries cannot be forced via filename).

Example:

```
Knight Mare.PLA-32.ROM
```

## Supported mapper types

| # | Type | Tag | Notes |
|---|------|-----|-------|
| 1 | Plain 16 KB | `PLA-16` | Single 16 KB bank at 4000h–7FFFh |
| 2 | Plain 32 KB | `PLA-32` | 32 KB at 4000h–BFFFh |
| 3 | Konami SCC | `KonSCC` | 8 KB banking; SCC auto-enabled |
| 4 | Planar 48 KB | `PLN-48` | Linear mapping for 32/48 KB ROMs with AB at 0x4000 |
| 5 | ASCII 8 KB | `ASC-08` | 8 KB bank switching |
| 6 | ASCII 16 KB | `ASC-16` | 16 KB bank switching |
| 7 | Konami | `Konami` | 8 KB banking without SCC |
| 8 | NEO 8 KB | `NEO-8` | FlashROM-compatible 8 KB banking |
| 9 | NEO 16 KB | `NEO-16` | FlashROM-compatible 16 KB banking |
| 10 | Sunrise IDE (USB) | SYSTEM | Nextor 2.1.4 Sunrise IDE via USB (`-s2`) |
| 11 | Sunrise IDE + Mapper (USB) | SYSTEM | Nextor + 1MB PSRAM mapper via USB (`-m2`) |
| 12 | ASCII16-X | `ASC16X` | ASCII16 + SST-compatible flash commands |
| 13 | Planar 64 KB | `PLN-64` | Full 64 KB linear mapping (0000h–FFFFh) |
| 14 | Manbow2 | `MANBW2` | Konami SCC banking + AM29F040B flash emulation |
| 15 | Sunrise IDE (SD) | SYSTEM | Nextor 2.1.4 Sunrise IDE via microSD (`-s1`); also used by the Nextor 3.0.0 Beta 2 entry (`-s3`) |
| 16 | Sunrise IDE + Mapper (SD) | SYSTEM | Nextor + 1MB PSRAM mapper via microSD (`-m1`) |
| 17 | Carnivore2 (SD) | SYSTEM | Nextor + 1MB mapper + Carnivore2 RAM-mode emulation via microSD (`-c1`) |
| 18 | Carnivore2 (USB) | SYSTEM | Nextor + 1MB mapper + Carnivore2 RAM-mode emulation via USB (`-c2`) |

## SCC / SCC+ emulation

- ROMs detected or forced as `KonSCC` (mapper 3) or `MANBW2` (mapper 14) can have SCC/SCC+ sound emulation.
- For Carnivore2 modes (`-c1` / `-c2`), the SCC flag applies to ROMs uploaded into the cartridge through the Carnivore2 SROM utility (`c2ramldr`) — the emulator listens to the standard Konami SCC / SCC+ register windows whenever an SCC-class ROM has been written to PSRAM.
- Use `-scc` for standard SCC emulation or `-sccplus` for enhanced SCC+ mode.
- The `-scc` and `-sccplus` flags are mutually exclusive.
- The flags apply globally to the build; non-SCC mapper types ignore them.
- Audio is output through the I2S DAC connected to the RP2350.

## Sunrise IDE Nextor

The Sunrise IDE options embed the Nextor 2.1.4 Sunrise IDE kernel ROM (128 KB) into the MultiROM image. Each option adds a SYSTEM entry that appears in the MSX menu alongside regular ROM entries.

### Nextor 3 beta (`-s3`)

- Embeds Konamiman's Nextor 3.0.0 Beta 2 Sunrise IDE MasterOnly kernel (128 KB, from the [SunriseIDE Nextor driver](https://github.com/Konamiman/SunriseIDE-Nextor-driver/releases/tag/v0.1.8-blueMSX-v0.1.5-Nextor-3.0-beta.2) release) instead of Nextor 2.1.4.
- microSD only for now; it uses the same firmware path as `-s1`, so everything in the microSD section below applies.
- It is a beta kernel: keep backups of the microSD card.

### USB modes (`-s2`, `-m2`)

- Storage is accessed via USB mass storage (flash drives, USB-to-SD adapters) through the cartridge's USB-C port.
- Core 1 runs the TinyUSB USB host stack with asynchronous MSC read/write.
- Device info for ATA IDENTIFY comes from the USB SCSI INQUIRY response.

### microSD modes (`-s1`, `-m1`, `-s3`)

- Storage is accessed via the on-board microSD card slot (SPI at 31.25 MHz).
- Real card identification (OEM, product name, revision) from the SD CID register is shown during Nextor boot.

### Mapper modes (`-m1`, `-m2`)

- Combines Nextor Sunrise IDE with a 1MB PSRAM memory mapper (64 × 16 KB pages).
- Uses an expanded sub-slot architecture: sub-slot 0 serves the Nextor ROM, sub-slot 1 provides mapper RAM.
- Mapper page registers (I/O ports FC–FF) are intercepted via PIO1.
- A bootstrap ROM phase ensures a clean cold-boot before the expanded-slot mapper is activated.

### Carnivore2 modes (`-c1`, `-c2`)

- Builds on the `-m1` / `-m2` infrastructure and adds an emulation of the Carnivore2 RAM-mode cartridge so SROM utilities (`c2ramldr`, `bdosldr`, etc.) can upload Konami / ASCII / Sunrise-mapper ROMs into the cartridge's PSRAM and launch them.
- Sub-slot layout matches the real Carnivore2 (`Sltsl_C` / `Sltsl_D` / `Sltsl_M`):
  - Sub-slot 0: Carnivore2 register window + bank windows (1 MB SROM upload target).
  - Sub-slot 1: Nextor 2.1.4 Sunrise IDE BIOS (USB or microSD backend, same as `-s1`/`-s2`).
  - Sub-slot 2: 1 MB MSX memory mapper RAM (same as `-m1`/`-m2`).
- Implements the Carnivore2 register block (`CardMDR`, `R1..R4` configs with `Mask/Addr/Reg/Mult/MaskR/AdrD`, `AddrFR`), the port `0xF0+PFXN` `'C'/'S'` handshake used by SROM, the AMD autoselect chip-ID probe (`M29W640GB`) used by `c2ramldr`, and the `CMFCCFRC` firmware signature overlay at `0x4010-0x4017`.
- When `-scc` or `-sccplus` is added, the cartridge also emulates the Konami SCC / SCC+ register window so SROM-uploaded SCC-class ROMs play music. Audio is mixed by the same I2S path used by the regular `-scc` modes (PIO1 SM2 + auto-claimed DMA channel; service is polled from the storage Core 1 task loop so it does not block USB / SD I/O).
- The 1 MB Carnivore2 RAM region is allocated from the same PSRAM bump allocator that backs the mapper RAM, so the cartridge needs the on-board PSRAM to be present and initialised. If allocation fails the firmware halts at the dispatch entry instead of booting.

## Menu usage (on MSX)

- Up/Down: move selection.
- Left/Right: change pages.
- Enter/Space: load selected ROM.
- A-Z: jump to the first ROM whose name starts with that letter (case-insensitive), switching to its page if needed.
- F1: show help screen.

SYSTEM entries (Sunrise IDE) appear alongside regular ROM entries and are selected the same way.

## Notes

- Only ROM files in the current folder are included (no subfolders).
- Unsupported or invalid ROMs are skipped with a message.
- Manbow2 save data is volatile (SRAM-backed, lost on power-off).
- ASCII16-X flash write emulation is SRAM-backed (volatile).
- The Sunrise options (`-s1`, `-m1`, `-s2`, `-m2`, `-c1`, `-c2`) can be combined — each adds its own menu entry.

Author: Cristiano Almeida Goncalves
Last updated: 04/26/2026
