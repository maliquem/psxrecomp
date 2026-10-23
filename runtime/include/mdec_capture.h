#ifndef PSXRECOMP_MDEC_CAPTURE_H
#define PSXRECOMP_MDEC_CAPTURE_H

/* MDEC command capture (debug-tool builds): PSX_MDEC_CAPTURE=<file> in the
 * environment, or mdec_capture_start(), writes one record per finished
 * command so an offline replay can feed the same commands to another decoder
 * build and compare its output byte for byte (by hash). The file carries the
 * game's own movie bitstream: keep it out of git.
 *
 * Layout, little-endian:
 *   header  "MDECCAP\0", u32 MDEC_CAPTURE_VERSION, u32 0
 *   record  u32 MDEC_CAPTURE_RECORD_TAG, u32 command word, u64 guest cycle,
 *           u32 n, u16 input halfwords[n], u32 output bytes,
 *           u64 mdec_fnv1a64(output)
 * Every DECODE/SET_QUANT/SET_SCALE that received all its input is recorded,
 * and every command that takes none (NOP, 4..7). Only DECODE has output; the
 * others record 0 bytes and the hash of nothing (MDEC_FNV64_INIT). Control
 * register writes (reset, DMA enables) are not recorded: they do not change
 * what a command decodes. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDEC_CAPTURE_MAGIC "MDECCAP"          /* 8 bytes with its NUL */
#define MDEC_CAPTURE_VERSION 1u
#define MDEC_CAPTURE_RECORD_TAG 0x444D4352u   /* "RCMD" */

#define MDEC_FNV64_INIT 0xcbf29ce484222325ull
#define MDEC_FNV64_PRIME 0x00000100000001b3ull

static inline uint64_t mdec_fnv1a64(const uint8_t *p, size_t n) {
    uint64_t h = MDEC_FNV64_INIT;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= MDEC_FNV64_PRIME;
    }
    return h;
}

/* 1 = capturing to `path` (truncated). A running capture is closed first.
 * 0 = could not open it, or a build without debug tools. */
int mdec_capture_start(const char *path);
void mdec_capture_stop(void);

#ifdef __cplusplus
}
#endif

#endif
