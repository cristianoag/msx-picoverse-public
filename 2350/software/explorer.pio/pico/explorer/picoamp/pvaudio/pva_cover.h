// MSX PICOVERSE PROJECT
// pva_cover.h - covers in the MSX's own terms: a picture made small (80x80)
// and then cells of SCREEN 4 (Graphics 3): 8 pixels a row, two colours each,
// from a palette of 16 of the MSX2's 512 colours. Some of the palette's
// entries are the player's own (they stay); the others are chosen for the
// cover (the player shows them in the cover's band of the screen only).
//
// Plain C without the engine: PicoAmp for Windows and for the web use the same
// file. Memory: one pva_cover_t (about 31 KB), no other allocation.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef PVA_COVER_H
#define PVA_COVER_H

#include <stdint.h>
#include <stdbool.h>

#define PVA_COVER_CELLS     10                      // 10 x 10 cells
#define PVA_COVER_PX        (PVA_COVER_CELLS * 8)   // 80 x 80 pixels
#define PVA_COVER_STRIP     20                      // rows in the making while a picture comes in
// the result: the palette (16 x 0RRR0BBB, 00000GGG) for the cover's band, then for every cell (row by
// row) 8 pattern bytes (bit 7 the left pixel, 1 = the first colour), then 8 colour bytes (first << 4 | second)
#define PVA_COVER_PAL_SIZE  32
#define PVA_COVER_PAT_SIZE  (PVA_COVER_CELLS * PVA_COVER_CELLS * 8)
#define PVA_COVER_SIZE      (PVA_COVER_PAL_SIZE + 2 * PVA_COVER_PAT_SIZE)

typedef struct
{
    // the picture as it comes in (pva_cover_begin / _block / _end)
    int      sw, sh;                    // its size
    int      cx, cy, cs;                // the square taken from it (the middle)
    int      done_rows;                 // rows of img finished
    uint16_t sum[PVA_COVER_STRIP][PVA_COVER_PX][3];
    uint8_t  cnt[PVA_COVER_STRIP][PVA_COVER_PX];
    uint8_t  img[PVA_COVER_PX * PVA_COVER_PX * 3];  // RGB, 80 x 80
    bool     have;                      // a whole picture is in img
    // the conversion, step by step (pva_cover_convert_begin / _step)
    uint8_t *out;
    int      rgb[16][3];
    int      fi[16], nf;
    int      it, cell;
    uint32_t acc[16][4];
} pva_cover_t;

// a picture of w x h pixels comes, in blocks; any order of blocks whose tops do not go back more than
// a strip (a JPEG's blocks in the decoder's order, or whole rows from the top)
void pva_cover_begin(pva_cover_t *c, int w, int h);
// a block of RGB pixels (3 bytes each, row after row, stride = w * 3) at x, y
void pva_cover_block(pva_cover_t *c, int x, int y, int w, int h, const uint8_t *rgb);
void pva_cover_end(pva_cover_t *c);

// img to the MSX: pal = the player's 16 colours (0RRR0BBB, 00000GGG each); free = the entries the
// cover may set (bit n: entry n; entry 0 is never used); out = PVA_COVER_SIZE bytes. With free = 0 the
// cover has the player's colours only.
void pva_cover_convert(pva_cover_t *c, const uint8_t pal[32], uint16_t free, uint8_t *out);

// the same in steps that each take a moment (for a player that must not stop): begin, then step until
// it says done (10 steps of the palette, then 4 cells a step)
void pva_cover_convert_begin(pva_cover_t *c, const uint8_t pal[32], uint16_t free, uint8_t *out);
bool pva_cover_convert_step(pva_cover_t *c);

// the result back to RGB (for a PC's screen, or a test's picture): 80 x 80 x 3
void pva_cover_render(const uint8_t *out, uint8_t *rgb);

#endif
