# MSX PicoVerse 2040 USB Joystick

This document describes the standalone USB joystick firmware for the PicoVerse 2040 cartridge. The firmware turns the cartridge into a dedicated USB-to-MSX joystick adapter: plug a standard USB gamepad into the cartridge's USB-C port and it appears as one or two native MSX joysticks to the host system, with no MSX-side software or driver required.

---

## Overview

MSX joysticks are managed through the PSG (Programmable Sound Generator, AY-3-8910 or YM2149) General Purpose I/O interface, not through the PPI. Software reads joystick state by writing to PSG registers via I/O ports `0xA0`–`0xA2`. The PicoVerse joystick firmware intercepts these three ports and overlays USB gamepad input onto the joystick bits of PSG register 14, using an open-drain bus driving technique that allows the real PSG chip to continue functioning normally for sound generation.

### Intercepted I/O Ports

| Port | Direction | Purpose |
| --- | --- | --- |
| `0xA0` | OUT (Z80 writes) | PSG register address latch — the firmware tracks which register the software wants to access |
| `0xA1` | OUT (Z80 writes) | PSG register data write — the firmware watches for writes to R15 bit 6 to know which joystick port is selected |
| `0xA2` | IN (Z80 reads) | PSG register data read — when the latched register is R14, the firmware injects joystick data using open-drain driving |

### Key Features

- Fully standalone: does not require any MSX-side software or driver.
- Supports generic USB HID gamepads, including gamepads connected through USB hubs.
- Supports Xbox 360 and XInput-compatible controllers via a dedicated XInput protocol driver, including the Xbox 360 wireless receiver.
- Supports Xbox One and Xbox Series X|S controllers via a dedicated GIP protocol driver.
- Full joystick coverage: 4 directions (Up, Down, Left, Right) and 2 trigger buttons (A and B).
- Both D-pad (hat switch) and analog stick are supported simultaneously, with a configurable deadzone for the analog stick.
- Up to 2 USB gamepads can be connected, mapped to MSX joystick ports 1 and 2.
- Open-drain bus driving coexists with the real PSG chip — sound output is unaffected.
- Runs at 250 MHz for minimal response latency.

---

## MSX Joystick Interface

This section describes how the MSX joystick hardware works, based on the [General Purpose port](https://www.msx.org/wiki/General_Purpose_port) and [Joystick/joypad controller](https://www.msx.org/wiki/Joystick/joypad_controller) pages of the MSX Wiki.

### Physical Connector

MSX computers provide two DE-9 female joystick connectors. Each connector carries four direction lines, two trigger buttons, an output line, and power:

| DE-9 Pin | Signal | Direction | PSG R14 Bit |
| --- | --- | --- | --- |
| 1 | Up | Input (active-low) | Bit 0 |
| 2 | Down | Input (active-low) | Bit 1 |
| 3 | Left | Input (active-low) | Bit 2 |
| 4 | Right | Input (active-low) | Bit 3 |
| 5 | +5V | Power | — |
| 6 | Trigger A | Input (active-low) | Bit 4 |
| 7 | Trigger B | Input (active-low) | Bit 5 |
| 8 | Output | Output (active-low) | R15 bits 4/5 |
| 9 | GND | Ground | — |

The pin numbering maps directly to the PSG register 14 bit numbering: pin 1 → bit 0, pin 2 → bit 1, and so on.

### Joystick Types

The MSX Technical Specification defines two joystick types:

- **Type A** has one trigger button (or two buttons wired together on pin 6). Compatible with Atari/Commodore single-button joysticks.
- **Type B** has two independent trigger buttons on pins 6 (Trigger A) and 7 (Trigger B). This is the full MSX joystick standard.

The PicoVerse firmware emulates a Type B joystick with full support for both triggers.

### PSG Register 14 (Port A — Joystick Input)

PSG register 14 is a read-only register that reflects the state of the currently selected joystick port. All joystick bits are **active-low** (0 = pressed, 1 = released):

| Bit 7 | Bit 6 | Bit 5 | Bit 4 | Bit 3 | Bit 2 | Bit 1 | Bit 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| CASRD | JIS/KANA | Trigger B | Trigger A | Right | Left | Down | Up |

- **Bits 0–5**: Joystick state from the port selected by R15 bit 6.
- **Bit 6**: JIS/KANA key indicator (Japanese MSX only).
- **Bit 7**: Cassette data input.

The firmware only drives bits 0–5. Bits 6–7 are left tri-stated so the real PSG chip provides the cassette and KANA data unmodified.

### PSG Register 15 (Port B — Joystick Output / Port Select)

PSG register 15 is a write-only register that controls the joystick port selection and output pins:

| Bit 7 | Bit 6 | Bit 5 | Bit 4 | Bit 3 | Bit 2 | Bit 1 | Bit 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Arabic/Kana | Port Select | Pin 8 J2 | Pin 8 J1 | Pin 7 J2 | Pin 6 J2 | Pin 7 J1 | Pin 6 J1 |

- **Bit 6** (Port Select): 1 = read joystick port 1, 0 = read joystick port 2 (active on next R14 read).
- **Bits 0–1**: Pin 6/7 of joystick port 1 (must be set HIGH to read the joystick).
- **Bits 2–3**: Pin 6/7 of joystick port 2 (must be set HIGH to read the joystick).
- **Bits 4–5**: Pin 8 of joystick ports 1/2 (must be LOW to read the joystick).

The firmware only tracks bit 6 (port select) to know which of the two MSX joystick ports the software is reading. All other bits are passed through to the real PSG.

### How Software Reads Joysticks

There are three standard methods to read MSX joysticks, all of which are supported by the PicoVerse firmware:

1. **BIOS routines** (`GTSTCK` at `0x00D5`, `GTTRIG` at `0x00D8`): The recommended method. These routines internally access the PSG via I/O ports.

2. **BIOS PSG access** (`WRTPSG` at `0x0093`, `RDPSG` at `0x0096`): Used by many games (e.g. Konami titles). These routines write/read PSG registers via the same I/O ports.

3. **Direct I/O port access** (`OUT (0xA0),reg` / `IN A,(0xA2)`): Not officially recommended by the MSX documentation, but commonly used in games for performance. The firmware intercepts these I/O ports directly, so all three methods work transparently.

A typical joystick read sequence performed by software:

```
OUT (0xA0), 0x0F    ; Latch register 15
IN  A, (0xA2)       ; Read current R15 value
OR  0x0F            ; Set bits 0-3 (trigger pins high)
AND 0x4F            ; Keep bit 6 (port select), clear bits 4-5 (pin 8 low)
OUT (0xA1), A       ; Write R15 — select port, set pin levels

OUT (0xA0), 0x0E    ; Latch register 14
IN  A, (0xA2)       ; Read R14 — joystick state in bits 0-5
```

The firmware tracks the `OUT (0xA0)` writes to maintain the register latch, the `OUT (0xA1)` writes to R15 to track the port select bit, and responds to `IN (0xA2)` reads when the latch points to R14.

---

## Architecture

The firmware uses both cores of the RP2040 and a dedicated pair of PIO state machines on PIO1, following the same dual-core architecture as the keyboard and MIDI standalone firmwares.

### Dual-Core Design

| Core | Role |
| --- | --- |
| **Core 0** | Services PIO1 IRQ. The IRQ fires when either PIO state machine has data in its RX FIFO. The handler processes PSG register latch writes (`0xA0`), R15 port select writes (`0xA1`), and R14 joystick state reads (`0xA2`). Between interrupts, Core 0 sleeps via `__wfi()`. |
| **Core 1** | Runs the TinyUSB host stack. Calls `tuh_task()` in a tight loop to poll for USB events. When a HID gamepad report arrives, the callback parses it and updates the shared `joystick_state[]` array. |

### PIO State Machines (PIO1)

| SM | Program | Role |
| --- | --- | --- |
| SM0 | `msx_joy_io_read` | Monitors `/IORQ` + `/RD`. When an I/O read cycle begins, asserts `/WAIT` (side-set), captures the 16-bit address, pushes it to the RX FIFO, then stalls on `pull block` until Core 0 supplies the response token. Drives D0–D7 with the response data and pindirs, releases `/WAIT`, waits for `/RD` to deassert, then tri-states the data bus. |
| SM1 | `msx_joy_io_write` | Monitors `/IORQ` + `/WR`. Captures a full 32-bit GPIO snapshot (`mov isr, pins`) containing address and data, pushes it to the RX FIFO for Core 0 to decode. Uses FIFO join for 8-deep RX buffering. No `/WAIT` assertion needed for writes. |

### `/WAIT` Signal

The `/WAIT` line (GPIO 28, active-low) freezes the Z80 while the RP2040 prepares the joystick response. The PIO read program uses side-set to control `/WAIT`:

- **`side 0`** = `/WAIT` asserted (Z80 frozen)
- **`side 1`** = `/WAIT` released (Z80 runs)

Without `/WAIT`, the Z80 completes an I/O read in 3–4 T-cycles (~840–1120 ns at 3.58 MHz), which is not enough time for the push/pull FIFO round-trip. Asserting `/WAIT` guarantees that the CPU receives the joystick data regardless of firmware processing time.

During initialisation, the firmware pre-sets the PIO output register for the `/WAIT` pin to HIGH before switching the GPIO mux to PIO control. This prevents a brief glitch where `/WAIT` could be driven LOW (freezing the Z80) between the `pio_gpio_init()` call and the first PIO instruction executing.

---

## Open-Drain Bus Driving

The most significant design challenge for the joystick firmware is that the real PSG chip on the MSX motherboard also drives the data bus when register 14 is read. Unlike the keyboard firmware (where the PPI can be cleanly overridden), both the PSG and the PicoVerse are electrically connected to D0–D7 during a read cycle.

### The Problem

When the Z80 reads I/O port `0xA2` with register 14 latched, the PSG drives all 8 data lines with its own register 14 value. If the PicoVerse also drives the same lines with different values, bus contention occurs — two outputs fighting over the same wire. This can cause incorrect data reads and, in theory, excessive current draw.

### The Solution: Open-Drain Emulation

The firmware uses a selective pin direction (open-drain) technique:

- **For pressed buttons** (bit value = 0): the PicoVerse drives the corresponding data line LOW by setting `pindir = output` and `pin = 0`.
- **For released buttons** (bit value = 1): the PicoVerse tri-states the corresponding data line by setting `pindir = input`, allowing the PSG to drive it HIGH undisturbed.

This works because:

1. With no USB gamepad connected, the PSG's own R14 shows all joystick bits HIGH (nothing pressed). The PicoVerse tri-states all lines → no contention, PSG data passes through.
2. When a USB gamepad button is pressed, the PicoVerse only pulls the corresponding bit LOW. The PSG is also driving that line HIGH (its own R14 shows nothing pressed on the physical connector), but the LOW drive wins on CMOS logic because the RP2040's output driver can sink current effectively. The resulting bus voltage is LOW, which is the correct value.
3. Bits 6–7 (KANA, cassette) are always tri-stated by the PicoVerse, so the PSG provides those signals without interference.

### Response Token Format

The PIO state machine uses a 16-bit response token:

| Bits | Content |
| --- | --- |
| 7:0 | Data byte (always `0x00` for open-drain — we only ever drive LOW) |
| 15:8 | Pin direction mask (`1` = output/drive, `0` = input/tri-state) |

For joystick data, the pin direction mask is calculated as `~joystick_state & 0x3F` — only the bits corresponding to pressed buttons (value 0 in the active-low state) become outputs.

For non-R14 register reads, the firmware responds with `data=0xFF, pindirs=0x00` (full tri-state), allowing the PSG to handle the read entirely on its own.

---

## USB Gamepad Support

The firmware supports two classes of USB gamepads:

1. **Standard USB HID gamepads**: Devices that present a USB HID interface with a Gamepad or Joystick usage. The firmware parses their HID report descriptors at enumeration time to discover the layout of their reports.

2. **XInput (Xbox 360) controllers**: Xbox 360 wired controllers and many third-party XInput-compatible gamepads use a vendor-specific USB protocol (interface class `0xFF`, subclass `0x5D`, protocol `0x01`) rather than standard USB HID. A dedicated TinyUSB custom class driver claims these interfaces and parses their fixed 20-byte input reports.

3. **Xbox 360 wireless receiver (XUSB)**: The Xbox 360 wireless gaming receiver exposes one vendor-specific interface per controller slot (interface class `0xFF`, subclass `0x5D`, protocol `0x81`). The same custom class driver claims them, asks the receiver which pads are present, assigns an MSX port when a pad connects and releases it when the pad disconnects.

4. **Xbox One / Series X|S (GIP) controllers**: Xbox One, Xbox One S/X, and Xbox Series X|S controllers use a different vendor-specific protocol called GIP (Game Input Protocol, interface class `0xFF`, subclass `0x47`, protocol `0xD0`). These controllers also require a 5-byte initialisation packet to be sent on the OUT endpoint before they begin producing input reports. The same custom class driver handles the Xbox 360 and Xbox One families.

All four coexist and are active simultaneously. Standard HID gamepads are handled by the TinyUSB HID host driver, while XInput, wireless receiver and GIP devices are handled by the custom class driver. Both drivers allocate from the same two MSX joystick ports (up to 2 gamepads in total across all drivers).

### Why Not Boot Protocol

USB HID defines two protocol modes: boot protocol and report protocol. The keyboard firmware uses boot protocol because USB keyboards have a standardised 8-byte boot report format. **Gamepads have no standard boot protocol** — the USB HID specification only defines boot protocol for keyboards and mice.

Therefore, the joystick firmware uses **HID report protocol** (`CFG_TUH_HID_DEFAULT_PROTOCOL = HID_PROTOCOL_REPORT`) and parses each gamepad's HID report descriptor at enumeration time to discover the layout of its reports.

### USB Host Configuration

| Setting | Value | Purpose |
| --- | --- | --- |
| `CFG_TUSB_RHPORT0_MODE` | `OPT_MODE_HOST` | USB host mode |
| `CFG_TUH_HID` | 4 | Up to 4 HID interfaces |
| `CFG_TUH_HUB` | 1 | USB hub support enabled |
| `CFG_TUH_DEVICE_MAX` | 4 | Up to 4 USB devices (with hub) |
| `CFG_TUH_ENUMERATION_BUFSIZE` | 512 | Larger buffer for gamepad descriptors |
| `CFG_TUH_HID_DEFAULT_PROTOCOL` | `HID_PROTOCOL_REPORT` | Report protocol (gamepads have no boot protocol) |

### HID Report Descriptor Parsing

When a USB HID device is mounted, the firmware receives its raw HID report descriptor — a compact binary encoding of the device's report format. The parser (`hid_gamepad_parser.c`) walks this descriptor to locate:

1. **Gamepad/Joystick collection**: The parser looks for a Collection with Usage Page `0x01` (Generic Desktop) and Usage `0x04` (Joystick) or `0x05` (Gamepad). Only fields inside this collection are processed.

2. **Hat switch**: Usage `0x39` (Hat Switch) in the Generic Desktop usage page. Standard 8-direction hats report values 0–7 for N/NE/E/SE/S/SW/W/NW, with the null state (no direction) at logical maximum + 1.

3. **X/Y axes**: Usages `0x30` (X) and `0x31` (Y) in the Generic Desktop usage page. These represent the left analog stick.

4. **Buttons**: Usage Page `0x09` (Button). Button 1 maps to Trigger A, Button 2 maps to Trigger B.

The parser tracks the global state (usage page, logical min/max, report size, report count, report ID) and local state (usages) as it walks the descriptor. For each Input item, it records the bit offset, bit size, and logical range of the field. This information is used at runtime to extract values from incoming HID reports.

If the descriptor does not contain a recognisable gamepad or joystick layout, the device is silently ignored.

### XInput Report Format

Xbox 360 wired controllers send a fixed 20-byte input report via an interrupt IN endpoint:

| Byte(s) | Field | Description |
| --- | --- | --- |
| 0 | Message type | `0x00` = input report |
| 1 | Message length | `0x14` (20) |
| 2–3 | Buttons | 16-bit digital button bitmap (little-endian) |
| 4 | Left trigger | 0–255 (analog) |
| 5 | Right trigger | 0–255 (analog) |
| 6–7 | Left stick X | Signed 16-bit (-32768 to 32767) |
| 8–9 | Left stick Y | Signed 16-bit (-32768 to 32767) |
| 10–11 | Right stick X | Signed 16-bit |
| 12–13 | Right stick Y | Signed 16-bit |
| 14–19 | Reserved | Padding |

**XInput button bitmap:**

| Bit | Button | MSX Mapping |
| --- | --- | --- |
| 0 | D-pad Up | Up (bit 0) |
| 1 | D-pad Down | Down (bit 1) |
| 2 | D-pad Left | Left (bit 2) |
| 3 | D-pad Right | Right (bit 3) |
| 4 | Start | — |
| 5 | Back | — |
| 6 | Left stick click | — |
| 7 | Right stick click | — |
| 8 | LB | — |
| 9 | RB | — |
| 10 | Guide | — |
| 12 | A | Trigger A (bit 4) |
| 13 | B | Trigger B (bit 5) |
| 14 | X | Trigger B (bit 5) |
| 15 | Y | — |

The Xbox left analog stick is also mapped to directions using a deadzone of approximately 25% (8192 out of 32768). The Y axis convention on XInput is positive = up, negative = down.

Xbox 360 pads also get their player LED lit for the assigned MSX port (port 1 = first quadrant, port 2 = second quadrant).

### Xbox 360 Wireless Receiver Packets

The receiver reports on its interrupt IN endpoint, one interface per controller slot:

| Packet | Meaning |
| --- | --- |
| Byte 0 bit 3 set, byte 1 bit 7 = 1 | Pad connected (bit 7 = 0: disconnected) |
| Byte 1 = `0x01`, bytes 0–3 = `00 01 00 F0` | Pad data: the 20-byte XInput report above follows the 4-byte header |

On configuration the firmware sends the 12-byte presence request `08 00 0F C0 00...` on the OUT endpoint so a receiver that was already powered reports its connected pads. When a pad connects, the firmware assigns the next free MSX port and sends the player LED command (`00 00 08 4x 00...`); when it disconnects, the port state returns to `0xFF`. The button, trigger and stick mapping is identical to the wired XInput driver.

### Xbox One / Series X|S (GIP) Report Format

Xbox One controllers use the GIP (Game Input Protocol). Unlike Xbox 360 controllers, they require a 5-byte initialisation packet sent to the interrupt OUT endpoint before they start sending input reports:

```
Init packet: 0x05 0x20 0x00 0x01 0x00
```

The firmware sends this packet automatically during USB configuration.

GIP input reports are 18 bytes: a 4-byte GIP header followed by 14 bytes of gamepad payload:

| Byte(s) | Field | Description |
| --- | --- | --- |
| 0 | Command | `0x20` = input report |
| 1 | Client | Usually `0x00` |
| 2 | Sequence | Incrementing sequence number |
| 3 | Length | Payload length (`0x0E` = 14) |
| 4–5 | Buttons | 16-bit digital button bitmap (little-endian) |
| 6–7 | Left trigger | 0–1023 (10-bit range, 16-bit field) |
| 8–9 | Right trigger | 0–1023 |
| 10–11 | Left stick X | Signed 16-bit (-32768 to 32767) |
| 12–13 | Left stick Y | Signed 16-bit (-32768 to 32767) |
| 14–15 | Right stick X | Signed 16-bit |
| 16–17 | Right stick Y | Signed 16-bit |

**Xbox One button bitmap** (different layout from Xbox 360!):

| Bit | Button | MSX Mapping |
| --- | --- | --- |
| 0 | Sync | — |
| 2 | Menu (Start) | — |
| 3 | View (Back) | — |
| 4 | A | Trigger A (bit 4) |
| 5 | B | Trigger B (bit 5) |
| 6 | X | Trigger B (bit 5) |
| 7 | Y | — |
| 8 | D-pad Up | Up (bit 0) |
| 9 | D-pad Down | Down (bit 1) |
| 10 | D-pad Left | Left (bit 2) |
| 11 | D-pad Right | Right (bit 3) |
| 12 | LB | — |
| 13 | RB | — |
| 14 | Left Stick Click | — |
| 15 | Right Stick Click | — |

The left analog stick deadzone and mapping are identical to the Xbox 360 driver.

---

## Input Mapping

### D-Pad (Hat Switch)

The hat switch is an 8-direction digital input. The standard encoding maps to MSX directions as follows:

| Hat Value | Direction | MSX Bits Cleared |
| --- | --- | --- |
| 0 | North | Bit 0 (Up) |
| 1 | North-East | Bit 0 + Bit 3 (Up + Right) |
| 2 | East | Bit 3 (Right) |
| 3 | South-East | Bit 1 + Bit 3 (Down + Right) |
| 4 | South | Bit 1 (Down) |
| 5 | South-West | Bit 1 + Bit 2 (Down + Left) |
| 6 | West | Bit 2 (Left) |
| 7 | North-West | Bit 0 + Bit 2 (Up + Left) |
| 8+ / null | Centred | None (all direction bits remain HIGH) |

Diagonal directions set two bits simultaneously, matching the behavior of a real joystick where two switches are closed at the same time.

### Analog Stick

The left analog stick's X and Y axes are mapped to digital directions using a centre-relative deadzone:

1. The firmware calculates the centre point as `logical_min + range / 2`.
2. A deadzone threshold is computed as `range × deadzone_percent / 200` (default: 25%).
3. If the stick deflection exceeds the threshold in any direction, the corresponding MSX direction bit is cleared.

The analog stick and hat switch inputs are combined with a logical AND (both active-low), so either input source can activate a direction. This means a gamepad with both a D-pad and an analog stick can use either one interchangeably.

### Trigger Buttons

| USB Button | MSX Function | PSG R14 Bit |
| --- | --- | --- |
| Button 1 (first in descriptor) | Trigger A | Bit 4 |
| Button 2 (second in descriptor) | Trigger B | Bit 5 |

Button numbering follows the order buttons appear in the HID report descriptor. On most gamepads, Button 1 is the primary face button (e.g. "A" on Xbox controllers, "Cross" on PlayStation controllers).

### Complete MSX Joystick Byte

The output is an active-low byte matching the PSG register 14 format:

| Bit | Signal | Value when pressed | Value when released |
| --- | --- | --- | --- |
| 0 | Up | 0 | 1 |
| 1 | Down | 0 | 1 |
| 2 | Left | 0 | 1 |
| 3 | Right | 0 | 1 |
| 4 | Trigger A | 0 | 1 |
| 5 | Trigger B | 0 | 1 |
| 6 | (unused) | 1 (tri-stated) | 1 (tri-stated) |
| 7 | (unused) | 1 (tri-stated) | 1 (tri-stated) |

When no gamepad is connected or no buttons are pressed, the state is `0xFF` (all released), which is electrically invisible on the bus due to the open-drain driving.

---

## Two-Port Support

The firmware supports up to two USB gamepads simultaneously, mapped to MSX joystick ports 1 and 2:

- The **first gamepad** connected is assigned to MSX port 1.
- The **second gamepad** connected is assigned to MSX port 2.
- Additional gamepads beyond two are silently ignored (no free MSX port).

The MSX software selects which port to read via PSG register 15 bit 6. The firmware tracks this bit from `OUT (0xA1)` writes and responds with the corresponding port's state when R14 is read.

When a gamepad is disconnected, its MSX port state is reset to `0xFF` (all released) and the port becomes available for a new gamepad. Up to 4 simultaneous USB HID interfaces are supported (via hubs), but only 2 can be mapped to MSX joystick ports.

---

## Concurrency and Data Sharing

The `joystick_state[2]` array is shared between Core 1 (writer, from USB gamepad reports) and Core 0 (reader, from PIO IRQ handler). Each element is a single `volatile uint8_t`, and updates are bracketed with `__dmb()` (data memory barrier) calls to ensure cross-core visibility.

The `psg_register_latch` and `joystick_port_sel` variables are written only by Core 0 (IRQ handler) and read only by Core 0, so no cross-core synchronisation is needed for them.

---

## FIFO Protocol

### I/O Read (SM0 → Core 0 → SM0)

1. SM0 detects `/IORQ=0` + `/RD=0`, asserts `/WAIT`, captures A0–A15, pushes 16-bit address to RX FIFO.
2. Core 0 IRQ handler reads the address. If port is `0xA2` and the register latch is 14, it pushes an open-drain response token: `data=0x00`, `pindirs=~joystick_state[port_sel] & 0x3F`.
3. For any other register or port, the handler pushes a tri-state token: `data=0xFF`, `pindirs=0x00`.
4. SM0 drives D0–D7 with the data and pindirs, releases `/WAIT`, waits for `/RD` deassert, then tri-states.

### I/O Write (SM1 → Core 0)

1. SM1 detects `/IORQ=0` + `/WR=0`, waits 3 PIO cycles for address and data lines to settle, snapshots all GPIOs via `mov isr, pins`, pushes 32-bit word to RX FIFO.
2. Core 0 IRQ handler extracts `port = bits[7:0]` and `data = bits[23:16]`.
3. Port `0xA0` → updates the register latch. Port `0xA1` with latch at 15 → updates the port select bit (bit 6 of the written data).

---

## GPIO Assignments

| GPIO Range | Signal | Direction |
| --- | --- | --- |
| 0–15 | A0–A15 (address bus) | Input |
| 16–23 | D0–D7 (data bus) | Bidirectional (PIO-controlled) |
| 24 | /RD | Input |
| 25 | /WR | Input |
| 26 | /IORQ | Input |
| 27 | /SLTSL | Input (unused — joystick uses I/O ports only) |
| 28 | /WAIT | Output (PIO side-set, active-low) |
| 29 | BUSSDIR | Input (unused) |

---

## Build System

The joystick firmware is built as part of the `loadrom.pio` aggregate Makefile:

```
make joystick       # Build only the joystick firmware
make all            # Builds all firmwares including joystick
make clean          # Cleans all firmware builds including joystick
```

The build produces `pico/joystick/dist/joystick.bin`, which is converted to a C header (`tool/src/joystick_fw.h`) and embedded in the `loadrom` PC tool. The tool generates a UF2 file for flashing:

```
loadrom -j -o joystick.uf2
```

The `-j` / `--joystick` option is standalone and cannot be combined with ROM loading options or other standalone firmware modes (`-k`, `-i`, `-p`).

---

## Source Files

| File | Purpose |
| --- | --- |
| `pico/joystick/joystick.h` | Pin definitions, PSG port/register constants, joystick bit assignments, deadzone setting |
| `pico/joystick/msx_joystick.pio` | PIO assembly for I/O read responder (with `/WAIT`) and I/O write captor |
| `pico/joystick/joystick_main.c` | Main firmware: PIO IRQ handler, GPIO/PIO init, gamepad management, TinyUSB callbacks, dual-core entry points |
| `pico/joystick/hid_gamepad_parser.h` | Public API for HID report descriptor parsing and joystick extraction |
| `pico/joystick/hid_gamepad_parser.c` | HID descriptor walker, hat/axis/button field extraction, deadzone calculation |
| `pico/joystick/xinput_host.h` | Xbox 360 (XInput, wired and wireless receiver) and Xbox One (GIP) protocol definitions, report structures, button constants |
| `pico/joystick/xinput_host.c` | TinyUSB custom class driver for Xbox 360 (wired and wireless receiver), Xbox One, and Xbox Series X\|S controllers |
| `pico/joystick/tusb_config.h` | TinyUSB host configuration (report protocol, 4 HID interfaces, hub support) |
| `pico/joystick/CMakeLists.txt` | CMake build configuration |
| `pico/joystick/pico_sdk_import.cmake` | Pico SDK import helper |
| `tool/src/joystick_fw.h` | Generated header with embedded firmware binary |

---

## Limitations and Notes

- **Bus contention on R14 reads**: The open-drain technique minimises but does not completely eliminate electrical stress. When a USB button is pressed, the RP2040 pulls a data line LOW while the PSG tries to drive it HIGH. In practice this is safe because the PSG's output drivers are relatively weak and the RP2040's sink current is within the PSG's specifications, but it is a departure from the ideal where only one device drives the bus. Sound output from the PSG is unaffected because PSG registers 0–13 are read/written normally.

- **No physical joystick passthrough**: The firmware does not read the physical DE-9 joystick ports. If a real joystick is also connected to the MSX, the PSG will report the logical AND (active-low) of both the real joystick and the USB gamepad, which effectively means both inputs work simultaneously — pressing a direction on either device activates it.

- **Gamepad compatibility**: Not all USB gamepads will be recognised. The firmware supports four USB gamepad families: standard USB HID gamepads (via HID report descriptor parsing), Xbox 360 / XInput controllers (via vendor-specific class `0xFF`/`0x5D`/`0x01`), the Xbox 360 wireless receiver (`0xFF`/`0x5D`/`0x81`), and Xbox One / Series X|S controllers (via GIP protocol, class `0xFF`/`0x47`/`0xD0`). Proprietary protocols not covered by any of these drivers (e.g. PlayStation DualSense via Bluetooth, third-party wireless adapters with their own protocol) may not be recognised. Wired USB gamepads that present a standard HID gamepad descriptor, an XInput interface, or a GIP interface, and Xbox 360 wireless receivers, should work. The wireless receiver support has not yet been validated on hardware.

- **Analog deadzone**: The 25% deadzone is compiled into the firmware. To change it, modify `DEADZONE_PERCENT` in `joystick.h` and rebuild.

- **Report ID handling**: If the gamepad uses HID report IDs, the firmware strips the first byte of each incoming report before parsing. Multi-report-ID devices where the gamepad data is not in the first report ID may not work correctly.
