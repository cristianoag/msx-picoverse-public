# MSX PicoVerse 2350 — DSK Disk Image Support (Explorer)

This document describes how the PicoVerse 2350 Explorer firmware boots `.DSK` floppy disk images from the microSD card. It covers the user-visible behaviour, the design choices, the launch sequence, the storage backend, the write-through mechanism, the flash layout change, the MSX menu changes, the tests, and the known limitations. The feature was introduced in **Explorer v2.52**.

Implementation phase: **Phase A (Nextor-backed)**. A later Phase B (custom MSX-DOS 1 disk ROM with multi-disk swapping) is outlined in [section 13](#13-roadmap-phase-b).

> Status: the firmware builds and all host test suites pass. Booting on real MSX hardware has not been validated yet.

## 1. Overview

Explorer lists `.DSK` files from the microSD card next to the `.ROM` files, with the type label **DSK**. When you select one and choose Run, the MSX boots from that image as if it were a floppy disk:

- The image is copied into the cartridge's PSRAM.
- The firmware serves the **Nextor 2.1.4 Sunrise IDE** kernel to the MSX and presents the PSRAM image as the IDE disk.
- Nextor mounts the image as a single, unpartitioned FAT12 drive. For disks with an MSX-DOS 1 boot sector (most game disks), Nextor starts in MSX-DOS 1 mode automatically.
- Writes from the MSX (game saves, `SAVE`, `COPY`) are written straight through to the `.DSK` file on the microSD card.

No floppy disk controller (FDC) is emulated. The disk is reached through Nextor's normal DOS/BIOS disk entry points (including the DOS 1 `DSKIO` at `4010h`), which covers software that loads through DOS, Disk BASIC or the disk BIOS.

## 2. Why this design

A `.DSK` file is a raw sector image of a floppy disk: a FAT12 volume that starts with a boot sector and has no partition table. Explorer already emulates a Sunrise IDE interface for Nextor, and its microSD backend already presents a single **partition** (with no partition table in front of it) as the IDE device. A `.DSK` image has exactly the same shape, so it can be served by a sibling backend with no changes to the Nextor kernel or to the ATA front-end.

Two alternatives were evaluated:

| Approach | Pros | Cons |
|---|---|---|
| **Nextor + Sunrise IDE backed by the image (chosen for Phase A)** | Reuses the proven Sunrise IDE emulation and the Nextor ROM already shipped with Explorer; no copyrighted kernel; small firmware change | Compatibility depends on Nextor (RAM use, drive order); disk swapping needs media-change support that the Sunrise path does not provide |
| **Custom FDC-less MSX-DOS 1 disk ROM (Phase B)** | Behaves like a real floppy disk ROM; lowest RAM use; full control of `DSKCHG`, which enables disk swapping | Needs an MSX-DOS 1 kernel. The available kernel source (used by Rookie Drive / MsxUsbFDD) is marked "Copyrighted by ASCII… study only". The alternative is patching a Disk ROM dump the user supplies, as OmniSCC does |

Reference implementations reviewed:

- **MsxUsbFDD** (MSXUSB project, based on Konamiman's Rookie Drive USB FDD BIOS): the MSX-DOS 1 kernel plus a `DSKIO`/`DSKCHG`/`GETDPB` driver that turns sector numbers into CH376 `BYTE_LOCATE` + read/write calls on an open `.DSK` file.
- **OmniSCC** (RBSC, RP2040): patches a user-supplied 16 KB Disk ROM so that its driver entries talk to the microcontroller through a memory-mapped mailbox. OmniSCC is GPLv3, which is not compatible with PicoVerse's CC BY-NC-SA licence, so only its ideas were used — no code.

## 3. Using it

1. Build and flash the Explorer UF2 as usual. Every Explorer UF2 now embeds the Nextor kernel used for DSK boot; you do not need `-s1` or `-a`.
2. Copy `.DSK` files to the microSD card (root or any folder).
3. In the Explorer menu, `.DSK` entries show the type label **DSK** and the source label **SD**.
4. Open an entry and choose Run (or quick-run it). The MSX resets and boots from the image.

To force MSX-DOS 1 mode for a disk that does not have a DOS 1 boot sector, hold `1` while the MSX boots (standard Nextor behaviour).

### Accepted files

A file is listed as a DSK entry when all of the following hold:

- The extension is `.DSK` (any letter case).
- The size is exactly **360 KB** (368,640 bytes, single-sided 1DD) or **720 KB** (737,280 bytes, double-sided 2DD). Other sizes (for example 180 KB images, or larger FAT12/FAT16 volumes) are not listed. The backend itself can serve any multiple of 512 bytes up to the 4 MB PSRAM staging region; the listing is limited to the two standard floppy formats on purpose until multi-disk and disk-change support exist.
- The UF2 carries the hidden Nextor payload (see [section 7](#7-flash-layout-hidden-nextor-payload)). A UF2 built by an older tool never lists DSK files.

The microSD partition that Explorer browses can be FAT16, FAT32 or exFAT. Unlike the Sunrise Nextor SYSTEM entries, DSK boot does **not** need a FAT16 partition: Nextor only sees the image, not the card.

### Options available on the ROM screen

DSK entries are treated as SYSTEM-style entries on the MSX detail screen:

| Option | Available for DSK |
|---|---|
| PSG Mirror | Yes |
| Audio profiles (SCC/SCC+, Dual PSG, MSX-MUSIC, SFG) | No |
| WiFi support | No |
| Mapper override | No |
| SD partition | No |
| 50/60 Hz, CPU speed | No |

Options are saved in a per-image file named after the full image name, for example `GAME.DSK.PVC`, so a `GAME.ROM` in the same folder keeps its own `GAME.PVC`.

## 4. Architecture

```
            MSX bus (cartridge slot)
                   │
          ┌────────┴─────────┐
          │ PIO bus engine    │
          └────────┬─────────┘
                   │ read/write tokens
   Core 0 ─────────┴──────────────────────────────────────────────
   loadrom_sunrise_storage()
     • serves the Nextor Sunrise ROM (8 × 16 KB pages, from flash
       via the PSRAM ROM cache)
     • Sunrise segment register + ATA task file (0x7C00–0x7EFF)
       via sunrise_ide.c
     • raises usb_read_requested / usb_write_requested
   ─────────────────────────────────────────────────────────────
   Core 1  sunrise_dsk_task()
     • reads:  memcpy from the PSRAM image into the sector buffer
     • writes: disk_write() to the card, then update the PSRAM image
     • completes IDENTIFY, advances LBA, services PSG Mirror audio
   ─────────────────────────────────────────────────────────────
   PSRAM: 4 MB SD staging region  → holds the .DSK image
   Flash: hidden Nextor payload    → Nextor 2.1.4 Sunrise IDE ROM
   microSD: the .DSK file          → write-through target
```

Core 0 is unchanged from the Sunrise microSD mode: the shared `loadrom_sunrise_storage()` loop serves the ROM and the IDE registers. Only the Core 1 storage task differs.

## 5. Launch sequence

The launch path in `main()` of [explorer.c](../2350/software/explorer.pio/pico/explorer/explorer.c) runs these steps for a DSK record:

1. **Record detection.** `is_dsk_record()` is true when the record is not a folder or MP3 and its mapper code is `MAPPER_DSK` (23).
2. **Profile clamp.** The audio mode and audio profile are forced to none and WiFi is forced off. The launch is treated like a SYSTEM launch, so no 50/60 Hz or CPU INIT patch is armed and no game audio pipeline is started.
3. **Hold `/WAIT`** and stop the MP3 core (common launch path).
4. **Stage the image.** `load_rom_from_sd()` streams the whole `.DSK` into the 4 MB PSRAM SD region (`sd_rom_region`).
5. **Build the write map.** `dsk_build_write_map()` resolves the file's clusters to card sectors (see [section 6](#6-write-through-and-the-write-map)).
6. **Attach the image.** `sunrise_dsk_attach_image()` gets the PSRAM pointer, the sector count (`size / 512`) and the extent table. An extent count of 0 means read-only.
7. **Switch the ROM source** to the hidden Nextor payload: `rom_data = flash_rom`, `rom_offset = NEXTOR_DSK_FLASH_OFFSET`, `active_rom_size = 128 KB`.
8. **Dispatch** `case MAPPER_DSK:` → `loadrom_sunrise_dsk()` → `loadrom_sunrise_storage(..., sunrise_dsk_task, sunrise_dsk_set_ide_ctx)`. This caches the Nextor ROM, launches Core 1, initialises PSG Mirror audio if enabled, and starts serving the bus.

The MSX then resets. Nextor's Sunrise driver sends IDENTIFY and sees a device of `size / 512` sectors. It reads sector 0, finds a FAT12 boot sector with no partition table, mounts it as one drive, and boots. For a DOS 1 boot sector it enters MSX-DOS 1 mode.

## 6. Write-through and the write map

### Why a write map

Core 0 owns FatFs while the menu runs. Once the MSX is running, Core 0 is fully occupied by the bus loop, and FatFs is not re-entrant across cores. Core 1 therefore must not call FatFs. It writes raw sectors instead, so it needs to know where each image sector lives on the card.

### Building it (Core 0, at launch)

`dsk_build_write_map()`:

1. Opens the image with `FA_READ | FA_WRITE`. FatFs itself refuses read-only files (`FR_DENIED`) and write-protected media (`FR_WRITE_PROTECTED`). Nothing is written, because the file is closed without being modified.
2. Uses FatFs fast seek: it sets `fil.cltbl` to a stack table and calls `f_lseek(&fil, CREATE_LINKMAP)`. FatFs fills the table with `(cluster count, first cluster)` pairs, one per fragment. This works on FAT12/16/32 and on exFAT, including exFAT's contiguous "no FAT chain" files.
3. Converts each fragment to an extent:
   - `image_sector` = running sector offset in the image
   - `lba` = `fs->database + fs->csize × (first_cluster − 2)`
   - `count` = `cluster_count × fs->csize`
4. Returns the number of extents. It returns 0 (read-only boot) when the open fails, the file is split into more than **16** fragments (`FR_NOT_ENOUGH_CORE`), or the extents do not cover the image.

The LBAs are FatFs volume-relative. Core 1 writes them with `disk_write()` (not `disk_write_raw()`), so the same partition window that FatFs used for the browsing partition is applied.

### Serving writes (Core 1)

For each sector the MSX writes:

1. Map the image sector to a card LBA by scanning the extents.
2. `disk_write()` the 512-byte sector to the card.
3. Only if the card write succeeds, copy the sector into the PSRAM image. A failed write never leaves the PSRAM copy and the file different from each other.
4. Report completion to the ATA front-end, advance the LBA registers, and either request the next sector (DRQ) or go idle.

Writes are rejected with an ATA abort (`ERR` + `ABRT`), which the MSX reports as a disk error, when:

- the image is read-only (no extents), or
- the card failed to re-initialise on Core 1 at start-up, or
- the sector is out of range or not covered by the map.

Every sector is written through synchronously, so a power loss only loses a write that was in progress. The file's modification timestamp is not updated, because the directory entry is not touched.

## 7. Flash layout: hidden Nextor payload

Before v2.52, the Nextor ROM was in the UF2 only when a Sunrise option (`-s1`, `-a`, …) was used, with one copy per SYSTEM entry. DSK boot must work with any UF2, so the tool now always embeds **one** extra Nextor copy as a hidden payload right after the SFG BIOS:

```
[firmware][menu ROM 32K][config 16K][WiFi config 8K][ESP8266P BIOS 16K]
[FM-PAC BIOS 64K][SFG BIOS 64K][DSK Nextor kernel 128K][SYSTEM Nextor copies + visible ROMs]
```

| Symbol | Where | Value |
|---|---|---|
| `NEXTOR_DSK_FLASH_OFFSET` | firmware `explorer.c` | `SFG_BIOS_FLASH_OFFSET + SFG_BIOS_ROM_SIZE` (200 KB after the firmware end) |
| `NEXTOR_DSK_ROM_SIZE` | firmware and tool | 128 KB |

- The tool now requires the embedded Nextor ROM to be exactly 128 KB, and visible ROM offsets start 128 KB later.
- `dsk_support_available()` in the firmware checks for the `AB` cartridge header at that offset before listing any `.DSK` file.
- The firmware and the tool are always built together (the tool embeds the firmware), so both sides always agree on the layout.

## 8. Menu records and mapper code

| Item | Value |
|---|---|
| Mapper code | `MAPPER_DSK = 23` (fits in the 5 low bits of the record's mapper byte) |
| Record flags | `SOURCE_SD_FLAG` set; not a folder, not MP3 |
| Record size | image size in bytes |

`MAPPER_DSK` is deliberately placed past the end of `MAPPER_DESCRIPTIONS`. As a result:

- it can never be produced by a filename tag such as `GAME.ASC8.ROM`;
- it is rejected as a saved `.PVC` mapper or as a `CMD_SET_MAPPER` override (`mapper <= MAPPER_DESCRIPTION_COUNT` fails).

In addition, the options loader and both `CMD_SET_MAPPER` handlers refuse to re-map a record that already is a DSK. DSK records are not sorted to the top of the list with the SYSTEM entries, and they never go through mapper detection.

## 9. MSX menu changes

- [menu.c](../2350/software/explorer.pio/msx/src/menu.c): `mapper_description()` returns `"DSK"` for code 23, and `build_menu_row_text()` shows `DSK` in the type column of the menu list (other SD files show `ROM`, `MP3`, `WAV` or `<DIR>`).
- [screen_rom.c](../2350/software/explorer.pio/msx/src/screen_rom.c): `record_is_system_rom()` includes code 23. This hides mapper override, 50/60 Hz and CPU speed. DSK is not a Sunrise system ROM, so the external audio profiles, WiFi and the SD partition option are hidden too. PSG Mirror stays available.

Size budget: the menu's `_CODE` segment now ends at `0x4050 + 0x77B6 = 0xB806`. That is 250 bytes below the Pico communication window at `0xB900` (`MEMORY_START`). The DSK changes cost 53 bytes.

## 10. Source files

| File | Role |
|---|---|
| [storage/sunrise_dsk.h](../2350/software/explorer.pio/pico/explorer/storage/sunrise_dsk.h) | Backend API, `sunrise_dsk_extent_t`, `SUNRISE_DSK_MAX_EXTENTS` (16) |
| [storage/sunrise_dsk.c](../2350/software/explorer.pio/pico/explorer/storage/sunrise_dsk.c) | Core 1 task: PSRAM reads, write-through, IDENTIFY, LBA advance |
| [explorer.c](../2350/software/explorer.pio/pico/explorer/explorer.c) | `.DSK` listing, `is_dsk_record()`, `dsk_support_available()`, `dsk_build_write_map()`, `loadrom_sunrise_storage()` / `loadrom_sunrise_dsk()`, launch wiring, `.PVC` naming |
| [CMakeLists.txt](../2350/software/explorer.pio/pico/explorer/CMakeLists.txt) | Adds `storage/sunrise_dsk.c` |
| [tool/src/explorer.c](../2350/software/explorer.pio/tool/src/explorer.c) | Always embeds the hidden Nextor payload |
| [msx/src/menu.c](../2350/software/explorer.pio/msx/src/menu.c), [msx/src/screen_rom.c](../2350/software/explorer.pio/msx/src/screen_rom.c) | "DSK" label and option rules |

### Backend API

```c
void sunrise_dsk_set_ide_ctx(sunrise_ide_t *ide);
void sunrise_dsk_attach_image(uint8_t *image, uint32_t sector_count,
                              const sunrise_dsk_extent_t *extents, uint8_t extent_count);
void sunrise_dsk_task(void);   // Core 1 entry point
```

The extent table is referenced, not copied, so it must stay valid while the image runs. The launcher keeps it in a static array. `extent_count == 0`, or a count above `SUNRISE_DSK_MAX_EXTENTS`, makes the disk read-only.

The task uses the same shared request flags as the SD and USB backends (`usb_read_requested`, `usb_write_requested`, `usb_read_lba`, `usb_write_lba`, `usb_write_buffer`, `usb_device_mounted`), which are defined in `sunrise_ide.c`. It publishes the device size with `sunrise_ide_set_device_info()`, and builds its own IDENTIFY block (model "PicoVerse DSK image") for an IDENTIFY that arrives before the backend is ready.

### Memory footprint

SRAM is tight in the Explorer firmware (link-time heap floor `PICO_HEAP_SIZE=0xA800`). The first build overflowed SRAM by 1,496 bytes. It was brought back under budget by:

- turning `loadrom_sunrise_sd()` into the shared `loadrom_sunrise_storage()`, so there is only one SRAM-resident loop and one static `sunrise_ide_t` (about 560 bytes);
- keeping the extent table in one place only, referenced by the backend;
- limiting the table to 16 extents (192 bytes);
- building the FatFs link-map table on the stack at launch;
- probing writability with the write-mode open instead of a `FILINFO`.

## 11. Tests

All host suites live in `2350/software/explorer.pio/tests` and are run with PowerShell (`powershell -File <suite>.ps1`).

| Suite | What it checks |
|---|---|
| `dsk_backend.ps1` | Compiles the production `sunrise_dsk.c` against stub Pico/FatFs headers and runs the Core 1 loop one iteration at a time. Covers: device size published at mount; reads served from the PSRAM copy; out-of-range reads abort; writes mapped through two extents (including across the extent boundary); LBA advance and DRQ/idle transitions; PSRAM left untouched when the card write fails; rejection when read-only, when card init fails, or when the extent table is too large; pending IDENTIFY completes with the image size |
| `dsk_write_map.ps1` | Runs the production `dsk_build_write_map()` against the real FatFs (ff15) on a 48 MB RAM disk, formatted as FAT16, FAT32 and exFAT, with contiguous and fragmented images. Verifies that every image sector maps to the card sector holding its data, that a sector written through the map reads back through FatFs, and that the probe leaves the card byte-identical. Over-fragmented, read-only, missing and path-less images must map no extents |
| `fmpac.ps1` | Existing suite; now expects the Nextor SYSTEM records after the hidden DSK payload in a generated UF2 |
| `fm_scheduler.ps1`, `audio_handoff.ps1`, `mp3_memory.ps1` | Existing suites; unchanged and passing (SRAM/heap budget still met) |

A one-off check also unpacked a generated UF2 and confirmed that the bytes at `NEXTOR_DSK_FLASH_OFFSET` are identical to `resources/Nextor-2.1.4.SunriseIDE.MasterOnly.ROM`.

### Recommended hardware test matrix

- A DOS 1 system disk (`MSXDOS.SYS` + `COMMAND.COM`)
- A Disk BASIC disk with `AUTOEXEC.BAS`
- A boot-sector loader game
- 360 KB and 720 KB images
- A game that saves to disk: confirm the `.DSK` on the card changed
- A read-only `.DSK` file: saves must fail cleanly
- A heavily fragmented `.DSK`: must boot read-only
- A machine with an internal floppy drive: check drive letters
- PSG Mirror on and off

## 12. Limitations (Phase A)

- **One image per boot.** Multi-disk games cannot swap disks yet; go back to the menu to change the image.
- **Two sizes only.** Only 360 KB and 720 KB images are listed; other sizes are hidden.
- **No FDC emulation.** Copy-protected disks and software that programs a floppy controller directly do not work.
- **Nextor RAM use.** Nextor reserves more work area than a plain floppy disk ROM, so software that needs every byte of RAM may fail.
- **Drive order.** On machines with an internal floppy drive, the cartridge drive is `A:` only when the cartridge slot is scanned before the internal disk ROM.
- **Fragmentation.** Images split into more than 16 fragments on the card boot read-only. Copy the file to a freshly formatted card, or defragment it.
- **No cartridge audio profiles or WiFi** for DSK entries; only PSG Mirror is available.
- **microSD only.** DSK files are not downloaded from File Hunter and are not embedded in flash by the tool.
- **Timestamps.** The file's modification time does not change after the MSX writes to it.

## 13. Roadmap: Phase B

Phase B would add a second DSK mode with a custom 32 KB FDC-less disk ROM:

- An MSX-DOS 1 kernel in bank 0 and a small driver in bank 1. `DSKIO`, `DSKCHG` and `GETDPB` would talk to the Pico through a memory-mapped mailbox: a 512-byte sector window plus command/status registers in page 1, with `/WAIT` held while sectors are served from PSRAM. Transfers into page 1 would go through a page-3 bounce routine.
- Kernel source: either the MsxUsbFDD/Rookie Drive kernel (licensing caveat above), or patching a Disk ROM dump that the user supplies on the SD card (copyright-clean, as OmniSCC does).
- Multi-disk support: group sibling images (`NAME1.DSK`, `NAME2.DSK`, `(Disk n of m)`), add a swap trigger (a `CALL` statement, an MSX-DOS tool, or keyboard snooping), and have `DSKCHG` report "changed" after a swap.

## 14. References

- Nextor 2.1 documentation — Konamiman: <https://github.com/Konamiman/Nextor/tree/v2.1/docs>
- MSXUSB / MsxUsbFDD driver (based on Rookie Drive USB FDD BIOS)
- OmniSCC — RBSC: <https://github.com/RBSC/OmniSCC> (GPLv3; used for design ideas only)
- [MSX PicoVerse 2350 Sunrise IDE Emulation for Nextor](./msx-picoverse-2350-sunrise-nextor.md)
- [MSX PicoVerse 2350 Explorer Tool Manual](./msx-picoverse-2350-explorer-tool-manual.en-us.md)
