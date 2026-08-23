# PicoVerse 2350 Explorer — Per-ROM CPU Speed (Turbo / R800)

This document describes the per-ROM CPU speed feature added to the PicoVerse 2350 **Explorer**
firmware in **v2.45**: booting a selected game with the Panasonic MSX2+ 5.37 MHz turbo or with
MSX turbo R **R800** mode instead of the machine default.

- Sub-project: `2350/software/explorer.pio`
- Introduced in: PicoVerse 2350 Explorer **v2.45**

---

## 1. Overview

Two unrelated hardware mechanisms are exposed through a single `CPU` option:

| Value | Mechanism | Machines |
|---|---|---|
| `Default` | nothing is changed | all |
| `Turbo` | Panasonic switched-I/O device 8, port `#41` bit 0 → 5.37 MHz Z80 | FS-A1FX, FS-A1WX, FS-A1WSX, FS-A1ST, FS-A1GT and faithful FPGA cores |
| `R800` | `CHGCPU` BIOS entry (`#0180`) with `A = #81` → R800 ROM mode | MSX turbo R only |

The option is offered only for **regular game ROMs**. The system ROMs (Nextor/Sunrise IDE,
Carnivore2, MegaRAM) manage their own boot flow and are intentionally excluded, exactly like the
50/60 Hz option.

Values the current machine cannot deliver are never shown, and the whole row is hidden on machines
that support neither.

---

## 2. User guide

1. Highlight a ROM in the Explorer menu and press **ENTER** to open its detail screen.
2. A `CPU` option is shown (between `Frequency` and `SD Part`) and cycled with **LEFT / RIGHT**
   through the values this machine supports.
3. Press **ENTER** on `Action: Run` to launch. The selection is saved per ROM and restored the next
   time the detail screen for that ROM is opened.

### What to expect from `R800`

On a turbo R the **external cartridge slot is always driven with Z80-compatible bus timing**,
in both R800 ROM mode and R800 DRAM mode — the S1990 inserts the wait states required for
compatibility and honours the cartridge `/WAIT` line. The R800 speed-up therefore applies to
internal RAM and BIOS access, not to code fetched from the PicoVerse. Games that run mostly from
cartridge ROM will see a modest gain; games that relocate to RAM benefit much more.

`R800` is also the riskier of the two options: mapper writes are captured by the PicoVerse PIO
without asserting `/WAIT`, so a faster CPU shortens the window in which core 0 must drain the write
FIFO. If a game misbehaves, set the option back to `Default`.

---

## 3. CPU mode encoding

A single byte carries the selection everywhere (menu state, `.PVC` file, control registers):

```c
#define CPU_MODE_DEFAULT 0u
#define CPU_MODE_TURBO   1u
#define CPU_MODE_R800    2u
```

---

## 4. Machine detection (MSX side)

`build_cpu_mode_list()` in `screen_rom.c` builds the list of selectable values once per detail
screen. It is written in assembly to keep the menu ROM small.

**Panasonic turbo** uses the switched-I/O convention: the manufacturer id is written to port `#40`
and reading the port back returns its complement. Matsushita/Panasonic is id `8`, so a machine with
the extension answers `#F7`. Port `#41` bit 2 then reports whether the 5.37 MHz turbo is actually
fitted (`0` = available):

```asm
        in   a,(#0x40)      ; save the currently selected device id
        cpl
        push af
        ld   a,#8
        out  (#0x40),a      ; select Matsushita
        in   a,(#0x40)
        cpl
        cp   #8             ; does the read-back match?
        jr   nz,no_pana
        in   a,(#0x41)
        bit  2,a            ; bit2 = 0 -> 5.37MHz turbo fitted
        jr   nz,no_pana
        ; ... CPU_MODE_TURBO is available
no_pana:
        pop  af
        cpl
        out  (#0x40),a      ; restore the previous device id
```

Saving and restoring the `#40` selection makes the probe harmless on machines without expanded I/O.

**turbo R** is detected from **MSXVER**, the MSX version byte at main-ROM `#002D`
(`0` = MSX1, `1` = MSX2, `2` = MSX2+, `3` = turbo R):

```asm
        ld   a,(#0x002D)
        cp   #3
        jr   nz,no_r800
        ; ... CPU_MODE_R800 is available
no_r800:
```

---

## 5. Persistence and communication

The value is stored as **byte 10** of the ROM's `.PVC` companion file, growing it from 10 to 11
bytes. Older files still load and default the CPU to `Default`; see
[the 50/60 Hz document](./msx-picoverse-2350-50-60hz.md) for the full `.PVC` layout.

| Direction | Channel |
|---|---|
| MSX → Pico (save) | query buffer **byte 8**, with `CMD_SAVE_OPTIONS` |
| Pico → MSX (load) | `CTRL_CPU_MODE` register at **`0xBFA2`**, after `CMD_LOAD_OPTIONS` |

`0xBFA2` sits in the free gap between the SD partition info buffer (`0xBF80..0xBF9F`) and the
chip-id buffer (`0xBFAF..0xBFBF`), next to `CTRL_VDP_FREQ` (`0xBFA0`) and `CTRL_NET_STATUS`
(`0xBFA1`). Like `CTRL_VDP_FREQ` it is only served while the File Hunter status window is inactive.

Both sides sanitize the value (anything `> CPU_MODE_R800` becomes `Default`), and the MSX menu
additionally re-maps a stored value the current machine cannot deliver back to `Default` — a
microSD card may have been configured on a different computer.

---

## 6. How the switch is applied

The Explorer launches a game by writing the entry index to `ROM_SELECT_REGISTER` and executing
`rst 00h`. That re-runs the whole BIOS boot, which returns a turbo R to **Z80 mode** and re-runs the
Panasonic firmware's own speed setup. Switching the CPU from the menu before the reset therefore
does not survive, so the switch is applied where the 50/60 Hz option is already applied: in the
**launched ROM's cartridge INIT**, patched into the cached image the Pico serves.

At INIT time the BIOS is in page 0, so `call #0180` is valid exactly like the existing `call #0047`.

**Turbo stub** (14 bytes + `jp`):

```asm
3E 08        ld  a,8          ; Matsushita device id
D3 40        out (#40),a
DB 40        in  a,(#40)
FE F7        cp  #F7          ; still a Panasonic machine?
20 04        jr  nz,skip
3E 80        ld  a,#80        ; bit0 = 0 -> 5.37 MHz
D3 41        out (#41),a
skip:
C3 ll hh     jp  <orig_init>
```

**R800 stub** (12 bytes + `jp`):

```asm
3A 2D 00     ld  a,(#002D)    ; MSXVER
FE 03        cp  3            ; turbo R?
20 05        jr  nz,skip
3E 81        ld  a,#81        ; R800 ROM mode + turbo LED
CD 80 01     call #0180       ; CHGCPU
skip:
C3 ll hh     jp  <orig_init>
```

Both stubs re-verify the machine at run time even though the menu already filtered the option.
This matters because the `CMD_PREPARE_QUICK_RUN` path replays a stored option without opening the
detail screen, so an `R800` value written on a turbo R must not call `#0180` on an MSX2.

### Stub placement

The writable part of an MSX cartridge header is only 12 bytes (`0x4004..0x400F`), which is not
enough for a CPU stub, let alone a combined frequency + CPU stub (up to 23 bytes). `apply_boot_patches()`
therefore:

1. builds the concatenated stub for whichever options are active;
2. writes it straight into the header when it fits in 12 bytes;
3. otherwise parks it in a run of `0x00`/`0xFF` padding inside the page-1 window and leaves a 3-byte
   `jp <stub>` trampoline in the header;
4. falls back to the frequency-only stub (which always fits) when no padding run exists.

The padding search runs backwards over the first 8 KB of the cached image and takes the top of the
highest qualifying run, so the stub lands in the trailing padding of block 0 whenever there is any —
far less likely to be read by the game than a zero-filled table in the middle of its data. The 8 KB
limit is deliberate: block 0 is what sits at `0x4000..0x5FFF` at INIT time for every cached mapper,
including ASCII8, which resets **all four** 8 KB windows to block 0 and therefore does not expose
image offsets `0x2000..0x3FFF` at boot at all. It is also what the `0x4000`-relative stub address
assumes.

---

## 7. Files changed

| File | Change |
|---|---|
| `2350/software/explorer.pio/msx/src/menu.h` | `CPU_MODE_*` values, `CTRL_CPU_MODE` (`0xBFA2`) |
| `2350/software/explorer.pio/msx/src/screen_rom.c` | machine probe, `CPU` option row, load/save plumbing |
| `2350/software/explorer.pio/pico/explorer/explorer.c` | `.PVC` byte 10, `ctrl_cpu_mode` read-back, `cpu_mode_launch` gating, `build_boot_stub()` / `find_boot_stub_slot()` / `apply_boot_patches()` |

---

## 8. Limitations & notes

- The turbo R always boots in Z80 mode; the stub re-applies the choice on every launch.
- `R800` selects **ROM mode** (`A = #81`). DRAM mode (`A = #82`) is not offered.
- Neither switch is undone when the game exits — the next `rst 00h` boot resets the CPU anyway.
- If the ROM has no run of at least ~15-23 padding bytes in its first 16 KB, the CPU switch is
  skipped silently and only the frequency stub (if any) is applied.
- Cartridge hardware in general is more sensitive in R800 mode; RBSC likewise does not guarantee
  Carnivore2 operation in R800 mode on turbo R machines.

---

## 9. Credits and reference

The CPU-switching techniques were implemented with reference to two public MSX projects:

- **`Z80-R800`** by **GDX** — MSX-DOS/BASIC commands that switch the turbo R CPU mode, showing the
  `CHGCPU` (`#0180`) call convention (`A = #80` for Z80, `#81` for R800 ROM mode) and the MSXVER
  check used to detect a turbo R. Repository: https://github.com/gdx-msx/Z80-R800
- **`msx-turbo`** by **Papipapito** — a command-line 3.58/5.37 MHz switch for Panasonic MSX2+
  machines, documenting the switched-I/O device 8 protocol on ports `#40`/`#41` and its polarity.
  Licensed under the MIT License. Repository: https://github.com/Papipapito/msx-turbo

The Panasonic port definitions were cross-checked against the MSX Assembly Page expanded-I/O port
table (https://map.grauw.nl/resources/msx_io_ports.php), and the `CHGCPU`/`GETCPU` entries against
the MSX BIOS table (https://map.grauw.nl/resources/msxbios.php).

PicoVerse keeps its own RP2350 cartridge-side implementation (a per-ROM option persisted in `.PVC`
files and applied by injecting the switch into the launched game's cartridge INIT), while
gratefully acknowledging the reference work above.
