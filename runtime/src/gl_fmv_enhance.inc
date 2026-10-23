/* FMV enhancement ([video] fmv_enhance, on by default): the 24-bit (MDEC)
 * movie frames shown through the CPU present path are cleaned and upscaled on
 * the GPU before they reach the window. Display-only: guest RAM, VRAM and the
 * decoder's output are never touched, so savestates, netplay and screenshots
 * of VRAM stay byte-exact.
 *
 *   1. chroma   MDEC keeps one Cb/Cr pair per 2x2 pixels and repeats it; each
 *               block's pair is recovered from its four pixels and
 *               interpolated between block centres (MPEG-1 siting), across
 *               macroblock edges too. Luma is kept per pixel.
 *   2. deblock  horizontal, then vertical: at the 8x8 block edges a small step
 *               between two flat sides is spread over three pixels each side.
 *               The threshold follows the movie's quantiser (MDEC qscale of
 *               the last decode); real edges, larger than it, are kept.
 *   3. EASU     FidelityFX Super Resolution 1, edge-adaptive upscale to the
 *               window rect.
 *   4. RCAS     FSR1 contrast-adaptive sharpening, at the window size.
 *   5. present  the usual present program, 1:1 (gamma, scanlines).
 *
 * The 8x8 grid's phase is measured on the frame itself (where the luma steps
 * line up), so a movie placed at any offset in the display is handled.
 *
 * EASU and RCAS below are ported from AMD FidelityFX FSR1 (ffx_fsr1.h,
 * github.com/GPUOpen-Effects/FidelityFX-FSR), under its MIT licence:
 *
 *   Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit
 *   persons to whom the Software is furnished to do so, subject to the
 *   following conditions: The above copyright notice and this permission
 *   notice shall be included in all copies or substantial portions of the
 *   Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 *   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 *   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 *   USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The chroma, deblocking and phase detection are this project's own. */
#include "mdec.h"

#define FE_RGBA16F 0x881A

static int      s_fe_on = 1;               /* [video] fmv_enhance */
static int      s_fe_state = 0;            /* 0 not built, 1 ready, -1 failed */
static GLuint   s_fe_prog[4];              /* chroma, deblock, easu, rcas */
static GLuint   s_fe_fbo = 0;
static GLuint   s_fe_tex[4];               /* src-size A, B; out-size C, D */
static int      s_fe_tw[4], s_fe_th[4];
static int      s_fe_phase[2] = {0, 0};    /* 8x8 grid phase, x and y */
static uint64_t s_fe_frames = 0;
/* Debug (TCP fmv_enhance only, not a setting): which stages run, and a
 * request to render the next movie frame through several stage sets and save
 * each as a BMP, for side-by-side checks on one identical source frame. */
enum { FE_CHROMA = 1, FE_DEBLOCK = 2, FE_RCAS = 4, FE_EASU = 8, FE_ALL = 15 };
static int      s_fe_mask = FE_ALL;
static char     s_fe_dump[256];

/* RCAS strength in stops (0 = strongest). 0.5 leaves the deblocked areas calm
 * while lines still gain definition. */
#define FE_RCAS_STOPS 0.5f

#define FE_FS_HEAD \
    "#version 330\n" \
    "uniform sampler2D u_src; uniform ivec2 u_size; out vec4 frag;\n" \
    "vec3 px(ivec2 p){ return texelFetch(u_src, clamp(p, ivec2(0), u_size - 1), 0).rgb; }\n"

static const char *FE_CHROMA_FS =
    FE_FS_HEAD
    "uniform ivec2 u_cphase;\n"
    "const vec3 KY = vec3(0.299, 0.587, 0.114);\n"
    "vec2 cb_cr(vec3 c){ float y = dot(c, KY); return vec2((c.b - y) * 0.564334, (c.r - y) * 0.713267); }\n"
    "vec2 blk(ivec2 b){\n"
    "  ivec2 p = b * 2 + u_cphase;\n"
    "  return 0.25 * (cb_cr(px(p)) + cb_cr(px(p + ivec2(1, 0))) + cb_cr(px(p + ivec2(0, 1))) + cb_cr(px(p + ivec2(1, 1))));\n"
    "}\n"
    "void main(){\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    /* block b's centre sits at pixel 2b + 0.5 (+ phase) */
    "  vec2 u = (vec2(p - u_cphase) - 0.5) * 0.5;\n"
    "  ivec2 b0 = ivec2(floor(u)); vec2 f = u - vec2(b0);\n"
    "  ivec2 lo = -u_cphase, hi = (u_size - 1 - u_cphase) / 2;\n"
    "  ivec2 b1 = clamp(b0 + 1, lo, hi); b0 = clamp(b0, lo, hi);\n"
    "  vec2 cc = mix(mix(blk(b0), blk(ivec2(b1.x, b0.y)), f.x), mix(blk(ivec2(b0.x, b1.y)), blk(b1), f.x), f.y);\n"
    "  float y = dot(px(p), KY);\n"
    "  frag = vec4(clamp(vec3(y + 1.402 * cc.y, y - 0.344136 * cc.x - 0.714136 * cc.y, y + 1.772 * cc.x), 0.0, 1.0), 1.0);\n"
    "}\n";

static const char *FE_DEBLOCK_FS =
    FE_FS_HEAD
    "uniform ivec2 u_dir; uniform int u_phase; uniform vec2 u_thr;   /* alpha, beta */\n"
    "float mx(vec3 v){ return max(max(v.r, v.g), v.b); }\n"
    "void main(){\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec3 c = px(p);\n"
    "  int pos = u_dir.x != 0 ? p.x : p.y, len = u_dir.x != 0 ? u_size.x : u_size.y;\n"
    "  int m = ((pos - u_phase) % 8 + 8) % 8;   /* 0: a block's first pixel */\n"
    "  int q0;\n"
    "  if (m <= 2) q0 = pos - m; else if (m >= 5) q0 = pos + 8 - m; else { frag = vec4(c, 1.0); return; }\n"
    "  if (q0 < 2 || q0 > len - 2) { frag = vec4(c, 1.0); return; }\n"
    "  ivec2 Q = p + u_dir * (q0 - pos);\n"
    "  vec3 P0 = px(Q - u_dir), P1 = px(Q - 2 * u_dir), Q0 = px(Q), Q1 = px(Q + u_dir);\n"
    "  float st = mx(abs(Q0 - P0)), fl = max(mx(abs(P1 - P0)), mx(abs(Q1 - Q0)));\n"
    "  float k = (1.0 - smoothstep(0.6 * u_thr.x, u_thr.x, st)) * (1.0 - smoothstep(0.6 * u_thr.y, u_thr.y, fl));\n"
    /* the step replaced by a ramp over 6 pixels: d is the distance from the
     * edge (p0 -0.5, q0 +0.5) */
    "  float d = float(pos - q0) + 0.5;\n"
    "  float r = clamp(0.5 + d / 6.0, 0.0, 1.0) - (d > 0.0 ? 1.0 : 0.0);\n"
    "  frag = vec4(clamp(c + (Q0 - P0) * r * k, 0.0, 1.0), 1.0);\n"
    "}\n";

/* FSR1 EASU, 32-bit path (FsrEasuF), with texel fetches for the gathers. */
static const char *FE_EASU_FS =
    FE_FS_HEAD
    "uniform vec4 u_con0;   /* input/output scale, and its half-pixel offset */\n"
    "uniform int u_nearest;  /* debug: plain pixel replication instead */\n"
    "float L(vec3 c){ return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
    "float rcpLo(float x){ return 1.0 / max(x, 1.0 / 32768.0); }\n"
    "void tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c){\n"
    "  vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * (-dir.y) + off.y * dir.x);\n"
    "  v *= len;\n"
    "  float d2 = min(dot(v, v), clp);\n"
    "  float wB = 0.4 * d2 - 1.0, wA = lob * d2 - 1.0;\n"
    "  wB *= wB; wA *= wA;\n"
    "  wB = 1.5625 * wB - 0.5625;\n"
    "  float w = wB * wA;\n"
    "  aC += c * w; aW += w;\n"
    "}\n"
    "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE){\n"
    "  float lenX = rcpLo(max(abs(lD - lC), abs(lC - lB)));\n"
    "  float dirX = lD - lB;\n"
    "  dir.x += dirX * w;\n"
    "  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0); lenX *= lenX;\n"
    "  len += lenX * w;\n"
    "  float lenY = rcpLo(max(abs(lE - lC), abs(lC - lA)));\n"
    "  float dirY = lE - lA;\n"
    "  dir.y += dirY * w;\n"
    "  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0); lenY *= lenY;\n"
    "  len += lenY * w;\n"
    "}\n"
    "void main(){\n"
    "  if (u_nearest != 0) { frag = vec4(px(ivec2(floor(gl_FragCoord.xy * u_con0.xy))), 1.0); return; }\n"
    "  vec2 pp = floor(gl_FragCoord.xy) * u_con0.xy + u_con0.zw;\n"
    "  vec2 fp = floor(pp); pp -= fp;\n"
    "  ivec2 F = ivec2(fp);\n"
    "  vec3 b = px(F + ivec2(0, -1)), c = px(F + ivec2(1, -1));\n"
    "  vec3 e = px(F + ivec2(-1, 0)), f = px(F), g = px(F + ivec2(1, 0)), h = px(F + ivec2(2, 0));\n"
    "  vec3 i = px(F + ivec2(-1, 1)), j = px(F + ivec2(0, 1)), k = px(F + ivec2(1, 1)), l = px(F + ivec2(2, 1));\n"
    "  vec3 n = px(F + ivec2(0, 2)), o = px(F + ivec2(1, 2));\n"
    "  float bL = L(b), cL = L(c), eL = L(e), fL = L(f), gL = L(g), hL = L(h);\n"
    "  float iL = L(i), jL = L(j), kL = L(k), lL = L(l), nL = L(n), oL = L(o);\n"
    "  vec2 dir = vec2(0.0); float len = 0.0;\n"
    "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
    "  setf(dir, len, pp.x * (1.0 - pp.y),         cL, fL, gL, hL, kL);\n"
    "  setf(dir, len, (1.0 - pp.x) * pp.y,         fL, iL, jL, kL, nL);\n"
    "  setf(dir, len, pp.x * pp.y,                 gL, jL, kL, lL, oL);\n"
    "  vec2 dir2 = dir * dir;\n"
    "  float dirR = dir2.x + dir2.y;\n"
    "  bool zro = dirR < 1.0 / 32768.0;\n"
    "  dirR = zro ? 1.0 : inversesqrt(dirR);\n"
    "  dir.x = zro ? 1.0 : dir.x;\n"
    "  dir *= dirR;\n"
    "  len = len * 0.5; len *= len;\n"
    "  float stretch = dot(dir, dir) * rcpLo(max(abs(dir.x), abs(dir.y)));\n"
    "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;\n"
    "  float clp = rcpLo(lob);\n"
    "  vec3 mn4 = min(min(f, g), min(j, k)), mx4 = max(max(f, g), max(j, k));\n"
    "  vec3 aC = vec3(0.0); float aW = 0.0;\n"
    "  tap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);\n"
    "  tap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);\n"
    "  tap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);\n"
    "  tap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);\n"
    "  tap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);\n"
    "  tap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);\n"
    "  tap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);\n"
    "  tap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);\n"
    "  tap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);\n"
    "  tap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);\n"
    "  tap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);\n"
    "  tap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);\n"
    "  vec3 pix = min(mx4, max(mn4, aC / aW));\n"
    /* Luma only (this project's change): MDEC chroma is half resolution and
     * EASU's negative lobe rings on its 2x2 steps, so the colour comes from
     * the plain bilinear sample and EASU's result gives the luma. */
    "  const vec3 KY = vec3(0.299, 0.587, 0.114);\n"
    "  vec3 bil = mix(mix(f, g, pp.x), mix(j, k, pp.x), pp.y);\n"
    "  frag = vec4(clamp(bil + (dot(pix, KY) - dot(bil, KY)), 0.0, 1.0), 1.0);\n"
    "}\n";

/* FSR1 RCAS, 32-bit path (FsrRcasF), no denoise, no alpha. */
static const char *FE_RCAS_FS =
    FE_FS_HEAD
    "uniform float u_sharp;   /* exp2(-stops) */\n"
    "void main(){\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec3 b = px(p + ivec2(0, -1)), d = px(p + ivec2(-1, 0)), e = px(p);\n"
    "  vec3 f = px(p + ivec2(1, 0)), h = px(p + ivec2(0, 1));\n"
    "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
    "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0e-5));\n"
    "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0e-5));\n"
    "  vec3 lobeC = max(-hitMin, hitMax);\n"
    "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(max(lobeC.r, lobeC.g), lobeC.b), 0.0)) * u_sharp;\n"
    "  vec3 pix = (lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0);\n"
    /* luma only (this project's change, as in EASU above) */
    "  const vec3 KY = vec3(0.299, 0.587, 0.114);\n"
    "  frag = vec4(clamp(e + (dot(pix, KY) - dot(e, KY)), 0.0, 1.0), 1.0);\n"
    "}\n";

static int fe_build(void) {
    const char *fs[4] = {FE_CHROMA_FS, FE_DEBLOCK_FS, FE_EASU_FS, FE_RCAS_FS};
    for (int i = 0; i < 4; i++) {
        s_fe_prog[i] = build_program(PRESENT_VS, fs[i]);
        if (!s_fe_prog[i]) return -1;
    }
    p_glGenFramebuffers(1, &s_fe_fbo);
    glGenTextures(4, s_fe_tex);
    return s_fe_fbo ? 1 : -1;
}

/* The context went away (its objects with it): rebuilt on the next frame. */
static void fe_release(void) {
    s_fe_state = 0; s_fe_fbo = 0;
    for (int i = 0; i < 4; i++) { s_fe_prog[i] = 0; s_fe_tex[i] = 0; s_fe_tw[i] = s_fe_th[i] = 0; }
}

static void fe_target(int i, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, s_fe_tex[i]);
    if (s_fe_tw[i] != w || s_fe_th[i] != h) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, FE_RGBA16F, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        s_fe_tw[i] = w; s_fe_th[i] = h;
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_fe_fbo);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fe_tex[i], 0);
    glViewport(0, 0, w, h);
}

/* One full-target pass of program `prog` reading texture `src` (sw x sh). */
static void fe_pass(GLuint prog, GLuint src, int sw, int sh) {
    p_glUseProgram(prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    p_glUniform1i(p_glGetUniformLocation(prog, "u_src"), 0);
    p_glUniform2i(p_glGetUniformLocation(prog, "u_size"), sw, sh);
    p_glUniform4f(p_glGetUniformLocation(prog, "u_uv_rect"), 0.f, 0.f, 1.f, 1.f);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* The 8x8 grid's phase along x and y: at each boundary position the step
 * between two neighbours beyond the steps on either side of it (blocking),
 * counted in flat areas only (real edges would drown it), summed by position
 * mod 8. Kept from the last frame unless this one shows the grid clearly
 * (a flat or dark frame says little). O(w*h). */
static void fe_detect_phase(const uint32_t *pix, int pitch, int w, int h) {
    static uint8_t *lum = NULL;
    static size_t   cap = 0;
    if (w < 32 || h < 32) return;
    if ((size_t)w * h > cap) {
        uint8_t *n = (uint8_t *)realloc(lum, (size_t)w * h);
        if (!n) return;
        lum = n; cap = (size_t)w * h;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t c = pix[(size_t)y * pitch + x];
            lum[(size_t)y * w + x] = (uint8_t)((((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 + (c & 0xFF) * 29) >> 8);
        }
    uint64_t s[2][8] = {{0}};
    for (int a = 0; a < 2; a++) {
        const int n = a ? h : w, m = a ? w : h;              /* along, across */
        const size_t st = a ? (size_t)w : 1u, cs = a ? 1u : (size_t)w;
        for (int b = 0; b < m; b += 2)
            for (int k = 2; k < n - 1; k++) {
                const uint8_t *q = lum + (size_t)b * cs + (size_t)k * st;
                const int d = abs((int)q[0] - (int)q[-(ptrdiff_t)st]);
                const int l = abs((int)q[-(ptrdiff_t)st] - (int)q[-2 * (ptrdiff_t)st]);
                const int r = abs((int)q[st] - (int)q[0]);
                if (d >= 24 || l >= 24 || r >= 24) continue;
                const int e = 2 * d - l - r;                     /* x2: integer */
                if (e > 0) s[a][k & 7] += (uint64_t)e;
            }
        int best = 0; uint64_t tot = 0;
        for (int k = 0; k < 8; k++) { tot += s[a][k]; if (s[a][k] > s[a][best]) best = k; }
        const uint64_t others = (tot - s[a][best]) / 7u;
        if (s[a][best] && s[a][best] * 100u > others * 115u) s_fe_phase[a] = best;
    }
}

/* Runs the stages in `mask` on the frame now in s_present_tex (cw x src_h)
 * up to an lw x lh texture; returns which s_fe_tex holds the result. */
static int fe_run(int mask, int cw, int src_h, int lw, int lh) {
    /* the movie's coarseness: alpha (largest step taken for blocking) grows
     * with the quantiser; beta (how flat each side must be) is half of it */
    const uint32_t q16 = mdec_mean_qscale_x16();
    const float q = q16 ? (float)q16 / 16.0f : 6.0f;
    float alpha = 0.02f + 0.008f * q;
    if (alpha < 0.03f) alpha = 0.03f;
    if (alpha > 0.16f) alpha = 0.16f;

    GLuint cur = s_present_tex;
    int nxt = 0;                                               /* A/B ping-pong */
    GLuint pr;
    if (mask & FE_CHROMA) {
        fe_target(nxt, cw, src_h);
        pr = s_fe_prog[0];
        p_glUseProgram(pr);
        p_glUniform2i(p_glGetUniformLocation(pr, "u_cphase"), s_fe_phase[0] & 1, s_fe_phase[1] & 1);
        fe_pass(pr, cur, cw, src_h);
        cur = s_fe_tex[nxt]; nxt ^= 1;
    }
    if (mask & FE_DEBLOCK) {
        pr = s_fe_prog[1];
        for (int a = 0; a < 2; a++) {
            fe_target(nxt, cw, src_h);
            p_glUseProgram(pr);
            p_glUniform2i(p_glGetUniformLocation(pr, "u_dir"), a ? 0 : 1, a ? 1 : 0);
            p_glUniform1i(p_glGetUniformLocation(pr, "u_phase"), s_fe_phase[a]);
            p_glUniform2f(p_glGetUniformLocation(pr, "u_thr"), alpha, alpha * 0.5f);
            fe_pass(pr, cur, cw, src_h);
            cur = s_fe_tex[nxt]; nxt ^= 1;
        }
    }
    fe_target(2, lw, lh);                                      /* EASU */
    pr = s_fe_prog[2];
    p_glUseProgram(pr);
    p_glUniform1i(p_glGetUniformLocation(pr, "u_nearest"), (mask & FE_EASU) ? 0 : 1);
    p_glUniform4f(p_glGetUniformLocation(pr, "u_con0"), (float)cw / (float)lw, (float)src_h / (float)lh,
                  0.5f * (float)cw / (float)lw - 0.5f, 0.5f * (float)src_h / (float)lh - 0.5f);
    fe_pass(pr, cur, cw, src_h);
    if (!(mask & FE_RCAS)) return 2;
    fe_target(3, lw, lh);                                      /* RCAS */
    pr = s_fe_prog[3];
    p_glUseProgram(pr);
    p_glUniform1f(p_glGetUniformLocation(pr, "u_sharp"), exp2f(-FE_RCAS_STOPS));
    fe_pass(pr, s_fe_tex[2], lw, lh);
    return 3;
}

/* Debug dump: the texture attached to the stage's FBO (fe_target) as a
 * bottom-up 24-bit BMP. */
static void fe_write_bmp(const char *path, int w, int h) {
    uint8_t *buf = (uint8_t *)malloc((size_t)w * h * 4);
    if (!buf) return;
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, buf);
    FILE *f = fopen(path, "wb");
    if (f) {
        const int row = (w * 3 + 3) & ~3;
        const uint32_t img = (uint32_t)row * (uint32_t)h, size = 54u + img;
        const int32_t bw = w, bh = h;                          /* positive height: bottom-up */
        uint8_t hd[54] = {'B', 'M'};
        memcpy(hd + 2, &size, 4); hd[10] = 54; hd[14] = 40;
        memcpy(hd + 18, &bw, 4); memcpy(hd + 22, &bh, 4);
        hd[26] = 1; hd[28] = 24; memcpy(hd + 34, &img, 4);
        fwrite(hd, 1, 54, f);
        uint8_t *line = (uint8_t *)calloc((size_t)row, 1);
        /* texture row 0 is the picture's top; a BMP starts at its bottom */
        for (int y = h - 1; line && y >= 0; y--) {
            for (int x = 0; x < w; x++) memcpy(line + x * 3, buf + ((size_t)y * w + x) * 4, 3);
            fwrite(line, 1, (size_t)row, f);
        }
        free(line);
        fclose(f);
    }
    free(buf);
}

static void fe_dump_variants(int cw, int src_h, int lw, int lh) {
    static const int masks[] = {0, FE_EASU, FE_EASU | FE_CHROMA, FE_EASU | FE_DEBLOCK,
                                FE_EASU | FE_CHROMA | FE_DEBLOCK, FE_ALL};
    for (size_t i = 0; i < sizeof masks / sizeof masks[0]; i++) {
        const int t = fe_run(masks[i], cw, src_h, lw, lh);
        fe_target(t, lw, lh);
        char path[300];
        snprintf(path, sizeof path, "%s_m%02d.bmp", s_fe_dump, masks[i]);
        fe_write_bmp(path, lw, lh);
    }
    s_fe_dump[0] = 0;
}

/* Runs the stage for one 24-bit frame and draws it into (lx, ly, lw, lh) of
 * the default framebuffer. 0 = not done (off, too small to upscale, or no GL
 * support): the caller presents the frame its usual way. */
static int fe_present(const uint32_t *pixels, int src_w, int src_h, int cw,
                      int lx, int ly, int lw, int lh) {
    if (!s_fe_on || cw < 32 || src_h < 32 || lw <= cw || lh <= src_h) return 0;
    if (s_fe_state == 0) s_fe_state = fe_build();
    if (s_fe_state < 0) return 0;
    fe_detect_phase(pixels, src_w, cw, src_h);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);
    if (s_fe_dump[0]) fe_dump_variants(cw, src_h, lw, lh);
    const int t = fe_run(s_fe_mask, cw, src_h, lw, lh);

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);                 /* present 1:1 */
    glViewport(lx, ly, lw, lh);
    p_glUseProgram(s_present_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_fe_tex[t]);
    p_glUniform1i(s_present_uTex, 0);
    present_set_gamma(s_present_uGamma, 1);
    present_set_sharp(-1, lw, lh, lw, lh);
    PRESENT_SCANLINE(src_h, src_h, lh);
    p_glUniform4f(s_present_uUvRect, 0.f, 0.f, 1.f, 1.f);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (blend) glEnable(GL_BLEND);
    s_fe_frames++;
    return 1;
}

void gl_renderer_set_fmv_enhance(int on) { GL_RT_SYNC("fmv_enhance"); s_fe_on = on ? 1 : 0; }
int  gl_renderer_fmv_enhance(void) { return s_fe_on; }
uint64_t gl_renderer_fmv_enhance_frames(void) { GL_RT_SYNC("fmv_enhance_stats"); return s_fe_frames; }
void gl_renderer_fmv_enhance_phase(int *px, int *py) { GL_RT_SYNC("fmv_enhance_phase"); *px = s_fe_phase[0]; *py = s_fe_phase[1]; }
void gl_renderer_fmv_enhance_debug(int mask, const char *dump_prefix) {
    GL_RT_SYNC("fmv_enhance_debug");
    if (mask >= 0) s_fe_mask = mask & FE_ALL;
    if (dump_prefix && *dump_prefix) snprintf(s_fe_dump, sizeof s_fe_dump, "%s", dump_prefix);
}
