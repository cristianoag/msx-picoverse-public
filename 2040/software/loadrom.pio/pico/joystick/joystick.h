// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// joystick.h - Pin definitions and constants for MSX PicoVerse USB Joystick firmware
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef JOYSTICK_H
#define JOYSTICK_H

// -----------------------------------------------------------------------
// GPIO pin assignments (directly mapped to MSX bus signals)
// -----------------------------------------------------------------------
// Address lines (A0-A15)
#define PIN_A0     0
#define PIN_A1     1
#define PIN_A2     2
#define PIN_A3     3
#define PIN_A4     4
#define PIN_A5     5
#define PIN_A6     6
#define PIN_A7     7
#define PIN_A8     8
#define PIN_A9     9
#define PIN_A10    10
#define PIN_A11    11
#define PIN_A12    12
#define PIN_A13    13
#define PIN_A14    14
#define PIN_A15    15

// Data lines (D0-D7)
#define PIN_D0     16
#define PIN_D1     17
#define PIN_D2     18
#define PIN_D3     19
#define PIN_D4     20
#define PIN_D5     21
#define PIN_D6     22
#define PIN_D7     23

// Control signals
#define PIN_RD     24   // /RD   — active-low read strobe
#define PIN_WR     25   // /WR   — active-low write strobe
#define PIN_IORQ   26   // /IORQ — active-low I/O request
#define PIN_SLTSL  27   // /SLTSL — active-low slot select (unused by joystick)
#define PIN_WAIT   28   // /WAIT  — active-low, driven by PIO to freeze Z80 during I/O reads
#define PIN_BUSSDIR 29  // BUSSDIR — bus direction control (unused by joystick)

#define PICO_FLASH_SPI_CLKDIV 2

// -----------------------------------------------------------------------
// MSX Joystick / PSG constants
// -----------------------------------------------------------------------

// PSG I/O ports
#define PSG_ADDR_PORT   0xA0u   // PSG register address latch (write)
#define PSG_WRITE_PORT  0xA1u   // PSG register data write
#define PSG_READ_PORT   0xA2u   // PSG register data read

// PSG registers used for joystick
#define PSG_REG_PORTA   14u     // Register 14 — joystick direction + triggers (read)
#define PSG_REG_PORTB   15u     // Register 15 — joystick port select (write, bit 6)

// PSG register 14 bit layout (active-low: 0 = pressed)
// Bit order matches DE-9 pin numbering: pin 1→bit 0, pin 2→bit 1, etc.
#define JOY_BIT_UP      0       // Bit 0: Up       (DE-9 pin 1)
#define JOY_BIT_DOWN    1       // Bit 1: Down     (DE-9 pin 2)
#define JOY_BIT_LEFT    2       // Bit 2: Left     (DE-9 pin 3)
#define JOY_BIT_RIGHT   3       // Bit 3: Right    (DE-9 pin 4)
#define JOY_BIT_TRIGA   4       // Bit 4: Trigger A (DE-9 pin 6)
#define JOY_BIT_TRIGB   5       // Bit 5: Trigger B (DE-9 pin 7)

// Analog stick deadzone (percentage of full range, 0–100)
#define DEADZONE_PERCENT  25

#include <stdbool.h>
#include <stdint.h>

// True when a HID gamepad currently owns the given MSX port (0 or 1).
// Defined in joystick_main.c; used by the Xbox driver so both drivers share
// the two MSX ports.
bool hid_gamepad_port_in_use(uint8_t port);

#endif
