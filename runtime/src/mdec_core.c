/* MDEC numeric core: the scale table, block decode (RLE walk, dequantisation,
 * inverse DCT) and YCbCr -> RGB for runtime/src/mdec.c.
 *
 * Clean-room: written from docs/MDEC_DECODER_SPEC.md and psx-spx's chapter
 * "Macroblock Decoder (MDEC)" only. Section numbers below are the spec's. */

#include "mdec_core.h"

#include <stdint.h>

#define MDEC_END_OF_BLOCK 0xFE00u

/* zagzig[k]: natural position 8 * vertical + horizontal frequency of scan
 * index k, the inverse of psx-spx's zigzag table (4.3). */
static const uint8_t zagzig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* floor(v / 2^n) for either sign. Right-shifting a negative int is
 * implementation-defined in C; this form is not, and compiles to one shift. */
static int32_t floor_shr(int32_t v, unsigned n) {
    return v < 0 ? ~(~v >> n) : v >> n;
}

static int32_t sat(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int32_t sgn(int32_t v) {
    return (v > 0) - (v < 0);
}

static int32_t sext10(uint32_t w) {
    int32_t v = (int32_t)(w & 0x3FFu);
    return v >= 0x200 ? v - 0x400 : v;
}

static int32_t sext16(uint16_t w) {
    return w >= 0x8000u ? (int32_t)w - 0x10000 : (int32_t)w;
}

/* clip8 (2): the low 9 bits as a signed number, then saturated to 8 bits. */
static int32_t clip8(int32_t v) {
    int32_t w = (int32_t)((uint32_t)v & 0x1FFu);
    if (w >= 0x100) w -= 0x200;
    return sat(w, -128, 127);
}

/* R(s) = floor((s + 2^14) / 2^15), the rounding of each IDCT pass (4.4). */
static int32_t idct_round(int32_t s) {
    return floor_shr(s + 16384, 15);
}

void mdec_core_set_scale(int16_t table[64], const uint16_t raw[64]) {
    /* 3.2-3.3: S[r][c] = floor(raw[8r + c] / 8), stored transposed. */
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            table[8 * c + r] = (int16_t)floor_shr(sext16(raw[8 * r + c]), 3);
}

/* 4.2: value x at scan index k, q = quant[0] (DC) or qscale * quant[k] (AC),
 * to a coefficient in sixteenths of psx-spx's units. */
static int32_t dequantise(int32_t x, int32_t q, int ac) {
    int32_t v;
    if (q == 0)
        v = 32 * x;
    else
        v = 16 * (ac ? floor_shr(x * q, 3) : x * q) - 8 * sgn(x);
    return sat(v, -16384, 16383);
}

/* 4.4: out[8y + x] = P[y][x] from coef[8u + h] = C[u][h], with
 * S[r][c] = table[8c + r]. `cols` has bit h set when column h of C may hold
 * a non-zero coefficient; an all-zero column gives T[y][h] = R(0) = 0. */
static void idct(int16_t out[64], const int32_t coef[64], unsigned cols,
                 const int16_t table[64]) {
    int32_t t[64]; /* T[y][h] at 8y + h; |T| <= 16384 */

    /* Pass 1 (vertical): T[y][h] = R(sum_u S[u][y] * C[u][h]). */
    for (int y = 0; y < 8; y++) {
        const int16_t *s_y = &table[8 * y]; /* s_y[u] = S[u][y] */
        for (int h = 0; h < 8; h++) {
            int32_t sum = 0;
            if (cols & (1u << h))
                for (int u = 0; u < 8; u++)
                    sum += (int32_t)s_y[u] * coef[8 * u + h];
            t[8 * y + h] = idct_round(sum);
        }
    }

    /* Pass 2 (horizontal): P[y][x] = clip8(R(sum_h T[y][h] * S[h][x])). */
    for (int y = 0; y < 8; y++) {
        const int32_t *t_y = &t[8 * y];
        for (int x = 0; x < 8; x++) {
            const int16_t *s_x = &table[8 * x]; /* s_x[h] = S[h][x] */
            int32_t sum = 0;
            for (int h = 0; h < 8; h++)
                sum += t_y[h] * (int32_t)s_x[h];
            out[8 * y + x] = (int16_t)clip8(idct_round(sum));
        }
    }
}

int mdec_core_decode_block(int16_t out[64], const uint16_t *in,
                           uint32_t *pos, uint32_t end,
                           const uint8_t quant[64], const int16_t table[64],
                           uint32_t *qscale) {
    int32_t coef[64] = {0};
    unsigned cols = 1u; /* the DC sits in column 0 */
    uint32_t p = *pos;
    uint32_t w, scale, k = 0;

    if (p >= end) return 0; /* 4.1a */

    /* 4.1b: end codes before a block are padding while input remains; when it
     * runs out, the last one read is the block's first word. */
    w = in[p++];
    while (w == MDEC_END_OF_BLOCK && p < end) w = in[p++];

    /* 4.1c: qscale and the DC value at scan index 0. */
    scale = w >> 10;
    coef[0] = dequantise(sext10(w), quant[0], 0);

    /* 4.1d: run/value words until an end code, k >= 64 or index 63 stored. */
    while (p < end && k < 63u) {
        uint32_t at;
        w = in[p++];
        if (w == MDEC_END_OF_BLOCK) break;
        k += (w >> 10) + 1u;
        if (k >= 64u) break;
        at = zagzig[k];
        coef[at] = dequantise(sext10(w), (int32_t)(scale * quant[k]), 1);
        cols |= 1u << (at & 7u);
    }

    *pos = p;
    *qscale = scale;
    idct(out, coef, cols, table);
    return 1;
}

/* 5.4: one channel (-128..127) to five bits. */
static unsigned to5(int32_t v) {
    uint32_t c5 = (uint32_t)(v + 128 + 4) >> 3;
    return c5 > 31u ? 31u : c5;
}

void mdec_core_color_macroblock(uint8_t *out, const int16_t cr[64],
                                const int16_t cb[64], const int16_t y[4][64],
                                unsigned depth, int is_signed, int bit15) {
    /* 5.2: the chroma terms of each Cr/Cb sample, shared by its 2x2 pixels. */
    int32_t dr[64], dg[64], db[64];
    const uint32_t flip24 = is_signed ? 0x80u : 0u;
    const uint32_t flip15 = (is_signed ? 0x4210u : 0u) | (bit15 ? 0x8000u : 0u);

    for (int i = 0; i < 64; i++) {
        int32_t vr = cr[i], vb = cb[i];
        dr[i] = floor_shr(359 * vr + 128, 8);
        dg[i] = floor_shr(32 * floor_shr(-88 * vb, 5) +
                          8 * floor_shr(-183 * vr, 3) + 128, 8);
        db[i] = floor_shr(454 * vb + 128, 8);
    }

    /* 5.1: row-major over the 16x16 macroblock; Y0 Y1 on top, Y2 Y3 below. */
    for (int py = 0; py < 16; py++) {
        for (int px = 0; px < 16; px++) {
            const int16_t *yb = y[2 * (py >= 8) + (px >= 8)];
            int32_t luma = yb[8 * (py & 7) + (px & 7)];
            int ci = (px >> 1) + 8 * (py >> 1);
            int32_t r = clip8(luma + dr[ci]);
            int32_t g = clip8(luma + dg[ci]);
            int32_t b = clip8(luma + db[ci]);
            if (depth == 3) {
                /* 5.4: 15-bit, R in the low bits, low byte first. */
                uint32_t w = (to5(r) | to5(g) << 5 | to5(b) << 10) ^ flip15;
                *out++ = (uint8_t)w;
                *out++ = (uint8_t)(w >> 8);
            } else {
                /* 5.3: 24-bit, bytes R, G, B. */
                *out++ = (uint8_t)((uint32_t)(r + 128) ^ flip24);
                *out++ = (uint8_t)((uint32_t)(g + 128) ^ flip24);
                *out++ = (uint8_t)((uint32_t)(b + 128) ^ flip24);
            }
        }
    }
}
