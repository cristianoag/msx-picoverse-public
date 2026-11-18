#ifndef EXPLORER_EMU278B_H
#define EXPLORER_EMU278B_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t last_output[2];
    uint8_t pcm_address;
} opl4_t;

void OPL4_Reset(opl4_t *chip, const uint8_t *rom, uint32_t rom_size, uint8_t *sample_ram, uint32_t sample_ram_size);
void OPL4_Clock(opl4_t *chip, int32_t *output);
void OPL4_Write(opl4_t *chip, uint32_t port, uint8_t data);
uint8_t OPL4_Read(opl4_t *chip, uint32_t port);

#ifdef __cplusplus
}
#endif

#endif
