# MSX PicoVerse 2350 MegaRAM Implementation

This document describes the MegaRAM support added to PicoVerse 2350 LoadROM and Explorer for the new Nextor system modes:

| Option | Storage backend | System image |
| --- | --- | --- |
| `-r1`, `--megaram-sd` | microSD Sunrise IDE | Nextor + 1MB MSX memory mapper + 1MB MegaRAM |
| `-r2`, `--megaram-usb` | USB Sunrise IDE | Nextor + 1MB MSX memory mapper + 1MB MegaRAM |

These modes are intended for MSX software and loaders that know how to use a MegaRAM cartridge: RAM that behaves like a writable MegaROM cartridge, with four 8KB banked cartridge windows. The PicoVerse implementation keeps Nextor storage and the normal MSX memory mapper available while adding a separate MegaRAM surface in the same expanded-slot cartridge. The control behavior follows the Cartucho II / SDMapper MegaRAM convention, extended from 512KB to 1MB by using seven bank bits.

## Historical Background

MegaRAM is a Brazilian MSX cartridge concept created by Ademir Carchano in 1987. The goal was practical and very MSX: let users load MegaROM cartridge games from disk into RAM, then run them through a cartridge-like banked memory interface instead of requiring the original ROM cartridge to be present.

MSX.org describes MegaRAM generically as a cartridge containing RAM mapped like a MegaROM, normally write-protected by default, and volatile when the MSX is powered off. Contemporary community discussions and preservation pages refer to Brazilian 512KB MegaRAM cartridges made by Ademir Carchano, later 2048KB boards, and the broader family of MegaRAM devices that followed. Brazilian articles about the history of MSX hardware describe the first MegaRAM work as coming after analysis of Konami MegaROM cartridges such as Nemesis, with `MEGARAM.BIN` becoming a common loader for disk-distributed MegaROM games.

Useful public references:

- [MSX.org Wiki: MegaRAM](https://www.msx.org/wiki/MegaRAM)
- [MSX.org forum: Brazilian MEGARAM - 512k made by Ademir Carchano](https://www.msx.org/forum/msx-talk/trading-and-collecting/brazilian-megaram-512k-made-ademir-carchano?page=1)
- [MSX.org forum: New MEGARAM 2048Kb from Ademir Carchano](https://www.msx.org/forum/msx-talk/trading-and-collecting/new-megaram-2048kb-ademir-carchano)
- [MegaRam MSX - Registros e Patentes](https://inventores.com.br/megaram-msx/)

## Tribute to Ademir Carchano

The PicoVerse 2350 MegaRAM mode is a tribute to Ademir Carchano's original MegaRAM idea and to the Brazilian MSX hardware culture around it. MegaRAM was not just another memory expansion; it was a clever answer to the scarcity and cost of imported cartridge software in Brazil, using banked RAM and loaders to make MegaROM-style software practical from local storage.

PicoVerse does not copy original MegaRAM hardware circuitry. It implements a compatible cartridge behavior in RP2350 firmware, backed by PSRAM, so modern PicoVerse users can experiment with the same class of software workflow while keeping the original inventor's contribution visible in the documentation and credits.

## User-Facing Behavior

The new LoadROM modes build dedicated Nextor system UF2 images. They do not take an external ROM file:

```text
loadrom.exe -r1
loadrom.exe -r1 -o nextor_megaram_sd.uf2

loadrom.exe -r2
loadrom.exe -r2 -o nextor_megaram_usb.uf2
```

At boot, the MSX sees an expanded cartridge in the physical PicoVerse slot. The cartridge contains:

- The embedded Nextor 2.1.4 Sunrise IDE ROM and Sunrise IDE register surface.
- A 1MB MSX memory mapper using standard mapper ports `0xFC`-`0xFF`.
- A separate 1MB MegaRAM, exposed as four switchable 8KB cartridge pages.

The SD mode (`-r1`) uses the on-board microSD card through the same Sunrise IDE backend as `-s1` and `-m1`. The USB mode (`-r2`) uses the USB mass-storage backend used by `-s2` and `-m2`.

## Expanded Slot Layout

The `-r1` and `-r2` modes use the same two-phase expanded-slot bootstrap used by the existing Sunrise + mapper modes. The first phase serves a tiny restart ROM so the MSX BIOS probes the cartridge again after the firmware has configured the full expanded-slot layout.

After that restart, the runtime layout is:

| Subslot | Content | Purpose |
| --- | --- | --- |
| `P-0` | Nextor ROM + Sunrise IDE | Boots Nextor and exposes the disk interface. |
| `P-1` | 1MB MSX memory mapper | Standard mapper RAM for MSX-DOS/Nextor, controlled by ports `0xFC`-`0xFF`. |
| `P-2` | Unused | Open bus. |
| `P-3` | 1MB MegaRAM | Writable Cartucho II-compatible 8KB banked cartridge RAM. |

`P` is the physical primary slot where the PicoVerse cartridge is inserted. The expanded slot register at `0xFFFF` is handled normally: writes select the active subslot per 16KB CPU page, and reads return the bitwise complement of the stored register.

The default expanded-slot register is `0x10`, matching the existing Sunrise + mapper boot arrangement: page 1 selects Nextor in subslot 0 for BIOS scanning, and page 2 selects mapper RAM in subslot 1 so the mapper can be discovered by the MSX.

## MegaRAM Capacity and Banks

The MegaRAM region uses 1MB of external PSRAM:

| Property | Value |
| --- | --- |
| Total size | 1MB (`1048576` bytes) |
| Bank size | 8KB (`8192` bytes) |
| Bank count | 128 |
| Bank numbers | `0`-`127` |
| Significant bank bits | bits `0`-`6`; bit `7` is ignored |

On reset, the four visible page registers are initialized to sequential banks:

```text
bank_reg[0] = 0
bank_reg[1] = 1
bank_reg[2] = 2
bank_reg[3] = 3
```

The PSRAM region is filled with `0xFF` at startup so an empty MegaRAM behaves like erased/open cartridge memory until software explicitly enables writes and loads data.

## MSX Visible Memory Map

The MegaRAM responds only when its subslot is selected and the CPU accesses the cartridge memory window `0x4000`-`0xBFFF`.

| Address range | MegaRAM page | Bank register | Physical offset |
| --- | --- | --- | --- |
| `0x4000`-`0x5FFF` | page 2 | `bank_reg[2]` | `(bank_reg[2] << 13) | (addr & 0x1FFF)` |
| `0x6000`-`0x7FFF` | page 3 | `bank_reg[3]` | `(bank_reg[3] << 13) | (addr & 0x1FFF)` |
| `0x8000`-`0x9FFF` | page 0 | `bank_reg[0]` | `(bank_reg[0] << 13) | (addr & 0x1FFF)` |
| `0xA000`-`0xBFFF` | page 1 | `bank_reg[1]` | `(bank_reg[1] << 13) | (addr & 0x1FFF)` |

Reads outside `0x4000`-`0xBFFF` are not claimed by the MegaRAM subslot.

## Bank Switching

When MegaRAM writes are disabled, memory writes in the visible `0x4000`-`0xBFFF` window update one of the four bank latches. The latch is selected by address bits A14:A13, matching the Cartucho II / SDMapper CPLD behavior:

| Write address range | Address bits A14:A13 | Effect |
| --- | --- | --- |
| `0x4000`-`0x5FFF` | `10` | `bank_reg[2] = data & 0x7F` |
| `0x6000`-`0x7FFF` | `11` | `bank_reg[3] = data & 0x7F` |
| `0x8000`-`0x9FFF` | `00` | `bank_reg[0] = data & 0x7F` |
| `0xA000`-`0xBFFF` | `01` | `bank_reg[1] = data & 0x7F` |

Only bits `0`-`6` of the written byte are used, giving 128 possible 8KB banks. Bit `7` is ignored. Original 512KB SDMapper/CPLD hardware used six bank bits; PicoVerse keeps the same latch behavior and expands the bank mask to seven bits for 1MB.

When MegaRAM writes are enabled, memory writes in `0x4000`-`0xBFFF` store bytes into the selected bank/page instead of changing the bank latches.

## Write Enable Control

MegaRAM RAM writes are disabled after reset:

```text
write_enabled = false
```

The PicoVerse implementation follows the Cartucho II / SDMapper write-enable convention on I/O ports `0x8E` and `0x8F`:

| I/O access | Effect |
| --- | --- |
| `IN 0x8E` or `IN 0x8F` | Enable MegaRAM RAM writes |
| `OUT 0x8E, data` or `OUT 0x8F, data` | Disable MegaRAM RAM writes and return memory writes to bank-latch mode |

The data byte written by `OUT` is ignored. The I/O read is used as a control strobe; the firmware does not return a meaningful MegaRAM data value on these ports.

When writes are enabled, memory writes to `0x4000`-`0xBFFF` store the byte into the currently selected bank/page.

When writes are disabled, memory writes in `0x4000`-`0xBFFF` update the bank latch selected by A14:A13.

## Read and Write Algorithms

### Memory read

```c
if (selected && RD && address >= 0x4000 && address <= 0xBFFF) {
    page = (address >> 13) & 0x03;
    offset = address & 0x1FFF;
    bank = bank_reg[page] & 0x7F;
    data = megaram[(bank << 13) | offset];
}
```

### Memory write

```c
if (selected && WR && address >= 0x4000 && address <= 0xBFFF) {
    page = (address >> 13) & 0x03;
    if (write_enabled) {
        offset = address & 0x1FFF;
        bank = bank_reg[page] & 0x7F;
        megaram[(bank << 13) | offset] = data;
    } else {
        bank_reg[page] = data & 0x7F;
    }
}
```

### I/O control

```c
if (IORQ && RD && (port == 0x8E || port == 0x8F))
    write_enabled = true;

if (IORQ && WR && (port == 0x8E || port == 0x8F))
    write_enabled = false;
```

## Bus Behavior

The MegaRAM follows the normal PicoVerse PIO bus rules:

- It responds only when the PicoVerse physical slot is selected through `/SLTSL`.
- It drives the data bus only during valid selected cartridge reads.
- It never drives the data bus during writes.
- It leaves reads outside `0x4000`-`0xBFFF` unclaimed for the MegaRAM subslot.
- I/O reads from `0x8E`/`0x8F` are treated as write-enable strobes and do not return a meaningful MegaRAM data byte.
- I/O writes to `0x8E`/`0x8F` disable MegaRAM RAM writes and return the cartridge to bank-latch mode.

The standard MSX memory mapper I/O ports `0xFC`-`0xFF` remain reserved for the 1MB MSX memory mapper in subslot 1. MegaRAM bank switching uses memory writes, not mapper I/O ports.

## PSRAM Allocation

The `-r1`/`-r2` firmware allocates two 1MB PSRAM regions at startup:

| Region | Size | Use |
| --- | --- | --- |
| `mapper_region` | 1MB | MSX memory mapper, 64 x 16KB pages |
| `megaram_region` | 1MB | MegaRAM, 128 x 8KB banks |

Both are allocated from the existing RP2350 PSRAM bump allocator. This is well within the 8MB PSRAM available on supported PicoVerse 2350 hardware.

## Source Files

LoadROM paths below are relative to `2350/software/loadrom.pio/`.

| File | Role |
| --- | --- |
| `tool/src/loadrom.c` | Adds `-r1`/`--megaram-sd`, `-r2`/`--megaram-usb`, mapper types 19/20, help text, and embedded Nextor UF2 generation. |
| `pico/loadrom/loadrom.h` | Defines `MEGARAM_SIZE`, `MEGARAM_BANKS`, and `MEGARAM_BANK_SIZE`. |
| `pico/loadrom/loadrom.c` | Allocates MegaRAM PSRAM, handles bank registers, ports `0x8E`/`0x8F`, expanded-slot dispatch, memory reads, and memory writes. |
| `pico/loadrom/sunrise_ide.c` | Existing Sunrise IDE front-end used by the same Nextor storage path. |
| `pico/loadrom/sunrise_sd.c` | microSD backend used by `-r1`. |
| `tool/Makefile` | Embeds the firmware and Nextor ROM into `loadrom.exe`. |
| `nextor/kernel/Nextor-2.1.4.SunriseIDE.MasterOnly.ROM` | Embedded Nextor Sunrise IDE ROM used by both MegaRAM modes. |

Explorer uses the same mapper IDs and MegaRAM bus behavior under `2350/software/explorer.pio/`:

| File | Role |
| --- | --- |
| `tool/src/explorer.c` | Adds `-r1`/`--megaram-sd`, `-r2`/`--megaram-usb`, `-a`/`--allnextor`, mapper types 19/20, help text, and embedded Nextor entries while still appending scanned folder ROMs. |
| `pico/explorer/explorer.h` | Defines `MEGARAM_SIZE`, `MEGARAM_BANKS`, and `MEGARAM_BANK_SIZE` for Explorer. |
| `pico/explorer/explorer.c` | Allocates MegaRAM PSRAM, handles bank registers, ports `0x8E`/`0x8F`, expanded-slot dispatch, memory reads, memory writes, and Explorer launch dispatch for mapper IDs 19/20. |
| `msx/src/screen_rom.c` | Offers PSG Mirror, MSX-MUSIC and the MegaRAM SCC/SCC+ profiles for the Nextor MegaRAM entries; WiFi and the other external-audio launch selections are not offered. |
| `msx/src/menu.c` | Displays MegaRAM mapper IDs 19/20 as `SYSTEM`. |

## Current Limitations

- MegaRAM support is currently exposed by LoadROM and Explorer. MultiROM does not expose `-r1`/`-r2` MegaRAM entries yet.
- The MegaRAM contents are volatile. They are cleared to `0xFF` on cartridge startup and lost when the MSX is powered off.
- The implementation provides the MegaRAM hardware surface; software still needs a MegaRAM-aware loader or patched game workflow to load and run data from it.
- The implementation follows the Cartucho II / SDMapper write-enable convention, where I/O reads from `0x8E`/`0x8F` enable RAM writes and I/O writes to those ports disable RAM writes.

Author: Cristiano Almeida Goncalves
Last updated: 06/27/2026
