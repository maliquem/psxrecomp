/* gl_texfilter.inc - the texture filters of TEX_FS past nearest, bilinear,
 * stable and xBR (gpu_gl_renderer.c includes it; PSX_TEXFILTER_GLSL goes into
 * TEX_FS after xbr_sample, so fetch_texel, stable_texel, col5 and xbr_d are
 * in scope).
 *
 * A primitive's v_filter: bits 0..3 the sampler (PSX_TEXTURE_FILTER_*), bit 4
 * texture LOD, bits 5..6 the edge mode (PSX_TEXTURE_EDGE_*):
 *   0 PS1   the cutout is the PS1's: the nearest texel decides whether a pixel
 *           is drawn, the filter only changes colours inside it;
 *   1 Clean the filter decides the outline too (its opacity, cut at one half):
 *           a smoother outline with a hard edge ("no edge blending");
 *   2 Soft  the same outline blended over what is under it by the filtered
 *           opacity (dual-source blending, u_softok); where the pass cannot
 *           blend it is Clean.
 * Transparent texels (0) never lend a colour: a kernel's colour is
 * renormalised over its opaque taps, and its opacity is a filtered opacity.
 * The colour of a filtered pixel comes from one STP class (the PS1 texel's,
 * or with an edge mode the heavier one), so a semi-transparent texel and an
 * opaque one never mix.
 *
 * Each sampler sits behind #if (TF_MASK & TFM_*) (gpu_gl_renderer.c builds
 * the textured program with the samplers some class uses now, tex_prog_for):
 * with none, TEX_FS is the program it always was; a picked filter compiles
 * alone, its loops unrolled and its texels in registers. (All of them in one
 * program, unrolled, took ~8 s to compile on a GeForce the first time; kept
 * rolled to compile fast, the texel arrays went to local memory and DCCI cost
 * 20x nearest.)
 *
 * Every sampler here is our own code from published descriptions:
 *   Sharp bilinear  bilinear with the blend squeezed to one output pixel at
 *                   each texel boundary (the framework's presentation filter);
 *   JINC2           EWA jinc windowed by jinc, radius 2 (taps on a 4x4 grid;
 *                   the kernel as a degree-6 polynomial in r^2 fitted to
 *                   jinc(pi r) jinc(pi r 0.61), error under 4e-4), clamped to
 *                   the nearest four texels (anti-ringing);
 *   Monotonic cubic separable Hermite with Fritsch-Carlson (1980) tangents:
 *                   no overshoot;
 *   DCCI            directional cubic convolution (Zhou, Shen, Dong 2012): the
 *                   2x image's diagonal points from the 4x4 texels round them
 *                   (edge strength over 7x7, threshold 1.15, k = 5), its
 *                   horizontal/vertical points from the texel row/column and
 *                   the two diagonal points beside them (the paper's second
 *                   pass, its perpendicular cubic shortened to linear), then
 *                   bilinear between the 2x image's points; worked in a
 *                   window mirrored so the sample is always in the same
 *                   quadrant (DCCI is symmetric);
 *   Scale2x / EPX, Scale3x   the published rules (Johnston 1992, Mazzoleni);
 *   MMPX            McGuire & Gagiu 2021 (JCGT 10(1)), whose reference code is
 *                   MIT (THIRD_PARTY_ATTRIBUTION.md): the 1:1, intersection and
 *                   2:1 rules on exact texel values, transparent the brightest.
 * The pixel-art scalers (Scale2x/3x, MMPX, and xBR with an edge mode) read
 * their neighbours in stages (a flat neighbourhood stops after the first) and
 * stay nearest while a texel covers under 4/3 output pixels (a far or
 * minified texture). */
#define TFM_BILINEAR 1u    /* bilinear with an edge mode, sharp bilinear */
#define TFM_JINC2    2u
#define TFM_CUBIC    4u
#define TFM_DCCI     8u
#define TFM_XBR      16u   /* xBR with an edge mode */
#define TFM_SCALE2X  32u
#define TFM_SCALE3X  64u
#define TFM_MMPX     128u
#define PSX_TEXFILTER_GLSL \
    "#if TF_MASK != 0\n" \
    "uniform int u_softok;    /* this pass blends (dual source): Soft edges may blend */\n" \
    "int g_raw[25];           /* the loaded texels */\n" \
    "#endif\n" \
    "#if (TF_MASK & 15) != 0\n" \
    /* a w x h window at o, row-major (the kernels) */ \
    "void tf_load(ivec2 o, int w, int h){\n" \
    "  for (int k = 0; k < w * h; ++k) g_raw[k] = stable_texel(o + ivec2(k % w, k / w));\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 3) != 0\n" \
    /* ---- weighted taps (bilinear, sharp, JINC2): colour per STP class ---- */ \
    "vec4 g_ta0, g_ta1; float g_tw;\n" \
    "void tf_tap(int raw, float w){\n" \
    "  g_tw += w;\n" \
    "  if (raw == 0) return;\n" \
    "  vec4 v = vec4(col5(raw), 1.0) * w;\n" \
    "  if (((raw >> 15) & 1) != 0) g_ta1 += v; else g_ta0 += v;\n" \
    "}\n" \
    /* rgb + opacity of the taps. stp: with the PS1 outline the PS1 texel's \
     * class, else the heavier one; the colour is that class's. */ \
    "vec4 tf_resolve(int base, int edge, out int stp){\n" \
    "  float op = clamp((g_ta0.a + g_ta1.a) / max(g_tw, 1e-6), 0.0, 1.0);\n" \
    "  stp = (edge == 0 && base != 0) ? ((base >> 15) & 1) : (g_ta1.a > g_ta0.a ? 1 : 0);\n" \
    "  vec4 a = stp == 1 ? g_ta1 : g_ta0, b = stp == 1 ? g_ta0 : g_ta1;\n" \
    "  vec3 rgb = base != 0 ? col5(base) : vec3(0.0);\n" \
    "  if (a.a > 0.02 * g_tw) rgb = a.rgb / a.a;\n" \
    "  else if (b.a > 0.02 * g_tw) rgb = b.rgb / b.a;\n" \
    "  return vec4(clamp(rgb, 0.0, 1.0), op);\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 1) != 0\n" \
    /* bilinear at p (texel centres at integers); sharp: the blend squeezed \
     * to the output pixel (fw texels wide) on the boundary */ \
    "vec4 tf_bilinear(vec2 p, vec2 fw, bool sharp, int base, int edge, out int stp){\n" \
    "  ivec2 b = ivec2(floor(p)); vec2 f = p - vec2(b);\n" \
    "  if (sharp) f = clamp((f - 0.5) / max(fw, vec2(1e-4)) + 0.5, 0.0, 1.0);\n" \
    "  tf_load(b, 2, 2);\n" \
    "  g_ta0 = vec4(0.0); g_ta1 = vec4(0.0); g_tw = 0.0;\n" \
    "  tf_tap(g_raw[0], (1.0-f.x)*(1.0-f.y)); tf_tap(g_raw[1], f.x*(1.0-f.y));\n" \
    "  tf_tap(g_raw[2], (1.0-f.x)*f.y);       tf_tap(g_raw[3], f.x*f.y);\n" \
    "  return tf_resolve(base, edge, stp);\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 2) != 0\n" \
    /* the kernel: jinc(pi r) windowed by jinc with its first zero (1.2197) \
     * moved to r = 2, as a polynomial in r^2 */ \
    "float tf_jinc_w(float t){\n" \
    "  if (t >= 4.0) return 0.0;\n" \
    "  return 0.9999048 + t*(-1.689967 + t*(1.132284 + t*(-0.410272 + t*(0.08805705\n" \
    "         + t*(-0.01076524 + t*0.0005815916)))));\n" \
    "}\n" \
    "vec4 tf_jinc2(vec2 p, int base, int edge, out int stp){\n" \
    "  ivec2 b = ivec2(floor(p)); vec2 f = p - vec2(b);\n" \
    "  tf_load(b - ivec2(1), 4, 4);\n" \
    "  g_ta0 = vec4(0.0); g_ta1 = vec4(0.0); g_tw = 0.0;\n" \
    "  for (int k = 0; k < 16; ++k) {\n" \
    "    vec2 d = vec2(k % 4 - 1, k / 4 - 1) - f;\n" \
    "    tf_tap(g_raw[k], tf_jinc_w(dot(d, d)));\n" \
    "  }\n" \
    "  vec4 r = tf_resolve(base, edge, stp);\n" \
    "  vec3 lo = vec3(1.0), hi = vec3(0.0); bool got = false;\n" \
    "  for (int k = 0; k < 4; ++k) {\n" \
    "    int raw = g_raw[5 + (k >> 1) * 4 + (k & 1)];\n" \
    "    if (raw == 0 || ((raw >> 15) & 1) != stp) continue;\n" \
    "    vec3 c = col5(raw); lo = min(lo, c); hi = max(hi, c); got = true;\n" \
    "  }\n" \
    "  if (got) r.rgb = clamp(r.rgb, lo, hi);\n" \
    "  return r;\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 12) != 0\n" \
    /* ---- windowed filters (cubic, DCCI): the loaded n x n window (cell \
     * top-left at local (1, 1), the sample at f in it) as colours (transparent \
     * texels and the other STP class filled with the opaque colour round the \
     * sample), opacity and luma (0..255) ---- */ \
    "vec3 g_wc[25]; float g_wo[25]; float g_wl[25];\n" \
    "int tf_window(int n, vec2 f, int base, int edge, out int stp, out float op){\n" \
    "  vec4 c0 = vec4(0.0), c1 = vec4(0.0); float o4 = 0.0;\n" \
    "  for (int k = 0; k < 4; ++k) {\n" \
    "    int i = k & 1, j = k >> 1;\n" \
    "    int raw = g_raw[(1+j)*n + 1+i];\n" \
    "    float w = (i == 0 ? 1.0-f.x : f.x) * (j == 0 ? 1.0-f.y : f.y);\n" \
    "    if (raw == 0) continue;\n" \
    "    o4 += w;\n" \
    "    vec4 v = vec4(col5(raw) * w, w + 1e-3);\n" \
    "    if (((raw >> 15) & 1) != 0) c1 += v; else c0 += v;\n" \
    "  }\n" \
    "  op = o4;\n" \
    "  stp = (edge == 0 && base != 0) ? ((base >> 15) & 1) : (c1.a > c0.a ? 1 : 0);\n" \
    "  vec4 a = stp == 1 ? c1 : c0;\n" \
    "  vec3 fill = a.a > 0.0 ? a.rgb / a.a : (base != 0 ? col5(base) : vec3(0.0));\n" \
    "  int opaque = 0;\n" \
    "  for (int k = 0; k < n * n; ++k) {\n" \
    "    int raw = g_raw[k];\n" \
    "    bool use = raw != 0 && ((raw >> 15) & 1) == stp;\n" \
    "    g_wc[k] = use ? col5(raw) : fill;\n" \
    "    g_wo[k] = raw != 0 ? 1.0 : 0.0;\n" \
    "    g_wl[k] = dot(g_wc[k], vec3(76.245, 149.685, 29.07));\n" \
    "    if (use) opaque++;\n" \
    "  }\n" \
    "  return opaque;\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 4) != 0\n" \
    /* Monotonic cubic: Hermite with Fritsch-Carlson tangents (three-point \
     * secant mean, zero at an extremum, alpha^2 + beta^2 <= 9). */ \
    "vec3 tf_mc(vec3 y0, vec3 y1, vec3 y2, vec3 y3, float t){\n" \
    "  vec3 d0 = y1-y0, d1 = y2-y1, d2 = y3-y2;\n" \
    "  vec3 m1 = 0.5*(d0+d1) * (1.0 - step(d0*d1, vec3(0.0)));\n" \
    "  vec3 m2 = 0.5*(d1+d2) * (1.0 - step(d1*d2, vec3(0.0)));\n" \
    "  vec3 nz = step(vec3(1e-6), abs(d1));\n" \
    "  m1 *= nz; m2 *= nz;\n" \
    "  vec3 dd = d1 + (1.0 - nz);\n" \
    "  vec3 al = m1/dd, be = m2/dd, s = al*al + be*be;\n" \
    "  vec3 tau = mix(vec3(1.0), 3.0*inversesqrt(max(s, vec3(1e-12))), step(vec3(9.0), s));\n" \
    "  m1 *= tau; m2 *= tau;\n" \
    "  float t2 = t*t, t3 = t2*t;\n" \
    "  return (2.0*t3-3.0*t2+1.0)*y1 + (t3-2.0*t2+t)*m1 + (3.0*t2-2.0*t3)*y2 + (t3-t2)*m2;\n" \
    "}\n" \
    "vec4 tf_cubic(vec2 p, int base, int edge, out int stp){\n" \
    "  ivec2 b = ivec2(floor(p)); vec2 f = p - vec2(b);\n" \
    "  float op;\n" \
    "  tf_load(b - ivec2(1), 4, 4);\n" \
    "  if (tf_window(4, f, base, edge, stp, op) == 0) return vec4(col5(base), op);\n" \
    "  vec3 r[4], o[4];\n" \
    "  for (int j = 0; j < 4; ++j) {\n" \
    "    r[j] = tf_mc(g_wc[j*4], g_wc[j*4+1], g_wc[j*4+2], g_wc[j*4+3], f.x);\n" \
    "    o[j] = vec3(tf_mc(vec3(g_wo[j*4]), vec3(g_wo[j*4+1]), vec3(g_wo[j*4+2]), vec3(g_wo[j*4+3]), f.x).x);\n" \
    "  }\n" \
    "  vec3 rgb = tf_mc(r[0], r[1], r[2], r[3], f.y);\n" \
    "  if (edge != 0) op = tf_mc(o[0], o[1], o[2], o[3], f.y).x;\n" \
    "  return vec4(clamp(rgb, 0.0, 1.0), clamp(op, 0.0, 1.0));\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 8) != 0\n" \
    /* DCCI on a 5x5 window mirrored by s so that the cell's top-left texel is \
     * local (1, 1) and the sample in its bottom-right quadrant (the anchor, \
     * the cell corner nearest it, local (2, 2)). tr: transposed, so one copy \
     * serves both directions of the second pass. */ \
    "int dc_i(int x, int y, bool tr){ return tr ? x*5 + y : y*5 + x; }\n" \
    "vec3 dc_pick(vec3 pa, vec3 pb, float da, float db){\n" \
    /*  pa runs along direction a (strength da): a strong change along a is an \
     *  edge across it, so the value comes from along b, and the reverse */ \
    "  if (1.0 + da > 1.15 * (1.0 + db)) return pb;\n" \
    "  if (1.0 + db > 1.15 * (1.0 + da)) return pa;\n" \
    "  float wa = 1.0 / (1.0 + pow(da, 5.0)), wb = 1.0 / (1.0 + pow(db, 5.0));\n" \
    "  return (wa*pa + wb*pb) / (wa + wb);\n" \
    "}\n" \
    /* the 2x image's point at the centre of the cell whose top-left texel is (x, y) */ \
    "vec3 dc_diag(int x, int y){\n" \
    "  float d1 = 0.0, d2 = 0.0;   /* up-right, down-right */\n" \
    "  for (int k = 0; k < 9; ++k) {\n" \
    "    int i = k % 3, j = k / 3;\n" \
    "    d1 += abs(g_wl[(y+j-1)*5 + x+i] - g_wl[(y+j)*5 + x+i-1]);\n" \
    "    d2 += abs(g_wl[(y+j)*5 + x+i] - g_wl[(y+j-1)*5 + x+i-1]);\n" \
    "  }\n" \
    "  vec3 p1 = (9.0*(g_wc[(y+1)*5+x] + g_wc[y*5+x+1]) - g_wc[(y+2)*5+x-1] - g_wc[(y-1)*5+x+2]) * 0.0625;\n" \
    "  vec3 p2 = (9.0*(g_wc[y*5+x] + g_wc[(y+1)*5+x+1]) - g_wc[(y-1)*5+x-1] - g_wc[(y+2)*5+x+2]) * 0.0625;\n" \
    "  return dc_pick(p1, p2, d1, d2);\n" \
    "}\n" \
    /* ... between texels (x, y) and (x+1, y) (tr: (y, x) and (y, x+1)): the \
     * row's cubic, or the mean of the diagonal points across it (a, b) */ \
    "vec3 dc_side(int x, int y, bool tr, vec3 a, vec3 b){\n" \
    "  float dh = 0.0, dv = 0.0;\n" \
    "  for (int k = 0; k < 9; ++k) {\n" \
    "    int i = k % 3 - 1, j = k / 3 - 1;\n" \
    "    dh += abs(g_wl[dc_i(x+i+1, y+j, tr)] - g_wl[dc_i(x+i, y+j, tr)]);\n" \
    "  }\n" \
    "  for (int k = 0; k < 8; ++k) {\n" \
    "    int i = k % 4 - 1, j = k / 4 - 1;\n" \
    "    dv += abs(g_wl[dc_i(x+i, y+j+1, tr)] - g_wl[dc_i(x+i, y+j, tr)]);\n" \
    "  }\n" \
    "  vec3 ph = (9.0*(g_wc[dc_i(x, y, tr)] + g_wc[dc_i(x+1, y, tr)]) - g_wc[dc_i(x-1, y, tr)] - g_wc[dc_i(x+2, y, tr)]) * 0.0625;\n" \
    "  return dc_pick(ph, 0.5*(a + b), dh, dv * 1.125);\n" \
    "}\n" \
    "vec4 tf_dcci(vec2 p, int base, int edge, out int stp){\n" \
    "  ivec2 c = ivec2(floor(p)); vec2 f = p - vec2(c);\n" \
    "  ivec2 s = ivec2(f.x >= 0.5 ? 1 : -1, f.y >= 0.5 ? 1 : -1);\n" \
    "  ivec2 a = c + max(s, ivec2(0));\n" \
    "  vec2 fm = vec2(s.x > 0 ? f.x : 1.0 - f.x, s.y > 0 ? f.y : 1.0 - f.y);\n" \
    "  for (int k = 0; k < 25; ++k) g_raw[k] = stable_texel(a + s * ivec2(k % 5 - 2, k / 5 - 2));\n" \
    "  float op;\n" \
    "  if (tf_window(5, fm, base, edge, stp, op) == 0) return vec4(col5(base), op);\n" \
    "  vec3 dg = dc_diag(1, 1), dy = dc_diag(1, 2), dx = dc_diag(2, 1);\n" \
    "  vec3 hp = dc_side(1, 2, false, dg, dy), vp = dc_side(1, 2, true, dg, dx);\n" \
    "  vec2 g = 1.0 - fract(2.0 * fm);   /* from the anchor toward the cell centre */\n" \
    "  vec3 rgb = mix(mix(g_wc[12], hp, g.x), mix(vp, dg, g.x), g.y);\n" \
    "  return vec4(clamp(rgb, 0.0, 1.0), op);\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 240) != 0\n" \
    /* ---- pixel-art scalers on exact texel values (0 = transparent): their \
     * neighbourhood round the PS1 texel e, mirrored by s, read in stages \
     * (k_tf_off: 0 E, 1-4 B D F H, 5-8 A C G I, 9-12 P Q R S, 13-20 the 2:1 \
     * slopes' texels, 21-24 the four 3 away; xBR: its far four from 9) ---- */ \
    "const ivec2 k_tf_off[25] = ivec2[25](ivec2(0,0), ivec2(0,-1), ivec2(-1,0), ivec2(1,0), ivec2(0,1),\n" \
    "  ivec2(-1,-1), ivec2(1,-1), ivec2(-1,1), ivec2(1,1), ivec2(0,-2), ivec2(-2,0), ivec2(2,0), ivec2(0,2),\n" \
    "  ivec2(2,-1), ivec2(-2,-1), ivec2(2,1), ivec2(-2,1), ivec2(1,2), ivec2(1,-2), ivec2(-1,2), ivec2(-1,-2),\n" \
    "  ivec2(3,0), ivec2(-3,0), ivec2(0,3), ivec2(0,-3));\n" \
    "void tf_fetch(ivec2 e, ivec2 s, int first, int n){\n" \
    "  for (int k = first; k < first + n; ++k) g_raw[k] = stable_texel(e + k_tf_off[k] * s);\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 32) != 0\n" \
    "int tf_scale2x(ivec2 e, vec2 f){\n" \
    "  tf_fetch(e, ivec2(1), 0, 5);\n" \
    "  int E = g_raw[0], B = g_raw[1], D = g_raw[2], F = g_raw[3], H = g_raw[4];\n" \
    "  if (B == H || D == F) return E;\n" \
    "  bool r = f.x >= 0.5, d = f.y >= 0.5;\n" \
    "  if (!r && !d) return D == B ? D : E;\n" \
    "  if (r && !d) return B == F ? F : E;\n" \
    "  if (!r) return D == H ? D : E;\n" \
    "  return H == F ? F : E;\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 64) != 0\n" \
    "int tf_scale3x(ivec2 e, vec2 f){\n" \
    "  tf_fetch(e, ivec2(1), 0, 5);\n" \
    "  int E = g_raw[0], B = g_raw[1], D = g_raw[2], F = g_raw[3], H = g_raw[4];\n" \
    "  if (B == H || D == F) return E;\n" \
    "  int k = min(int(f.y * 3.0), 2) * 3 + min(int(f.x * 3.0), 2);\n" \
    "  if (k == 4) return E;\n" \
    "  if (k == 0) return D == B ? D : E;\n" \
    "  if (k == 2) return B == F ? F : E;\n" \
    "  if (k == 6) return D == H ? D : E;\n" \
    "  if (k == 8) return H == F ? F : E;\n" \
    "  tf_fetch(e, ivec2(1), 5, 4);\n" \
    "  int A = g_raw[5], C = g_raw[6], G = g_raw[7], I = g_raw[8];\n" \
    "  if (k == 1) return ((D == B && E != C) || (B == F && E != A)) ? B : E;\n" \
    "  if (k == 3) return ((D == B && E != G) || (D == H && E != A)) ? D : E;\n" \
    "  if (k == 5) return ((B == F && E != I) || (H == F && E != C)) ? F : E;\n" \
    "  return ((D == H && E != I) || (H == F && E != G)) ? H : E;\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 128) != 0\n" \
    /* MMPX: transparent texels count as the brightest */ \
    "int mm_luma(int c){ return ((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31) + 1) * (c == 0 ? 256 : 1); }\n" \
    "bool mm_any3(int b, int x, int y, int z){ return b == x || b == y || b == z; }\n" \
    "bool mm_all3(int b, int x, int y, int z){ return b == x && b == y && b == z; }\n" \
    "bool mm_all4(int b, int x, int y, int z, int w){ return b == x && b == y && b == z && b == w; }\n" \
    "bool mm_none4(int b, int x, int y, int z, int w){ return b != x && b != y && b != z && b != w; }\n" \
    "int tf_mmpx(ivec2 e, vec2 f){\n" \
    "  tf_fetch(e, ivec2(1), 0, 9);\n" \
    "  int E = g_raw[0], B = g_raw[1], D = g_raw[2], F = g_raw[3], H = g_raw[4];\n" \
    "  int A = g_raw[5], C = g_raw[6], G = g_raw[7], I = g_raw[8];\n" \
    "  if (A == E && B == E && C == E && D == E && F == E && G == E && H == E && I == E) return E;\n" \
    "  tf_fetch(e, ivec2(1), 9, 16);\n" \
    "  int P = g_raw[9], Q = g_raw[10], R = g_raw[11], S = g_raw[12];\n" \
    "  int Bl = mm_luma(B), Dl = mm_luma(D), El = mm_luma(E), Fl = mm_luma(F), Hl = mm_luma(H);\n" \
    "  int J = E, K = E, L = E, M = E;\n" \
    /*  1:1 slopes */ \
    "  if (D == B && D != H && D != F && (El >= Dl || E == A) && mm_any3(E, A, C, G) &&\n" \
    "      (El < Dl || A != D || E != P || E != Q)) J = D;\n" \
    "  if (B == F && B != D && B != H && (El >= Bl || E == C) && mm_any3(E, A, C, I) &&\n" \
    "      (El < Bl || C != B || E != P || E != R)) K = B;\n" \
    "  if (H == D && H != F && H != B && (El >= Hl || E == G) && mm_any3(E, A, G, I) &&\n" \
    "      (El < Hl || G != H || E != S || E != Q)) L = H;\n" \
    "  if (F == H && F != B && F != D && (El >= Fl || E == I) && mm_any3(E, C, G, I) &&\n" \
    "      (El < Fl || I != H || E != R || E != S)) M = F;\n" \
    /*  intersections (the four 3 away: g_raw 21-24) */ \
    "  if (E != F && mm_all4(E, C, I, D, Q) && F == B && F == H && F != g_raw[21]) { K = F; M = F; }\n" \
    "  if (E != D && mm_all4(E, A, G, F, R) && D == B && D == H && D != g_raw[22]) { J = D; L = D; }\n" \
    "  if (E != H && mm_all4(E, G, I, B, P) && H == D && H == F && H != g_raw[23]) { L = H; M = H; }\n" \
    "  if (E != B && mm_all4(E, A, C, H, S) && B == D && B == F && B != g_raw[24]) { J = B; K = B; }\n" \
    "  if (Bl < El && mm_all4(E, G, H, I, S) && mm_none4(E, A, D, C, F)) { J = B; K = B; }\n" \
    "  if (Hl < El && mm_all4(E, A, B, C, P) && mm_none4(E, D, G, I, F)) { L = H; M = H; }\n" \
    "  if (Fl < El && mm_all4(E, A, D, G, Q) && mm_none4(E, B, C, I, H)) { K = F; M = F; }\n" \
    "  if (Dl < El && mm_all4(E, C, F, I, R) && mm_none4(E, B, A, G, H)) { J = D; L = D; }\n" \
    /*  2:1 slopes (13 (2,-1), 14 (-2,-1), 15 (2,1), 16 (-2,1), 17 (1,2), \
     *  18 (1,-2), 19 (-1,2), 20 (-1,-2)) */ \
    "  if (H != B) {\n" \
    "    if (H != A && H != E && H != C) {\n" \
    "      if (mm_all3(H, G, F, R) && H != D && H != g_raw[13]) L = M;\n" \
    "      if (mm_all3(H, I, D, Q) && H != F && H != g_raw[14]) M = L;\n" \
    "    }\n" \
    "    if (B != I && B != G && B != E) {\n" \
    "      if (mm_all3(B, A, F, R) && B != D && B != g_raw[15]) J = K;\n" \
    "      if (mm_all3(B, C, D, Q) && B != F && B != g_raw[16]) K = J;\n" \
    "    }\n" \
    "  }\n" \
    "  if (F != D) {\n" \
    "    if (D != I && D != E && D != C) {\n" \
    "      if (mm_all3(D, A, H, S) && D != B && D != g_raw[17]) J = L;\n" \
    "      if (mm_all3(D, G, B, P) && D != H && D != g_raw[18]) L = J;\n" \
    "    }\n" \
    "    if (F != E && F != A && F != G) {\n" \
    "      if (mm_all3(F, C, H, S) && F != B && F != g_raw[19]) K = M;\n" \
    "      if (mm_all3(F, I, B, P) && F != H && F != g_raw[20]) M = K;\n" \
    "    }\n" \
    "  }\n" \
    "  bool r = f.x >= 0.5, d = f.y >= 0.5;\n" \
    "  return d ? (r ? M : L) : (r ? K : J);\n" \
    "}\n" \
    "#endif\n" \
    "#if (TF_MASK & 16) != 0\n" \
    /* xBR that may reshape the outline: transparent texels take part as a \
     * colour, so a corner cut can open or fill the cutout (xbr_sample keeps \
     * the PS1 one). Its neighbourhood mirrored toward the corner (s); the \
     * far four (2,0) (0,2) (2,1) (1,2) are k_tf_off 11, 12, 15, 17. */ \
    "vec4 tf_xbr(vec2 uv, vec2 fw, out int stp){\n" \
    "  ivec2 e = ivec2(floor(uv)); vec2 f = fract(uv);\n" \
    "  ivec2 s = ivec2(f.x < 0.5 ? -1 : 1, f.y < 0.5 ? -1 : 1);\n" \
    "  vec2 l = vec2(s.x > 0 ? f.x : 1.0 - f.x, s.y > 0 ? f.y : 1.0 - f.y);\n" \
    "  tf_fetch(e, s, 0, 5);\n" \
    "  int E = g_raw[0], B = g_raw[1], D = g_raw[2], F = g_raw[3], H = g_raw[4], px = E; float a = 0.0;\n" \
    "  if (E != F && E != H) {\n" \
    "    tf_fetch(e, s, 5, 13);\n" \
    "    int C = g_raw[6], G = g_raw[7], I = g_raw[8];\n" \
    "    int F4 = g_raw[11], H5 = g_raw[12], I4 = g_raw[15], I5 = g_raw[17];\n" \
    "    float we = xbr_d(E,C)+xbr_d(E,G)+xbr_d(I,H5)+xbr_d(I,F4)+4.0*xbr_d(H,F);\n" \
    "    float wi = xbr_d(H,D)+xbr_d(H,I5)+xbr_d(F,I4)+xbr_d(F,B)+4.0*xbr_d(E,I);\n" \
    "    if (we < wi) {\n" \
    "      px = xbr_d(E,F) <= xbr_d(E,H) ? F : H;\n" \
    "      a = clamp((l.x + l.y - 1.5) / max(fw.x + fw.y, 1e-3) + 0.5, 0.0, 1.0);\n" \
    "    }\n" \
    "  }\n" \
    "  int dom = a >= 0.5 ? px : E;\n" \
    "  if (dom == 0) dom = px != 0 ? px : E;\n" \
    "  stp = (dom >> 15) & 1;\n" \
    "  vec3 rgb = (E != 0 && px != 0) ? mix(col5(E), col5(px), a) : col5(dom);\n" \
    "  return vec4(rgb, mix(E != 0 ? 1.0 : 0.0, px != 0 ? 1.0 : 0.0, a));\n" \
    "}\n" \
    "#endif\n" \
    "#if TF_MASK != 0\n" \
    /* One primitive's filter past the legacy four: rgb + opacity. base is the \
     * PS1 texel (opaque when edge is 0); uv the PS1 sample point; fw its \
     * footprint in texels per output pixel. A sampler this build lacks (the \
     * program follows the picks a frame late): the PS1 texel. */ \
    "vec4 tf_sample(int mode, int edge, vec2 uv, int base, vec2 fw, out int stp){\n" \
    "  vec2 p = uv + vec2(u_shift) - 0.5;\n" \
    "  bool big = max(fw.x, fw.y) < 0.75;   /* a texel over 4/3 output pixels */\n" \
    "#if (TF_MASK & 1) != 0\n" \
    "  if (mode == 1 || mode == 4) return tf_bilinear(p, fw, mode == 4, base, edge, stp);\n" \
    "#endif\n" \
    "#if (TF_MASK & 2) != 0\n" \
    "  if (mode == 5) return tf_jinc2(p, base, edge, stp);\n" \
    "#endif\n" \
    "#if (TF_MASK & 4) != 0\n" \
    "  if (mode == 6) return tf_cubic(p, base, edge, stp);\n" \
    "#endif\n" \
    "#if (TF_MASK & 8) != 0\n" \
    "  if (mode == 7) return tf_dcci(p, base, edge, stp);\n" \
    "#endif\n" \
    "#if (TF_MASK & 16) != 0\n" \
    "  if (mode == 3 && big) return tf_xbr(uv, fw, stp);\n" \
    "#endif\n" \
    "  int raw = base;\n" \
    "  ivec2 e = ivec2(floor(uv)); vec2 f = fract(uv);\n" \
    "#if (TF_MASK & 32) != 0\n" \
    "  if (mode == 8 && big) raw = tf_scale2x(e, f);\n" \
    "#endif\n" \
    "#if (TF_MASK & 64) != 0\n" \
    "  if (mode == 9 && big) raw = tf_scale3x(e, f);\n" \
    "#endif\n" \
    "#if (TF_MASK & 128) != 0\n" \
    "  if (mode == 10 && big) raw = tf_mmpx(e, f);\n" \
    "#endif\n" \
    "  if (edge == 0 && raw == 0) raw = base;\n" \
    "  stp = (raw >> 15) & 1;\n" \
    "  return vec4(col5(raw), raw != 0 ? 1.0 : 0.0);\n" \
    "}\n" \
    "#endif\n"
