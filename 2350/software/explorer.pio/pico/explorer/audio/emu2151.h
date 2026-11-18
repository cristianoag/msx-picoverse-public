#ifndef EXPLORER_EMU2151_H
#define EXPLORER_EMU2151_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    opm_flags_none = 0,
    opm_flags_ym2164 = 1,
};

typedef struct {
    int32_t last_output[2];
} opm_t;

void OPM_Clock(opm_t *chip, int32_t *output, uint8_t *sh1, uint8_t *sh2, uint8_t *so);
void OPM_Write(opm_t *chip, uint32_t port, uint8_t data);
uint8_t OPM_Read(opm_t *chip, uint32_t port);
uint8_t OPM_ReadIRQ(opm_t *chip);
uint8_t OPM_ReadCT1(opm_t *chip);
uint8_t OPM_ReadCT2(opm_t *chip);
void OPM_SetIC(opm_t *chip, uint8_t ic);
void OPM_Reset(opm_t *chip, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif
