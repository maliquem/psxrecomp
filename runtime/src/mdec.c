#include "mdec.h"
#include "mdec_capture.h"
#include "mdec_core.h"
#include "pst_wire.h"
#include "psx_cycles.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t s_frame_count;

/* FMV-activity detector: guest-cycle stamp of the newest colour (15/24-bit)
 * MDEC decode, read by mdec_recently_active() / mdec_color_age_cycles(). The
 * snapshot carries its age, so peers with identical FIFO/tables but different
 * present rates hash equal. MDEC_NEVER_DECODED = no colour decode yet. */
#define MDEC_NEVER_DECODED ((uint64_t)0 - 1000u)
static uint64_t mdec_last_color_decode_cycle = MDEC_NEVER_DECODED;

/* Data-out FIFO depth: 0x20 words, one 32-word DMA block. */
#define MDEC_OUT_FIFO_BYTES (0x20u * 4u)

enum {
    MDEC_CMD_NOP = 0,
    MDEC_CMD_DECODE = 1,
    MDEC_CMD_SET_QUANT = 2,
    MDEC_CMD_SET_SCALE = 3
};

enum {
    MDEC_EVT_RESET = 1,
    MDEC_EVT_CTRL_WRITE,
    MDEC_EVT_CMD_BEGIN,
    MDEC_EVT_CMD_DONE,
    MDEC_EVT_DECODE_DONE,
    MDEC_EVT_DMA_IN_START,
    MDEC_EVT_DMA_IN_END,
    MDEC_EVT_DMA_OUT_START,
    MDEC_EVT_DMA_OUT_END,
    MDEC_EVT_OUTPUT_DRAINED,
    MDEC_EVT_READ_UNDERFLOW
};

enum {
    MDEC_STOP_NONE = 0,
    MDEC_STOP_INPUT_END = 1,
    MDEC_STOP_CR = 2,
    MDEC_STOP_CB = 3,
    MDEC_STOP_Y0 = 4,
    MDEC_STOP_Y1 = 5,
    MDEC_STOP_Y2 = 6,
    MDEC_STOP_Y3 = 7
};

typedef struct MDECState {
    uint32_t command;
    uint32_t expected_halfwords;
    uint32_t input_count;
    uint16_t *input;
    uint32_t input_cap;

    uint8_t *output;
    uint32_t output_size;
    uint32_t output_pos;
    uint32_t output_cap;

    uint8_t y_quant[64];
    uint8_t uv_quant[64];
    int16_t scale[64];

    uint8_t output_bit15;
    uint8_t output_signed;
    uint8_t output_depth;
    uint8_t current_block;
    uint8_t busy;
    uint8_t input_full;
    uint8_t enable_dma_in;
    uint8_t enable_dma_out;

    uint32_t last_status;
    uint32_t decode_macroblocks;
    uint32_t decode_blocks;
    uint32_t decode_stop_reason;
    uint32_t decode_input_pos;
    uint32_t decode_input_end;
    uint32_t dma_in_words;
    uint32_t dma_out_words;
    uint32_t dma_read_underflows;
} MDECState;

static MDECState mdec;

#define MDEC_TRACE_CAP 4096u
static MDECDebugEvent mdec_trace[MDEC_TRACE_CAP];
static uint64_t mdec_trace_seq;
static uint32_t mdec_trace_head;

static void trace_event(uint32_t kind, uint32_t value) {
#ifdef PSX_NO_DEBUG_TOOLS
    (void)kind;
    (void)value;
    return;
#else
    extern int debug_server_fmv_quiet(void);
    if (debug_server_fmv_quiet()) return;
    MDECDebugEvent *e = &mdec_trace[mdec_trace_head];
    e->seq = mdec_trace_seq++;
    e->frame = (uint32_t)s_frame_count;
    e->kind = kind;
    e->value = value;
    e->command = mdec.command;
    e->input_count = mdec.input_count;
    e->expected_halfwords = mdec.expected_halfwords;
    e->output_size = mdec.output_size;
    e->output_pos = mdec.output_pos;
    e->macroblocks = mdec.decode_macroblocks;
    e->blocks = mdec.decode_blocks;
    e->stop_reason = mdec.decode_stop_reason;
    e->underruns = mdec.dma_read_underflows;
    mdec_trace_head = (mdec_trace_head + 1u) % MDEC_TRACE_CAP;
#endif
}

static int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void clear_output(void) {
    mdec.output_size = 0;
    mdec.output_pos = 0;
}

static int ensure_input_capacity(uint32_t halfwords) {
    if (halfwords <= mdec.input_cap) return 1;
    uint32_t new_cap = mdec.input_cap ? mdec.input_cap : 256u;
    while (new_cap < halfwords) new_cap *= 2u;
    uint16_t *new_input = (uint16_t *)realloc(mdec.input, new_cap * sizeof(uint16_t));
    if (!new_input) return 0;
    mdec.input = new_input;
    mdec.input_cap = new_cap;
    return 1;
}

static int ensure_output_capacity(uint32_t bytes) {
    if (bytes <= mdec.output_cap) return 1;
    uint32_t new_cap = mdec.output_cap ? mdec.output_cap : 4096u;
    while (new_cap < bytes) new_cap *= 2u;
    uint8_t *new_output = (uint8_t *)realloc(mdec.output, new_cap);
    if (!new_output) return 0;
    mdec.output = new_output;
    mdec.output_cap = new_cap;
    return 1;
}

static void append_byte(uint8_t value) {
    if (!ensure_output_capacity(mdec.output_size + 1u)) return;
    mdec.output[mdec.output_size++] = value;
}

/* Reserve `bytes` of output room once (MotK FMV macroblocks write 512/768
 * bytes each). Avoids ensure_output_capacity per channel byte. */
static uint8_t *output_reserve(uint32_t bytes) {
    if (!ensure_output_capacity(mdec.output_size + bytes)) return NULL;
    return mdec.output + mdec.output_size;
}

static uint8_t input_byte(uint32_t byte_index) {
    uint16_t hw = mdec.input[byte_index >> 1];
    return (byte_index & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

/* ---- command capture (format in mdec_capture.h) -------------------------- */
#ifndef PSX_NO_DEBUG_TOOLS
static FILE *s_capture;
static int s_capture_env_checked;

static void capture_failed(void) {
    fprintf(stderr, "[mdec] capture: write failed; capture off\n");
    mdec_capture_stop();
}

/* Record the whole decoder state (snapshot blob): opens the capture and
 * follows mdec_init / a snapshot load, so a replay starts from the guest's
 * tables even when the run began from a savestate. */
static void capture_state(void) {
    uint8_t head[16];
    uint8_t *blob;
    uint32_t n;
    PstW w;
    if (!s_capture) return;
    n = mdec_snapshot_bytes();
    blob = n ? (uint8_t *)malloc(n) : NULL;
    if (!blob) {
        capture_failed();
        return;
    }
    mdec_snapshot_write(blob);
    pst_w_init(&w, head, sizeof(head));
    (void)pst_w_u32(&w, MDEC_CAPTURE_STATE_TAG);
    (void)pst_w_u64(&w, psx_cycle_count);
    (void)pst_w_u32(&w, n);
    if (fwrite(head, 1, sizeof(head), s_capture) != sizeof(head) ||
        fwrite(blob, 1, n, s_capture) != n || fflush(s_capture) != 0)
        capture_failed();
    free(blob);
}

int mdec_capture_start(const char *path) {
    uint8_t hdr[16];
    PstW w;
    mdec_capture_stop();
    s_capture_env_checked = 1;   /* an explicit start wins over the env var */
    if (!path || !path[0]) return 0;
    s_capture = fopen(path, "wb");
    if (!s_capture) {
        fprintf(stderr, "[mdec] capture: cannot open %s\n", path);
        return 0;
    }
    pst_w_init(&w, hdr, sizeof(hdr));
    (void)pst_w_bytes(&w, MDEC_CAPTURE_MAGIC, 8u);
    (void)pst_w_u32(&w, MDEC_CAPTURE_VERSION);
    (void)pst_w_u32(&w, 0u);
    if (fwrite(hdr, 1, sizeof(hdr), s_capture) != sizeof(hdr) ||
        fflush(s_capture) != 0) {
        fprintf(stderr, "[mdec] capture: cannot write %s\n", path);
        mdec_capture_stop();
        return 0;
    }
    capture_state();
    return s_capture != NULL;
}

void mdec_capture_stop(void) {
    if (s_capture) fclose(s_capture);
    s_capture = NULL;
}

/* PSX_MDEC_CAPTURE is looked up once, when the first command begins: by then
 * any savestate the run started from is loaded, and its state opens the file. */
static void capture_poll_env(void) {
    const char *path;
    if (s_capture_env_checked) return;
    s_capture_env_checked = 1;
    path = getenv("PSX_MDEC_CAPTURE");
    if (path && path[0]) (void)mdec_capture_start(path);
}

/* Record the command that just finished. `has_output`: a DECODE that ran, so
 * mdec.output holds exactly its output. */
static void capture_command(int has_output) {
    uint8_t head[20], tail[12];
    PstW w;
    uint32_t out_bytes = has_output ? mdec.output_size : 0u;
    if (!s_capture) return;
    pst_w_init(&w, head, sizeof(head));
    (void)pst_w_u32(&w, MDEC_CAPTURE_RECORD_TAG);
    (void)pst_w_u32(&w, mdec.command);
    (void)pst_w_u64(&w, psx_cycle_count);
    (void)pst_w_u32(&w, mdec.input_count);
    pst_w_init(&w, tail, sizeof(tail));
    (void)pst_w_u32(&w, out_bytes);
    (void)pst_w_u64(&w, mdec_fnv1a64(mdec.output, out_bytes));
    /* Halfwords go out as stored: the host is little-endian, as
     * mdec_dma_read_word already assumes. Flushed per record because a
     * capture run usually ends with the process killed. */
    if (fwrite(head, 1, sizeof(head), s_capture) != sizeof(head) ||
        (mdec.input_count &&
         fwrite(mdec.input, sizeof(uint16_t), mdec.input_count, s_capture) !=
             mdec.input_count) ||
        fwrite(tail, 1, sizeof(tail), s_capture) != sizeof(tail) ||
        fflush(s_capture) != 0)
        capture_failed();
}
#else
int mdec_capture_start(const char *path) { (void)path; return 0; }
void mdec_capture_stop(void) {}
static void capture_state(void) {}
static void capture_poll_env(void) {}
static void capture_command(int has_output) { (void)has_output; }
#endif

static void finish_command(void) {
    mdec.expected_halfwords = 0;
    mdec.input_count = 0;
    mdec.busy = 0;
    mdec.input_full = 0;
    trace_event(MDEC_EVT_CMD_DONE, mdec.command);
}

static void soft_reset(void) {
    uint16_t *input = mdec.input;
    uint32_t input_cap = mdec.input_cap;
    uint8_t *output = mdec.output;
    uint32_t output_cap = mdec.output_cap;
    uint8_t y_quant[64];
    uint8_t uv_quant[64];
    int16_t scale[64];

    memcpy(y_quant, mdec.y_quant, sizeof(y_quant));
    memcpy(uv_quant, mdec.uv_quant, sizeof(uv_quant));
    memcpy(scale, mdec.scale, sizeof(scale));

    memset(&mdec, 0, sizeof(mdec));
    mdec.input = input;
    mdec.input_cap = input_cap;
    mdec.output = output;
    mdec.output_cap = output_cap;
    memcpy(mdec.y_quant, y_quant, sizeof(mdec.y_quant));
    memcpy(mdec.uv_quant, uv_quant, sizeof(mdec.uv_quant));
    memcpy(mdec.scale, scale, sizeof(mdec.scale));
    mdec.output_depth = 3;
    mdec.current_block = 4;
}

/* Mean AC quantiser scale of the last colour decode, x16 (0 = none yet). A
 * read-only statistic for the FMV present stage (its deblocking strength); it
 * changes nothing decoded and is not part of a savestate. */
static uint32_t s_q_sum = 0, s_q_n = 0;
static volatile uint32_t g_mdec_mean_q16 = 0;
uint32_t mdec_mean_qscale_x16(void) { return g_mdec_mean_q16; }

/* One block through the core, counted for the snapshot and the qscale
 * statistic. */
static int decode_block(int16_t *block, const uint8_t *quant,
                        uint32_t *pos, uint32_t end) {
    uint32_t qscale;
    if (!mdec_core_decode_block(block, mdec.input, pos, end, quant,
                                mdec.scale, &qscale))
        return 0;
    mdec.decode_blocks++;
    s_q_sum += qscale;
    s_q_n++;
    return 1;
}

static uint8_t to_output_u8(int value) {
    value = clamp_int(value, -128, 127);
    if (mdec.output_signed) return (uint8_t)(int8_t)value;
    return (uint8_t)(value + 128);
}

static void append_luma_block(const int16_t *yblk) {
    uint8_t *out = output_reserve(64u);
    if (!out) return;
    for (int i = 0; i < 64; i++) out[i] = to_output_u8(yblk[i]);
    mdec.output_size += 64u;
}

static void append_color_macroblock(const int16_t *crblk, const int16_t *cbblk,
                                    const int16_t yblk[4][64]) {
    /* 16×16: 512 bytes @15bpp, 768 @24bpp — reserve once per MB. */
    uint32_t need = (mdec.output_depth == 3) ? 512u : 768u;
    uint8_t *out = output_reserve(need);
    if (!out) return;
    mdec_core_color_macroblock(out, crblk, cbblk, yblk, mdec.output_depth,
                               mdec.output_signed, mdec.output_bit15);
    mdec.output_size += need;
}

/* Monotonic count of decode invocations — the frontend FMV detector samples
 * this per display-frame to tell "MDEC is actively producing frames" (FMV)
 * from idle. */
static volatile uint32_t g_mdec_decode_count = 0;
uint32_t mdec_get_decode_count(void) { return g_mdec_decode_count; }

static void execute_decode(void) {
    uint32_t pos = 0;
    uint32_t end = mdec.input_count;
    g_mdec_decode_count++;
    clear_output();
    s_q_sum = 0; s_q_n = 0;
    mdec.decode_macroblocks = 0;
    mdec.decode_blocks = 0;
    mdec.decode_stop_reason = MDEC_STOP_NONE;
    mdec.decode_input_pos = 0;
    mdec.decode_input_end = end;
    mdec.dma_out_words = 0;
    mdec.dma_read_underflows = 0;

    if (mdec.output_depth < 2) {
        int16_t yblk[64];
        while (pos < end && decode_block(yblk, mdec.y_quant, &pos, end)) {
            append_luma_block(yblk);
            mdec.decode_macroblocks++;
        }
        mdec.decode_stop_reason = (pos >= end) ? MDEC_STOP_INPUT_END : MDEC_STOP_Y0;
        mdec.decode_input_pos = pos;
        trace_event(MDEC_EVT_DECODE_DONE, mdec.output_size);
        return;
    }

    while (pos < end) {
        int16_t crblk[64];
        int16_t cbblk[64];
        int16_t yblk[4][64];
        if (!decode_block(crblk, mdec.uv_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_CR; break; }
        if (!decode_block(cbblk, mdec.uv_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_CB; break; }
        if (!decode_block(yblk[0], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y0; break; }
        if (!decode_block(yblk[1], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y1; break; }
        if (!decode_block(yblk[2], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y2; break; }
        if (!decode_block(yblk[3], mdec.y_quant, &pos, end)) { mdec.decode_stop_reason = MDEC_STOP_Y3; break; }
        append_color_macroblock(crblk, cbblk, yblk);
        mdec.decode_macroblocks++;
    }
    if (pos >= end && mdec.decode_stop_reason == MDEC_STOP_NONE) {
        mdec.decode_stop_reason = MDEC_STOP_INPUT_END;
    }
    mdec.decode_input_pos = pos;
    /* FMV detector: stamp colour (15/24-bit) decodes only — streamed video.
     * The 4/8-bit luma path above is texture decompression, not video. */
    mdec_last_color_decode_cycle = psx_cycle_count;
    if (s_q_n) g_mdec_mean_q16 = (s_q_sum * 16u + s_q_n / 2u) / s_q_n;
    trace_event(MDEC_EVT_DECODE_DONE, mdec.output_size);
}

static void execute_command(void) {
    uint32_t op = mdec.command >> 29;
    if (op == MDEC_CMD_SET_QUANT) {
        for (uint32_t i = 0; i < 64u; i++) {
            mdec.y_quant[i] = input_byte(i);
        }
        if (mdec.command & 1u) {
            for (uint32_t i = 0; i < 64u; i++) {
                mdec.uv_quant[i] = input_byte(64u + i);
            }
        } else {
            memcpy(mdec.uv_quant, mdec.y_quant, sizeof(mdec.uv_quant));
        }
    } else if (op == MDEC_CMD_SET_SCALE) {
        mdec_core_set_scale(mdec.scale, mdec.input);
    } else if (op == MDEC_CMD_DECODE) {
        execute_decode();
    }

    capture_command(op == MDEC_CMD_DECODE);
    finish_command();
}

static void begin_command(uint32_t value) {
    capture_poll_env();
    mdec.command = value;
    mdec.output_bit15 = (uint8_t)((value >> 25) & 1u);
    mdec.output_signed = (uint8_t)((value >> 26) & 1u);
    mdec.output_depth = (uint8_t)((value >> 27) & 3u);
    mdec.current_block = 4;
    mdec.input_count = 0;
    mdec.input_full = 0;
    mdec.busy = 1;

    switch (value >> 29) {
        case MDEC_CMD_DECODE:
            mdec.expected_halfwords = (value & 0xFFFFu) * 2u;
            break;
        case MDEC_CMD_SET_QUANT:
            mdec.expected_halfwords = (value & 1u) ? 64u : 32u;
            break;
        case MDEC_CMD_SET_SCALE:
            mdec.expected_halfwords = 64u;
            break;
        default:
            mdec.expected_halfwords = 0;
            break;
    }

    if (mdec.expected_halfwords == 0) {
        capture_command(0);
        finish_command();
    } else if (!ensure_input_capacity(mdec.expected_halfwords)) {
        finish_command();
    } else {
        trace_event(MDEC_EVT_CMD_BEGIN, value);
    }
}

static void write_data(uint32_t value) {
    if (mdec.busy && mdec.input_count < mdec.expected_halfwords) {
        mdec.input[mdec.input_count++] = (uint16_t)value;
        if (mdec.input_count < mdec.expected_halfwords) {
            mdec.input[mdec.input_count++] = (uint16_t)(value >> 16);
        }
        if (mdec.input_count >= mdec.expected_halfwords) {
            execute_command();
        }
        return;
    }

    begin_command(value);
}

int mdec_recently_active(uint32_t within_frames) {
    /* Guest-cycle hysteresis (not host s_frame_count). Present-skip / Replay
     * leave s_frame_count stale so a single decode looked "recent" forever
     * (false FMV lockstep on rematch; tip episodes into MotK FMV entry). */
    const uint64_t cycles_per_frame = 338688ull;
    const uint64_t window =
        (uint64_t)within_frames * cycles_per_frame + (cycles_per_frame / 2ull);
    if (psx_cycle_count < mdec_last_color_decode_cycle)
        return 0;
    return (psx_cycle_count - mdec_last_color_decode_cycle) <= window;
}

uint64_t mdec_color_age_cycles(void) {
    if (mdec_last_color_decode_cycle == MDEC_NEVER_DECODED)
        return (uint64_t)0 - 1u;
    if (psx_cycle_count < mdec_last_color_decode_cycle)
        return (uint64_t)0 - 1u;
    return psx_cycle_count - mdec_last_color_decode_cycle;
}

void mdec_init(void) {
    memset(&mdec, 0, sizeof(mdec));
    memset(mdec_trace, 0, sizeof(mdec_trace));
    mdec_trace_seq = 0;
    mdec_trace_head = 0;
    /* A rematch resets the guest clock; a stale stamp would make
     * mdec_recently_active lie for the whole next session. */
    mdec_last_color_decode_cycle = MDEC_NEVER_DECODED;
    for (int i = 0; i < 64; i++) {
        mdec.y_quant[i] = 1;
        mdec.uv_quant[i] = 1;
    }
    mdec.output_depth = 3;
    mdec.current_block = 4;
    capture_state();
}

uint32_t mdec_read(uint32_t addr) {
    uint32_t offset = addr & 7u;
    if (offset == 0) {
        return mdec_dma_read_word();
    }

    uint32_t remaining_words = 0;
    if (mdec.busy && mdec.expected_halfwords > mdec.input_count) {
        remaining_words = (mdec.expected_halfwords - mdec.input_count + 1u) / 2u;
    }

    uint32_t status = remaining_words ? ((remaining_words - 1u) & 0xFFFFu) : 0xFFFFu;
    status |= ((uint32_t)mdec.current_block & 7u) << 16;
    status |= ((uint32_t)mdec.output_bit15 & 1u) << 23;
    status |= ((uint32_t)mdec.output_signed & 1u) << 24;
    status |= ((uint32_t)mdec.output_depth & 3u) << 25;
    int write_ready = mdec_dma_write_ready();
    if (!write_ready) status |= 1u << 30;
    if (mdec.enable_dma_out && mdec_dma_read_ready()) status |= 1u << 27;
    if (mdec.enable_dma_in && write_ready) status |= 1u << 28;
    /* Bit 29 is Command Busy (psx-spx: busy receiving or processing
     * parameters). The hardware decodes progressively and stalls while its
     * 32-word data-out FIFO is full, so a decode command stays in progress
     * until all but the last FIFO load of its output has been read. This
     * model decodes a whole command at once, so derive that
     * from the unread output instead of clearing the bit at execute time:
     * libpress DecDCTin spins on this bit before issuing the next frame, and
     * reporting ready early let Breath of Fire IV's player issue the next
     * DecDCTin/DecDCTout while a column DMA was mid-transfer. DecDCTout's
     * CHCR=0 restart then dropped the words already moved, misaligning every
     * later column (vertical stripes + one-macroblock wrap in the FMV). */
    if (mdec.busy || mdec.output_size - mdec.output_pos > MDEC_OUT_FIFO_BYTES)
        status |= 1u << 29;
    if (mdec.output_pos >= mdec.output_size) status |= 1u << 31;
    mdec.last_status = status;
    return status;
}

void mdec_write(uint32_t addr, uint32_t value) {
    uint32_t offset = addr & 7u;
    if (offset == 0) {
        write_data(value);
        return;
    }

    if (value & 0x80000000u) {
        soft_reset();
        trace_event(MDEC_EVT_RESET, value);
    }
    mdec.enable_dma_in = (uint8_t)((value >> 30) & 1u);
    mdec.enable_dma_out = (uint8_t)((value >> 29) & 1u);
    trace_event(MDEC_EVT_CTRL_WRITE, value);
}

void mdec_dma_write_word(uint32_t value) {
    write_data(value);
}

/* Feed up to `max_words` from a contiguous LE word source (DMA ch0). Stops
 * early if the FIFO becomes not-ready after a decode completes mid-burst.
 * Guest bytes + decode triggers match N× mdec_dma_write_word. */
uint32_t mdec_dma_write_words(const uint32_t *src, uint32_t max_words) {
    uint32_t moved = 0;
    while (moved < max_words) {
        if (!mdec_dma_write_ready()) break;
        /* Fast fill while a DECODE/QUANT/SCALE command is collecting input —
         * same halfword packing + single execute_command as write_data. */
        if (mdec.busy && mdec.input_count < mdec.expected_halfwords) {
            uint32_t need_hw = mdec.expected_halfwords - mdec.input_count;
            uint32_t need_words = (need_hw + 1u) / 2u;
            uint32_t n = max_words - moved;
            if (n > need_words) n = need_words;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t value = src[moved++];
                mdec.input[mdec.input_count++] = (uint16_t)value;
                if (mdec.input_count < mdec.expected_halfwords) {
                    mdec.input[mdec.input_count++] = (uint16_t)(value >> 16);
                }
            }
            if (mdec.input_count >= mdec.expected_halfwords) {
                execute_command();
            }
            continue;
        }
        write_data(src[moved++]);
    }
    return moved;
}

uint32_t mdec_dma_read_word(void) {
    uint32_t value = 0;
    uint32_t start_pos = mdec.output_pos;
    uint32_t avail = (start_pos < mdec.output_size)
                   ? (mdec.output_size - start_pos) : 0u;
    if (avail >= 4u) {
        memcpy(&value, mdec.output + start_pos, sizeof(value));
        mdec.output_pos = start_pos + 4u;
    } else {
        for (uint32_t i = 0; i < 4u; i++) {
            if (mdec.output_pos < mdec.output_size) {
                value |= (uint32_t)mdec.output[mdec.output_pos++] << (i * 8u);
            }
        }
    }
    mdec.dma_out_words++;
    if (start_pos >= mdec.output_size) {
        mdec.dma_read_underflows++;
        if (mdec.dma_read_underflows == 1u) {
            trace_event(MDEC_EVT_READ_UNDERFLOW, mdec.dma_out_words);
        }
    }
    if (mdec.output_pos >= mdec.output_size) {
        trace_event(MDEC_EVT_OUTPUT_DRAINED, mdec.output_size);
        clear_output();
    }
    return value;
}

/* Drain up to `max_words` into a contiguous LE destination (DMA ch1). */
uint32_t mdec_dma_read_words(uint32_t *dst, uint32_t max_words) {
    uint32_t moved = 0;
    while (moved < max_words && mdec.output_pos < mdec.output_size) {
        dst[moved] = mdec_dma_read_word();
        moved++;
    }
    return moved;
}

int mdec_dma_write_ready(void) {
    if (mdec.output_pos < mdec.output_size) return 0;
    return !mdec.busy || mdec.input_count < mdec.expected_halfwords;
}

int mdec_dma_read_ready(void) {
    return mdec.output_pos < mdec.output_size;
}

void mdec_debug_get_state(MDECDebugState *out) {
    if (!out) return;
    out->command = mdec.command;
    out->expected_halfwords = mdec.expected_halfwords;
    out->input_count = mdec.input_count;
    out->output_size = mdec.output_size;
    out->output_pos = mdec.output_pos;
    out->output_depth = mdec.output_depth;
    out->output_signed = mdec.output_signed;
    out->output_bit15 = mdec.output_bit15;
    out->busy = mdec.busy;
    out->input_full = mdec.input_full;
    out->enable_dma_in = mdec.enable_dma_in;
    out->enable_dma_out = mdec.enable_dma_out;
    out->last_status = mdec.last_status;
    out->decode_macroblocks = mdec.decode_macroblocks;
    out->decode_blocks = mdec.decode_blocks;
    out->decode_stop_reason = mdec.decode_stop_reason;
    out->decode_input_pos = mdec.decode_input_pos;
    out->decode_input_end = mdec.decode_input_end;
    out->dma_in_words = mdec.dma_in_words;
    out->dma_out_words = mdec.dma_out_words;
    out->dma_read_underflows = mdec.dma_read_underflows;
}

uint64_t mdec_debug_get_event_total(void) {
    return mdec_trace_seq;
}

uint32_t mdec_debug_copy_events(uint64_t seq_lo, uint64_t seq_hi,
                                MDECDebugEvent *out, uint32_t max_count) {
    if (!out || max_count == 0) return 0;
    uint64_t oldest = (mdec_trace_seq > MDEC_TRACE_CAP) ? mdec_trace_seq - MDEC_TRACE_CAP : 0;
    if (seq_lo < oldest) seq_lo = oldest;
    if (seq_hi > mdec_trace_seq) seq_hi = mdec_trace_seq;
    uint32_t n = 0;
    for (uint64_t seq = seq_lo; seq < seq_hi && n < max_count; seq++) {
        out[n++] = mdec_trace[seq % MDEC_TRACE_CAP];
    }
    return n;
}

void mdec_debug_clear(void) {
    memset(mdec_trace, 0, sizeof(mdec_trace));
    mdec_trace_seq = 0;
    mdec_trace_head = 0;
}

void mdec_debug_dma_in_start(uint32_t addr, uint32_t words) {
    (void)addr;
    trace_event(MDEC_EVT_DMA_IN_START, words);
}

void mdec_debug_dma_in_end(uint32_t addr, uint32_t words) {
    (void)addr;
    mdec.dma_in_words += words;
    trace_event(MDEC_EVT_DMA_IN_END, words);
}

void mdec_debug_dma_out_start(uint32_t addr, uint32_t words) {
    (void)addr;
    trace_event(MDEC_EVT_DMA_OUT_START, words);
}

void mdec_debug_dma_out_end(uint32_t addr, uint32_t words) {
    (void)addr;
    trace_event(MDEC_EVT_DMA_OUT_END, words);
}

/* ---- boot_state snapshot (variable-length input/output FIFOs) ------------ */
/* v2: the colour-decode age is MDEC_SNAP_AGE_NEVER when nothing was decoded.
 * v1 wrote age 1000 for that case, which a load turned into "decoded 1000
 * cycles ago" -- a false FMV for the next ~8 frames after every snapshot load
 * made before the first video (issue #475). v1 payloads still load. */
#define MDEC_SNAP_VER 2u
#define MDEC_SNAP_VER_V1 1u
#define MDEC_SNAP_AGE_NEVER UINT64_MAX
#define MDEC_SNAP_V1_AGE_NEVER 1000ull
#define MDEC_SNAP_INPUT_MAX  (4u * 1024u * 1024u) /* halfwords */
#define MDEC_SNAP_OUTPUT_MAX (8u * 1024u * 1024u) /* bytes */

static uint32_t mdec_snap_fixed_bytes(void) {
    /* ver + scalars + tables + counts + last_color_age (guest cycles) */
    return 4u + /* ver */
           4u * 14u + /* u32 scalars */
           1u * 8u +  /* u8 flags */
           64u + 64u + /* y/uv quant */
           64u * 2u + /* scale i16 */
           4u + 4u + /* input_count, output_size */
           8u;       /* last_color_age in guest cycles */
}

uint32_t mdec_snapshot_bytes(void) {
    uint64_t n = (uint64_t)mdec_snap_fixed_bytes() +
                 (uint64_t)mdec.input_count * 2u +
                 (uint64_t)mdec.output_size;
    if (n > 0xffffffffu) return 0;
    return (uint32_t)n;
}

void mdec_snapshot_write(uint8_t *p) {
    PstW w;
    uint32_t n = mdec_snapshot_bytes();
    uint64_t age;
    if (!p || n == 0) return;
    pst_w_init(&w, p, n);
    (void)pst_w_u32(&w, MDEC_SNAP_VER);
    (void)pst_w_u32(&w, mdec.command);
    (void)pst_w_u32(&w, mdec.expected_halfwords);
    (void)pst_w_u32(&w, mdec.last_status);
    (void)pst_w_u32(&w, mdec.decode_macroblocks);
    (void)pst_w_u32(&w, mdec.decode_blocks);
    (void)pst_w_u32(&w, mdec.decode_stop_reason);
    (void)pst_w_u32(&w, mdec.decode_input_pos);
    (void)pst_w_u32(&w, mdec.decode_input_end);
    (void)pst_w_u32(&w, mdec.dma_in_words);
    (void)pst_w_u32(&w, mdec.dma_out_words);
    (void)pst_w_u32(&w, mdec.dma_read_underflows);
    (void)pst_w_u32(&w, mdec.output_pos);
    (void)pst_w_u32(&w, 0u); /* reserved */
    (void)pst_w_u32(&w, 0u); /* reserved */
    (void)pst_w_u8(&w, mdec.output_bit15);
    (void)pst_w_u8(&w, mdec.output_signed);
    (void)pst_w_u8(&w, mdec.output_depth);
    (void)pst_w_u8(&w, mdec.current_block);
    (void)pst_w_u8(&w, mdec.busy);
    (void)pst_w_u8(&w, mdec.input_full);
    (void)pst_w_u8(&w, mdec.enable_dma_in);
    (void)pst_w_u8(&w, mdec.enable_dma_out);
    (void)pst_w_bytes(&w, mdec.y_quant, 64u);
    (void)pst_w_bytes(&w, mdec.uv_quant, 64u);
    for (int i = 0; i < 64; i++)
        (void)pst_w_i16(&w, mdec.scale[i]);
    (void)pst_w_u32(&w, mdec.input_count);
    (void)pst_w_u32(&w, mdec.output_size);
    /* Guest-cycle age (not host s_frame_count) — netplay aux digests this blob. */
    if (mdec_last_color_decode_cycle == MDEC_NEVER_DECODED ||
        psx_cycle_count < mdec_last_color_decode_cycle)
        age = MDEC_SNAP_AGE_NEVER;
    else
        age = psx_cycle_count - mdec_last_color_decode_cycle;
    (void)pst_w_u64(&w, age);
    for (uint32_t i = 0; i < mdec.input_count; i++)
        (void)pst_w_u16(&w, mdec.input ? mdec.input[i] : 0u);
    if (mdec.output_size && mdec.output)
        (void)pst_w_bytes(&w, mdec.output, mdec.output_size);
}

/* Wire offsets of the fields mdec_snapshot_read gates on (layout above): ver
 * first, output_pos the 12th scalar, then the two counts and the u64 age that
 * close the fixed part. */
#define MDEC_SNAP_OFF_OUTPUT_POS (4u + 11u * 4u)
#define MDEC_SNAP_OFF_COUNTS     (mdec_snap_fixed_bytes() - 16u)

static uint32_t mdec_wire_u32(const uint8_t *p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1u] << 8) |
           ((uint32_t)p[off + 2u] << 16) | ((uint32_t)p[off + 3u] << 24);
}

/* Non-mutating pre-check for boot_state's two-pass load. mdec_snapshot_read
 * runs it first, so the two cannot disagree: a section that passes here
 * cannot fail the reader short of the FIFO allocation itself failing. */
int mdec_snapshot_validate(const uint8_t *p, uint32_t len) {
    uint32_t input_count, output_size;
    if (!p || len < mdec_snap_fixed_bytes()) return 0;
    if (mdec_wire_u32(p, 0u) != MDEC_SNAP_VER &&
        mdec_wire_u32(p, 0u) != MDEC_SNAP_VER_V1) return 0;
    input_count = mdec_wire_u32(p, MDEC_SNAP_OFF_COUNTS);
    output_size = mdec_wire_u32(p, MDEC_SNAP_OFF_COUNTS + 4u);
    if (input_count > MDEC_SNAP_INPUT_MAX || output_size > MDEC_SNAP_OUTPUT_MAX)
        return 0;
    if (mdec_wire_u32(p, MDEC_SNAP_OFF_OUTPUT_POS) > output_size) return 0;
    return (uint64_t)len - mdec_snap_fixed_bytes() >=
           (uint64_t)input_count * 2u + output_size;
}

int mdec_snapshot_read(const uint8_t *p, uint32_t len) {
    PstR r;
    uint32_t ver = 0, input_count = 0, output_size = 0, reserved;
    uint64_t age = MDEC_SNAP_AGE_NEVER;
    int16_t s16;
    if (!mdec_snapshot_validate(p, len)) return 0;
    pst_r_init(&r, p, len);
    if (!pst_r_u32(&r, &ver) ||
        (ver != MDEC_SNAP_VER && ver != MDEC_SNAP_VER_V1)) return 0;
    if (!pst_r_u32(&r, &mdec.command) ||
        !pst_r_u32(&r, &mdec.expected_halfwords) ||
        !pst_r_u32(&r, &mdec.last_status) ||
        !pst_r_u32(&r, &mdec.decode_macroblocks) ||
        !pst_r_u32(&r, &mdec.decode_blocks) ||
        !pst_r_u32(&r, &mdec.decode_stop_reason) ||
        !pst_r_u32(&r, &mdec.decode_input_pos) ||
        !pst_r_u32(&r, &mdec.decode_input_end) ||
        !pst_r_u32(&r, &mdec.dma_in_words) ||
        !pst_r_u32(&r, &mdec.dma_out_words) ||
        !pst_r_u32(&r, &mdec.dma_read_underflows) ||
        !pst_r_u32(&r, &mdec.output_pos) ||
        !pst_r_u32(&r, &reserved) ||
        !pst_r_u32(&r, &reserved))
        return 0;
    if (!pst_r_u8(&r, &mdec.output_bit15) ||
        !pst_r_u8(&r, &mdec.output_signed) ||
        !pst_r_u8(&r, &mdec.output_depth) ||
        !pst_r_u8(&r, &mdec.current_block) ||
        !pst_r_u8(&r, &mdec.busy) ||
        !pst_r_u8(&r, &mdec.input_full) ||
        !pst_r_u8(&r, &mdec.enable_dma_in) ||
        !pst_r_u8(&r, &mdec.enable_dma_out))
        return 0;
    if (!pst_r_bytes(&r, mdec.y_quant, 64u) ||
        !pst_r_bytes(&r, mdec.uv_quant, 64u))
        return 0;
    for (int i = 0; i < 64; i++) {
        if (!pst_r_i16(&r, &s16)) return 0;
        mdec.scale[i] = s16;
    }
    if (!pst_r_u32(&r, &input_count) || !pst_r_u32(&r, &output_size) ||
        !pst_r_u64(&r, &age))
        return 0;
    if (input_count > MDEC_SNAP_INPUT_MAX || output_size > MDEC_SNAP_OUTPUT_MAX)
        return 0;
    if (mdec.output_pos > output_size) return 0;
    if ((size_t)(r.end - r.p) <
        (size_t)input_count * 2u + (size_t)output_size)
        return 0;
    if (!ensure_input_capacity(input_count ? input_count : 1u)) return 0;
    if (!ensure_output_capacity(output_size ? output_size : 1u)) return 0;
    mdec.input_count = input_count;
    mdec.output_size = output_size;
    for (uint32_t i = 0; i < input_count; i++) {
        uint16_t hw;
        if (!pst_r_u16(&r, &hw)) return 0;
        mdec.input[i] = hw;
    }
    if (output_size && !pst_r_bytes(&r, mdec.output, output_size))
        return 0;
    /* Age is guest cycles since the last colour decode. "Never" stays never:
     * restoring it as a recent decode reports FMV that is not playing. A decode
     * older than the restored clock's origin cannot be recent either. */
    if (ver == MDEC_SNAP_VER_V1 && age == MDEC_SNAP_V1_AGE_NEVER)
        age = MDEC_SNAP_AGE_NEVER;
    if (age == MDEC_SNAP_AGE_NEVER || age > psx_cycle_count)
        mdec_last_color_decode_cycle = MDEC_NEVER_DECODED;
    else
        mdec_last_color_decode_cycle = psx_cycle_count - age;
    capture_state();
    return 1;
}
