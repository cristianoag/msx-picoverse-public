# Change Log

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
