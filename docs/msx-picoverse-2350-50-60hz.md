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

When a regular game ROM with a non-default frequency is launched, the Pico **patches the cached ROM
image** it serves so that the ROM's `INIT` vector points to a tiny stub. The stub writes VDP R9 and
its BIOS shadow, then jumps to the game's original `INIT`.

An MSX ROM header is 16 bytes:

```
0..1   "AB" signature
2..3   INIT entry (word)
4..5   STATEMENT   ┐
6..7   DEVICE      │  unused by INIT-only game ROMs (all zero)
8..9   TEXT        │
10..15 reserved    ┘
```

The stub itself is:

```asm
3A E8 FF     ld  a,(0FFE8h)    ; RG9SAV, the BIOS R9 shadow (current value)
CB CF        set 1,a           ; 50Hz/PAL   (CB 8F = res 1,a -> 60Hz/NTSC)
47           ld  b,a           ; B = value
0E 09        ld  c,9           ; C = register 9
CD 47 00     call 0047h        ; WRTVDP: applies R9 and updates RG9SAV
C3 ll hh     jp  <orig_init>   ; continue into the game's real INIT
```

Only **bit 1** is changed; every other R9 bit (line count, interlace, display control) is carried
over from the current shadow. This mirrors what the Carnivore2 boot menu does.

### Why the BIOS `WRTVDP` entry is used

On MSX2 `WRTVDP` writes the port itself only for registers 0..7; for every register `>= 8` — which
includes R9 — it forwards the call to the **SUB-ROM**:

```asm
061D  cp   8
061F  jr   c,<direct port write>
0621  push ix
0623  ld   ix,#012D
0627  jr   #0644          ; -> jp #0295 -> EXTROM: ld iy,(EXBRSA) / call CALSLT
```

That SUB-ROM path performs the full, machine-correct update of both the VDP register and its
`RG9SAV` shadow. Explorer v2.46 instead wrote port `#99` directly and poked `RG9SAV` by hand; that
is sufficient in an emulator but had **no effect on real hardware** (verified on a Panasonic
FS-A1FX and a Philips NMS 8245), so v2.47 returns to the BIOS entry.

A cartridge `INIT` is entered with the BIOS in page 0, so the `call 0047h` is valid there. The
Carnivore2 boot menu applies the refresh mode from its own pre-start code the same way.

> **Historical note.** The v2.46 notes claimed `WRTVDP` could not be called from a cartridge `INIT`.
> That was based on a faulty experiment which used `CB CE`/`CB 8E` (`SET`/`RES 1,(HL)`) with an
> uninitialised `HL` instead of `CB CF`/`CB 8F` (`SET`/`RES 1,A`), so the value passed to `WRTVDP`
> was never actually modified and the call appeared inert. The genuine cause of the v2.45 boot
> failure was the corrupted cartridge header described below.

### Why `RG9SAV` matters

Writing the VDP register alone is not enough: the BIOS reloads R9 from its shadow `RG9SAV`
(`0xFFE8`) whenever a screen call goes through it, so a stub that only touched the port had the
requested mode reverted within a couple of seconds of the game starting. `WRTVDP` updates the
shadow as part of the same call.

### Where the stub is placed

The 14-byte frequency stub (longer when a CPU option is combined with it) does not fit the 12
reserved ROM-header bytes. More importantly, those bytes cannot safely contain executable code:
offsets 4..9 are the `STATEMENT`, `DEVICE` and `TEXT` words, and unused fields must remain zero.
The first v2.46 split fallback violated this rule; on a real Panasonic FS-A1FX its nonzero `TEXT`
word was treated as a tokenized BASIC program pointer, so King's Valley II fell through to the MSX
logo/BASIC screen.

`apply_boot_patches()` therefore uses only header-safe outcomes:

1. **Chained across padding runs** — `place_boot_stub()` lays the stub into the largest available
   runs of `0x00`/`0xFF`, splitting it across up to four of them and ending each fragment with a
   `jp` to the next. A single large run holds the whole stub; otherwise a run as short as 4 bytes
   still contributes (one code byte plus the jump). The `INIT` word points at the first fragment and
   header offsets 4..15 stay byte-for-byte unchanged. Fragments are only ever cut at instruction
   boundaries, and the CPU fragments — which contain relative `jr nz` jumps — are indivisible blocks.
2. **Compact frequency-only fallback** — when the full stub cannot be laid out, a 9-byte form
   (`ld bc,r9*256+9` / `call 0047h` / `jp`) is chained instead. It writes R9 outright rather than
   preserving the register's other bits, which is safe because a cartridge `INIT` runs while the
   BIOS still has its boot defaults loaded. The CPU option has no compact form and is dropped.
3. **No safe placement** — the ROM is left untouched. The requested frequency is not applied, but
   selecting it cannot prevent the game from booting.

`find_boot_cave()` returns the **largest** remaining run each time and the stub is written at its
top, because the largest run is almost always genuine trailing padding rather than a short gap
between data tables the game reads. Chosen runs are checked against each other so fragments can
never overlap.

This chaining is what makes **DSK2ROM** images work. They are ASCII8, so only image block 0 is
reachable at `INIT` time, and a packed dsk2rom block 0 may contain nothing longer than a 6-byte run
— too small for the 14-byte stub or any two-run split, but enough once the compact form is spread
over three runs.

### The relocation window

The stub must be reachable **when the BIOS calls `INIT`**, and at that moment the BIOS has the
cartridge enabled in **page 1 only** (`0x4000`..`0x7FFF`). Page 2 still holds RAM until the ROM
enables its own slot there, which games do from their own `INIT` — King's Valley II calls `ENASLT`
with `H=0x80` for exactly that. A stub parked in the page-2 half of the image therefore never runs
correctly.

So the search window is:

| Mapper | Window | Why |
|---|---|---|
| ASCII8 | first 8 KB | resets all four 8 KB windows to block 0, so only image `0x0000`..`0x1FFF` is reachable |
| plain 16/32 KB, linear 48 KB, Konami, Konami-SCC, ASCII16 | first 16 KB | initial blocks are sequential, so image `0x0000`..`0x3FFF` is at `0x4000`..`0x7FFF` |

It is selected at launch time in `main()` (`boot_stub_search_limit`) next to the other launch flags.

### Sharing the stub with the CPU speed option

Since Explorer v2.45 the same mechanism also carries the per-ROM **CPU** option (Panasonic 5.37 MHz
turbo / turbo R R800), appended to the frequency fragment by `build_boot_stub()`. `R800` uses
`call 0180h` (CHGCPU); that is a plain main-ROM entry with no SUB-ROM involvement, so it remains a
direct call. Since v2.46 the CPU option is only applied when the combined stub can be parked in one
run (outcome 1); the compact frequency-only fallback has no room for it.

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
(`BOOTCMFC.ASM`, by RBSC): keeping the BIOS R9 shadow `RG9SAV` in step with the register so the
change survives a later BIOS screen call, and restricting the frequency change to MSX2+ machines.
Explorer v2.41 first did this by calling the BIOS `WRTVDP` routine. v2.46 briefly replaced it with a
direct port write, which proved ineffective on real hardware, so since v2.47 the stub is back to
toggling only bit 1 of `RG9SAV` and applying it through `WRTVDP`, as Carnivore2 does.
Carnivore2 is developed by the RBSC group; only its publicly available technique was used
as a reference for the PicoVerse implementation.
