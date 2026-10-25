/* ---- HDR output (docs/HDR_OUTPUT.md) ----------------------------------------
 * Opt-in, off by default (PSX_HDR=1). With it on, the window is created with
 * a float16 back buffer (SDL_GL_FLOATBUFFERS, set before the window exists:
 * gl_renderer_hdr_window_attributes). On Windows with HDR on in the display
 * settings the compositor shows that buffer as scRGB: linear light, Rec.709
 * primaries, 1.0 = 80 nits, values above the SDR white reaching the display's
 * highlights (an 8-bit buffer is clamped to 1.0 and shown at the SDR white).
 *
 * Everything before the swap still composes the usual SDR picture (the game,
 * the post effects, CRT, OSD), sRGB encoded in 0..1. The last step before the
 * swap (hdr_finish) turns it into scRGB: clamp to 0..1 (what the 8-bit buffer
 * kept), decode (the sRGB curve, which is how Windows decodes every SDR window
 * in HDR mode, or gamma 2.2), times the paper white (Windows' SDR white level
 * unless PSX_HDR_PAPER gives nits), and, opt-in (PSX_HDR_BOOST > 0), the
 * bright part of the scene's lights lifted toward the display's peak
 * (PSX_HDR_PEAK nits, or what the display reports). The lights are the
 * frame's glowing pieces the fire light counts (pfx_lights_update): additive,
 * not black, not dim grey (dust, smoke), and either 3D or with 3D scenery
 * drawn after them (the HUD's, drawn over the finished scene, never are),
 * drawn into a mask of the picture (hdr_lights_build) and kept only where the
 * 3D mark is (mask3d green: opaque 2D drawn over a light, a sprite or a text
 * box, clears it), so the interface and the characters keep the paper white.
 * A big overlay (save-state menu, a title's page or HUD, rewind) drawn over
 * the picture switches the boost off for that present. The boost needs the
 * 3D mark, so geometry correction (PGXP) on.
 *
 * Boost 0 with Windows' paper white is, pixel for pixel, the picture an SDR
 * window shows in HDR mode. On an SDR display (HDR off in Windows) 1.0 is
 * the display's white: the paper white is 1 and nothing is lifted. Off
 * (PSX_HDR unset): none of this runs and the window keeps its usual 8-bit
 * buffer, so the picture is byte-identical to a build without it.
 *
 * Dynamic tone mapping (opt-in, PSX_HDR_DYNAMIC=1): each frame is measured on
 * the GPU (hdr_dyn_measure: the picture's log-average light and the share of
 * it the boost lifts, 64x36 cells, then one texel), followed in time (light:
 * PSX_HDR_SPEED seconds, faster on a cut; lifted share: at once up, slowly
 * down, as a compressor) and read by the last pass, no CPU round trip:
 *   - exposure: the paper white follows the scene a little, up in the dark
 *     (a cave), down in the bright (noon in the desert): (0.18 / light)^(0.2
 *     PSX_HDR_ADAPT), within 0.85..1.3;
 *   - highlight budget: while the lifted share is over PSX_HDR_AREA (1.5% of
 *     the picture) the boost shrinks by sqrt(area / share), so a flash over the
 *     whole screen does not go to the display's peak;
 *   - a soft shoulder from 80% of the display's peak to the peak, so nothing
 *     clips (also when PSX_HDR_PEAK asks for more than the display has). */

/* Dynamic tone mapping (see above). */
enum { HDR_DYN_W = 64, HDR_DYN_H = 36 };
static int    s_hdr_dyn = 0;            /* PSX_HDR_DYNAMIC */
static float  s_hdr_adapt = 0.5f;       /* PSX_HDR_ADAPT: exposure following, 0..1 */
static float  s_hdr_area = 0.015f;      /* PSX_HDR_AREA: lifted share with the full boost */
static float  s_hdr_speed = 0.5f;       /* PSX_HDR_SPEED: seconds (time constant) */
static GLuint s_hdr_st_prog = 0, s_hdr_ad_prog = 0, s_hdr_st_tex = 0, s_hdr_st_fbo = 0;
static GLuint s_hdr_ad_tex[2], s_hdr_ad_fbo[2];
static int    s_hdr_dyn_failed = 0, s_hdr_ad_cur = 0, s_hdr_ad_valid = 0;
static uint64_t s_hdr_ad_t = 0;         /* SDL counter at the last measure */
static float  s_hdr_dyn_state[4];       /* the last probe's followed state (TCP) */
static GLint  s_hdr_st_uSrc = -1, s_hdr_st_uMask = -1, s_hdr_st_uLmask = -1, s_hdr_st_uMrect = -1,
              s_hdr_st_uP = -1, s_hdr_st_uGamma = -1, s_hdr_st_uMr = -1, s_hdr_st_uPic = -1;
static GLint  s_hdr_ad_uSt = -1, s_hdr_ad_uPrev = -1, s_hdr_ad_uK = -1;
static GLint  s_hdr_uState = -1, s_hdr_uDyn = -1, s_hdr_uDynp = -1;

/* PSX_HDR and its settings, read once (before the window is created). */
static void hdr_env_once(void) {
    if (s_hdr_want >= 0) return;
    s_hdr_want = 0;
    const char *e;
#if defined(_WIN32) && defined(PSX_SDL3)
    e = getenv("PSX_HDR");
    s_hdr_want = (e && *e && *e != '0') ? 1 : 0;
#endif
    if ((e = getenv("PSX_HDR_PAPER")) && *e) s_hdr_paper_nits = (float)atof(e);
    if ((e = getenv("PSX_HDR_PEAK")) && *e) s_hdr_peak_nits = (float)atof(e);
    if ((e = getenv("PSX_HDR_BOOST")) && *e) s_hdr_boost = (float)atof(e);
    if ((e = getenv("PSX_HDR_KNEE")) && *e) s_hdr_knee = (float)atof(e);
    if ((e = getenv("PSX_HDR_GAMMA")) && *e) s_hdr_gamma = !strcmp(e, "2.2") ? 1 : 0;
    if ((e = getenv("PSX_HDR_DYNAMIC")) && *e) s_hdr_dyn = *e != '0';
    if ((e = getenv("PSX_HDR_ADAPT")) && *e) s_hdr_adapt = (float)atof(e);
    if ((e = getenv("PSX_HDR_AREA")) && *e) s_hdr_area = (float)atof(e);
    if ((e = getenv("PSX_HDR_SPEED")) && *e) s_hdr_speed = (float)atof(e);
    if (!(s_hdr_adapt >= 0.0f)) s_hdr_adapt = 0.0f;
    if (s_hdr_adapt > 1.0f) s_hdr_adapt = 1.0f;
    if (!(s_hdr_area > 0.0005f)) s_hdr_area = 0.015f;
    if (!(s_hdr_speed >= 0.02f)) s_hdr_speed = 0.5f;
    if (!(s_hdr_paper_nits >= 0.0f)) s_hdr_paper_nits = 0.0f;
    if (!(s_hdr_peak_nits >= 0.0f)) s_hdr_peak_nits = 0.0f;
    if (!(s_hdr_boost >= 0.0f)) s_hdr_boost = 0.0f;
    if (s_hdr_boost > 1.0f) s_hdr_boost = 1.0f;
    if (!(s_hdr_knee >= 0.0f) || s_hdr_knee > 0.95f) s_hdr_knee = 0.6f;
}

int gl_renderer_hdr_wanted(void) {
    hdr_env_once();
    return s_hdr_want;
}

/* The next window's pixel format. SDL's attributes are global: what HDR
 * changes is put back when it is off, so the usual window is the usual one. */
void gl_renderer_hdr_window_attributes(int on) {
#if defined(PSX_SDL3)
    static int saved = 0, r = 8, g = 8, b = 8, a = 0;
    if (on) {
        if (!saved) {
            SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
            SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
            SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
            SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
            saved = 1;
        }
        SDL_GL_SetAttribute(SDL_GL_FLOATBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 16);
    } else if (saved) {
        SDL_GL_SetAttribute(SDL_GL_FLOATBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, r);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, g);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, b);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, a);
    }
#else
    (void)on;
#endif
}

/* After the context exists: HDR runs only when it was asked for and the
 * window really got a float back buffer. */
static void hdr_init_context(void) {
    s_hdr_on = 0;
    s_hdr_float = 0;
    if (!gl_renderer_hdr_wanted()) return;
    typedef void (APIENTRY *PFN_gfap)(GLenum, GLenum, GLenum, GLint *);
    PFN_gfap gfap = (PFN_gfap)SDL_GL_GetProcAddress("glGetFramebufferAttachmentParameteriv");
    GLint type = 0, bits = 0;
    p_glBindFramebuffer_raw(PSXGL_FRAMEBUFFER, 0);
    while (glGetError() != GL_NO_ERROR) {}
    if (gfap) {
        gfap(PSXGL_FRAMEBUFFER, GL_BACK_LEFT, 0x8211 /* COMPONENT_TYPE */, &type);
        gfap(PSXGL_FRAMEBUFFER, GL_BACK_LEFT, 0x8212 /* RED_SIZE */, &bits);
    }
    if (glGetError() != GL_NO_ERROR) type = 0;
    s_hdr_float = type == GL_FLOAT && bits >= 16;
    s_hdr_on = s_hdr_float;
    fprintf(stdout, "psxrecomp: HDR output %s (back buffer: %d-bit %s)\n",
            s_hdr_on ? "on" : "asked for but unavailable", (int)bits,
            type == GL_FLOAT ? "float" : "fixed");
    pfx_mask_wanted_update();
}

/* paper and peak in scRGB units (1.0 = 80 nits); 0: an SDR display. */
static int hdr_levels(float *paper, float *peak) {
    float white = 1.0f, head = 1.0f;
    int hdr = 0;
#if defined(PSX_SDL3)
    SDL_PropertiesID p = SDL_GetWindowProperties(s_win);
    hdr = SDL_GetBooleanProperty(p, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false) ? 1 : 0;
    white = SDL_GetFloatProperty(p, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
    head = SDL_GetFloatProperty(p, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
#endif
    if (!(white >= 1.0f)) white = 1.0f;
    if (!(head >= 1.0f)) head = 1.0f;
    s_hdr_sdr_white = white;
    s_hdr_headroom = head;
    if (!hdr) { *paper = 1.0f; *peak = 1.0f; return 0; }
    *paper = s_hdr_paper_nits > 0.0f ? s_hdr_paper_nits / 80.0f : white;
    *peak = s_hdr_peak_nits > 0.0f ? s_hdr_peak_nits / 80.0f : white * head;
    if (*peak < *paper) *peak = *paper;
    return 1;
}

/* What the measure and the last pass share: the decode and how much of the
 * boost a pixel takes (0..1). */
static const char *HDR_COMMON =
    "#version 330\n"
    "uniform sampler2D u_src;\n"   /* the composed SDR picture, window sized */
    "uniform sampler2D u_mask;\n"  /* the effects' mask of the letterbox rect (g: 3D mark) */
    "uniform sampler2D u_lmask;\n" /* the lights of that rect (hdr_lights_build) */
    "uniform ivec4 u_mrect;\n"     /* that rect in window px (GL), z = 0: no boost */
    "uniform vec4 u_p;\n"          /* paper, peak/paper - 1, boost, knee */
    "uniform int u_gamma;\n"       /* 0: sRGB curve, 1: gamma 2.2 */
    "uniform int u_mr;\n"          /* mask softening radius (px) */
    "vec3 eotf(vec3 c){\n"
    "  if (u_gamma == 1) return pow(c, vec3(2.2));\n"
    "  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));\n"
    "}\n"
    "float lift_w(ivec2 p, vec3 c){\n"
    "  if (u_mrect.z <= 0) return 0.0;\n"
    "  ivec2 q = p - u_mrect.xy;\n"
    "  if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, u_mrect.zw))) return 0.0;\n"
    /* a soft mask (3x3 taps u_mr apart): a light's edge fades in */
    "  float m = 0.0;\n"
    "  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {\n"
    "    ivec2 t = clamp(q + ivec2(x, y) * u_mr, ivec2(0), u_mrect.zw - 1);\n"
    "    m += texelFetch(u_lmask, t, 0).r * texelFetch(u_mask, t, 0).g;\n"
    "  }\n"
    /* only its bright part: from the knee (encoded max channel) to full */
    "  float mx = max(c.r, max(c.g, c.b));\n"
    "  float t = clamp((mx - u_p.w) / (1.0 - u_p.w), 0.0, 1.0);\n"
    "  return m * (1.0 / 9.0) * t * t * (3.0 - 2.0 * t);\n"
    "}\n";

static const char *HDR_MAIN =
    "out vec4 o;\n"
    "uniform sampler2D u_state;\n" /* the followed measure (dynamic): log2 light, lifted share */
    "uniform int u_dyn;\n"
    "uniform vec4 u_dynp;\n"       /* exposure following, full-boost share, display peak (0: none), - */
    "uniform int u_dbg;\n"         /* 1: lifted pixels tinted magenta */
    "void main(){\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec3 c = clamp(texelFetch(u_src, p, 0).rgb, 0.0, 1.0);\n"
    "  vec3 lin = eotf(c);\n"
    "  float paper = u_p.x, peak = (u_p.y + 1.0) * u_p.x, boost = u_p.z;\n"
    "  if (u_dyn == 1) {\n"
    "    vec4 st = texelFetch(u_state, ivec2(0), 0);\n"
    "    paper = min(paper * clamp(pow(0.18 / exp2(st.r), 0.2 * u_dynp.x), 0.85, 1.3), peak);\n"
    "    boost *= min(1.0, sqrt(u_dynp.y / max(st.g, 1e-6)));\n"
    "  }\n"
    "  float w = boost > 0.0 ? lift_w(p, c) : 0.0;\n"
    "  float g = 1.0 + w * boost * (peak / paper - 1.0);\n"
    "  vec3 r = lin * paper * g;\n"
    /* the shoulder: 80% of the display's peak to its peak, nothing clips */
    "  float M = max(r.r, max(r.g, r.b)), dm = u_dynp.z, ks = 0.8 * dm;\n"
    "  if (dm > 0.0 && M > ks) r *= (ks + (dm - ks) * tanh((M - ks) / (dm - ks))) / M;\n"
    "  if (u_dbg == 1 && g > 1.001) r = vec3(r.r + r.g + r.b, 0.0, r.r + r.g + r.b) * 0.5;\n"
    "  o = vec4(r, 1.0);\n"
    "}\n";

/* The measure, one texel per cell of the picture (4x4 taps): log2 light (a
 * floor of 0.005, so black bars and night do not sink it), lifted share,
 * mean light. */
static const char *HDR_STATS =
    "out vec4 o;\n"
    "uniform ivec4 u_pic;\n"       /* the picture's rect, window px */
    "void main(){\n"
    "  vec2 cell = vec2(u_pic.zw) / vec2(64, 36);\n"
    "  float sg = 0.0, sw = 0.0, sl = 0.0;\n"
    "  for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {\n"
    "    ivec2 p = u_pic.xy + ivec2((floor(gl_FragCoord.xy) + (vec2(i, j) + 0.5) * 0.25) * cell);\n"
    "    vec3 c = clamp(texelFetch(u_src, p, 0).rgb, 0.0, 1.0);\n"
    "    float L = dot(eotf(c), vec3(0.2126, 0.7152, 0.0722));\n"
    "    sg += log2(max(L, 0.005)); sw += lift_w(p, c); sl += L;\n"
    "  }\n"
    "  o = vec4(sg, sw, sl, 16.0) * (1.0 / 16.0);\n"
    "}\n";

/* The cells into one texel, followed in time: light at u_k (u_k.y on a cut,
 * over a stop and a half), lifted share at once up and at u_k down. */
static const char *HDR_ADAPT =
    "#version 330\n"
    "out vec4 o;\n"
    "uniform sampler2D u_st;\n"
    "uniform sampler2D u_prev;\n"
    "uniform vec2 u_k;\n"          /* x: this frame's weight, y: on a cut; x = 1: start over */
    "void main(){\n"
    "  vec4 s = vec4(0.0);\n"
    "  for (int y = 0; y < 36; y++) for (int x = 0; x < 64; x++)\n"
    "    s += texelFetch(u_st, ivec2(x, y), 0);\n"
    "  s *= 1.0 / float(64 * 36);\n"
    "  vec4 q = texelFetch(u_prev, ivec2(0), 0);\n"
    "  if (u_k.x >= 1.0) { o = s; return; }\n"
    "  float kl = abs(s.r - q.r) > 1.5 ? u_k.y : u_k.x;\n"
    "  o = vec4(mix(q.r, s.r, kl), s.g > q.g ? s.g : mix(q.g, s.g, u_k.x), mix(q.b, s.b, kl), 1.0);\n"
    "}\n";

/* The scene's lights (sc_presented's glowing pieces, as pfx_lights_update
 * counts them) into an R8 mask of the letterbox rect, lw x lh: the pieces
 * that touch (boxes within 2 native px) make one light, drawn as a soft
 * ellipse over its box (1 in the middle, 0 at the edge), so a light lifts
 * its hot middle and never shows its quad's square edge (a campfire's quad
 * also holds the floor it lights). The number of lights drawn. */
static int hdr_lights_build(int lw, int lh) {
    if (!s_hdr_lm_prog) {
        if (s_hdr_lm_failed) return 0;
        s_hdr_lm_prog = build_program(
            "#version 330\nlayout(location = 0) in vec4 a_p;\nout vec2 v_l;\n"
            "void main(){ v_l = a_p.zw; gl_Position = vec4(a_p.xy, 0.0, 1.0); }\n",
            "#version 330\nin vec2 v_l; out vec4 o;\n"
            "void main(){ o = vec4(1.0 - smoothstep(0.3, 1.0, length(v_l))); }\n");
        if (!s_hdr_lm_prog) { s_hdr_lm_failed = 1; return 0; }
        p_glGenVertexArrays(1, &s_hdr_lm_vao);
        p_glGenBuffers(1, &s_hdr_lm_vbo);
        p_glBindVertexArray(s_hdr_lm_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_hdr_lm_vbo);
        p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        p_glEnableVertexAttribArray(0);
        p_glBindVertexArray(0);
    }
    if (s_hdr_lm_w != lw || s_hdr_lm_h != lh) {
        pfx_tex_alloc(&s_hdr_lm_tex, lw, lh, -1 /* R8 */, 1);
        pfx_fbo_attach(&s_hdr_lm_fbo, s_hdr_lm_tex);
        s_hdr_lm_w = lw; s_hdr_lm_h = lh;
    }
    float gb[HDR_LG_MAX][4];   /* the lights' boxes, VRAM px: x0, y0, x1, y1 */
    int ng = 0;
    const ScSlot *sc = sc_presented();
    if (sc && sc->en > 0 && s_sc_nh > 0.0f) {
        const float after_min = fmaxf(4.0f, 0.03f * (float)sc->n);
        for (int i = 0; i < sc->en; i++) {
            const float *e = sc->ev + (size_t)i * SC_EV;
            if (fmaxf(e[5], fmaxf(e[6], e[7])) * e[14] < LIT_DARK) continue;   /* adds nothing */
            if (emit_dim_grey(e)) continue;                                      /* dust, smoke */
            if (e[2] <= 0.0f && (float)sc->n - e[4] < after_min) continue;       /* over the scene: HUD */
            float b[4] = { fminf(e[8], fminf(e[10], e[12])), fminf(e[9], fminf(e[11], e[13])),
                           fmaxf(e[8], fmaxf(e[10], e[12])), fmaxf(e[9], fmaxf(e[11], e[13])) };
            int k = 0;
            for (; k < ng; k++)
                if (b[0] <= gb[k][2] + 2.0f && b[2] >= gb[k][0] - 2.0f &&
                    b[1] <= gb[k][3] + 2.0f && b[3] >= gb[k][1] - 2.0f) break;
            if (k == ng) {
                if (ng == HDR_LG_MAX) continue;
                memcpy(gb[ng++], b, sizeof b);
                continue;
            }
            gb[k][0] = fminf(gb[k][0], b[0]); gb[k][1] = fminf(gb[k][1], b[1]);
            gb[k][2] = fmaxf(gb[k][2], b[2]); gb[k][3] = fmaxf(gb[k][3], b[3]);
        }
        /* a light that grew into another: one */
        for (int again = 1; again;) {
            again = 0;
            for (int a = 0; a < ng; a++)
                for (int c = a + 1; c < ng; c++) {
                    if (gb[c][0] > gb[a][2] + 2.0f || gb[c][2] < gb[a][0] - 2.0f ||
                        gb[c][1] > gb[a][3] + 2.0f || gb[c][3] < gb[a][1] - 2.0f) continue;
                    gb[a][0] = fminf(gb[a][0], gb[c][0]); gb[a][1] = fminf(gb[a][1], gb[c][1]);
                    gb[a][2] = fmaxf(gb[a][2], gb[c][2]); gb[a][3] = fmaxf(gb[a][3], gb[c][3]);
                    memcpy(gb[c], gb[--ng], sizeof gb[c]);
                    again = 1;
                    c--;
                }
        }
    }
    const float k = s_sc_nh > 0.0f ? (float)lh / s_sc_nh : 1.0f;
    static const float corner[6][2] = { {-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1} };
    for (int g = 0; g < ng; g++)
        for (int v = 0; v < 6; v++) {
            const float x = corner[v][0] < 0 ? gb[g][0] - 1.0f : gb[g][2] + 1.0f;
            const float y = corner[v][1] < 0 ? gb[g][1] - 1.0f : gb[g][3] + 1.0f;
            float *o = s_hdr_lm_buf + ((size_t)g * 6 + v) * 4;
            o[0] = 2.0f * (x - s_sc_cx) * k / (float)lw;
            o[1] = -2.0f * (y - s_sc_cy) * k / (float)lh;
            o[2] = corner[v][0]; o[3] = corner[v][1];
        }
    GLfloat cc[4];
    glGetFloatv(0x0C22 /* COLOR_CLEAR_VALUE */, cc);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hdr_lm_fbo);
    glViewport(0, 0, lw, lh);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    if (ng) {
        p_glUseProgram(s_hdr_lm_prog);
        p_glBindVertexArray(s_hdr_lm_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_hdr_lm_vbo);
        p_glBufferData(PSXGL_ARRAY_BUFFER, (GLsizeiptr)((size_t)ng * 6 * 4 * sizeof(float)), s_hdr_lm_buf,
                       PSXGL_STREAM_DRAW);
        glEnable(GL_BLEND);
        p_glBlendEquationSeparate(0x8008 /* MAX */, 0x8008);
        glBlendFunc(GL_ONE, GL_ONE);
        glDrawArrays(GL_TRIANGLES, 0, ng * 6);
        p_glBlendEquationSeparate(PSXGL_FUNC_ADD, PSXGL_FUNC_ADD);
        glDisable(GL_BLEND);
        p_glBindVertexArray(0);
        p_glUseProgram(0);
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    return ng;
}

/* The present just composed shows the mask built for it (ok) over this rect:
 * its lights are drawn now, while its scene list is the presented one. */
static void hdr_mask_noted(int ok, int lx, int ly, int lw, int lh) {
    s_hdr_mask_ok = ok && s_mask_tex && lw > 0 && lh > 0;
    s_hdr_mrect[0] = lx; s_hdr_mrect[1] = ly; s_hdr_mrect[2] = lw; s_hdr_mrect[3] = lh;
    s_hdr_lights = s_hdr_mask_ok ? hdr_lights_build(lw, lh) : 0;
    if (!s_hdr_lm_tex) s_hdr_mask_ok = 0;
}

/* A present with no post-effect chain running: the mask for the boost alone. */
static void hdr_mask_present(int fresh, int lx, int ly, int lw, int lh) {
    if (!s_ctx || !post_clip_rect(&lx, &ly, &lw, &lh, 8)) {
        s_mask_src_valid = s_mask_ctr_valid = 0;
        return;
    }
    PfxGlState saved;
    pfx_state_save(&saved);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    hdr_mask_noted(pfx_mask_build(fresh, lw, lh), lx, ly, lw, lh);
    s_mask_src_valid = s_mask_ctr_valid = 0;
    if (fresh && s_sc_on && !s_pfx_src_gen) sc_mark_shown();   /* as postfx_present: the next frame starts over */
    pfx_state_restore(&saved);
}

static float hdr_h2f(uint16_t h) {
    const uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 31u, m = h & 0x3FFu;
    union { uint32_t u; float f; } v;
    if (e == 0) { v.f = (float)m * (1.0f / 16777216.0f); v.u |= s; return v.f; }
    if (e == 31) v.u = s | 0x7F800000u | (m << 13);
    else v.u = s | ((e + 112u) << 23) | (m << 13);
    return v.f;
}

/* TCP hdr "probe": the finished frame read back (half floats): statistics
 * and, with a path, a false-colour PNG at half size (grey: at or under the
 * paper white, as the SDR picture; yellow to red: lifted, red = peak). */
static void hdr_probe_take(int ww, int wh, float paper, float peak) {
    const size_t n = (size_t)ww * (size_t)wh;
    uint16_t *px = (uint16_t *)malloc(n * 4 * sizeof(uint16_t));
    if (!px) { s_hdr_probe.done = -1; return; }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    glReadPixels(0, 0, ww, wh, GL_RGBA, 0x140B /* HALF_FLOAT */, px);
    double sum = 0.0;
    float mx = 0.0f;
    uint64_t over = 0;
    const float lim = paper * 1.02f;
    for (size_t i = 0; i < n; i++) {
        const float r = hdr_h2f(px[i * 4]), g = hdr_h2f(px[i * 4 + 1]), b = hdr_h2f(px[i * 4 + 2]);
        const float m = fmaxf(r, fmaxf(g, b));
        sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
        if (m > mx) mx = m;
        if (m > lim) over++;
    }
    s_hdr_probe.max_nits = mx * 80.0f;
    s_hdr_probe.mean_nits = (float)(sum / (double)n * 80.0);
    s_hdr_probe.over_paper = (float)((double)over / (double)n);
    s_hdr_probe.paper_nits = paper * 80.0f;
    s_hdr_probe.peak_nits = peak * 80.0f;
    s_hdr_probe.w = ww; s_hdr_probe.h = wh;
    s_hdr_probe.written = 0;
    if (s_hdr_probe_path[0]) {
        const int ow = ww / 2, oh = wh / 2;
        uint8_t *rgb = (ow > 0 && oh > 0) ? (uint8_t *)malloc((size_t)ow * oh * 3) : NULL;
        if (rgb) {
            for (int y = 0; y < oh; y++)
                for (int x = 0; x < ow; x++) {
                    const uint16_t *s = px + ((size_t)(wh - 1 - 2 * y) * ww + (size_t)(2 * x)) * 4;
                    const float r = hdr_h2f(s[0]), g = hdr_h2f(s[1]), b = hdr_h2f(s[2]);
                    const float m = fmaxf(r, fmaxf(g, b));
                    uint8_t *d = rgb + ((size_t)y * ow + x) * 3;
                    if (m <= lim) {
                        const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        const uint8_t v = (uint8_t)lrintf(255.0f * powf(fminf(l / paper, 1.0f), 1.0f / 2.2f));
                        d[0] = d[1] = d[2] = v;
                    } else {
                        const float t = peak > paper ? fminf((m - paper) / (peak - paper), 1.0f) : 1.0f;
                        d[0] = 255; d[1] = (uint8_t)lrintf(255.0f * (1.0f - t)); d[2] = 0;
                    }
                }
            FILE *f = fopen(s_hdr_probe_path, "wb");
            if (f) {
                s_hdr_probe.written = png_write_rgb(f, rgb, (uint32_t)ow, (uint32_t)oh);
                fclose(f);
            }
            free(rgb);
        }
    }
    free(px);
    s_hdr_probe.done = 1;
}

/* Uniforms the measure and the last pass share (HDR_COMMON). */
static void hdr_common_uniforms(GLint uSrc, GLint uMask, GLint uLmask, GLint uMrect, GLint uP,
                                GLint uGamma, GLint uMr, int boost, float paper, float peak, int wh) {
    p_glUniform1i(uSrc, 0);
    p_glUniform1i(uMask, 1);
    p_glUniform1i(uLmask, 2);
    p_glUniform4i(uMrect, s_hdr_mrect[0], s_hdr_mrect[1], boost ? s_hdr_mrect[2] : 0, s_hdr_mrect[3]);
    p_glUniform4f(uP, paper, peak / paper - 1.0f, s_hdr_boost, s_hdr_knee);
    p_glUniform1i(uGamma, s_hdr_gamma);
    p_glUniform1i(uMr, wh >= 1080 ? wh / 540 : 1);
}

/* Dynamic tone mapping: this frame measured (the composed copy on unit 0,
 * the masks on 1 and 2) and followed into s_hdr_ad_tex[s_hdr_ad_cur]. 0 when
 * it cannot run (the last pass then maps as without it). */
static int hdr_dyn_measure(int ww, int wh, int boost, float paper, float peak) {
    if (s_hdr_dyn_failed) return 0;
    if (!s_hdr_st_prog) {
        char st[4096], ad[2048];
        snprintf(st, sizeof st, "%s%s", HDR_COMMON, HDR_STATS);
        snprintf(ad, sizeof ad, "%s", HDR_ADAPT);
        s_hdr_st_prog = build_program(PRESENT_VS, st);
        s_hdr_ad_prog = build_program(PRESENT_VS, ad);
        s_hdr_st_tex = make_tex(0x881A /* RGBA16F */, HDR_DYN_W, HDR_DYN_H, GL_RGBA, GL_FLOAT);
        int ok = s_hdr_st_prog && s_hdr_ad_prog && make_fbo(&s_hdr_st_fbo, s_hdr_st_tex, 0);
        for (int i = 0; i < 2 && ok; i++) {
            s_hdr_ad_tex[i] = make_tex(0x8814 /* RGBA32F */, 1, 1, GL_RGBA, GL_FLOAT);
            ok = make_fbo(&s_hdr_ad_fbo[i], s_hdr_ad_tex[i], 0);
        }
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
        if (!ok) { s_hdr_dyn_failed = 1; return 0; }
        s_hdr_st_uSrc = p_glGetUniformLocation(s_hdr_st_prog, "u_src");
        s_hdr_st_uMask = p_glGetUniformLocation(s_hdr_st_prog, "u_mask");
        s_hdr_st_uLmask = p_glGetUniformLocation(s_hdr_st_prog, "u_lmask");
        s_hdr_st_uMrect = p_glGetUniformLocation(s_hdr_st_prog, "u_mrect");
        s_hdr_st_uP = p_glGetUniformLocation(s_hdr_st_prog, "u_p");
        s_hdr_st_uGamma = p_glGetUniformLocation(s_hdr_st_prog, "u_gamma");
        s_hdr_st_uMr = p_glGetUniformLocation(s_hdr_st_prog, "u_mr");
        s_hdr_st_uPic = p_glGetUniformLocation(s_hdr_st_prog, "u_pic");
        s_hdr_ad_uSt = p_glGetUniformLocation(s_hdr_ad_prog, "u_st");
        s_hdr_ad_uPrev = p_glGetUniformLocation(s_hdr_ad_prog, "u_prev");
        s_hdr_ad_uK = p_glGetUniformLocation(s_hdr_ad_prog, "u_k");
        s_hdr_ad_valid = 0;
    }
    /* the picture's rect, else the window */
    int pic[4] = { s_hdr_pic_rect[0], s_hdr_pic_rect[1], s_hdr_pic_rect[2], s_hdr_pic_rect[3] };
    if (!post_clip_rect(&pic[0], &pic[1], &pic[2], &pic[3], HDR_DYN_W)) {
        pic[0] = pic[1] = 0; pic[2] = ww; pic[3] = wh;
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hdr_st_fbo);
    glViewport(0, 0, HDR_DYN_W, HDR_DYN_H);
    p_glUseProgram(s_hdr_st_prog);
    hdr_common_uniforms(s_hdr_st_uSrc, s_hdr_st_uMask, s_hdr_st_uLmask, s_hdr_st_uMrect, s_hdr_st_uP,
                        s_hdr_st_uGamma, s_hdr_st_uMr, boost, paper, peak, wh);
    p_glUniform4i(s_hdr_st_uPic, pic[0], pic[1], pic[2], pic[3]);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    /* followed: weight 1 - exp(-dt / speed), a cut four times faster */
    const uint64_t now = SDL_GetPerformanceCounter();
    float dt = s_hdr_ad_t ? (float)((double)(now - s_hdr_ad_t) / (double)SDL_GetPerformanceFrequency()) : 1.0f;
    if (dt > 0.25f) dt = 0.25f;
    s_hdr_ad_t = now;
    const float k = s_hdr_ad_valid ? 1.0f - expf(-dt / s_hdr_speed) : 1.0f;
    const float kc = s_hdr_ad_valid ? 1.0f - expf(-4.0f * dt / s_hdr_speed) : 1.0f;
    const int prev = s_hdr_ad_cur, cur = s_hdr_ad_cur ^ 1;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hdr_ad_fbo[cur]);
    glViewport(0, 0, 1, 1);
    p_glUseProgram(s_hdr_ad_prog);
    p_glActiveTexture(PSXGL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, s_hdr_ad_tex[prev]);
    p_glActiveTexture(PSXGL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, s_hdr_st_tex);
    p_glUniform1i(s_hdr_ad_uSt, 3);
    p_glUniform1i(s_hdr_ad_uPrev, 4);
    p_glUniform2f(s_hdr_ad_uK, k, kc);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glActiveTexture(PSXGL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glActiveTexture(PSXGL_TEXTURE0);
    s_hdr_ad_cur = cur;
    s_hdr_ad_valid = 1;
    return 1;
}

/* The last step before every swap: the composed SDR picture -> scRGB. */
static void hdr_finish(void) {
    if (!s_hdr_on || !s_ctx || !s_win) return;
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;
    float paper = 1.0f, peak = 1.0f;
    const int hdr_display = hdr_levels(&paper, &peak);
    s_hdr_last_paper = paper; s_hdr_last_peak = peak; s_hdr_display = hdr_display;
    if (!s_hdr_prog) {
        if (s_hdr_prog_failed) return;
        char fs[8192];
        snprintf(fs, sizeof fs, "%s%s", HDR_COMMON, HDR_MAIN);
        s_hdr_prog = build_program(PRESENT_VS, fs);
        if (!s_hdr_prog) { s_hdr_prog_failed = 1; return; }
        s_hdr_uSrc = p_glGetUniformLocation(s_hdr_prog, "u_src");
        s_hdr_uMask = p_glGetUniformLocation(s_hdr_prog, "u_mask");
        s_hdr_uLmask = p_glGetUniformLocation(s_hdr_prog, "u_lmask");
        s_hdr_uMrect = p_glGetUniformLocation(s_hdr_prog, "u_mrect");
        s_hdr_uP = p_glGetUniformLocation(s_hdr_prog, "u_p");
        s_hdr_uGamma = p_glGetUniformLocation(s_hdr_prog, "u_gamma");
        s_hdr_uMr = p_glGetUniformLocation(s_hdr_prog, "u_mr");
        s_hdr_uDbg = p_glGetUniformLocation(s_hdr_prog, "u_dbg");
        s_hdr_uState = p_glGetUniformLocation(s_hdr_prog, "u_state");
        s_hdr_uDyn = p_glGetUniformLocation(s_hdr_prog, "u_dyn");
        s_hdr_uDynp = p_glGetUniformLocation(s_hdr_prog, "u_dynp");
    }
    if (!s_hdr_fbo || s_hdr_w != ww || s_hdr_h != wh) {
        if (s_hdr_fbo) { p_glDeleteFramebuffers(1, &s_hdr_fbo); glDeleteTextures(1, &s_hdr_tex); }
        s_hdr_fbo = 0;
        s_hdr_tex = make_tex(0x881A /* RGBA16F */, ww, wh, GL_RGBA, GL_FLOAT);
        if (!make_fbo(&s_hdr_fbo, s_hdr_tex, 0)) {
            glDeleteTextures(1, &s_hdr_tex);
            s_hdr_tex = s_hdr_fbo = 0;
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
            return;
        }
        s_hdr_w = ww; s_hdr_h = wh;
    }
    PfxGlState saved;
    pfx_state_save(&saved);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_hdr_fbo);
    p_glBlitFramebuffer(0, 0, ww, wh, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    const int boost = hdr_display && s_hdr_boost > 0.0f && s_hdr_mask_ok && s_hdr_lights > 0 &&
                      !s_hdr_overlay && peak > paper;
    p_glActiveTexture(PSXGL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, boost ? s_hdr_lm_tex : 0);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, boost ? s_mask_tex : 0);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_hdr_tex);
    const int dyn = hdr_display && s_hdr_dyn && hdr_dyn_measure(ww, wh, boost, paper, peak);
    /* the shoulder at the display's own peak: dynamic, or asked for more than it has */
    const float dmax = s_hdr_sdr_white * s_hdr_headroom;
    const float shoulder = hdr_display && (dyn || peak > dmax * 1.001f) ? dmax : 0.0f;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    p_glUseProgram(s_hdr_prog);
    p_glActiveTexture(PSXGL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, dyn ? s_hdr_ad_tex[s_hdr_ad_cur] : 0);
    p_glActiveTexture(PSXGL_TEXTURE0);
    hdr_common_uniforms(s_hdr_uSrc, s_hdr_uMask, s_hdr_uLmask, s_hdr_uMrect, s_hdr_uP,
                        s_hdr_uGamma, s_hdr_uMr, boost, paper, peak, wh);
    p_glUniform1i(s_hdr_uState, 3);
    p_glUniform1i(s_hdr_uDyn, dyn);
    p_glUniform4f(s_hdr_uDynp, s_hdr_adapt, s_hdr_area, shoulder, 0.0f);
    p_glUniform1i(s_hdr_uDbg, s_hdr_debug);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    s_hdr_frames++;
    if (boost) s_hdr_boosted++;
    if (s_hdr_probe_req) {
        s_hdr_probe_req = 0;
        hdr_probe_take(ww, wh, paper, peak);
        memset(s_hdr_dyn_state, 0, sizeof s_hdr_dyn_state);
        if (dyn) {
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hdr_ad_fbo[s_hdr_ad_cur]);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, s_hdr_dyn_state);
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        }
    }
    p_glActiveTexture(PSXGL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glActiveTexture(PSXGL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glUseProgram(0);
    pfx_state_restore(&saved);
}

static void hdr_release(void) {
    if (s_ctx) {
        if (s_hdr_prog && p_glDeleteProgram) p_glDeleteProgram(s_hdr_prog);
        if (s_hdr_fbo) p_glDeleteFramebuffers(1, &s_hdr_fbo);
        if (s_hdr_tex) glDeleteTextures(1, &s_hdr_tex);
        if (s_hdr_lm_prog && p_glDeleteProgram) p_glDeleteProgram(s_hdr_lm_prog);
        if (s_hdr_lm_fbo) p_glDeleteFramebuffers(1, &s_hdr_lm_fbo);
        if (s_hdr_lm_tex) glDeleteTextures(1, &s_hdr_lm_tex);
        if (s_hdr_lm_vbo) p_glDeleteBuffers(1, &s_hdr_lm_vbo);
        if (s_hdr_lm_vao) p_glDeleteVertexArrays(1, &s_hdr_lm_vao);
    }
    s_hdr_prog = s_hdr_fbo = s_hdr_tex = 0;
    s_hdr_lm_prog = s_hdr_lm_fbo = s_hdr_lm_tex = s_hdr_lm_vbo = s_hdr_lm_vao = 0;
    s_hdr_lm_failed = 0; s_hdr_lm_w = s_hdr_lm_h = 0; s_hdr_lights = 0;
    if (s_ctx) {
        if (s_hdr_st_prog && p_glDeleteProgram) p_glDeleteProgram(s_hdr_st_prog);
        if (s_hdr_ad_prog && p_glDeleteProgram) p_glDeleteProgram(s_hdr_ad_prog);
        if (s_hdr_st_fbo) p_glDeleteFramebuffers(1, &s_hdr_st_fbo);
        if (s_hdr_st_tex) glDeleteTextures(1, &s_hdr_st_tex);
        for (int i = 0; i < 2; i++) {
            if (s_hdr_ad_fbo[i]) p_glDeleteFramebuffers(1, &s_hdr_ad_fbo[i]);
            if (s_hdr_ad_tex[i]) glDeleteTextures(1, &s_hdr_ad_tex[i]);
        }
    }
    s_hdr_st_prog = s_hdr_ad_prog = s_hdr_st_fbo = s_hdr_st_tex = 0;
    memset(s_hdr_ad_fbo, 0, sizeof s_hdr_ad_fbo);
    memset(s_hdr_ad_tex, 0, sizeof s_hdr_ad_tex);
    s_hdr_dyn_failed = 0; s_hdr_ad_valid = 0; s_hdr_ad_t = 0;
    s_hdr_prog_failed = 0;
    s_hdr_w = s_hdr_h = 0;
    s_hdr_mask_ok = 0;
}

/* TCP hdr: live settings (a negative value keeps the current one). */
void gl_renderer_hdr_set(float boost, float paper_nits, float peak_nits, float knee,
                         int gamma, int debug, int dynamic, float adapt, float area,
                         const char *probe_path) {
    GL_RT_SYNC("hdr");
    hdr_env_once();
    if (boost >= 0.0f) s_hdr_boost = boost > 1.0f ? 1.0f : boost;
    if (paper_nits >= 0.0f) s_hdr_paper_nits = paper_nits;
    if (peak_nits >= 0.0f) s_hdr_peak_nits = peak_nits;
    if (knee >= 0.0f && knee <= 0.95f) s_hdr_knee = knee;
    if (gamma >= 0) s_hdr_gamma = gamma ? 1 : 0;
    if (debug >= 0) s_hdr_debug = debug ? 1 : 0;
    if (dynamic >= 0) { s_hdr_dyn = dynamic ? 1 : 0; s_hdr_ad_valid = 0; }
    if (adapt >= 0.0f) s_hdr_adapt = adapt > 1.0f ? 1.0f : adapt;
    if (area > 0.0005f) s_hdr_area = area;
    if (probe_path) {
        snprintf(s_hdr_probe_path, sizeof s_hdr_probe_path, "%s", probe_path);
        s_hdr_probe.done = 0;
        s_hdr_probe_req = 1;
    }
    pfx_mask_wanted_update();
}

int gl_renderer_hdr_json(char *out, size_t cap) {
    return snprintf(out, cap,
        "\"hdr\":{\"wanted\":%d,\"float_buffer\":%d,\"on\":%d,\"display_hdr\":%d,"
        "\"sdr_white_nits\":%.1f,\"headroom\":%.3f,\"paper_nits\":%.1f,\"peak_nits\":%.1f,"
        "\"set_paper_nits\":%.1f,\"set_peak_nits\":%.1f,\"boost\":%.3f,\"knee\":%.3f,"
        "\"gamma\":\"%s\",\"debug\":%d,\"mask\":%d,\"lights\":%d,\"overlay\":%d,"
        "\"frames\":%llu,\"boosted_frames\":%llu,"
        "\"dynamic\":%d,\"adapt\":%.3f,\"area\":%.4f,\"speed\":%.3f,"
        "\"followed\":{\"light\":%.4f,\"lifted_share\":%.5f,\"mean_light\":%.4f,\"exposure\":%.3f,\"boost_scale\":%.3f},"
        "\"probe\":{\"done\":%d,\"w\":%d,\"h\":%d,\"max_nits\":%.1f,\"mean_nits\":%.2f,"
        "\"over_paper\":%.5f,\"paper_nits\":%.1f,\"peak_nits\":%.1f,\"png\":%d}}",
        s_hdr_want > 0, s_hdr_float, s_hdr_on, s_hdr_display,
        s_hdr_sdr_white * 80.0f, s_hdr_headroom, s_hdr_last_paper * 80.0f, s_hdr_last_peak * 80.0f,
        s_hdr_paper_nits, s_hdr_peak_nits, s_hdr_boost, s_hdr_knee,
        s_hdr_gamma ? "2.2" : "srgb", s_hdr_debug, s_hdr_mask_ok, s_hdr_lights, s_hdr_overlay,
        (unsigned long long)s_hdr_frames, (unsigned long long)s_hdr_boosted,
        s_hdr_dyn, s_hdr_adapt, s_hdr_area, s_hdr_speed,
        exp2f(s_hdr_dyn_state[0]), s_hdr_dyn_state[1], s_hdr_dyn_state[2],
        fminf(fmaxf(powf(0.18f / fmaxf(exp2f(s_hdr_dyn_state[0]), 1e-4f), 0.2f * s_hdr_adapt), 0.85f), 1.3f),
        fminf(1.0f, sqrtf(s_hdr_area / fmaxf(s_hdr_dyn_state[1], 1e-6f))),
        s_hdr_probe.done, s_hdr_probe.w, s_hdr_probe.h, s_hdr_probe.max_nits, s_hdr_probe.mean_nits,
        s_hdr_probe.over_paper, s_hdr_probe.paper_nits, s_hdr_probe.peak_nits, s_hdr_probe.written);
}
