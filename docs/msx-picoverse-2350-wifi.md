# MSX PicoVerse 2350 WiFi Support

This document describes the ESP-01 WiFi support currently implemented for the PicoVerse 2350 firmware family.

The PicoVerse 2350 WiFi implementation has two user-facing surfaces: an ESP8266P-compatible system ROM and memio UART bridge on top of the Sunrise IDE Nextor modes, and the integrated Explorer File Hunter browser that uses the ESP-01 module directly from the Pico firmware.

## 1. Overview

WiFi support is enabled from the RP2350 LoadROM tool with the `-w` or `--wifi` flag.

The flag is currently valid only with these four firmware modes:

| LoadROM Option | Storage Backend | Mapper | WiFi Support |
|---|---|---|---|
| `-s1 -w` | microSD | none | yes |
| `-m1 -w` | microSD | 1MB PSRAM mapper | yes |
| `-s2 -w` | USB mass storage | none | yes |
| `-m2 -w` | USB mass storage | 1MB PSRAM mapper | yes |

WiFi support is available in both LoadROM (`loadrom.exe -w`) and MultiROM (`multirom.exe -w`) for Sunrise IDE modes. Explorer also uses the ESP-01 module for File Hunter browsing and exposes WiFi setup through `F4`; this Explorer path is built into the Explorer firmware and does not use a `-w` tool option.

The Sunrise IDE `-w` option is not currently exposed in:

- `-c1` / `-c2` Carnivore2-compatible RAM loader modes

In practical terms, `-w` keeps the normal Nextor boot/storage behavior and adds a second ROM/interface surface for WiFi-aware MSX software.

## 2. What The Feature Adds

When `-w` is enabled, the generated UF2 contains:

- The existing PicoVerse 2350 Sunrise IDE firmware
- The embedded Nextor 2.1.4 Sunrise IDE ROM
- A WiFi flag in the configuration record (`0x20`)
- An embedded 16KB ESP8266P system ROM
- A memory-mapped UART backend that services the ESP ROM protocol through the installed ESP-01 module

The host tool prints this explicitly when building the image:

```text
WiFi Support: Enabled (ESP8266P system ROM + UART support)
```

## 3. User-Facing Build Commands

Typical commands are:

```bash
# Nextor via microSD + WiFi
loadrom.exe -s1 -w -o nextor_sd_wifi.uf2

# Nextor via microSD + 1MB mapper + WiFi
loadrom.exe -m1 -w -o nextor_mapper_sd_wifi.uf2

# Nextor via USB + WiFi
loadrom.exe -s2 -w -o nextor_usb_wifi.uf2

# Nextor via USB + 1MB mapper + WiFi
loadrom.exe -m2 -w -o nextor_mapper_usb_wifi.uf2
```

The `-w` flag is rejected for any other mode:

```text
Error: -w/--wifi is supported only with -s1, -m1, -s2 or -m2.
```

## 4. Hardware Context

The PicoVerse 2350 hardware includes an ESP-01 header. The WiFi implementation assumes:

- A PicoVerse 2350 board revision that routes the ESP-01 UART signals to the RP2350 firmware backend
- An ESP-01 / ESP8266 module installed on the board
- A storage device appropriate for the selected Nextor mode
  - microSD card for `-s1 -w` / `-m1 -w` / `-s3 -w`
  - USB mass storage device for `-s2 -w` / `-m2 -w`

The firmware exposes the ESP link through the MSX-visible memio interface. It does not replace the ESP firmware running on the ESP-01 itself.

## 5. Boot Model

WiFi support is implemented as an overlay on the Sunrise IDE modes.

### Plain Sunrise WiFi Modes

For `-s1 -w` and `-s2 -w`, the firmware uses a two-subslot expanded layout after the bootstrap restart:

| Sub-slot | Function |
|---|---|
| 0 | ESP8266P system ROM and memio UART registers |
| 1 | Nextor Sunrise IDE ROM and IDE register surface |

### Mapper WiFi Modes

For `-m1 -w` and `-m2 -w`, the firmware uses a three-subslot layout:

| Sub-slot | Function |
|---|---|
| 0 | ESP8266P system ROM and memio UART registers |
| 1 | Nextor Sunrise IDE ROM and IDE register surface |
| 2 | 1MB PSRAM-backed MSX memory mapper |

This matters because WiFi support is not just a serial sidecar. It is mapped into the MSX-visible cartridge space in the same expanded-slot model used by the Sunrise mapper implementation.

The ESP8266P BIOS is placed in sub-slot 0 so its `INIT` routine runs before Nextor's during the MSX BIOS expanded-slot scan. This ordering is required for compatibility with FPGA-based MSX cores (e.g. ESEMSX3-derived cores), which are sensitive to the bus / sub-slot state immediately after a sub-slot transition. On real MSX hardware the ordering is functionally equivalent to the previous layout.

## 6. Embedded ESP System ROM

The PicoVerse firmware embeds a dedicated 16KB system ROM:

- Source asset: `2350/software/loadrom.pio/wifi/ESP8266P.rom`
- Tool-generated header: `2350/software/loadrom.pio/tool/src/esp8266p_rom.h`

This ROM is intended to run as a page-1 system ROM (`0x4000`-`0x7FFF`) and expects a memory-mapped WiFi transport rather than a classic I/O-port UART.

The Pico firmware does not embed this ROM as a source asset anymore. The standard project flow is used instead: the tool Makefile generates a C header from the ROM, and `loadrom.exe` appends that ROM after the Sunrise ROM in the final UF2 payload.

The integration goal is compatibility with MSX software targeting the ESP8266P memio environment used by Ducasp's MSX WiFi work.

## 7. Memory Map

The WiFi surface lives in the `0x4000`-`0x7FFF` area of the WiFi subslot.

### ROM Window

- `0x4000`-`0x7FFF`: embedded ESP8266P system ROM

### memio Registers

| Address | Direction | Purpose |
|---|---|---|
| `0x7F05` | read/write | F2 state byte used by the ESP system ROM boot/status logic |
| `0x7F06` | write | UART command port |
| `0x7F06` | read | UART RX data port |
| `0x7F07` | write | UART TX data port |
| `0x7F07` | read | UART status port |

## 8. F2 State Byte

The firmware exposes the `0x7F05` state byte expected by the ESP system ROM. The values used by the original ROM/software ecosystem include:

| Value | Meaning |
|---|---|
| `0xFF` | cold boot default |
| `0xF0` | system ROM loaded once |
| `0xF1` | enter setup flow |
| `0xF2` | force ESP detection on warm boot |
| `0xFE` | unexpected error |
| `0xEF` | disabled or ESP not found |

The PicoVerse backend stores and returns the byte as written by the MSX-side ROM/software.

## 9. UART Command Port (`0x7F06` write)

The write-side command port now follows Oduvaldo's MSXPICO memio recommendation used by the ROM-side code path: the UART stays fixed at `859372` bps and only command `20` is honored.

| Command | Behavior |
|---|---|
| `20` | Clear the RX FIFO and drain any already-buffered UART bytes |

All other command values written to `0x7F06` are ignored. They do not change the UART baud rate.

The PicoVerse UART backend is initialized once and remains fixed at `859372` bps for the whole session.

## 10. UART Status Port (`0x7F07` read)

The status byte follows the WiFi memio conventions used by the reference FPGA implementations.

| Bit | Meaning |
|---|---|
| 0 | RX FIFO has data available |
| 1 | TX transmission in progress / UART not writable |
| 2 | RX FIFO full |
| 3 | quick receive supported |
| 4 | RX underrun occurred; clears after status read |
| 6 | set to `1` in the normal non-interrupt state |
| 7 | set to `1` in the normal non-interrupt state |

The current PicoVerse implementation does not expose the old interrupt-oriented WiFi behavior. It implements the lite/request-response memio behavior instead.

## 11. Quick Receive and FIFO Behavior

The WiFi backend implements the two important behavioral details from the reference designs:

- **RX FIFO depth**: `2080` bytes
- **Quick receive wait**: about `25 ms`

That means a read from `0x7F06` behaves like this:

1. If data is already buffered, return it immediately.
2. If the FIFO is empty, wait briefly for incoming UART data.
3. If no byte arrives before the quick-receive timeout expires, return `0xFF` and latch the underrun bit.

This behavior is important because existing MSX WiFi software expects the adapter to absorb short timing gaps without immediately failing an RX read.

## 12. Firmware Architecture

The implementation is split across the RP2350 host tool and the RP2350 cartridge firmware.

### Host Tool

File:

- `2350/software/loadrom.pio/tool/src/loadrom.c`

Responsibilities:

- parse `-w` / `--wifi`
- reject `-w` outside `-s1/-m1/-s2/-m2`
- set the WiFi flag bit in the configuration record
- report WiFi as enabled in the build output

### Firmware

Files:

- `2350/software/loadrom.pio/pico/loadrom/loadrom.c`
- `2350/software/loadrom.pio/pico/loadrom/loadrom.h`

Responsibilities:

- decode the WiFi flag from `rom_type`
- dispatch WiFi-aware Sunrise handlers
- serve the ESP system ROM in the WiFi subslot
- implement the memio register block at `0x7F05`-`0x7F07`
- service RX/TX through the RP2350 UART backend

The build also links `hardware_uart` so the WiFi backend is included in the RP2350 firmware image.

## 13. Relationship To Nextor And Carnivore2 Modes

### Nextor

WiFi support is additive to the standard Sunrise IDE modes.

- Nextor still boots from the embedded Sunrise IDE ROM.
- Storage still comes from either microSD or USB.
- WiFi adds a second ROM/interface surface for MSX-side WiFi software.

### Carnivore2 RAM Loader

The current WiFi flag is intentionally not enabled for `-c1` or `-c2`.

Those modes already extend the mapper path with Carnivore2-compatible RAM-mode behavior for `SROM.COM /D15`. Combining WiFi with the Carnivore2 RAM loader was left out of the first implementation to keep the expanded-slot routing and compatibility surface controlled.

## 14. Typical Usage Flow

1. Build a UF2 with one of the supported WiFi commands:
   - `loadrom.exe -s1 -w`
   - `loadrom.exe -m1 -w`
   - `loadrom.exe -s2 -w`
   - `loadrom.exe -m2 -w`
2. Flash the UF2 to the PicoVerse 2350 in BOOTSEL mode.
3. Install the storage media required by the chosen Nextor mode.
4. Install the ESP-01 module on the cartridge.
5. Power on the MSX and let Nextor boot normally.
6. Run MSX software designed for the ESP8266P memio ROM/interface.

The important point is that WiFi support is not a standalone boot mode. It is a Nextor-side system-ROM add-on carried by the WiFi-enabled Sunrise UF2.

### Explorer File Hunter

Explorer firmware uses the same ESP-01 hardware path for its integrated File Hunter browser. Press `F3` in Explorer to open File Hunter, and press `F4` from Explorer or File Hunter to enter WiFi setup. File Hunter requests are handled by the Pico firmware through the ESP8266 command protocol; the MSX menu is used as the browser UI rather than booting a separate UNAPI BIOS ROM.

File Hunter downloads are received into PSRAM and saved as `.ROM` files in the root of the microSD card. A microSD card is required for this workflow.

## 15. Current Scope And Limitations

- WiFi support is available in **LoadROM** and **MultiROM** (in MultiROM only on `-s1`/`-m1`/`-s2`/`-m2`/`-s3` Nextor entries).
- WiFi support is currently **not available** in `-c1` / `-c2` Carnivore2 RAM-loader builds.
- Explorer exposes WiFi setup and direct ESP-01 access for File Hunter browsing, but it does not expose the Sunrise IDE `-w` system-ROM boot mode.
- The PicoVerse firmware provides the ROM mapping and serial transport; the ESP-01 still needs compatible firmware on the module itself.
- The implementation follows the ESP8266P memio conventions, so compatibility is best with software already targeting that environment.

## 16. Related Documents

- `docs/msx-picoverse-2350-loadrom-tool-manual.en-us.md`
- `docs/msx-picoverse-2350-explorer-tool-manual.en-us.md`
- `docs/msx-picoverse-2350-sunrise-nextor.md`
- `docs/msx-picoverse-2350-features.md`

## 17. External Design References

The PicoVerse 2350 WiFi implementation was informed by these reference projects and documents:

- Ducasp's MSXPICO ESP memio ROM work
- Ducasp's ESP8266 UNAPI firmware protocol
- Ducasp's FPGA WiFi / WiFi Lite memio implementations

These references were used to match the existing ESP memio conventions: address map, status bits, quick receive behavior, baud command table, and FIFO expectations.

Author: Cristiano Goncalves
Last updated: 05/04/2026