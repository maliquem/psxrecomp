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
 * program build/cache, uniform binding (u_rcp, u_size, u_org, u_time: seconds,
 * wall clock; and the preset's uniforms) and GL state: everything the pass
 * touches is put back.
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
enum { PFX_LOC_RCP, PFX_LOC_SIZE, PFX_LOC_ORG, PFX_LOC_UV, PFX_LOC_TIME, PFX_LOC_FIXED };
/* u_time: seconds since the first present (wall clock, so flickers and other
 * motion keep their pace whatever the frame rate), wrapped at an hour. */
static float pfx_time_now(void) {
    static double t0 = -1.0;
    struct timespec ts;
    if (!timespec_get(&ts, TIME_UTC)) return 0.0f;
    const double t = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    if (t0 < 0.0) t0 = t;
    return (float)fmod(t - t0, 3600.0);
}
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
/* A Smooth motion generated frame: where its redraw left no depth (what the
 * in-between camera uncovers shows the newer real frame's pixels, copied),
 * the depth input takes that real frame's depth there (postfx_note_fill*):
 * without it the effects saw no scene there and left it unlit (a dark band
 * that blinked while the camera turned). Source rects as s_mask_src/ctr. */
static GLuint s_dfill_fbo = 0, s_dfill_ctr_fbo = 0;
static int    s_dfill[4], s_dfill_ctr[4], s_dfill_valid = 0, s_dfill_ctr_valid = 0;
static float  s_dfill_ctr_x0 = 0.0f, s_dfill_ctr_x1 = 0.0f;
static GLuint s_dep2_tex = 0, s_dep2_fbo = 0, s_dfill_prog = 0;
static GLint  s_dfill_uMode = -1, s_dfill_uD = -1, s_dfill_uA = -1, s_dfill_uB = -1, s_dfill_uNat = -1, s_dfill_uH = -1;
static int    s_dep2_w = 0, s_dep2_h = 0;
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
    if (hdr_mask_wanted()) want3d = 1;    /* HDR: lights with no opaque 2D over them, the scenery (gl_hdr.inc) */
    int wantd = 0, wants = 0, wantw = 0;
    for (int i = 0; i < PFX_NSTAGES; i++)
        if (pfx_stage_on(i) && s_pfx[i].loaded && s_pfx[i].chain.uses_depth) wantd = 1;
    for (int i = 0; i < PFX_NSTAGES; i++)
        if (pfx_stage_on(i) && s_pfx[i].loaded && s_pfx[i].chain.uses_shadow) wants = 1;
    for (int i = 0; i < PFX_NSTAGES; i++)
        if (pfx_stage_on(i) && s_pfx[i].loaded && s_pfx[i].chain.uses_world) wantw = 1;
    int wspr = 0;
    for (int i = 0; i < PFX_NSTAGES; i++)
        if (pfx_stage_on(i) && s_pfx[i].loaded && s_pfx[i].chain.uses_sprite) wspr = 1;
    s_spr_on = wspr;
    if (hdr_boost_wanted()) wantw = 1;    /* the HDR boost: the frame's light pieces (sc_add_emit) */
    if (wantw && !s_sc_on) gl_renderer_shadow_cache_clear();   /* switched on: a fresh memory */
    s_sc_on = wantw;
    s_sun_on = wants;
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
                             "uniform vec2 u_rcp; uniform vec2 u_size; uniform ivec2 u_org; uniform float u_time;\n");
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

/* A generated frame (s_pfx_vx_on): what the chains get in the real frame's
 * view space, in the picture's. A direction turns; a point turns and moves;
 * a mapping row (dot(xyz, P) - w) keeps its value at the same scene point. */
static void pfx_vx_dir(const float *d, float *o) {
    const float *A = s_pfx_vx_a;
    const float x = d[0], y = d[1], z = d[2];
    for (int r = 0; r < 3; r++) o[r] = A[r * 3] * x + A[r * 3 + 1] * y + A[r * 3 + 2] * z;
}
static void pfx_vx_point(const float *p, float *o) {
    pfx_vx_dir(p, o);
    for (int r = 0; r < 3; r++) o[r] += s_pfx_vx_b[r];
}
static void pfx_vx_row(const float *row, float *o) {
    pfx_vx_dir(row, o);
    o[3] = row[3] + o[0] * s_pfx_vx_b[0] + o[1] * s_pfx_vx_b[1] + o[2] * s_pfx_vx_b[2];
}

/* The holes' depth for the next present only (a generated frame's newer real
 * frame, as postfx_note_source / postfx_note_center take theirs). */
static void postfx_note_fill(GLuint fbo, GLuint tex, float tex_w, float tex_h, int x, int y, int w, int h) {
    s_dfill_valid = 0;
    s_dfill_ctr_valid = 0;
    if (!s_dep_on || !fbo || !tex || tex_w <= 0.0f || tex_h <= 0.0f) return;
    int tw, th;
    pfx_rect_px(tex, tex_w, tex_h, x, y, w, h, s_dfill, &tw, &th);
    s_dfill_fbo = fbo;
    s_dfill_valid = 1;
}
static void postfx_note_fill_center(GLuint fbo, GLuint tex, float tex_w, float tex_h,
                                    int x, int y, int w, int h, int off, int wide_w) {
    s_dfill_ctr_valid = 0;
    if (!s_dfill_valid || !fbo || !tex || wide_w <= 0) return;
    int tw, th;
    pfx_rect_px(tex, tex_w, tex_h, x, y, w, h, s_dfill_ctr, &tw, &th);
    s_dfill_ctr_fbo = fbo;
    s_dfill_ctr_x0 = (float)off / (float)wide_w;
    s_dfill_ctr_x1 = (float)(off + w) / (float)wide_w;
    s_dfill_ctr_valid = 1;
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
/* s_dep_tex's holes (still the clear depth, 1.0) take s_dep2_tex's depth:
 * the holes are marked in s_dep_tex's stencil (a quad at depth 1.0 passes
 * GL_EQUAL there), then a quad writes the other depth where marked. With the
 * in-between camera's map (s_pfx_vx_on, mode 2) a hole pixel takes the point
 * of the real frame that the in-between camera sees there (found by a few
 * steps: its depth, to view space, through the map, back on the picture,
 * moved by what is left), at the in-between camera's depth: the real
 * frame's depth as it was lit the uncovered strips along the edges wrong
 * while the camera turned (a seam that blinked). Depth <-> view z as the
 * chains read it: SZ = 256 / (1 - d) - 256. */
static void pfx_depth_fill(int lw, int lh) {
    if (!s_dfill_prog) {
        s_dfill_prog = build_program(PRESENT_VS,
            "#version 330\nin vec2 v_uv; out vec4 o; uniform int u_mode; uniform sampler2D u_d;\n"
            "uniform vec3 u_A0, u_A1, u_A2, u_B; uniform vec2 u_nat; uniform float u_H;\n"
            "vec3 pos_at(vec2 uv, float z) { return vec3((uv - 0.5) * u_nat * z / u_H, z); }\n"
            "vec3 vx(vec3 p) { return vec3(dot(u_A0, p), dot(u_A1, p), dot(u_A2, p)) + u_B; }\n"
            "void main(){\n"
            "  o = vec4(0.0);\n"
            "  if (u_mode == 0) { gl_FragDepth = 1.0; return; }\n"
            "  if (u_mode == 1) { gl_FragDepth = texelFetch(u_d, ivec2(gl_FragCoord.xy), 0).r; return; }\n"
            "  vec2 sz = vec2(textureSize(u_d, 0)), uv = gl_FragCoord.xy / sz, su = uv;\n"
            "  for (int i = 0; i < 4; i++) {\n"
            "    float d = textureLod(u_d, su, 0.0).r;\n"
            "    if (d >= 0.99999) break;\n"
            "    vec3 g = vx(pos_at(su, 256.0 / (1.0 - d) - 256.0));\n"
            "    if (g.z < 1.0) break;\n"
            "    su = clamp(su + uv - (g.xy * u_H / (g.z * u_nat) + 0.5), vec2(0.0), vec2(1.0));\n"
            "  }\n"
            "  float d = textureLod(u_d, su, 0.0).r;\n"
            "  if (d >= 0.99999) { gl_FragDepth = 1.0; return; }\n"
            "  vec3 g = vx(pos_at(su, 256.0 / (1.0 - d) - 256.0));\n"
            "  gl_FragDepth = g.z < 1.0 ? d : 1.0 - 256.0 / (g.z + 256.0);\n"
            "}\n");
        if (!s_dfill_prog) return;
        s_dfill_uMode = p_glGetUniformLocation(s_dfill_prog, "u_mode");
        s_dfill_uD = p_glGetUniformLocation(s_dfill_prog, "u_d");
        s_dfill_uA = p_glGetUniformLocation(s_dfill_prog, "u_A0");   /* u_A1, u_A2 follow by name */
        s_dfill_uB = p_glGetUniformLocation(s_dfill_prog, "u_B");
        s_dfill_uNat = p_glGetUniformLocation(s_dfill_prog, "u_nat");
        s_dfill_uH = p_glGetUniformLocation(s_dfill_prog, "u_H");
    }
    if (s_dep2_w != lw || s_dep2_h != lh) {
        if (!s_dep2_tex) glGenTextures(1, &s_dep2_tex);
        glBindTexture(GL_TEXTURE_2D, s_dep2_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, PSXGL_DEPTH24_STENCIL8, lw, lh, 0,
                     0x84F9 /*DEPTH_STENCIL*/, 0x84FA /*UNSIGNED_INT_24_8*/, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!s_dep2_fbo) p_glGenFramebuffers(1, &s_dep2_fbo);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_dep2_fbo);
        p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_dep2_tex, 0);
        glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
        s_dep2_w = lw; s_dep2_h = lh;
    }
    /* the real frame's depth, mapped to the window as the source's */
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_dep2_fbo);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_dfill_fbo);
    p_glBlitFramebuffer(s_dfill[0], s_dfill[1], s_dfill[2], s_dfill[3], 0, lh, lw, 0, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (s_dfill_ctr_valid) {
        const int c0 = (int)lrintf(s_dfill_ctr_x0 * (float)lw), c1 = (int)lrintf(s_dfill_ctr_x1 * (float)lw);
        if (c1 > c0) {
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_dfill_ctr_fbo);
            p_glBlitFramebuffer(s_dfill_ctr[0], s_dfill_ctr[1], s_dfill_ctr[2], s_dfill_ctr[3],
                                c0, lh, c1, 0, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    GLint dfunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &dfunc);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_dep_fbo);
    glDrawBuffer(GL_NONE);
    glViewport(0, 0, lw, lh);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    p_glUseProgram(s_dfill_prog);
    p_glBindVertexArray(s_empty_vao);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    /* the holes: stencil 1 */
    p_glUniform1i(s_dfill_uMode, 0);
    glDepthFunc(GL_EQUAL);
    glDepthMask(GL_FALSE);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    /* the real frame's depth there (at the in-between camera's, mode 2) */
    const int remap = s_pfx_vx_on && s_cam_h > 0.0f;
    p_glUniform1i(s_dfill_uMode, remap ? 2 : 1);
    if (remap) {
        static const char *const an[3] = {"u_A0", "u_A1", "u_A2"};
        for (int r = 0; r < 3; r++)
            p_glUniform3f(r ? p_glGetUniformLocation(s_dfill_prog, an[r]) : s_dfill_uA,
                          s_pfx_vx_a[3 * r], s_pfx_vx_a[3 * r + 1], s_pfx_vx_a[3 * r + 2]);
        p_glUniform3f(s_dfill_uB, s_pfx_vx_b[0], s_pfx_vx_b[1], s_pfx_vx_b[2]);
        p_glUniform2f(s_dfill_uNat, 240.0f * (float)lw / (float)lh, 240.0f);
        p_glUniform1f(s_dfill_uH, s_cam_h);
    }
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_dep2_tex);
    p_glUniform1i(s_dfill_uD, 0);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc((GLenum)dfunc);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
}
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
        if (s_dfill_valid) pfx_depth_fill(lw, lh);
        s_dep_ok = 1;
    }
    return s_dep_ok;
}

/* A dim grey glowing piece drawn in the scene loses the see-through cover mark
 * (mask3d blue) over its triangles: it adds little light (under UNC_DIM) and
 * grey (its weakest channel over half its strongest: dust, smoke; BoF4's
 * running dust adds about 0.3 of each, a campfire 0.42 0.27 0, spells more)
 * and 3D scenery was drawn after it, as the light sources count it
 * (pfx_lights_update: the HUD,
 * drawn over the finished scene, keeps it). The effects then light it with the
 * ground under it, as the textured cover pass already does for 3D pieces (it
 * stood out as a light where they darkened the ground). The scene-capture
 * pieces (sc_add_emit) are kept while a chain reads the shadow map or the
 * sprites. */
enum { UNC_MAX = 1024 };
#define UNC_DIM 0.4f
/* a glowing piece that is dim and grey (dust, smoke), not a light */
static int emit_dim_grey(const float *e) {
    const float mx = fmaxf(e[5], fmaxf(e[6], e[7])), mn = fminf(e[5], fminf(e[6], e[7]));
    return mx * e[14] < UNC_DIM && mx - mn < 0.5f * mx;
}
static GLuint s_unc_prog = 0, s_unc_vao = 0, s_unc_vbo = 0;
static float  s_unc_buf[UNC_MAX * 6];
static int    s_unc_last = 0;   /* TCP shadow_cache "uncovered": the last mask's pieces */
static void pfx_mask_uncover(int lw, int lh) {
    s_unc_last = 0;
    const ScSlot *sc = sc_presented();
    if (!sc || sc->en <= 0 || s_sc_nh <= 0.0f || lw <= 0 || lh <= 0) return;
    const float k = (float)lh / s_sc_nh, after_min = fmaxf(4.0f, 0.03f * (float)sc->n);
    int n = 0;
    for (int i = 0; i < sc->en && n < UNC_MAX; i++) {
        const float *e = sc->ev + (size_t)i * SC_EV;
        if (!emit_dim_grey(e)) continue;                                     /* a light */
        if ((float)sc->n - e[4] < after_min) continue;                      /* over the scene */
        const float cx = (e[8] + e[10] + e[12]) / 3.0f, cy = (e[9] + e[11] + e[13]) / 3.0f;
        for (int v = 0; v < 3; v++) {   /* native px from the picture's centre -> NDC (y up) */
            /* grown 1 native px from its centre: the drawn piece sits up to
             * half a pixel off (the rasteriser's sample grid), which left a
             * lit sliver at one edge */
            float x = e[8 + 2 * v], y = e[9 + 2 * v];
            const float dx = x - cx, dy = y - cy, dl = sqrtf(dx * dx + dy * dy);
            if (dl > 1e-3f) { x += dx / dl; y += dy / dl; }
            s_unc_buf[n * 6 + 2 * v] = 2.0f * (x - s_sc_cx) * k / (float)lw;
            s_unc_buf[n * 6 + 2 * v + 1] = -2.0f * (y - s_sc_cy) * k / (float)lh;
        }
        n++;
    }
    s_unc_last = n;
    if (!n) return;
    if (!s_unc_prog) {
        s_unc_prog = build_program(
            "#version 330\nlayout(location = 0) in vec2 a_p;\nvoid main(){ gl_Position = vec4(a_p, 0.0, 1.0); }\n",
            "#version 330\nout vec4 o;\nvoid main(){ o = vec4(0.0); }\n");
        if (!s_unc_prog) return;
        p_glGenVertexArrays(1, &s_unc_vao);
        p_glGenBuffers(1, &s_unc_vbo);
        p_glBindVertexArray(s_unc_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_unc_vbo);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
        p_glEnableVertexAttribArray(0);
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_mask_fbo);
    glViewport(0, 0, lw, lh);
    glColorMask(GL_FALSE, GL_FALSE, GL_TRUE, GL_FALSE);   /* the cover mark only */
    p_glUseProgram(s_unc_prog);
    p_glBindVertexArray(s_unc_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_unc_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * 6 * sizeof(float)), s_unc_buf, PSXGL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, n * 3);
    p_glBindVertexArray(0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
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
        pfx_mask_uncover(lw, lh);
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
 * there (0 = nearest the light). No casters: the map is cleared (no shadow).
 * The map is SM_SIZE texels square, or what the pass's uniform u_smsize asks
 * (1024..8192, rounded to a power of two). */
enum { SM_SIZE = 2048 };
static int s_sm_size = 0;   /* allocated size */
static GLuint s_sm_tex = 0, s_sm_fbo = 0, s_sm_prog = 0, s_sm_vao = 0, s_sm_vbo = 0;
static GLint  s_sm_uvram = -1;
/* The fire lights' shadow map (`firemap`, pfx_fire_build): per light a row of
 * cube faces, FIRE_FACE px each (3 x 2 tiles), up to FIRE_MAX lights stacked. */
enum { FIRE_MAX = 2, FIRE_FACE = 512 };
static GLuint s_fm_tex = 0, s_fm_fbo = 0, s_fm_prog = 0;
static GLint  s_fm_uvram = -1, s_fm_uatl = -1;
static unsigned s_fire_built = ~0u;
static float  s_sm_u[4][4];
/* The sprites' own map (`sprshadow`): same light, size and mapping as the
 * scenery's, so a reader can soften the scenery's shadows and keep the
 * sprites' pixel-art silhouettes sharp. */
static GLuint s_sm2_tex = 0, s_sm2_fbo = 0;
/* ... unless the sun is higher than the pass's u_sprsun.x (degrees): a sprite
 * is an upright plane, and a sun near the zenith casts it as a line at its
 * feet (it floats). The sprites then take that elevation (same azimuth), a
 * short shadow from their feet, and their map its own mapping (u_ssx, u_ssy,
 * u_ssz, u_sss, as u_smx..u_sms; w 0 = the scenery's). */
static float  s_ss_u[4][4];
/* In that mode a sprite's foot point is dropped onto the surface under it:
 * the game's point can sit a little above the floor drawn there (BoF4: Cray
 * on the sandflier's deck), hidden in a long shadow but floating under a
 * high sun. s_spr_drop_h: the H the frame's triangles are placed with (0 = off). */
static float  s_spr_drop_h = 0.0f;
/* Does the scene's geometry (gl_renderer_shadow_world: a mod's map) lie
 * under what the picture draws? A cutscene can draw its own floor away from
 * the map (BoF4's sandflier crash), which then casts shadows nobody sees the
 * cause of. Up to WG_SAMPLES of the frame's opaque 3D vertices, taken to the
 * world: how many fall within a world triangle's footprint (across the floor)
 * and how many of those lie on it (within WG_ON units along up). */
enum { WG_SAMPLES = 96 };
#define WG_ON 24.0f
static int s_wg_in = 0, s_wg_on = 0;
/* the map is used while at least WG_TRUST_ON of those lie on it; it is set
 * aside after WG_TRUST_N presents in a row with fewer, back after as many
 * with enough (no flicker at a cut) */
enum { WG_TRUST_ON = 3, WG_TRUST_N = 3 };
static int s_wg_trust = 1, s_wg_run = 0;
static void wg_agree(const ScSlot *sc, float H) {
    s_wg_in = s_wg_on = 0;
    if (!sc || sc->n <= 0 || s_wg_n <= 0 || H <= 0.0f) return;
    const float *r = s_cam_r, *t = s_cam_t, k = 1.0f / H;
    const int step = sc->n > WG_SAMPLES ? sc->n / WG_SAMPLES : 1;
    for (int i = 0; i < sc->n; i += step) {
        const float *v = sc->v + (size_t)i * 3 * SC_V;
        if (v[9] == -2.0f) continue;   /* a culled face */
        const float z = v[2];
        const float V[3] = {(v[0] - s_sc_cx) * z * k - t[0], (v[1] - s_sc_cy) * z * k - t[1], z - t[2]};   /* GTE view (y down) less T */
        float w[3];
        for (int c = 0; c < 3; c++) w[c] = r[c] * V[0] + r[3 + c] * V[1] + r[6 + c] * V[2];
        int in = 0, on = 0;
        for (int j = 0; j < s_wg_n && !on; j++) {
            const float *a = s_wg + (size_t)j * 9, *b = a + 3, *c = a + 6;
            /* footprint across the floor: world x, z (GTE y is up/down) */
            const float d = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
            if (fabsf(d) < 1e-3f) continue;
            const float l1 = ((w[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (w[2] - a[2])) / d;
            const float l2 = ((b[0] - a[0]) * (w[2] - a[2]) - (w[0] - a[0]) * (b[2] - a[2])) / d;
            if (l1 < 0.0f || l2 < 0.0f || l1 + l2 > 1.0f) continue;
            in = 1;
            const float y = a[1] + l1 * (b[1] - a[1]) + l2 * (c[1] - a[1]);
            if (fabsf(y - w[1]) < WG_ON) on = 1;
        }
        s_wg_in += in; s_wg_on += on;
    }
}
static void wg_trust_update(void) {
    const int ok = s_wg_on >= WG_TRUST_ON;
    if (ok == s_wg_trust) { s_wg_run = 0; return; }
    if (++s_wg_run >= WG_TRUST_N) { s_wg_trust = ok; s_wg_run = 0; }
}
static unsigned s_spr_dropped = 0;   /* TCP: foot points dropped */
/* The nearest surface of the frame's 3D triangles under view-space point A
 * (y up) along -u, from 8 units over it to 128 under: how far down (0 = none). */
static float spr_drop(const ScSlot *sc, const float *A, const float *u) {
    if (!sc || s_spr_drop_h <= 0.0f) return 0.0f;
    const float k = 1.0f / s_spr_drop_h, d[3] = {-u[0], -u[1], -u[2]};
    const float O[3] = {A[0] + u[0] * 8.0f, A[1] + u[1] * 8.0f, A[2] + u[2] * 8.0f};
    float best = 1e30f;
    for (int t = 0; t < sc->n; t++) {
        float P[3][3];
        for (int j = 0; j < 3; j++) {
            const float *v = sc->v + ((size_t)t * 3 + j) * SC_V;
            P[j][0] = (v[0] - s_sc_cx) * v[2] * k; P[j][1] = -(v[1] - s_sc_cy) * v[2] * k; P[j][2] = v[2];
        }
        const float e1[3] = {P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2]};
        const float e2[3] = {P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2]};
        const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
        const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (fabsf(det) < 1e-6f) continue;
        const float id = 1.0f / det;
        const float s[3] = {O[0] - P[0][0], O[1] - P[0][1], O[2] - P[0][2]};
        const float bu = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * id;
        if (bu < 0.0f || bu > 1.0f) continue;
        const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
        const float bv = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * id;
        if (bv < 0.0f || bu + bv > 1.0f) continue;
        const float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * id;
        if (tt >= 0.0f && tt < best) best = tt;
    }
    return best <= 136.0f ? fmaxf(best - 8.0f, 0.0f) : 0.0f;
}
static float *s_sm_pos = NULL; static int s_sm_pcap = 0;   /* each caster vertex, view space (y up) */
static float  s_sm_world[3][4];   /* u_world, u_worldup, u_shadowdir (view space, y up; w 1 = set) */
static float *s_sm_buf = NULL; static int s_sm_cap = 0;
/* Sprites standing in the scene (gl_renderer_shadow_anchor): their pieces as
 * casters (view space) and their view depth on the picture (`sprite`, R32F,
 * SPR_NONE where no sprite is). */
enum { SPR_NONE = 60000 };
static GLuint s_spr_tex = 0, s_spr_fbo = 0, s_spr_prog = 0;
static GLint  s_spr_uvram = -1, s_sm_uatl = -1, s_spr_uatl = -1;
static GLuint s_spr_atl_tex = 0;   /* the presented slot's sprite cut-outs (R8, SC_ATL square) */
static int    s_spr_w = 0, s_spr_h = 0, s_spr_n = 0;
static float *s_spr_cast = NULL, *s_spr_scr = NULL; static int s_spr_cap = 0;
static float *s_spr_cau = NULL;   /* per caster vertex: its sprite's floor height (dot with world up) */
static const float *pfx_pass_uniform(const PfxPass *ps, const char *name) {
    for (int k = 0; k < ps->nuniforms; k++) if (!strcmp(ps->u[k].name, name)) return ps->u[k].v;
    return NULL;
}
/* Union-find root of piece i (path halving). */
static int spr_root(int *par, int i) {
    while (par[i] != i) i = par[i] = par[par[i]];
    return i;
}
/* Where the picture point (x, y) (native px from the centre, y down) meets the
 * floor at height au along the world up u (view space, y up): 0 if never. */
static int spr_ground(float x, float y, float au, const float *u, float *G) {
    const float d[3] = {x / s_cam_h, -y / s_cam_h, 1.0f};
    const float du = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    if (fabsf(du) < 1e-4f) return 0;
    const float t = au / du;
    if (t <= 0.0f) return 0;
    for (int k = 0; k < 3; k++) G[k] = t * d[k];
    return 1;
}
enum { SPR_COLS = 136, SPR_ZONE = 64, SPR_FOOT_MIN = 3, SPR_MIN_H = 12, SPR_AIR = 64 };
/* A sprite's bottom, from its cut-outs: col, the lowest drawn pixel of each
 * picture column (native px from the centre, below the pixel; X0 the first
 * column, W of them); mask, its drawn pixels in the SPR_ZONE rows above Yb
 * (row k = VRAM row Yb - 1 - k); lo, its lowest; band, how far above lo a
 * column's bottom is still a foot (its bottom fifth). */
typedef struct { int X0, W, Yb; float lo, band; float col[SPR_COLS]; uint8_t mask[SPR_ZONE][SPR_COLS]; } SprFoot;
/* TCP, debugging the feet (u_ss.z, u_ss.w; see pfx_shadow_build): 1 = no
 * bend; the bent zone's height (native px, 0 = by the sprite's height) */
static int   s_spr_bend_dbg = 0;
static float s_spr_bend_h = 0.0f;
/* Feet bent down to the floor: as casters (s_spr_xc) and on the
 * picture (s_spr_xs, with the standing pieces' depth, the nearest winning). */
static float *s_spr_xc = NULL, *s_spr_xs = NULL; static int s_spr_nx = 0, s_spr_xcap = 0;
static float *s_spr_xau = NULL;   /* per bent-foot caster vertex: its floor height */
static float  s_spr_kx = 0.0f, s_spr_ky = 0.0f;
/* The bottom of sprite r (pieces i with root r) into f; 0 = not known. */
static int spr_feet(const ScSlot *sc, const int *ids, int *par, int r, int m,
                    float bx0, float bx1, float by0, float ob, SprFoot *f) {
    if (!sc->atl) return 0;
    const int X0 = (int)floorf(bx0 + s_sc_cx), W = (int)ceilf(bx1 + s_sc_cx) - X0;
    if (W < 3 || W > SPR_COLS) return 0;
    float *col = f->col;
    const int Yb = (int)lroundf(ob + s_sc_cy);
    f->X0 = X0; f->W = W; f->Yb = Yb;
    memset(f->mask, 0, sizeof f->mask);
    for (int x = 0; x < W; x++) col[x] = -1e30f;
    for (int i = r; i < m; i++) {
        if (spr_root(par, i) != r) continue;
        const float *v = sc->sv + (size_t)ids[i] * 3 * SC_V;
        if (v[9] != 3.0f) continue;
        const float x0 = v[0], y0 = v[1], x1 = v[SC_V], y1 = v[SC_V + 1], x2 = v[2 * SC_V], y2 = v[2 * SC_V + 1];
        const float ar = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (fabsf(ar) < 1e-3f) continue;
        const int px0 = (int)floorf(fminf(x0, fminf(x1, x2))), px1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
        const int py0 = (int)floorf(fminf(y0, fminf(y1, y2))), py1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
        for (int Y = py0; Y < py1; Y++)
            for (int X = px0; X < px1; X++) {
                if (X - X0 < 0 || X - X0 >= W) continue;
                const float px = X + 0.5f, py = Y + 0.5f;
                const float w1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / ar;
                const float w2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / ar;
                const float w0 = 1.0f - w1 - w2;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                const int tu = (int)floorf(w0 * v[3] + w1 * v[SC_V + 3] + w2 * v[2 * SC_V + 3]);
                const int tv = (int)floorf(w0 * v[4] + w1 * v[SC_V + 4] + w2 * v[2 * SC_V + 4]);
                if (tu < 0 || tv < 0 || tu >= SC_ATL || tv >= SC_ATL || !sc->atl[(size_t)tv * SC_ATL + tu]) continue;
                const float b = (float)(Y + 1) - s_sc_cy;
                if (b > col[X - X0]) col[X - X0] = b;
                if (Yb - 1 - Y >= 0 && Yb - 1 - Y < SPR_ZONE) f->mask[Yb - 1 - Y][X - X0] = 1;
            }
    }
    float lo = -1e30f;
    for (int x = 0; x < W; x++) lo = fmaxf(lo, col[x]);
    if (lo < -1e29f || fabsf(lo - ob) > 4.0f) return 0;
    f->lo = lo;
    f->band = fminf(14.0f, fmaxf(3.0f, 0.2f * (lo - by0)));
    return 1;
}
/* Every foot bent down to the floor, as extra casters: a sprite stands on one
 * plane (through its lowest drawn pixel's floor point, turned toward a light
 * across the picture), so a foot drawn higher (a 3/4 pose's back foot) or to
 * one side of a turned plane would float or sink there, its shadow off the
 * foot. Each column whose bottom is a foot's (within f->band of the lowest,
 * in a run of SPR_FOOT_MIN such columns or more) has its drawn pixels over the zone above that bottom moved from the plane
 * (na_d along nrm) toward one through where that bottom meets the floor (at
 * height au along u), smoothly from none at the zone's top to all of it at
 * the bottom. The leg's shadow meets the foot; the sprite's own pieces still
 * cast as they stand. The zone is the whole leg and the hips (3/4 of the
 * sprite's height): bending only the foot's end (a third, at most 24 px)
 * left the back leg's top on the plane, its shadow apart from the bent
 * bottom's, so a low sun drew four legs. */
static void spr_bend(const SprFoot *f, const float *nrm, float na_d, float au, const float *u, float height) {
    const float h = s_cam_h;
    const float hz = s_spr_bend_h > 0.0f ? s_spr_bend_h : fmaxf(0.75f * height, 8.0f);
    const float hb = fminf(hz, (float)SPR_ZONE - f->band - 2.0f);
    if (hb < 2.0f) return;
    int any = 0, run = 0;
    for (int x = 0; x < f->W; x++) {
        const float cb = f->col[x];
        if (cb < f->lo - f->band) { run = 0; continue; }
        /* a foot: SPR_FOOT_MIN columns or more (not a staff's or sword's tip) */
        if (!run) {
            while (x + run < f->W && f->col[x + run] >= f->lo - f->band) run++;
            if (run < SPR_FOOT_MIN) { x += run - 1; run = 0; continue; }
        }
        run--;
        float G[3];
        if (!spr_ground((float)(f->X0 + x) + 0.5f - s_sc_cx, cb, au, u, G)) continue;
        const float na_b = nrm[0] * G[0] + nrm[1] * G[1] + nrm[2] * G[2];
        if (fabsf(na_b - na_d) < 2.0f || fabsf(na_b - na_d) > 256.0f) continue;
        const float top = cb - hb;
        for (float y = top; y < cb - 0.5f; y += 1.0f) {
            const int k = f->Yb - 1 - (int)lroundf(y + s_sc_cy);
            if (k < 0 || k >= SPR_ZONE || !f->mask[k][x]) continue;
            if (s_spr_nx + 6 > s_spr_xcap) {
                const int nc = s_spr_xcap ? s_spr_xcap * 2 : 1536;
                float *p = (float *)realloc(s_spr_xc, (size_t)nc * SC_V * sizeof(float));
                if (p) s_spr_xc = p;
                float *q = (float *)realloc(s_spr_xs, (size_t)nc * SC_V * sizeof(float));
                if (q) s_spr_xs = q;
                float *r = (float *)realloc(s_spr_xau, (size_t)nc * sizeof(float));
                if (r) s_spr_xau = r;
                if (!p || !q || !r) return;
                s_spr_xcap = nc;
            }
            const float xl = (float)(f->X0 + x) - s_sc_cx, xr = xl + 1.0f;
            any = 1;
            const float qx[6] = {xl, xr, xr, xl, xr, xl}, qy[6] = {y, y, y + 1.0f, y, y + 1.0f, y + 1.0f};
            for (int j = 0; j < 6; j++) {
                float t = (qy[j] - top) / hb;
                t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
                t = t * t * (3.0f - 2.0f * t);
                const float nb = na_d + (na_b - na_d) * t;
                /* the caster a pixel wider each side, so neighbouring columns
                 * (bent to other depths) leave no gaps in the shadow; on the
                 * picture, the pixel itself */
                const float xc = qx[j] + (qx[j] > xl ? 1.0f : -1.0f);
                const float dc[3] = {xc / h, -qy[j] / h, 1.0f}, d[3] = {qx[j] / h, -qy[j] / h, 1.0f};
                const float zc = nb / (nrm[0] * dc[0] + nrm[1] * dc[1] + nrm[2] * dc[2]);
                const float z = nb / (nrm[0] * d[0] + nrm[1] * d[1] + nrm[2] * d[2]);
                float *c = s_spr_xc + (size_t)(s_spr_nx + j) * SC_V;
                memset(c, 0, SC_V * sizeof(float));
                c[0] = dc[0] * zc; c[1] = dc[1] * zc; c[2] = zc; c[9] = -1.0f;   /* solid */
                float *o = s_spr_xs + (size_t)(s_spr_nx + j) * SC_V;
                memset(o, 0, SC_V * sizeof(float));
                o[0] = qx[j] * s_spr_kx; o[1] = -qy[j] * s_spr_ky; o[2] = z; o[9] = -1.0f;
                s_spr_xau[s_spr_nx + j] = au;
            }
            s_spr_nx += 6;
        }
    }
    if (any) s_spr_bent++;
}
/* The frame's 2D pieces standing at its sprites' foot points: the pieces of
 * one sprite (a run drawn together, see below) go to the foot point at their
 * bottom, and each is put on that sprite's upright plane (through the foot
 * point, facing the camera across the floor, turned a little toward a light
 * that comes across the picture) along its own view ray. Out: s_spr_n
 * vertices, as casters in view space (s_spr_cast) and on the picture
 * (s_spr_scr: clip x, y, view z). */
#define SPR_LIGHT_MIN 0.7f   /* least cosine between a sprite's plane normal and the level light */
static unsigned s_pfx_presents = 0, s_spr_built = ~0u;   /* the `sprite` input built this present? */
/* The CLUTs of the sprites seen standing on the floor (a run at its foot
 * point, SPR_MIN_H tall or more) in the last SPR_CLUT_KEEP builds: only those
 * stand off the floor (a jump), never a UI palette (its bars are smaller). */
enum { SPR_CLUT_N = 32, SPR_CLUT_KEEP = 600 };
static float    s_sprc_code[SPR_CLUT_N];
static unsigned s_sprc_seen[SPR_CLUT_N], s_sprc_build = 0;
static int      s_sprc_n = 0;
static float spr_clut_code(const ScSlot *sc, int id) {
    const float *v = sc->sv + (size_t)id * 3 * SC_V;
    return v[8] * 64.0f + v[7] / 16.0f;
}
static void sprc_note(float code) {
    int k = 0, old = 0;
    while (k < s_sprc_n && s_sprc_code[k] != code) k++;
    if (k == s_sprc_n) {
        if (s_sprc_n < SPR_CLUT_N) s_sprc_n++;
        else { for (int i = 1; i < SPR_CLUT_N; i++) if (s_sprc_seen[i] < s_sprc_seen[old]) old = i; k = old; }
        s_sprc_code[k] = code;
    }
    s_sprc_seen[k] = s_sprc_build;
}
static int sprc_known(float code) {
    for (int k = 0; k < s_sprc_n; k++)
        if (s_sprc_code[k] == code && s_sprc_build - s_sprc_seen[k] <= SPR_CLUT_KEEP) return 1;
    return 0;
}
static void pfx_sprites_stand(const ScSlot *sc, float aspect, const float *L) {
    s_spr_n = 0; s_spr_nx = 0; s_spr_stood_n = 0;
    if (!s_spr_on || !sc || !s_cam_ok || sc->nanc <= 0 || sc->sn <= 0 || s_sm_world[1][3] < 0.5f) return;
    float AP[SC_ANC_MAX][3], AS[SC_ANC_MAX][2];
    int na = 0;
    for (int q = 0; q < sc->nanc; q++) {
        const float *V = sc->anc[q];   /* view space of the camera that drew its pieces */
        if (V[2] < 16.0f) continue;
        AP[na][0] = V[0]; AP[na][1] = -V[1]; AP[na][2] = V[2];
        AS[na][0] = s_cam_h * V[0] / V[2]; AS[na][1] = s_cam_h * V[1] / V[2];
        {   /* a high sun: onto the surface under it (s_spr_drop_h) */
            const float *u = s_sm_world[1], dz = spr_drop(sc, AP[na], u);
            if (dz > 0.5f) { for (int i = 0; i < 3; i++) AP[na][i] -= u[i] * dz; s_spr_dropped++; }
        }
        na++;
    }
    if (!na) return;
    s_spr_stood_n = na; s_spr_dbg_nrun = 0; s_sprc_build++;
    for (int q = 0; q < na; q++) { s_spr_stood[q] = 0; s_spr_dbg_anc[q][0] = AS[q][0]; s_spr_dbg_anc[q][1] = AS[q][1]; }
    const float *u = s_sm_world[1];   /* world up, view space (y up) */
    float nrm[3] = {-u[0] * u[2], -u[1] * u[2], 1.0f - u[2] * u[2]};   /* forward minus its up part */
    const float nl = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    if (nl < 1e-3f) { nrm[0] = nrm[1] = 0.0f; nrm[2] = 1.0f; } else for (int i = 0; i < 3; i++) nrm[i] /= nl;
    {   /* Facing the camera, a sprite casts a line when the light comes across
         * the picture (in its plane, as in BoF4's battles): the plane turns
         * about the up axis, just enough that the light's level direction
         * meets it at SPR_LIGHT_MIN (cosine), so its shadow keeps that much of
         * its width. The sprite's own depth turns with it (never shading itself). */
        const float lu = L[0] * u[0] + L[1] * u[1] + L[2] * u[2];
        float lh[3] = {L[0] - u[0] * lu, L[1] - u[1] * lu, L[2] - u[2] * lu};
        const float hl = sqrtf(lh[0] * lh[0] + lh[1] * lh[1] + lh[2] * lh[2]);
        if (hl > 0.1f) {
            for (int i = 0; i < 3; i++) lh[i] /= hl;
            const float c = nrm[0] * lh[0] + nrm[1] * lh[1] + nrm[2] * lh[2];
            if (fabsf(c) < SPR_LIGHT_MIN) {
                float t[3] = {lh[0] - nrm[0] * c, lh[1] - nrm[1] * c, lh[2] - nrm[2] * c};
                const float tl = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                for (int i = 0; i < 3; i++) t[i] /= tl;
                const float phi = atan2f(tl, c), a = acosf(SPR_LIGHT_MIN);   /* lh = cos(phi) nrm + sin(phi) t */
                const float th = c >= 0.0f ? phi - a : phi - 3.14159265f + a;
                for (int i = 0; i < 3; i++) nrm[i] = nrm[i] * cosf(th) + t[i] * sinf(th);
            }
        }
    }
    if (sc->sn * 3 > s_spr_cap) {
        float *c = (float *)realloc(s_spr_cast, (size_t)sc->sn * 3 * SC_V * sizeof(float));
        if (c) s_spr_cast = c;
        float *s = (float *)realloc(s_spr_scr, (size_t)sc->sn * 3 * SC_V * sizeof(float));
        if (s) s_spr_scr = s;
        float *f = (float *)realloc(s_spr_cau, (size_t)sc->sn * 3 * sizeof(float));
        if (f) s_spr_cau = f;
        if (!c || !s || !f) return;
        s_spr_cap = sc->sn * 3;
    }
    const float h = s_cam_h, kx = 1.0f / (120.0f * aspect), ky = 1.0f / 120.0f;
    s_spr_kx = kx; s_spr_ky = ky;
    /* One sprite = its opaque pieces from one texture page and CLUT that touch
     * one another, in whatever order they were drawn (the game sorts by depth,
     * so two sprites' pieces interleave). Translucent pieces (dust, glows, the
     * game's own blob) neither cast nor take shadows. */
    enum { SPR_MAXP = 1024 };
    static int   par[SPR_MAXP], ids[SPR_MAXP];
    static float box[SPR_MAXP][4];
    int m = 0;
    for (int t = 0; t < sc->sn && m < SPR_MAXP; t++) {
        const float *v = sc->sv + (size_t)t * 3 * SC_V;
        if (v[2] != 0.0f) continue;   /* translucent */
        float *b = box[m];
        b[0] = b[2] = 1e30f; b[1] = b[3] = -1e30f;
        for (int j = 0; j < 3; j++) {
            const float x = v[j * SC_V] - s_sc_cx, y = v[j * SC_V + 1] - s_sc_cy;
            b[0] = fminf(b[0], x); b[1] = fmaxf(b[1], x); b[2] = fminf(b[2], y); b[3] = fmaxf(b[3], y);
        }
        ids[m] = t; par[m] = m; m++;
    }
    for (int i = 0; i < m; i++) {
        const float *vi = sc->sv + (size_t)ids[i] * 3 * SC_V;
        for (int j = i + 1; j < m; j++) {
            const float *vj = sc->sv + (size_t)ids[j] * 3 * SC_V;
            if (vi[5] != vj[5] || vi[6] != vj[6] || vi[7] != vj[7] || vi[8] != vj[8]) continue;
            if (box[j][0] > box[i][1] + 2.0f || box[j][1] < box[i][0] - 2.0f ||
                box[j][2] > box[i][3] + 2.0f || box[j][3] < box[i][2] - 2.0f) continue;
            /* the root stays the lowest index of its run: the runs are walked from it */
            const int a = spr_root(par, i), c = spr_root(par, j);
            if (a < c) par[c] = a; else if (c < a) par[a] = c;
        }
    }
    /* Each run: its box, lowest drawn row and foot point. A run whose bottom
     * is at a foot point within its columns is that sprite's (direct; a sword
     * held out puts the point near one end). A run that is not (a part cut
     * off from its sprite: another texture page) joins a sprite only when it
     * has that sprite's CLUT, touches one of its runs (or is up to SPR_AIR px
     * over it), its middle is within that sprite's columns and it is at most
     * twice as wide, so
     * the HUD's panels, bars, texts and labels on or beside a character (UI
     * palettes) never stand there. A foot point no run is at takes the run
     * over it, up to SPR_AIR px up, in a palette seen standing on the floor
     * (a jump: off the floor, its shadow on the floor below). */
    static float rb[SPR_MAXP][5];   /* bx0, bx1, by0, by1, ob */
    static int   rq[SPR_MAXP];      /* foot point, -1 none */
    static uint8_t rdir[SPR_MAXP];  /* 1 = at its foot point */
    static uint8_t rair[SPR_MAXP];  /* 1 = over it, off the floor */
    float ax[SC_ANC_MAX][2];        /* each foot point's direct runs: columns */
    for (int q = 0; q < na; q++) { ax[q][0] = 1e30f; ax[q][1] = -1e30f; }
    for (int r = 0; r < m; r++) {
        rq[r] = -1; rdir[r] = 0; rair[r] = 0;
        if (spr_root(par, r) != r) continue;
        float *b = rb[r];
        b[0] = b[2] = 1e30f; b[1] = b[3] = b[4] = -1e30f;
        for (int i = r; i < m; i++)
            if (spr_root(par, i) == r) {
                b[0] = fminf(b[0], box[i][0]); b[1] = fmaxf(b[1], box[i][1]);
                b[2] = fminf(b[2], box[i][2]); b[3] = fmaxf(b[3], box[i][3]);
                if (sc->sb && sc->sb[ids[i]] > -1e29f) b[4] = fmaxf(b[4], sc->sb[ids[i]] - s_sc_cy);
            }
        if (b[4] < -1e29f) b[4] = b[3];   /* its lowest drawn pixel row (else its box) */
        if (b[1] - b[0] > 128.0f || b[3] - b[2] > 192.0f) continue;   /* panels, not a sprite */
        const float cx = 0.5f * (b[0] + b[1]);
        float bs = 1e30f;
        for (int q = 0; q < na; q++) {
            if (AS[q][0] < b[0] - 8.0f || AS[q][0] > b[1] + 8.0f || b[3] < AS[q][1] - 8.0f || b[3] > AS[q][1] + 20.0f) continue;
            const float s = fabsf(b[3] - AS[q][1]) + 0.25f * fabsf(cx - AS[q][0]);
            if (s < bs) { bs = s; rq[r] = q; }
        }
        if (rq[r] >= 0) {
            rdir[r] = 1;
            /* its columns, when it stands (a blob piece too small to does not
             * keep a jumping sprite from its foot point) */
            if (b[3] - b[2] >= SPR_MIN_H) { ax[rq[r]][0] = fminf(ax[rq[r]][0], b[0]); ax[rq[r]][1] = fmaxf(ax[rq[r]][1], b[1]); }
        }
    }
    for (int q = 0; q < na; q++) {   /* off the floor: the nearest run over a foot point left alone */
        if (ax[q][1] >= ax[q][0]) continue;
        int bestr = -1;
        float bs = 1e30f;
        for (int r = 0; r < m; r++) {
            if (spr_root(par, r) != r || rq[r] >= 0) continue;
            const float *b = rb[r];
            if (b[1] - b[0] > 128.0f || b[3] - b[2] > 192.0f || b[3] - b[2] < SPR_MIN_H) continue;
            if (AS[q][0] < b[0] - 8.0f || AS[q][0] > b[1] + 8.0f || b[3] > AS[q][1] || b[3] < AS[q][1] - SPR_AIR) continue;
            const float cx = 0.5f * (b[0] + b[1]);
            if (fabsf(cx - AS[q][0]) > 0.5f * (b[1] - b[0]) + 16.0f) continue;
            if (!sprc_known(spr_clut_code(sc, ids[r]))) continue;
            const float sc2 = AS[q][1] - b[3] + 0.25f * fabsf(cx - AS[q][0]);
            if (sc2 < bs) { bs = sc2; bestr = r; }
        }
        if (bestr < 0) continue;
        rq[bestr] = q; rdir[bestr] = 1; rair[bestr] = 1;
        ax[q][0] = rb[bestr][0]; ax[q][1] = rb[bestr][1];
    }
    for (int pass = 0; pass < 3; pass++) {   /* parts touching parts that joined */
        int added = 0;
        for (int r = 0; r < m; r++) {
            if (spr_root(par, r) != r || rq[r] >= 0) continue;
            const float *b = rb[r];
            if (b[1] - b[0] > 128.0f || b[3] - b[2] > 192.0f) continue;
            const float cx = 0.5f * (b[0] + b[1]);
            for (int o = 0; o < m && rq[r] < 0; o++) {
                const int q = rq[o];
                if (o == r || q < 0 || spr_root(par, o) != o) continue;
                const float *c = rb[o];
                const float *vr = sc->sv + (size_t)ids[r] * 3 * SC_V, *vo = sc->sv + (size_t)ids[o] * 3 * SC_V;
                if (vr[7] != vo[7] || vr[8] != vo[8]) continue;   /* another palette: the UI's, not this sprite's */
                /* touching it, or over it (eyes floating over a body; a body in
                 * the air over its feet, drawn apart in a leap): its CLUT says
                 * it is the same sprite */
                if (b[0] > c[1] + 2.0f || b[1] < c[0] - 2.0f || b[2] > c[3] + 2.0f || b[3] < c[2] - SPR_AIR) continue;
                if (cx < ax[q][0] - 4.0f || cx > ax[q][1] + 4.0f || b[3] > AS[q][1] + 10.0f) continue;
                /* not much wider than that sprite (a body over its feet is wider) */
                if (b[1] - b[0] > 2.0f * (ax[q][1] - ax[q][0]) + 8.0f) continue;
                rq[r] = q; added = 1;
            }
        }
        if (!added) break;
    }
    /* Its own runs first (each sprite's plane), then the parts joined to it,
     * on that same plane (a part's own bottom is no floor: eyes over a body). */
    float anc_d[SC_ANC_MAX];
    uint8_t anc_ok[SC_ANC_MAX];
    memset(anc_ok, 0, sizeof anc_ok);
    for (int pass = 0; pass < 2; pass++)
    for (int r = 0; r < m; r++) {
        if (spr_root(par, r) != r || (pass == 0) != (rdir[r] != 0)) continue;
        const float bx0 = rb[r][0], bx1 = rb[r][1], by0 = rb[r][2], by1 = rb[r][3], ob = rb[r][4];
        const int best = rq[r];
        if (s_spr_dbg_nrun < SPR_DBG_RUNS) {   /* sprite_dump: every run (9 = too big, then the rest) */
            float *g = s_spr_dbg_run[s_spr_dbg_nrun++];
            g[0] = bx0; g[1] = by0; g[2] = bx1; g[3] = by1; g[4] = (float)best; g[5] = rdir[r]; g[6] = 9.0f;
        }
        if (bx1 - bx0 > 128.0f || by1 - by0 > 192.0f) continue;
        if (s_spr_dbg_nrun > 0 && s_spr_dbg_run[s_spr_dbg_nrun - 1][6] == 9.0f) s_spr_dbg_nrun--;
        s_spr_runs[best < 0 ? 2 : rdir[r] ? 0 : 1]++;
        if (s_spr_dbg_nrun < SPR_DBG_RUNS) {
            float *g = s_spr_dbg_run[s_spr_dbg_nrun++];
            g[0] = bx0; g[1] = by0; g[2] = bx1; g[3] = by1; g[4] = (float)best; g[5] = rdir[r] + 2 * rair[r];
            g[6] = sc->sv[(size_t)ids[r] * 3 * SC_V + 8] * 64.0f + sc->sv[(size_t)ids[r] * 3 * SC_V + 7] / 16.0f;   /* its CLUT */
        }
        if (best < 0) continue;
        if (rdir[r] && by1 - by0 < SPR_MIN_H) continue;   /* a marker or icon (a map's), not a figure; a part joined to one may be small (eyes) */
        if (!rdir[r] && !anc_ok[best]) continue;           /* a part of a sprite that does not stand */
        /* It stands where its lowest drawn row meets the floor at its foot
         * point's height (a front foot drawn below the foot point would be
         * under the floor on a plane through the point, its shadow lost). */
        float na_d = nrm[0] * AP[best][0] + nrm[1] * AP[best][1] + nrm[2] * AP[best][2];
        const float au = AP[best][0] * u[0] + AP[best][1] * u[1] + AP[best][2] * u[2];
        const int joined = !rdir[r];   /* a joined part: the sprite's plane, as drawn */
        /* Over its foot point: the game moves a sprite in an animation (an
         * attack's step forward) away from the point it projects, so it is
         * taken to stand where its lowest row meets the floor, however far
         * (a jump then keeps its shadow at its feet, never out of place). */
        const int air = rair[r];
        float G[3];
        if (!rdir[r]) na_d = anc_d[best];
        else if (spr_ground(AS[best][0], ob, au, u, G)) {
            const float g = nrm[0] * G[0] + nrm[1] * G[1] + nrm[2] * G[2];
            if (air || fabsf(g - na_d) < 256.0f) na_d = g;
        }
        if (rdir[r] && !anc_ok[best]) { anc_d[best] = na_d; anc_ok[best] = 1; }
        if (rdir[r] && !air) sprc_note(spr_clut_code(sc, ids[r]));
        /* its feet bent down to the floor */
        static SprFoot sf;
        if (!joined && !air && s_spr_bend_dbg != 1 && spr_feet(sc, ids, par, r, m, bx0, bx1, by0, ob, &sf))
            spr_bend(&sf, nrm, na_d, au, u, by1 - by0);
        for (int i = r; i < m; i++) {
            if (spr_root(par, i) != r) continue;
            const float *v = sc->sv + (size_t)ids[i] * 3 * SC_V;
            float *oc = s_spr_cast + (size_t)s_spr_n * SC_V, *os = s_spr_scr + (size_t)s_spr_n * SC_V;
            int ok = 1;
            for (int j = 0; j < 3 && ok; j++) {
                const float x = v[j * SC_V] - s_sc_cx, y = v[j * SC_V + 1] - s_sc_cy;
                const float d[3] = {x / h, -y / h, 1.0f};
                const float den = nrm[0] * d[0] + nrm[1] * d[1] + nrm[2] * d[2];
                if (fabsf(den) < 1e-4f) { ok = 0; break; }
                const float z = na_d / den;
                float *c = oc + j * SC_V, *s = os + j * SC_V;
                c[0] = d[0] * z; c[1] = d[1] * z; c[2] = z;
                s[0] = x * kx; s[1] = -y * ky; s[2] = z;
                memcpy(c + 3, v + j * SC_V + 3, (SC_V - 3) * sizeof(float));
                memcpy(s + 3, v + j * SC_V + 3, (SC_V - 3) * sizeof(float));
            }
            if (ok) { for (int j = 0; j < 3; j++) s_spr_cau[s_spr_n + j] = au;
                      s_spr_n += 3; s_spr_stood[best]++; }
        }
    }
    if (s_spr_n) s_spr_frames++;
    s_spr_last = (unsigned)s_spr_n / 3u;
    /* the pieces' cut-outs as they were drawn (sc_add_sprite_tri) */
    const int rows = sc_atlas_rows(sc);
    if (s_spr_n && rows > 0) {
        if (!s_spr_atl_tex) {
            glGenTextures(1, &s_spr_atl_tex);
            glBindTexture(GL_TEXTURE_2D, s_spr_atl_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, 0x8229 /*R8*/, SC_ATL, SC_ATL, 0, 0x1903 /*RED*/, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        GLint pbo = 0, rl = 0;
        glGetIntegerv(0x88EF /*PIXEL_UNPACK_BUFFER_BINDING*/, &pbo);
        glGetIntegerv(PSXGL_UNPACK_ROW_LENGTH, &rl);
        if (pbo) p_glBindBuffer(PSXGL_PIXEL_UNPACK_BUFFER, 0);
        if (rl) glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, 0);
        glBindTexture(GL_TEXTURE_2D, s_spr_atl_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SC_ATL, rows, 0x1903 /*RED*/, GL_UNSIGNED_BYTE, sc->atl);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (rl) glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, rl);
        if (pbo) p_glBindBuffer(PSXGL_PIXEL_UNPACK_BUFFER, (GLuint)pbo);
    }
}
/* Sprite caster vertex k: the standing pieces, then the bent feet. */
static const float *spr_caster(int k) {
    return k < s_spr_n ? s_spr_cast + (size_t)k * SC_V : s_spr_xc + (size_t)(k - s_spr_n) * SC_V;
}
/* The `sprite` input: the sprites' view depth on the picture, SPR_NONE elsewhere. */
static void pfx_sprites_depth(int lw, int lh) {
    if (!s_spr_on || !s_spr_prog || lw <= 0 || lh <= 0) return;
    if (!s_spr_tex || s_spr_w != lw || s_spr_h != lh) {
        if (!s_spr_tex) { glGenTextures(1, &s_spr_tex); p_glGenFramebuffers(1, &s_spr_fbo); }
        glBindTexture(GL_TEXTURE_2D, s_spr_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x822E /*R32F*/, lw, lh, 0, 0x1903 /*RED*/, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_spr_fbo);
        p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, 0x8CE0 /*COLOR_ATTACHMENT0*/, GL_TEXTURE_2D, s_spr_tex, 0);
        s_spr_w = lw; s_spr_h = lh;
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_spr_fbo);
    glViewport(0, 0, lw, lh);
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glClearColor((float)SPR_NONE, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    if (s_spr_n > 0) {
        p_glUseProgram(s_spr_prog);
        p_glBindVertexArray(s_sm_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_sm_vbo);
        p_glBufferData(PSXGL_ARRAY_BUFFER, (GLsizeiptr)((size_t)s_spr_n * SC_V * sizeof(float)), s_spr_scr, PSXGL_STREAM_DRAW);
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_raw_tex);
        p_glUniform1i(s_spr_uvram, 0);
        p_glActiveTexture(PSXGL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, s_spr_atl_tex);
        p_glUniform1i(s_spr_uatl, 1);
        p_glActiveTexture(PSXGL_TEXTURE0);
        glEnable(GL_BLEND);
        p_glBlendEquationSeparate(0x8007 /*MIN*/, 0x8007);
        glBlendFunc(GL_ONE, GL_ONE);
        glDrawArrays(GL_TRIANGLES, 0, s_spr_n);
        /* the bent feet too, the nearest still winning: written over, a
         * sprite's bent feet behind another (Ryu's behind Nina) gave her
         * pixels their place, in a shadow she is not in */
        if (s_spr_nx > 0) {
            p_glBufferData(PSXGL_ARRAY_BUFFER, (GLsizeiptr)((size_t)s_spr_nx * SC_V * sizeof(float)), s_spr_xs, PSXGL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, s_spr_nx);
        }
        p_glBlendEquationSeparate(PSXGL_FUNC_ADD, PSXGL_FUNC_ADD);
        glDisable(GL_BLEND);
        p_glActiveTexture(PSXGL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, 0);
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        p_glBindVertexArray(0);
    }
}
/* The shadow maps' textures, programs and vertex layout (the sun's, the
 * sprites' depth, the fire lights'), made once. */
static void pfx_sm_init(void) {
    glGenTextures(1, &s_sm_tex);
    glBindTexture(GL_TEXTURE_2D, s_sm_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6 /*DEPTH_COMPONENT24*/, SM_SIZE, SM_SIZE, 0,
                 0x1902 /*DEPTH_COMPONENT*/, 0x1405 /*UNSIGNED_INT*/, NULL);
    s_sm_size = SM_SIZE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glGenFramebuffers(1, &s_sm_fbo);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_sm_fbo);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, 0x8D00 /*DEPTH_ATTACHMENT*/, GL_TEXTURE_2D, s_sm_tex, 0);
    glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
    glGenTextures(1, &s_sm2_tex);
    glBindTexture(GL_TEXTURE_2D, s_sm2_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6 /*DEPTH_COMPONENT24*/, SM_SIZE, SM_SIZE, 0,
                 0x1902 /*DEPTH_COMPONENT*/, 0x1405 /*UNSIGNED_INT*/, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glGenFramebuffers(1, &s_sm2_fbo);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_sm2_fbo);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, 0x8D00 /*DEPTH_ATTACHMENT*/, GL_TEXTURE_2D, s_sm2_tex, 0);
    glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
    /* Transparent texels (raw 0) cast no shadow, as they draw nothing:
     * the fetch is the textured shader's (page, CLUT, window). */
#define SM_VS_HEAD \
        "#version 330\n" \
        "layout(location = 0) in vec3 a_p; layout(location = 1) in vec2 a_uv;\n" \
        "layout(location = 2) in vec4 a_tc; layout(location = 3) in vec2 a_dw;\n" \
        "out vec2 v_uv; flat out ivec4 v_tc; flat out ivec2 v_dw; out float v_z;\n" \
        "void main(){ v_uv = a_uv; v_tc = ivec4(a_tc); v_dw = ivec2(a_dw); v_z = a_p.z;\n"
#define SM_FS_HEAD \
        "#version 330\n" \
        "in vec2 v_uv; flat in ivec4 v_tc; flat in ivec2 v_dw; in float v_z; out vec4 o;\n" \
        "uniform usampler2D u_vram; uniform sampler2D u_atl;\n" \
        "int vram_at(int x, int y){ return int(texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r); }\n" \
        "void main(){\n"
#define SM_FS_TEXEL \
        "  if (v_dw.x < 0) return;\n" \
        "  if (v_dw.x == 3) { if (texelFetch(u_atl, ivec2(floor(v_uv)), 0).r < 0.5) discard; return; }\n" \
        "  int u = int(floor(v_uv.x)) & 255, v = int(floor(v_uv.y)) & 255;\n" \
        "  ivec4 tw = ivec4(v_dw.y & 31, (v_dw.y >> 5) & 31, (v_dw.y >> 10) & 31, (v_dw.y >> 15) & 31);\n" \
        "  if ((tw.x | tw.y) != 0) { u = (u & ~(tw.x * 8)) | ((tw.z & tw.x) * 8);\n" \
        "                            v = (v & ~(tw.y * 8)) | ((tw.w & tw.y) * 8); }\n" \
        "  int raw;\n" \
        "  if (v_dw.x == 0) raw = vram_at(v_tc.z + ((vram_at(v_tc.x + (u >> 2), v_tc.y + v) >> ((u & 3) * 4)) & 0xF), v_tc.w);\n" \
        "  else if (v_dw.x == 1) raw = vram_at(v_tc.z + ((vram_at(v_tc.x + (u >> 1), v_tc.y + v) >> ((u & 1) * 8)) & 0xFF), v_tc.w);\n" \
        "  else raw = vram_at(v_tc.x + u, v_tc.y + v);\n" \
        "  if (raw == 0) discard;\n" \
        "}\n"
    s_sm_prog = build_program(SM_VS_HEAD "  gl_Position = vec4(a_p, 1.0); }\n",
                              SM_FS_HEAD "  o = vec4(1.0);\n" SM_FS_TEXEL);
    /* The sprites' view depth on the picture (the `sprite` input): the
     * nearest one wins (min blending), transparent texels leave none. */
    s_spr_prog = build_program(SM_VS_HEAD "  gl_Position = vec4(a_p.xy, 0.0, 1.0); }\n",
                               SM_FS_HEAD "  o = vec4(v_z, 0.0, 0.0, 1.0);\n" SM_FS_TEXEL);
    /* The fire lights' shadow map (pfx_fire_build): a cube round each light,
     * its six faces side by side; a_p is a view-space point, drawn from the
     * light (u_lp) along the face's axes (u_ff forward, u_fr right, u_fu up,
     * 90 degrees each way), its depth the distance to the light over u_far. */
    s_fm_prog = build_program(
        "#version 330\n"
        "layout(location = 0) in vec3 a_p; layout(location = 1) in vec2 a_uv;\n"
        "layout(location = 2) in vec4 a_tc; layout(location = 3) in vec2 a_dw;\n"
        "out vec2 v_uv; flat out ivec4 v_tc; flat out ivec2 v_dw; out float v_z;\n"
        "uniform vec3 u_lp, u_ff, u_fr, u_fu; uniform float u_far;\n"
        "void main(){ v_uv = a_uv; v_tc = ivec4(a_tc); v_dw = ivec2(a_dw);\n"
        "  vec3 d = a_p - u_lp; v_z = length(d);\n"
        "  float z = dot(d, u_ff), n = 2.0;\n"
        "  gl_Position = vec4(dot(d, u_fr), dot(d, u_fu), (z * (u_far + n) - 2.0 * u_far * n) / (u_far - n), z); }\n",
        "#version 330\n"
        "in vec2 v_uv; flat in ivec4 v_tc; flat in ivec2 v_dw; in float v_z; out vec4 o;\n"
        "uniform usampler2D u_vram; uniform sampler2D u_atl; uniform float u_far;\n"
        "int vram_at(int x, int y){ return int(texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r); }\n"
        "void main(){\n"
        "  o = vec4(1.0); gl_FragDepth = clamp(v_z / u_far, 0.0, 1.0);\n" SM_FS_TEXEL);
    s_fm_uvram = s_fm_prog ? p_glGetUniformLocation(s_fm_prog, "u_vram") : -1;
    s_fm_uatl = s_fm_prog ? p_glGetUniformLocation(s_fm_prog, "u_atl") : -1;
#undef SM_VS_HEAD
#undef SM_FS_HEAD
#undef SM_FS_TEXEL
    s_sm_uvram = s_sm_prog ? p_glGetUniformLocation(s_sm_prog, "u_vram") : -1;
    s_spr_uvram = s_spr_prog ? p_glGetUniformLocation(s_spr_prog, "u_vram") : -1;
    s_sm_uatl = s_sm_prog ? p_glGetUniformLocation(s_sm_prog, "u_atl") : -1;
    s_spr_uatl = s_spr_prog ? p_glGetUniformLocation(s_spr_prog, "u_atl") : -1;
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
static void pfx_shadow_build(const PfxPass *ps, float aspect, int lw, int lh) {
    if (!s_sm_tex) pfx_sm_init();
    memset(s_sm_u, 0, sizeof s_sm_u);
    {   /* the size the pass asks for */
        const float *rq = pfx_pass_uniform(ps, "u_smsize");
        int want = SM_SIZE;
        if (rq && rq[0] > 0.0f) { want = 1024; while (want < 8192 && (float)want < rq[0] * 0.75f) want *= 2; }
        if (want != s_sm_size) {
            for (int t = 0; t < 2; t++) {
                glBindTexture(GL_TEXTURE_2D, t ? s_sm2_tex : s_sm_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6 /*DEPTH_COMPONENT24*/, want, want, 0,
                             0x1902 /*DEPTH_COMPONENT*/, 0x1405 /*UNSIGNED_INT*/, NULL);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            s_sm_size = want;
        }
    }
    s_sm_u[3][3] = (float)s_sm_size;
    const float *dir = pfx_pass_uniform(ps, "u_shadowdir"), *proj = pfx_pass_uniform(ps, "u_proj");
    const float *sun = pfx_pass_uniform(ps, "u_sun");
    float area_sun[2];   /* [video.shadow] sun_areas: this area's own sun */
    if (sun && gpu_shadow_sun_area(&area_sun[0], &area_sun[1])) sun = area_sun;
    float night_sun[2];  /* gl_renderer_set_night: that far toward the pass's moon (u_moon) */
    {
        const float *moon = pfx_pass_uniform(ps, "u_moon");
        if (sun && moon && moon[1] > 0.0f && s_night_f > 0.0f) {
            night_sun[0] = sun[0] + moon[0] * s_night_f;
            night_sun[1] = sun[1] + (moon[1] - sun[1]) * s_night_f;
            sun = night_sun;
        }
    }
    const float H = proj ? proj[0] : 0.0f;
    float L[3] = {dir ? dir[0] : 0.0f, dir ? dir[1] : 0.0f, dir ? dir[2] : 0.0f};
    float Ls[3] = {L[0], L[1], L[2]};   /* the sprites' light (u_sprsun) */
    int spr_own = 0;                    /* the sprites' map has its own mapping */
    memset(s_ss_u, 0, sizeof s_ss_u);
    const ScSlot *sc = sc_presented();
    /* The scene's own geometry (gl_renderer_shadow_world) and the camera the
     * picture on screen was drawn with: the whole scenery, placed exactly. */
    int use_world = s_wg_n > 0 && sc && sc->cam_ok;
    if (use_world) {
        memcpy(s_cam_r, sc->cam.r, sizeof s_cam_r); memcpy(s_cam_t, sc->cam.t, sizeof s_cam_t);
        s_cam_of[0] = sc->cam.of[0]; s_cam_of[1] = sc->cam.of[1]; s_cam_h = sc->cam.h;
        s_cam_ok = 1;
        wg_agree(sc, H);   /* ... only while the picture's floor lies on it */
        wg_trust_update();
        use_world = s_wg_trust;
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
            const float *ssun = pfx_pass_uniform(ps, "u_sprsun");
            if (ssun && ssun[0] > 0.0f && sun[1] > ssun[0]) {
                const float es = ssun[0] * 0.01745329f;
                for (int i = 0; i < 3; i++)
                    Ls[i] = u[i] * sinf(es) + (rt[i] * cosf(az) + fw[i] * sinf(az)) * cosf(es);
                spr_own = 1;
            } else memcpy(Ls, L, sizeof Ls);
        }
    }
    {   /* u_ss.z, u_ss.w (TCP, debugging the feet): 1 = no bend; the bent zone's height */
        const float *ss = pfx_pass_uniform(ps, "u_ss");
        s_spr_bend_dbg = ss ? (int)(ss[2] + 0.5f) : 0;
        s_spr_bend_h = ss ? ss[3] : 0.0f;
    }
    s_spr_drop_h = spr_own ? H : 0.0f;
    pfx_sprites_stand(sc, aspect, Ls);
    {   /* u_ss.x 0: sprite shadows off (no casters, no shading; the game's blobs stay) */
        const float *ss = pfx_pass_uniform(ps, "u_ss");
        s_spr_ss = !(ss && ss[0] < 0.5f);
        if (!s_spr_ss) { s_spr_n = s_spr_nx = 0; }
    }
    {   /* TCP shadow_cache "presents" */
        int *D = s_pfx_dbg[s_pfx_dbg_n++ & 31u];
        D[0] = s_sc_gen_read; D[1] = !sc ? -1 : sc == &s_sc_held ? 9 : (int)(sc - s_sc); D[2] = sc ? sc->area[1] : -1;
        D[3] = sc ? sc->n : -1; D[4] = sc ? sc->sn : -1; D[5] = sc ? sc->nanc : -1;
        D[6] = s_spr_n / 3; D[7] = use_world; D[8] = (int)s_sc_cy;
    }
    const float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    int n = 0, nbase_all = 0;
    /* the sprites' own mapping (spr_own): across, up and along their light,
     * their box on the map, the depth range of everything along it, and each
     * sprite vertex's bias (as the scenery's sprites get theirs below) */
    float A2[3] = {0}, B2[3] = {0}, X0 = 1e30f, X1 = -1e30f, Y0 = 1e30f, Y1 = -1e30f, D0 = 1e30f, D1 = -1e30f, sbias = 0.0f, slam = 0.0f;
    float a[3], b[3], x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, d0 = 1e30f, d1 = -1e30f;
    /* the scenery memory where no map is given, and with the map for what
     * stands on it off the picture (a bush, a rock: the map has the ground
     * and its cliffs only, and the game draws what is on screen, so their
     * shadows went with them at the edges); not with a map set aside (a
     * cutscene drawing its own floor, wg_agree): the memory would keep a
     * moving object (the sandflier in a chase) at its past places, a trail
     * of shadow behind it */
    const int use_store = (use_world || s_wg_n <= 0) && s_cam_ok && s_scc_alive > 0;
    const int store_off_only = use_world;   /* with the map: off the picture only (the frame has the rest) */
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
                if (c->seen == s_scc_stamp && store_off_only) continue;
                if (c->seen != s_scc_stamp) {
                    if (c->hits < SCC_STATIC) continue;
                    /* with the map: whatever static the frame did not draw (the
                     * game culls its objects at the picture's edge, inside the
                     * overdraw band) */
                    if (store_off_only) { pick[npick++] = e; continue; }
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
        /* world geometry first (nw vertices), then the frame's own triangles
         * (nf; in the memory itself without a map), then the memory's (ns) */
        const int nw = use_world ? s_wg_n * 3 : 0;
        const int nf = use_store && !store_off_only ? 0 : sc->n * 3;
        const int ns = use_store ? npick * 3 : 0;
        const int nbase = nw + nf + ns;   /* then the sprites standing */
        n = nbase + s_spr_n + s_spr_nx;
        nbase_all = nbase;
        if (n > s_sm_cap) {
            float *p = (float *)realloc(s_sm_buf, (size_t)n * SC_V * sizeof(float));
            if (!p) n = 0; else { s_sm_buf = p; s_sm_cap = n; }
        }
        if (spr_own && n > s_sm_pcap) {
            float *p = (float *)realloc(s_sm_pos, (size_t)n * 3 * sizeof(float));
            if (!p) spr_own = 0; else { s_sm_pos = p; s_sm_pcap = n; }
        }
        const float k = 1.0f / H;
        const float hw = 120.0f * aspect * 1.05f, hh = 120.0f * 1.05f;   /* the picture, native px */
        float sx0 = 1e30f, sx1 = -1e30f, sy0 = 1e30f, sy1 = -1e30f;
        int ci = 0;   /* store entry; each gives 3 vertices */
        for (int i = 0; i < n; i++) {
            float P[3], scr[2];
            float *o = s_sm_buf + (size_t)i * SC_V;
            if (i >= nbase) {
                const float *c = spr_caster(i - nbase);
                P[0] = c[0]; P[1] = c[1]; P[2] = c[2];
                scr[0] = s_cam_h * P[0] / P[2]; scr[1] = -s_cam_h * P[1] / P[2];
                memcpy(o + 3, c + 3, (SC_V - 3) * sizeof(float));
            } else if (i < nw) {
                float V[3]; scc_view(s_wg + (size_t)i * 3, V);
                P[0] = V[0]; P[1] = -V[1]; P[2] = V[2];
                if (V[2] > 1.0f) { scr[0] = s_cam_h * V[0] / V[2]; scr[1] = s_cam_h * V[1] / V[2]; }
                else scr[0] = scr[1] = 1e30f;   /* behind the camera: casts, never on screen */
                o[3] = o[4] = o[5] = o[6] = o[7] = o[8] = o[10] = 0.0f; o[9] = -1.0f;   /* solid */
            } else if (i >= nw + nf) {
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
            if (spr_own) memcpy(s_sm_pos + (size_t)i * 3, P, sizeof P);
            o[0] = P[0] * a[0] + P[1] * a[1] + P[2] * a[2];
            o[1] = P[0] * b[0] + P[1] * b[1] + P[2] * b[2];
            o[2] = P[0] * L[0] + P[1] * L[1] + P[2] * L[2];
            if (o[0] < x0) x0 = o[0]; if (o[0] > x1) x1 = o[0];
            if (o[1] < y0) y0 = o[1]; if (o[1] > y1) y1 = o[1];
            if (o[2] < d0) d0 = o[2]; if (o[2] > d1) d1 = o[2];
            int on = fabsf(scr[0]) <= hw && fabsf(scr[1]) <= hh;
            if (!on && s_pfx_vx_on) {   /* a generated frame: on its own picture too (a turning camera sees past the real one's) */
                float g[3];
                pfx_vx_point(P, g);
                on = g[2] > 1.0f && fabsf(s_cam_h * g[0] / g[2]) <= hw && fabsf(s_cam_h * g[1] / g[2]) <= hh;
            }
            if (on) {
                if (o[0] < sx0) sx0 = o[0]; if (o[0] > sx1) sx1 = o[0];
                if (o[1] < sy0) sy0 = o[1]; if (o[1] > sy1) sy1 = o[1];
            }
        }
        if (sx1 > sx0 && sy1 > sy0) {   /* the on-screen part, a small margin round it */
            const float mx = (sx1 - sx0) * 0.03f, my = (sy1 - sy0) * 0.03f;
            x0 = fmaxf(x0, sx0 - mx); x1 = fminf(x1, sx1 + mx);
            y0 = fmaxf(y0, sy0 - my); y1 = fminf(y1, sy1 + my);
        }
        /* Sprites stand on the floor: moved toward the light by the bias a
         * floor receiver uses (sun.glsl: (u_shadow.y Z + 2 texel)(1 + 2(1 -
         * lambert))), so their shadows start at their feet, not that far off.
         * A sprite pixel's own bias is larger, so a sprite never shades itself. */
        const float *shb = pfx_pass_uniform(ps, "u_shadow");
        if (n > nbase && x1 > x0 && s_sm_world[1][3] > 0.5f) {
            const float texel = (x1 - x0) / (float)s_sm_size;
            const float *up = s_sm_world[1];
            const float lam = fmaxf(0.0f, up[0] * L[0] + up[1] * L[1] + up[2] * L[2]);
            for (int i = nbase; i < n; i++) {
                float *o = s_sm_buf + (size_t)i * SC_V;
                const float z = spr_caster(i - nbase)[2];
                o[2] += ((shb ? shb[1] : 0.002f) * z + 2.0f * texel) * (1.0f + 2.0f * (1.0f - lam));
                if (o[2] > d1) d1 = o[2];
            }
        }
        if (spr_own && n > nbase) {
            const float ls = sqrtf(Ls[0] * Ls[0] + Ls[1] * Ls[1] + Ls[2] * Ls[2]);
            for (int i = 0; i < 3; i++) Ls[i] /= ls;
            A2[0] = Ls[2]; A2[2] = -Ls[0];
            float la2 = sqrtf(A2[0] * A2[0] + A2[2] * A2[2]);
            if (la2 < 1e-4f) { A2[0] = 1.0f; A2[2] = 0.0f; la2 = 1.0f; }
            A2[0] /= la2; A2[2] /= la2;
            B2[0] = Ls[1] * A2[2] - Ls[2] * A2[1]; B2[1] = Ls[2] * A2[0] - Ls[0] * A2[2]; B2[2] = Ls[0] * A2[1] - Ls[1] * A2[0];
            {   /* each sprite laid along its light onto the floor at its feet:
                 * its whole silhouette at that height, so the reach (sun.glsl
                 * u_sprsun.y) keeps it off a floor far below (the sand under a
                 * deck) without cutting it */
                const float *up = s_sm_world[1];
                const float lu = Ls[0] * up[0] + Ls[1] * up[1] + Ls[2] * up[2];
                if (lu > 0.1f)
                    for (int i = nbase; i < n; i++) {
                        float *P = s_sm_pos + (size_t)i * 3;
                        const int k = i - nbase;
                        const float fh = k < s_spr_n ? s_spr_cau[k] : s_spr_xau[k - s_spr_n];
                        const float h = P[0] * up[0] + P[1] * up[1] + P[2] * up[2] - fh;
                        if (h <= 0.0f) continue;
                        for (int c = 0; c < 3; c++) P[c] -= Ls[c] * (h / lu);
                    }
            }
            for (int i = 0; i < n; i++) {
                const float *P = s_sm_pos + (size_t)i * 3;
                const float d = P[0] * Ls[0] + P[1] * Ls[1] + P[2] * Ls[2];
                if (d < D0) D0 = d; if (d > D1) D1 = d;
                if (i < nbase) continue;
                const float x = P[0] * A2[0] + P[1] * A2[1] + P[2] * A2[2], y = P[0] * B2[0] + P[1] * B2[1] + P[2] * B2[2];
                if (x < X0) X0 = x; if (x > X1) X1 = x; if (y < Y0) Y0 = y; if (y > Y1) Y1 = y;
            }
            const float mx = fmaxf((X1 - X0) * 0.05f, 8.0f), my = fmaxf((Y1 - Y0) * 0.05f, 8.0f);
            X0 -= mx; X1 += mx; Y0 -= my; Y1 += my;
            const float *up = s_sm_world[1];
            slam = fmaxf(0.0f, up[0] * Ls[0] + up[1] * Ls[1] + up[2] * Ls[2]);
            sbias = (X1 - X0) / (float)s_sm_size;   /* a texel, for the bias */
            const float *shb = pfx_pass_uniform(ps, "u_shadow");
            const float by = shb ? shb[1] : 0.002f;
            for (int i = nbase; i < n; i++) {   /* toward the light by a floor receiver's bias (feet first) */
                const float z = s_sm_pos[(size_t)i * 3 + 2];
                const float d = s_sm_pos[(size_t)i * 3] * Ls[0] + s_sm_pos[(size_t)i * 3 + 1] * Ls[1] + z * Ls[2]
                              + (by * z + 2.0f * sbias) * (1.0f + 2.0f * (1.0f - slam));
                if (d > D1) D1 = d;
            }
            if (!(X1 > X0 && Y1 > Y0 && D1 > D0)) spr_own = 0;
        } else spr_own = 0;
    }
    GLboolean dmask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    glViewport(0, 0, s_sm_size, s_sm_size);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_sm2_fbo);
    glClear(GL_DEPTH_BUFFER_BIT);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_sm_fbo);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (n > 0 && x1 > x0 && y1 > y0 && d1 > d0) {
        const float sx = 1.0f / (x1 - x0), sy = 1.0f / (y1 - y0), sd = 1.0f / (d1 - d0);
        for (int i = 0; i < n; i++) {
            float *o = s_sm_buf + (size_t)i * SC_V;
            o[0] = (o[0] - x0) * sx * 2.0f - 1.0f;
            o[1] = (o[1] - y0) * sy * 2.0f - 1.0f;
            o[2] = (d1 - o[2]) * sd * 2.0f - 1.0f;
        }
        if (spr_own) {   /* the sprites onto their own map, along their light */
            const float *shb = pfx_pass_uniform(ps, "u_shadow");
            const float by = shb ? shb[1] : 0.002f;
            const float qx = 1.0f / (X1 - X0), qy = 1.0f / (Y1 - Y0), qd = 1.0f / (D1 - D0);
            for (int i = nbase_all; i < n; i++) {
                const float *P = s_sm_pos + (size_t)i * 3;
                float *o = s_sm_buf + (size_t)i * SC_V;
                const float d = P[0] * Ls[0] + P[1] * Ls[1] + P[2] * Ls[2]
                              + (by * P[2] + 2.0f * sbias) * (1.0f + 2.0f * (1.0f - slam));
                o[0] = ((P[0] * A2[0] + P[1] * A2[1] + P[2] * A2[2]) - X0) * qx * 2.0f - 1.0f;
                o[1] = ((P[0] * B2[0] + P[1] * B2[1] + P[2] * B2[2]) - Y0) * qy * 2.0f - 1.0f;
                o[2] = (D1 - d) * qd * 2.0f - 1.0f;
            }
            const float su[4][4] = {{A2[0], A2[1], A2[2], X0}, {B2[0], B2[1], B2[2], Y0},
                                    {Ls[0], Ls[1], Ls[2], D1}, {qx, qy, qd, (float)s_sm_size}};
            memcpy(s_ss_u, su, sizeof s_ss_u);
        }
        p_glUseProgram(s_sm_prog);
        p_glBindVertexArray(s_sm_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_sm_vbo);
        p_glBufferData(PSXGL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * SC_V * sizeof(float)), s_sm_buf, PSXGL_STREAM_DRAW);
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_raw_tex);
        p_glUniform1i(s_sm_uvram, 0);
        p_glActiveTexture(PSXGL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, s_spr_atl_tex);
        p_glUniform1i(s_sm_uatl, 1);
        p_glActiveTexture(PSXGL_TEXTURE0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        const int nsc = n < nbase_all ? n : nbase_all;   /* the scenery; then the sprites */
        if (nsc > 0) glDrawArrays(GL_TRIANGLES, 0, nsc);
        if (n > nsc) {
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_sm2_fbo);
            glDrawArrays(GL_TRIANGLES, nsc, n - nsc);
        }
        glDisable(GL_DEPTH_TEST);
        p_glActiveTexture(PSXGL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, 0);
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        p_glBindVertexArray(0);
        const float u[4][4] = {{a[0], a[1], a[2], x0}, {b[0], b[1], b[2], y0},
                               {L[0], L[1], L[2], d1}, {sx, sy, sd, (float)s_sm_size}};
        memcpy(s_sm_u, u, sizeof s_sm_u);
    }
    glDepthMask(dmask);
    pfx_sprites_depth(lw, lh);
    s_spr_built = s_pfx_presents;
}
/* Light sources: the presented picture's glowing pieces (sc_add_emit)
 * grouped (within LIT_JOIN native px and 20% of the depth), each group placed
 * by its own depth (3D) or the point projected with the camera nearest it
 * (2D; none near: the HUD, not a light), taken to the world (the picture's
 * camera), and followed from present to present
 * there (within LIT_MATCH units): one seen in LIT_STEADY presents or more,
 * and lately, or in LIT_START with hardly a miss (LIT_ALL), is a light that
 * stays (a fire, a lamp); a spell's flash is not (in a battle, up to 14
 * presents in a row, or 25 now and then). It comes up over LIT_START..
 * LIT_STEADY (ramp); one seen LIT_LEARN presents teaches its pieces' looks (sc_add_emit:
 * texels and colour, not where they sit in VRAM), and a glow with a look so
 * learned is a light at once: the campfire after a battle, the menu or
 * another room, or another one like it. (BoF4: the campfire's 2 pieces stay
 * for good; Nina's wind spell stays 130 VBlanks and comes up, as before, but
 * is not learned.)
 * Out: s_lit (world, GTE axes), for the lights that cast (steps to come) and
 * TCP shadow_cache "lights". */
enum { LIT_MAX = 16, LIT_JOIN = 24, LIT_START = 15, LIT_STEADY = 30, LIT_GONE = 20, LIT_LEARN = 90,
       LIT_LOOKS = 1024 };
#define LIT_DARK 0.02f   /* a glowing piece's brightest channel below this adds nothing */
#define LIT_MATCH 160.0f
#define LIT_ALL 0.9f     /* seen in this share of the light steps since it was first */
typedef struct { float w[3]; float sx, sy, z, size, after, c[3], ramp; unsigned hits, seen, first; int active, up, known; } PfxLight;
static PfxLight s_lit[LIT_MAX];
static int      s_lit_n = 0;
static unsigned s_lit_steps = 0;   /* pfx_lights_update calls (fresh presents) */
static uint32_t s_lit_look[LIT_LOOKS];   /* the learned looks (open addressing, 0 = empty) */
static int      s_lit_looks = 0;
static uint32_t *lit_look_slot(uint32_t key) {
    uint32_t i = (key * 2654435761u) >> 22;   /* 10 bits: LIT_LOOKS */
    while (s_lit_look[i] && s_lit_look[i] != key) i = (i + 1) & (LIT_LOOKS - 1);
    return &s_lit_look[i];
}
static int lit_look_known(uint32_t key) { return key && s_lit_looks > 0 && *lit_look_slot(key) == key; }
static void lit_look_learn(uint32_t key) {
    if (!key || lit_look_known(key)) return;
    if (s_lit_looks >= LIT_LOOKS * 3 / 4) { memset(s_lit_look, 0, sizeof s_lit_look); s_lit_looks = 0; }   /* full: learn anew */
    *lit_look_slot(key) = key;
    s_lit_looks++;
}
static void pfx_lights_update(void) {
    const ScSlot *sc = sc_presented();
    s_lit_steps++;
    for (int i = 0; i < s_lit_n; i++) s_lit[i].active = 0;
    if (!sc || !sc->cam_ok || sc->en <= 0) goto age;
    {
        const float *r = sc->cam.r, *t = sc->cam.t, h = sc->cam.h;
        /* groups: the pieces joined to a nearby earlier one (on the picture,
         * and in depth when both have one) */
        enum { G_MAX = 32 };
        float g[G_MAX][11];  /* sum x*a, sum y*a, sum a, sum z*a and sum a over the pieces with a depth, count,
                                most 3D triangles drawn after a piece, sum rgb*a, a piece's look learned */
        int ng = 0, gl[G_MAX];
        static uint8_t pg[1024];   /* each piece's group (255: none) */
        for (int i = 0; i < sc->en; i++) {
            const float *e = sc->ev + (size_t)i * SC_EV;
            pg[i] = 255;
            /* a piece that adds nothing (black) gives no light: BoF4 hides the
             * party member standing behind the leader by drawing it additive
             * in black, which lit the field round them after a battle */
            if (fmaxf(e[5], fmaxf(e[6], e[7])) < LIT_DARK) continue;
            /* nor does a dim grey one (dust, smoke: emit_dim_grey), however
             * long it stays (a party member running on the spot) */
            if (emit_dim_grey(e)) continue;
            const float x = e[0] - s_sc_cx, y = e[1] - s_sc_cy, z = e[2], a = fmaxf(e[3], 1.0f);
            int k = 0;
            for (; k < ng; k++) {
                const float gx = g[k][0] / g[k][2], gy = g[k][1] / g[k][2];
                if (fabsf(x - gx) >= LIT_JOIN || fabsf(y - gy) >= LIT_JOIN) continue;
                if (z > 0.0f && g[k][4] > 0.0f && fabsf(z - g[k][3] / g[k][4]) > 0.2f * z) continue;
                break;
            }
            if (k == ng) { if (ng == G_MAX) continue; memset(g[ng], 0, sizeof g[ng]); gl[ng] = -1; ng++; }
            pg[i] = (uint8_t)k;
            g[k][0] += x * a; g[k][1] += y * a; g[k][2] += a; g[k][5] += 1.0f;
            g[k][6] = fmaxf(g[k][6], (float)sc->n - e[4]);
            for (int c = 0; c < 3; c++) g[k][7 + c] += e[5 + c] * a;
            if (z > 0.0f) { g[k][3] += z * a; g[k][4] += a; }
            if (lit_look_known((uint32_t)e[15])) g[k][10] = 1.0f;
        }
        for (int k = 0; k < ng; k++) {
            const float sx = g[k][0] / g[k][2], sy = g[k][1] / g[k][2];
            /* its place in view space (GTE: y down): its own depth (3D), else
             * the point projected with the camera nearest it on the picture
             * (an effect drawn in 2D round it); none, or drawn over the
             * finished scene: not a light (the HUD). A piece in the scene is
             * sorted among it, so nearer scenery comes after it; after the HUD
             * at most an overlay (a 3D cursor: 2 of 538 triangles in a battle,
             * 12 of 68 after the cave's campfire) */
            float Vv[3];
            if (g[k][4] > 0.0f) {
                const float z = g[k][3] / g[k][4];
                Vv[0] = sx * z / h; Vv[1] = sy * z / h; Vv[2] = z;
            } else {
                if (g[k][6] < fmaxf(4.0f, 0.03f * (float)sc->n)) continue;
                int best = -1;
                float bd = (float)(LIT_JOIN * LIT_JOIN);
                for (int p = 0; p < sc->npts; p++) {
                    const float *P = sc->pts[p];
                    if (P[2] < 1.0f) continue;
                    const float dx = h * P[0] / P[2] - sx, dy = h * P[1] / P[2] - sy, d = dx * dx + dy * dy;
                    if (d < bd) { bd = d; best = p; }
                }
                if (best < 0) continue;
                memcpy(Vv, sc->pts[best], sizeof Vv);
            }
            const float z = Vv[2];
            /* GTE view (y down) -> world: R^T (V - t) */
            const float V[3] = {Vv[0] - t[0], Vv[1] - t[1], Vv[2] - t[2]};
            float w[3];
            for (int c = 0; c < 3; c++) w[c] = r[c] * V[0] + r[3 + c] * V[1] + r[6 + c] * V[2];
            int m = -1;
            float best = LIT_MATCH * LIT_MATCH;
            for (int i = 0; i < s_lit_n; i++) {
                const float dx = s_lit[i].w[0] - w[0], dy = s_lit[i].w[1] - w[1], dz = s_lit[i].w[2] - w[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < best) { best = d; m = i; }
            }
            if (m < 0) {
                if (s_lit_n < LIT_MAX) m = s_lit_n++;
                else {   /* the one seen longest ago makes room */
                    m = 0;
                    for (int i = 1; i < s_lit_n; i++) if (s_lit[i].seen < s_lit[m].seen) m = i;
                }
                memset(&s_lit[m], 0, sizeof s_lit[m]);
                s_lit[m].first = s_lit_steps;
                memcpy(s_lit[m].w, w, sizeof w);
                for (int c = 0; c < 3; c++) s_lit[m].c[c] = g[k][7 + c] / g[k][2];
            }
            PfxLight *L = &s_lit[m];
            for (int c = 0; c < 3; c++) L->w[c] = 0.8f * L->w[c] + 0.2f * w[c];   /* a flame moves; its light, less */
            for (int c = 0; c < 3; c++) L->c[c] = 0.9f * L->c[c] + 0.1f * g[k][7 + c] / g[k][2];   /* its colour: the pieces' (emit_tex_rgb) */
            L->sx = sx; L->sy = sy; L->z = z; L->size = g[k][2]; L->after = g[k][4] > 0.0f ? -1.0f : g[k][6];
            L->hits++; L->seen = s_pfx_presents;
            /* a learned look: at once (2 presents: not a one-off) and at full */
            if (g[k][10] > 0.0f && L->hits >= 2) L->known = 1;
            if (L->hits >= LIT_STEADY ||
                (L->hits >= LIT_START && (float)L->hits >= LIT_ALL * (float)(s_lit_steps - L->first + 1))) L->up = 1;
            L->active = L->known || L->up;
            L->ramp = L->known ? 1.0f
                    : fminf(fmaxf((float)((int)L->hits - LIT_START + 1) / (float)(LIT_STEADY - LIT_START + 1), 0.0f), 1.0f);
            gl[k] = m;
        }
        for (int i = 0; i < sc->en; i++)   /* a light that stays teaches its pieces' looks */
            if (pg[i] != 255 && gl[pg[i]] >= 0 && s_lit[gl[pg[i]]].hits >= LIT_LEARN)
                lit_look_learn((uint32_t)sc->ev[(size_t)i * SC_EV + 15]);
    }
age:
    for (int i = 0; i < s_lit_n;) {   /* gone: not seen for a while */
        if (s_pfx_presents - s_lit[i].seen > LIT_GONE) { s_lit[i] = s_lit[--s_lit_n]; continue; }
        i++;
    }
}
int gl_renderer_lights_json(char *out, int cap) {
    GL_RT_SYNC("lights_json");
    const ScSlot *sc = sc_presented();
    int k = snprintf(out, (size_t)cap, "\"glow_pieces\":[%u,%u,%u],\"glow_now\":%d,\"points_now\":%d,\"tris_now\":%d,\"uncovered\":%d,\"looks\":%d,\"lights\":[",
                     s_emit_seen[0], s_emit_seen[1], s_emit_seen[2], sc ? sc->en : -1, sc ? sc->npts : -1, sc ? sc->n : -1,
                     s_unc_last, s_lit_looks);
    for (int i = 0; i < s_lit_n && k < cap - 200; i++) {
        const PfxLight *L = &s_lit[i];
        k += snprintf(out + k, (size_t)(cap - k), "%s{\"world\":[%.0f,%.0f,%.0f],\"screen\":[%.1f,%.1f],\"z\":%.0f,\"size\":%.0f,\"after\":%.0f,\"rgb\":[%.2f,%.2f,%.2f],\"hits\":%u,\"active\":%d,\"known\":%d,\"ramp\":%.2f}",
                      i ? "," : "", L->w[0], L->w[1], L->w[2], L->sx, L->sy, L->z, L->size, L->after,
                      L->c[0], L->c[1], L->c[2], L->hits, L->active, L->known, L->ramp);
    }
    k += snprintf(out + k, (size_t)(cap - k), "],\"wg_agree\":[%d,%d,%d],\"bdm\":[%u,%u,%u,%d],\"pieces\":[", s_wg_in, s_wg_on, s_wg_trust, s_dbg_bdm[0], s_dbg_bdm[1], s_dbg_bdm[2], s_b3d_on);
  /* this frame's: x, y, z, area, 3D before it, look */
    for (int i = 0; sc && i < sc->en && i < 48 && k < cap - 96; i++) {
        const float *e = sc->ev + (size_t)i * SC_EV;
        k += snprintf(out + k, (size_t)(cap - k), "%s[%.0f,%.0f,%.0f,%.0f,%.0f,%.0f]", i ? "," : "", e[0], e[1], e[2], e[3], e[4], e[15]);
    }
    return k + snprintf(out + k, (size_t)(cap - k), "]");
}
/* The `sprite` input with no shadow map this present (a light that lights
 * the sprites, in an area with no sun or with the Sun off): the sprites stand
 * facing the camera, from the presented picture's own camera. */
static void pfx_sprites_alone(float aspect, int lw, int lh) {
    s_spr_built = s_pfx_presents;
    if (!s_sm_tex) pfx_sm_init();   /* the sprites' depth program */
    const ScSlot *sc = sc_presented();
    memset(s_sm_world, 0, sizeof s_sm_world);
    s_spr_n = s_spr_nx = 0;
    if (sc && sc->cam_ok) {
        memcpy(s_cam_r, sc->cam.r, sizeof s_cam_r); memcpy(s_cam_t, sc->cam.t, sizeof s_cam_t);
        s_cam_of[0] = sc->cam.of[0]; s_cam_of[1] = sc->cam.of[1]; s_cam_h = sc->cam.h;
        s_cam_ok = 1;
        const float *r = s_cam_r;
        float u[3] = {r[1], -r[4], r[7]};
        if (u[1] < 0.0f) { u[0] = -u[0]; u[1] = -u[1]; u[2] = -u[2]; }
        for (int i = 0; i < 3; i++) s_sm_world[1][i] = u[i];
        s_sm_world[1][3] = 1.0f;
        const float none[3] = {0.0f, 0.0f, 0.0f};   /* no light to turn toward */
        pfx_sprites_stand(sc, aspect, none);
    }
    pfx_sprites_depth(lw, lh);
}
/* The fire lights' shadow map (`firemap`): for the steady lights (s_lit,
 * the FIRE_MAX biggest), each raised u_firecfg.y units off its point (a
 * flame stands over its base), a cube of six 90-degree faces round it, drawn
 * side by side (3 x 2 tiles of FIRE_FACE px, one row pair per light); each
 * point keeps its distance to the light over the reach (u_firecfg.x world
 * units: beyond it, no light). Casting: the area's geometry, the frame's own
 * triangles (culled faces too) and the sprites standing (their cut-outs).
 * The face axes below and in the shaders that read it (fire_face) agree:
 * forward, right, up per face; a direction's face is its largest axis. */
static const float k_fire_face[6][3][3] = {
    {{ 1, 0, 0}, {0, 0, -1}, {0, 1,  0}}, {{-1, 0, 0}, {0, 0,  1}, {0, 1,  0}},
    {{ 0, 1, 0}, {1, 0,  0}, {0, 0, -1}}, {{ 0,-1, 0}, {1, 0,  0}, {0, 0,  1}},
    {{ 0, 0, 1}, {1, 0,  0}, {0, 1,  0}}, {{ 0, 0,-1}, {-1, 0, 0}, {0, 1,  0}}};
static float s_fl_view[FIRE_MAX][4];   /* each light: view space (y up), reach */
static float s_fl_scr[FIRE_MAX][4];    /* its place on the picture (uv), 1 */
static float s_fl_base[FIRE_MAX][3];   /* its glow's point (view space, y up), s_fl_scr's */
static float s_fl_k[2];                /* the picture's projection: uv per unit of x / z, y / z */
static float s_fl_col[FIRE_MAX][4];    /* its colour (its glow's, brightest channel 1), its strength (0..1) */
#define LIT_FULL 384.0f                /* a glow's size (px, the cave's campfire) that lights at full strength */
static int   s_fl_n = 0;
/* A fire light cast shadows in the last present (gpu.c: the game's own blob
 * shadows are hidden then, as under the sun). */
/* a fire light cast in the last presents (the chain may be gone since) */
int gl_renderer_fire_casting(void) { return s_fl_n > 0 && s_pfx_presents - s_fire_built <= 2u; }
static void pfx_fire_build(const PfxPass *ps, float aspect, int lw, int lh) {
    s_fire_built = s_pfx_presents;
    s_fl_n = 0;
    const float *fcol = pfx_pass_uniform(ps, "u_firecol");
    if (fcol && fcol[3] <= 0.0f) return;   /* the fire lights off (menu) */
    if (!s_sm_tex) pfx_sm_init();
    if (!s_fm_prog) return;
    const float *cfg = pfx_pass_uniform(ps, "u_firecfg");
    const float reach = cfg && cfg[0] > 0.0f ? cfg[0] : 1200.0f, lift = cfg ? cfg[1] : 48.0f;
    const ScSlot *sc = sc_presented();
    if (!sc || !sc->cam_ok) return;
    if (s_spr_built != s_pfx_presents) pfx_sprites_alone(aspect, lw, lh);
    memcpy(s_cam_r, sc->cam.r, sizeof s_cam_r); memcpy(s_cam_t, sc->cam.t, sizeof s_cam_t);
    s_cam_of[0] = sc->cam.of[0]; s_cam_of[1] = sc->cam.of[1]; s_cam_h = sc->cam.h; s_cam_ok = 1;
    const float *r = s_cam_r, h = s_cam_h;
    float up[3] = {r[1], -r[4], r[7]};   /* world up in view space (y up) */
    if (up[1] < 0.0f) { up[0] = -up[0]; up[1] = -up[1]; up[2] = -up[2]; }
    /* the lights: the steady ones, biggest first */
    int pick[LIT_MAX], np = 0;
    for (int i = 0; i < s_lit_n; i++)
        if (s_lit[i].active) {
            int j = np++;
            while (j > 0 && s_lit[pick[j - 1]].size < s_lit[i].size) { pick[j] = pick[j - 1]; j--; }
            pick[j] = i;
        }
    int nl = 0;
    for (int k = 0; k < np && nl < FIRE_MAX; k++) {
        const float *w = s_lit[pick[k]].w;
        float V[3];
        for (int c = 0; c < 3; c++) V[c] = r[c * 3] * w[0] + r[c * 3 + 1] * w[1] + r[c * 3 + 2] * w[2] + s_cam_t[c];
        if (V[2] < 16.0f) continue;
        const float P[3] = {V[0] + up[0] * lift, -V[1] + up[1] * lift, V[2] + up[2] * lift};
        memcpy(s_fl_view[nl], P, sizeof P); s_fl_view[nl][3] = reach;
        s_fl_scr[nl][0] = 0.5f + (h * V[0] / V[2]) / (240.0f * aspect);
        s_fl_scr[nl][1] = 0.5f - (h * V[1] / V[2]) / 240.0f;
        s_fl_scr[nl][2] = 1.0f; s_fl_scr[nl][3] = 0.0f;
        s_fl_base[nl][0] = V[0]; s_fl_base[nl][1] = -V[1]; s_fl_base[nl][2] = V[2];
        s_fl_k[0] = h / (240.0f * aspect); s_fl_k[1] = h / 240.0f;
        {   /* its glow's colour, as bright as can be (none known: u_firecol's);
             * its strength and reach by how big its glow is: a campfire's
             * (LIT_FULL px) full, a staff's small orb a quarter or so */
            const float *c = s_lit[pick[k]].c, mx = fmaxf(c[0], fmaxf(c[1], c[2]));
            for (int i = 0; i < 3; i++) s_fl_col[nl][i] = mx > 0.05f ? c[i] / mx : (fcol ? fcol[i] : 1.0f);
            const float big = fminf(fmaxf(sqrtf(s_lit[pick[k]].size / LIT_FULL), 0.25f), 1.0f);
            /* light-out.glsl: strength, and reach (the map's depth stays over
             * `reach`); a new light's grow over its ramp */
            s_fl_col[nl][3] = big * s_lit[pick[k]].ramp;
        }
        nl++;
    }
    s_fl_n = nl;
    if (!nl) return;
    /* the casters, in view space (y up) */
    static float *buf = NULL; static int cap = 0;
    const int use_world = s_wg_n > 0 && s_wg_trust;   /* the map, while the picture's floor lies on it (wg_agree) */
    const int nw = use_world ? s_wg_n * 3 : 0, nf = sc->n * 3, ns = s_spr_n + s_spr_nx;
    const int n = nw + nf + ns;
    if (n <= 0) { s_fl_n = 0; return; }
    if (n > cap) {
        float *p = (float *)realloc(buf, (size_t)n * SC_V * sizeof(float));
        if (!p) { s_fl_n = 0; return; }
        buf = p; cap = n;
    }
    const float k = 1.0f / h;
    for (int i = 0; i < n; i++) {
        float *o = buf + (size_t)i * SC_V;
        if (i < nw) {
            float V[3]; scc_view(s_wg + (size_t)i * 3, V);
            o[0] = V[0]; o[1] = -V[1]; o[2] = V[2];
            o[3] = o[4] = o[5] = o[6] = o[7] = o[8] = o[10] = 0.0f; o[9] = -1.0f;   /* solid */
        } else if (i < nw + nf) {
            const float *v = sc->v + (size_t)(i - nw) * SC_V;
            const float z = v[2];
            o[0] = (v[0] - s_sc_cx) * z * k; o[1] = -(v[1] - s_sc_cy) * z * k; o[2] = z;
            memcpy(o + 3, v + 3, (SC_V - 3) * sizeof(float));
        } else {
            memcpy(o, spr_caster(i - nw - nf), SC_V * sizeof(float));
        }
    }
    /* the map: its texture and target, made once */
    if (!s_fm_tex) {
        glGenTextures(1, &s_fm_tex);
        glBindTexture(GL_TEXTURE_2D, s_fm_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6 /*DEPTH_COMPONENT24*/, 3 * FIRE_FACE, 2 * FIRE_FACE * FIRE_MAX, 0,
                     0x1902 /*DEPTH_COMPONENT*/, 0x1405 /*UNSIGNED_INT*/, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        p_glGenFramebuffers(1, &s_fm_fbo);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_fm_fbo);
        p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, 0x8D00 /*DEPTH_ATTACHMENT*/, GL_TEXTURE_2D, s_fm_tex, 0);
        glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
    }
    static GLint u_lp = -2, u_ff, u_fr, u_fu, u_far;
    if (u_lp == -2) {
        u_lp = p_glGetUniformLocation(s_fm_prog, "u_lp"); u_ff = p_glGetUniformLocation(s_fm_prog, "u_ff");
        u_fr = p_glGetUniformLocation(s_fm_prog, "u_fr"); u_fu = p_glGetUniformLocation(s_fm_prog, "u_fu");
        u_far = p_glGetUniformLocation(s_fm_prog, "u_far");
    }
    GLboolean dmask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_fm_fbo);
    glViewport(0, 0, 3 * FIRE_FACE, 2 * FIRE_FACE * FIRE_MAX);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    p_glUseProgram(s_fm_prog);
    p_glBindVertexArray(s_sm_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_sm_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * SC_V * sizeof(float)), buf, PSXGL_STREAM_DRAW);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_raw_tex);
    p_glUniform1i(s_fm_uvram, 0);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, s_spr_atl_tex);
    p_glUniform1i(s_fm_uatl, 1);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    for (int l = 0; l < nl; l++) {
        p_glUniform3f(u_lp, s_fl_view[l][0], s_fl_view[l][1], s_fl_view[l][2]);
        p_glUniform1f(u_far, reach);
        for (int f = 0; f < 6; f++) {
            glViewport((f % 3) * FIRE_FACE, (f / 3 + 2 * l) * FIRE_FACE, FIRE_FACE, FIRE_FACE);
            p_glUniform3f(u_ff, k_fire_face[f][0][0], k_fire_face[f][0][1], k_fire_face[f][0][2]);
            p_glUniform3f(u_fr, k_fire_face[f][1][0], k_fire_face[f][1][1], k_fire_face[f][1][2]);
            p_glUniform3f(u_fu, k_fire_face[f][2][0], k_fire_face[f][2][1], k_fire_face[f][2][2]);
            glDrawArrays(GL_TRIANGLES, 0, n);
        }
    }
    glDisable(GL_DEPTH_TEST);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glBindVertexArray(0);
    glDepthMask(dmask);
}
/* For a pass reading `firemap`: u_fl0, u_fl1 the lights (view space, y up;
 * w the reach), u_fls0, u_fls1 their place on the picture (uv; z 1 = lit),
 * u_flc0, u_flc1 their colours (w their strength), u_flmap (face px,
 * lights, row pairs). */
static void pfx_fire_uniforms(GLuint prog) {
    static const char *const pn[FIRE_MAX] = {"u_fl0", "u_fl1"}, *const sn[FIRE_MAX] = {"u_fls0", "u_fls1"};
    static const char *const cn[FIRE_MAX] = {"u_flc0", "u_flc1"};
    for (int l = 0; l < FIRE_MAX; l++) {
        const GLint lc = p_glGetUniformLocation(prog, cn[l]);
        if (lc >= 0) {
            if (l < s_fl_n) p_glUniform4f(lc, s_fl_col[l][0], s_fl_col[l][1], s_fl_col[l][2], s_fl_col[l][3]);
            else p_glUniform4f(lc, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        float v[4] = {0.0f, 0.0f, 0.0f, 0.0f}, s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (l < s_fl_n) {
            memcpy(v, s_fl_view[l], sizeof v); memcpy(s, s_fl_scr[l], sizeof s);
            if (s_pfx_vx_on) {
                float b[3];
                pfx_vx_point(s_fl_view[l], v);
                pfx_vx_point(s_fl_base[l], b);
                if (b[2] > 1.0f) { s[0] = 0.5f + s_fl_k[0] * b[0] / b[2]; s[1] = 0.5f + s_fl_k[1] * b[1] / b[2]; }
            }
        }
        GLint loc = p_glGetUniformLocation(prog, pn[l]);
        if (loc >= 0) p_glUniform4f(loc, v[0], v[1], v[2], v[3]);
        loc = p_glGetUniformLocation(prog, sn[l]);
        if (loc >= 0) p_glUniform4f(loc, s[0], s[1], s[2], s[3]);
    }
    const GLint loc = p_glGetUniformLocation(prog, "u_flmap");
    if (loc >= 0) p_glUniform4f(loc, (float)FIRE_FACE, (float)s_fl_n, (float)(2 * FIRE_MAX), 0.0f);
}
static void pfx_shadow_uniforms(GLuint prog) {
    static const char *const names[4] = {"u_smx", "u_smy", "u_smz", "u_sms"};
    for (int k = 0; k < 4; k++) {
        const GLint loc = p_glGetUniformLocation(prog, names[k]);
        float u[4];
        if (k < 3 && s_pfx_vx_on) pfx_vx_row(s_sm_u[k], u); else memcpy(u, s_sm_u[k], sizeof u);
        if (loc >= 0) p_glUniform4f(loc, u[0], u[1], u[2], u[3]);
    }
    static const char *const sn[4] = {"u_ssx", "u_ssy", "u_ssz", "u_sss"};   /* the sprites' map (w 0: the scenery's) */
    for (int k = 0; k < 4; k++) {
        const GLint loc = p_glGetUniformLocation(prog, sn[k]);
        float u[4];
        if (k < 3 && s_pfx_vx_on && s_ss_u[3][3] > 0.0f) pfx_vx_row(s_ss_u[k], u); else memcpy(u, s_ss_u[k], sizeof u);
        if (loc >= 0) p_glUniform4f(loc, u[0], u[1], u[2], u[3]);
    }
    /* the camera's world axes and the light, when the framework knows them
     * (after the preset's own values, which they replace) */
    {   /* the horizon: world up in view space (y up) and the camera's H, so a
         * pass finds the view rays that point below it (sun-out.glsl: a painted
         * backdrop's ground); w 0 = no camera */
        const GLint loc = p_glGetUniformLocation(prog, "u_horizon");
        if (loc >= 0) {
            float up[3];
            if (s_pfx_vx_on) pfx_vx_dir(s_sm_world[1], up); else memcpy(up, s_sm_world[1], sizeof up);
            if (s_sm_world[1][3] > 0.5f && s_cam_h > 0.0f) p_glUniform4f(loc, up[0], up[1], up[2], s_cam_h);
            else p_glUniform4f(loc, 0.0f, 0.0f, 0.0f, 0.0f);
        }
    }
    static const char *const wn[3] = {"u_world", "u_worldup", "u_shadowdir"};
    for (int k = 0; k < 3; k++) {
        if (s_sm_world[k][3] < 0.5f) continue;
        const GLint loc = p_glGetUniformLocation(prog, wn[k]);
        float d[3];
        if (s_pfx_vx_on) pfx_vx_dir(s_sm_world[k], d); else memcpy(d, s_sm_world[k], sizeof d);
        if (loc >= 0) p_glUniform4f(loc, d[0], d[1], d[2], 1.0f);
    }
    {   /* gl_renderer_set_night: x = the night's share of the light */
        const GLint loc = p_glGetUniformLocation(prog, "u_night");
        if (loc >= 0) p_glUniform4f(loc, s_night_f, 0.0f, 0.0f, 0.0f);
    }
}
/* gl_renderer_scene_tone: the picture on screen's 3D scenery (its slot's
 * tone), at each real present; none = nothing (cover 0). A slot stays on
 * screen while nothing new is drawn (a black screen, a 2D menu): after
 * TONE_STALE presents with no 3D triangle summed anywhere it is none, and
 * stays none until a slot summed after that stretch is shown (the first
 * new frame goes to the other buffer of a double-buffered game). */
enum { TONE_STALE = 6 };
static void pfx_tone_publish(void) {
    static unsigned seen = 0, idle = 0;
    if (s_tone_adds != seen) { seen = s_tone_adds; idle = 0; }
    else if (idle <= TONE_STALE && ++idle > TONE_STALE) s_tone_epoch++;
    float t[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const ScSlot *sc = sc_presented();
    if (sc && sc->tone[3] > 0.0f && sc->tone_epoch == s_tone_epoch) {
        const float da = (float)(sc->area[2] - sc->area[0] + 1) * (float)(sc->area[3] - sc->area[1] + 1);
        for (int c = 0; c < 3; c++) t[c] = sc->tone[c] / sc->tone[3];
        t[3] = da > 0.0f ? sc->tone[3] / da : 0.0f;
    }
    tone_publish(t);
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
                L->fixed[PFX_LOC_TIME] = p_glGetUniformLocation(pg, "u_time");
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
        if (reads_shadow) pfx_shadow_build(ps, (float)lw / (float)lh, lw, lh);
        for (int k = 0; k < ps->ninputs; k++)
            if (ps->in[k].from == PFX_IN_SPRITE && s_spr_built != s_pfx_presents) pfx_sprites_alone((float)lw / (float)lh, lw, lh);
        int reads_fire = 0;
        for (int k = 0; k < ps->ninputs; k++)
            if (ps->in[k].from == PFX_IN_FIREMAP) reads_fire = 1;
        if (reads_fire && s_fire_built != s_pfx_presents) pfx_fire_build(ps, (float)lw / (float)lh, lw, lh);
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
                           : from == PFX_IN_SPRSHADOW ? s_sm2_tex
                           : from == PFX_IN_SPRITE ? s_spr_tex
                           : from == PFX_IN_FIREMAP ? s_fm_tex
                           : st->tex[from];
            p_glActiveTexture(PSXGL_TEXTURE0 + (GLenum)k);
            glBindTexture(GL_TEXTURE_2D, t);
            p_glUniform1i(L->in[k], k);
        }
        p_glActiveTexture(PSXGL_TEXTURE0);
        p_glUniform2f(L->fixed[PFX_LOC_RCP], 1.0f / (float)w, 1.0f / (float)h);
        p_glUniform2f(L->fixed[PFX_LOC_SIZE], (float)w, (float)h);
        p_glUniform2i(L->fixed[PFX_LOC_ORG], last ? lx : 0, last ? ly : 0);
        if (L->fixed[PFX_LOC_TIME] >= 0) p_glUniform1f(L->fixed[PFX_LOC_TIME], pfx_time_now());
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
        /* after the preset's own; every pass of a chain that reads the map (its
         * composite needs the light too: a painted backdrop lit as ground) */
        if (reads_shadow || st->chain.uses_shadow) pfx_shadow_uniforms(prog);
        if (reads_fire) pfx_fire_uniforms(prog);
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
 * with_aa: a real frame (runs AA too); 0: a Smooth motion generated frame.
 * s_pfx_src_gen: a generated frame noted its own surfaces (fg_compose): its
 * marks and depth are built from them, the rest stays a generated frame's
 * (no light tracking step, no slot marked shown). */
static int s_pfx_src_gen = 0;
static void postfx_present(int fresh, int with_aa, int lx, int ly, int lw, int lh) {
    const int src_fresh = fresh || s_pfx_src_gen;
    s_pfx_presents++;
    s_night_f = atomic_load_explicit(&s_night_req, memory_order_relaxed);
    if (fresh) { pfx_lights_update(); pfx_tone_publish(); }
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
        if (st->chain.uses_shadow && gpu_shadow_off_active()) continue;   /* no place under the sun (a world map) */
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
    if (hdr_mask_wanted()) {    /* HDR reads the masks at the swap (gl_hdr.inc) */
        if (!mask_done) { mask_ok = pfx_mask_build(src_fresh, lw, lh); mask_done = 1; }
        hdr_mask_noted(mask_ok, lx, ly, lw, lh);
    }
    s_mask_src_valid = 0; s_mask_ctr_valid = 0;
    s_dfill_valid = s_dfill_ctr_valid = 0;
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
 *     window, heavily blurred and darkened, as video players and remasters do
 *     (mode 1); in mode 2 (flat colour only) they stay black instead.
 * FMV keeps black bars (CPU-readout presents and recent MDEC decodes are
 * skipped). It reads only the finished output pixels (after the post-effect
 * chains, before hold-last capture and the OSD) and draws only outside the
 * picture, so the picture, guest VRAM and every surface stay untouched. Off by
 * default; a bezel image, when set, takes precedence. */
extern int mdec_recently_active(uint32_t within_frames);
static GLuint s_bezel_tex;                  /* defined with the bezel code below */
static int    s_pillar_blur = 0;            /* 0 off, 1 flat colour or blur, 2 flat colour or black */
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
    p_glUniform1f(s_pb_draw_dim, s_pillar_blur == 2 ? 0.0f : 0.7f);   /* 2: no blur, black */
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

/* on: 0 off, 1 flat colour or blur, 2 flat colour or black.
 * PSX_PILLARBOX_FILL=auto|flat|black overrides [video] pillarbox_fill. */
void gl_renderer_set_pillarbox_blur(int on) {
    GL_RT_SYNC("pillarbox_fill");
    const char *e = getenv("PSX_PILLARBOX_FILL");
    if (e && *e) on = !strcmp(e, "black") ? 0 : !strcmp(e, "flat") ? 2 : 1;
    s_pillar_blur = on == 2 ? 2 : on ? 1 : 0;
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
