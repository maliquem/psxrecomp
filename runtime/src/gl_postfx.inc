/* ---- Post-effect pipeline (docs/POST_EFFECTS.md) ---------------------------
 * One executor for every post effect on the composed game image: chains of
 * full-screen fragment passes (postfx_chain.h). Three stages, in order, each
 * a chain that reads a fresh copy of the image and writes it back:
 *   1. bloom      built-in "bloom" preset, [video] bloom (strength uniform)
 *   2. user       [video] post_shader / PSX_POST_SHADER: any .pfx preset
 *   3. AA         built-in "fxaa"/"fxaa_hq" preset, [video] antialiasing_mode
 * All are elective and off by default; with no stage active nothing here
 * runs. Like the former fixed passes this runs in the window rect before
 * hold-last capture and the OSD, on whichever thread presents (the render
 * thread included), and Smooth motion's generated frames run stages 1 and 2.
 *
 * Inputs a pass may read: `source` (the composed image, one shared copy),
 * `mask` (optional: the primitive-derived "may glow" mask, see below),
 * `mask3d` (optional: 1 where the last opaque thing drawn was a PGXP-proven 3D
 * primitive, or a 3D primitive shows through something see-through; marked in
 * stencil bit 2, only while a chain reads it; the same texture, green; blue
 * is 1 where something semi-transparent was drawn over the last opaque thing,
 * stencil bit 3, e.g. a see-through HUD panel or a light shaft over scenery),
 * `depth` (optional: the PGXP depth buffer of the presented picture, window
 * aligned like the masks, DEPTH24_STENCIL8 sampled as .r = window depth d;
 * the PS1 SZ is 256 / (1 - d) - 256, and d = 1 where nothing 3D wrote: the
 * cleared background, 2D, sprites and semi-transparent primitives never write
 * depth, so under a sprite it is the scenery behind it; combine with mask3d),
 * and
 * earlier passes' targets. The executor owns target allocation and lifetime,
 * program build/cache, uniform binding (u_rcp, u_size, u_org and the preset's
 * uniforms) and GL state: everything the pass touches is put back.
 *
 * The mask. While an active chain reads `mask`, every draw keeps a mark in
 * stencil bit 1 of the surface it draws into (BCLS_DRAW in gpu_gl_renderer.c):
 * set by 3D (PGXP-proven) and additive primitives, cleared by opaque 2D ones
 * (sprites, fills, the HUD). The marks travel with the picture (native-wide
 * centre copies, Smooth motion surfaces). At the present they are blitted
 * from the presented surface into a window-sized stencil and turned into an
 * R8 texture (1 = lit). A temporal-blending in-between present reuses the last
 * mask. A chain that does not read `mask` costs none of this. */
/* The GL-free preset parser is compiled into this translation unit (every
 * build and GL fixture that compiles the renderer gets it with no extra
 * source); runtime/tests/test_postfx_chain.c builds it alone. */
#include "postfx_chain.c"

/* Uniform locations, looked up once per program (a lookup per frame is a
 * driver round trip on a threaded GL). */
enum { PFX_LOC_RCP, PFX_LOC_SIZE, PFX_LOC_ORG, PFX_LOC_UV, PFX_LOC_FIXED };
typedef struct {
    GLint fixed[PFX_LOC_FIXED];
    GLint in[PFX_MAX_INPUTS];
    GLint u[PFX_MAX_UNIFORMS];
} PfxLocs;
typedef struct {
    PfxChain chain;
    int      loaded, failed;
    GLuint   prog[PFX_MAX_PASSES];
    PfxLocs  loc[PFX_MAX_PASSES];
    GLuint   tex[PFX_MAX_PASSES], fbo[PFX_MAX_PASSES];
    GLuint   dtex[PFX_MAX_PASSES], dfbo[PFX_MAX_PASSES];   /* source_down copies */
    int      dw[PFX_MAX_PASSES], dh[PFX_MAX_PASSES];
    int      tw[PFX_MAX_PASSES], th[PFX_MAX_PASSES];
} PfxStage;
/* EXTRA0..4: further file chains run, in order, before USER (e.g. a depth
 * look, then a down-sample, under a CRT chain). */
#define PFX_EXTRA 5
enum { PFX_STAGE_BLOOM, PFX_STAGE_EXTRA0, PFX_STAGE_USER = PFX_STAGE_EXTRA0 + PFX_EXTRA,
       PFX_STAGE_AA, PFX_STAGE_HQ, PFX_NSTAGES };
#define PFX_IS_EXTRA(i)  ((i) >= PFX_STAGE_EXTRA0 && (i) < PFX_STAGE_EXTRA0 + PFX_EXTRA)
static PfxStage s_pfx[PFX_NSTAGES];
static char   s_pfx_user_path[1024] = "";
static char   s_pfx_extra_path[PFX_EXTRA][1024];
static int    s_pfx_user_env = 0;

static float  s_bloom_thr = 0.55f;          /* bloom bright-pass threshold */
/* Live overrides of the user chain's uniforms (gl_renderer_set_post_uniform),
 * applied every frame so they survive a reload of the preset. */
enum { PFX_USER_OVR = 64 };   /* live uniforms: CRT, looks, AO, light, sun... */
static struct { char name[PFX_NAME]; int n; float v[4]; } s_user_ovr[PFX_USER_OVR];
static int    s_user_novr = 0;
static uint64_t s_bloom_passes = 0;
static int    s_bloom_time = 0;             /* debug: glFinish-bracketed cost */
static double s_bloom_us_sum = 0.0; static uint64_t s_bloom_us_n = 0;   /* real frames */
static double s_bloom_gus_sum = 0.0; static uint64_t s_bloom_gus_n = 0; /* generated frames */
static uint64_t s_pfx_reused = 0;           /* passes skipped on generated frames */

/* The mask (see above). */
static GLuint s_mask_src_fbo = 0, s_mask_src_rb = 0;
static int    s_mask_src_tw = 0, s_mask_src_th = 0;
static int    s_mask_src[4];                /* x0,y0,x1,y1 in source FBO px (y0 = top) */
static int    s_mask_src_valid = 0, s_mask_ok = 0;
/* Native-wide: the centre's marks come from the hr surface (its copy into
 * the wide surface is colour only). */
static GLuint s_mask_ctr_fbo = 0, s_mask_ctr_rb = 0;
static int    s_mask_ctr_tw = 0, s_mask_ctr_th = 0;
static int    s_mask_ctr[4], s_mask_ctr_valid = 0;
static float  s_mask_ctr_x0 = 0.0f, s_mask_ctr_x1 = 0.0f;   /* fractions of the displayed width */
static GLuint s_mask_tex = 0, s_mask_fbo = 0, s_mask_prog = 0, s_mask_map_prog = 0;
static GLint  s_mask_uUv = -1, s_mask_uV = -1;
static GLint  s_mmap_uUv = -1, s_mmap_uS = -1, s_mmap_uSrc = -1, s_mmap_uDst = -1;
/* Marks resolved in the surfaces' own px: an R8 texture per surface,
 * attached with that surface's own depth-stencil renderbuffer, so no
 * depth-stencil blit is needed (one costs the CPU milliseconds on macOS). */
typedef struct { GLuint rb, tex, fbo; int w, h; } PfxSurfMask;
static PfxSurfMask s_smask[2];
static int    s_mask_w = 0, s_mask_h = 0;

static int pfx_stage_on(int i) {
    if (PFX_IS_EXTRA(i)) return s_pfx_extra_path[i - PFX_STAGE_EXTRA0][0] != 0;
    switch (i) {
    case PFX_STAGE_BLOOM: return s_bloom > 0.0f;
    case PFX_STAGE_USER:  return s_pfx_user_path[0] != 0;
    case PFX_STAGE_AA:    return s_post_aa == GL_POST_AA_FXAA;
    case PFX_STAGE_HQ:    return s_post_aa == GL_POST_AA_FXAA_HQ;
    }
    return 0;
}
static int postfx_active(void) {
    for (int i = 0; i < PFX_NSTAGES; i++) if (pfx_stage_on(i)) return 1;
    return 0;
}

static void pfx_stage_unload(PfxStage *st) {
    for (int i = 0; i < PFX_MAX_PASSES; i++) {
        if (s_ctx) {
            if (st->prog[i]) p_glDeleteProgram(st->prog[i]);
            if (st->fbo[i]) p_glDeleteFramebuffers(1, &st->fbo[i]);
            if (st->tex[i]) glDeleteTextures(1, &st->tex[i]);
            if (st->dfbo[i]) p_glDeleteFramebuffers(1, &st->dfbo[i]);
            if (st->dtex[i]) glDeleteTextures(1, &st->dtex[i]);
        }
        st->prog[i] = st->tex[i] = st->fbo[i] = 0; st->tw[i] = st->th[i] = 0;
        st->dtex[i] = st->dfbo[i] = 0; st->dw[i] = st->dh[i] = 0;
    }
    pfx_chain_free(&st->chain);
    st->loaded = st->failed = 0;
}

/* Which stages read the mask decides whether draws keep it. */
static void pfx_mask_wanted_update(void) {
    int want = 0;
    for (int i = 0; i < PFX_NSTAGES; i++) {
        if (!pfx_stage_on(i)) continue;
        if (i == PFX_STAGE_BLOOM) want = 1;          /* the bloom preset reads it */
        else if (s_pfx[i].loaded && s_pfx[i].chain.uses_mask) want = 1;
    }
    int want3d = 0;
    for (int i = 0; i < PFX_NSTAGES; i++)
        if (pfx_stage_on(i) && s_pfx[i].loaded && s_pfx[i].chain.uses_mask3d) want3d = 1;
    int wantd = 0;
    for (int i = 0; i < PFX_NSTAGES; i++)
        if (pfx_stage_on(i) && s_pfx[i].loaded && s_pfx[i].chain.uses_depth) wantd = 1;
    if (want != s_bcls_on || want3d != s_b3d_on || wantd != s_dep_on) {
        s_bcls_on = want; s_b3d_on = want3d; s_dep_on = wantd;
        pgxp_render_wanted_update();
    }
}

static int pfx_stage_load(int i) {
    PfxStage *st = &s_pfx[i];
    /* failed first: a preset that loaded but has a pass that did not
     * compile stays off until it is set again (no rebuild every present). */
    if (st->failed) return 0;
    if (st->loaded) return 1;
    char err[256] = "";
    int ok;
    const char *file = i == PFX_STAGE_USER ? s_pfx_user_path
                     : PFX_IS_EXTRA(i) ? s_pfx_extra_path[i - PFX_STAGE_EXTRA0] : NULL;
    if (file) ok = pfx_chain_load_file(file, &st->chain, err, sizeof err);
    else ok = pfx_chain_builtin(i == PFX_STAGE_BLOOM ? "bloom" : i == PFX_STAGE_AA ? "fxaa" : "fxaa_hq",
                                &st->chain, err, sizeof err);
    if (!ok) {
        fprintf(stdout, "psxrecomp: post effect '%s' not loaded: %s\n",
                file ? file : "built-in", err);
        st->failed = 1;
        return 0;
    }
    st->loaded = 1;
    if (file)
        fprintf(stdout, "psxrecomp: post shader '%s' (%d passes%s%s)\n", file,
                st->chain.npasses, st->chain.uses_mask ? ", mask" : "",
                st->chain.uses_mask3d ? ", mask3d" : "");
    pfx_mask_wanted_update();   /* what the chain reads is known now */
    return 1;
}

static uint64_t s_pfx_builds = 0;           /* program builds (tests) */
static GLuint pfx_build_pass(const PfxPass *ps) {
    s_pfx_builds++;
    size_t n = strlen(ps->shader) + 512 + (size_t)ps->ninputs * 64;
    char *src = (char *)malloc(n);
    if (!src) return 0;
    int k = snprintf(src, n, "#version 330\nin vec2 v_uv; out vec4 o;\n"
                             "uniform vec2 u_rcp; uniform vec2 u_size; uniform ivec2 u_org;\n");
    for (int i = 0; i < ps->ninputs; i++)
        k += snprintf(src + k, n - (size_t)k, "uniform sampler2D %s;\n", ps->in[i].sampler);
    snprintf(src + k, n - (size_t)k, "%s", ps->shader);
    GLuint prog = build_program(PRESENT_VS, src);
    free(src);
    return prog;
}

/* The depth-stencil renderbuffer of a presentable surface (0: unknown). */
static GLuint pfx_surface_rb(GLuint fbo) {
    if (fbo == s_hr_fbo) return s_hr_rb;
    for (int i = 0; i < s_hiw_n; i++) if (s_hiw_t[i].fbo == fbo) return s_hiw_t[i].rb;
    const int wi = wide_index(fbo);
    return wi >= 0 ? s_wide_rb[wi] : 0;
}
/* A displayed rect of a surface in that surface's px (y0 = top row). */
static void pfx_rect_px(GLuint tex, float tex_w, float tex_h, int x, int y, int w, int h, int out[4],
                        int *out_tw, int *out_th) {
    GLint tw = 0, th = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glBindTexture(GL_TEXTURE_2D, 0);
    const float sx = (float)tw / tex_w, sy = (float)th / tex_h;
    *out_tw = tw; *out_th = th;
    out[0] = (int)lrintf((float)x * sx);
    out[1] = (int)lrintf((float)y * sy);
    out[2] = (int)lrintf((float)(x + w) * sx);
    out[3] = (int)lrintf((float)(y + h) * sy);
}
/* Native-wide: the centre (native px x..x+w of the hr surface) shows at
 * native-wide columns off..off+w of the wide_w displayed. */
static void postfx_note_center(GLuint fbo, GLuint tex, float tex_w, float tex_h,
                               int x, int y, int w, int h, int off, int wide_w) {
    s_mask_ctr_valid = 0;
    if (!(s_bcls_on || s_b3d_on || s_dep_on) || !fbo || !tex || wide_w <= 0) return;
    s_mask_ctr_rb = pfx_surface_rb(fbo);
    if (!s_mask_ctr_rb) return;
    pfx_rect_px(tex, tex_w, tex_h, x, y, w, h, s_mask_ctr, &s_mask_ctr_tw, &s_mask_ctr_th);
    s_mask_ctr_fbo = fbo;
    s_mask_ctr_x0 = (float)off / (float)wide_w;
    s_mask_ctr_x1 = (float)(off + w) / (float)wide_w;
    s_mask_ctr_valid = 1;
}

/* The presented surface: its FBO and the displayed rect, in native units of
 * a texture tex_w x tex_h native units across (as present_target_quad). */
static void postfx_note_source(GLuint fbo, GLuint tex, float tex_w, float tex_h,
                               int x, int y, int w, int h) {
    s_mask_src_valid = 0;
    s_mask_ctr_valid = 0;
    if (!(s_bcls_on || s_b3d_on || s_dep_on) || !fbo || !tex || tex_w <= 0.0f || tex_h <= 0.0f) return;
    s_mask_src_rb = pfx_surface_rb(fbo);
    if (!s_mask_src_rb) return;
    pfx_rect_px(tex, tex_w, tex_h, x, y, w, h, s_mask_src, &s_mask_src_tw, &s_mask_src_th);
    s_mask_src_fbo = fbo;
    s_mask_src_valid = 1;
}

static void pfx_tex_alloc(GLuint *t, int w, int h, int fmt, int nearest) {
    if (!*t) glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    if (fmt == PFX_FMT_RGBA16F) glTexImage2D(GL_TEXTURE_2D, 0, 0x881A /*RGBA16F*/, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    else if (fmt == -1)         glTexImage2D(GL_TEXTURE_2D, 0, 0x8229 /*R8*/, w, h, 0, 0x1903 /*RED*/, GL_UNSIGNED_BYTE, NULL);
    else                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
}
static void pfx_fbo_attach(GLuint *f, GLuint tex) {
    if (!*f) p_glGenFramebuffers(1, f);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, *f);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
}

/* Window-sized mask from the presented surface's marks (fresh) or the last. */
/* The marks of rect r (surface px) of surface rb (tw x th px) into slot k's
 * RGBA8 texture: 0 over the rect, then red 1 where stencil bit 1 (may glow) is
 * set, green 1 where bit 2 (3D) is and blue 1 where bit 3 (see-through cover) is. */
static int pfx_surf_mask(int k, GLuint rb, int tw, int th, const int r[4]) {
    PfxSurfMask *m = &s_smask[k];
    if (m->rb != rb || m->w != tw || m->h != th) {
        pfx_tex_alloc(&m->tex, tw, th, 0, 1);
        pfx_fbo_attach(&m->fbo, m->tex);
        p_glFramebufferRenderbuffer(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT,
                                    PSXGL_RENDERBUFFER, rb);
        m->rb = rb; m->w = tw; m->h = th;
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, m->fbo);
    glViewport(r[0], r[1], r[2] - r[0], r[3] - r[1]);
    p_glUseProgram(s_mask_prog);
    p_glUniform1f(s_mask_uV, 0.0f);
    post_fullscreen(s_mask_prog, s_mask_uUv);
    glEnable(GL_STENCIL_TEST);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    p_glUniform1f(s_mask_uV, 1.0f);
    if (s_bcls_on) {
        glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
        glStencilFunc(GL_EQUAL, 0x02, 0x02);
        post_fullscreen(s_mask_prog, s_mask_uUv);
    }
    if (s_b3d_on) {
        glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
        glStencilFunc(GL_EQUAL, 0x04, 0x04);
        post_fullscreen(s_mask_prog, s_mask_uUv);
        glColorMask(GL_FALSE, GL_FALSE, GL_TRUE, GL_FALSE);
        glStencilFunc(GL_EQUAL, 0x08, 0x08);
        post_fullscreen(s_mask_prog, s_mask_uUv);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    return 1;
}
/* Slot k's rect r into window columns [x0, x1) of the lw x lh mask, the way
 * a NEAREST blit (row 0 = the rect's bottom) would map it. */
static void pfx_mask_map(int k, const int r[4], int x0, int x1, int lh) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_mask_fbo);
    glViewport(x0, 0, x1 - x0, lh);
    p_glUseProgram(s_mask_map_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_smask[k].tex);
    p_glUniform1i(s_mmap_uS, 0);
    p_glUniform4i(s_mmap_uSrc, r[0], r[1], r[2], r[3]);
    p_glUniform4f(s_mmap_uDst, (float)x0, (float)(x1 - x0), (float)lh, 0.0f);
    post_fullscreen(s_mask_map_prog, s_mmap_uUv);
}

/* Window-sized depth (DEPTH24_STENCIL8 texture) from the presented surface's
 * depth buffer (fresh) or the last: a NEAREST depth blit of the displayed rect,
 * flipped so window row lh-1 is the rect's top (as the masks map), then the
 * native-wide centre from the hr surface over its columns. */
static GLuint s_dep_tex = 0, s_dep_fbo = 0;
static int    s_dep_w = 0, s_dep_h = 0, s_dep_ok = 0;
static int pfx_depth_build(int fresh, int lw, int lh) {
    if (s_dep_w != lw || s_dep_h != lh) {
        if (!s_dep_tex) glGenTextures(1, &s_dep_tex);
        glBindTexture(GL_TEXTURE_2D, s_dep_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, PSXGL_DEPTH24_STENCIL8, lw, lh, 0,
                     0x84F9 /*DEPTH_STENCIL*/, 0x84FA /*UNSIGNED_INT_24_8*/, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!s_dep_fbo) p_glGenFramebuffers(1, &s_dep_fbo);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_dep_fbo);
        p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_dep_tex, 0);
        s_dep_w = lw; s_dep_h = lh; s_dep_ok = 0;
    }
    if (fresh && s_mask_src_valid) {
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_dep_fbo);
        glDepthMask(GL_TRUE);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_mask_src_fbo);
        p_glBlitFramebuffer(s_mask_src[0], s_mask_src[1], s_mask_src[2], s_mask_src[3],
                            0, lh, lw, 0, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        if (s_mask_ctr_valid) {
            const int c0 = (int)lrintf(s_mask_ctr_x0 * (float)lw), c1 = (int)lrintf(s_mask_ctr_x1 * (float)lw);
            if (c1 > c0) {
                p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_mask_ctr_fbo);
                p_glBlitFramebuffer(s_mask_ctr[0], s_mask_ctr[1], s_mask_ctr[2], s_mask_ctr[3],
                                    c0, lh, c1, 0, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            }
        }
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
        s_dep_ok = 1;
    }
    return s_dep_ok;
}

/* Window-sized mask from the presented surface's marks (fresh) or the last. */
static int pfx_mask_build(int fresh, int lw, int lh) {
    if (!s_mask_prog) {
        s_mask_prog = build_program(PRESENT_VS,
            "#version 330\nin vec2 v_uv; out vec4 o; uniform float u_v;\nvoid main(){ o = vec4(u_v); }\n");
        s_mask_map_prog = build_program(PRESENT_VS,
            "#version 330\nin vec2 v_uv; out vec4 o; uniform sampler2D u_s;\n"
            "uniform ivec4 u_src; uniform vec4 u_dst;\n"   /* dst: x0, width, height */
            "void main(){\n"
            "  vec2 f = gl_FragCoord.xy;\n"
            "  int sx = u_src.x + int(floor((f.x - u_dst.x) * float(u_src.z - u_src.x) / u_dst.y));\n"
            "  int sy = u_src.y + int(floor((u_dst.z - f.y) * float(u_src.w - u_src.y) / u_dst.z));\n"
            "  vec3 m = texelFetch(u_s, ivec2(sx, sy), 0).rgb;\n"
            "  o = vec4(m, m.r);\n"
            "}\n");
        if (!s_mask_prog || !s_mask_map_prog) { s_mask_prog = 0; return 0; }
        s_mask_uUv = p_glGetUniformLocation(s_mask_prog, "u_uv_rect");
        s_mask_uV = p_glGetUniformLocation(s_mask_prog, "u_v");
        s_mmap_uUv = p_glGetUniformLocation(s_mask_map_prog, "u_uv_rect");
        s_mmap_uS = p_glGetUniformLocation(s_mask_map_prog, "u_s");
        s_mmap_uSrc = p_glGetUniformLocation(s_mask_map_prog, "u_src");
        s_mmap_uDst = p_glGetUniformLocation(s_mask_map_prog, "u_dst");
    }
    if (s_mask_w != lw || s_mask_h != lh) {
        pfx_tex_alloc(&s_mask_tex, lw, lh, 0, 1);
        pfx_fbo_attach(&s_mask_fbo, s_mask_tex);
        s_mask_w = lw; s_mask_h = lh; s_mask_ok = 0;
    }
    if (fresh && s_mask_src_valid) {
        pfx_surf_mask(0, s_mask_src_rb, s_mask_src_tw, s_mask_src_th, s_mask_src);
        if (s_mask_ctr_valid)
            pfx_surf_mask(1, s_mask_ctr_rb, s_mask_ctr_tw, s_mask_ctr_th, s_mask_ctr);
        pfx_mask_map(0, s_mask_src, 0, lw, lh);
        if (s_mask_ctr_valid) {
            const int c0 = (int)lrintf(s_mask_ctr_x0 * (float)lw), c1 = (int)lrintf(s_mask_ctr_x1 * (float)lw);
            if (c1 > c0) pfx_mask_map(1, s_mask_ctr, c0, c1, lh);
        }
        s_mask_ok = 1;
    }
    return s_mask_ok;
}

/* Run one stage's chain over the window rect. 0 when it could not run.
 * generated: a Smooth motion in-between frame; passes marked `generated
 * reuse` keep the target the last real frame left (when it is there at this
 * size) instead of running. The full-size copy of the image is made only
 * if a pass that runs reads `source`; `source_down` is a blit of the window
 * rect straight to that pass's size. */
static int pfx_stage_run(PfxStage *st, int generated, int lx, int ly, int lw, int lh) {
    PfxChain *c = &st->chain;
    int copied = 0;
    for (int i = 0; i < c->npasses; i++) {
        const PfxPass *ps = &c->pass[i];
        const int last = i == c->npasses - 1;
        if (!st->prog[i]) {
            st->prog[i] = pfx_build_pass(ps);
            if (st->prog[i]) {
                PfxLocs *L = &st->loc[i];
                const GLuint pg = st->prog[i];
                L->fixed[PFX_LOC_RCP] = p_glGetUniformLocation(pg, "u_rcp");
                L->fixed[PFX_LOC_SIZE] = p_glGetUniformLocation(pg, "u_size");
                L->fixed[PFX_LOC_ORG] = p_glGetUniformLocation(pg, "u_org");
                L->fixed[PFX_LOC_UV] = p_glGetUniformLocation(pg, "u_uv_rect");
                for (int k = 0; k < ps->ninputs; k++) L->in[k] = p_glGetUniformLocation(pg, ps->in[k].sampler);
                for (int k = 0; k < ps->nuniforms; k++) L->u[k] = p_glGetUniformLocation(pg, ps->u[k].name);
            }
            if (!st->prog[i]) {
                fprintf(stdout, "psxrecomp: post effect '%s' pass '%s' did not compile; effect off\n",
                        c->name, ps->name);
                st->failed = 1;
                return 0;
            }
        }
        int w = lw, h = lh;
        if (!last) {
            if (ps->lines > 0) {   /* a fixed height (e.g. the console's lines), the rect's aspect */
                h = ps->lines;
                w = (int)lrintf((float)ps->lines * (float)lw / (float)lh);
            } else {
                w = (int)((float)lw * ps->scale); h = (int)((float)lh * ps->scale);
            }
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            if (generated && ps->reuse_generated && st->fbo[i] && st->tw[i] == w && st->th[i] == h) {
                s_pfx_reused++;
                continue;
            }
            if (st->tw[i] != w || st->th[i] != h) {
                pfx_tex_alloc(&st->tex[i], w, h, ps->format, ps->nearest);
                pfx_fbo_attach(&st->fbo[i], st->tex[i]);
                st->tw[i] = w; st->th[i] = h;
            }
        }
        /* The image this pass reads, before its target is bound. */
        for (int k = 0; k < ps->ninputs; k++) {
            if (ps->in[k].from == PFX_IN_SOURCE && !copied) {
                post_copy_bind(lw, lh);
                post_copy_window(lx, ly, lw, lh);
                copied = 1;
            } else if (ps->in[k].from == PFX_IN_SOURCE_DOWN) {
                if (st->dw[i] != w || st->dh[i] != h) {
                    pfx_tex_alloc(&st->dtex[i], w, h, PFX_FMT_RGBA8, 0);
                    pfx_fbo_attach(&st->dfbo[i], st->dtex[i]);
                    st->dw[i] = w; st->dh[i] = h;
                }
                p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
                p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, st->dfbo[i]);
                p_glBlitFramebuffer(lx, ly, lx + lw, ly + lh, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            }
        }
        if (last) { p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0); glViewport(lx, ly, lw, lh); }
        else { p_glBindFramebuffer(PSXGL_FRAMEBUFFER, st->fbo[i]); glViewport(0, 0, w, h); }
        const GLuint prog = st->prog[i];
        const PfxLocs *L = &st->loc[i];
        p_glUseProgram(prog);
        for (int k = 0; k < ps->ninputs; k++) {
            const int from = ps->in[k].from;
            const GLuint t = from == PFX_IN_SOURCE ? s_post_copy_tex
                           : from == PFX_IN_SOURCE_DOWN ? st->dtex[i]
                           : (from == PFX_IN_MASK || from == PFX_IN_MASK3D) ? s_mask_tex
                           : from == PFX_IN_DEPTH ? s_dep_tex
                           : st->tex[from];
            p_glActiveTexture(PSXGL_TEXTURE0 + (GLenum)k);
            glBindTexture(GL_TEXTURE_2D, t);
            p_glUniform1i(L->in[k], k);
        }
        p_glActiveTexture(PSXGL_TEXTURE0);
        p_glUniform2f(L->fixed[PFX_LOC_RCP], 1.0f / (float)w, 1.0f / (float)h);
        p_glUniform2f(L->fixed[PFX_LOC_SIZE], (float)w, (float)h);
        p_glUniform2i(L->fixed[PFX_LOC_ORG], last ? lx : 0, last ? ly : 0);
        for (int k = 0; k < ps->nuniforms; k++) {
            const PfxUniform *u = &ps->u[k];
            const GLint loc = L->u[k];
            if (loc < 0) continue;
            switch (u->count) {
            case 1: p_glUniform1f(loc, u->v[0]); break;
            case 2: p_glUniform2f(loc, u->v[0], u->v[1]); break;
            case 3: p_glUniform3f(loc, u->v[0], u->v[1], u->v[2]); break;
            default: p_glUniform4f(loc, u->v[0], u->v[1], u->v[2], u->v[3]); break;
            }
        }
        if (ps->blend == PFX_BLEND_SCREEN) {
            glEnable(GL_BLEND);
            p_glBlendEquationSeparate(PSXGL_FUNC_ADD, PSXGL_FUNC_ADD);
            p_glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
        }
        post_fullscreen(prog, L->fixed[PFX_LOC_UV]);
        if (ps->blend != PFX_BLEND_REPLACE) glDisable(GL_BLEND);
    }
    for (int k = PFX_MAX_INPUTS - 1; k >= 0; k--) {
        p_glActiveTexture(PSXGL_TEXTURE0 + (GLenum)k);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    return 1;
}

/* The GL state the executor changes, put back afterwards so whatever runs
 * next (the OSD, the next frame's draws) sees what it left. It queries
 * little (capability flags and the viewport; every query can be a driver
 * round trip on a threaded GL, per present, generated frames included) and
 * never touches the clear colour. It runs inside a present, which draws to
 * the window, so both framebuffer bindings go back to the window (0) and
 * texture unit 0 is left active. */
typedef struct {
    GLboolean blend, depth, scissor, stencil;
    GLint viewport[4];
} PfxGlState;
static void pfx_state_save(PfxGlState *st) {
    st->blend = glIsEnabled(GL_BLEND); st->depth = glIsEnabled(GL_DEPTH_TEST);
    st->scissor = glIsEnabled(GL_SCISSOR_TEST); st->stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_VIEWPORT, st->viewport);
}
static void pfx_state_restore(const PfxGlState *st) {
    if (st->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (st->depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (st->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (st->stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glViewport(st->viewport[0], st->viewport[1], st->viewport[2], st->viewport[3]);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    p_glActiveTexture(PSXGL_TEXTURE0);
}

/* fresh: the noted source is this present's (0: reuse the last mask).
 * with_aa: a real frame (runs AA too); 0: a Smooth motion generated frame. */
static void postfx_present(int fresh, int with_aa, int lx, int ly, int lw, int lh) {
    const int src_fresh = fresh;
    if (!s_ctx) { s_mask_src_valid = 0; return; }
    if (!post_clip_rect(&lx, &ly, &lw, &lh, 8)) { s_mask_src_valid = 0; return; }
    PfxGlState saved;
    pfx_state_save(&saved);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    int mask_done = 0, mask_ok = 0, dep_done = 0, dep_ok = 0;
    for (int i = 0; i < PFX_NSTAGES; i++) {
        if (!pfx_stage_on(i)) continue;
        if ((i == PFX_STAGE_AA || i == PFX_STAGE_HQ) && !with_aa) continue;
        if (!pfx_stage_load(i)) continue;
        PfxStage *st = &s_pfx[i];
        if (st->chain.uses_mask || st->chain.uses_mask3d) {
            if (!mask_done) { mask_ok = pfx_mask_build(src_fresh, lw, lh); mask_done = 1; }
            if (!mask_ok) continue;                  /* no marks yet */
        }
        if (st->chain.uses_depth) {
            if (!dep_done) { dep_ok = pfx_depth_build(src_fresh, lw, lh); dep_done = 1; }
            if (!dep_ok) continue;                   /* no depth yet */
        }
        if (i == PFX_STAGE_BLOOM) {
            const float k = s_bloom;
            pfx_chain_set_uniform(&st->chain, "u_strength", 1, &k);
            pfx_chain_set_uniform(&st->chain, "u_thr", 1, &s_bloom_thr);
            const float sp = s_bloom_add_only ? 1.0f : 0.0f;
            pfx_chain_set_uniform(&st->chain, "u_spread", 1, &sp);
        }
        if (i == PFX_STAGE_USER || PFX_IS_EXTRA(i))
            for (int k = 0; k < s_user_novr; k++)
                pfx_chain_set_uniform(&st->chain, s_user_ovr[k].name, s_user_ovr[k].n, s_user_ovr[k].v);
        int reps = 1; double *sum = NULL; uint64_t *cnt = NULL;
#ifndef PSX_NO_DEBUG_TOOLS
        if (i == PFX_STAGE_AA || i == PFX_STAGE_HQ) {
            if (s_paa_time < 0) {
                const char *e = getenv("PSX_POST_AA_TIME");
                s_paa_time = e ? atoi(e) : 0;
                if (s_paa_time < 0) s_paa_time = 0;
                if (s_paa_time > 64) s_paa_time = 64;
            }
            if (s_paa_time) { reps = s_paa_time > 1 ? s_paa_time : 1; sum = &s_paa_gpu_us_sum; cnt = &s_paa_gpu_n; }
        } else if (i == PFX_STAGE_BLOOM && s_bloom_time) {
            sum = with_aa ? &s_bloom_us_sum : &s_bloom_gus_sum;
            cnt = with_aa ? &s_bloom_us_n : &s_bloom_gus_n;
        }
        uint64_t t0 = 0;
        if (sum) { glFinish(); t0 = SDL_GetPerformanceCounter(); }
#endif
        int ran = 0;
        for (int r = 0; r < reps; r++) ran = pfx_stage_run(st, !with_aa, lx, ly, lw, lh);
#ifndef PSX_NO_DEBUG_TOOLS
        if (sum && ran) {
            glFinish();
            *sum += (double)(SDL_GetPerformanceCounter() - t0) * 1e6 /
                    (double)SDL_GetPerformanceFrequency() / (double)reps;
            (*cnt)++;
        }
#endif
        if (!ran) continue;
        if (i == PFX_STAGE_BLOOM) s_bloom_passes++;
        if (i == PFX_STAGE_AA || i == PFX_STAGE_HQ) s_paa_passes++;
    }
    s_mask_src_valid = 0; s_mask_ctr_valid = 0;
    pfx_state_restore(&saved);
}

/* ---- Pillarbox fill ([video] pillarbox_fill) ---------------------------------
 * A picture narrower than the window (a 4:3 logo, title screen or menu in a
 * 16:9 window) normally leaves black side bars. With the fill on, each
 * present looks at the picture's own left and right edges:
 *   - edges of one flat colour (a plain logo or title background): the bars
 *     take exactly that colour, so the background simply continues;
 *   - anything else (menu art): the bars show the picture scaled to cover the
 *     window, heavily blurred and darkened, as video players and remasters do.
 * FMV keeps black bars (CPU-readout presents and recent MDEC decodes are
 * skipped). It reads only the finished output pixels (after the post-effect
 * chains, before hold-last capture and the OSD) and draws only outside the
 * picture, so the picture, guest VRAM and every surface stay untouched. Off by
 * default; a bezel image, when set, takes precedence. */
extern int mdec_recently_active(uint32_t within_frames);
static GLuint s_bezel_tex;                  /* defined with the bezel code below */
static int    s_pillar_blur = 0;
/* 0..2: 8x, 4x, 2x staging; 3 and 4: blur ping-pong at 1x; 5: 1x1 edge colour. */
#define PB_LEVELS 6
static GLuint s_pb_tex[PB_LEVELS], s_pb_fbo[PB_LEVELS];
static GLuint s_pb_blur_prog, s_pb_edge_prog, s_pb_draw_prog;
static GLint  s_pb_blur_uv, s_pb_blur_src, s_pb_blur_dir;
static GLint  s_pb_edge_uv, s_pb_edge_src, s_pb_edge_px, s_pb_edge_rows;
static GLint  s_pb_draw_uv, s_pb_draw_src, s_pb_draw_edge, s_pb_draw_dim, s_pb_draw_flat;
/* Edge flatness limit, on the edge pixel's alpha (4 x the largest colour step
 * along the edges relative to their brightness). Measured with the CRT
 * shaders on: plain logo / title backgrounds ~0.11..0.15, menu art ~0.87. */
static float  s_pb_flat = 0.28f;
static int    s_pb_w, s_pb_h, s_pb_failed;
static uint64_t s_pb_fills = 0;             /* filled presents */

#define PB_SMALL_W 96                       /* work width; height follows the window */
static const char *PB_BLUR_FS =
    "#version 330\nin vec2 v_uv; out vec4 o;\n"
    "uniform sampler2D u_src; uniform vec2 u_dir;\n"
    "void main(){\n"
    "  vec4 c = texture(u_src, v_uv) * 0.227027;\n"
    "  c += (texture(u_src, v_uv + u_dir * 1.384615) + texture(u_src, v_uv - u_dir * 1.384615)) * 0.316216;\n"
    "  c += (texture(u_src, v_uv + u_dir * 3.230769) + texture(u_src, v_uv - u_dir * 3.230769)) * 0.070270;\n"
    "  o = c; }\n";
/* One pixel: rgb = mean of the outermost columns (each already the average
 * of ~15 window columns and several rows, so scanlines and dithering wash
 * out), a = their largest step relative to their brightness (x4, clamped). */
static const char *PB_EDGE_FS =
    "#version 330\nin vec2 v_uv; out vec4 o;\n"
    "uniform sampler2D u_src; uniform vec2 u_px; uniform int u_rows;\n"
    "void main(){\n"
    "  vec3 sum = vec3(0.0), lo = vec3(1.0), hi = vec3(0.0);\n"
    "  for (int i = 0; i < u_rows; i++) {\n"
    "    float v = (float(i) + 0.5) * u_px.y;\n"
    "    vec3 a = texture(u_src, vec2(0.5 * u_px.x, v)).rgb;\n"
    "    vec3 b = texture(u_src, vec2(1.0 - 0.5 * u_px.x, v)).rgb;\n"
    "    sum += a + b; lo = min(lo, min(a, b)); hi = max(hi, max(a, b));\n"
    "  }\n"
    "  vec3 d = hi - lo;\n"
    "  vec3 m = sum / float(2 * u_rows);\n"
    "  float rel = max(d.r, max(d.g, d.b)) / max(max(m.r, max(m.g, m.b)), 0.1);\n"
    "  o = vec4(m, clamp(rel * 4.0, 0.0, 1.0)); }\n";
static const char *PB_DRAW_FS =
    "#version 330\nin vec2 v_uv; out vec4 o;\n"
    "uniform sampler2D u_src; uniform sampler2D u_edge; uniform float u_dim, u_flat;\n"
    "void main(){\n"
    "  vec4 e = texture(u_edge, vec2(0.5));\n"
    "  o = vec4(e.a < u_flat ? e.rgb : texture(u_src, v_uv).rgb * u_dim, 1.0); }\n";

static void pillar_release(void) {
    memset(s_pb_tex, 0, sizeof s_pb_tex);
    memset(s_pb_fbo, 0, sizeof s_pb_fbo);
    s_pb_blur_prog = s_pb_edge_prog = s_pb_draw_prog = 0;
    s_pb_w = s_pb_h = 0; s_pb_failed = 0;
}

static int pillar_ready(int w, int h) {
    if (s_pb_failed) return 0;
    if (!s_pb_blur_prog) {
        s_pb_blur_prog = build_program(PRESENT_VS, PB_BLUR_FS);
        s_pb_edge_prog = build_program(PRESENT_VS, PB_EDGE_FS);
        s_pb_draw_prog = build_program(PRESENT_VS, PB_DRAW_FS);
        if (!s_pb_blur_prog || !s_pb_edge_prog || !s_pb_draw_prog) {
            fprintf(stdout, "psxrecomp: pillarbox fill shaders failed; bars stay black\n");
            s_pb_failed = 1;
            return 0;
        }
        s_pb_blur_uv = p_glGetUniformLocation(s_pb_blur_prog, "u_uv_rect");
        s_pb_blur_src = p_glGetUniformLocation(s_pb_blur_prog, "u_src");
        s_pb_blur_dir = p_glGetUniformLocation(s_pb_blur_prog, "u_dir");
        s_pb_edge_uv = p_glGetUniformLocation(s_pb_edge_prog, "u_uv_rect");
        s_pb_edge_src = p_glGetUniformLocation(s_pb_edge_prog, "u_src");
        s_pb_edge_px = p_glGetUniformLocation(s_pb_edge_prog, "u_px");
        s_pb_edge_rows = p_glGetUniformLocation(s_pb_edge_prog, "u_rows");
        s_pb_draw_uv = p_glGetUniformLocation(s_pb_draw_prog, "u_uv_rect");
        s_pb_draw_src = p_glGetUniformLocation(s_pb_draw_prog, "u_src");
        s_pb_draw_edge = p_glGetUniformLocation(s_pb_draw_prog, "u_edge");
        s_pb_draw_dim = p_glGetUniformLocation(s_pb_draw_prog, "u_dim");
        s_pb_draw_flat = p_glGetUniformLocation(s_pb_draw_prog, "u_flat");
        const char *e = getenv("PSX_PILLARBOX_FLAT");
        if (e && *e) s_pb_flat = (float)atof(e);
    }
    if (s_pb_w != w || s_pb_h != h) {
        /* Each staging step is 2:1 linear; one big step would sample a few
         * rows and alias scanlines into bands. */
        for (int i = 0; i < PB_LEVELS; i++) {
            const int k = i < 3 ? 8 >> i : 1;
            if (i == 5) pfx_tex_alloc(&s_pb_tex[i], 1, 1, PFX_FMT_RGBA8, 1);
            else pfx_tex_alloc(&s_pb_tex[i], w * k, h * k, PFX_FMT_RGBA8, 0);
            pfx_fbo_attach(&s_pb_fbo[i], s_pb_tex[i]);
        }
        s_pb_w = w; s_pb_h = h;
    }
    return 1;
}

static void pillar_pass(GLuint prog, GLint uv, float u0, float v0, float u1, float v1) {
    p_glUseProgram(prog);
    p_glUniform4f(uv, u0, v0, u1, v1);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* Fill the window outside the picture rect (lx, ly, lw, lh; GL window px). */
static void postfx_pillar_fill(int lx, int ly, int lw, int lh) {
    if (!s_pillar_blur || !s_ctx || !s_win || s_bezel_tex) return;
    if (mdec_recently_active(30)) return;          /* FMV keeps black bars */
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww <= 0 || wh <= 0 || lw <= 0 || lh <= 0) return;
    if (lx <= 0 && lw >= ww) return;                /* no side bars */
    const int bw = PB_SMALL_W;
    int bh = (int)((int64_t)bw * wh / ww);
    if (bh < 8) bh = 8;
    if (!pillar_ready(bw, bh)) return;
    PfxGlState saved;
    pfx_state_save(&saved);
    GLint prev_tex = 0;
    p_glActiveTexture(PSXGL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);   /* unit 0, left as found */
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    /* The picture scaled to cover the window: full width, centre rows. */
    int sh = (int)((int64_t)lw * wh / ww);
    if (sh > lh) sh = lh;
    const int sy = ly + (lh - sh) / 2;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_pb_fbo[0]);
    p_glBlitFramebuffer(lx, sy, lx + lw, sy + sh, 0, 0, bw * 8, bh * 8, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    for (int i = 0; i < 3; i++) {
        const int k = 8 >> i;
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_pb_fbo[i]);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_pb_fbo[i + 1]);
        p_glBlitFramebuffer(0, 0, bw * k, bh * k, 0, 0, bw * k / 2, bh * k / 2,
                            GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    /* Edge colour and flatness, from the 1x picture blurred vertically only
     * (scanlines and CRT masks average out; nothing mixes in from the side). */
    glViewport(0, 0, bw, bh);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_pb_fbo[4]);
    glBindTexture(GL_TEXTURE_2D, s_pb_tex[3]);
    p_glUseProgram(s_pb_blur_prog);
    p_glUniform1i(s_pb_blur_src, 0);
    p_glUniform2f(s_pb_blur_dir, 0.0f, 1.0f / (float)bh);
    pillar_pass(s_pb_blur_prog, s_pb_blur_uv, 0.0f, 1.0f, 1.0f, 0.0f);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_pb_fbo[5]);
    glViewport(0, 0, 1, 1);
    glBindTexture(GL_TEXTURE_2D, s_pb_tex[4]);
    p_glUseProgram(s_pb_edge_prog);
    p_glUniform1i(s_pb_edge_src, 0);
    p_glUniform2f(s_pb_edge_px, 1.0f / (float)bw, 1.0f / (float)bh);
    p_glUniform1i(s_pb_edge_rows, bh);
    pillar_pass(s_pb_edge_prog, s_pb_edge_uv, 0.0f, 1.0f, 1.0f, 0.0f);
    /* Separable Gaussian, two rounds with doubled taps: a strong, smooth blur. */
    glViewport(0, 0, bw, bh);
    p_glUseProgram(s_pb_blur_prog);
    p_glUniform1i(s_pb_blur_src, 0);
    for (int r = 0; r < 2; r++) {
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_pb_fbo[4]);
        glBindTexture(GL_TEXTURE_2D, s_pb_tex[3]);
        p_glUniform2f(s_pb_blur_dir, 2.0f / (float)bw, 0.0f);
        pillar_pass(s_pb_blur_prog, s_pb_blur_uv, 0.0f, 1.0f, 1.0f, 0.0f);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_pb_fbo[3]);
        glBindTexture(GL_TEXTURE_2D, s_pb_tex[4]);
        p_glUniform2f(s_pb_blur_dir, 0.0f, 2.0f / (float)bh);
        pillar_pass(s_pb_blur_prog, s_pb_blur_uv, 0.0f, 1.0f, 1.0f, 0.0f);
    }
    /* The bars: the flat edge colour, or window-space uv into the blurred cover image. */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, s_pb_tex[5]);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_pb_tex[3]);
    p_glUseProgram(s_pb_draw_prog);
    p_glUniform1i(s_pb_draw_src, 0);
    p_glUniform1i(s_pb_draw_edge, 1);
    p_glUniform1f(s_pb_draw_dim, 0.7f);
    p_glUniform1f(s_pb_draw_flat, s_pb_flat);
    const float fw = (float)ww;
    if (lx > 0) {
        glViewport(0, 0, lx, wh);
        pillar_pass(s_pb_draw_prog, s_pb_draw_uv, 0.0f, 1.0f, (float)lx / fw, 0.0f);
    }
    const int rx = lx + lw;
    if (rx < ww) {
        glViewport(rx, 0, ww - rx, wh);
        pillar_pass(s_pb_draw_prog, s_pb_draw_uv, (float)rx / fw, 1.0f, 1.0f, 0.0f);
    }
    p_glActiveTexture(PSXGL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
#ifndef PSX_NO_DEBUG_TOOLS
    /* PSX_PILLARBOX_DEBUG=1: log the edge measure every 60 fills (a sync read). */
    static int dbg = -1;
    if (dbg < 0) { const char *e = getenv("PSX_PILLARBOX_DEBUG"); dbg = e && e[0] == '1'; }
    if (dbg && s_pb_fills % 60 == 0) {
        unsigned char px[4] = {0, 0, 0, 0};
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_pb_fbo[5]);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        fprintf(stdout, "psxrecomp: pillarbox edge rgb %u %u %u measure %.3f (%s)\n",
                px[0], px[1], px[2], px[3] / 255.0, px[3] / 255.0f < s_pb_flat ? "flat" : "blur");
        fflush(stdout);
    }
#endif
    pfx_state_restore(&saved);
    s_pb_fills++;
}

/* PSX_PILLARBOX_FILL=auto|black overrides [video] pillarbox_fill. */
void gl_renderer_set_pillarbox_blur(int on) {
    GL_RT_SYNC("pillarbox_fill");
    const char *e = getenv("PSX_PILLARBOX_FILL");
    if (e && *e) on = strcmp(e, "black") != 0;
    s_pillar_blur = on ? 1 : 0;
}
int gl_renderer_get_pillarbox_blur(void) { return s_pillar_blur; }

/* Objects died with the context. */
static void postfx_release(void) {
    for (int i = 0; i < PFX_NSTAGES; i++) {
        for (int k = 0; k < PFX_MAX_PASSES; k++) {
            s_pfx[i].prog[k] = s_pfx[i].tex[k] = s_pfx[i].fbo[k] = 0;
            s_pfx[i].tw[k] = s_pfx[i].th[k] = 0;
            s_pfx[i].dtex[k] = s_pfx[i].dfbo[k] = 0; s_pfx[i].dw[k] = s_pfx[i].dh[k] = 0;
        }
        s_pfx[i].failed = 0;
    }
    s_mask_tex = s_mask_fbo = s_mask_prog = s_mask_map_prog = 0;
    memset(s_smask, 0, sizeof s_smask);
    s_mask_w = s_mask_h = 0; s_mask_ok = 0; s_mask_src_valid = 0;
    s_dep_tex = s_dep_fbo = 0; s_dep_w = s_dep_h = 0; s_dep_ok = 0;
    pillar_release();
}

/* [video] post_shader (PSX_POST_SHADER overrides): a .pfx chain, "" = none. */
void gl_renderer_set_post_shader(const char *path) {
    GL_RT_SYNC("post_shader");
    if (!s_pfx_user_env) {
        const char *e = getenv("PSX_POST_SHADER");
        if (e) { snprintf(s_pfx_user_path, sizeof s_pfx_user_path, "%s", e); s_pfx_user_env = 1; }
    }
    if (!s_pfx_user_env) {
        /* Setting it again (same path or not) reloads: the file may have
         * changed, and a failed preset gets one new try. */
        pfx_stage_unload(&s_pfx[PFX_STAGE_USER]);
        snprintf(s_pfx_user_path, sizeof s_pfx_user_path, "%s", path ? path : "");
    }
    if (s_pfx_user_path[0] && s_ctx) (void)pfx_stage_load(PFX_STAGE_USER);
    else if (s_pfx_user_path[0]) {
        /* No context yet: the parse needs none, so the mask decision is made now. */
        (void)pfx_stage_load(PFX_STAGE_USER);
    }
    pfx_mask_wanted_update();
}
/* Further file chains run in slot order right before the post_shader one (the
 * same format; "" = none), e.g. a look and a down-sample a CRT chain then
 * reads. No config key: set by code (a game's menu). Live uniforms below
 * reach every file chain. */
void gl_renderer_set_extra_shader(int slot, const char *path) {
    if (slot < 0 || slot >= PFX_EXTRA) return;
    GL_RT_SYNC("extra_shader");
    const int st = PFX_STAGE_EXTRA0 + slot;
    pfx_stage_unload(&s_pfx[st]);
    snprintf(s_pfx_extra_path[slot], sizeof s_pfx_extra_path[slot], "%s", path ? path : "");
    if (s_pfx_extra_path[slot][0]) (void)pfx_stage_load(st);
    pfx_mask_wanted_update();
}
/* Set a user-chain uniform live (every pass that declares it), e.g. from an
 * in-game menu. count 1..4 floats; the value stays until changed or cleared. */
void gl_renderer_set_post_uniform(const char *name, int count, const float *v) {
    if (!name || !*name || !v || count < 1) return;
    GL_RT_SYNC("post_uniform");
    if (count > 4) count = 4;
    int k = 0;
    while (k < s_user_novr && strcmp(s_user_ovr[k].name, name)) k++;
    if (k == s_user_novr) {
        if (s_user_novr == PFX_USER_OVR) return;
        snprintf(s_user_ovr[k].name, sizeof s_user_ovr[k].name, "%s", name);
        s_user_novr++;
    }
    s_user_ovr[k].n = count;
    for (int i = 0; i < count; i++) s_user_ovr[k].v[i] = v[i];
}
void gl_renderer_clear_post_uniforms(void) {
    GL_RT_SYNC("post_uniform");
    s_user_novr = 0;
}
int gl_renderer_post_shader_loaded(void) {
    GL_RT_SYNC("post_shader");
    return s_pfx[PFX_STAGE_USER].loaded && !s_pfx[PFX_STAGE_USER].failed;
}

/* PSX_BLOOM=<0..2> overrides [video] bloom. */
void gl_renderer_set_bloom(float strength) {
    GL_RT_SYNC("set_bloom");
    const char *e = getenv("PSX_BLOOM");
    if (e && *e) strength = (float)atof(e);
    if (!(strength >= 0.0f)) strength = 0.0f;
    if (strength > 2.0f) strength = 2.0f;
    s_bloom = strength;
    pfx_mask_wanted_update();
}
float gl_renderer_get_bloom(void) { return s_bloom; }

/* [video] bloom_threshold (0 keeps the built-in 0.55) and bloom_source
 * ("all" = 3D + additive, "additive" = lights and effects only).
 * PSX_BLOOM_THRESHOLD / PSX_BLOOM_SOURCE=additive override. */
void gl_renderer_set_bloom_threshold(float thr) {
    GL_RT_SYNC("set_bloom_threshold");
    const char *e = getenv("PSX_BLOOM_THRESHOLD");
    if (e && *e) thr = (float)atof(e);
    if (thr > 0.0f && thr <= 2.0f) s_bloom_thr = thr;
}
void gl_renderer_set_bloom_additive_only(int on) {
    GL_RT_SYNC("set_bloom_source");
    const char *e = getenv("PSX_BLOOM_SOURCE");
    if (e && *e) on = !strcmp(e, "additive3d") ? 2 : !strcmp(e, "additive") ? 1 : 0;
    s_bloom_add_only = on == 2 ? 2 : on ? 1 : 0;
}

void gl_renderer_bloom_debug(float strength, float threshold, int time,
                             GlBloomInfo *out) {
    GL_RT_SYNC("bloom_debug");
    if (strength >= 0.0f && strength <= 2.0f) { s_bloom = strength; pfx_mask_wanted_update(); }
    if (threshold > 0.0f) s_bloom_thr = threshold;
    if (time >= 0) s_bloom_time = time ? 1 : 0;
    if (out) {
        out->strength = s_bloom; out->threshold = s_bloom_thr;
        out->passes = s_bloom_passes;
        out->us_mean = s_bloom_us_n ? s_bloom_us_sum / (double)s_bloom_us_n : 0.0;
        out->us_n = s_bloom_us_n;
        out->gen_us_mean = s_bloom_gus_n ? s_bloom_gus_sum / (double)s_bloom_gus_n : 0.0;
        out->gen_us_n = s_bloom_gus_n;
        out->reused = s_pfx_reused;
        s_bloom_us_sum = 0.0; s_bloom_us_n = 0; s_bloom_gus_sum = 0.0; s_bloom_gus_n = 0;
    }
}
