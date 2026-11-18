#ifndef EXPLORER_EMU2608_H
#define EXPLORER_EMU2608_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Makoto YM2608 (OPNA) at 8 MHz: FM, SSG and ADPCM-B with external sample RAM.
// The chip state lives in memory lent by the firmware (see OPNA_StateSize).
uint32_t OPNA_StateSize(void);
bool OPNA_Attach(void *memory, uint32_t size);

// adpcm_ram may be NULL (ADPCM-B data is then ignored).
void OPNA_Reset(uint8_t *adpcm_ram, uint32_t adpcm_ram_size);

// Renders one 44.1 kHz stereo sample.
void OPNA_Clock(int32_t *output);

// port is the Makoto offset: 0 = FM1 address, 1 = FM1 data, 2 = FM2 address, 3 = FM2 data.
void OPNA_Write(uint32_t port, uint8_t data);
uint8_t OPNA_Read(uint32_t port);

#ifdef __cplusplus
}
#endif

#endif
