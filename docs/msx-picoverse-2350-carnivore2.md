# MSX PicoVerse 2350 — Carnivore2 Implementation and Tool Guide

The Carnivore2 firmware turns the PicoVerse 2350 into a software-compatible **RBSC Carnivore2** multifunction cartridge. It is a dedicated firmware: it does not include the Explorer menu, and it does not use the LoadROM/Explorer ROM database. The Carnivore2 boot menu, Nextor IDE BIOS and FM-PAC BIOS that run on the cartridge are the original RBSC ones, loaded from a Carnivore2 FlashROM image, so the original utilities (`C2MAN`, `C2RAMLDR`, `C2BACKUP`, `C2CFGBCK`, `C2SRAM`, `C2IDETST`, `C2FINDER`) work unchanged.

## What is emulated

| Carnivore2 function | PicoVerse implementation |
|---|---|
| FlashROM (M29W640GB, 8 MB, bottom boot) | Image in RP2350 flash (read through XIP), command set modelled in `core/c2_flash.c`: read/reset, autoselect, CFI, byte program, double/quad byte program, unlock bypass, block and chip erase, and `RP#` reset through `ConfFl`. `VPP/WP#` does not protect the boot blocks, because C2MAN rewrites them with `ConfFl` at its power-on value |
| RAM chip (2 MB) | PSRAM: 1 MB MegaRAM / shadow RAM at `0x000000`, 1 MB memory mapper at `0x100000`, 8 KB FM-PAC SRAM at `0x0FE000` (same addresses as the original chip) |
| Expanded slot | Four sub-slots selected by `Mconf`/`SLM_cfg`: main (bank windows), IDE, mapper, FM-PAC. Slot register at `0xFFFF` |
| Configuration registers | The 64-byte register window at `0x0F80/0x4F80/0x8F80/0xCF80`, bank windows R1–R4 with page registers, delayed reconfiguration (`CardMDR[3:2]`), direct vector port `DatM0`, `Mconf`, `NSReg`, `SLM_cfg`, `PFXN`, `PsgAlt` |
| Port `#F0` protocol | `C`, `S`, `R`, `H`, `0`–`3`, `A`, `M` commands, relocatable with `PFXN` |
| Memory mapper | 1 MB, ports `FC–FF` (read gated by `Mconf[6]`), page protection and memory-mapped registers through port `3C` |
| IDE / Nextor | Sunrise IDE window at `0x7C00–0x7EFF`, control register at `0x4104`, Nextor BIOS from the FlashROM at `0x010000`, **microSD card instead of a CF card** |
| FM-PAC | BIOS from the FlashROM at `0x030000` (page register `0x7FF7`), SRAM unlock keys, `0x7FF4/0x7FF5` and I/O `0x7C/0x7D`, YM2413 (emu2413), stereo or mono through `CardMDR+0x22` |
| SCC / SCC+ | Bank/mode registers, wave and register windows decoded as in the FPGA (`0x9800`/`0xB800`), NSC SCC+ bank/RAM modes, emu2212 |
| PSG | Port `A0/A1` (or `10/11` with `PsgAlt`) mirror, mixed when `CardMDR+0x24[7]` is set |
| PPI beeper | Port `AA/AB` bit, mixed when `CardMDR+0x24[6]` is set |
| Configuration EEPROM | 128-byte Microwire EEPROM behind `CardMDR+0x23`, persisted in the last 4 KB of the RP2350 flash |
| Slot detection | Port `#F0` `S` reports the primary slot, tracked from the writes to port `A8` |

## Memory and flash layout

```
RP2350 flash (16 MB)                         PSRAM (8 MB)
0x000000  firmware (< 4 MB)                  0x000000  Carnivore2 RAM chip (2 MB)
0x400000  Carnivore2 FlashROM image (8 MB)                 0x000000  MegaRAM / shadow RAM (1 MB)
0xBFFFFF                                                   0x100000  memory mapper (1 MB)
0xFFF000  configuration EEPROM (4 KB)        0x200000  FlashROM write-back cache (6 MB = 1536 sectors)
```

### FlashROM SRAM read cache

Reads of the FlashROM image go through a 192 KB SRAM cache (512-byte blocks, direct mapped) that is filled from the flash the first time a block is read, so the ROM that is running executes from on-chip SRAM instead of the QSPI flash. Copying the ROM to PSRAM would not help: the PSRAM is behind the same QMI/XIP cache and runs slower than the flash. Change `C2S_CACHE_KB` if the SRAM budget needs it (the build leaves roughly 60 KB of heap free at 192 KB). The RP2350 runs at 300 MHz (the OPL4 clock and QMI timings) to keep the time the cartridge holds `/WAIT` short; build with `-DC2_SYS_CLOCK_KHZ=210000` to return to 210 MHz.

### FlashROM write-back cache

Program and erase cycles from the MSX complete instantly, as on the real chip: they are applied to 4 KB sector copies held in PSRAM and reads see the modified data at once. The dirty sectors are committed to the RP2350 flash when the MSX has been quiet for 200 ms (no FlashROM command, EEPROM access or memory mapper port write), one sector at a time with 25 ms between sectors. A commit erases only the sectors where bits must go from 0 to 1 and writes only the pages that changed. Erases are instant too: an erased sector is only marked as blank (reads return `0xFF`) and its flash erase is done later by the same background commit, so a block or chip erase does not stall the MSX. If the cache fills up the oldest sector is committed immediately.

Uploaded ROMs and settings therefore survive power cycles, but a sector that has not been committed yet is lost if power is removed or the MSX is reset during the first fraction of a second after the write. While a sector is committed the bus is stalled for tens of milliseconds, the audio can click, and the MSX can miss writes to the cartridge during that time; this is why the commit only runs when the MSX is quiet.

## Building

```
cd 2350/software/carnivore2.pio
make          # firmware (CMake/Ninja, Pico SDK 2.3.1) and the PC tool
make test     # host tests of the Carnivore2 core (needs a host C compiler)
```

The tool is written to `tool/dist/carnivore2.exe`; the firmware image is `pico/carnivore2/dist/carnivore2.bin`.

## Creating the UF2

```
carnivore2.exe -f Carnivore2.rom -o carnivore2.uf2
```

| Option | Description |
|---|---|
| `-f, --flash <file>` | Carnivore2 FlashROM image (up to 8 MB, padded with `0xFF`) written at flash offset 4 MB. Use `Carnivore2.rom` from the RBSC repository (`OpenMSX/Carnivore2.rom`) or a `C2BACKUP` dump of a real cartridge. The image is not distributed with PicoVerse. |
| `-o, --output <file>` | Output file (default `carnivore2.uf2`) |

### Getting a FlashROM image for `-f`

Any of these gives a file the tool accepts:

1. **Factory image (simplest).** Use `OpenMSX/Carnivore2.rom` from the RBSC repository ([github.com/RBSC/Carnivore2](https://github.com/RBSC/Carnivore2)). It is the full 8 MB image of a new cartridge.
2. **Build it from the RBSC component files.** `tool/make-flash-image.ps1` assembles the same layout as the factory image from three files of the repository:

   ```
   powershell -File tool\make-flash-image.ps1 `
       -BootMenu BootMenu\BOOTCMFC.BIN -IdeBios BIOSes\BIDECMFC.BIN -FmPacBios BIOSes\FMPCCMFC.BIN `
       -Output flash.rom
   carnivore2.exe -f flash.rom -o carnivore2.uf2
   ```

   The boot menu is stored as four 8 KB pieces at `0x0000`, `0x2000`, `0xC000` and `0xE000`, the IDE BIOS at `0x010000`, the FM-PAC BIOS at `0x030000` and the default configuration record at `0x4000`; the rest is `0xFF`. The result is identical to the factory image except for one byte of the FM-PAC BIOS (`0x03FFED`).
3. **Back up a working cartridge.** On the PicoVerse (or a real Carnivore2) run `C2BACKUP` from MSX-DOS and save the FlashROM to a file on the SD card; copy the file to the PC and use it as `-f`. This keeps the uploaded ROMs and the ROM directory.
4. **Update an existing cartridge without a new image.** Create the UF2 without `-f` and update the boot menu and BIOSes from MSX-DOS with `C2MAN` (`9`, `4` writes `BOOTCMFC.BIN`).

Without `-f` the UF2 contains only the firmware and keeps the FlashROM already stored in the cartridge, so firmware updates do not erase uploaded ROMs and settings.

To install it, hold BOOTSEL while connecting the cartridge to the PC and copy the UF2 to the `RP2350` drive. The full UF2 is about 16 MB and takes a little while to copy.

## Using it

1. Insert a FAT-formatted microSD card with a Nextor partition layout (the same cards used by the other PicoVerse Sunrise/Nextor builds). The card replaces the CF card of the original cartridge.
2. Power the MSX. The cartridge answers a small restart ROM while it initialises, so the MSX restarts once (the original Carnivore2 also reboots twice after a cold start). The Carnivore2 boot menu appears afterwards.
3. Connect headphones or an amplifier to the PicoVerse I2S DAC output for SCC/SCC+, FM-PAC, PSG and beeper sound. Volumes follow the boot menu settings (`CardMDR+0x22/0x24`).

The RP2350 is reset together with the MSX (the cartridge `/RESET` line is connected to the RP2350 `RUN` pin), so a hardware reset returns all Carnivore2 registers to their power-on values, as the FPGA does.

## Architecture

| File | Role |
|---|---|
| `pico/carnivore2/core/c2_core.[ch]` | Portable model of `mcscc.vhd`: slot/sub-slot decode, registers, bank windows, mapper, IDE/FM-PAC/SCC windows, ports. Runs from RAM on the RP2350 |
| `pico/carnivore2/core/c2_flash.[ch]` | M29W640GB command interface |
| `pico/carnivore2/core/c2_eeprom.[ch]` | Microwire configuration EEPROM |
| `pico/carnivore2/storage/c2_store.[ch]` | Flash image reads, PSRAM write-back cache, commit and EEPROM persistence |
| `pico/carnivore2/storage/sunrise_*.c` | Sunrise IDE controller and microSD back-end (shared with the other PicoVerse firmwares) |
| `pico/carnivore2/audio/c2_audio.[ch]` | SCC/SCC+, YM2413, PSG and beeper mixing with the `mv16` volume curve, I2S output |
| `pico/carnivore2/carnivore2.c` | PSRAM/PIO setup, cold-boot handshake, core 0 bus loop, core 1 storage/audio loop |
| `tool/src/carnivore2.c` | UF2 creator |
| `tests/c2_core_test.c` | Host tests of the core, FlashROM and EEPROM models |

Core 0 services the memory cycles (PIO0) and I/O cycles (PIO1). Core 1 runs the microSD IDE back-end and renders audio. Register writes for the sound chips are queued from core 0 to core 1 in a lock-free ring.

## Known limitations

- **Slave slot / "dual slot" modes.** The original FPGA also watches `/MERQ`, `/M1` and the slot registers of other slots. The PicoVerse cartridge edge only provides `/SLTSL`, so the `SCART_*` registers are stored and read back but do not change decoding, and the second cartridge ("Slave Slot as Master's Subslot") features are not available.
- **Delayed reconfiguration** triggered by the opcode fetch from address `0x0000` is detected by sampling the address bus, which can miss a very short access.
- **IDE** uses a microSD card (SD back-end of the Sunrise IDE emulation), so the CF card compatibility list does not apply. Only the master device is emulated.
- **FlashROM timing.** Program/erase finish immediately; toggle-bit and busy timing are not reproduced. Erase suspend/resume is accepted but has no effect.
- **Audio** is rendered at 44.1 kHz by the PicoVerse emulators (emu2212, emu2413, emu2149), not by the FPGA wave/OPLL cores, and is mixed digitally in the RP2350.
- **Configuration EEPROM** starts blank (`0xFF`) until the boot menu writes its settings.

## Credits

Carnivore2 is designed by the Russian Bear Service Crew (RBSC): [github.com/RBSC/Carnivore2](https://github.com/RBSC/Carnivore2). The PicoVerse implementation is an independent emulation based on the published sources and is not affiliated with RBSC. SCC/SCC+ uses emu2212, PSG uses emu2149 and FM uses emu2413 (© Mitsutaka Okazaki, MIT License).
