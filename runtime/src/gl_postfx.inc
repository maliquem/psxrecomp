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
 * `shadow` (optional: a shadow map of the scenery seen from the light the
 * reading pass gives, and the camera's world axes; see pfx_shadow_build),
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
    int wantd = 0, wants = 0;
    for (int i = 0; i < PFX_NSTAGES; i++)
        if (pfx_stage_on(i) && s_pfx[i].loaded && s_pfx[i].chain.uses_depth) wantd = 1;
    for (int i = 0; i < PFX_NSTAGES; i++)
        if (pfx_stage_on(i) && s_pfx[i].loaded && s_pfx[i].chain.uses_shadow) wants = 1;
    if (wants && !s_sc_on) gl_renderer_shadow_cache_clear();   /* switched on: a fresh memory */
    s_sc_on = wants;
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
/* The `shadow` input. The reading pass gives the light: uniform u_shadowdir
 * (xyz: view space toward the light, y up) and u_proj.x = H (the GTE
 * projection distance). The casters (sc_add_tri) are rebuilt the way readers
 * of `depth` rebuild a pixel: X = dx SZ / H, Y = -dy SZ / H, Z = SZ, with
 * (dx, dy) the native px from the displayed picture's centre. The map is an
 * orthographic depth render along the light over the bounds of the casters
 * on screen (a receiver can only be on screen; a caster off screen still
 * lands in those bounds when it shades one), all of them in depth; the
 * pass also gets u_smx = (a, x0), u_smy = (b, y0), u_smz = (L, d1) and
 * u_sms = (1/(x1-x0), 1/(y1-y0), 1/(d1-d0), map size): a point P is at map uv
 * ((dot(P,a)-x0) sms.x, (dot(P,b)-y0) sms.y), depth (d1 - dot(P,L)) sms.z
 * there (0 = nearest the light). No casters: the map is cleared (no shadow). */
enum { SM_SIZE = 2048 };
static GLuint s_sm_tex = 0, s_sm_fbo = 0, s_sm_prog = 0, s_sm_vao = 0, s_sm_vbo = 0;
static GLint  s_sm_uvram = -1;
static float  s_sm_u[4][4];
static float  s_sm_world[3][4];   /* u_world, u_worldup, u_shadowdir (view space, y up; w 1 = set) */
static float *s_sm_buf = NULL; static int s_sm_cap = 0;
static const float *pfx_pass_uniform(const PfxPass *ps, const char *name) {
    for (int k = 0; k < ps->nuniforms; k++) if (!strcmp(ps->u[k].name, name)) return ps->u[k].v;
    return NULL;
}
static void pfx_shadow_build(const PfxPass *ps, float aspect) {
    if (!s_sm_tex) {
        glGenTextures(1, &s_sm_tex);
        glBindTexture(GL_TEXTURE_2D, s_sm_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6 /*DEPTH_COMPONENT24*/, SM_SIZE, SM_SIZE, 0,
                     0x1902 /*DEPTH_COMPONENT*/, 0x1405 /*UNSIGNED_INT*/, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        p_glGenFramebuffers(1, &s_sm_fbo);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_sm_fbo);
        p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, 0x8D00 /*DEPTH_ATTACHMENT*/, GL_TEXTURE_2D, s_sm_tex, 0);
        glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
        /* Transparent texels (raw 0) cast no shadow, as they draw nothing:
         * the fetch is the textured shader's (page, CLUT, window). */
        s_sm_prog = build_program(
            "#version 330\n"
            "layout(location = 0) in vec3 a_p; layout(location = 1) in vec2 a_uv;\n"
            "layout(location = 2) in vec4 a_tc; layout(location = 3) in vec2 a_dw;\n"
            "out vec2 v_uv; flat out ivec4 v_tc; flat out ivec2 v_dw;\n"
            "void main(){ v_uv = a_uv; v_tc = ivec4(a_tc); v_dw = ivec2(a_dw);\n"
            "  gl_Position = vec4(a_p, 1.0); }\n",
            "#version 330\n"
            "in vec2 v_uv; flat in ivec4 v_tc; flat in ivec2 v_dw; out vec4 o;\n"
            "uniform usampler2D u_vram;\n"
            "int vram_at(int x, int y){ return int(texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r); }\n"
            "void main(){\n"
            "  o = vec4(1.0);\n"
            "  if (v_dw.x < 0) return;\n"
            "  int u = int(floor(v_uv.x)) & 255, v = int(floor(v_uv.y)) & 255;\n"
            "  ivec4 tw = ivec4(v_dw.y & 31, (v_dw.y >> 5) & 31, (v_dw.y >> 10) & 31, (v_dw.y >> 15) & 31);\n"
            "  if ((tw.x | tw.y) != 0) { u = (u & ~(tw.x * 8)) | ((tw.z & tw.x) * 8);\n"
            "                            v = (v & ~(tw.y * 8)) | ((tw.w & tw.y) * 8); }\n"
            "  int raw;\n"
            "  if (v_dw.x == 0) raw = vram_at(v_tc.z + ((vram_at(v_tc.x + (u >> 2), v_tc.y + v) >> ((u & 3) * 4)) & 0xF), v_tc.w);\n"
            "  else if (v_dw.x == 1) raw = vram_at(v_tc.z + ((vram_at(v_tc.x + (u >> 1), v_tc.y + v) >> ((u & 1) * 8)) & 0xFF), v_tc.w);\n"
            "  else raw = vram_at(v_tc.x + u, v_tc.y + v);\n"
            "  if (raw == 0) discard;\n"
            "}\n");
        s_sm_uvram = s_sm_prog ? p_glGetUniformLocation(s_sm_prog, "u_vram") : -1;
        p_glGenVertexArrays(1, &s_sm_vao);
        p_glGenBuffers(1, &s_sm_vbo);
        p_glBindVertexArray(s_sm_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_sm_vbo);
        const GLsizei st = SC_V * sizeof(float);
        p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void *)0);
        p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, (void *)(3 * sizeof(float)));
        p_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, (void *)(5 * sizeof(float)));
        p_glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, (void *)(9 * sizeof(float)));
        for (GLuint k = 0; k < 4; k++) p_glEnableVertexAttribArray(k);
        p_glBindVertexArray(0);
    }
    memset(s_sm_u, 0, sizeof s_sm_u);
    s_sm_u[3][3] = (float)SM_SIZE;
    const float *dir = pfx_pass_uniform(ps, "u_shadowdir"), *proj = pfx_pass_uniform(ps, "u_proj");
    const float *sun = pfx_pass_uniform(ps, "u_sun");
    const float H = proj ? proj[0] : 0.0f;
    float L[3] = {dir ? dir[0] : 0.0f, dir ? dir[1] : 0.0f, dir ? dir[2] : 0.0f};
    const ScSlot *sc = sc_presented();
    /* The scene's own geometry (gl_renderer_shadow_world) and the camera the
     * picture on screen was drawn with: the whole scenery, placed exactly. */
    const int use_world = s_wg_n > 0 && sc && sc->cam_ok;
    if (use_world) {
        memcpy(s_cam_r, sc->cam.r, sizeof s_cam_r); memcpy(s_cam_t, sc->cam.t, sizeof s_cam_t);
        s_cam_of[0] = sc->cam.of[0]; s_cam_of[1] = sc->cam.of[1]; s_cam_h = sc->cam.h;
        s_cam_ok = 1;
    }
    memset(s_sm_world, 0, sizeof s_sm_world);
    if (s_cam_ok) {
        /* The world's axes in view space (y up) from the frame's camera:
         * east = R x, up = R y with the sign that points up on screen (a
         * camera looks down on its scenery). u_sun (azimuth from east toward
         * the far side, elevation; degrees) then fixes the light in the world. */
        const float *r = s_cam_r;
        float e[3] = {r[0], -r[3], r[6]}, u[3] = {r[1], -r[4], r[7]};
        if (u[1] < 0.0f) { u[0] = -u[0]; u[1] = -u[1]; u[2] = -u[2]; }
        const float eu = e[0] * u[0] + e[1] * u[1] + e[2] * u[2];
        float rt[3] = {e[0] - u[0] * eu, e[1] - u[1] * eu, e[2] - u[2] * eu};
        const float rl = sqrtf(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
        if (rl > 1e-4f) for (int i = 0; i < 3; i++) rt[i] /= rl;
        const float fw[3] = {rt[1] * u[2] - rt[2] * u[1], rt[2] * u[0] - rt[0] * u[2], rt[0] * u[1] - rt[1] * u[0]};
        for (int i = 0; i < 3; i++) { s_sm_world[0][i] = e[i]; s_sm_world[1][i] = u[i]; }
        s_sm_world[0][3] = s_sm_world[1][3] = 1.0f;
        if (sun) {
            const float az = sun[0] * 0.01745329f, el = sun[1] * 0.01745329f;
            for (int i = 0; i < 3; i++)
                L[i] = u[i] * sinf(el) + (rt[i] * cosf(az) + fw[i] * sinf(az)) * cosf(el);
            for (int i = 0; i < 3; i++) s_sm_world[2][i] = L[i];
            s_sm_world[2][3] = 1.0f;
        }
    }
    const float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    int n = 0;
    float a[3], b[3], x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, d0 = 1e30f, d1 = -1e30f;
    const int use_store = !use_world && s_cam_ok && s_scc_alive > 0;
    if (H > 0.0f && ll > 1e-6f && (sc || use_store) && s_sm_prog) {
        L[0] /= ll; L[1] /= ll; L[2] /= ll;
        /* a: across the light, level in view space when it can be */
        a[0] = L[2]; a[1] = 0.0f; a[2] = -L[0];
        float la = sqrtf(a[0] * a[0] + a[2] * a[2]);
        if (la < 1e-4f) { a[0] = 1.0f; a[2] = 0.0f; la = 1.0f; }
        a[0] /= la; a[2] /= la;
        b[0] = L[1] * a[2] - L[2] * a[1]; b[1] = L[2] * a[0] - L[0] * a[2]; b[2] = L[0] * a[1] - L[1] * a[0];
        /* From memory: what this frame drew, plus static scenery outside the
         * area the game draws (the picture and its overdraw band). */
        static int *pick = NULL; static int pick_cap = 0;
        int npick = 0;
        if (use_store) {
            if (s_scc_n > pick_cap) {
                int *p = (int *)realloc(pick, (size_t)s_scc_n * sizeof(int));
                if (p) { pick = p; pick_cap = s_scc_n; }
            }
            const float dw = 120.0f * aspect + (float)psx_overdraw_x() * 0.8f;
            const float dh = 120.0f + (float)psx_overdraw_y() * 0.8f;
            for (int e = 0; e < s_scc_n && npick < pick_cap; e++) {
                const ScCached *c = &s_scc[e];
                if (!c->alive) continue;
                if (c->seen != s_scc_stamp) {
                    if (c->hits < SCC_STATIC) continue;
                    float V[3], cxs = 0.0f, cys = 0.0f; int behind = 0;
                    for (int j = 0; j < 3; j++) {
                        scc_view(c->w + j * 3, V);
                        if (V[2] < 1.0f) { behind = 1; break; }
                        cxs += s_cam_h * V[0] / V[2]; cys += s_cam_h * V[1] / V[2];
                    }
                    if (!behind && fabsf(cxs / 3.0f) < dw && fabsf(cys / 3.0f) < dh) continue;
                }
                pick[npick++] = e;
            }
        }
        /* world geometry first (nw vertices), then the frame's own triangles */
        const int nw = use_world ? s_wg_n * 3 : 0;
        n = use_store ? npick * 3 : nw + sc->n * 3;
        if (n > s_sm_cap) {
            float *p = (float *)realloc(s_sm_buf, (size_t)n * SC_V * sizeof(float));
            if (!p) n = 0; else { s_sm_buf = p; s_sm_cap = n; }
        }
        const float k = 1.0f / H;
        const float hw = 120.0f * aspect * 1.05f, hh = 120.0f * 1.05f;   /* the picture, native px */
        float sx0 = 1e30f, sx1 = -1e30f, sy0 = 1e30f, sy1 = -1e30f;
        int ci = 0;   /* store entry; each gives 3 vertices */
        for (int i = 0; i < n; i++) {
            float P[3], scr[2];
            float *o = s_sm_buf + (size_t)i * SC_V;
            if (i < nw) {
                float V[3]; scc_view(s_wg + (size_t)i * 3, V);
                P[0] = V[0]; P[1] = -V[1]; P[2] = V[2];
                if (V[2] > 1.0f) { scr[0] = s_cam_h * V[0] / V[2]; scr[1] = s_cam_h * V[1] / V[2]; }
                else scr[0] = scr[1] = 1e30f;   /* behind the camera: casts, never on screen */
                o[3] = o[4] = o[5] = o[6] = o[7] = o[8] = o[10] = 0.0f; o[9] = -1.0f;   /* solid */
            } else if (use_store) {
                const ScCached *c = &s_scc[pick[ci]];
                const int j = i % 3;
                float V[3]; scc_view(c->w + j * 3, V);
                if (V[2] < 1.0f) V[2] = 1.0f;
                P[0] = V[0]; P[1] = -V[1]; P[2] = V[2];
                scr[0] = s_cam_h * V[0] / V[2]; scr[1] = s_cam_h * V[1] / V[2];
                o[3] = c->t[j * 2]; o[4] = c->t[j * 2 + 1];
                memcpy(o + 5, c->t + 6, 6 * sizeof(float));
                if (j == 2) ci++;
            } else {
                const float *v = sc->v + (size_t)(i - nw) * SC_V;
                const float z = v[2];
                P[0] = (v[0] - s_sc_cx) * z * k; P[1] = -(v[1] - s_sc_cy) * z * k; P[2] = z;
                scr[0] = v[0] - s_sc_cx; scr[1] = v[1] - s_sc_cy;
                memcpy(o + 3, v + 3, (SC_V - 3) * sizeof(float));
            }
            o[0] = P[0] * a[0] + P[1] * a[1] + P[2] * a[2];
            o[1] = P[0] * b[0] + P[1] * b[1] + P[2] * b[2];
            o[2] = P[0] * L[0] + P[1] * L[1] + P[2] * L[2];
            if (o[0] < x0) x0 = o[0]; if (o[0] > x1) x1 = o[0];
            if (o[1] < y0) y0 = o[1]; if (o[1] > y1) y1 = o[1];
            if (o[2] < d0) d0 = o[2]; if (o[2] > d1) d1 = o[2];
            if (fabsf(scr[0]) <= hw && fabsf(scr[1]) <= hh) {
                if (o[0] < sx0) sx0 = o[0]; if (o[0] > sx1) sx1 = o[0];
                if (o[1] < sy0) sy0 = o[1]; if (o[1] > sy1) sy1 = o[1];
            }
        }
        if (sx1 > sx0 && sy1 > sy0) {   /* the on-screen part, a small margin round it */
            const float mx = (sx1 - sx0) * 0.03f, my = (sy1 - sy0) * 0.03f;
            x0 = fmaxf(x0, sx0 - mx); x1 = fminf(x1, sx1 + mx);
            y0 = fmaxf(y0, sy0 - my); y1 = fminf(y1, sy1 + my);
        }
    }
    GLboolean dmask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_sm_fbo);
    glViewport(0, 0, SM_SIZE, SM_SIZE);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (n > 0 && x1 > x0 && y1 > y0 && d1 > d0) {
        const float sx = 1.0f / (x1 - x0), sy = 1.0f / (y1 - y0), sd = 1.0f / (d1 - d0);
        for (int i = 0; i < n; i++) {
            float *o = s_sm_buf + (size_t)i * SC_V;
            o[0] = (o[0] - x0) * sx * 2.0f - 1.0f;
            o[1] = (o[1] - y0) * sy * 2.0f - 1.0f;
            o[2] = (d1 - o[2]) * sd * 2.0f - 1.0f;
        }
        p_glUseProgram(s_sm_prog);
        p_glBindVertexArray(s_sm_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_sm_vbo);
        p_glBufferData(PSXGL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * SC_V * sizeof(float)), s_sm_buf, PSXGL_STREAM_DRAW);
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_raw_tex);
        p_glUniform1i(s_sm_uvram, 0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDrawArrays(GL_TRIANGLES, 0, n);
        glDisable(GL_DEPTH_TEST);
        glBindTexture(GL_TEXTURE_2D, 0);
        p_glBindVertexArray(0);
        const float u[4][4] = {{a[0], a[1], a[2], x0}, {b[0], b[1], b[2], y0},
                               {L[0], L[1], L[2], d1}, {sx, sy, sd, (float)SM_SIZE}};
        memcpy(s_sm_u, u, sizeof s_sm_u);
    }
    glDepthMask(dmask);
}
static void pfx_shadow_uniforms(GLuint prog) {
    static const char *const names[4] = {"u_smx", "u_smy", "u_smz", "u_sms"};
    for (int k = 0; k < 4; k++) {
        const GLint loc = p_glGetUniformLocation(prog, names[k]);
        if (loc >= 0) p_glUniform4f(loc, s_sm_u[k][0], s_sm_u[k][1], s_sm_u[k][2], s_sm_u[k][3]);
    }
    /* the camera's world axes and the light, when the framework knows them
     * (after the preset's own values, which they replace) */
    static const char *const wn[3] = {"u_world", "u_worldup", "u_shadowdir"};
    for (int k = 0; k < 3; k++) {
        if (s_sm_world[k][3] < 0.5f) continue;
        const GLint loc = p_glGetUniformLocation(prog, wn[k]);
        if (loc >= 0) p_glUniform4f(loc, s_sm_world[k][0], s_sm_world[k][1], s_sm_world[k][2], 1.0f);
    }
}


/* ---- Per-pass GPU timing (TCP postfx_timing) ---------------------------
 * While on, every pass of every stage is bracketed by GL_TIMESTAMP queries
 * (they do not nest-conflict with the frame timers' TIME_ELAPSED); a slot is
 * read three presents later, when the GPU is long done, so nothing stalls.
 * Mean microseconds per pass and per stage since the last reset. */
enum { PT_RING = 3, PT_TS = PFX_MAX_PASSES + 1 };
static int      s_pxt_on = 0, s_pxt_stage = -1, s_pxt_slot = 0;
static GLuint   s_pxt_q[PT_RING][PFX_NSTAGES][PT_TS];
static uint8_t  s_pxt_used[PT_RING][PFX_NSTAGES];   /* timestamps issued: npasses + 1, 0 = none */
static double   s_pxt_sum[PFX_NSTAGES][PFX_MAX_PASSES];
static uint32_t s_pxt_n[PFX_NSTAGES][PFX_MAX_PASSES];
static void pxt_stamp(int k) {
    if (s_pxt_on && s_pxt_stage >= 0 && p_glQueryCounter)
        p_glQueryCounter(s_pxt_q[s_pxt_slot][s_pxt_stage][k], 0x8E28 /*GL_TIMESTAMP*/);
}
/* At the start of a present: collect the slot about to be reused. */
static void pxt_frame_begin(void) {
    if (!s_pxt_on || !p_glQueryCounter) return;
    static int made = 0;
    if (!made) { p_glGenQueries(PT_RING * PFX_NSTAGES * PT_TS, &s_pxt_q[0][0][0]); made = 1; }
    s_pxt_slot = (s_pxt_slot + 1) % PT_RING;
    for (int st = 0; st < PFX_NSTAGES; st++) {
        const int n = s_pxt_used[s_pxt_slot][st];
        if (n < 2) continue;
        GLuint64 t[PT_TS];
        for (int k = 0; k < n; k++) p_glGetQueryObjectui64v(s_pxt_q[s_pxt_slot][st][k], GL_QUERY_RESULT, &t[k]);
        for (int k = 0; k + 1 < n; k++) {
            s_pxt_sum[st][k] += (double)(t[k + 1] - t[k]) * 1e-3;
            s_pxt_n[st][k]++;
        }
        s_pxt_used[s_pxt_slot][st] = 0;
    }
}
void gl_renderer_postfx_timing(int on, int reset) {
    GL_RT_SYNC("postfx_timing");
    if (reset || (on && !s_pxt_on)) { memset(s_pxt_sum, 0, sizeof s_pxt_sum); memset(s_pxt_n, 0, sizeof s_pxt_n); memset(s_pxt_used, 0, sizeof s_pxt_used); }
    if (on >= 0) s_pxt_on = on ? 1 : 0;
}
int gl_renderer_postfx_timing_json(char *out, int cap) {
    GL_RT_SYNC("postfx_timing");
    static const char *const fixed[] = {"bloom", "extra0", "extra1", "extra2", "extra3", "extra4", "user", "aa", "hq"};
    int k = snprintf(out, (size_t)cap, "\"on\":%d,\"stages\":[", s_pxt_on);
    int first = 1;
    for (int st = 0; st < PFX_NSTAGES && k < cap - 512; st++) {
        double tot = 0.0; int any = 0;
        for (int p = 0; p < PFX_MAX_PASSES; p++) if (s_pxt_n[st][p]) { tot += s_pxt_sum[st][p] / s_pxt_n[st][p]; any = 1; }
        if (!any) continue;
        const char *file = st == PFX_STAGE_USER ? s_pfx_user_path
                         : PFX_IS_EXTRA(st) ? s_pfx_extra_path[st - PFX_STAGE_EXTRA0] : "";
        k += snprintf(out + k, (size_t)(cap - k), "%s{\"stage\":\"%s\",\"file\":\"%s\",\"us\":%.1f,\"passes\":[",
                      first ? "" : ",", st < (int)(sizeof fixed / sizeof *fixed) ? fixed[st] : "?", file, tot);
        first = 0;
        int pf = 1;
        for (int p = 0; p < PFX_MAX_PASSES && k < cap - 128; p++) {
            if (!s_pxt_n[st][p]) continue;
            k += snprintf(out + k, (size_t)(cap - k), "%s{\"pass\":\"%s\",\"us\":%.1f}", pf ? "" : ",",
                          s_pfx[st].loaded && p < s_pfx[st].chain.npasses ? s_pfx[st].chain.pass[p].name : "?",
                          s_pxt_sum[st][p] / s_pxt_n[st][p]);
            pf = 0;
        }
        k += snprintf(out + k, (size_t)(cap - k), "]}");
    }
    k += snprintf(out + k, (size_t)(cap - k), "]");
    return k;
}

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
        pxt_stamp(i);
        /* The image this pass reads, before its target is bound. */
        int reads_shadow = 0;
        for (int k = 0; k < ps->ninputs; k++)
            if (ps->in[k].from == PFX_IN_SHADOW) reads_shadow = 1;
        if (reads_shadow) pfx_shadow_build(ps, (float)lw / (float)lh);
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
                           : from == PFX_IN_SHADOW ? s_sm_tex
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
        if (reads_shadow) pfx_shadow_uniforms(prog);   /* after the preset's own */
        if (ps->blend == PFX_BLEND_SCREEN) {
            glEnable(GL_BLEND);
            p_glBlendEquationSeparate(PSXGL_FUNC_ADD, PSXGL_FUNC_ADD);
            p_glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
        }
        post_fullscreen(prog, L->fixed[PFX_LOC_UV]);
        if (ps->blend != PFX_BLEND_REPLACE) glDisable(GL_BLEND);
    }
    pxt_stamp(c->npasses);
    if (s_pxt_on && s_pxt_stage >= 0) s_pxt_used[s_pxt_slot][s_pxt_stage] = (uint8_t)(c->npasses + 1);
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
    const int mark_shown = fresh && s_sc_on;   /* after this present's shadow map */
    if (with_aa) pxt_frame_begin();   /* real frames only */
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
        s_pxt_stage = with_aa ? i : -1;
        for (int r = 0; r < reps; r++) ran = pfx_stage_run(st, !with_aa, lx, ly, lw, lh);
        s_pxt_stage = -1;
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
    if (mark_shown) sc_mark_shown();
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
