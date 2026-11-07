// MSX PICOVERSE PROJECT
// pva.h - PicoVerse Audio Engine: public interface.
//
// Core 0 (bus loop): pva_mem_read() / pva_mem_write() for every memory access
// to the engine's sub-slot. Both are small RAM functions that only touch the
// register window and a mailbox; all real work runs on core 1.
//
// Core 1: pva_core1_init() once (after the microSD card is initialised), then
// pva_core1_poll() in its main loop. pva_core1_poll() never blocks for long:
// at most one MP3 frame or one 4 KB read per call.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef PVA_H
#define PVA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pva_proto.h"

#ifndef PVA_RAM_FUNC
#ifdef PVA_HOST
#define PVA_RAM_FUNC(f) f
#else
#include "pico.h"
#define PVA_RAM_FUNC(f) __not_in_flash_func(f)
#endif
#endif

// Smallest arena the engine accepts (directory index, play list, decoder).
#define PVA_ARENA_MIN_SIZE  (96u * 1024u)

// Shared state. 'arena' must stay valid forever and is not touched by core 0.
extern volatile uint8_t *pva_win;   // the register window (NULL before pva_init)

void pva_init(uint8_t *arena, size_t arena_size);

// Before core 1 starts: true when nothing else writes the card (no Nextor),
// so the engine may change RADIOFAV.M3U itself (FAV_ADD / FAV_DEL).
void pva_allow_card_writes(bool on);

// Optional: the core 1 stack, pre-filled with 0xA5, so DIAG can report its use.
void pva_set_stack(const uint8_t *base, size_t size);

// core 0
uint8_t pva_mem_read(uint16_t addr);
void    pva_mem_write(uint16_t addr, uint8_t data);

// core 1
// True once the MSX has posted a command (the engine may be started lazily).
bool pva_command_pending(void);
void pva_core1_init(void);
// Returns true if it did work (a command, or a block of audio). When it
// returns false the caller may sleep briefly (50-100 us) to leave the shared
// buses to core 0.
bool pva_core1_poll(void);
bool pva_core1_busy(void);      // true while playing

#endif
