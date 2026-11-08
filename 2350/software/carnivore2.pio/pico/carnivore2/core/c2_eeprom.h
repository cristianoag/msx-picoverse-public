// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// c2_eeprom.h - Microwire configuration EEPROM (128 bytes, 8 bit organisation)
// reachable through CardMDR+0x23 of the Carnivore2. The boot menu stores the
// volume, autostart, PSG and display settings here; c2cfgbck backs it up.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef C2_EEPROM_H
#define C2_EEPROM_H

#include <stdbool.h>
#include <stdint.h>

#include "c2_config.h"

#define C2_EEPROM_SIZE 128u

typedef enum {
    EE_IDLE = 0,    // waiting for the start bit
    EE_CMD,         // shifting opcode + 7 address bits
    EE_WRITE_DATA,  // shifting 8 data bits (WRITE / WRAL)
    EE_WRITE_DONE,  // frame complete, executes when CS falls
    EE_READ,        // shifting data out on DO
    EE_IGNORE       // command without data (EWEN/EWDS/ERAL/ERASE), executes when CS falls
} c2_ee_state_t;

typedef struct {
    uint8_t data[C2_EEPROM_SIZE];
    bool write_enabled;
    bool dirty;                 // data changed since the host last saved it
    uint8_t cs, ck;
    c2_ee_state_t state;
    uint8_t nbits;
    uint16_t shift;
    uint8_t op;                 // 0 = misc, 1 = WRITE, 2 = READ, 3 = ERASE
    uint8_t addr;
    uint8_t out_bit;            // 7..0 index of the next bit to shift out
    uint8_t do_pin;
    bool pending_all;           // WRAL
    bool pending_erase_all;     // ERAL
} c2_eeprom_t;

C2_FUNC void c2_eeprom_init(c2_eeprom_t *e);
C2_FUNC void c2_eeprom_pins(c2_eeprom_t *e, uint8_t cs, uint8_t ck, uint8_t di);
C2_FUNC uint8_t c2_eeprom_do(const c2_eeprom_t *e);

#endif // C2_EEPROM_H
