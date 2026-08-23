# PicoVerse 2350 Explorer — Per-ROM 50/60 Hz (VDP Frequency)

This document describes the per-ROM VDP video frequency (50 Hz / 60 Hz) feature added to
the PicoVerse 2350 **Explorer** firmware in **v2.40**, including the user interface, the
on‑disk persistence format, the MSX ⇄ Pico communication, and — most importantly — how the
selected frequency is actually applied to the launched ROM.

- Sub-project: `2350/software/explorer.pio`
- Introduced in: PicoVerse 2350 Explorer **v2.40**

---

## 1. Overview

On the V9938/V9958 (MSX2 and later), the VDP line frequency is controlled by **VDP register 9
(R9)**, whose **bit 1** selects the refresh mode:

| R9 bit 1 | Mode | Refresh |
|---|---|---|
| `0` | NTSC | 60 Hz |
| `1` | PAL  | 50 Hz |

The feature lets the user choose, per ROM, whether the game runs at **60 Hz**, **50 Hz**, or the
machine **Default**. The choice is stored per ROM and applied automatically each time that ROM is
launched.

It is offered only for **regular game ROMs**. The system ROMs (Nextor/Sunrise IDE, Carnivore2,
MegaRAM) manage their own boot flow and are intentionally excluded.

---

## 2. User guide

1. Highlight a ROM in the Explorer menu and press **ENTER** to open its detail screen.
2. A `Frequency` option is shown with three values, cycled with **LEFT / RIGHT**:
   - `Default` — leave the machine's boot frequency untouched.
   - `60Hz` — force NTSC / 60 Hz.
   - `50Hz` — force PAL / 50 Hz.
3. Press **ENTER** on `Action: Run` to launch. The selection is saved per ROM and restored the
   next time the detail screen for that ROM is opened.

The option does **not** appear for SYSTEM entries (Nextor/Sunrise, Carnivore2, MegaRAM).

---

## 3. Frequency encoding

A single byte carries the selection everywhere (menu state, `.PVC` file, control registers):

| Value | Symbol (menu.h / explorer.c) | Meaning |
|---|---|---|
| `0` | `VDP_FREQ_DEFAULT` | No change (machine default) |
| `1` | `VDP_FREQ_60HZ` | Force 60 Hz (NTSC) |
| `2` | `VDP_FREQ_50HZ` | Force 50 Hz (PAL) |

---

## 4. Persistence — the `.PVC` options file

Per-ROM options are stored in a small `.PVC` companion file on the microSD card (the same file
that already holds the audio profile, PSG mirror, mapper override, SD partition, and volume). The
frequency is appended as a **new byte 9**, growing the file from 9 to 10 bytes.

```
Offset  Field                 Notes
0..3    Magic "PVC1"
4       audio_selection       AUDIO_PROFILE_*
5       psg_emulation         0/1
6       mapper                mapper code override (v1+)
7       sd_partition          Sunrise partition number (v2+)
8       audio_volume          volume percent (v3+)
9       vdp_frequency         0=Default / 1=60Hz / 2=50Hz  (v4+, new)
10      cpu_mode              0=Default / 1=Turbo / 2=R800 (v5+, see the CPU option below)
```

Size milestones in `explorer.c`:

```c
#define PVC_OPTIONS_LEGACY_SIZE    6u   // v0: bytes 0-5
#define PVC_OPTIONS_MAPPER_SIZE    7u   // v1: + mapper
#define PVC_OPTIONS_PARTITION_SIZE 8u   // v2: + sd_partition
#define PVC_OPTIONS_VOLUME_SIZE    9u   // v3: + audio_volume
#define PVC_OPTIONS_FREQ_SIZE      10u  // v4: + vdp_frequency
#define PVC_OPTIONS_SIZE           11u  // v5: + cpu_mode
```

**Backward compatibility:** older 9-byte files still load; the load handler only reads byte 9 when
`br >= PVC_OPTIONS_FREQ_SIZE`, so pre-v2.40 files simply default the frequency to `Default`. The value
is sanitized on both load and save (anything `> VDP_FREQ_50HZ` is treated as `Default`).

---

## 5. MSX ⇄ Pico communication

The MSX menu (Z80) and the Pico exchange the option through the existing shared memory window.

### Save (MSX → Pico)

When the ROM is launched, the menu writes the option bytes into the query buffer and issues
`CMD_SAVE_OPTIONS`. The frequency uses **query buffer byte 7**:

```c
Poke(CTRL_QUERY_BASE + 2, audio_profile);   // 0xBFC2
Poke(CTRL_QUERY_BASE + 3, psg_enabled);     // 0xBFC3
Poke(CTRL_QUERY_BASE + 4, mapper);          // 0xBFC4
Poke(CTRL_QUERY_BASE + 5, sd_partition);    // 0xBFC5
Poke(CTRL_QUERY_BASE + 6, audio_volume);    // 0xBFC6
Poke(CTRL_QUERY_BASE + 7, vdp_freq);        // 0xBFC7  (new)
```

The Pico's `process_save_options_request()` reads `filter_query[7]`, writes it as `.PVC` byte 9,
and mirrors it into `ctrl_vdp_frequency`.

### Load (Pico → MSX)

`CMD_LOAD_OPTIONS` makes the Pico read the `.PVC` file and expose the values. The existing control
registers `0xBFF0..0xBFFF` were already full, so the frequency read‑back uses a **dedicated byte in
the free gap** between the SD-partition-info buffer (`0xBF80..0xBF9F`) and the chip-id buffer
(`0xBFAF..`):

```c
#define CTRL_VDP_FREQ 0xBFA0   // Pico -> MSX: loaded VDP frequency
```

The Pico serves this address from `ctrl_vdp_frequency` (guarded so it does not collide with the
File Hunter status-text window), and the menu reads it back with `Peek(CTRL_VDP_FREQ)`.

---

## 6. The core problem: the launch reset

Launching a ROM in Explorer is a hard hand-off:

```c
Poke(ROM_SELECT_REGISTER, index);   // tell the Pico which ROM to map
execute_rst00();                    // RST 00h  -> warm reset
```

`RST 00h` re-enters the MSX BIOS, which **re-initializes the VDP during boot**, including R9.
Because there is no persistent user store for the frequency (the RTC/clock chip does **not** hold a
50/60 Hz value on MSX2), anything written on the MSX side *before* the reset — R9 or the `RG9SAV`
shadow — is wiped by the boot. This was verified in practice: setting the frequency in the menu
changed it momentarily, then it reverted the instant the ROM booted.

**Conclusion:** the frequency must be applied **after** the BIOS boot, right as the ROM starts. The
reliable place is the game's own cartridge **INIT** entry, which the BIOS calls once the VDP is
already initialized.

---

## 7. The solution: Pico-injected INIT stub

When a regular game ROM with a non-default frequency is launched, the Pico **patches the game's
cartridge header** (in the cached copy it serves at `0x4000`) so that the ROM's `INIT` vector points
to a tiny stub. The stub writes VDP R9 directly, then jumps to the game's original `INIT`.

An MSX ROM header is 16 bytes:

```
0..1   "AB" signature
2..3   INIT entry (word)
4..5   STATEMENT   ┐
6..7   DEVICE      │  unused by INIT-only game ROMs (all zero)
8..9   TEXT        │
10..15 reserved    ┘
```

The 9-byte stub is written into the unused header bytes (`4..12`), and the INIT vector (`2..3`) is
repointed to `0x4004`:

```asm
; served at 0x4004 (header offset 4)
01 09 vv     ld  bc, vv*256+9 ; B = R9 value (0x02 = 50Hz, 0x00 = 60Hz), C = register 9
CD 47 00     call 0047h       ; BIOS WRTVDP: write R9 and update RG9SAV (0xFFE8)
C3 ll hh     jp  <orig_init>  ; continue into the game's real INIT
```

Using the BIOS `WRTVDP` routine (`0x0047`) — the same approach as the Carnivore2 boot menu — rather
than a raw `out (0x99)` port write is deliberate: on MSX2 `WRTVDP` writes VDP register 9 **and**
updates the BIOS R9 shadow `RG9SAV` at `0xFFE8`. Games that later reload R9 from that shadow through a
BIOS screen call therefore keep the requested refresh mode. The stub runs as the cartridge `INIT`
with the BIOS in page 0, so the direct `call 0047h` is valid.

Because the stub runs as the cartridge `INIT` — after the BIOS has finished initializing the VDP —
the requested refresh mode is active by the time the game starts.

### Sharing the header with the CPU speed option

Since Explorer v2.45 the same mechanism also carries the per-ROM **CPU** option (Panasonic 5.37 MHz
turbo / turbo R R800). The header only offers 12 writable bytes, which is not enough for both
stubs, so the builder in `apply_boot_patches()` concatenates the selected fragments and then decides
where to put the result:

- **Fits in 12 bytes** (frequency alone, 9 bytes) — written straight into header bytes `4..`.
- **Larger than 12 bytes** (any CPU option, with or without the frequency; up to 23 bytes) — parked
  in a run of `0x00`/`0xFF` padding found inside the ROM's block 0 (`find_boot_stub_slot()`), with a
  3-byte `jp <stub>` trampoline left in the header. The search runs backwards and takes the top of
  the highest qualifying run, so the stub lands in the trailing padding of block 0 whenever there is any.
- **No padding run available** — the CPU switch is dropped and the frequency-only stub is used,
  because that one always fits the header.

Only the first 8 KB of the cached image is searched: block 0 is what sits at `0x4000..0x5FFF` at
INIT time for every supported mapper (ASCII8 resets all four 8 KB windows to block 0, so its image
offsets `0x2000..0x3FFF` are not reachable at boot), and that is also what the `0x4000`-relative
stub address assumes.

### Where it is applied

The patch lives in `prepare_rom_source()` (the shared function that DMA-caches a ROM's leading
window into `rom_sram`), immediately after the cache is populated:

```c
apply_boot_patches(rom_sram, bytes_to_cache);
```

Guards inside `apply_boot_patches()`:
- At least one of `vdp_freq_launch` / `cpu_mode_launch` is non-default.
- `"AB"` signature present and `INIT` in the cartridge window (`0x4000..0xBFFF`).
- `cached_len >= 16` — the whole header is in the writable `rom_sram` cache (the header is served
  from `rom_sram`, not the read-only flash/PSRAM source).

### System-ROM exclusion

`prepare_rom_source()` is shared by the regular game mappers **and** the Nextor/Sunrise loaders, so
the patch is gated by launch flags set in `main()`:

```c
bool system_mapper = is_system_mapper(mapper);
vdp_freq_launch = system_mapper ? VDP_FREQ_DEFAULT : ctrl_vdp_frequency;
cpu_mode_launch = system_mapper ? CPU_MODE_DEFAULT : ctrl_cpu_mode;
```

`is_system_mapper()` covers `MAPPER_SUNRISE_USB/SD`, `MAPPER_SUNRISE_MAPPER_USB/SD`,
`MAPPER_C2_SD/USB`, and `MAPPER_MEGARAM(_SD/_USB)`. Setting the launch flags to `DEFAULT` for those
guarantees their headers are never touched. (Patching the Nextor header previously caused a boot
loop; this gate fixes it.)

---

## 8. Menu-side UI details

- The `Frequency` line is rendered on the ROM detail screen only when
  `allow_freq = !record_is_system_rom(record) && (Peek(0x002D) != 0)` is true. It is therefore
  hidden for system ROMs **and** on MSX1 (VDP R9 only exists on the V9938/V9958; the main-ROM MSX
  version byte at `0x002D` is `0` on MSX1). When hidden, the following options (SD partition / Wi-Fi /
  Action) shift up to fill the gap, and the launch forces the applied value to `Default` so the Pico
  never patches R9 on a machine without register 9.
- The option cycles `Default → 60Hz → 50Hz` with LEFT/RIGHT.
- The ROM detail footer was collapsed to a single generic hint,
  `[ESC - BACK] [LEFT/RIGHT - CHANGE]` (compact `[ESC-BACK] [L/R-CHANGE]` in 40-column mode), to
  recover MSX menu ROM space and keep `_CODE` below the `0xB900` Pico communication window.

---

## 9. Files changed

| File | Change |
|---|---|
| `2350/software/explorer.pio/msx/src/menu.h` | `VDP_FREQ_*` constants, `CTRL_VDP_FREQ` (0xBFA0) |
| `2350/software/explorer.pio/msx/src/screen_rom.c` | `Frequency` picker, load/save wiring, system-ROM hiding, footer simplification |
| `2350/software/explorer.pio/pico/explorer/explorer.c` | `.PVC` byte 9 load/save, `ctrl_vdp_frequency` read-back at `0xBFA0`, `vdp_freq_launch` gating, INIT-stub patch in `prepare_rom_source()` |

The initial iteration also added a menu-side R9 write in `menu_input.c`, but it was removed once
confirmed that the launch reset clobbers it; the final implementation applies the frequency entirely
from the Pico-injected stub.

---

## 10. Limitations & notes

- **MSX2+ only.** R9 exists on the V9938/V9958. A fixed-frequency MSX1 VDP (TMS9918) has no R9 to
  change, so the option is hidden on MSX1 and the launch forces `Default` there (no R9 write).
- **192-line default value.** The stub writes R9 with the standard 192-line, non-interlaced value
  plus the chosen frequency bit — which matches the boot default for the vast majority of games.
- **Games that reprogram R9 themselves** (e.g. some 212-line MSX2 titles) will use their own R9
  value; the injected setting is overridden by design.
- **System ROMs are excluded** (Nextor/Sunrise, Carnivore2, MegaRAM). For DOS/Nextor use, tools such
  as SofaRun can switch the frequency from within the environment.

---

## 11. Credits, reference, and license

This feature was implemented with reference to the **`50-60hz`** project by **sdsnatcher73**, which
demonstrates how to switch the V9938/V9958 VDP line frequency (VDP Register #9, PAL/NTSC bit)
reliably on MSX without corrupting the screen:

- Reference repository: https://github.com/sdsnatcher73/50-60hz
- License: **Apache License 2.0**
- As credited by its author, that project could not have been created without the help of
  **gdx**, **Grauw**, and **NYRIKKI**.

PicoVerse keeps its own RP2350 cartridge-side implementation (a per-ROM option persisted in `.PVC`
files and applied by injecting a VDP R9 write into the launched game's cartridge INIT), while
gratefully acknowledging sdsnatcher73's reference work and the Apache-2.0 licensing of the original
project.

The application technique was further refined after studying the **Carnivore2** boot menu
(`BOOTCMFC.ASM`, by RBSC): using the BIOS `WRTVDP` routine so the R9 change also updates the
`RG9SAV` shadow for better game compatibility, and restricting the frequency change to MSX2+
machines. Carnivore2 is developed by the RBSC group; only its publicly available technique was used
as a reference for the PicoVerse implementation.
