/*
 * mdec_replay: feed MDEC commands to a decoder build through its public API
 * and fingerprint everything it produces, so two builds can be proven
 * byte-identical.
 *
 * Input (one of):
 *   --corpus FILE        a PSX_MDEC_CAPTURE file (format in mdec_capture.h)
 *   --synthetic SEED N   N generated events covering what a movie does not:
 *                        every depth, signed/bit15, qscale 0, quant entries 0,
 *                        saturating levels, long runs, FE00 padding, missing
 *                        end codes, truncated input, unusual scale tables,
 *                        NOP/invalid commands and snapshot save/load.
 * Options:
 *   --list               one line per event
 *   --expect HASH        exit 1 unless the final fingerprint is HASH (hex)
 *   --check-live         (corpus) each output must match the hash recorded
 *                        live: proves the corpus complete for this decoder
 *   --capture FILE       also capture this run (PSX_MDEC_CAPTURE format)
 *   --show N             print event N's input and stop
 *
 * Per event the fingerprint takes the output (size + FNV-1a-64), the
 * snapshot blob after it (tables, counters, flags: the savestate format) and
 * mdec_mean_qscale_x16.
 *
 * mdec_replay_ab (configure with -DMDEC_REF_COMMIT=<commit>) runs the same
 * events through this tree's decoder and a reference one compiled from that
 * commit's runtime/src/mdec.c under ref_ names, and stops at the first
 * difference with the macroblock, pixel and RLE involved. The reference
 * source is extracted into the build tree only; it is an oracle to run, not
 * to read, for whoever writes a new decoder.
 *
 * Exit: 0 match, 1 mismatch, 2 usage or I/O error.
 */
#include "mdec.h"
#include "mdec_capture.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t s_frame_count = 0;
uint64_t psx_cycle_count = 0;
int debug_server_fmv_quiet(void) { return 0; }

#define MDEC_DATA 0x1F801820u
#define MDEC_EOB 0xFE00u

/* ---- small helpers ------------------------------------------------------- */

typedef struct Bytes {
    uint8_t *p;
    size_t n, cap;
} Bytes;

/* Room for `extra` more bytes at p + n (n unchanged). Fail-fast on OOM. */
static uint8_t *bytes_room(Bytes *b, size_t extra) {
    if (b->n + extra > b->cap) {
        size_t cap = b->cap ? b->cap : 4096u;
        uint8_t *p;
        while (cap < b->n + extra) cap *= 2u;
        p = (uint8_t *)realloc(b->p, cap);
        if (!p) {
            fprintf(stderr, "mdec_replay: out of memory\n");
            exit(2);
        }
        b->p = p;
        b->cap = cap;
    }
    return b->p + b->n;
}

static void bytes_put16(Bytes *b, uint32_t v) {
    uint8_t *p = bytes_room(b, 2u);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    b->n += 2u;
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static uint64_t fnv_add(uint64_t h, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        h ^= (uint8_t)(v >> (8 * i));
        h *= MDEC_FNV64_PRIME;
    }
    return h;
}

/* ---- events -------------------------------------------------------------- */

typedef enum { EV_CMD, EV_STATE, EV_SAVE, EV_LOAD } EvKind;
static const char *const k_ev_name[] = {"cmd", "state", "save", "load"};

typedef struct Event {
    EvKind kind;
    uint64_t cycle;
    uint32_t command;
    const uint8_t *data;   /* EV_CMD: nhw LE halfwords; EV_STATE: snapshot blob */
    uint32_t nhw;
    uint32_t blob_len;
    int has_live;          /* corpus command: output seen live */
    uint32_t live_bytes;
    uint64_t live_hash;
} Event;

typedef struct Source {
    const uint8_t *file;   /* corpus */
    size_t len, pos;
    int synthetic;
    uint64_t rng;
    uint32_t left, idx;
    int have_save;
    Bytes buf;             /* synthetic: current event's halfwords */
} Source;

static int corpus_next(Source *s, Event *e) {
    size_t room = s->len - s->pos;
    const uint8_t *p = s->file + s->pos;
    uint32_t tag;
    if (room == 0) return 0;
    if (room < 16) goto truncated;
    memset(e, 0, sizeof(*e));
    tag = rd32(p);
    if (tag == MDEC_CAPTURE_RECORD_TAG) {
        if (room < 20) goto truncated;
        e->kind = EV_CMD;
        e->command = rd32(p + 4);
        e->cycle = rd64(p + 8);
        e->nhw = rd32(p + 16);
        if ((size_t)e->nhw * 2u + 32u > room) goto truncated;
        e->data = p + 20;
        e->has_live = 1;
        e->live_bytes = rd32(p + 20 + (size_t)e->nhw * 2u);
        e->live_hash = rd64(p + 24 + (size_t)e->nhw * 2u);
        s->pos += (size_t)e->nhw * 2u + 32u;
        return 1;
    }
    if (tag == MDEC_CAPTURE_STATE_TAG) {
        e->kind = EV_STATE;
        e->cycle = rd64(p + 4);
        e->blob_len = rd32(p + 12);
        if ((size_t)e->blob_len + 16u > room) goto truncated;
        e->data = p + 16;
        s->pos += (size_t)e->blob_len + 16u;
        return 1;
    }
    fprintf(stderr, "mdec_replay: unknown record %08x at offset %llu\n", tag,
            (unsigned long long)s->pos);
    return -1;
truncated:   /* the capturing process was killed mid-record */
    fprintf(stderr, "mdec_replay: ignoring a partial last record at offset %llu\n",
            (unsigned long long)s->pos);
    return 0;
}

/* ---- synthetic generator ------------------------------------------------- */

static uint32_t rnd(Source *s) {   /* xorshift64* */
    s->rng ^= s->rng >> 12;
    s->rng ^= s->rng << 25;
    s->rng ^= s->rng >> 27;
    return (uint32_t)((s->rng * 0x2545F4914F6CDD1Dull) >> 32);
}

/* The generator calls rnd() at most once per expression (or across ?: and
 * &&, which are sequenced): C leaves the order of calls inside one
 * expression open, and the pinned fingerprint must not depend on the
 * compiler. */
static uint32_t below(Source *s, uint32_t n) { return rnd(s) % n; }

static int pick_level(Source *s) {
    static const int extremes[4] = {-512, 511, -511, 510};
    uint32_t r = below(s, 8);
    int sign;
    if (r == 0) return extremes[below(s, 4)];
    if (r == 1) return 0;
    if (r <= 4) {
        sign = below(s, 2) ? -1 : 1;
        return sign * (int)(1 + below(s, 16));
    }
    return (int)below(s, 1024) - 512;
}

static void gen_block(Source *s) {
    uint32_t style = below(s, 5), k = 0, n, maxrun, qscale;
    uint32_t lead = below(s, 10) == 0 ? 1 + below(s, 3) : 0;
    while (lead--) bytes_put16(&s->buf, MDEC_EOB);
    qscale = below(s, 8) == 0 ? 0 : below(s, 64);
    bytes_put16(&s->buf, (qscale << 10) | ((uint32_t)pick_level(s) & 0x3FFu));
    switch (style) {
    case 0: n = 0; maxrun = 1; break;                       /* DC only */
    case 1: n = 1 + below(s, 4); maxrun = 8; break;         /* a few */
    case 2: n = 64; maxrun = 2; break;                      /* dense */
    case 3: n = 1 + below(s, 20); maxrun = 64; break;       /* scattered */
    default: n = 64; maxrun = 16; break;                    /* runs past 63 */
    }
    while (n--) {
        uint32_t run = below(s, maxrun);
        bytes_put16(&s->buf, (run << 10) | ((uint32_t)pick_level(s) & 0x3FFu));
        k += run + 1;
        if (k >= 64) return;   /* the word past index 63 ends the block */
    }
    if (style == 4 || below(s, 16) != 0)   /* rarely: no end code at all */
        bytes_put16(&s->buf, MDEC_EOB);
}

static uint32_t flag_noise(Source *s) { return (rnd(s) & 0xFu) << 25; }

static void gen_quant(Source *s, Event *e) {
    uint32_t color = below(s, 2), mode = below(s, 4), n = color ? 128u : 64u;
    uint8_t *p = bytes_room(&s->buf, n);
    for (uint32_t i = 0; i < n; i++) {
        switch (mode) {
        case 0: p[i] = (uint8_t)(2 + below(s, 40)); break;
        case 1: p[i] = (uint8_t)below(s, 256); break;
        case 2: p[i] = (uint8_t)(below(s, 6) == 0 ? 0 : 1 + below(s, 64)); break;
        default: p[i] = (uint8_t)(below(s, 2) ? 255 : below(s, 2)); break;
        }
    }
    s->buf.n += n;
    e->command = 0x40000000u | flag_noise(s) | color;
    e->command |= (rnd(s) & 0xFFFFFFu) << 1;   /* ignored bits */
}

static void gen_scale(Source *s, Event *e) {
    /* psx-spx's standard table: row k is the k-th cosine basis of s0..s7. */
    static const uint16_t sv[8] = {0x5A82, 0x7D8A, 0x7641, 0x6A6D,
                                   0x5A82, 0x471C, 0x30FB, 0x18F8};
    static const int8_t pat[64] = {
        1, 1, 1, 1, 1, 1, 1, 1,     2, 4, 6, 8, -8, -6, -4, -2,
        3, 7, -7, -3, -3, -7, 7, 3, 4, -8, -2, -6, 6, 2, 8, -4,
        5, -5, -5, 5, 5, -5, -5, 5, 6, -2, 8, 4, -4, -8, 2, -6,
        7, -3, 3, -7, -7, 3, -3, 7, 8, -6, 4, -2, 2, -4, 6, -8};
    static const int16_t extremes[6] = {-32768, 32767, -1, 0, 1, 0x5A82};
    uint32_t mode = below(s, 4);
    for (int i = 0; i < 64; i++) {
        int v = pat[i] > 0 ? sv[pat[i] - 1] : -(int)sv[-pat[i] - 1];
        switch (mode) {
        case 0: break;
        case 1: v = (int)(rnd(s) & 0xFFFFu) - 32768; break;
        case 2: v += (int)below(s, 64) - 32; break;
        default: v = extremes[below(s, 6)]; break;
        }
        bytes_put16(&s->buf, (uint32_t)v & 0xFFFFu);
    }
    e->command = 0x60000000u | flag_noise(s);
    e->command |= rnd(s) & 0x1FFFFFFu;   /* ignored bits */
}

static void gen_decode(Source *s, Event *e) {
    static const uint8_t depth_of[8] = {0, 1, 2, 2, 2, 3, 3, 3};
    uint32_t depth = depth_of[below(s, 8)];
    uint32_t units = depth >= 2 ? 1 + below(s, 8) : 1 + below(s, 16);
    uint32_t blocks = depth >= 2 ? units * 6u : units, nw;
    for (uint32_t b = 0; b < blocks; b++) gen_block(s);
    if (below(s, 5) == 0)
        for (uint32_t t = 1 + below(s, 4); t; t--) bytes_put16(&s->buf, MDEC_EOB);
    if (s->buf.n & 2u) bytes_put16(&s->buf, MDEC_EOB);
    nw = (uint32_t)(s->buf.n / 4u);
    if (nw > 1 && below(s, 12) == 0) nw = 1 + below(s, nw - 1);   /* cut short */
    s->buf.n = (size_t)nw * 4u;
    e->command = 0x20000000u | (depth << 27) | nw;
    e->command |= below(s, 2) << 26;            /* signed */
    e->command |= below(s, 2) << 25;            /* bit15 */
    e->command |= (rnd(s) & 0x1FFu) << 16;      /* ignored bits */
}

static int synthetic_next(Source *s, Event *e) {
    uint32_t r;
    if (s->left == 0) return 0;
    s->left--;
    memset(e, 0, sizeof(*e));
    s->buf.n = 0;
    e->kind = EV_CMD;
    e->cycle = (uint64_t)(s->idx + 1u) * 2000u;
    /* Tables first, so the decodes have something to work with. */
    r = s->idx == 0 ? 5 : s->idx == 1 ? 10 : below(s, 100);
    s->idx++;
    if (r < 2) {
        e->kind = EV_SAVE;
        s->have_save = 1;
    } else if (r < 4 && s->have_save) {
        e->kind = EV_LOAD;
    } else if (r < 9) {
        gen_quant(s, e);
    } else if (r < 14) {
        gen_scale(s, e);
    } else if (r < 17) {
        uint32_t k = below(s, 3);
        if (k == 2) {
            e->command = 0x20000000u | flag_noise(s);   /* DECODE of 0 words */
        } else {
            e->command = k == 0 ? 0u : (4 + below(s, 4)) << 29;   /* NOP, 4..7 */
            e->command |= rnd(s) & 0x1FFFFFFFu;
        }
    } else {
        gen_decode(s, e);
    }
    e->data = s->buf.p;
    e->nhw = (uint32_t)(s->buf.n / 2u);
    return 1;
}

static int source_next(Source *s, Event *e) {
    return s->synthetic ? synthetic_next(s, e) : corpus_next(s, e);
}

/* ---- decoders ------------------------------------------------------------ */

typedef struct Decoder {
    const char *name;
    void (*init)(void);
    void (*write)(uint32_t addr, uint32_t value);
    uint32_t (*write_words)(const uint32_t *src, uint32_t n);
    uint32_t (*read_words)(uint32_t *dst, uint32_t n);
    int (*read_ready)(void);
    uint32_t (*snap_bytes)(void);
    void (*snap_write)(uint8_t *p);
    int (*snap_read)(const uint8_t *p, uint32_t len);
    uint32_t (*mean_q16)(void);
    Bytes out, snap, saved;
} Decoder;

typedef struct Result {
    uint32_t out_bytes;
    uint64_t out_hash, snap_hash;
    uint32_t q16;
} Result;

static Decoder s_cur = {"this tree", mdec_init, mdec_write, mdec_dma_write_words,
                        mdec_dma_read_words, mdec_dma_read_ready,
                        mdec_snapshot_bytes, mdec_snapshot_write,
                        mdec_snapshot_read, mdec_mean_qscale_x16,
                        {0}, {0}, {0}};

#ifdef MDEC_REPLAY_HAVE_REF
/* The reference decoder's public API under the names mdec_ref_names.h gives
 * it (its own translation unit; nothing here is renamed). */
void ref_mdec_init(void);
void ref_mdec_write(uint32_t addr, uint32_t value);
uint32_t ref_mdec_dma_write_words(const uint32_t *src, uint32_t max_words);
uint32_t ref_mdec_dma_read_words(uint32_t *dst, uint32_t max_words);
int ref_mdec_dma_read_ready(void);
uint32_t ref_mdec_snapshot_bytes(void);
void ref_mdec_snapshot_write(uint8_t *p);
int ref_mdec_snapshot_read(const uint8_t *p, uint32_t len);
uint32_t ref_mdec_mean_qscale_x16(void);
static Decoder s_ref = {"reference", ref_mdec_init, ref_mdec_write,
                        ref_mdec_dma_write_words, ref_mdec_dma_read_words,
                        ref_mdec_dma_read_ready, ref_mdec_snapshot_bytes,
                        ref_mdec_snapshot_write, ref_mdec_snapshot_read,
                        ref_mdec_mean_qscale_x16, {0}, {0}, {0}};
#endif

static Bytes s_words;

static void snapshot_into(Decoder *d, Bytes *b) {
    uint32_t n = d->snap_bytes();
    b->n = 0;
    bytes_room(b, n ? n : 1u);
    if (n) d->snap_write(b->p);
    b->n = n;
}

static int run_event(Decoder *d, const Event *e, Result *r) {
    psx_cycle_count = e->cycle;
    d->out.n = 0;
    switch (e->kind) {
    case EV_CMD: {
        uint32_t nw = (e->nhw + 1u) / 2u, moved = 0;
        uint32_t *w = NULL;
        if (nw) {
            s_words.n = 0;
            w = (uint32_t *)bytes_room(&s_words, (size_t)nw * 4u);
            for (uint32_t i = 0; i < nw; i++) {
                uint32_t hi = 2u * i + 1u < e->nhw ? rd16(e->data + 4u * i + 2u) : 0u;
                w[i] = rd16(e->data + 4u * i) | (hi << 16);
            }
        }
        d->write(MDEC_DATA, e->command);
        if (nw) moved = d->write_words(w, nw);
        if (moved != nw) {
            fprintf(stderr, "mdec_replay: %s decoder took %u of %u input words\n",
                    d->name, moved, nw);
            return -1;
        }
        while (d->read_ready()) {
            uint32_t *dst = (uint32_t *)bytes_room(&d->out, 4096u * 4u);
            uint32_t got = d->read_words(dst, 4096u);
            if (!got) break;
            d->out.n += (size_t)got * 4u;
        }
        break;
    }
    case EV_STATE:
        if (!d->snap_read(e->data, e->blob_len)) {
            fprintf(stderr, "mdec_replay: %s decoder refused a state record\n", d->name);
            return -1;
        }
        break;
    case EV_SAVE:
        snapshot_into(d, &d->saved);
        break;
    case EV_LOAD:
        if (!d->snap_read(d->saved.p, (uint32_t)d->saved.n)) {
            fprintf(stderr, "mdec_replay: %s decoder refused its own snapshot\n", d->name);
            return -1;
        }
        break;
    }
    r->out_bytes = (uint32_t)d->out.n;
    r->out_hash = mdec_fnv1a64(d->out.p, d->out.n);
    snapshot_into(d, &d->snap);
    r->snap_hash = mdec_fnv1a64(d->snap.p, d->snap.n);
    r->q16 = d->mean_q16();
    return 0;
}

/* ---- reports ------------------------------------------------------------- */

static void print_event(uint32_t idx, const Event *e) {
    printf("event %u: %s, cycle %llu", idx, k_ev_name[e->kind],
           (unsigned long long)e->cycle);
    if (e->kind == EV_STATE) printf(", %u-byte snapshot", e->blob_len);
    if (e->kind != EV_CMD) {
        printf("\n");
        return;
    }
    printf(", command %08x, %u halfwords\n", e->command, e->nhw);
    for (uint32_t i = 0; i < e->nhw; i++)
        printf("%s%04x%s", i % 16 ? " " : "  ", rd16(e->data + 2u * i),
               i % 16 == 15 || i + 1 == e->nhw ? "\n" : "");
}

#ifdef MDEC_REPLAY_HAVE_REF
/* Walk the input the way a decode does (leading FE00 padding skipped, the DC
 * word, then run/level words until FE00 or past index 63) and print blocks
 * first..first+count-1. */
static void print_blocks(const Event *e, uint32_t first, uint32_t count) {
    static const char *const color[6] = {"Cr", "Cb", "Y0", "Y1", "Y2", "Y3"};
    int is_color = ((e->command >> 27) & 3u) >= 2u;
    uint32_t pos = 0;
    for (uint32_t b = 0; b < first + count && pos < e->nhw; b++) {
        uint32_t w = rd16(e->data + 2u * pos++), dc, k = 0;
        while (w == MDEC_EOB && pos < e->nhw) w = rd16(e->data + 2u * pos++);
        dc = pos - 1u;
        while (pos < e->nhw && k < 63u) {
            w = rd16(e->data + 2u * pos++);
            if (w == MDEC_EOB) break;
            k += (w >> 10) + 1u;
            if (k >= 64u) break;
        }
        if (b < first) continue;
        printf("    %-3s halfwords %u..%u:", is_color ? color[b % 6u] : "Y",
               dc, pos - 1u);
        for (uint32_t i = dc; i < pos && i < dc + 24u; i++)
            printf(" %04x", rd16(e->data + 2u * i));
        printf("%s\n", pos > dc + 24u ? " ..." : "");
    }
}

static void explain_output(const Event *e, const Bytes *ref, const Bytes *cur) {
    uint32_t depth = (e->command >> 27) & 3u;
    size_t n = ref->n < cur->n ? ref->n : cur->n, off = 0;
    printf("  output: reference %llu bytes, this tree %llu bytes\n",
           (unsigned long long)ref->n, (unsigned long long)cur->n);
    while (off < n && ref->p[off] == cur->p[off]) off++;
    if (off == n) return;
    if (depth >= 2) {
        uint32_t bpp = depth == 2 ? 3u : 2u, mb = (uint32_t)(off / (256u * bpp));
        uint32_t pix = (uint32_t)(off % (256u * bpp)) / bpp, x = pix % 16u, y = pix / 16u;
        size_t at = off - off % bpp;
        printf("  first byte %llu: macroblock %u, pixel (%u,%u) in Y%u, chroma (%u,%u)\n",
               (unsigned long long)off, mb, x, y, (y >= 8u) * 2u + (x >= 8u), x / 2u, y / 2u);
        printf("  reference");
        for (uint32_t i = 0; i < bpp && at + i < ref->n; i++) printf(" %02x", ref->p[at + i]);
        printf(", this tree");
        for (uint32_t i = 0; i < bpp && at + i < cur->n; i++) printf(" %02x", cur->p[at + i]);
        printf("\n  RLE of macroblock %u:\n", mb);
        print_blocks(e, mb * 6u, 6u);
    } else {
        uint32_t blk = (uint32_t)(off / 64u), pix = (uint32_t)(off % 64u);
        printf("  first byte %llu: block %u, pixel (%u,%u): reference %02x, this tree %02x\n",
               (unsigned long long)off, blk, pix % 8u, pix / 8u, ref->p[off], cur->p[off]);
        printf("  RLE of block %u:\n", blk);
        print_blocks(e, blk, 1u);
    }
}

/* Name of the field at byte `off` of the snapshot blob (mdec_snapshot_write). */
static void snap_field(uint32_t off, char *buf, size_t len) {
    static const char *const scalars[16] = {
        "version", "command", "expected_halfwords", "last_status",
        "decode_macroblocks", "decode_blocks", "decode_stop_reason",
        "decode_input_pos", "decode_input_end", "dma_in_words", "dma_out_words",
        "dma_read_underflows", "output_pos", "reserved", "reserved", ""};
    static const char *const flags[8] = {
        "output_bit15", "output_signed", "output_depth", "current_block",
        "busy", "input_full", "enable_dma_in", "enable_dma_out"};
    if (off < 60) snprintf(buf, len, "%s", scalars[off / 4u]);
    else if (off < 68) snprintf(buf, len, "%s", flags[off - 60u]);
    else if (off < 132) snprintf(buf, len, "y_quant[%u]", off - 68u);
    else if (off < 196) snprintf(buf, len, "uv_quant[%u]", off - 132u);
    else if (off < 324) snprintf(buf, len, "scale[%u] (stored order)", (off - 196u) / 2u);
    else if (off < 328) snprintf(buf, len, "input_count");
    else if (off < 332) snprintf(buf, len, "output_size");
    else if (off < 340) snprintf(buf, len, "colour-decode age");
    else snprintf(buf, len, "FIFO contents");
}

static void explain_snapshot(const Bytes *ref, const Bytes *cur) {
    size_t n = ref->n < cur->n ? ref->n : cur->n, off = 0;
    char name[64];
    while (off < n && ref->p[off] == cur->p[off]) off++;
    if (off == n) {
        printf("  snapshot: reference %llu bytes, this tree %llu bytes\n",
               (unsigned long long)ref->n, (unsigned long long)cur->n);
        return;
    }
    snap_field((uint32_t)off, name, sizeof(name));
    printf("  snapshot byte %llu (%s): reference %02x, this tree %02x\n",
           (unsigned long long)off, name, ref->p[off], cur->p[off]);
}
#endif

/* ---- main ---------------------------------------------------------------- */

static int usage(void) {
    fprintf(stderr,
            "usage: mdec_replay (--corpus FILE | --synthetic SEED N) [--list]\n"
            "                   [--expect HASH] [--check-live] [--capture FILE]\n"
            "                   [--show N]\n");
    return 2;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = (uint8_t *)malloc(n ? (size_t)n : 1u);
        if (buf && n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
        *len = (size_t)n;
    }
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    const char *corpus = NULL, *capture = NULL, *expect = NULL;
    int list = 0, check_live = 0, have_show = 0, rc = 0;
    unsigned long long seed = 0, show = 0;
    Source src;
    Event e;
    uint64_t fp = MDEC_FNV64_INIT, out_total = 0;
    uint32_t idx = 0, decodes = 0, live_bad = 0, tables_at = 0;
    int got;

    memset(&src, 0, sizeof(src));
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus = argv[++i];
        else if (!strcmp(argv[i], "--synthetic") && i + 2 < argc) {
            src.synthetic = 1;
            seed = strtoull(argv[++i], NULL, 0);
            src.left = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--check-live")) check_live = 1;
        else if (!strcmp(argv[i], "--expect") && i + 1 < argc) expect = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) {
            have_show = 1;
            show = strtoull(argv[++i], NULL, 0);
        } else return usage();
    }
    if (!!corpus == src.synthetic) return usage();
    if (corpus) {
        src.file = read_file(corpus, &src.len);
        if (!src.file) {
            fprintf(stderr, "mdec_replay: cannot read %s\n", corpus);
            return 2;
        }
        if (src.len < 16 || memcmp(src.file, MDEC_CAPTURE_MAGIC, 8) != 0 ||
            rd32(src.file + 8) == 0 || rd32(src.file + 8) > MDEC_CAPTURE_VERSION) {
            fprintf(stderr, "mdec_replay: %s is not an MDEC capture this tool reads\n", corpus);
            return 2;
        }
        src.pos = 16;
    }
    src.rng = seed * 0x9E3779B97F4A7C15ull + 1u;

    /* Both decoders must stay off any capture the environment asks for. */
#ifdef _WIN32
    _putenv_s("PSX_MDEC_CAPTURE", "");
#else
    unsetenv("PSX_MDEC_CAPTURE");
#endif
    s_cur.init();
#ifdef MDEC_REPLAY_HAVE_REF
    s_ref.init();
#endif
    if (capture && !mdec_capture_start(capture)) return 2;

    while ((got = source_next(&src, &e)) == 1) {
        Result cr;
        if (have_show && idx == show) {
            print_event(idx, &e);
            break;
        }
        if (run_event(&s_cur, &e, &cr) != 0) { rc = 2; break; }
        fp = fnv_add(fp, e.kind, 4);
        fp = fnv_add(fp, e.command, 4);
        fp = fnv_add(fp, cr.out_bytes, 4);
        fp = fnv_add(fp, cr.out_hash, 8);
        fp = fnv_add(fp, cr.snap_hash, 8);
        fp = fnv_add(fp, cr.q16, 4);
        out_total += cr.out_bytes;
        if (e.kind == EV_CMD && (e.command >> 29) == 1u && e.nhw) decodes++;
        if (list)
            printf("%7u %-5s %08x hw=%-6u out=%-7u out_h=%016llx snap_h=%016llx q16=%u\n",
                   idx, k_ev_name[e.kind], e.command, e.nhw, cr.out_bytes,
                   (unsigned long long)cr.out_hash, (unsigned long long)cr.snap_hash,
                   cr.q16);
        if (check_live && e.has_live &&
            (cr.out_bytes != e.live_bytes || cr.out_hash != e.live_hash)) {
            if (!live_bad)
                printf("live mismatch at event %u: recorded %u bytes %016llx, "
                       "replayed %u bytes %016llx\n", idx, e.live_bytes,
                       (unsigned long long)e.live_hash, cr.out_bytes,
                       (unsigned long long)cr.out_hash);
            live_bad++;
        }
#ifdef MDEC_REPLAY_HAVE_REF
        {
            Result rr;
            if (run_event(&s_ref, &e, &rr) != 0) { rc = 2; break; }
            if (rr.out_bytes != cr.out_bytes || rr.out_hash != cr.out_hash ||
                rr.snap_hash != cr.snap_hash || rr.q16 != cr.q16) {
                printf("first difference from the reference at event %u (%s %08x)\n",
                       idx, k_ev_name[e.kind], e.command);
                if (e.kind == EV_CMD && (e.command >> 29) == 1u)
                    printf("  decode: depth %u, signed %u, bit15 %u, %u halfwords; "
                           "tables last set at event %u\n",
                           (e.command >> 27) & 3u, (e.command >> 26) & 1u,
                           (e.command >> 25) & 1u, e.nhw, tables_at);
                if (rr.out_bytes != cr.out_bytes || rr.out_hash != cr.out_hash)
                    explain_output(&e, &s_ref.out, &s_cur.out);
                else if (rr.snap_hash != cr.snap_hash)
                    explain_snapshot(&s_ref.snap, &s_cur.snap);
                if (rr.q16 != cr.q16)
                    printf("  mdec_mean_qscale_x16: reference %u, this tree %u\n",
                           rr.q16, cr.q16);
                printf("  (mdec_replay ... --show %u prints its input)\n", idx);
                rc = 1;
                break;
            }
        }
#endif
        if (e.kind == EV_STATE || e.kind == EV_LOAD ||
            (e.kind == EV_CMD && ((e.command >> 29) == 2u || (e.command >> 29) == 3u)))
            tables_at = idx;
        idx++;
    }
    (void)tables_at;   /* only reported against a reference */
    if (got < 0) rc = 2;
    mdec_capture_stop();

    printf("events=%u decodes=%u output=%llu bytes fingerprint=%016llx\n", idx,
           decodes, (unsigned long long)out_total, (unsigned long long)fp);
    if (check_live)
        printf("live: %u mismatch(es)\n", live_bad);
#ifdef MDEC_REPLAY_HAVE_REF
    if (rc == 0) printf("reference: identical\n");
#endif
    if (rc == 0 && live_bad) rc = 1;
    if (rc == 0 && expect && strtoull(expect, NULL, 16) != fp) {
        printf("fingerprint %016llx, expected %s\n", (unsigned long long)fp, expect);
        rc = 1;
    }
    return rc;
}
