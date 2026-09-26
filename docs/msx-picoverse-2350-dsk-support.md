# MSX PicoVerse 2350 — DSK Disk Image Support (Explorer)

This document describes how the PicoVerse 2350 Explorer firmware boots `.DSK` floppy disk images from the microSD card. It covers the user-visible behaviour, the design choices, the launch sequence, the storage backend, the write-through mechanism, multi-disk files and disk swapping, the flash layout change, the MSX menu changes, the tests, and the known limitations. Single-disk images were introduced in **Explorer v2.52**; multi-disk files with number-key swapping in **v2.53**.

## 1. Overview

Explorer lists `.DSK` files from the microSD card next to the `.ROM` files, with the type label **DSK**. When you select one and choose Run, the MSX boots from that image as if it were a floppy disk:

- The image is copied into the cartridge's PSRAM.
- The firmware serves the **Nextor 2.1.4 Sunrise IDE** kernel to the MSX and presents the PSRAM image as the IDE disk.
- Nextor mounts the image as a single, unpartitioned FAT12 drive. For disks with an MSX-DOS 1 boot sector (most game disks), Nextor starts in MSX-DOS 1 mode automatically.
- A `.DSK` made of **several disks joined together** boots in Nextor's **disk emulation mode**: the MSX starts from disk 1, and the number keys swap disks (`1` for disk 1, `2` for disk 2, …). See [section 7](#7-multi-disk-images-and-disk-swapping).
- Writes from the MSX (game saves, `SAVE`, `COPY`) are written straight through to the `.DSK` file on the microSD card.

No floppy disk controller (FDC) is emulated. The disk is reached through Nextor's normal DOS/BIOS disk entry points (including the DOS 1 `DSKIO` at `4010h`), which covers software that loads through DOS, Disk BASIC or the disk BIOS.

## 2. Why this design

A `.DSK` file is a raw sector image of a floppy disk: a FAT12 volume that starts with a boot sector and has no partition table. Explorer already emulates a Sunrise IDE interface for Nextor, and its microSD backend already presents a single **partition** (with no partition table in front of it) as the IDE device. A `.DSK` image has exactly the same shape, so it can be served by a sibling backend with no changes to the Nextor kernel or to the ATA front-end.

## 3. Using it

1. Build and flash the Explorer UF2 as usual. Every Explorer UF2 now embeds the Nextor kernel used for DSK boot; you do not need `-s1` or `-a`.
2. Copy `.DSK` files to the microSD card (root or any folder).
3. In the Explorer menu, `.DSK` entries show the type label **DSK** and the source label **SD**.
4. Open an entry and choose Run (or quick-run it). The MSX resets and boots from the image.

To force MSX-DOS 1 mode for a disk that does not have a DOS 1 boot sector, hold `1` while the MSX boots (standard Nextor behaviour).

### Multi-disk games

Join the disks of a game into one `.DSK` file, in order, and copy that file to the card. On Windows:

```
copy /b "GAME (Disk 1).DSK" + "GAME (Disk 2).DSK" + "GAME (Disk 3).DSK" GAME.DSK
```

On Linux or macOS: `cat "GAME (Disk 1).DSK" "GAME (Disk 2).DSK" > GAME.DSK`.

Run the joined file from Explorer like any other DSK entry. The MSX boots disk 1. When the game asks for another disk, press that disk's number key — together with the key the game asks for, for example `2` and `SPACE` for "insert disk 2 and press space" — and the next disk access reads the new disk:

| Key | Disk |
|---|---|
| `1` … `9` | disks 1 to 9 |
| `A` … `W` | disks 10 to 32 |

You can also press `GRAPH` while the disk is being read: the CAPS LED lights and the MSX waits until you release `GRAPH` and press a disk key (press `GRAPH` again to keep the current disk). A 4 MB PSRAM staging area limits a joined file to 11 disks of 360 KB, or 5 disks of 720 KB (plus one 360 KB disk).

### Accepted files

A file is listed as a DSK entry when all of the following hold:

- The extension is `.DSK` (any letter case).
- The size is a whole number of standard floppies: a multiple of **360 KB** (368,640 bytes). That covers a single 360 KB (1DD) or 720 KB (2DD) disk and any joined set of them, up to 4 MB minus 1 KB (the PSRAM staging region less the two generated header sectors). Other sizes, such as 180 KB images or larger FAT12/FAT16 volumes, are not listed.
- The UF2 carries the hidden Nextor payload (see [section 8](#8-flash-layout-hidden-nextor-payload)). A UF2 built by an older tool never lists DSK files.

The microSD partition that Explorer browses can be FAT16, FAT32 or exFAT. Unlike the Sunrise Nextor SYSTEM entries, DSK boot does **not** need a FAT16 partition: Nextor only sees the image, not the card.

### Options available on the ROM screen

DSK entries are treated as SYSTEM-style entries on the MSX detail screen:

| Option | Available for DSK |
|---|---|
| PSG Mirror | Yes |
| **1MB Mapper** | Yes (default `No`) |
| Audio profiles (SCC/SCC+, Dual PSG, MSX-MUSIC, SFG) | No |
| WiFi support | No |
| Mapper override | No |
| SD partition | No |
| 50/60 Hz, CPU speed | No |

Options are saved in a per-image file named after the full image name, for example `GAME.DSK.PVC`, so a `GAME.ROM` in the same folder keeps its own `GAME.PVC`.

### The 1MB Mapper option

With `1MB Mapper: Yes`, the image boots with **Nextor plus the 1 MB PSRAM-backed memory mapper**, the same layout as the `Nextor Sunrise 2.1.4 + 1MB Mapper` SYSTEM entries: Nextor in expanded subslot 0 and 1 MB of mapper RAM in subslot 1. Use it on machines with little RAM. For example, Aleste 2 keeps reading the disk and never starts on a 64 KB Panasonic FS-A1FX with plain Nextor, but runs with the mapper.

With `No` (the default), the image boots with plain Nextor, as before. A few programs may not like an expanded cartridge slot with an extra mapper, so the mapper is opt-in per image. The choice is saved in the image's `.PVC` file (byte 11) and also applies to quick run.

How it is wired:

- **Menu:** the option reuses the ROM screen's on/off row that Sunrise SYSTEM entries use for WiFi; no DSK entry offers WiFi. The row is labelled `1MB Mapper:` and shown for DSK entries only. Turning it on does not clear the audio or PSG Mirror settings, as WiFi does.
- **Firmware:** it receives the choice through `CTRL_WIFI_SUPPORT` and saves it as `.PVC` byte 11. It restores that byte only for DSK records, so other entries keep starting with WiFi off.
- **Launch:** `loadrom_sunrise_mapper_dsk()` shares the `loadrom_sunrise_mapper_storage()` loop with the SYSTEM entry's `loadrom_sunrise_mapper_sd()`; only the Core 1 storage task differs. The mapper region takes 1 MB of PSRAM next to the 4 MB staging region.

## 4. Architecture

```
            MSX bus (cartridge slot)
                   │
          ┌────────┴─────────┐
          │ PIO bus engine   │
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
   PSRAM: 4 MB SD staging region  → [2 generated header sectors][.DSK image]
   Flash: hidden Nextor payload    → Nextor 2.1.4 Sunrise IDE ROM
   microSD: the .DSK file          → write-through target
```

Core 0 is unchanged from the Sunrise microSD mode: the shared `loadrom_sunrise_storage()` loop serves the ROM and the IDE registers. Only the Core 1 storage task differs.

## 5. Launch sequence

The launch path in `main()` of [explorer.c](../2350/software/explorer.pio/pico/explorer/explorer.c) runs these steps for a DSK record:

1. **Record detection.** `is_dsk_record()` is true when the record is not a folder or MP3 and its mapper code is `MAPPER_DSK` (23).
2. **Profile clamp.** The audio mode and audio profile are forced to none and WiFi is forced off. The launch is treated like a SYSTEM launch, so no 50/60 Hz or CPU INIT patch is armed and no game audio pipeline is started.
3. **Hold `/WAIT`** and stop the MP3 core (common launch path).
4. **Stage the image.** `load_rom_from_sd()` streams the whole `.DSK` into the 4 MB PSRAM SD region (`sd_rom_region`), starting 1 KB in, so the two sectors in front of it are free for the multi-disk header.
5. **Build the write map.** `dsk_build_write_map()` resolves the file's clusters to card sectors (see [section 6](#6-write-through-and-the-write-map)).
6. **Split and attach.** `sunrise_dsk_split_disks()` finds the disks in the file (see [section 7](#7-multi-disk-images-and-disk-swapping)).
   - **One disk:** `sunrise_dsk_attach_image()` gets the file itself as the device, exactly as before.
   - **Several disks:** `sunrise_dsk_build_emulation_header()` fills the two free sectors, and the whole PSRAM region (`2 + file sectors`) becomes the device, with the file starting at device sector 2.
   - An extent count of 0 means the file is read-only.
7. **Switch the ROM source** to the hidden Nextor payload: `rom_data = flash_rom`, `rom_offset = NEXTOR_DSK_FLASH_OFFSET`, `active_rom_size = 128 KB`.
8. **Dispatch** `case MAPPER_DSK:` → `loadrom_sunrise_dsk()` → `loadrom_sunrise_storage(..., sunrise_dsk_task, sunrise_dsk_set_ide_ctx)`. This caches the Nextor ROM, launches Core 1, initialises PSG Mirror audio if enabled, and starts serving the bus. With the `1MB Mapper` option on, it calls `loadrom_sunrise_mapper_dsk()` → `loadrom_sunrise_mapper_storage(..., sunrise_dsk_task, sunrise_dsk_set_ide_ctx)` instead (see [The 1MB Mapper option](#the-1mb-mapper-option)).

The MSX then resets. Nextor's Sunrise driver sends IDENTIFY and sees a device of `size / 512` sectors. It reads sector 0, finds a FAT12 boot sector with no partition table, mounts it as one drive, and boots. For a DOS 1 boot sector it enters MSX-DOS 1 mode. For a multi-disk file, sector 0 is the generated partition table instead, and Nextor enters disk emulation mode (section 7).

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

For a multi-disk file, device sectors below the file base (the two generated header sectors) are never written to the card. Writes to them update only the PSRAM copy and always succeed, even when the file is read-only. File sectors are mapped as `device sector − 2` before the extent lookup.

## 7. Multi-disk images and disk swapping

### How it works

Nextor 2.1 has a **disk emulation mode**: it boots in MSX-DOS 1 mode from a list of up to 32 disk images and swaps between them when the user presses a disk key during a disk access. The list is the *emulation data*: a signature, the number of images, the image to boot, and each image's start sector and size.

Nextor 2.1.4 enters the **persistent** variant of this mode when the first partition table entry of a device it controls holds a pointer to the emulation data. The Pico generates every sector of the IDE device, so it can present that partition table itself. No code has to run on the MSX, no file is written to the card, and nothing extra is needed beyond Nextor 2.1.4. (Nextor 3 no longer reads that partition table pointer; it uses a pointer in RAM or a `_NEXTOR.PSF` file instead.)

### Device layout

For a file with two or more disks, the PSRAM region is laid out exactly as the device Nextor sees:

| Device sector | Contents |
|---|---|
| 0 | Generated partition table (see below), `55AA` signature |
| 1 | Generated Nextor emulation data |
| 2 … | The `.DSK` file as loaded: disk 1, disk 2, … disk N, back to back |

The images are consecutive by construction, as Nextor requires, however the file is fragmented on the card.

**Sector 0, first partition table entry** (offset `1BEh`):

| Offset | Value | Meaning |
|---|---|---|
| +0 | `81h` | Status: active (bit 7) and "enter disk emulation mode" (bit 0) |
| +1 | `01h` | Device holding the emulation data (the Sunrise master) |
| +2 | `01h` | Logical unit |
| +3, +5, +6, +7 | `00 00 00 01` | Emulation data sector, most significant byte first: sector 1 |
| +4 | `01h` | Partition type FAT12 (Nextor requires it to be non-zero) |
| +8 | `2` | LBA start of disk 1 |
| +12 | disk 1 size | LBA size of disk 1 |

The CHS fields carry Nextor's pointer. The LBA fields map disk 1 as a normal FAT12 partition, so a boot **without** emulation mode (for example with `0` held) still mounts disk 1.

**Sector 1, emulation data:**

| Offset | Value |
|---|---|
| +0 | `NEXTOR_EMU_DATA`, zero terminated |
| +16 | Number of disks |
| +17 | Disk to boot: 1 |
| +18 | Work area address: 0 (Nextor allocates it) |
| +24 + 8·i | Disk *i*: device 0 (same device), logical unit 0, start sector (4 bytes), size in sectors (2 bytes) |

### Finding the disks in a joined file

`sunrise_dsk_split_disks()` walks the file one disk at a time. Each disk's size (720 or 1440 sectors) comes from the first rule that applies:

1. **Boot sector BPB:** 512 bytes per sector and a total sector count of 720 or 1440 that fits in what is left.
2. **FAT media byte** (first byte of sector 1 of that disk): `F8h` = 360 KB, `F9h` = 720 KB. MSX-DOS 1 identifies disks this way, so game disks with no usable BPB are still split correctly.
3. **Even split:** 720 KB if what is left is a multiple of 720 KB, otherwise 360 KB.

A file with only one disk keeps the single-disk path (no generated header, no emulation mode).

### Keys and boot behaviour

- Disk keys: `1`–`9` for disks 1–9, `A`–`W` for disks 10–32, or `GRAPH` then the key.
- Emulation mode always boots MSX-DOS 1 in Z80 mode. For R800 mode, hold `GRAPH` and `2` while booting, release both when the CAPS LED lights, then press `1`.
- The mode is persistent: a reset boots disk 1 in emulation mode again.
- Holding `0` while booting makes Nextor clear the pointer by rewriting sector 0. That write stays in PSRAM, so Nextor then boots normally from disk 1 until the entry is launched again from the Explorer menu.

### Verified in openMSX

The design was checked in openMSX with Explorer's own `Nextor-2.1.4.SunriseIDE.MasterOnly.ROM` on a Sunrise IDE, running on a Philips NMS 8250. The device image was built with the production `sunrise_dsk_split_disks()` and `sunrise_dsk_build_emulation_header()` from two joined 720 KB disks. Results:

- Nextor booted disk 1 in MSX-DOS 1 mode (`Disk BASIC version 1.0`).
- A reset booted disk 1 in emulation mode again.
- At a "press a key" prompt, pressing `2` and then `1` switched the next disk access to disk 2 and back to disk 1.
- Holding `0` at boot cleared only sector 0's pointer and booted disk 1 normally through the partition entry.

One problem showed up: holding a disk key *during* back-to-back directory reads (a BASIC loop running `FILES` continuously) hung Nextor while it rebuilt the disk parameters. Press the disk key when the game asks for the disk, not while it is loading.

## 8. Flash layout: hidden Nextor payload

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

## 9. Menu records and mapper code

| Item | Value |
|---|---|
| Mapper code | `MAPPER_DSK = 23` (fits in the 5 low bits of the record's mapper byte) |
| Record flags | `SOURCE_SD_FLAG` set; not a folder, not MP3 |
| Record size | image size in bytes |

`MAPPER_DSK` is deliberately placed past the end of `MAPPER_DESCRIPTIONS`. As a result:

- it can never be produced by a filename tag such as `GAME.ASC8.ROM`;
- it is rejected as a saved `.PVC` mapper or as a `CMD_SET_MAPPER` override (`mapper <= MAPPER_DESCRIPTION_COUNT` fails).

In addition, the options loader and both `CMD_SET_MAPPER` handlers refuse to re-map a record that already is a DSK. DSK records are not sorted to the top of the list with the SYSTEM entries, and they never go through mapper detection.

## 10. MSX menu changes

- [menu.c](../2350/software/explorer.pio/msx/src/menu.c): `mapper_description()` returns `"DSK"` for code 23, and `build_menu_row_text()` shows `DSK` in the type column of the menu list (other SD files show `ROM`, `MP3`, `WAV` or `<DIR>`).
- [screen_rom.c](../2350/software/explorer.pio/msx/src/screen_rom.c): `record_is_system_rom()` includes code 23. This hides mapper override, 50/60 Hz and CPU speed. DSK is not a Sunrise system ROM, so the external audio profiles, WiFi and the SD partition option are hidden too. PSG Mirror stays available, and DSK entries get the `1MB Mapper` row (the WiFi row, relabelled).
- The first line of every detail screen (ROM, DSK, MP3 and WAV) is `Name:`, printed by one shared `render_detail_name_line()` instead of per-type `ROM:`, `MP3:` and `WAV:` code. That saved 185 bytes of menu code.

Size budget: the menu's `_CODE` segment ends at `0x4050 + 0x7758 = 0xB7A8`, 344 bytes below the Pico communication window at `0xB900` (`MEMORY_START`).

## 11. Source files

| File | Role |
|---|---|
| [storage/sunrise_dsk.h](../2350/software/explorer.pio/pico/explorer/storage/sunrise_dsk.h) | Backend API, `sunrise_dsk_extent_t`, `SUNRISE_DSK_MAX_EXTENTS` (16), `SUNRISE_DSK_MAX_DISKS` (32), `SUNRISE_DSK_HEADER_SECTORS` (2) |
| [storage/sunrise_dsk.c](../2350/software/explorer.pio/pico/explorer/storage/sunrise_dsk.c) | Core 1 task: PSRAM reads, write-through, header writes, IDENTIFY, LBA advance; disk splitting and emulation header builder |
| [explorer.c](../2350/software/explorer.pio/pico/explorer/explorer.c) | `.DSK` listing, `is_dsk_record()`, `dsk_support_available()`, `dsk_build_write_map()`, `loadrom_sunrise_storage()` / `loadrom_sunrise_dsk()`, `loadrom_sunrise_mapper_storage()` / `loadrom_sunrise_mapper_dsk()`, launch wiring, `.PVC` naming and byte 11 |
| [CMakeLists.txt](../2350/software/explorer.pio/pico/explorer/CMakeLists.txt) | Adds `storage/sunrise_dsk.c` |
| [tool/src/explorer.c](../2350/software/explorer.pio/tool/src/explorer.c) | Always embeds the hidden Nextor payload |
| [msx/src/menu.c](../2350/software/explorer.pio/msx/src/menu.c), [msx/src/screen_rom.c](../2350/software/explorer.pio/msx/src/screen_rom.c) | "DSK" label and option rules |

### Backend API

```c
void sunrise_dsk_set_ide_ctx(sunrise_ide_t *ide);
void sunrise_dsk_attach_image(uint8_t *device, uint32_t device_sectors, uint32_t file_sector_base,
                              const sunrise_dsk_extent_t *extents, uint8_t extent_count);
uint8_t sunrise_dsk_split_disks(const uint8_t *image, uint32_t image_sectors,
                                uint16_t *disk_sectors, uint8_t max_disks);
void sunrise_dsk_build_emulation_header(uint8_t *header, const uint16_t *disk_sectors, uint8_t disk_count);
void sunrise_dsk_task(void);   // Core 1 entry point
```

- `device` points at the PSRAM buffer that holds all `device_sectors`; the `.DSK` file starts `file_sector_base` sectors in (0 for a single disk, `SUNRISE_DSK_HEADER_SECTORS` = 2 for a multi-disk file). A base at or past the device end is ignored.
- `sunrise_dsk_split_disks()` returns the disk count, or 0 when the file is not a whole number of 360 KB/720 KB disks or has more than `max_disks` (`SUNRISE_DSK_MAX_DISKS` = 32).
- `sunrise_dsk_build_emulation_header()` writes the two generated sectors described in section 7.

The extent table is referenced, not copied, so it must stay valid while the image runs. The launcher keeps it in a static array. `extent_count == 0`, or a count above `SUNRISE_DSK_MAX_EXTENTS`, makes the file read-only.

The task uses the same shared request flags as the SD and USB backends (`usb_read_requested`, `usb_write_requested`, `usb_read_lba`, `usb_write_lba`, `usb_write_buffer`, `usb_device_mounted`), which are defined in `sunrise_ide.c`. It publishes the device size with `sunrise_ide_set_device_info()`, and builds its own IDENTIFY block (model "PicoVerse DSK image") for an IDENTIFY that arrives before the backend is ready.

### Memory footprint

SRAM is tight in the Explorer firmware (link-time heap floor `PICO_HEAP_SIZE=0xA800`). The first build overflowed SRAM by 1,496 bytes. It was brought back under budget by:

- turning `loadrom_sunrise_sd()` into the shared `loadrom_sunrise_storage()`, so there is only one SRAM-resident loop and one static `sunrise_ide_t` (about 560 bytes);
- keeping the extent table in one place only, referenced by the backend;
- limiting the table to 16 extents (192 bytes);
- building the FatFs link-map table on the stack at launch;
- probing writability with the write-mode open instead of a `FILINFO`.

Multi-disk support adds no SRAM: the generated header lives in the first 1 KB of the PSRAM staging region, and the per-disk size table is a 64-byte stack array used once at launch.

## 12. Tests

All host suites live in `2350/software/explorer.pio/tests` and are run with PowerShell (`powershell -File <suite>.ps1`).

| Suite | What it checks |
|---|---|
| `dsk_backend.ps1` | Compiles the production `sunrise_dsk.c` against stub Pico/FatFs headers and runs the Core 1 loop one iteration at a time. Covers: device size published at mount; reads served from the PSRAM copy; out-of-range reads abort; writes mapped through two extents (including across the extent boundary); LBA advance and DRQ/idle transitions; PSRAM left untouched when the card write fails; rejection when read-only, when card init fails, or when the extent table is too large; pending IDENTIFY completes with the image size. Multi-disk: joined files split by BPB, then FAT media byte, then evenly, with invalid sizes and too many disks rejected; the generated header's partition entry, pointer, `55AA` signature and per-disk start/size table; a multi-disk device serving the header, mapping disk sectors to file sectors and the card, and keeping header writes in PSRAM even for a read-only file |
| `dsk_write_map.ps1` | Runs the production `dsk_build_write_map()` against the real FatFs (ff15) on a 48 MB RAM disk, formatted as FAT16, FAT32 and exFAT, with contiguous and fragmented images. Verifies that every image sector maps to the card sector holding its data, that a sector written through the map reads back through FatFs, and that the probe leaves the card byte-identical. Over-fragmented, read-only, missing and path-less images must map no extents |
| `fmpac.ps1` | Existing suite; now expects the Nextor SYSTEM records after the hidden DSK payload in a generated UF2 |
| `fm_scheduler.ps1`, `audio_handoff.ps1`, `mp3_memory.ps1` | Existing suites; unchanged and passing (SRAM/heap budget still met) |

A one-off check also unpacked a generated UF2 and confirmed that the bytes at `NEXTOR_DSK_FLASH_OFFSET` are identical to `resources/Nextor-2.1.4.SunriseIDE.MasterOnly.ROM`.

### Recommended hardware test matrix

- A DOS 1 system disk (`MSXDOS.SYS` + `COMMAND.COM`)
- A Disk BASIC disk with `AUTOEXEC.BAS`
- A boot-sector loader game
- 360 KB and 720 KB images
- A joined multi-disk game: boot disk 1, swap with the number keys when asked, save on a later disk and check that the right part of the `.DSK` changed
- Reset during a multi-disk game (should boot disk 1 again) and `0` held at boot (should boot disk 1 without emulation mode)
- A game that saves to disk: confirm the `.DSK` on the card changed
- A read-only `.DSK` file: saves must fail cleanly
- A heavily fragmented `.DSK`: must boot read-only
- A machine with an internal floppy drive: check drive letters
- PSG Mirror on and off

## 13. Limitations 

- **Disk swapping is key-driven.** Swapping uses Nextor's emulation mode keys (section 7). The swap is only picked up on the next disk access, and holding a disk key during continuous disk reads hung Nextor in testing.
- **Multi-disk sets boot MSX-DOS 1.** Emulation mode always starts MSX-DOS 1 in Z80 mode, so multi-disk software that needs MSX-DOS 2 will not run from a joined file.
- **Standard sizes only.** Files must be a multiple of 360 KB, made of 360 KB and 720 KB disks, and at most 4 MB minus 1 KB (11 × 360 KB or 5 × 720 KB + 360 KB).
- **No FDC emulation.** Copy-protected disks and software that programs a floppy controller directly do not work.
- **Nextor RAM use.** Nextor reserves more work area than a plain floppy disk ROM, so software that needs every byte of RAM may fail, especially on 64 KB machines. Turn on the `1MB Mapper` option for that image, or hold `SHIFT` while booting to disable the internal floppy drive's disk ROM and free some memory.
- **Drive order.** On machines with an internal floppy drive, the cartridge drive is `A:` only when the cartridge slot is scanned before the internal disk ROM.
- **Fragmentation.** Images split into more than 16 fragments on the card boot read-only. Copy the file to a freshly formatted card, or defragment it.
- **No cartridge audio profiles or WiFi** for DSK entries; only PSG Mirror is available.
- **microSD only.** DSK files boot only from the microSD card and are not embedded in flash by the tool. The File Hunter browser (`F3`, then `T` for the DSK catalog) can download them straight to the card root; images with an unsupported size are rejected before saving.
- **Timestamps.** The file's modification time does not change after the MSX writes to it.

## 14. References

- Nextor 2.1 documentation — Konamiman: <https://github.com/Konamiman/Nextor/tree/v2.1/docs> (disk emulation mode: User Manual section 3.9; emulation data and persistent pointer format: Programmers Reference section 7.2)
- MSXUSB / MsxUsbFDD driver (based on Rookie Drive USB FDD BIOS)
- [MSX PicoVerse 2350 Sunrise IDE Emulation for Nextor](./msx-picoverse-2350-sunrise-nextor.md)
- [MSX PicoVerse 2350 Explorer Tool Manual](./msx-picoverse-2350-explorer-tool-manual.en-us.md)
