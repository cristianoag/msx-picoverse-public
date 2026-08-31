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

The stub does not live in the ROM's own spare space. Releases up to v2.47 searched the image for a
run of `0x00`/`0xFF` padding big enough to hold the code, chaining fragments across several runs when
no single one was large enough. That works for most commercial ROMs but fails on densely packed ones:
a 120-ROM sweep left 9 unpatchable, and every **DSK2ROM** image failed, because a packed dsk2rom
block 0 may hold nothing longer than a 6-byte run.

Since v2.48 the Pico simply serves the stub instead. It is written straight over whatever bytes
occupy the chosen slot; the originals are kept in `boot_patch_saved[]` together with the original
`INIT` vector, and the Pico puts all of them back the instant the MSX reads the stub's **last** byte:

```c
static inline uint8_t read_rom_byte(const uint8_t *rom_base, uint32_t rel)
{
    uint8_t value = (rel < rom_cached_size) ? rom_sram[rel] : rom_base[rel];
    if (rel == boot_patch_trigger_rel) boot_patch_restore();
    return value;
}
```

That last byte is the high half of the stub's `jp orig_init` operand, so the Z80 already holds the
whole jump and never reads the stub again. The MSX therefore sees the stub only while the BIOS is
calling `INIT`; from the game's first instruction the image is byte-for-byte original. No padding is
required, so **any** ROM can be patched, and the CPU option no longer has to be dropped to make room.

`read_rom_byte()` is the single read path shared by every cached game mapper, so hooking the restore
there needed no changes to the individual mapper loops. The check is one compare against
`boot_patch_trigger_rel`, which holds a sentinel offset no read can produce while no patch is
pending.

This is the same idea SofaRun uses — it applies the setting from outside the ROM image, as the last
thing that runs before the game — adapted to a cartridge that has to survive the launch reset.

#### Choosing the slot

Two constraints decide where the stub goes.

**It must sit wholly inside image block 0** (offset `< 0x2000`). ASCII8 maps block 0 into all four
8 KB windows at reset, so image offset `0x2000` is *not* what follows `0x1FFF` on the bus; a stub
straddling that boundary would run on into the wrong bytes. The stub is therefore anchored to end
exactly at `0x2000`. With both a frequency and a CPU fragment it is 28 bytes, so it always fits.

**It must avoid the conventional entry-point area.** Image offset `0x10` — the obvious spot just
after the header — is where a disk ROM keeps its driver jump table (`INIHRD`, `INIENV`, `DRIVES`,
`DSKIO`, ...), and that is exactly what DSK2ROM emits. `Herzog - Tecno Soft (1989) [dsk2rom]` has one
at `0x4010`:

```
4010: jp 75E0     4013: jp 7506     4016: jp 7516     4019: jp 75D5   ...
```

The stub is only exposed between being installed and the BIOS calling `INIT`, but during that window
the BIOS may call one of those entries, which would execute stub bytes as a driver routine. Nothing
calls into the top of block 0 that early, so that is where the stub goes. A `BOOT_STUB_FALLBACK_OFFSET`
of 16 is kept only for images too small to reach `0x2000`.

Because the restore also puts the `INIT` vector back, a later **soft reset** re-runs the untouched
game rather than jumping at a stub that is no longer there. The trade-off is that a manual reset boots
at the machine default instead of re-applying the option.

### The page the stub is served from

The stub must be reachable **when the BIOS calls `INIT`**, and at that moment the BIOS has the
cartridge enabled in **page 1 only** (`0x4000`..`0x7FFF`). Page 2 still holds RAM until the ROM
enables its own slot there, which games do from their own `INIT` — King's Valley II calls `ENASLT`
with `H=0x80` for exactly that.

Every cached mapper has image block 0 at `0x4000` at that point (ASCII8 resets all four 8 KB windows
to block 0; the linear, Konami/Konami-SCC and ASCII16 layouts all start with sequential blocks), so an
offset below 8 KB is always visible at `0x4000 + offset`, and the chosen address is never a
bank-switch address for any of them.

ROMs whose `INIT` lives in page 2 are left alone, since page 1 would still be RAM when it ran. Of the
155 ROMs on the test microSD, 154 have a page-1 `INIT`; the remaining one declares `INIT=0x0000`, so
it has no INIT routine to redirect at all.

### Sharing the stub with the CPU speed option

The same stub carries the per-ROM **CPU** option (Panasonic 5.37 MHz turbo / turbo R R800), appended
to the frequency fragment by `build_boot_stub()`. `R800` uses `call 0180h` (CHGCPU); that is a plain
main-ROM entry with no SUB-ROM involvement, so it remains a direct call. Since v2.48 the two options
are always applied together — there is no longer a fallback that has to drop the CPU switch.

### Where it is applied

The patch lives in `prepare_rom_source()` (the shared function that DMA-caches a ROM's leading
window into `rom_sram`), immediately after the cache is populated and before `/WAIT` is released:

```c
apply_boot_patches(rom_sram, bytes_to_cache);
```

`prepare_rom_source()` also calls `boot_patch_disarm()` on entry: `apply_boot_patches()` is only
reached on the caching path, so without that a mapper which skips caching could inherit an armed
trigger from an earlier launch.

Guards inside `apply_boot_patches()`:
- At least one of `vdp_freq_launch` / `cpu_mode_launch` is non-default.
- `"AB"` signature present at image offset 0 and `INIT` inside page 1 (`0x4000..0x7FFF`).
- The cache covers the whole stub, so it is served from the writable `rom_sram` copy rather than the
  read-only flash/PSRAM source.

When any guard fails the ROM is left completely untouched, so choosing a frequency can never stop a
game from booting.
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
- **Only bit 1 of R9 is changed.** The stub reads the current `RG9SAV`, flips the 50/60 Hz bit and
  writes the result back through `WRTVDP`, so the line-count, interlace and display-control bits
  are preserved rather than forced to a fixed value.
- **Games that reprogram R9 themselves** (e.g. some 212-line MSX2 titles) will use their own R9
  value; the injected setting is overridden by design.
- **A manual reset boots at the machine default.** The patch restores the original INIT vector once
  the stub has run, so a soft reset re-runs the untouched game rather than re-applying the option.
- **A few mappers are not covered.** NEO8, NEO16 and ASCII16-X do not use the shared ROM cache the
  patch is written into, and Planar64 carries its `AB` header at image offset `0x4000` rather than
  0. Those ROMs launch normally, with the option simply not applied.
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
