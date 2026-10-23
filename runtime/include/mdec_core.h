#ifndef PSXRECOMP_MDEC_CORE_H
#define PSXRECOMP_MDEC_CORE_H

/* MDEC numeric core: what runtime/src/mdec.c (the device) asks of it.
 * Behaviour is specified in docs/MDEC_DECODER_SPEC.md (sections 3-5). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Command 3: the 64 halfwords as received -> the table the decoder keeps
 * (and the snapshot stores): table[8c + r] = floor(raw[8r + c] / 8). */
void mdec_core_set_scale(int16_t table[64], const uint16_t raw[64]);

/* One block from in[*pos .. end). Returns 0, consuming nothing, when
 * *pos >= end. Otherwise returns 1 with out[8*y + x] the decoded samples
 * (-128..127), *pos just past the last word read and *qscale the quantiser
 * scale field of the block's first word. quant: the luma or chroma table,
 * 64 bytes in scan order as uploaded by command 2. */
int mdec_core_decode_block(int16_t out[64], const uint16_t *in,
                           uint32_t *pos, uint32_t end,
                           const uint8_t quant[64], const int16_t table[64],
                           uint32_t *qscale);

/* One 16x16 colour macroblock from its six decoded blocks: 768 bytes at out
 * for depth 2 (24-bit), 512 for depth 3 (15-bit). */
void mdec_core_color_macroblock(uint8_t *out, const int16_t cr[64],
                                const int16_t cb[64], const int16_t y[4][64],
                                unsigned depth, int is_signed, int bit15);

#ifdef __cplusplus
}
#endif

#endif
