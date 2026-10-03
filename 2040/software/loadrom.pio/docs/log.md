# Change Log

## PicoVerse 2040 Loadrom v2.63

- Version bumped to v2.63 (top-level and tool Makefiles).
- `../tools/gen_romdb.py` now downloads the latest database by default from Vampier's ROM DB (`https://romdb.vampier.net/Archive/xml-msxromsdb.zip`) and reads `softwaredb.xml` from the zip in memory, so a local openMSX install is no longer needed. A local `softwaredb.xml` or `.zip` can still be passed as an argument for offline use. The generated header now records the source and the database timestamp. The script also accepts the `ASCII16-X` and lowercase `konami` mapper names used by this database.
- Regenerated `tool/src/romdb.h` from the 2026-10-03 database: 3115 → 3218 entries (103 added, none removed or changed), including 31 ASCII16-X ROMs (mapper 12) that the old DB never matched. Rebuilt `tool/dist/loadrom.exe`.

## PicoVerse 2040 Loadrom v2.62

- Version bumped to v2.62 (top-level and tool Makefiles).
- Fixed lost bank-switch writes in every banked mapper loop (`banked8_loop` for Konami/Konami-SCC/ASCII8, plus ASCII16, ASCII16-X, Neo-8 and Neo-16). Those loops blocked on `pio_sm_get_blocking()` for the next cartridge read and only drained the memory write captor FIFO before and after it, so game code executing from MSX RAM that issued a burst of segment switches without any intervening cartridge read overflowed the 8-entry RX FIFO. The write state machine then stalled on `push block` and the extra switches were silently dropped, leaving the mapper register on a stale segment (and, with an ASCII16-X/Neo cache miss, forcing the bank refill to run while `/WAIT` was already asserted). Added `pio_get_read_draining_writes()`, which drains the memory write FIFO while waiting for the next read, and switched all banked loops to it. Same fix as PicoVerse 2350 Loadrom v2.70, reported with "Go Figure v1.2" (ASCII16-X), which alternates the page-1 segment twice per call from a page-3 RAM routine and hung on the palette cross-fade.
- Kept the new idle loop at the same read-response latency as the previous blocking wait by sampling `FSTAT` once per iteration and testing the read and write RX-empty flags from that single sample. Polling the two FIFOs with separate `pio_sm_is_rx_fifo_empty()` calls doubled the PIO register accesses in the loop, which on the Cortex-M0+ stretched `/WAIT` on every cartridge read enough to slow VDP transfer loops and trigger "VDP too slow" reports in timing-sensitive games.

## PicoVerse 2040 Loadrom v2.61

- Fixed the PC tool mapper-detection read path to reject truncated ROM reads before hashing or scanning the allocated ROM buffer.
- Version bumped to v2.61 (top-level and tool Makefiles).

## PicoVerse 2040 Loadrom v2.60

- Refreshed the generated ROM mapper SHA1 database from the current openMSX `softwaredb.xml` and updated the generator to parse the new attribute-based XML format.
  
## PicoVerse 2040 Loadrom v2.59

- Bumped the loadrom.pio version to v2.59.
- Changed the PIO LoadROM memory-read `/WAIT` driver to open-drain behaviour: the cartridge now pulls `/WAIT` low only while stretching its own slot read cycles and releases the shared line as hi-Z afterward, avoiding contention with another `/WAIT`-using cartridge in the other MSX slot.
- Changed the MIDI and joystick I/O read `/WAIT` responders to the same open-drain behaviour so those auxiliary firmware targets no longer drive the shared `/WAIT` line high when idle or after an I/O read response.

## PicoVerse 2040 Loadrom v2.58

- Bumped the loadrom.pio version to v2.58.
- Set all loadrom.pio firmware target system clocks to 230 MHz to improve stability with pico boards with bad quality flash memory.
