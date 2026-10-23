/* The MDEC numeric core as it was before the clean-room rewrite, moved
 * here unchanged behind mdec_core.h so that mdec.c (the device) no longer
 * carries it. Its numeric rules were transcribed from Beetle PSX /
 * Mednafen (see the audit behind docs/MDEC_DECODER_SPEC.md); it is to be
 * replaced by a clean-room mdec_core.c and deleted. Whoever writes that
 * file must not read this one. */
#include "mdec_core.h"
#include "psx_align.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* x86-64 always has SSE2; MSVC x64 does not define __SSE2__. */
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define MDEC_HAVE_SSE2 1
#endif
#if !defined(MDEC_HAVE_SSE2) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define MDEC_HAVE_NEON 1
#endif

/* Zig-zag scatter table — Beetle ZigZag[64] (mdec.cpp:115), the column-major
 * order that pairs with Beetle's IDCT matrix transpose + IDCT_1D_Multi below.
 * (Our old table was the row-major transpose of this; it only decoded correctly
 * because our old IDCT was correspondingly transposed. The faithful pipeline
 * uses Beetle's table + matrix + IDCT together so the result is byte-exact.) */
static const uint8_t zigzag_to_linear[64] = {
    0x00, 0x08, 0x01, 0x02, 0x09, 0x10, 0x18, 0x11,
    0x0a, 0x03, 0x04, 0x0b, 0x12, 0x19, 0x20, 0x28,
    0x21, 0x1a, 0x13, 0x0c, 0x05, 0x06, 0x0d, 0x14,
    0x1b, 0x22, 0x29, 0x30, 0x38, 0x31, 0x2a, 0x23,
    0x1c, 0x15, 0x0e, 0x07, 0x0f, 0x16, 0x1d, 0x24,
    0x2b, 0x32, 0x39, 0x3a, 0x33, 0x2c, 0x25, 0x1e,
    0x17, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x3c, 0x35,
    0x2e, 0x27, 0x2f, 0x36, 0x3d, 0x3e, 0x37, 0x3f
};

static int16_t sign_extend_10(uint16_t value) {
    return (int16_t)((int16_t)(value << 6) >> 6);
}

static int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/* Sign-extend the low `bits` of v to a full int (Beetle sign_x_to_s32). */
static int sign_x_to_s32(int bits, int v) {
    int shift = 32 - bits;
    return (int)(((int32_t)((uint32_t)v << shift)) >> shift);
}

/* 9-bit mask then clamp to int8 (Beetle Mask9ClampS8, mdec.cpp:230). The MDEC
 * keeps intermediate samples to 9 bits before the ±127 clamp, so a value outside
 * the 9-bit window WRAPS before clamping — reproducing the hardware ringing. */
static int mask9_clamp_s8(int v) {
    v = sign_x_to_s32(9, v);
    if (v < -128) v = -128;
    if (v >  127) v =  127;
    return v;
}

/* Faithful R3000A MDEC IDCT (Beetle IDCT/IDCT_1D_Multi). Two separable 1-D
 * passes over the >>3 scale matrix: pass 1 keeps int16 and transposes
 * (out[x*8+col]); pass 2 clamps via Mask9ClampS8. Rounding (sum+0x4000)>>15.
 * SSE2 path matches Beetle's madd_epi16 reduce (bit-identical to scalar). */

static void idct_block_dc_only(int16_t *block, const int16_t *scale)
{
    int16_t tmp0[8];
    int dc = block[0];
    int x, col;
    for (x = 0; x < 8; x++) {
        int sum = dc * (int)scale[x * 8];
        tmp0[x] = (int16_t)((sum + 0x4000) >> 15);
    }
    for (col = 0; col < 8; col++) {
        for (x = 0; x < 8; x++) {
            int sum = (int)tmp0[col] * (int)scale[x * 8];
            block[col * 8 + x] =
                (int16_t)mask9_clamp_s8((sum + 0x4000) >> 15);
        }
    }
}

static void idct_block_scalar(int16_t *block, const int16_t *scale)
{
    int ac = 0;
    int i, col, x, u;
    int16_t tmp[64];

    for (i = 1; i < 64; i++)
        ac |= block[i];
    if (!ac) {
        idct_block_dc_only(block, scale);
        return;
    }

    for (col = 0; col < 8; col++) {
        const int16_t *src = block + col * 8;
        int col_or = (int)src[0] | (int)src[1] | (int)src[2] | (int)src[3] |
                     (int)src[4] | (int)src[5] | (int)src[6] | (int)src[7];
        if (!col_or) {
            for (x = 0; x < 8; x++)
                tmp[x * 8 + col] = 0;
            continue;
        }
        for (x = 0; x < 8; x++) {
            int sum = 0;
            const int16_t *sc = scale + x * 8;
            for (u = 0; u < 8; u++)
                sum += (int)src[u] * (int)sc[u];
            tmp[x * 8 + col] = (int16_t)((sum + 0x4000) >> 15);
        }
    }
    for (col = 0; col < 8; col++) {
        const int16_t *src = tmp + col * 8;
        int col_or = (int)src[0] | (int)src[1] | (int)src[2] | (int)src[3] |
                     (int)src[4] | (int)src[5] | (int)src[6] | (int)src[7];
        if (!col_or) {
            for (x = 0; x < 8; x++)
                block[col * 8 + x] = 0;
            continue;
        }
        for (x = 0; x < 8; x++) {
            int sum = 0;
            const int16_t *sc = scale + x * 8;
            for (u = 0; u < 8; u++)
                sum += (int)src[u] * (int)sc[u];
            block[col * 8 + x] =
                (int16_t)mask9_clamp_s8((sum + 0x4000) >> 15);
        }
    }
}

#if defined(MDEC_HAVE_SSE2)
/* Horizontal sum of 4×i32 after madd_epi16 — same reduce as Beetle mdec.c. */
static int idct_sse2_dot8(const int16_t *src8, const int16_t *scale8)
{
    __m128i c = _mm_loadu_si128((const __m128i *)src8);
    __m128i m = _mm_loadu_si128((const __m128i *)scale8);
    __m128i sum = _mm_madd_epi16(m, c);
    PSX_ALIGN(16) int32_t tmp[4];
    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(0, 1, 2, 3)));
    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(0, 0, 0, 1)));
    _mm_store_si128((__m128i *)tmp, sum);
    return tmp[0];
}

static void idct_block_sse2(int16_t *block, const int16_t *scale)
{
    int ac = 0;
    int i, col, x;
    PSX_ALIGN(16) int16_t tmp[64];

    for (i = 1; i < 64; i++)
        ac |= block[i];
    if (!ac) {
        idct_block_dc_only(block, scale);
        return;
    }

    for (col = 0; col < 8; col++) {
        const int16_t *src = block + col * 8;
        int col_or = (int)src[0] | (int)src[1] | (int)src[2] | (int)src[3] |
                     (int)src[4] | (int)src[5] | (int)src[6] | (int)src[7];
        if (!col_or) {
            for (x = 0; x < 8; x++)
                tmp[x * 8 + col] = 0;
            continue;
        }
        for (x = 0; x < 8; x++) {
            int sum = idct_sse2_dot8(src, scale + x * 8);
            tmp[x * 8 + col] = (int16_t)((sum + 0x4000) >> 15);
        }
    }
    for (col = 0; col < 8; col++) {
        const int16_t *src = tmp + col * 8;
        int col_or = (int)src[0] | (int)src[1] | (int)src[2] | (int)src[3] |
                     (int)src[4] | (int)src[5] | (int)src[6] | (int)src[7];
        if (!col_or) {
            for (x = 0; x < 8; x++)
                block[col * 8 + x] = 0;
            continue;
        }
        for (x = 0; x < 8; x++) {
            int sum = idct_sse2_dot8(src, scale + x * 8);
            block[col * 8 + x] =
                (int16_t)mask9_clamp_s8((sum + 0x4000) >> 15);
        }
    }
}

static void idct_simd_selfcheck(void)
{
    uint32_t seed = 0xC0FFEEu;
    int n;
    int16_t scale[64];
    for (n = 0; n < 64; n++)
        scale[n] = (int16_t)(((n % 9) - 4) * 256);
    /* Deterministic patterns + PRNG — abort if SSE2 ever drifts from scalar. */
    for (n = 0; n < 512; n++) {
        int16_t a[64], b[64];
        int i;
        for (i = 0; i < 64; i++) {
            seed = seed * 1664525u + 1013904223u;
            a[i] = (int16_t)((int)(seed >> 16) - 32768);
            if (n < 8)
                a[i] = (i == 0) ? (int16_t)(n * 100) : 0; /* DC-only */
            b[i] = a[i];
        }
        idct_block_scalar(a, scale);
        idct_block_sse2(b, scale);
        if (memcmp(a, b, sizeof(a)) != 0)
            abort();
    }
}

static void idct_block(int16_t *block, const int16_t *scale)
{
    idct_block_sse2(block, scale);
}
#else
static void idct_block(int16_t *block, const int16_t *scale)
{
    idct_block_scalar(block, scale);
}
#endif

/* 8-bit unsigned channel → 5-bit, Beetle RGB_to_RGB555 rounding (mdec.cpp:306).
 * Beetle's RGB_to_RGB555 takes uint8 params, so the ^0x80 result is truncated to
 * 0..255 BEFORE the round/shift — `c` here is already that uint8. */
static int rgb_to_555_chan(uint8_t c) {
    int v = (c + 4) >> 3;
    if (v > 0x1F) v = 0x1F;
    return v;
}

/* Emit one YCbCr pixel into a pre-reserved output cursor (bit-identical to
 * the former append_byte path). Returns advanced cursor. */
static uint8_t *emit_rgb_pixel(uint8_t *out, int y, int cr, int cb,
                               unsigned depth, int is_signed, int bit15) {
    /* Beetle YCbCr_to_RGB (mdec.cpp:293-304): /256 coeffs (359,-88/-183,454),
     * +0x80 rounding, the reduced-precision GREEN mask (-88*cb &~0x1F, -183*cr
     * &~0x07) — the hardware quirk our old /1024 path lacked, the main green-hue
     * error — Mask9ClampS8, then ^0x80 to unsigned 0..255. */
    int r = mask9_clamp_s8(y + (((359 * cr) + 0x80) >> 8));
    int g = mask9_clamp_s8(y + ((((-88 * cb) & ~0x1F) + ((-183 * cr) & ~0x07) + 0x80) >> 8));
    int b = mask9_clamp_s8(y + (((454 * cb) + 0x80) >> 8));
    int ru = r ^ 0x80, gu = g ^ 0x80, bu = b ^ 0x80;   /* signed → unsigned */

    if (depth == 3) {
        /* 16bpp (mdec.cpp:397-418): RGB555 then pixel_xor = bit15(0x8000) |
         * signed(0x4210 = MSB of each 5-bit channel). */
        uint16_t packed = (uint16_t)(rgb_to_555_chan(ru)
                                     | (rgb_to_555_chan(gu) << 5)
                                     | (rgb_to_555_chan(bu) << 10));
        uint16_t pixel_xor = (uint16_t)((bit15 ? 0x8000u : 0u)
                                        | (is_signed ? 0x4210u : 0u));
        packed ^= pixel_xor;
        out[0] = (uint8_t)packed;
        out[1] = (uint8_t)(packed >> 8);
        return out + 2;
    }
    /* 24bpp (mdec.cpp:370-393): rgb_xor = signed ? 0x80 : 0x00. */
    uint8_t rgb_xor = is_signed ? 0x80u : 0x00u;
    out[0] = (uint8_t)(ru ^ rgb_xor);
    out[1] = (uint8_t)(gu ^ rgb_xor);
    out[2] = (uint8_t)(bu ^ rgb_xor);
    return out + 3;
}

#if defined(MDEC_HAVE_SSE2)
/* Beetle mdec.c EncodeRow24 — 8 luma + 4 chroma → 24 RGB bytes, bit-exact. */
#define MDEC_MUL32(a, b)                                                       \
    _mm_or_si128(                                                              \
        _mm_and_si128(_mm_mul_epu32((a), (b)), _mm_set1_epi64x(0xFFFFFFFFull)),\
        _mm_slli_epi64(                                                        \
            _mm_mul_epu32(_mm_srli_epi64((a), 32), _mm_srli_epi64((b), 32)),  \
            32))
#define MDEC_M9(v)                                                             \
    _mm_min_epi16(_mm_max_epi16(_mm_srai_epi16(_mm_slli_epi16((v), 7), 7),     \
                                _mm_set1_epi16(-128)),                         \
                  _mm_set1_epi16(127))

static void mdec_encode_row24_sse2(const int16_t *by, const int16_t *cb4,
                                   const int16_t *cr4, uint8_t rgb_xor,
                                   uint8_t *out)
{
    int16_t cbd[8], crd[8];
    int8_t by8[8];
    int l, i;
    __m128i R, G, B;
    __m128i flip = _mm_set1_epi16((short)(0x80 ^ rgb_xor));
    uint8_t r8[8], g8[8], b8[8];
    __m128i y16, CB, CR, cb_lo, cb_hi, cr_lo, cr_hi, y_lo, y_hi;
    const __m128i k359 = _mm_set1_epi32(359);
    const __m128i k454 = _mm_set1_epi32(454);
    const __m128i km88 = _mm_set1_epi32(-88);
    const __m128i km183 = _mm_set1_epi32(-183);
    const __m128i bias = _mm_set1_epi32(0x80);
    __m128i r_lo, r_hi, b_lo, b_hi, a0, a1, c0, c1, g_lo, g_hi;

    for (l = 0; l < 8; l++)
        by8[l] = (int8_t)by[l];
    for (l = 0; l < 4; l++) {
        cbd[2 * l] = cbd[2 * l + 1] = (int16_t)cb4[l];
        crd[2 * l] = crd[2 * l + 1] = (int16_t)cr4[l];
    }

    y16 = _mm_srai_epi16(
        _mm_slli_epi16(
            _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)by8),
                              _mm_setzero_si128()),
            8),
        8);
    CB = _mm_loadu_si128((const __m128i *)cbd);
    CR = _mm_loadu_si128((const __m128i *)crd);
    cb_lo = _mm_srai_epi32(_mm_unpacklo_epi16(CB, CB), 16);
    cb_hi = _mm_srai_epi32(_mm_unpackhi_epi16(CB, CB), 16);
    cr_lo = _mm_srai_epi32(_mm_unpacklo_epi16(CR, CR), 16);
    cr_hi = _mm_srai_epi32(_mm_unpackhi_epi16(CR, CR), 16);
    y_lo = _mm_srai_epi32(_mm_unpacklo_epi16(y16, y16), 16);
    y_hi = _mm_srai_epi32(_mm_unpackhi_epi16(y16, y16), 16);

    r_lo = _mm_add_epi32(
        y_lo, _mm_srai_epi32(_mm_add_epi32(MDEC_MUL32(k359, cr_lo), bias), 8));
    r_hi = _mm_add_epi32(
        y_hi, _mm_srai_epi32(_mm_add_epi32(MDEC_MUL32(k359, cr_hi), bias), 8));
    b_lo = _mm_add_epi32(
        y_lo, _mm_srai_epi32(_mm_add_epi32(MDEC_MUL32(k454, cb_lo), bias), 8));
    b_hi = _mm_add_epi32(
        y_hi, _mm_srai_epi32(_mm_add_epi32(MDEC_MUL32(k454, cb_hi), bias), 8));
    a0 = _mm_andnot_si128(_mm_set1_epi32(0x1F), MDEC_MUL32(km88, cb_lo));
    a1 = _mm_andnot_si128(_mm_set1_epi32(0x1F), MDEC_MUL32(km88, cb_hi));
    c0 = _mm_andnot_si128(_mm_set1_epi32(0x07), MDEC_MUL32(km183, cr_lo));
    c1 = _mm_andnot_si128(_mm_set1_epi32(0x07), MDEC_MUL32(km183, cr_hi));
    g_lo = _mm_add_epi32(
        y_lo,
        _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(a0, c0), bias), 8));
    g_hi = _mm_add_epi32(
        y_hi,
        _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(a1, c1), bias), 8));
    R = MDEC_M9(_mm_packs_epi32(r_lo, r_hi));
    G = MDEC_M9(_mm_packs_epi32(g_lo, g_hi));
    B = MDEC_M9(_mm_packs_epi32(b_lo, b_hi));
    R = _mm_xor_si128(_mm_and_si128(R, _mm_set1_epi16(0xFF)), flip);
    G = _mm_xor_si128(_mm_and_si128(G, _mm_set1_epi16(0xFF)), flip);
    B = _mm_xor_si128(_mm_and_si128(B, _mm_set1_epi16(0xFF)), flip);
    _mm_storel_epi64((__m128i *)r8, _mm_packus_epi16(R, R));
    _mm_storel_epi64((__m128i *)g8, _mm_packus_epi16(G, G));
    _mm_storel_epi64((__m128i *)b8, _mm_packus_epi16(B, B));
    for (i = 0; i < 8; i++) {
        out[i * 3 + 0] = r8[i];
        out[i * 3 + 1] = g8[i];
        out[i * 3 + 2] = b8[i];
    }
}
#undef MDEC_MUL32
#undef MDEC_M9
#endif /* MDEC_HAVE_SSE2 */

/* Self-checks that used to run in mdec_init: abort if a SIMD path ever
 * drifts from the scalar one. Run once, on first use. */
static void legacy_selfcheck(void) {
    static int done;
    if (done) return;
    done = 1;
#if defined(MDEC_HAVE_SSE2)
    idct_simd_selfcheck();
    /* YCbCr row vs scalar emit — MotK FMV path (depth 2 = 24bpp). */
    {
        uint32_t seed = 0xBADC0DEEu;
        int n, i;
        for (n = 0; n < 256; n++) {
            int16_t by[8], cb4[4], cr4[4];
            uint8_t sse[24], ref[24];
            uint8_t *rp = ref;
            for (i = 0; i < 8; i++) {
                seed = seed * 1664525u + 1013904223u;
                by[i] = (int16_t)((int)(seed % 255u) - 128);
            }
            for (i = 0; i < 4; i++) {
                seed = seed * 1664525u + 1013904223u;
                cb4[i] = (int16_t)((int)(seed % 255u) - 128);
                seed = seed * 1664525u + 1013904223u;
                cr4[i] = (int16_t)((int)(seed % 255u) - 128);
            }
            mdec_encode_row24_sse2(by, cb4, cr4, 0, sse);
            for (i = 0; i < 8; i++)
                rp = emit_rgb_pixel(rp, by[i], cr4[i >> 1], cb4[i >> 1], 2, 0, 0);
            if (memcmp(sse, ref, 24) != 0)
                abort();
        }
        for (n = 0; n < 64; n++) {
            int16_t by[8], cb4[4], cr4[4];
            uint8_t sse[24], ref[24];
            uint8_t *rp = ref;
            for (i = 0; i < 8; i++) {
                seed = seed * 1664525u + 1013904223u;
                by[i] = (int16_t)((int)(seed % 255u) - 128);
            }
            for (i = 0; i < 4; i++) {
                seed = seed * 1664525u + 1013904223u;
                cb4[i] = (int16_t)((int)(seed % 255u) - 128);
                seed = seed * 1664525u + 1013904223u;
                cr4[i] = (int16_t)((int)(seed % 255u) - 128);
            }
            mdec_encode_row24_sse2(by, cb4, cr4, 0x80u, sse);
            for (i = 0; i < 8; i++)
                rp = emit_rgb_pixel(rp, by[i], cr4[i >> 1], cb4[i >> 1], 2, 1, 0);
            if (memcmp(sse, ref, 24) != 0)
                abort();
        }
    }
#endif
}

void mdec_core_set_scale(int16_t table[64], const uint16_t raw[64]) {
    /* Load the IDCT matrix exactly as Beetle (mdec.cpp:647): store each entry
     * TRANSPOSED ([(i&7)<<3 | (i>>3)&7]) and pre-shifted >>3 (arithmetic), so
     * IDCT_1D_Multi above can index it [x*8+u] directly with no per-tap /8. */
    for (uint32_t i = 0; i < 64u; i++) {
        uint32_t t = ((i & 7u) << 3) | ((i >> 3) & 7u);
        table[t] = (int16_t)((int16_t)raw[i] >> 3);
    }
}

int mdec_core_decode_block(int16_t block[64], const uint16_t *in,
                           uint32_t *pos, uint32_t end,
                           const uint8_t quant[64], const int16_t table[64],
                           uint32_t *qscale_out) {
    legacy_selfcheck();
    memset(block, 0, 64 * sizeof(int16_t));
    if (*pos >= end) return 0;

    uint16_t word = in[(*pos)++];
    while (word == 0xFE00u && *pos < end) {
        word = in[(*pos)++];
    }

    /* Dequant in Beetle's <<4 fixed-point domain (mdec.cpp:439-485), clamp
     * ±0x4000. DC uses quant[0] with no qscale; AC uses qscale*quant[k]. Each
     * nonzero coeff gets the sign-magnitude rounding bias (ci<0 ? +8 : -8) the
     * old +4/÷8 model omitted, and the <<4 domain feeds the >>3 IDCT matrix. */
    uint32_t qscale = (word >> 10) & 0x3Fu;
    *qscale_out = qscale;
    uint32_t k = 0;
    int ci = sign_extend_10(word & 0x03FFu);
    int q  = (int)quant[0];
    int tmp = (q != 0) ? (((ci * q) << 4) + (ci ? (ci < 0 ? 8 : -8) : 0))
                       : ((ci * 2) << 4);
    block[0] = (int16_t)clamp_int(tmp, -0x4000, 0x3FFF);

    while (*pos < end && k < 63u) {
        word = in[(*pos)++];
        if (word == 0xFE00u) break;

        k += ((word >> 10) & 0x3Fu) + 1u;
        if (k >= 64u) break;

        ci = sign_extend_10(word & 0x03FFu);
        q  = (int)qscale * (int)quant[k];
        tmp = (q != 0) ? ((((ci * q) >> 3) << 4) + (ci ? (ci < 0 ? 8 : -8) : 0))
                       : ((ci * 2) << 4);
        block[zigzag_to_linear[k]] = (int16_t)clamp_int(tmp, -0x4000, 0x3FFF);
    }

    idct_block(block, table);
    return 1;
}

void mdec_core_color_macroblock(uint8_t *out, const int16_t crblk[64],
                                const int16_t cbblk[64],
                                const int16_t yblk[4][64], unsigned depth,
                                int is_signed, int bit15) {
    uint8_t *p = out;
    legacy_selfcheck();
#if defined(MDEC_HAVE_SSE2)
    /* MotK FMV is 24bpp — row SIMD (Beetle EncodeRow24). 16bpp stays scalar. */
    if (depth != 3) {
        uint8_t rgb_xor = is_signed ? 0x80u : 0x00u;
        int py;
        for (py = 0; py < 16; py++) {
            int y_left = (py >= 8 ? 2 : 0);
            int y_right = y_left + 1;
            int ly = py & 7;
            int crow = (py >> 1) * 8;
            mdec_encode_row24_sse2(&yblk[y_left][ly * 8], &cbblk[crow],
                                   &crblk[crow], rgb_xor, p);
            p += 24;
            mdec_encode_row24_sse2(&yblk[y_right][ly * 8], &cbblk[crow + 4],
                                   &crblk[crow + 4], rgb_xor, p);
            p += 24;
        }
        return;
    }
#endif
    for (int py = 0; py < 16; py++) {
        for (int px = 0; px < 16; px++) {
            int y_index = (py >= 8 ? 2 : 0) + (px >= 8 ? 1 : 0);
            int lx = px & 7;
            int ly = py & 7;
            int chroma = (px >> 1) + (py >> 1) * 8;
            p = emit_rgb_pixel(p, yblk[y_index][lx + ly * 8],
                               crblk[chroma], cbblk[chroma], depth, is_signed,
                               bit15);
        }
    }
}
