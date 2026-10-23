/*
 * MDEC command capture (mdec_capture.h): every finished command lands in the
 * file with its command word, input halfwords and output hash, and turning the
 * capture on changes nothing the decoder outputs.
 *
 * The capture feeds an offline replay that compares decoder builds byte for
 * byte, so a record that drifts from what the guest actually sent would make
 * that comparison prove nothing.
 *
 * Build/run: ctest -R mdec_capture_test
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
#define MAX_WORDS 64u
#define MAX_OUT   1024u

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

typedef struct Cmd {
    uint32_t command;
    uint32_t words[MAX_WORDS];
    uint32_t nwords;
    uint8_t out[MAX_OUT];
    uint32_t out_bytes;
} Cmd;

/* Send one command and drain whatever it decodes. */
static void run_cmd(Cmd *c) {
    uint32_t out_words[MAX_OUT / 4u];
    uint32_t n;
    mdec_write(MDEC_DATA, c->command);
    if (c->nwords) mdec_dma_write_words(c->words, c->nwords);
    n = mdec_dma_read_words(out_words, MAX_OUT / 4u);
    c->out_bytes = n * 4u;
    memcpy(c->out, out_words, c->out_bytes);
}

/* One colour macroblock: six blocks (Cr, Cb, Y0..Y3), each a DC word in the
 * low half and the 0xFE00 end code in the high half. */
static void make_decode(Cmd *c, uint32_t flags) {
    static const int16_t dc[6] = {40, -60, 100, -100, 7, 0};
    c->command = 0x20000000u | flags | 6u;
    c->nwords = 6u;
    for (uint32_t i = 0; i < 6u; i++)
        c->words[i] = 0xFE000000u | (8u << 10) | ((uint32_t)dc[i] & 0x3FFu);
}

static void make_commands(Cmd cmds[5]) {
    memset(cmds, 0, 5 * sizeof(Cmd));
    /* SET_QUANT, luma + chroma: 128 bytes = 32 words. */
    cmds[0].command = 0x40000001u;
    cmds[0].nwords = 32u;
    for (uint32_t i = 0; i < 32u; i++) {
        uint32_t b = i * 4u;
        cmds[0].words[i] = (b + 1u) | ((b + 2u) << 8) | ((b + 3u) << 16) |
                           ((b + 4u) << 24);
    }
    /* SET_SCALE: 64 signed halfwords = 32 words, both signs present. */
    cmds[1].command = 0x60000000u;
    cmds[1].nwords = 32u;
    for (uint32_t i = 0; i < 32u; i++) {
        uint16_t lo = (uint16_t)(0x5A82 - (int)i * 713);
        uint16_t hi = (uint16_t)(-0x3B21 + (int)i * 389);
        cmds[1].words[i] = lo | ((uint32_t)hi << 16);
    }
    cmds[2].command = 0x00000000u;           /* NOP: takes no input */
    make_decode(&cmds[3], 0x10000000u);      /* 24bpp, unsigned */
    make_decode(&cmds[4], 0x1E000000u);      /* 15bpp, signed, bit15 */
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc(n > 0 ? (size_t)n : 1u);
    if (buf && n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)(n > 0 ? n : 0);
    return buf;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* Parse `path` and check it holds exactly `cmds[0..n)` in order. */
static void check_file(const char *path, const Cmd *cmds, uint32_t n) {
    size_t len = 0, pos = 16;
    uint8_t *f = read_file(path, &len);
    CHECK(f != NULL, "capture file %s unreadable", path);
    if (!f) return;
    CHECK(len >= 16 && memcmp(f, MDEC_CAPTURE_MAGIC, 8) == 0, "bad magic");
    CHECK(len >= 16 && rd32(f + 8) == MDEC_CAPTURE_VERSION, "bad version");
    for (uint32_t r = 0; r < n && failures == 0; r++) {
        const Cmd *c = &cmds[r];
        int is_decode = (c->command >> 29) == 1u;
        uint32_t nhw, out_bytes;
        CHECK(pos + 20 <= len, "record %u truncated", r);
        if (pos + 20 > len) break;
        CHECK(rd32(f + pos) == MDEC_CAPTURE_RECORD_TAG, "record %u tag", r);
        CHECK(rd32(f + pos + 4) == c->command, "record %u command %08x",
              r, rd32(f + pos + 4));
        nhw = rd32(f + pos + 16);
        CHECK(nhw == c->nwords * 2u, "record %u has %u halfwords", r, nhw);
        pos += 20;
        if (nhw != c->nwords * 2u || pos + 2u * nhw + 12u > len) break;
        for (uint32_t i = 0; i < c->nwords; i++) {
            uint32_t got = (uint32_t)f[pos + 4 * i] |
                           ((uint32_t)f[pos + 4 * i + 1] << 8) |
                           ((uint32_t)f[pos + 4 * i + 2] << 16) |
                           ((uint32_t)f[pos + 4 * i + 3] << 24);
            CHECK(got == c->words[i], "record %u word %u", r, i);
        }
        pos += 2u * nhw;
        out_bytes = rd32(f + pos);
        CHECK(out_bytes == (is_decode ? c->out_bytes : 0u),
              "record %u output %u bytes", r, out_bytes);
        CHECK(rd64(f + pos + 4) ==
                  mdec_fnv1a64(c->out, is_decode ? c->out_bytes : 0u),
              "record %u output hash", r);
        pos += 12;
    }
    CHECK(failures != 0 || pos == len, "%u trailing bytes", (unsigned)(len - pos));
    free(f);
}

static void run_all(Cmd cmds[5]) {
    mdec_init();
    make_commands(cmds);
    for (int i = 0; i < 5; i++) run_cmd(&cmds[i]);
}

static void check_same_output(const Cmd *a, const Cmd *b, const char *what) {
    for (int i = 0; i < 5; i++)
        CHECK(a[i].out_bytes == b[i].out_bytes &&
                  memcmp(a[i].out, b[i].out, b[i].out_bytes) == 0,
              "command %d decodes differently %s", i, what);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char env_path[512], api_path[512];
    Cmd by_env[5], plain[5], by_api[5];

    snprintf(env_path, sizeof(env_path), "%s/mdec_capture_env.bin", dir);
    snprintf(api_path, sizeof(api_path), "%s/mdec_capture_api.bin", dir);

    /* PSX_MDEC_CAPTURE is read at the first finished command, so it has to
     * be set before any. */
#ifdef _WIN32
    _putenv_s("PSX_MDEC_CAPTURE", env_path);
#else
    setenv("PSX_MDEC_CAPTURE", env_path, 1);
#endif
    run_all(by_env);
    mdec_capture_stop();
    check_file(env_path, by_env, 5);

    /* Capture off: the reference outputs. */
    run_all(plain);
    CHECK(plain[3].out_bytes == 768u, "24bpp macroblock is %u bytes",
          plain[3].out_bytes);
    CHECK(plain[4].out_bytes == 512u, "15bpp macroblock is %u bytes",
          plain[4].out_bytes);
    check_same_output(by_env, plain, "with the env capture on");

    /* mdec_capture_start() over the same commands. */
    CHECK(mdec_capture_start(api_path) == 1, "cannot capture to %s", api_path);
    run_all(by_api);
    mdec_capture_stop();
    check_same_output(by_api, plain, "with the capture started by call");
    check_file(api_path, by_api, 5);

    if (failures) {
        fprintf(stderr, "mdec_capture_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("mdec_capture_test: ok\n");
    return 0;
}
