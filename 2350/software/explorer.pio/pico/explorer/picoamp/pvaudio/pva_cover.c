// MSX PICOVERSE PROJECT
// pva_cover.c - covers in the MSX's own terms (see pva_cover.h).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "pva_cover.h"

#define N PVA_COVER_PX

// ---------------------------------------------------------------- the picture made small
void pva_cover_begin(pva_cover_t *c, int w, int h)
{
    c->sw = w > 0 ? w : 1;
    c->sh = h > 0 ? h : 1;
    c->cs = c->sw < c->sh ? c->sw : c->sh;      // the middle square
    c->cx = (c->sw - c->cs) / 2;
    c->cy = (c->sh - c->cs) / 2;
    c->done_rows = 0;
    c->have = false;
    memset(c->sum, 0, sizeof c->sum);
    memset(c->cnt, 0, sizeof c->cnt);
    memset(c->img, 0, sizeof c->img);
}

// rows of img up to (not with) 'to' are complete: out of the strip
static void flush_rows(pva_cover_t *c, int to)
{
    if (to > N) to = N;
    for (; c->done_rows < to; c->done_rows++)
    {
        int r = c->done_rows % PVA_COVER_STRIP;
        uint8_t *d = c->img + (size_t)c->done_rows * N * 3;
        for (int x = 0; x < N; x++)
        {
            int n = c->cnt[r][x];
            if (n)
                for (int k = 0; k < 3; k++) d[x * 3 + k] = (uint8_t)((c->sum[r][x][k] + n / 2) / n);
            else if (x)                                         // (none: as the one on the left)
                memcpy(d + x * 3, d + x * 3 - 3, 3);
            else if (c->done_rows)                              // (or the one above)
                memcpy(d, d - N * 3, 3);
        }
        memset(c->sum[r], 0, sizeof c->sum[r]);
        memset(c->cnt[r], 0, sizeof c->cnt[r]);
    }
}

void pva_cover_block(pva_cover_t *c, int x, int y, int w, int h, const uint8_t *rgb)
{
    if (c->cs < N)
    {
        // a small picture (a logo): every pixel fills its part of img
        for (int j = 0; j < h; j++)
        {
            int sy = y + j - c->cy;
            if (sy < 0 || sy >= c->cs) continue;
            int y0 = sy * N / c->cs, y1 = (sy + 1) * N / c->cs;
            for (int i = 0; i < w; i++)
            {
                int sx = x + i - c->cx;
                if (sx < 0 || sx >= c->cs) continue;
                int x0 = sx * N / c->cs, x1 = (sx + 1) * N / c->cs;
                const uint8_t *p = rgb + ((size_t)j * w + i) * 3;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) memcpy(c->img + ((size_t)yy * N + xx) * 3, p, 3);
            }
        }
        return;
    }
    // a big one: every pixel adds to one of img (an average); rows above this block are complete
    int top = y - c->cy;
    if (top > 0) flush_rows(c, top * N / c->cs);
    for (int j = 0; j < h; j++)
    {
        int sy = y + j - c->cy;
        if (sy < 0 || sy >= c->cs) continue;
        int dy = sy * N / c->cs;
        if (dy < c->done_rows || dy >= c->done_rows + PVA_COVER_STRIP) continue;   // (not in order)
        int r = dy % PVA_COVER_STRIP;
        for (int i = 0; i < w; i++)
        {
            int sx = x + i - c->cx;
            if (sx < 0 || sx >= c->cs) continue;
            int dx = sx * N / c->cs;
            const uint8_t *p = rgb + ((size_t)j * w + i) * 3;
            if (c->cnt[r][dx] == 255) continue;
            c->cnt[r][dx]++;
            c->sum[r][dx][0] += p[0];
            c->sum[r][dx][1] += p[1];
            c->sum[r][dx][2] += p[2];
        }
    }
}

void pva_cover_end(pva_cover_t *c)
{
    if (c->cs >= N) flush_rows(c, N);
    c->have = true;
}

// ---------------------------------------------------------------- to the MSX
static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static void pal_rgb(const uint8_t *pal, int i, int *rgb)
{
    rgb[0] = (pal[i * 2] >> 4 & 7) * 255 / 7;
    rgb[1] = (pal[i * 2 + 1] & 7) * 255 / 7;
    rgb[2] = (pal[i * 2] & 7) * 255 / 7;
}

// how far apart two colours look (green counts most, then red)
static int dist(const int *a, const uint8_t *b)
{
    int r = a[0] - b[0], g = a[1] - b[1], bl = a[2] - b[2];
    return 3 * r * r + 4 * g * g + 2 * bl * bl;
}

#define KMEANS_STEPS 10
#define CELLS_A_STEP 4
#ifndef ROW_SPAN
#define ROW_SPAN     2
#endif
#define ROW_STEPS    6              // a row's palette: from the row before (or the picture's), a few steps

static const int pick[16] = { 4211, 1517, 5583, 2999, 6301, 820, 3655, 4977, 2190, 5864, 333, 3071, 1209, 4466, 6100, 2600 };

static void begin(pva_cover_t *c, const uint8_t pal[32], uint16_t free, uint8_t *out, bool rows)
{
    c->out = out;
    c->rows = rows;
    c->row = 0;
    memcpy(out, pal, 32);
    for (int i = 0; i < 16; i++) pal_rgb(pal, i, c->rgb[i]);
    free &= 0xFFFEu;
    c->nf = 0;
    for (int i = 1; i < 16 && c->nf < PVA_COVER_ROW_ENT + 11; i++) if (free & (1u << i)) c->fi[c->nf++] = i;
    // the free entries start at pixels spread over the picture
    for (int k = 0; k < c->nf; k++)
        for (int ch = 0; ch < 3; ch++) c->rgb[c->fi[k]][ch] = c->img[pick[k] * 3 + ch];
    c->it = c->nf ? 0 : KMEANS_STEPS;
    c->cell = 0;
    if (rows)
    {
        uint8_t *t = out + PVA_COVER_SIZE;
        memset(t, 0, PVA_COVER_ROWS_SIZE);
        t[0] = c->nf ? PVA_COVER_CELLS - 1 : 0;
        t[1] = 2;
    }
}

void pva_cover_convert_begin(pva_cover_t *c, const uint8_t pal[32], uint16_t free, uint8_t *out)
{
    begin(c, pal, free, out, false);
}

void pva_cover_convert_begin_rows(pva_cover_t *c, const uint8_t pal[32], uint16_t free, uint8_t *out)
{
    begin(c, pal, free, out, true);
}

// the free entries: the picture's own colours (k-means; the player's colours take part, so that the free
// ones go where those do not fit), at the end in the MSX2's 512 colours. Rows y0 to y1 of the picture
// (all of it, or a row of cells), every 'step'th pixel.
static void kmeans_pass(pva_cover_t *c, int y0, int y1, int step, int kmax)
{
    memset(c->acc, 0, sizeof c->acc);
    for (int p = y0 * N; p < y1 * N; p += step)
    {
        const uint8_t *px = c->img + p * 3;
        int best = 1, bd = 0x7FFFFFFF;
        for (int i = 1; i < 16; i++)
        {
            int d = dist(c->rgb[i], px);
            if (d < bd) { bd = d; best = i; }
        }
        c->acc[best][0] += px[0]; c->acc[best][1] += px[1]; c->acc[best][2] += px[2]; c->acc[best][3]++;
    }
    for (int k = 0; k < c->nf && k < kmax; k++)
    {
        int i = c->fi[k];
        if (c->acc[i][3]) for (int ch = 0; ch < 3; ch++) c->rgb[i][ch] = (int)(c->acc[i][ch] / c->acc[i][3]);
    }
}

static void to_512(pva_cover_t *c, uint8_t *pal)
{
    for (int k = 0; k < c->nf; k++)
    {
        int i = c->fi[k];
        int r = (c->rgb[i][0] * 7 + 127) / 255, g = (c->rgb[i][1] * 7 + 127) / 255, b = (c->rgb[i][2] * 7 + 127) / 255;
        pal[i * 2] = (uint8_t)(r << 4 | b);
        pal[i * 2 + 1] = (uint8_t)g;
        pal_rgb(pal, i, c->rgb[i]);
    }
}

static void kmeans_step(pva_cover_t *c)
{
    kmeans_pass(c, 0, N, 2, 16);                // (every other pixel is enough)
    if (++c->it < KMEANS_STEPS) return;
    to_512(c, c->out);
}

// a row's colours in the entries where the row before had the nearest ones (when a switch comes late,
// the rest of a line shows colours like its own)
static void row_match(pva_cover_t *c, const int prev[][3])
{
    int n = c->nf < PVA_COVER_ROW_ENT ? c->nf : PVA_COVER_ROW_ENT;
    int now[PVA_COVER_ROW_ENT][3], perm[PVA_COVER_ROW_ENT], best[PVA_COVER_ROW_ENT];
    for (int k = 0; k < n; k++) { memcpy(now[k], c->rgb[c->fi[k]], sizeof now[k]); perm[k] = k; }
    long bd = -1;
    // every order (5! = 120 at most), Heap's way
    int cnt[PVA_COVER_ROW_ENT] = { 0 };
    for (int i = 0;;)
    {
        long d = 0;
        for (int k = 0; k < n; k++)
        {
            int r = now[perm[k]][0] - prev[k][0], g = now[perm[k]][1] - prev[k][1], b = now[perm[k]][2] - prev[k][2];
            d += 3 * r * r + 4 * g * g + 2 * b * b;
        }
        if (bd < 0 || d < bd) { bd = d; memcpy(best, perm, sizeof best); }
        while (i < n && cnt[i] >= i) cnt[i++] = 0;
        if (i >= n) break;
        int j = (i & 1) ? cnt[i] : 0, t = perm[j];
        perm[j] = perm[i]; perm[i] = t;
        cnt[i]++;
        i = 1;
    }
    for (int k = 0; k < n; k++) memcpy(c->rgb[c->fi[k]], now[best[k]], sizeof now[k]);
}

// a row's palette is complete: its entries for the player (rows 1-9)
static void row_out(pva_cover_t *c, int row)
{
    uint8_t pal[32];
    memcpy(pal, c->out, 32);
    to_512(c, pal);
    if (row == 0) { memcpy(c->out, pal, 32); return; }
    uint8_t *t = c->out + PVA_COVER_SIZE + PVA_COVER_ROW_HEAD + (row - 1) * PVA_COVER_ROW_BYTES;
    for (int k = 0; k < PVA_COVER_ROW_ENT; k++)
    {
        int i = c->fi[k < c->nf ? k : 0];
        t[k * 4] = (uint8_t)i;
        t[k * 4 + 1] = 0x90;
        t[k * 4 + 2] = pal[i * 2];
        t[k * 4 + 3] = pal[i * 2 + 1];
    }
}

// every 8 pixels of a cell's row: the two colours (and the dither between them) closest to the picture
static void do_cell(pva_cover_t *c, int cell)
{
    uint8_t *pat = c->out + PVA_COVER_PAL_SIZE, *col = pat + PVA_COVER_PAT_SIZE;
    int cy = cell / PVA_COVER_CELLS, cx = cell % PVA_COVER_CELLS;
    int (*rgb)[3] = c->rgb;
    for (int row = 0; row < 8; row++)
    {
        int y = cy * 8 + row;
        const uint8_t *seg = c->img + ((size_t)y * N + cx * 8) * 3;
        int best_err = 0x7FFFFFFF;
        uint8_t best_bits = 0, best_col = 0x11;
        for (int a = 1; a < 16; a++)
            for (int b = a; b < 16; b++)
            {
                int d[3] = { rgb[b][0] - rgb[a][0], rgb[b][1] - rgb[a][1], rgb[b][2] - rgb[a][2] };
                int L = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                int err = 0;
                uint8_t bits = 0;
                for (int x = 0; x < 8 && err < best_err; x++)
                {
                    const uint8_t *p = seg + x * 3;
                    int one = 0;
                    if (L)
                    {
                        int t = ((p[0] - rgb[a][0]) * d[0] + (p[1] - rgb[a][1]) * d[1] + (p[2] - rgb[a][2]) * d[2]) * 16 / L;
                        one = t > bayer[y & 3][(cx * 8 + x) & 3];
                    }
                    err += dist(rgb[one ? b : a], p);
                    if (one) bits |= (uint8_t)(0x80 >> x);
                }
                if (err < best_err) { best_err = err; best_bits = bits; best_col = (uint8_t)(b << 4 | a); }
            }
        pat[cell * 8 + row] = best_bits;
        col[cell * 8 + row] = best_col;
    }
}

bool pva_cover_convert_step(pva_cover_t *c)
{
    if (!c->rows)
    {
        if (c->it < KMEANS_STEPS) { kmeans_step(c); return false; }
        for (int k = 0; k < CELLS_A_STEP && c->cell < PVA_COVER_CELLS * PVA_COVER_CELLS; k++) do_cell(c, c->cell++);
        return c->cell >= PVA_COVER_CELLS * PVA_COVER_CELLS;
    }
    // a palette for every row: the picture's first (row 0 starts from it), then each row's own, from the
    // row before; then the row's cells
    if (c->it < KMEANS_STEPS) { kmeans_pass(c, 0, N, 2, 16); c->it++; return false; }
    int it = c->it - KMEANS_STEPS;              // (the steps of this row)
    if (it < ROW_STEPS && c->nf)
    {
        // (with some lines of the rows around it: the colours change less from row to row)
        int y0 = c->row * 8 - ROW_SPAN, y1 = c->row * 8 + 8 + ROW_SPAN;
        kmeans_pass(c, y0 < 0 ? 0 : y0, y1 > N ? N : y1, 1, PVA_COVER_ROW_ENT);
        c->it++;
        return false;
    }
    if (it == ROW_STEPS || !c->nf)
    {
        if (c->row > 0 && c->nf)
        {
            int prev[PVA_COVER_ROW_ENT][3];
            const uint8_t *t = c->out + PVA_COVER_SIZE + PVA_COVER_ROW_HEAD + (c->row - 2) * PVA_COVER_ROW_BYTES;
            for (int k = 0; k < PVA_COVER_ROW_ENT && k < c->nf; k++)
            {
                uint8_t e[2];
                if (c->row == 1) { e[0] = c->out[c->fi[k] * 2]; e[1] = c->out[c->fi[k] * 2 + 1]; }
                else { e[0] = t[k * 4 + 2]; e[1] = t[k * 4 + 3]; }
                pal_rgb(e, 0, prev[k]);
            }
            row_match(c, prev);
        }
        row_out(c, c->row);
        c->it = KMEANS_STEPS + ROW_STEPS + 1;
    }
    for (int k = 0; k < CELLS_A_STEP && c->cell < (c->row + 1) * PVA_COVER_CELLS; k++) do_cell(c, c->cell++);
    if (c->cell < (c->row + 1) * PVA_COVER_CELLS) return false;
    if (++c->row >= PVA_COVER_CELLS) return true;
    c->it = KMEANS_STEPS;                       // (the next row: from this one's colours)
    return false;
}

void pva_cover_convert(pva_cover_t *c, const uint8_t pal[32], uint16_t free, uint8_t *out)
{
    pva_cover_convert_begin(c, pal, free, out);
    while (!pva_cover_convert_step(c)) {}
}

static void render(const uint8_t *out, uint8_t *rgb_out, bool rows)
{
    const uint8_t *pal = out, *pat = out + PVA_COVER_PAL_SIZE, *col = pat + PVA_COVER_PAT_SIZE;
    int rgb[16][3];
    for (int i = 0; i < 16; i++) pal_rgb(pal, i, rgb[i]);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
        {
            if (rows && x == 0 && (y & 7) == 0 && y && out[PVA_COVER_SIZE])
            {
                const uint8_t *t = out + PVA_COVER_SIZE + PVA_COVER_ROW_HEAD + (y / 8 - 1) * PVA_COVER_ROW_BYTES;
                for (int k = 0; k < PVA_COVER_ROW_ENT; k++) pal_rgb(t + k * 4 + 2, 0, rgb[t[k * 4] & 15]);
            }
            int cell = (y / 8) * PVA_COVER_CELLS + x / 8, row = y & 7;
            int on = pat[cell * 8 + row] & (0x80 >> (x & 7));
            int ci = on ? col[cell * 8 + row] >> 4 : col[cell * 8 + row] & 15;
            uint8_t *d = rgb_out + ((size_t)y * N + x) * 3;
            d[0] = (uint8_t)rgb[ci][0]; d[1] = (uint8_t)rgb[ci][1]; d[2] = (uint8_t)rgb[ci][2];
        }
}

void pva_cover_render(const uint8_t *out, uint8_t *rgb_out) { render(out, rgb_out, false); }
void pva_cover_render_rows(const uint8_t *out, uint8_t *rgb_out) { render(out, rgb_out, true); }
