# Change Log

## PicoVerse 2350 Explorer v2.48

- Bumped Explorer version to v2.48.
- Fixed MSX-MUSIC (FM-PAC) playback where the music level pumps up and down and clicks, reported against `Shrines of Enigma (1995) Element [DSK2ROM Conversion]` with the FMPAC profile and **PSG Mirror enabled**. The cause was the FM/PSG channel routing in `msx_music_write_stereo_sample()`, not the YM2413 emulation or the bus interface.
- Removed the conditional hard-pan. The routing was decided per sample: while the mirrored PSG had audible channels the FM went to the left channel and the PSG to the right, and while it did not the FM went to *both* channels. Every time a game started or stopped a PSG sound effect the FM level therefore moved in a mono sum, and the right channel cut instantly between two unrelated signals. Driving the real `emu2413` and `emu2149` through both paths on the host with a six-channel FM backing plus a sound effect toggling four times a second measured a **5.47 dB swing** in the mono FM level and 7 full-scale sample-to-sample discontinuities over four seconds; the new path measures **+1.11 dB** (an added source correctly makes the mix slightly louder) with **zero** discontinuities.
- Replaced it by summing FM and PSG into both channels, the way a real FM-PAC and the machine's own PSG both reach the MSX audio bus, and the way the YM2151/SFG profile in the same firmware already mixes its mirrored PSG. The FM level is now independent of PSG activity.
- Added a DC blocker on the mirrored PSG before it enters the mix. `emu2149` output is unipolar (three 0..4080 channel levels summed), so it carries a pedestal roughly half its own amplitude that would have offset the FM and stepped on every note. It reuses the ~35 Hz pole already used on the FM side, and is fed zero while the PSG is inaudible so the gate relaxes instead of cutting.
- Fixed `main_psg_calc_audible_sample_shifted()` skipping `PSG_calc()` whenever the mixer/volume state made the PSG inaudible. Freezing the emulator stops its tone, noise and envelope phase, so the waveform resumed mid-cycle from stale state and clicked the moment a channel became audible again. The emulator is now clocked on every sample and only its *contribution* is gated. This also removes the same latent click from the YM2151/SFG profile, which shares the helper.
- Replaced `MSX_MUSIC_VOLUME_SHIFT 2` (4x) applied to `OPLL_calc()` output. `emu2413` halves its six melody slots but not its five rhythm slots, so the chip reaches +/-27340 in practice (melody-only maximum +/-18500, structural bound +/-32757). Multiplying that by 4 gives +/-109360, more than three times what the output stage can carry, which left the soft-knee limiter permanently engaged on dense music. A worst-case host measurement (nine channels, maximum volume) had **21.84% of samples compressed by up to 7.03 dB** - heard as the FM gain pumping.
- Substituted a fixed-point gain of `56/32` (1.75x), mirroring the `109/32` treatment the SCC mixer received in this release. It maps the melody-mode maximum onto +/-32375 so ordinary music never reaches the knee. Limiter engagement on the worst case drops from 21.84% to **0.08%** (worst 7.03 dB -> 0.89 dB), while the peak level of loud passages is nearly unchanged (31375 -> 27836), because those passages were being limited down to that level anyway. Quiet passages get their correct relative level back instead of being pushed into the compressor.
- Moved the soft limiter from the FM sample to the final FM + PSG sum, so it now bounds what actually reaches the DAC rather than one source in isolation.
- Reset the FM and PSG filter state in `msx_music_init()` as well as on Core 1 entry, so the Sunrise + FM-PAC servicing path starts from silence instead of inheriting filter history from a previous launch.
- Fixed SCC audio breaking up into continuous harsh noise, reported against ROMs converted to SCC from other sound sources (`Zanac A.I. - Compile (1986) [TFH SCC version]`) and most obvious when a game is paused with STOP. The cause was gain staging in the SCC mixer, not the SCC emulation itself and not the bus interface.
- Replaced the `SCC_VOLUME_SHIFT 2` (4x) left shift applied to `SCC_calc()` output. An SCC channel peaks at `volume(15) * wave(-128) = -1920`, so all five channels together span +/-9600; multiplying that by 4 gives +/-38400, well past the 32767 the following clamp allows. Any SCC output above raw 8191 was therefore squared off. A worst-case measurement (five channels, maximum volume, phase aligned) had **49.97% of samples hard-clipped**.
- Substituted a fixed-point gain of `109/32` (3.40625x), which maps the SCC's full +/-9600 range onto +/-32700 so the SCC can no longer clip against its own output stage. Nominal loudness drops about 1.4 dB relative to the old 4x shift, which is the amount that was previously being lost to distortion anyway.
- Removed the double hard clamp in the SCC mixing path. `boosted` was clamped to int16 *before* `main_psg_calc_sample()` was added, and the sum was then clamped a second time. Since the PSG is independently boosted to full scale (`3 * 4080 * 4 = 48960`), any SCC game with PSG accompaniment was clipping twice.
- Added `soft_limit_i16()`, a soft limiter now used for the combined SCC + PSG sum in `core1_scc_audio()` and `scc_audio_service_buffer()`. Below 26000 the signal passes through bit-exact; above it the excess is compressed along an asymptote toward, but never reaching, full scale, so the result is bounded by construction and needs no clamp. Verified transfer points: 26000 -> 26000, 32700 -> 28424, 65467 -> 31189.
- Confirmed the fix against the real `emu2212.c` compiled on the host and driven through the same mixing arithmetic as the firmware. Hard clipping went from 49.81%/49.97%/50.34%/49.88% (five-channel chord, five-channel unison, unison plus full-scale PSG, three-channel chord plus full-scale PSG) to **0.00% in every case**, with single-channel and three-channel content unchanged.
- Investigated and ruled out the reduced oversampling in `emu2212.c` (`SCC_OVERSAMPLE_DIV 8` instead of the upstream 2). Building the emulator both ways and comparing spectra at 440 Hz showed the shipped divisor is in fact cleaner, at 22.0 dB non-harmonic energy versus 14.1 dB, so it was left alone.
- Noted but did not change the absence of a spin lock around `scc_instance`, unlike `main_psg_lock` for the PSG. Core 0 `SCC_write()` does race Core 1 `SCC_calc()`, but every shared field is a byte or an aligned 32-bit store that cannot tear, so the worst case is an isolated single-sample click rather than the sustained noise being reported.
- Explains the pause symptom: the SCC has no envelope generator, so a note sounds indefinitely until the music driver writes new register values. With the driver stopped, a loud sustained chord sat permanently inside the clipper, turning a held note into a continuous buzz.
- Replaced the per-ROM `Frequency` / `CPU` boot patch with a Pico-served stub, so the options now work with **any** ROM instead of only those with enough spare padding. Previous releases hid the stub in a run of `#00`/`#FF` bytes inside the image; densely packed ROMs have no such run, and a 120-ROM sweep left 9 of them unpatchable while every DSK2ROM image failed outright. Inspired by how SofaRun applies the setting from outside the ROM, adapted to a cartridge that must survive the launch reset.
- Stopped searching for padding altogether. The stub is written straight over whatever bytes occupy the chosen slot, the originals are kept in `boot_patch_saved[]` along with the original INIT vector, and the Pico puts them all back the moment the MSX reads the stub's final byte. The MSX only ever sees the stub while the BIOS is calling INIT; from the game's first instruction the image is byte-for-byte original again.
- Hooked the restore into `read_rom_byte()`, the single read path shared by every cached game mapper, so no mapper loop needed changing. The check is one compare against `boot_patch_trigger_rel`, which holds a sentinel offset no read can produce unless a patch is actually pending.
- Anchored the stub to end exactly at image offset `#2000`. ASCII8 maps block 0 into all four 8 KB windows at reset, so a stub straddling that boundary would continue into the wrong bytes; ending on it also keeps the stub clear of the header. With both a frequency and a CPU option the stub is 28 bytes, so it always fits inside block 0.
- Deliberately avoided image offset `#10`, the obvious spot after the header. On a disk ROM — which is what DSK2ROM produces — that offset holds the driver jump table (`INIHRD` / `INIENV` / `DRIVES` / `DSKIO` / ...), and the BIOS may call one of those entries during the window in which the stub is installed. `Herzog - Tecno Soft (1989) [dsk2rom]` has exactly such a table at `#4010`.
- Restored the original INIT vector along with the covered bytes, so a later soft reset re-runs the untouched game instead of jumping at a stub that is no longer there. The trade-off is that a manual reset boots at the machine default rather than re-applying the option.
- Kept `Turbo` / `R800` on the same mechanism, so the CPU option is no longer dropped when the frequency stub has to fall back — both fragments are simply part of the one served stub.
- Cleared any pending patch at the start of `prepare_rom_source()`. `apply_boot_patches()` is only reached on the caching path, so a mapper that skips caching could otherwise inherit an armed trigger from an earlier launch.
- Verified against every ROM on the test microSD plus the DSK2ROM image: 155 of 156 patch and restore byte-for-byte across all frequency/CPU combinations, with no early or double restore. The one exception is `Super Lode Runner`, whose header declares `INIT=#0000` — it has no INIT routine to redirect and is correctly left alone.
- Confirmed on the Panasonic FS-A1FX and Philips NMS 8245 openMSX profiles with their real BIOS dumps that the stub runs before anything reads its trigger byte, that forcing 50 Hz gives R9/RG9SAV `02`/`82` and 60 Hz gives `00`/`80` on both machines, and that the Herzog DSK2ROM image boots through its disk loader into the game holding the forced mode.
- Left NEO8/NEO16, ASCII16-X and Planar64 unsupported, as before: the first three do not use the shared ROM cache and Planar64 carries its header at image offset `#4000` rather than 0.
- Added a bus/audio tracer (`debug/audio_trace.c`, `debug/audio_trace.h`) to investigate SCC noise that a beta tester still reproduces after the mixer fix above, but **only on certain Yamaha machines and only with PSG Mirror enabled**. That combination points at the mirrored I/O capture path rather than the SCC emulation, since the SCC is fed from the memory bus and is unaffected by the PSG Mirror setting.
- Gated the tracer on the existing `EXPLORER_USB_STDIO_DEBUG` switch rather than adding a second build flag. The tracer has no output path of its own and every reporting function early-returns unless the USB CDC link is up, so an independent flag would only have created a configuration that costs RAM and hot-path cycles while printing nothing.
- Recorded events into one ring buffer **per core** (1024 entries each, 8 bytes per entry). A producer only ever increments an index no other core writes, so the hot paths need no lock, no atomic and no cross-core exclusive monitor. The two rings are merged by timestamp when dumped, using unsigned difference so ordering survives the 32-bit microsecond wrap.
- Instrumented `pio_try_get_io_write()`, the single funnel for every I/O write captured from the MSX bus. The tracer decodes the PSG register-select (`#A0`) / data (`#A1`) pairing and attributes each data write to the register selected before it, so a desynchronised pair is visible in the log even when both writes look plausible on their own.
- Instrumented the SCC by wrapping `SCC_write()` with a traced inline in `explorer.c`, defined before the macro that redirects to it so the wrapper still reaches the real emulator entry point instead of recursing. This covers every SCC write site in the file (Konami SCC, Manbow2, MegaRAM, Sunrise external SCC) rather than only the mapper under suspicion.
- Instrumented `main_psg_write_ring_push()` so a full core0 -> core1 hand-off ring is counted and logged instead of silently discarding the write.
- Added PIO FIFO stall detection through the `FDEBUG` register, which is the decisive evidence for the dropped-write hypothesis: `RXSTALL` on the I/O write captor means the state machine tried to push while the FIFO was full, so MSX bus writes were **lost** rather than merely delayed. A lost register-select write would send the following data byte to the wrong PSG register, which is audible as noise and would persist while a game is paused. The memory write captor and the I/O read responder are sampled the same way; flags are write-1-to-clear so each event counts once.
- Sampled the stall flags and handled all reporting from `audio_trace_service()`, called once per 256-sample audio buffer from `core1_scc_audio()` and `scc_audio_service_buffer()` — never from the per-sample path and never from core0. Core0 must not block: it holds `/WAIT` while the PIO read state machine waits for a token, so a long print there would freeze the Z80 and lose VDP interrupts.
- Added automatic trigger dumps for the four anomalies that would explain the report (hand-off ring overflow, PSG register select > 15, I/O write captor stall, memory write captor stall), rate limited to one every 2 s and capped at 400 events, so a healthy run prints nothing while a fault captures its own pre-history.
- Added interactive USB CDC commands so the tester can capture the moment the noise starts: `d` dump ring, `s` counters plus live PSG/SCC register state, `r` per-register PSG write histogram, `a` include or exclude non-PSG events, `p` pause capture, `c` clear, `m` insert a marker, `h` help. All reporting is gated on `stdio_usb_connected()`, because with no terminal attached each `printf()` would block for the full stdout timeout and stall the audio core.
- Replaced the literal delay in both write captors in `msx_bus.pio` with named `.define` constants (`MSX_MEM_SETTLE`, `MSX_IO_SETTLE`, both still 2) and verified the generated opcodes are byte-identical (`0xa242`, `nop [2]`), so the timing knob is documented without any behaviour change. The I/O captor is the interesting one: an I/O write never asserts `/SLTSL`, so the point in the cycle at which the cartridge slot data buffers are actually driven is machine dependent — a candidate explanation for a fault that appears only on certain machines.
- Confirmed both configurations build clean and that the default production build is unchanged, with every hook compiling to `((void)0)` when the tracer is off.
- **Diagnosed the Yamaha + PSG Mirror noise from the tester's traces as an I/O write data-bus sampling error, and fixed the I/O write captor.** Two logs of the same ROM on the same machine, one with PSG Mirror off (clean audio) and one with it on (noise), isolate the fault to the mirrored I/O path.
- Established from the counters that **every** mirrored PSG register select was invalid: `sel=107573`, `badSel=107573`, i.e. 100%, not an occasional race. `drops=0` and `memStall=0` in the same run rule out the lost-write and hand-off-overflow theories entirely, and `scc=225754` with a healthy register spread (`9000`, `9880`-`988F`) confirms the SCC path over the memory bus is correct. The SCC emulation was never at fault.
- Identified the mechanism from the captured values: the byte sampled on `D0`-`D7` **equals the port number** for `OUT (n),A` traffic - port `99` matched 98.0% of the time (mean Hamming distance 0.02), port `AA` 96.5%, and port `A0` produced only `A0`/`A4`/`AC`/`AF`. Port `98` was the exception at 0.2%, which fits: VDP fills use `OUTI`/`OTIR`, whose preceding memory read is the data byte itself rather than an operand. The captor was therefore latching the operand byte left over from the preceding opcode fetch on a bus the machine had not yet driven.
- Traced the audible result through `PSG_writeIO()`, which masks the register with `& 0x1f`. The garbage selects `AC`/`AF`/`A4`/`A0` alias onto registers 12, 15, 4 and 0 - envelope period, ioB, channel C frequency and channel A frequency - which then received garbage data. That is a continuous-tone generator, and it explains why the noise survives a STOP pause and why it never appears with PSG Mirror disabled. (`psg->adr = val & 0x1f` does bound the index, so there was no out-of-bounds write into the PSG state.)
- Replaced the fixed settling delay in `msx_io_write_captor` with a re-sampling loop that keeps the last snapshot taken while `/WR` is still asserted. An I/O write never asserts `/SLTSL`, so on machines that gate the cartridge slot data buffer the real byte appears late in the cycle; latching ~14 ns after `/WR` falls captured the undriven bus. Real I/O hardware latches on the trailing edge of `/WR`, which is what the loop now approximates. It needs no per-machine tuning and adapts to 3.58 MHz, turbo and R800 alike.
- Left `msx_write_captor` (memory writes) unchanged at `nop [2]`, verified byte-identical in the generated header (`0xa242`). Memory writes assert `/SLTSL` early, the traces show that path working correctly, and there was no evidence to justify disturbing it.
- Fixed a tracer bug that made the PSG-Mirror-off log report `ioStall=9553`. With the mirror disabled the I/O state machines are never initialised, so `msx_io_bus` is all zero and the stall poll dereferenced a NULL `PIO`, reading a ROM word as if it were `FDEBUG` and reporting a large fictitious count that could never be cleared. The poll now skips uninitialised state machines; that figure in the first round of logs should be disregarded.
- Fixed the SCC counter classification, which reported `freq=0` and `vol=0` in both logs. The wavetable range was too wide (`0x800`-`0x89F`) and swallowed the standard-SCC frequency and volume registers at `0x880`-`0x88F`, while the frequency and volume ranges used the SCC+ offsets only. Both layouts are now classified.
- Extended the tracer to record the **full 16-bit** I/O address rather than just the port. On an `OUT (n),A` the Z80 drives the port on `A0`-`A7` and the value being written on `A8`-`A15`, so the address bus carries an independent copy of the data byte. The dump now prints `D0-7` and `A8-15` side by side and flags `[D=port: STALE BUS]` or `[D=A8-15: bus OK]`, and the counters report `D==port` and `D==A8-15` percentages, so the next capture confirms or refutes the fix directly rather than by inference. The ring was halved to 512 entries per core to offset the wider record.
- **Gave every file the firmware writes to the microSD card a real timestamp**, instead of only the File Hunter ROM downloads fixed earlier. `.PVC` option files, the `PICOVERSE.PVC` browse-partition config and the last-executed-entry file were all being created with a null FAT date, which file managers show as blank and which sorts unpredictably.
- Traced the cause to `get_fattime()` in the vendored FatFS RTC shim: it reports the RP2350 always-on timer, which the Explorer never starts, so it returned `0` - FatFS's "no timestamp" value - on every call. Downloads escaped only because they set `fatfs_set_fattime_override()` from the HTTP `Last-Modified` header first.
- Added a firmware wall clock in `explorer.c` seeded from the WiFi module. The `Date` response header of *any* File Hunter request (search, list page, download, WiFi status) carries the server's current time; it is parsed by the existing `fh_parse_http_timestamp()` and the Pico's own `time_us_64()` keeps it running from there. `Last-Modified` is deliberately not used for this: it is the age of the file being fetched, not the current time. The clock is re-seeded on every response, so it cannot drift over a long session.
- Chose the timestamp for a `.PVC` options file in the requested order: the WiFi time when the firmware has it, otherwise the date of the ROM the options belong to (read with `f_stat()` on the microSD ROM through the new `build_rom_source_path()`), otherwise 1980-01-01 00:00:00, the FAT epoch. Flash entries write `/<NAME>.flash.PVC` and have no source file on the card, so they use the WiFi time or the FAT epoch.
- Applied the same wall clock to the other three files the firmware creates, and made a ROM download fall back to the wall clock when the server sends no `Last-Modified`. The override is now cleared on every path, including the error paths.
- Kept the date arithmetic self-contained (`days_from_civil` / `civil_from_days` on integers only) rather than depending on the C library's time zone handling, and kept all of it in `explorer.c`: the vendored FatFS library is git-ignored, so putting new code there would have been lost on a fresh clone and broken the build.
- Verified the conversion on the host against `_mkgmtime` for **all 46751 valid dates from 1980-01-01 to 2107-12-31**, with an exact FAT round trip for each, plus month and leap-day rollovers (2026-02-28 23:59:58 + 5s -> 2026-03-01 00:00:02; 2028-02-28 23:59:58 + 5s -> 2028-02-29 00:00:02) and the full source-priority matrix. Caught and fixed a 32-bit overflow in the seconds counter that would have wrapped in February 2106.
- Note that HTTP `Date` is UTC and FAT timestamps carry no time zone, so files are stamped in UTC. This matches what the existing download fix already did. No MSX-side change was needed, so the menu ROM is untouched.

## PicoVerse 2350 Explorer v2.47

- Bumped Explorer version to v2.47.
- Fixed the per-ROM `Frequency` option having no effect on real hardware. v2.46 booted correctly but applied the refresh mode with a raw `out (#99),a` register write plus a manual `RG9SAV` (`#FFE8`) poke. That is enough in emulation but did not take effect on a real Panasonic FS-A1FX or Philips NMS 8245, where the game kept running at the machine's default rate.
- Replaced the frequency fragment with the sequence the Carnivore2 boot menu uses, which is known to work on those machines: read the current `RG9SAV`, flip **only** bit 1 (`set 1,a` / `res 1,a`), and apply it through the BIOS `WRTVDP` entry (`call #0047` with `B` = value, `C` = 9). On MSX2 `WRTVDP` routes register 9 through the SUB-ROM, which performs the full machine-correct update of both the VDP register and its shadow.
- Preserved every other bit of R9 instead of writing a bare `#00`/`#02`. The previous code cleared the line-count, interlace and display-control bits as a side effect of forcing the refresh mode.
- Corrected the v2.46 claim that `WRTVDP` cannot be called from a cartridge INIT. That conclusion came from a faulty test of mine that used `CB CE`/`CB 8E` (`SET`/`RES 1,(HL)`) with an uninitialised `HL` instead of `CB CF`/`CB 8F` (`SET`/`RES 1,A`), so the value handed to `WRTVDP` was never modified and the call looked inert. Re-tested with the correct opcodes, `WRTVDP` works from a cartridge INIT. The real cause of the v2.45 boot failure was the corrupted cartridge header alone, which v2.46 fixed.
- Kept the full stub at 14 bytes (an 11-byte frequency fragment plus its jump to the game's INIT), the same size as in v2.46.
- Fixed a latent race in `prepare_rom_source()`: `/WAIT` was released immediately after the DMA copy, while `rom_cached_size` and `apply_boot_patches()` were still pending. A machine stalled mid-read of the cartridge header could complete that read against the unpatched image. `/WAIT` is now released only after the cache bookkeeping and the boot patch are in place.
- Added per-ROM `Frequency` support for ROMs built with **DSK2ROM**, which previously ignored the setting entirely. These are ASCII8 images, and an ASCII8 cartridge maps image block 0 into all four 8 KB windows at reset, so only the first 8 KB is reachable when the BIOS calls the cartridge INIT. `Herzog - Tecno Soft (1989) [dsk2rom]` has nothing larger than a 6-byte run of padding in that block, so neither the 14-byte stub nor the 8 + 9 two-cave split fitted and the patch was silently declined.
- Replaced the fixed one-or-two-cave placement with a general chaining pass (`place_boot_stub()` / `find_boot_cave()`). The stub is now split across as many padding runs as needed, up to four, with each fragment ending in a `jp` to the next. Runs as short as 4 bytes are usable (one code byte plus the jump). The Herzog ROM is placed across its three 6-byte runs.
- Restricted fragment boundaries to instruction boundaries. The chaining pass records the offsets at which the stub may legally be cut and only splits there, so a multi-byte instruction can never be severed across two runs. The CPU fragments contain relative `jr nz` jumps and are therefore emitted as indivisible blocks, which is why a CPU option is dropped when it cannot be placed whole.
- Added a compact frequency-only fallback for ROMs that still cannot host the full stub: `ld bc,r9*256+9` / `call #0047` / `jp` in 9 bytes. It writes R9 outright rather than preserving the register's other bits, which is safe here because a cartridge INIT runs while the BIOS still has its boot defaults loaded. The CPU option has no compact form and is dropped when this fallback is used.
- Kept the header guarantee unchanged: only the INIT word at `#4002` is ever rewritten, and offsets `#4004`..`#400F` are left untouched in every placement path. A ROM that cannot host any stub is left completely unpatched so selecting a frequency can never stop it from booting.
- Improved placement coverage across a 120-ROM microSD sweep from 99 patched / 21 declined to **111 patched / 9 declined**, with no header corruption in any case.
- Verified on the Panasonic FS-A1FX and Philips NMS 8245 openMSX profiles with their real BIOS dumps, driving every ROM through the actual firmware patch code: the Herzog dsk2rom image forced to 50 Hz reaches R9/RG9SAV `02` and holds it through the emulated disk boot into the running game, while King's Valley II still forces 50 Hz on the FS-A1FX (`82`) and 60 Hz on the NMS 8245 (`80`).

## PicoVerse 2350 Explorer v2.46

- Bumped Explorer version to v2.46.
- Fixed games not starting when the per-ROM `Frequency` option was set to `60Hz` or `50Hz` (reported with `King's Valley II - The Seal of El Giza`, but it affected any patched game). The v2.41 INIT stub applied the frequency with `call #0047` (BIOS `WRTVDP`), which on MSX2 does not simply write the port: registers `>= 8` — so VDP R9 — are forwarded to the SUB-ROM through an inter-slot `EXTROM`/`CALSLT` call (`push ix / ld ix,#012D / jp #0295`). Doing that from a cartridge INIT, while the BIOS boot slot scan is still running, depends on boot state that is not established yet and is machine dependent, so the launched ROM never got control. The stub now writes VDP R9 directly through port `#99` again and makes no BIOS call at all.
- Restored the `RG9SAV` (`#FFE8`) mirror that the `WRTVDP` call was introduced for, without the BIOS: the stub writes the shadow itself. This matters in practice — the BIOS reloads R9 from `RG9SAV` on a screen call, so a stub that only wrote the port had the requested refresh mode reverted within a couple of seconds of the game starting.
- Kept the two port `#99` writes inside the cartridge INIT path, which the BIOS invokes through an inter-slot call with interrupts disabled.
- Stopped storing executable bytes in cartridge header offsets `#4004`..`#400F`. The first v2.46 fallback still put the start of a split stub there, making the `STATEMENT`, `DEVICE` and especially `TEXT` words nonzero; on a real Panasonic FS-A1FX the BIOS treated that `TEXT` value as a tokenized BASIC program pointer after INIT, so King's Valley II fell through to the MSX logo/BASIC screen. Every successful patch now changes only the INIT word at `#4002` and leaves all remaining header fields at their original values.
- Corrected the stub relocation window. It was documented as "block 0 because that is what sits at `#4000` at INIT time", but the real constraint is the *page*: when the BIOS calls a cartridge INIT it has the cartridge enabled in page 1 only, and page 2 still holds RAM until the ROM enables its own slot there (King's Valley II does exactly that with `ENASLT`, `H=#80`). The search now covers the whole 16 KB page-1 window (`#4000`..`#7FFF`) for the sequential-block mappers and stays at 8 KB for ASCII8, which remaps every 8 KB window to block 0 at reset.
- Changed `find_boot_stub_slot()` to keep the *largest* qualifying padding run instead of the highest one. Over the wider window the highest run is often a short gap between data tables, while the largest one is almost always genuine trailing padding.
- Added a header-safe two-cave fallback for ROMs whose page-1 window has no single 14-byte padding run. An 8-byte head writes `RG9SAV` and jumps to a separate 9-byte tail that writes VDP R9 and jumps to the original INIT; King's Valley II provides suitable runs at image offsets `#1948` and `#1DC8`. The two ranges are explicitly checked for overlap. If the full stub and the two-cave form both do not fit, the ROM is left untouched so selecting a frequency can never prevent it from booting.
- Verified the exact generated bytes on the Panasonic FS-A1FX openMSX profile with its real MSX2+ BIOS: both forced modes reach King's Valley II's original INIT, 60 Hz settles at R9/RG9SAV `#80`, and 50 Hz settles at `#82`, while header offsets `#4004`..`#400F` remain zero. A 120-ROM placement sweep uses one full cave for 92 ROMs, two caves for 7 ROMs, and safely leaves 21 ROMs unpatched rather than corrupting their headers.
- Updated `docs/msx-picoverse-2350-50-60hz.md` for the direct VDP write, the `RG9SAV` mirror, header-safe placement and the page-1 relocation window.

## PicoVerse 2350 Explorer v2.45

- Bumped Explorer version to v2.45.
- Changed the `F3 - File Hunter` opening list to show the latest releases instead of the ROMs whose names start with `1`. The catalog is now requested from the new `http://msxpico.file-hunter.com/new-pico.php` endpoint, which returns the most recently added ROMs newest-first in the same packed listing struct (`base=1BA0`, 32-bit name offset + uint16 size + uint8 load type) already parsed by the Explorer, so the list parser, page buffer and download path are unchanged.
- Added `FH_LATEST_ENDPOINT` (`/new-pico.php`) next to the existing `FH_ENDPOINT` (`/picoverse.php`) and made `fh_build_http_path()` select between them: an empty query builds `new-pico.php?base=1BA0&type=rom&download=`, and any non-empty query keeps the name-filtered `picoverse.php?base=1BA0&type=rom&msx=&char=<query>&download=` request used by the `/` search key. Both endpoints index `&download=<index>` against the listing they returned, so ROM downloads keep working in either mode.
- Made an empty query the "latest releases" selector: `fh_copy_query_from_buffer()` no longer substitutes the old `1` default, and the File Hunter state is initialized with an empty query instead of `1`. An empty query cannot be produced by the search prompt (it returns "cancelled" when nothing is typed), so it is unambiguous.
- Made the MSX menu request the latest list on every `F3` entry by issuing the empty query as a search (`CMD_FH_SEARCH`) instead of writing the `1` query and paging. The previous code reused the cached catalog whenever one was already loaded, so leaving File Hunter and pressing `F3` again came back to the results of the last search instead of the current releases.
- Updated the Explorer tool manual "Opening File Hunter" section to describe the latest-releases list and the refresh-on-entry behavior.
- Added a per-ROM `CPU` option to the ROM detail screen that boots the selected game with the Panasonic 5.37 MHz turbo (`Turbo`) or MSX turbo R R800 ROM mode (`R800`) instead of the machine default. The row appears between `Frequency` and `SD Part`, cycles with LEFT/RIGHT like the other options, and is offered only for regular game ROMs — the system ROMs (Nextor/Sunrise/C2/MegaRAM) manage their own boot and are never patched.
- Detected the available CPU speeds on the MSX side in `build_cpu_mode_list()` and hid the row entirely when the machine offers no alternative. `Turbo` is listed when the Panasonic switched-I/O turbo device answers (write manufacturer id `8` to port `#40`, read back `#F7`, then port `#41` bit 2 clear = 5.37 MHz fitted); `R800` is listed when MSXVER (main ROM `#002D`) is 3. The probe saves and restores the previously selected `#40` device id, so it is harmless on machines without expanded I/O.
- Applied the CPU speed by extending the existing cartridge-INIT boot patch instead of switching from the menu: the `rst 00h` launch re-runs the BIOS boot, which returns a turbo R to Z80 mode, so the switch has to happen when the launched ROM's INIT runs. `Turbo` emits `out (#40),8 / out (#41),#80`; `R800` emits `call #0180` (CHGCPU) with `A=#81` (R800 ROM mode plus turbo LED).
- Made the injected stubs re-verify the machine at run time (MSXVER for the turbo R, the port `#40` read-back for Panasonic) because the `CMD_PREPARE_QUICK_RUN` path replays a stored option without the detail screen's machine check, and a microSD card may have been configured on a different computer.
- Reworked the boot patch into `apply_boot_patches()` / `build_boot_stub()` / `find_boot_stub_slot()` so the 50/60 Hz and CPU options can be combined. The 12-byte ROM header window (`0x4004`..`0x400F`) only fits one of them, so a combined stub is parked in a run of `0x00`/`0xFF` padding inside the ROM's block 0 and the header holds a 3-byte `jp` trampoline. The search runs backwards and takes the top of the highest qualifying run, so the stub lands in the trailing padding of block 0 whenever there is any. Only the first 8 KB is searched because block 0 is what sits at `0x4000..0x5FFF` at INIT time for every cached mapper — ASCII8 resets all four 8 KB windows to block 0, so a stub placed at image offset `0x2000`..`0x3FFF` would never be reachable. When no run is available the CPU switch is dropped and the 50/60 Hz-only stub is used, which always fits the header.
- Compacted the 50/60 Hz stub from `ld a,r9 / ld b,a / ld c,9` to `ld bc,r9*256+9`, saving 2 bytes of the header budget.
- Persisted the CPU selection in each ROM's `.PVC` options file as a new byte 10 (options file grows from 10 to 11 bytes; older files still load and default the CPU to `Default`). The MSX menu saves it through query-buffer byte 8 and reads it back through the new `CTRL_CPU_MODE` (0xBFA2) register served by the Pico.
- Reduced the MSX menu ROM by moving the ROM detail screen's live state (`record`, `waiting_mapper`, `audio_profile`, `psg_enabled`, `wifi_enabled`, the Wi-Fi capability flag and the cursor position) into file statics and making `render_rom_options_block()` take no arguments. The renderer is called from a dozen key handlers and the 8-argument marshalling alone cost several hundred ROM bytes; the same change dropped the unused `allow_mapper_override` argument.
- Centralised the option row indices in `compute_option_selections()` so the renderer and the key handler no longer recompute the same `freq/cpu/partition/wifi/action` arithmetic independently. `_CODE` now ends at 0xB784, 174 bytes below the v2.44 build and 380 bytes clear of the `0xB900` page-buffer limit.

## PicoVerse 2350 Explorer v2.44

- Bumped Explorer version to v2.44.
- Added last-executed-entry memory for the `F1 - Flash` and `F2 - MicroSD` browsers: launching a ROM or starting an MP3/WAV stores the entry in `/PICOVERSE.PVL` on the root of the microSD browse partition, and the next boot reopens the menu on that source, folder and cursor position instead of always starting at the top of the flash list.
- Added the `CMD_LOAD_LAST_SELECTION` (0x0C) and `CMD_SAVE_LAST_SELECTION` (0x0D) menu commands. The save command records the source plus the full microSD path (or the flash entry name) and skips the write when the stored entry is unchanged; the load command validates the file, reports the source in `CTRL_MAPPER`, restores the microSD folder used by the following `CMD_SET_SOURCE`, and drops back to the flash list when the card, file or entry is gone.
- Made the Pico resolve the saved entry into a list position when the browse list finishes rebuilding, publishing it in `CTRL_MATCH_L/H` and flagging completion with `CTRL_ACK = CTRL_MAGIC`.
- Kept the restore to a single menu build: `refresh_menu_state()` waits on the `CTRL_ACK` completion flag (instead of guessing from a settling record count), applies the saved cursor position before drawing, and renders the page once. The first implementation ran two full refresh passes and three page renders, which made the remembered entry visibly slow to appear at boot.
- Consolidated the MSX menu Pico-command plumbing into the shared `wait_ctrl_cmd()` / `run_ctrl_cmd()` helpers plus a `clear_query_tail()` query-fill helper, replacing the duplicated wait/ack/zero-fill loops in `screen_rom.c` to keep the menu ROM within the `0xB900` code limit (`_CODE` now ends at 0xB83C).
- Restored the File Hunter `Network: Online` / `Network: Offline` status message (`Net: Online` / `Net: Offline` in 40-column mode) on the bottom status line, redrawn with the screen and refreshed every ~3 seconds while browsing.
- Moved the whole ESP-01 link decision to the Pico and exposed the result as a single byte the MSX reads from the new `CTRL_NET_STATUS` register (0xBFA1, 0 = offline, 1 = online), instead of having the MSX menu parse the ESP-01 answer text.
- Made the ESP-01 status check reliable: the UART is drained until quiet before the `g` (get AP status) query is sent, the reply reader resynchronizes on the reply tag and validates the whole frame (so bytes left over from an earlier transfer no longer look like a missing module), and failed attempts are retried after the 250 ms ESP-01 receive-parser timeout.
- Fixed the root cause of the old false `Offline` reports: the link is now considered up whenever the module reports `STATION_GOT_IP`, regardless of whether the firmware returns the SSID string, and a `STATION_CONNECTING` answer is polled for up to 3 seconds instead of being treated as offline.
- Cached the link state on the Pico with a 3-second re-probe interval while a module answers and a 30-second back-off (single short attempt) when no module is fitted, so the MSX can poll the indicator without stalling; a completed HTTP transfer marks the link online for 10 seconds and outranks a status query the busy module could not answer.
- Applied the same relaxed connection rule to the pre-transfer Wi-Fi check used by the catalog fetch, ROM downloads and the Nexus tracker check-in, which previously refused to start when the ESP-01 reported an empty SSID.

## PicoVerse 2350 Explorer v2.43

- Bumped Explorer version to v2.43.
- Fixed banked mapper ROMs hanging when the game switches segments in bursts from MSX RAM without reading the cartridge in between. The bus loops blocked waiting for a read and let the 8-entry write FIFO overflow, silently dropping bank switches and leaving the mapper on a stale segment. All banked loops (Konami/Konami-SCC/ASCII8, ASCII16, ASCII16-X, Neo-8, Neo-16) now drain writes while waiting for the next read. Reported with "Go Figure v1.2"; same fix as PicoVerse 2350 Loadrom v2.70.
- Fixed ASCII16-X being too slow to keep up with tight VDP transfer loops ("VDP too slow"). Explorer's ROM cache lives in PSRAM, so every cartridge read cost a QMI transaction while `/WAIT` was asserted. ASCII16-X now serves reads from real internal SRAM (8 x 16KB banks), borrowing the OPLL table that is idle unless MSX-MUSIC is running; with MSX-MUSIC active it falls back to the PSRAM cache. Firmware RAM usage is unchanged.
- Trimmed the mapper bus loops so the read response is not delayed: WaveGame I/O servicing was removed from the mappers that cannot use it (ASCII16-X, Neo-8, Neo-16, Konami SCC with SCC audio).
- Added the `YM2164 SFG05 4MHZ` and `YM2151 SFG01 4MHZ` audio profiles (profile ids 11 and 12), which reuse the existing SFG surface, BIOS image and slot layout but clock the emulated OPM/OPP core at 4 MHz so pitch, envelopes, LFO and FM timers run 1.1174x faster, matching the arcade boards most YM2151 music was written for.
- Documented the two new profiles in the Explorer tool manual audio-profile section.

## PicoVerse 2350 Explorer v2.42

- Bumped Explorer version to v2.42.
- Reorganized `pico/explorer` sources into per-type subfolders: `audio/` (`emu2149`, `emu2151` + its `ymfm/` library, `emu2212`, `emu2413`, `mp3`), `memory/` (`c2_emu`, the Carnivore2-style mapper/RAM emulation), and `storage/` (`hw_config.c`, `sunrise_ide`, `sunrise_sd`), updating `CMakeLists.txt` and `explorer.c` includes accordingly. Verified with a full reconfigure and clean rebuild.
- Deleted a ROM's associated `.PVC` options file when the ROM is deleted from microSD, while continuing normally when no options file exists.
- Stamped FileHunter ROM downloads with the HTTP `Last-Modified` time, falling back to the response `Date`, instead of leaving a null microSD file timestamp.
- Fixed FileHunter showing `Network: Offline` after successfully loading its online catalog by retaining successful HTTP connectivity when the ESP8266 AP-status query cannot return an SSID.
- Removed the FileHunter `Network: Online`/`Network: Offline` footer and its redundant Wi-Fi status probes from the MSX menu to reduce ROM usage.
- Updated the public README per-ROM 50/60 Hz credit paragraph to also acknowledge the Carnivore2 (RBSC) boot-menu strategy used as a reference for the BIOS `WRTVDP` / `RG9SAV` application technique and the MSX2+ restriction.

## PicoVerse 2350 Explorer v2.41

- Bumped Explorer version to v2.41.
- Changed the per-ROM 50/60Hz INIT stub to call the BIOS `WRTVDP` routine (0x0047) instead of a raw port 0x99 VDP register write, following the Carnivore2 boot-menu pattern. On MSX2 `WRTVDP` updates both VDP register 9 and its BIOS shadow `RG9SAV` (0xFFE8), so games that reload R9 from the shadow via a BIOS screen call keep the requested refresh mode (better compatibility).
- Restricted the `Frequency` option to MSX2/MSX2+ machines: VDP R9 only exists on the V9938/V9958, so the option is hidden on MSX1 (main-ROM version byte 0x002D = 0) and the launch forces the applied value to `Default` there, preventing a stray R9 write from corrupting the TMS9918 registers.
- Updated the `docs/msx-picoverse-2350-50-60hz.md` implementation document for the `WRTVDP`-based stub and the MSX2+ restriction, and credited the Carnivore2 (RBSC) boot menu as the reference for that technique.

## PicoVerse 2350 Explorer v2.40

- Bumped Explorer version to v2.40.
- Updated the public README Softwaredb attribution to mention that the latest database updates are based on Vampier's work and to add the romdb.vampier.net reference link.
- Added a per-ROM VDP `Frequency` option (Default / 60Hz / 50Hz) to the ROM detail screen for every launchable ROM entry, selectable with LEFT/RIGHT.
- Persisted the frequency selection in each ROM's `.PVC` options file as a new byte 9 (options file grows from 9 to 10 bytes; older 9-byte files still load and default the frequency to `Default`). The MSX menu saves it through query-buffer byte 7 and reads it back through the new `CTRL_VDP_FREQ` (0xBFA0) register served by the Pico.
- Applied the selected frequency after the launch reset by having the Pico patch the launched game's cartridge INIT: an 11-byte stub injected into the cached ROM header writes VDP register 9 directly through port 0x99 (bit 1 = 50/60Hz, other bits = the standard 192-line text default), then jumps to the game's original INIT. Because the stub runs as the cartridge INIT (after the BIOS boot re-initializes the VDP), the requested refresh mode is active when the game starts. The patch is applied only to regular game ROMs and is disabled for the Nextor/Sunrise/C2/MegaRAM system ROMs (which manage their own boot), fixing a boot loop when selecting 50/60Hz on those entries.
- Hid the `Frequency` option in the ROM detail screen for system ROMs (Nextor/Sunrise/C2/MegaRAM), since the VDP frequency patch does not apply to them; the option is only shown for regular game ROMs.
- Simplified the ROM detail footer to a single generic `[ESC - BACK] [LEFT/RIGHT - CHANGE]` hint (compact `[ESC-BACK] [L/R-CHANGE]` in 40-column mode), removing the per-option footer strings to recover MSX menu ROM space.
- Added the `docs/msx-picoverse-2350-50-60hz.md` implementation document and a public README credit paragraph acknowledging the `50-60hz` project by sdsnatcher73 (Apache-2.0; with help from gdx, Grauw, and NYRIKKI) used as the VDP frequency reference.

## PicoVerse 2350 Explorer v2.39

- Bumped Explorer version to v2.39.
- Added a dedicated PicoVerse 2350 microSD build guide covering large-card partitioning, Nextor 4GB-per-partition limits, Explorer partition cycling with `P`, and the planned Nextor starter-files download link.
- Expanded the PicoVerse 2350 microSD guide with step-by-step Windows partitioning procedures for both DiskPart (CLI) and Disk Management (GUI).
- Added Explorer `-r`/`--megaram` to create a standalone `Brazilian MegaRAM (1MB)` SYSTEM entry without Nextor or the 1MB MSX memory mapper, allowing the MSX to boot to BIOS while the Cartucho II-compatible MegaRAM surface remains available.
- Updated the Explorer MSX menu titles for the Nextor + Mapper + MegaRAM entries to say `1MB MegaRAM`.
- Added MegaRAM-only `MegaRAM SCC` and `MegaRAM SCC+` audio profiles that keep the MegaRAM surface in-place while intercepting SCC register accesses and routing SCC audio to the Pico DAC.
- Saved standalone `Brazilian MegaRAM (1MB)` ROM detail options to the root display-name `.flash.PVC` file so MegaRAM SCC/SCC+ profile selections persist without a backing ROM file.
- Reduced the MSX Explorer menu ROM size by consolidating ROM/MP3/File Hunter detail-frame rendering and trimming duplicated footer/status formatting, increasing the `_CODE` margin below the `0xB900` Pico communication window.
- Fixed the MSX help screen Chip ID after visiting File Hunter by prioritizing the Pico Chip ID subrange over the broader File Hunter status-text read window.
- Restored WAVEGAME PSG Mirror output by draining queued PSG register writes from the MP3/WAV audio callback before mixing mirrored PSG samples (regression bug).

## PicoVerse 2350 Explorer v2.38

- Bumped Explorer version to v2.38.
- Added Explorer `-a`/`--allnextor` to include every embedded Nextor system option in one build while still scanning and appending ROMs from the current folder.
- Added Explorer `-r1`/`--megaram-sd` and `-r2`/`--megaram-usb` Nextor system options with the existing Sunrise IDE ROM, a 1MB MSX memory mapper, and a separate 1MB PSRAM-backed MegaRAM surface using the Cartucho II-compatible latch/write-enable behavior from LoadROM.
- Fixed the MSX menu mapper label for MegaRAM Nextor entries so mapper IDs 19/20 display as `SYSTEM (detected)` instead of `Unknown (detected)`.
- Applied the same mapper-backed MSX menu restrictions to MegaRAM entries: PSG Mirror is disabled, MSX-MUSIC is unavailable, Wi-Fi is not exposed, and unsupported saved external-audio selections are sanitized back to the base MegaRAM launch path.
- Updated the Explorer manual and related public documentation for the new Explorer Nextor options and `-a`/`--allnextor` shortcut.

## PicoVerse 2350 Explorer v2.37

- Bumped Explorer version to v2.37.
- Kept the MSX menu function-key shortcuts disabled during the initial flash-source load, then cleared the key buffer and enabled F1/F2/F3 only once the menu is ready for commands.
- Added Carnivore2 SD/USB Nextor system options to Explorer, including tool config generation, MSX menu policy, Pico firmware launch dispatch, 1MB PSRAM mapper RAM, C2 RAM-mode emulation, and Explorer-compatible external SCC/SCC+ and YM2151/SFG audio surfaces while keeping Wi-Fi unavailable for C2 to match LoadROM's current policy.
- Fixed MSX menu mapper decoding so Carnivore2 SYSTEM mapper codes 17/18 are shown as SYSTEM and sorted with the other Flash SYSTEM entries instead of being mistaken for manual PLA-16/PLA-32 overrides.
- Fixed Pico-side mapper decoding and saved manual mapper values so mapper codes 16/17/18 are preserved, keeping C2 records in the Flash SYSTEM group and dispatching C2 launches through the Carnivore2 Nextor loader instead of the PLA-16/PLA-32 paths.

## PicoVerse 2350 Explorer v2.36

- Bumped Explorer version to v2.36.
- Fixed Nextor + 1MB mapper instability and random errors that appeared only when PSG Mirror was enabled. 
- Extended the same lock-free hand-off to every Sunrise mapper audio profile so no audio chip write can stall core0 and drop mapper segment-register writes. 
- Disabled the MSX-MUSIC (YM2413/FM-PAC) audio profile for Sunrise Nextor ROMs that add the 1MB mapper (the `+ 1MB Mapper` USB/SD options), keeping it available only for the non-mapper Sunrise Nextor options and regular game ROMs. MSX-MUSIC with the mapper is pushing the limit of the core coordination and need more research to make it stable. The other Sunrise mapper audio profiles (Dual PSG, YM2151/SFG01, YM2151/SFG05, SCC, SCC+) remain available for the Sunrise + 1MB mapper options. This option will be re-enabled in a future Explorer release once the core coordination is improved to avoid dropped mapper writes and random errors.
- Disabled the PSG Mirror option for Sunrise Nextor ROMs that add the 1MB mapper.

## PicoVerse 2350 Explorer v2.35

- Bumped Explorer version to v2.35.
- Refreshed the generated ROM mapper SHA1 databases used by the Explorer tool and Pico firmware from the current openMSX `softwaredb.xml`.
- Fixed the Kikikaikai - Mystery - TAITO (ASCII8) hang under the YM2151 (SFG05/SFG01) audio profiles (the earlier v2.34 ASCII8 banking patch did not address the real cause). The combined SFG bus loops (`loadrom_external_sfg` and `loadrom_sunrise_sfg_common`) only drained the captured-write FIFO at the top of the loop, then serviced a read without re-draining. A bank-switch or `ENASLT` secondary-slot (`0xFFFF`) write landing between that drain and the read dequeue was applied only after the read, so the read was answered with a stale subslot/bank and returned a corrupt byte. The game's music driver remaps the SFG into page 2 via `ENASLT` every frame, so this race eventually corrupted a read and froze the MSX after some time. Both SFG loops now re-drain pending writes immediately after dequeuing the read address (matching the proven `banked8_loop` ordering), so the response always reflects the latest subslot/bank state.
- Improved speed when drawing of the ROM/MP3/WAV list by rendering each menu row with a single VRAM block write.
- Added MP3/WAV folder playback controls with Single, All, and Random modes, plus guarded Next/Previous music navigation.
- Improved the MP3/WAV detail screen so it keeps the current track information and controls aligned while playback changes.
- Fixed microSD folder navigation after returning from subfolders, keeping the page and cursor state consistent.
- Improved WAVEGAME audio startup/restart reliability and reduced MSX menu ROM size so the menu stays below the Pico page-buffer window.
- Fixed WAVEGAME ROMs launching with no audio after an MP3/WAV had been played first.
- Fixed File Hunter downloads being rejected with "audio busy" after MP3/WAV playback.
- Fixed the MSX freezing (and the audio dying permanently) when entering File Hunter after playing an MP3/WAV. 
- Reordered the MP3/WAV detail screen options so the playback Mode is the last option and Action: Play is selected by default.
- Added the Pico unique Chip ID to the MSX help screen and moved the return prompt to the row below it.
- Shortened the selected-row inverted highlight by two columns on the right to align it with the menu frame.
- Added Sunrise Nextor SD partition selection for FAT16 microSD partitions up to 4GB, persisted in each ROM's `.PVC` options file.
- Changed Sunrise Nextor SD partition selection to show Pico-supplied partition labels/messages without detail-screen footer text, and added F2 microSD partition cycling across FAT16, FAT32, and exFAT partitions with `/PICOVERSE.PVC` persistence.
- Fixed F2 microSD partition cycling so it can move through supported primary/logical partitions and keeps F1/F3 source switching responsive after a partition change.
- Fixed the empty F2 microSD partition screen so `P` can cycle back to another partition and `1`/`2` source shortcuts still work.
- Changed the ROM detail screen to show only the selected-option helper footer, restored the SD Part helper text, and show the actual compatible FAT16 partition label instead of `SINGLE PARTITION`.
- Show the selected F2 microSD partition label in the status area whenever no transient status message is active.
- Fixed FAT16/FAT32/exFAT partition label detection so F2 and Sunrise SD partition names use the filesystem volume label instead of boot-sector defaults or generic partition names.
- Fixed a regression where scanning a filesystem label could overwrite the MBR/EBR sector buffer and hide later microSD partitions from the F2 partition cycle.
- Added free-space MB reporting to the F2 microSD partition status label, including the first time F2 selects the microSD source.
- Changed the ROM detail and quick-run defaults so PSG Mirror starts enabled unless saved `.PVC` options override it.
- Fixed F3 File Hunter launching from an empty F2 microSD partition screen.
- Fixed blinking status messages so the hidden phase never substitutes the F2 partition buffer and cuts the start of File Hunter network text.
- Added a confirmed `D` delete command for selected files on the F2 microSD screen while leaving folders protected.
- Changed the 40-column F2 microSD status to alternate between the partition label and free-space amount instead of truncating the free-space text.
- Improved help screen with additional `D` and `P` commands.
- Updated Explorer, feature, public README, and Sunrise Nextor documentation for the new `P`/`D` commands, multi-partition microSD browsing, and FAT16 up-to-4GB Nextor partition requirements.
- Fixed Sunrise Nextor SD partition detection for exact 4GB FAT16 partitions and show `PARTITION1` fallback text when the filesystem has no user volume label.
- Added a per-ROM audio volume control to the ROM detail screen, persisted it in `.PVC` options files, and raised the YM2151 SFG01/SFG05 baseline output to better match other audio profiles.
- Fixed MP3/WAV Random mode so pressing `N` or `P` while playback is active chooses a random track instead of stepping through the folder list.
- Fixed MP3/WAV and WAVEGAME audio becoming silent after ROM detail SD work by quiescing the lazy MP3 core before option load/save and mapper detection, preserving the I2S handoff pool for the next audio launch.

## PicoVerse 2350 Explorer v2.34

- Bumped Explorer to v2.34.
- Fixed long microSD folder names by sending the selected record index to the Pico instead of truncating through the query buffer.
- Changed the memory-read and ROM-load `/WAIT` release paths to open-drain behaviour.
- Added microSD WAV discovery and playback through the existing MP3 detail screen controls.
- Added WAVEGAME runtime support for microSD game ROMs via MSX I/O port `0x92` (stop/pause/start/loop, fade-out/in, play-once/loop, `pause.wav`, `multi.wav`, deferred commands, per-song cfg offsets), with looped WAV playback in the shared MP3/WAV core.
- Fixed non-standard ASCII8 WaveGame ROMs (e.g. the 33-block Outrun ROM) that showed the intro but black-screened into the game, by resetting all ASCII8 banks to block 0 (matching openMSX) and wrapping out-of-range banks for non power-of-two images. 
- The non-standard ASCII8 patch also fixed the Kikikaikai - Mystery - TAITO hanging when using SFG-01/SFG-05 reported issue (#19) by ensuring the music data is correctly banked in the expected location.
- Added combined WAVEGAME + PSG Mirror support and WAVEGAME sidecar auto-detection that forces the ROM audio profile to None while keeping PSG Mirror selectable.
- Updated WAVEGAME and public README documentation.

## PicoVerse 2350 Explorer v2.33

- Bumped Explorer to v2.33.
- Improved MSX-MUSIC output with a soft-knee limiter, DC blocking, and light low-pass filtering so dense FM arrangements avoid hard clipping while normal-level material remains linear.
- Refined the ROM detail workflow with chip-style audio profile labels, aligned option text, ENTER-to-detail behavior, SPACE quick-run using saved/default `.PVC` options, and mapper detection before launch when needed.
- Added external SCC/SCC+ profiles for non-SYSTEM ROMs, keeping the game mapper in expanded subslot 0 and exposing the virtual SCC/SCC+ surface in subslot 1.
- Enabled external SCC/SCC+ profiles for Sunrise Nextor SYSTEM ROMs by exposing the virtual SCC/SCC+ cartridge in a free expanded subslot while keeping Nextor storage, optional WiFi, and optional mapper RAM in their existing subslots.
- Fixed Sunrise Nextor external SCC/SCC+ startup by guarding SCC audio servicing until the I2S audio pool is fully initialized, avoiding an early core1 storage-backend fault during boot.
- Added YM2151 (SFG05) and YM2151 (SFG01) profiles for non-SYSTEM ROMs, keeping the game mapper in subslot 0 and exposing an SFG-like YM2151 register surface plus the selected 32K Yamaha SFG BIOS image in subslot 1.
- Enabled YM2151 (SFG05/SFG01) profiles for Sunrise Nextor SYSTEM ROMs by exposing the virtual SFG cartridge in a free expanded subslot while keeping mapper RAM available for Nextor.
- Fixed the YM2151 (SFG05/SFG01) profiles so the plain "Nextor Sunrise IDE (SD/USB)" options no longer expose the 1MB mapper RAM (only the explicit "+ 1MB Mapper" options do), while still exposing the SFG cartridge subslot; gated the mapper RAM region, mapper memory subslot, and mapper I/O ports (0xFC-0xFF) behind a new `mapper_enable` flag in `loadrom_sunrise_sfg_common`.
- Renamed the MSX menu's SFG05 audio profile to YM2164 (SFG05) and route SFG05 audio through YM2164/OPP test-register and Timer B behavior while leaving SFG01 as YM2151/OPM.
- Exposed the FM-PAC BIOS for Sunrise Nextor SYSTEM ROMs when MSX-MUSIC is selected by launching a Sunrise + FM-PAC expanded-slot layout while keeping Nextor storage, optional WiFi, mapper RAM, and YM2413 audio servicing active.
- Disabled the optional PSRAM `0xC0` post-QPI initialization command after LY68L6400S compatibility issues were reported by Ludovic Avot.
- Matched the RBSC-style SFG top-128-byte control aperture outside the BIOS ROM window and mixed primary PSG mirror audio only when the mirrored PSG has audible channels.
- Consolidated the bundled Nextor and SFG ROM payloads under `resources/` and updated the Explorer tool to embed them directly from that shared project folder.

## PicoVerse 2350 Explorer v2.32

- Bumped Explorer to v2.32.
- Reworked MSX-MUSIC plus PSG mixing to follow the openMSX mixer model: keep FM and PSG as independent sources, apply per-device gain, accumulate them, and run DC removal on the final mixed output.
- Removed the previous PSG-only DC filter and fixed 3:1 weighted blend that could make active PSG writes corrupt MSX-MUSIC playback.
- Separated MSX-MUSIC and PSG output completely when both are enabled, routing FM and PSG to independent stereo channels instead of mixing their samples together.
- Reduced MSX-MUSIC plus PSG runtime contention by using the fast PSG renderer only when primary PSG mirroring is paired with MSX-MUSIC and by servicing I/O between FM and PSG sample generation.

## PicoVerse 2350 Explorer v2.31

- Bumped Explorer to v2.31.
- Mixed primary PSG mirroring into MSX-MUSIC with lower gain and DC filtering so MSX-MUSIC ROMs can enable PSG emulation without FM distortion.
- Gated the MSX-MUSIC PSG mix on audible PSG channel state so silent PSG emulation no longer changes the MSX-MUSIC output.
- Mixed active PSG with MSX-MUSIC using reserved headroom so combined PSG/FM playback avoids clipping artifacts.
- Allowed the MSX-MUSIC audio profile to be selected and launched with every non-SYSTEM mapper.
- Enabled MSX-MUSIC selection and SYSTEM audio servicing for Sunrise Nextor SYSTEM ROMs.
- Kept Sunrise Nextor SYSTEM ROMs on the Sunrise loader when MSX-MUSIC is selected instead of routing them through the FM-PAC ROM launcher.
- Added KonamiSCC mapper handling to the MSX-MUSIC/FM-PAC launcher so SCC mapper ROMs still boot when MSX-MUSIC is selected.

## PicoVerse 2350 Explorer v2.30

- Bumped Explorer to v2.30.
- Enabled the primary PSG DAC mirror for Sunrise SYSTEM ROM launches through a reusable SYSTEM audio service, including Sunrise + Mapper modes without stealing mapper I/O or blocking mapper bootstrap.
- Enabled the Dual PSG audio profile for Sunrise SYSTEM ROMs through the SYSTEM audio service.
- Split Dual PSG and primary PSG mirror output into separate stereo channels when both are enabled.
- Kept Sunrise SYSTEM PSG I/O serviced while producing audio buffers so non-mapper Sunrise launches remain stable with Dual PSG plus PSG mirror enabled.

## PicoVerse 2350 Explorer v2.29

- Bumped Explorer to v2.29.
- Applied each decoded MP3 frame's sample rate to the I2S output clock so MP3 files encoded at rates other than 44.1 kHz play at the correct pitch and speed.
- Retuned the existing MP3 I2S producer format in place on sample-rate changes instead of allocating a second audio pool during playback.
- Expanded Explorer ROM/MP3 record names to the 80-column detail-screen width and only truncate detail-screen names when they exceed the active screen width.
- Reported a missing microSD card to the MSX menu, display a clear no-card message when switching to the microSD source without a card inserted, and avoid reading stale records while keeping the MSX ROM code below the Pico communication window.
- Enabled SDCC code-size optimization for the MSX menu build so the ROM code stays clear of the `0xB900` Pico page-buffer window.
- Show the missing-microSD state as a single menu entry so the user can still switch to Flash or File Hunter from the microSD page.
- Reduced the MSX menu ROM code size by reusing shared row rendering, status text selection, last-line blinking, and source-switch helpers.
- Block File Hunter offline/status message rows from opening the detail screen and fail File Hunter network checks quickly when the ESP8266 does not respond.
- Align the missing-microSD row with File Hunter status rows so it is visible but not selected, scrolled, or actionable.
- Shorten 40-column missing-microSD and File Hunter offline messages so they fit without truncation.
- Refresh status-row text after column toggles so 80-column missing-microSD/File Hunter messages are not reused in 40-column mode.
- Bound each packed Explorer page name so all 19 microSD rows receive a visible name when long filenames fill the page-buffer string pool.
- Persist each ROM detail screen's audio profile and primary PSG setting to a `.PVC` file on microSD, reusing it on later launches and keeping those files out of the menu listing.
- Added the selected mapper to persisted `.PVC` ROM settings while keeping existing audio/PSG option files readable.
- Updated File Hunter catalog requests to use `http://msxpico.file-hunter.com/picoverse.php` while preserving the packed-list `base=1BA0` ROM query parameters required by the Explorer parser.
- Expanded File Hunter page records and detail rendering so 80-column ROM detail screens can show names up to the shared 71-character limit.


## PicoVerse 2350 Explorer v2.28

- Bumped Explorer to v2.28.
- Added microSD MP3 discovery, MP3 list/detail UI, play counter/status display, full filename rendering, and Play/Stop plus Pause/Resume controls backed by the Pico MP3 decoder service.
- Kept MP3 startup lazy so idle Stop/Pause/Resume commands do not start Core 1 during Explorer startup or normal flash browsing.
- Moved MP3 stream buffering to a dedicated 64 KB PSRAM allocation and reduced the SRAM fallback buffer to 8 KB, freeing SRAM for MSX-MUSIC initialization after MP3 playback.
- Kept the MP3/I2S backend on PIO1 SM2 and preserved the live MSX bus PIO programs when selecting, stopping, or leaving MP3 playback.
- Blocked File Hunter downloads above the 4 MB microSD/PSRAM launch limit used for SD-loaded ROMs.
- Kept the I2S DAC mute line asserted while Explorer is idle, only unmuting it after MP3 playback or a ROM audio profile starts.
- Added a ROM detail PSG option that mirrors primary PSG writes to the Pico DAC and mixes with existing ROM audio profiles.
- Updated Explorer, public, feature, Dual PSG, and PIO documentation for the MP3 player and primary PSG DAC mirroring behavior.
- Reused the MP3 I2S pipeline for ROM audio profiles after MP3 playback, resetting Core 1 before ROM launch and forcing the Pico audio singleton through disable/enable so SCC and MSX-MUSIC DMA output can restart cleanly.
- Moved MSX-MUSIC/FM-PAC I/O bus responders to PIO2, keeping FM-PAC ports isolated from the PIO1 I2S audio backend used by MP3 and ROM audio.
- Started MSX-MUSIC audio output only after the final FM-PAC ROM bus initialization releases `/WAIT`.
- Added temporary USB CDC diagnostics, with TinyUSB host disabled and a CDC-only PicoVerse debug product descriptor, to capture MP3-to-ROM audio launch checkpoints without using UART.

## PicoVerse 2350 Explorer v2.27

- Bumped Explorer to v2.27.
- Raised the MSX-MUSIC/FM-PAC audio output gain to match the existing SCC, SCC+, and Dual PSG I2S volume boost while preserving sample clipping protection.

## PicoVerse 2350 Explorer v2.26

- Bumped Explorer to v2.26.
- Added an MSX-MUSIC audio profile to the MSX Explorer ROM detail screen for non-system, non-SCC-class ROMs.
- Ported the LoadROM YM2413/emu2413 audio engine into Explorer and added runtime FM-PAC expanded-slot handling that reads the bundled FM-PAC BIOS from the Explorer UF2 flash payload.
- Moved the Explorer ROM cache from RP2350 SRAM to PSRAM so MSX-MUSIC can coexist with the existing Explorer features without overflowing firmware RAM.
- Updated the Explorer user documentation for MSX-MUSIC profile selection, mapper restrictions, and flash-payload FM-PAC BIOS storage.
- Allowed SPACE as well as ENTER to execute a ROM from the ROM detail screen when `Action: Run` is selected.
- Changed the File Hunter ROM download flow to show `Downloading...` instead of the generic network status check message.
- Removed the WiFi setup hint and F4 special handling from the help page return prompt.
- Increased the Explorer microSD ROM size limit and PSRAM SD ROM buffer from 2 MB to 4 MB.

## PicoVerse 2350 Explorer v2.25

- Bumped Explorer to v2.25.
- Reworked ROM audio selection around named, mutually exclusive audio profiles so new audio chips can be added without conflicting with SCC or system ROM options.
- Added a Dual PSG audio option for non-system, non-Konami SCC/Manbow2 ROMs, using the same secondary PSG port model as LoadROM.
- Updated the Explorer and PicoVerse 2350 documentation to describe Dual PSG support, audio profile exclusivity, and mapper restrictions.
- Added the shared PicoVerse 2350 Dual PSG implementation reference covering Explorer and LoadROM behavior.
- Updated the public README with user-facing Explorer instructions for selecting the Dual PSG audio profile on supported ROMs.
