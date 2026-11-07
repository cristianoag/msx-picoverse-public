# MSX PicoVerse 2040 — Sunrise IDE Emulation for Nextor

This document describes the implementation of the Sunrise IDE interface emulation in the MSX PicoVerse 2040 firmware. The emulation allows the RP2040's USB-C port to function as a Sunrise IDE-compatible hard disk, running Nextor DOS from any USB mass storage device (flash drive, USB-to-SD adapter, etc.).

## 1. Overview

The MSX PicoVerse 2040 Sunrise IDE emulation provides a complete implementation of the Sunrise MSX IDE interface, allowing the MSX to boot Nextor DOS and access USB mass storage devices through the RP2040's USB-C port. The implementation:

- Emulates Sunrise IDE FlashROM banking for the 128 KB Nextor ROM image (8 × 16 KB pages)
- In mapper mode (`-m` / mapper type `11`), adds 192KB memory mapper RAM (12 x 16KB pages)
- Emulates the full ATA task-file register set
- Translates ATA sector read/write commands into USB Mass Storage Class (MSC) Bulk-Only Transport operations
- Runs the Nextor 2.1.4 Sunrise IDE driver (Master Only edition)
- Uses the RP2040's dual-core architecture for deterministic bus timing

Note: The 128KB value above refers only to the Sunrise/Nextor ROM image size. Mapper mode (`-m`) provides a separate 192KB RAM mapper region (12 x 16KB pages) for Nextor compatibility.

## 2. The Original Sunrise IDE Hardware

The Sunrise IDE interface is a cartridge-based IDE controller designed for the MSX platform. It was one of the most popular mass storage solutions for MSX computers. Key characteristics:

### FlashROM

- **Size**: 128 KB organized as 8 pages × 16 KB each
- **Window**: Single 16 KB window at `0x4000`–`0x7FFF` (MSX page 1)
- **No ROM at page 2**: The `0x8000`–`0xBFFF` range is not used by the Sunrise cartridge; the MSX provides its own RAM there
- **Contains**: The Nextor (or MSX-DOS 2) kernel plus the Sunrise IDE device driver

### Control Register at 0x4104

- **Write-only** register
- **Bit 0**: IDE register enable (0 = disabled, 1 = enabled)
- **Bits 7:5**: FlashROM page number (BIT-REVERSED — see §5)
- **Bits 4:1**: Unused / reserved

### IDE Registers

When bit 0 of the control register is set (IDE enabled), the following regions are overlaid on top of the ROM in the `0x7C00`–`0x7EFF` address range:

| Address Range     | Function                                                |
| ----------------- | ------------------------------------------------------- |
| `0x7C00`–`0x7DFF` | 16-bit IDE data register (low/high byte latch)          |
| `0x7E00`–`0x7EFF` | ATA task-file registers (mirrored every 16 bytes)       |
| `0x7F00`–`0x7FFF` | Normal ROM data (not intercepted by IDE)                |

### ATA Interface

The Sunrise IDE implements a standard ATA (IDE) interface supporting:

- Master/Slave device selection
- PIO data transfer (no DMA)
- 28-bit LBA addressing
- Standard ATA commands (IDENTIFY DEVICE, READ SECTORS, WRITE SECTORS, etc.)

### Reference Implementation

The Carnivore2 cartridge includes a Sunrise IDE-compatible implementation in VHDL. The relevant signal for bank switching is:

```vhdl
IDEROMADDR <= cReg(5) & cReg(6) & cReg(7) & Addr(13..0)
```

This reverses bits 5, 6, and 7 of the control register to recover the actual page number — confirming the bit-reversal scheme used by the Nextor bank-switching routine (see §5).


## 3. Architecture

The Sunrise IDE emulation spans all major subsystems of the PicoVerse 2040:

```
┌──────────────────────────────────────────────────────────┐
│  MSX Bus                                                 │
│  /SLTSL, /RD, /WR, A0-A15, D0-D7, /WAIT                │
└──────────────┬───────────────────────┬───────────────────┘
               │                       │
        ┌──────▼───────┐        ┌──────▼────────┐
        │  SM0 (PIO)   │        │  SM1 (PIO)    │
        │  Read Resp.  │        │  Write Capt.  │
        │  /WAIT ctrl  │        │  Addr+Data    │
        └──────┬───────┘        └──────┬────────┘
               │ RX FIFO               │ RX FIFO
               │                       │
        ┌──────▼───────────────────────▼────────┐
        │            Core 0 (CPU)               │
        │  ┌─────────────────────────────────┐  │
        │  │  loadrom_sunrise() main loop    │  │
        │  │  - Drain write FIFO             │  │
        │  │  - Get read address (blocking)  │  │
        │  │  - sunrise_ide_handle_read()    │  │
        │  │  - sunrise_ide_handle_write()   │  │
        │  │  - Sunrise mapper page lookup   │  │
        │  │  - Respond via TX FIFO          │  │
        │  └─────────────────────────────────┘  │
        └──────────────────┬────────────────────┘
                           │  Shared volatile struct
                           │  (sunrise_ide_t)
        ┌──────────────────▼────────────────────┐
        │            Core 1 (CPU)               │
        │  ┌─────────────────────────────────┐  │
        │  │  sunrise_usb_task() loop        │  │
        │  │  - tuh_task() USB host polling  │  │
        │  │  - Service read/write requests  │  │
        │  │  - tuh_msc_read10/write10       │  │
        │  │  - Completion callbacks          │  │
        │  │  - Pending IDENTIFY handling    │  │
        │  └─────────────────────────────────┘  │
        └──────────────────┬────────────────────┘
                           │ USB Host (Full Speed)
                    ┌──────▼──────┐
                    │   USB-C     │
                    │ Mass Storage│
                    │   Device    │
                    └─────────────┘
```

## 4. Memory Map

### Full Address Space View (slot `0x4000`–`0x7FFF`)

When the PicoVerse is selected via `/SLTSL`, the following addresses in page 1 are active:

| Address Range       | IDE Disabled                | IDE Enabled                          |
| ------------------- | --------------------------- | ------------------------------------ |
| `0x4000`–`0x4103`   | ROM (current page)          | ROM (current page)                   |
| `0x4104`            | ROM data (read) / cReg (write) | ROM data (read) / cReg (write)    |
| `0x4105`–`0x7BFF`   | ROM (current page)          | ROM (current page)                   |
| `0x7C00`–`0x7DFF`   | ROM (current page)          | **IDE 16-bit data register**         |
| `0x7E00`–`0x7EFF`   | ROM (current page)          | **IDE task-file registers**          |
| `0x7F00`–`0x7FFF`   | ROM (current page)          | ROM (current page)                   |

Note: `0x8000`–`0xBFFF` is **not** served by the PicoVerse in Sunrise mode. The MSX's own RAM mapper or other device provides this range.

### ROM Page Mapping

The 128 KB FlashROM is divided into 8 pages of 16 KB each:

| Page | ROM Offset         | Content (Nextor 2.1.4)              |
| ---- | ------------------ | ----------------------------------- |
| 0    | `0x00000`–`0x03FFF` | Nextor kernel bank 0                |
| 1    | `0x04000`–`0x07FFF` | Nextor kernel bank 1                |
| 2    | `0x08000`–`0x0BFFF` | Nextor kernel bank 2                |
| 3    | `0x0C000`–`0x0FFFF` | Nextor kernel bank 3                |
| 4    | `0x10000`–`0x13FFF` | Nextor kernel bank 4                |
| 5    | `0x14000`–`0x17FFF` | Nextor driver code (bank 5)         |
| 6    | `0x18000`–`0x1BFFF` | Nextor kernel bank 6                |
| 7    | `0x1C000`–`0x1FFFF` | Nextor kernel bank 7                |


## 5. Control Register (0x4104) and Bank Bit Reversal

This is one of the most critical implementation details and a common source of bugs.

### The Register Format

```
  Bit:    7    6    5    4    3    2    1    0
        +----+----+----+----+----+----+----+----+
        | S0 | S1 | S2 |  ----- unused --- | EN |
        +----+----+----+----+----+----+----+----+
```

- **EN** (bit 0): IDE registers enable
- **S0, S1, S2** (bits 7, 6, 5): Page number bits (BIT-REVERSED)

### Why Bit Reversal Happens

The Nextor kernel's `chgbnk.mac` routine for the Sunrise IDE driver reverses the bits of the bank number before writing them to the control register. From the Nextor source:

```z80
; Bank selection routine for Sunrise IDE
;   bit 0 of bank → bit 7 of register
;   bit 1 of bank → bit 6 of register
;   bit 2 of bank → bit 5 of register
```

So when Nextor wants to select page 5 (binary `101`):

1. Bank number = `101` (5)
2. Bit-reversed: bit0→bit7, bit1→bit6, bit2→bit5 = `10100000` (`0xA0`)
3. OR with IDE enable: `0xA1`
4. Written to `0x4104`

### How the Hardware Reverses It Back

The real Sunrise hardware (and Carnivore2 VHDL) reverses the bits to form the ROM address:

```vhdl
IDEROMADDR <= cReg(5) & cReg(6) & cReg(7) & Addr(13..0)
```

This means:
- ROM address bit 16 = register bit 5 (= original bank bit 2)
- ROM address bit 15 = register bit 6 (= original bank bit 1)
- ROM address bit 14 = register bit 7 (= original bank bit 0)

### PicoVerse Emulation Code

```c
uint8_t raw = (data >> 5) & 0x07;  // Extract bits [7:5] as 3-bit value
// Reverse 3 bits: {bit2,bit1,bit0} → {bit0,bit1,bit2}
ide->segment = (uint8_t)(((raw & 4) >> 2) | (raw & 2) | ((raw & 1) << 2));
```

The `segment` value (0–7) is then used directly to compute the ROM offset:

```c
uint32_t rel = ((uint32_t)seg << 14) + (addr & 0x3FFFu);
```

## 6. IDE Register Emulation

### Task-File Registers (`0x7E00`–`0x7EFF`)

The IDE registers are mapped at `0x7E00`–`0x7E0F` and mirrored every 16 bytes up to `0x7EFF`. Only the low 4 bits of the address select the register.

| Offset | Read                  | Write                 |
| ------ | --------------------- | --------------------- |
| `+0`   | Data (use 0x7C00)    | Data (use 0x7C00)    |
| `+1`   | Error Register        | Feature Register      |
| `+2`   | Sector Count          | Sector Count          |
| `+3`   | Sector Number / LBA[7:0]   | Sector Number / LBA[7:0]   |
| `+4`   | Cylinder Low / LBA[15:8]   | Cylinder Low / LBA[15:8]   |
| `+5`   | Cylinder High / LBA[23:16] | Cylinder High / LBA[23:16] |
| `+6`   | Device/Head           | Device/Head           |
| `+7`   | Status Register       | Command Register      |
| `+E`   | Alternate Status      | Device Control        |

### Status Register Bits

| Bit | Name | Description                |
| --- | ---- | -------------------------- |
| 7   | BSY  | Device is busy             |
| 6   | DRDY | Device is ready            |
| 4   | DSC  | Device seek complete       |
| 3   | DRQ  | Data request (ready for transfer) |
| 0   | ERR  | Error occurred             |

### Error Register Bits

| Bit | Name | Description                     |
| --- | ---- | ------------------------------- |
| 6   | UNC  | Uncorrectable data error        |
| 5   | MC   | Media changed                   |
| 4   | IDNF | ID not found                    |
| 3   | MCR  | Media change requested          |
| 2   | ABRT | Aborted command                 |
| 1   | NM   | No media                        |

### Device/Head Register

```
  Bit:    7    6    5    4    3    2    1    0
        +----+----+----+----+----+----+----+----+
        |  1 | LBA|  1 | DEV| HD3| HD2| HD1| HD0|
        +----+----+----+----+----+----+----+----+
```

- **DEV** (bit 4): 0 = master, 1 = slave
- **LBA** (bit 6): 0 = CHS mode, 1 = LBA mode
- **HD[3:0]** (bits 3:0): Head number (CHS) or LBA bits 27:24

### Device Control Register

| Bit | Name  | Description                           |
| --- | ----- | ------------------------------------- |
| 2   | SRST  | Software reset (assert to reset)      |
| 1   | nIEN  | Interrupt enable (0 = enable)         |

## 7. 16-bit Data Register and Byte Latch Mechanism

The IDE data register is inherently 16-bit, but the MSX is an 8-bit system. The Sunrise IDE solves this with a byte-latch mechanism across the `0x7C00`–`0x7DFF` address range:

### Read Operation

1. MSX reads from an **even** address (`addr & 1 == 0`):
   - The emulator fetches a 16-bit word from the sector buffer
   - Returns the **low byte** immediately
   - Latches the **high byte** internally
2. MSX reads from an **odd** address (`addr & 1 == 1`):
   - Returns the previously latched **high byte**
   - Advances the buffer pointer

### Write Operation

1. MSX writes to an **even** address:
   - The byte is latched as the **low byte**
2. MSX writes to an **odd** address:
   - The latched low byte and the current high byte form a 16-bit word
   - The word is stored in the sector buffer
   - If 512 bytes have been received, the sector is committed

### Code Pattern (Nextor driver)

The Nextor driver transfers data using `LDIR` from/to `IDE_DATA` (`0x7C00`):

```z80
ld hl, IDE_DATA     ; 0x7C00
ld de, destination
ld bc, 512
ldir                ; Reads alternate low/high bytes automatically
```

Since `LDIR` increments HL after each byte, the sequence of accesses is:
`0x7C00` (low), `0x7C01` (high), `0x7C02` (low), `0x7C03` (high), ...

## 8. ATA Command Handling

The emulation handles the following ATA commands:

### IDENTIFY DEVICE (0xEC)

- Builds a 512-byte IDENTIFY response with device geometry, model name, serial number, and LBA capacity
- If USB is not mounted yet, sets **BSY** and defers completion until Core 1 sees the USB device mount
- The Nextor driver has a 5-second timeout on `WAIT_DRQ`, giving USB plenty of time to enumerate

### READ SECTORS (0x20)

- **Sector count**: Per the ATA specification, a sector count register value of 0 means **256 sectors** (not zero). Any value 1–255 is taken literally. The `sectors_remaining` field is `uint16_t` to accommodate the 256-sector case.

1. Core 0 sets `usb_read_lba` and `usb_read_requested`, transitions to `IDE_STATE_BUSY`
2. Core 1 picks up the request, issues `tuh_msc_read10()` to the USB device
3. On completion, data is copied to `sector_buffer`, state transitions to `IDE_STATE_READ_DATA` with DRQ set
4. MSX reads 512 bytes through the data register latch mechanism
5. For multi-sector reads, LBA is auto-incremented and the process repeats for up to 256 sectors

### WRITE SECTORS (0x30)

- **Sector count**: Same rule as READ SECTORS — a value of 0 means 256 sectors.

1. Core 0 sets DRQ immediately, transitions to `IDE_STATE_WRITE_DATA`
2. MSX writes 512 bytes through the data register latch mechanism
3. Once 512 bytes are received, the buffer is copied to `usb_write_buffer`
4. Core 1 issues `tuh_msc_write10()` to the USB device
5. On completion, LBA is auto-incremented; if more sectors remain, DRQ is set again

### EXECUTE DEVICE DIAGNOSTIC (0x90)

- Sets the PATA device signature in all registers
- Error register = `0x01` (diagnostic code: no error)
- Sector Count = `0x01`, Sector Number = `0x01`
- Cylinder Low = `0x00`, Cylinder High = `0x00` (PATA signature)
- This is critical for the Nextor `CHKDIAG` routine (see §9)

### DEVICE RESET (0x08)

- Same as EXECUTE DEVICE DIAGNOSTIC for our ATA device

### Software Reset (SRST via Device Control Register)

- **SRST asserted** (bit 2 = 1): Device enters BSY state
- **SRST deasserted** (bit 2 = 0): Device sets the full PATA post-reset signature, including error = `0x01` and cylinder = `0x0000`

### Other Commands

| Command            | Code   | Behavior                    |
| ------------------ | ------ | --------------------------- |
| SET FEATURES       | `0xEF` | Accepted, no action         |
| INITIALIZE DEVICE PARAMETERS | `0x91` | Accepted, no action         |
| RECALIBRATE        | `0x10` | Accepted, no action         |
| Unknown commands   | —      | Status = ERR, Error = ABRT  |

### Slave Device Handling

Any command targeting the slave device (Device/Head register bit 4 = 1) immediately returns ERR + ABRT. The emulation only supports the master device.

## 9. Device Initialization Sequence

Understanding the Nextor Sunrise IDE driver's initialization sequence (`DRV_INIT` in `sunride.asm`) is essential to correct emulation. The sequence proceeds as follows:

### Step 1: Software Reset

```
DRV_INIT (2nd call) →
  IDE_ON (write 0x4104 with bit0=1 to enable IDE) →
  RESET_ALL:
    Select SLAVE → WAIT_BSY
    Select MASTER → WAIT_BSY
    Write M_SRST+M_nIEN to IDE_DEVCTRL  → (our code: BSY)
    Wait ~5µs
    Write M_nIEN to IDE_DEVCTRL          → (our code: sets PATA signature)
    Wait ~2ms
    WAIT_RST (polls BSY for up to 5s)
```

When SRST deasserts, we immediately set the full PATA device signature (status = DRDY|DSC, error = 0x01, cylinder = 0x0000). BSY is already clear.

### Step 2: Execute Device Diagnostic

```
  Write ATACMD.DEVDIAG (0x90) to IDE_CMD
  WAIT_RST (polls BSY for up to 5s)
```

 `ide_set_device_signature()` — error = 0x01, status = DRDY|DSC. BSY is clear immediately.

### Step 3: Check Diagnostics (CHKDIAG)

```
  Read IDE_STATUS → Check M_ERR bit
  If ERR not set → "Initializing: Ok"
  Read IDE_ERROR → Check diagnostic code
    bits[6:0] == 1  → No error (success)
    bits[6:0] == 2  → "formatter device error"
    bits[6:0] == 3  → "sector buffer error"
    bits[6:0] == 4  → "ECC circuitry error"
    bits[6:0] == 5  → "controlling microprocessor error"
    bits[6:0] == 0  → "failed" (no master detected)
    bit 7           → Error flag for slave
```

**Critical**: If we return error = `0x00`, the driver prints `<failed>`. If we return error = `0x04` (ABRT), the driver interprets bits[6:0] = 4 as "ECC circuitry error". Only error = `0x01` means success.

### Step 4: Device Signature Check (GETDEVTYPE)

```
  RESET_ALL.ataonly →
    Select MASTER → WAIT_BSY
    Write SRST → deassert SRST → WAIT_RST
  Read IDE_LBAMID (cylinder low + high as 16-bit value)
    0x0000 → PATA device
    0xEB14 → PATAPI device
    0xC33C → SATA device
    0x9669 → SATAPI device
    0x7F7F / 0xFFFF → Unconnected
```

 After SRST deassert, cylinder_low = 0x00, cylinder_high = 0x00 → driver recognizes a PATA device.

### Step 5: IDENTIFY DEVICE

```
  For PATA: Send ATACMD.IDENTIFY (0xEC) → PIO_CMD → WAIT_DRQ
  Read 512 bytes from IDE_DATA
  Check Word 49 bit 1 (LBA support):
    Set → devtype = LBA (value 2)
    Clear → devtype = CHS (value 1)
```

Build IDENTIFY data with Word 49 = `0x0200` (LBA bit set). If USB is not yet mounted, the device stays BSY and the pending IDENTIFY is completed by Core 1 once USB enumeration finishes. The driver's `WAIT_DRQ` polls for up to 5 seconds.

### Step 6: Device Name Display

The driver prints the model name from IDENTIFY words 27–46, and the firmware revision from words 23–26.

**ATA string byte order**: Each 16-bit word stores the first character of the pair in the **high byte** (bits 15:8) and the second character in the **low byte** (bits 7:0). Our `ata_string_to_words()` helper handles this correctly:

```c
w[i] = ((uint16_t)(uint8_t)src[i * 2] << 8) | (uint8_t)src[i * 2 + 1];
```

The model name is derived from the actual USB device's SCSI INQUIRY response (vendor ID + product ID), so the MSX displays the real name of the connected USB device.

## 10. ATA IDENTIFY DEVICE Response

The 512-byte IDENTIFY DEVICE response is constructed in `build_identify_data()`:

| Word(s)  | Field                         | Value                                           |
| -------- | ----------------------------- | ----------------------------------------------- |
| 0        | General configuration         | `0x0040` (fixed device, non-removable)          |
| 1        | Cylinders                     | Computed from USB block count (max 16383)       |
| 3        | Heads                         | 16                                               |
| 6        | Sectors per track             | 63                                               |
| 10–19    | Serial number (20 chars)      | `"PICOVERSE00000001   "`                        |
| 23–26    | Firmware revision (8 chars)   | From USB SCSI INQUIRY product revision          |
| 27–46    | Model number (40 chars)       | From USB SCSI INQUIRY vendor + product ID       |
| 47       | Max sectors per R/W multiple  | 1                                                |
| 49       | Capabilities                  | `0x0200` (LBA supported)                        |
| 53       | Fields validity               | `0x0001` (words 54–58 valid)                    |
| 54–56    | Current CHS                   | Same as words 1, 3, 6                           |
| 57–58    | Current CHS capacity          | cylinders × heads × spt                         |
| 60–61    | Total LBA sectors             | From USB device block count                     |
| 117–118  | Sector size                   | Not set (driver defaults to 512)                |

The CHS geometry is a synthetic translation for compatibility. The Nextor driver uses LBA mode exclusively after detecting the LBA capability flag.

## 11. USB Mass Storage Integration

### TinyUSB Host Stack

The USB subsystem uses TinyUSB in host mode with the Mass Storage Class (MSC) driver:

- **Configuration**: `tusb_config.h` sets `CFG_TUSB_RHPORT0_MODE = OPT_MODE_HOST`
- **MSC enabled**: `CFG_TUH_MSC = 1`
- **Hub support**: `CFG_TUH_HUB = 1` (supports USB hubs with up to 4 downstream devices)
- **Buffer alignment**: All I/O buffers are 4-byte aligned for DMA (`TU_ATTR_ALIGNED(4)`)

### Device Enumeration

1. USB device is plugged into the RP2040's USB-C port
2. TinyUSB enumerates the device and calls `tuh_msc_mount_cb()`
3. A SCSI INQUIRY command is sent to retrieve vendor/product information
4. On INQUIRY completion, block count and block size are obtained via `tuh_msc_get_block_count()` / `tuh_msc_get_block_size()`
5. `usb_device_mounted` is set to `true`

### Block I/O

- **Read**: `tuh_msc_read10(dev_addr, lun, buffer, lba, 1, callback, 0)`
- **Write**: `tuh_msc_write10(dev_addr, lun, buffer, lba, 1, callback, 0)`
- Transfers are single-sector (512 bytes) due to the ATA↔USB bridge design
- Completion is asynchronous via callbacks (`read_complete_cb`, `write_complete_cb`)

### Error Handling

- LBA out of range → `usb_read_failed` / `usb_write_failed` flags
- Transfer failure (CSW status ≠ 0) → error flags set
- Core 0 translates these into ATA error responses (ERR status + ABRT error)

### Hot-Plug

When a USB device is removed, `tuh_msc_umount_cb()` clears all state. Subsequent ATA commands return ERR + ABRT until a new device is mounted because `usb_device_mounted` will be `false`.

## 12. Dual-Core Design

### Core 0: PIO Bus Engine + IDE Logic

Core 0 runs the main event loop in `loadrom_sunrise()`:

```
while (true) {
    poll loop:
        drain pending writes from SM1 FIFO      ← continuous!
        if SM0 read FIFO non-empty:
            get address, break
    drain any remaining writes
    if address in 0x4000-0x7FFF:
        if sunrise_ide_handle_read() intercepts → return IDE data
        else → return ROM data (page selected by ide.segment)
    else:
        return 0xFF (tri-state)
    respond via SM0 TX FIFO
}
```

**Critical: write FIFO draining must be continuous.** Unlike other mapper types where bank switches involve 1–2 writes between reads, the Sunrise IDE requires bursts of 8–9 writes for each ATA command (bank switch + IDE enable + 6 task-file registers + command). The PIO write SM FIFO is 8 entries deep (joined). If writes are only drained around blocking read calls, the FIFO can overflow during ATA command setup. The PIO write SM stalls on its `push block` instruction when full, silently missing any writes that occur while stalled. This causes lost command or LBA register writes, leading to data corruption.

### Core 1: USB Host Stack

Core 1 runs `sunrise_usb_task()`:

```
tusb_init()
tuh_init(0)
while (true) {
    tuh_task()                    // Process USB events
    service read requests         // tuh_msc_read10
    service write requests        // tuh_msc_write10
    propagate completion to IDE   // Update status/state/buffers
    complete pending IDENTIFY     // If USB just mounted
}
```

### Inter-Core Communication

The two cores communicate through the shared `sunrise_ide_t` structure. All shared fields are declared `volatile`:

| Direction       | Mechanism                                           |
| --------------- | --------------------------------------------------- |
| Core 0 → Core 1 | `usb_read_requested`, `usb_write_requested`, `usb_read_lba`, `usb_write_lba` |
| Core 1 → Core 0 | `usb_read_ready`, `usb_read_failed`, `usb_write_ready`, `usb_write_failed` |
| Core 1 → Core 0 | `status`, `error`, `state`, `buffer_index`, `buffer_length` |
| Core 1 → Core 0 | `sector_buffer[]` contents (after read completion) |
| Core 0 → Core 1 | `usb_identify_pending` |

The communication is safe because:
- Flag transitions are unidirectional (one core sets, the other clears)
- Buffer ownership follows the state machine (Core 0 owns the buffer during DRQ states, Core 1 owns it during BSY)
- The RP2040's memory model provides coherent access between cores for volatile variables

## 13. Nextor ROM Embedding

The Nextor 2.1.4 Sunrise IDE (Master Only) ROM is embedded directly in the loadrom and multirom tool binaries, together with Konamiman's released Nextor 3.0.0 Sunrise IDE (Master Only) ROM:

- **Source files**: `nextor_sunrise.h` (Nextor 2.1.4) and `nextor3.h` (Nextor 3.0.0), generated via `xxd` from the ROM files in each project's `resources/` folder
- **ROM files**: `Nextor-2.1.4.SunriseIDE.MasterOnly.ROM` and `Nextor-3.0.0.SunriseIDE.MasterOnly.ROM`
- **Size**: 131,072 bytes (128 KB, exactly 8 × 16 KB pages)
- **ROM type**: 10 (`ROM_TYPE_SUNRISE`) for standard Sunrise mode, 11 for Sunrise + 192KB mapper mode. Both kernels use the same existing Sunrise loader types.

The loadrom tool creates a UF2 file with a configuration record containing:
- ROM name: `"Nextor Sunrise IDE"` (`-s`), `"Nextor Sunrise+Mapper"` (`-m`), `"Nextor Sunrise 3.0.0"` (`-s3`) or `"Nextor Sunrise 3.0.0+Mapper"` (`-m3`) (up to 50 chars, padded)
- ROM type: `10` or `11`
- ROM size: `131072`
- ROM data: The full 128 KB ROM

In the multirom tool, the same options add menu entries named `Nextor 2.1.4 Sunrise IDE`, `Nextor 2.1.4 Sunrise IDE + 192KB Mapper`, `Nextor 3.0.0 Sunrise IDE` and `Nextor 3.0.0 Sunrise IDE + 192KB Mapper`, each followed by its own copy of the kernel.

The firmware's `main()` reads the rom_type byte and dispatches to `loadrom_sunrise()` when type = 10, or `loadrom_sunrise_mapper()` when type = 11.

## 14. LoadROM Tool Usage

### Building a Sunrise IDE UF2

```bash
loadrom -s
```

Or with a custom output filename:

```bash
loadrom -s -o my_nextor.uf2
```

The `-s` / `--sunrise` flag tells the tool to use the embedded Nextor Sunrise ROM. No external ROM file is needed or accepted.

### Output

```
ROM Type: Sunrise [Embedded]
ROM Name: Nextor Sunrise IDE
ROM Size: 131072 bytes
Pico Offset: 0x00000037
UF2 Output: loadrom.uf2
```

### Flashing

1. Hold the BOOTSEL button on the Pico and connect USB
2. Copy `loadrom.uf2` to the RPI-RP2 drive
3. Connect a USB mass storage device to the Pico's USB-C port
4. Insert the PicoVerse cartridge into the MSX and power on


## 15. Build Configuration

### CMakeLists.txt

The Sunrise IDE build requires these additional dependencies compared to the base loadrom:

```cmake
add_executable(loadrom
    ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c
    loadrom.c
    sunrise_ide.c)

target_link_libraries(loadrom
    pico_stdlib
    pico_multicore      # For multicore_launch_core1
    hardware_dma
    hardware_pio
    tinyusb_board       # Board support for TinyUSB
    tinyusb_host)       # USB host stack
```

### TinyUSB Configuration (`tusb_config.h`)

| Setting                        | Value            | Purpose                     |
| ------------------------------ | ---------------- | --------------------------- |
| `CFG_TUSB_RHPORT0_MODE`       | `OPT_MODE_HOST`  | USB host mode               |
| `CFG_TUH_MSC`                 | `1`              | Mass Storage Class          |
| `CFG_TUH_HUB`                 | `1`              | USB hub support             |
| `CFG_TUH_DEVICE_MAX`          | `4`              | Max downstream devices      |
| `CFG_TUH_ENUMERATION_BUFSIZE` | `256`            | Enumeration buffer          |

## 16. Source File Reference

| File | Purpose |
| ---- | ------- |
| `pico/loadrom/sunrise_ide.h` | Sunrise IDE address map, ATA register/command definitions, IDE state machine, `sunrise_ide_t` shared context structure |
| `pico/loadrom/sunrise_ide.c` | Full Sunrise IDE emulation: ATA command handling, data register latch, mapper bit reversal, USB host task, IDENTIFY response builder, TinyUSB callbacks |
| `pico/loadrom/tusb_config.h` | TinyUSB host mode configuration for MSC |
| `pico/loadrom/loadrom.c` | Main firmware: `loadrom_sunrise()` function with PIO bus loop, write handler, mapper page lookup |
| `pico/loadrom/CMakeLists.txt` | Build configuration with TinyUSB and multicore dependencies |
| `tool/src/loadrom.c` | Windows tool to generate UF2 files, `-s` / `--sunrise` flag for embedded Nextor ROM |
| `tool/src/nextor_sunrise.h` | Embedded Nextor 2.1.4 Sunrise IDE ROM as C byte array |

All paths are relative to `2040/software/loadrom.pio/`.

Author: Cristiano Goncalves
Last updated: 02/22/2026
