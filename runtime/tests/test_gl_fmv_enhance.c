/* [video] fmv_enhance (gl_fmv_enhance.inc) on a real, hidden OpenGL context.
 *
 * Synthetic 24-bit frames presented through gl_renderer_present, the
 * CPU/24-bit present path:
 *  - on by default; a frame that is not a 24-bit scanout is byte-identical to
 *    the stage off, and only 24-bit presents are counted;
 *  - blocking: flat 8x8 blocks a few levels apart on a grid placed at (3, 5):
 *    the grid's phase is found, the steps at the block edges shrink, and a
 *    real luma edge next to them keeps its contrast and stays narrow;
 *  - chroma: constant luma, chroma constant per 2x2 block and flipping hue
 *    block to block: the chroma steps shrink, luma is kept;
 *  - a flat colour inside letterbox bars keeps its value;
 *  - off again is byte-identical to off before.
 * Original source-owned scenes; no retail payload. */
#define GL_PRESENT_TEST_HOOK(gen) cap_hook(gen)
static void cap_hook(int gen);
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
void gpu_hd_textures_begin_upload(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_track_upload(int x,int y,int w,int h,const uint16_t* p){(void)x;(void)y;(void)w;(void)h;(void)p;}
void gpu_hd_textures_invalidate(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_begin_copy(int sx,int sy,int x,int y,int w,int h){(void)sx;(void)sy;(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_end_copy(void){}
#ifndef PSX_NO_DEBUG_TOOLS
int present_image_ring_accepting(void){return 0;}
int present_image_ring_thumb_w(int w,int h){(void)w;(void)h;return 0;}
void present_image_ring_push(uint32_t f,const uint8_t* p,int w,int pitch,int flip){(void)f;(void)p;(void)w;(void)pitch;(void)flip;}
void present_image_ring_push_argb(uint32_t f,const uint32_t* p,int w,int h,int pitch){(void)f;(void)p;(void)w;(void)h;(void)pitch;}
#endif
void present_shot_done(int ok) { (void)ok; }
int host_osd_needs_present(void) { return 0; }
int psx_present_vsync_owns_cadence(void) { return 0; }
void latency_ring_mark(LatencyStage stage) { (void)stage; }
void psx_host_sleep_ms(unsigned ms) { SDL_Delay(ms); }
void psx_host_sleep_micros(unsigned us) { SDL_Delay((us + 999u) / 1000u); }
int present_shot_take(char* out,int n) { (void)out; (void)n; return 0; }
int host_osd_image(const uint32_t** p,int* w,int* h) { (void)p; (void)w; (void)h; return 0; }
int host_osd_volume_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
void host_osd_present_done(void) {}
int psx_rewind_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
float psx_rewind_slide(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
/* the renderer's links to the decoder, the shadow map and telemetry */
static uint32_t s_q16 = 6u * 16u;
uint32_t mdec_mean_qscale_x16(void) { return s_q16; }
int mdec_recently_active(uint32_t within_frames) { (void)within_frames; return 0; }
int gpu_shadow_off_active(void) { return 0; }
int gpu_shadow_sun_area(float *az, float *el) { (void)az; (void)el; return 0; }
int psx_overdraw_x(void) { return 0; }
int psx_overdraw_y(void) { return 0; }
void psx_video_telemetry_compose(int renderer, int scale, int source_w, int source_h,
                                 int drawable_w, int drawable_h, int viewport_w, int viewport_h) {
    (void)renderer; (void)scale; (void)source_w; (void)source_h;
    (void)drawable_w; (void)drawable_h; (void)viewport_w; (void)viewport_h;
}
void psx_video_telemetry_present(int result) { (void)result; }

static uint8_t *s_cap; static int s_cap_w, s_cap_h, s_cap_n;
static void cap_hook(int gen) {
    (void)gen;
    SDL_GL_GetDrawableSize(s_win, &s_cap_w, &s_cap_h);
    free(s_cap);
    s_cap = (uint8_t *)malloc((size_t)s_cap_w * s_cap_h * 3);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, s_cap_w, s_cap_h, GL_RGB, GL_UNSIGNED_BYTE, s_cap);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    s_cap_n++;
}

#define SW 320
#define SH 240
#define GX 3          /* the blocking scene's grid phase */
#define GY 5
#define EDGE_X 272    /* its real luma edge */
static uint32_t src[SW * SH];
static uint32_t rgb(double y, double cb, double cr) {
    double r = y + 1.402 * cr, g = y - 0.344136 * cb - 0.714136 * cr, b = y + 1.772 * cb;
    int R = (int)(r < 0 ? 0 : r > 1 ? 255 : r * 255 + 0.5);
    int G = (int)(g < 0 ? 0 : g > 1 ? 255 : g * 255 + 0.5);
    int B = (int)(b < 0 ? 0 : b > 1 ? 255 : b * 255 + 0.5);
    return 0xff000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;   /* BGRA bytes */
}
/* Flat 8x8 blocks 0..4 steps of 4 levels apart left of x = 224; right of it
 * a clean dark/bright edge at EDGE_X. */
static void frame_blocks(void) {
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            const int bx = (x - GX + 8) / 8, by = (y - GY + 8) / 8;
            double l = 0.35 + (4.0 / 255.0) * ((bx * 3 + by * 2) % 5);
            if (x >= 224) l = x < EDGE_X ? 0.15 : 0.85;
            src[y * SW + x] = rgb(l, 0.0, 0.0);
        }
}
/* The 2x2 chroma blocks sit on the 8x8 grid, as MDEC lays them out. */
static void frame_chroma(void) {
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            int blk = (((x - GX + 8) >> 1) + ((y - GY + 8) >> 1)) & 1;
            src[y * SW + x] = rgb(0.5, blk ? 0.15 : -0.15, blk ? -0.15 : 0.15);
        }
}
static uint8_t *present(int d24) {
    const int n0 = s_cap_n;
    gl_renderer_note_present_depth24(d24);
    gl_renderer_present(src, SW, SH, 0, 1, 0);
    check(s_cap_n == n0 + 1 && s_cap, "present captured");
    uint8_t *c = (uint8_t *)malloc((size_t)s_cap_w * s_cap_h * 3);
    memcpy(c, s_cap, (size_t)s_cap_w * s_cap_h * 3);
    return c;
}
static double luma(const uint8_t *p) { return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; }
static double cb(const uint8_t *p) { return -0.168736 * p[0] - 0.331264 * p[1] + 0.5 * p[2]; }

/* The 4:3 picture's rect in the capture (rows bottom-up) and a source point's
 * output pixel. */
static int s_px0, s_py0, s_pw, s_ph;
static void picture_rect(const uint8_t *c) {
    s_px0 = 0; s_pw = s_cap_w; s_py0 = 0; s_ph = s_cap_h;
    const int y = s_cap_h / 2;
    while (s_px0 < s_cap_w / 2 && luma(c + ((size_t)y * s_cap_w + s_px0) * 3) < 1.0) s_px0++;
    int x1 = s_cap_w - 1;
    while (x1 > s_cap_w / 2 && luma(c + ((size_t)y * s_cap_w + x1) * 3) < 1.0) x1--;
    s_pw = x1 + 1 - s_px0;
}
static const uint8_t *at(const uint8_t *c, double sx, double sy) {
    int x = s_px0 + (int)(sx * s_pw / SW), y = s_cap_h - 1 - (s_py0 + (int)(sy * s_ph / SH));
    if (x < 0) x = 0; if (x >= s_cap_w) x = s_cap_w - 1;
    if (y < 0) y = 0; if (y >= s_cap_h) y = s_cap_h - 1;
    return c + ((size_t)y * s_cap_w + x) * 3;
}
/* Mean |luma step| across the block edges (vertical and horizontal) of the
 * blocking area, sampled half a source pixel either side. */
static double block_steps(const uint8_t *c) {
    double s = 0; int n = 0;
    for (int e = GX + 8; e < 216; e += 8)
        for (int y = 20; y < SH - 20; y += 3) { s += fabs(luma(at(c, e + 0.5, y + 0.5)) - luma(at(c, e - 0.5, y + 0.5))); n++; }
    for (int e = GY + 8; e < SH - 16; e += 8)
        for (int x = 20; x < 200; x += 3) { s += fabs(luma(at(c, x + 0.5, e + 0.5)) - luma(at(c, x + 0.5, e - 0.5))); n++; }
    return s / n;
}
/* The real edge: contrast 3 source pixels either side, and its width in
 * output pixels (luma between the two levels' 20%..80%). */
static void edge(const uint8_t *c, double *contrast, int *width) {
    const double sy = SH / 2 + 0.5;
    *contrast = luma(at(c, EDGE_X + 3.5, sy)) - luma(at(c, EDGE_X - 2.5, sy));
    const double lo = luma(at(c, EDGE_X - 6.5, sy)), hi = luma(at(c, EDGE_X + 6.5, sy));
    const int y = s_cap_h - 1 - (s_py0 + (int)(sy * s_ph / SH));
    int w = 0;
    for (int x = s_px0 + (EDGE_X - 6) * s_pw / SW; x < s_px0 + (EDGE_X + 6) * s_pw / SW; x++) {
        const double l = luma(c + ((size_t)y * s_cap_w + x) * 3);
        w += l > lo + 0.2 * (hi - lo) && l < lo + 0.8 * (hi - lo);
    }
    *width = w;
}
/* Mean |Cb step| between horizontal output neighbours and the max luma error
 * against the flat luma, over the picture's middle. */
static void chroma_stats(const uint8_t *c, double *step, double *lerr) {
    double s = 0; int n = 0; *lerr = 0;
    for (int y = s_cap_h / 3; y < 2 * s_cap_h / 3; y += 3)
        for (int x = s_px0 + s_pw / 8; x < s_px0 + 7 * s_pw / 8; x++) {
            const uint8_t *p = c + ((size_t)y * s_cap_w + x) * 3;
            s += fabs(cb(p + 3) - cb(p)); n++;
            const double e = fabs(luma(p) - 127.5);
            if (e > *lerr) *lerr = e;
        }
    *step = s / n;
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video\n"); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("FMV enhance hidden test", 0, 0, 1280, 960,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    glb_init(vram); glb_set_scale(1); gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    check(gl_renderer_fmv_enhance() == 1, "on by default");

    /* blocking */
    frame_blocks();
    gl_renderer_set_fmv_enhance(0);
    uint8_t *off = present(1);
    const size_t bytes = (size_t)s_cap_w * s_cap_h * 3;
    picture_rect(off);
    gl_renderer_set_fmv_enhance(1);
    uint8_t *not24 = present(0);
    check(memcmp(off, not24, bytes) == 0, "a 15-bit present is untouched");
    check(gl_renderer_fmv_enhance_frames() == 0, "only 24-bit presents are enhanced");
    uint8_t *on = present(1);
    check(gl_renderer_fmv_enhance_frames() == 1, "24-bit present enhanced");
    int phx = -1, phy = -1;
    gl_renderer_fmv_enhance_phase(&phx, &phy);
    printf("drawable=%dx%d picture x0=%d w=%d phase=(%d,%d)\n", s_cap_w, s_cap_h, s_px0, s_pw, phx, phy);
    check(phx == GX && phy == GY, "block grid phase found");
    const double b_off = block_steps(off), b_on = block_steps(on);
    double c_off, c_on; int w_off, w_on;
    edge(off, &c_off, &w_off);
    edge(on, &c_on, &w_on);
    printf("block steps off=%.2f on=%.2f  edge contrast off=%.1f on=%.1f width off=%d on=%d\n",
           b_off, b_on, c_off, c_on, w_off, w_on);
    check(b_on < b_off * 0.5, "block edge steps shrink");
    check(c_on > c_off * 0.9, "real edge keeps its contrast");
    check(w_on <= w_off + 4, "real edge stays narrow");
    /* a coarser movie (higher quantiser) is smoothed at least as much */
    s_q16 = 12u * 16u;
    uint8_t *coarse = present(1);
    printf("block steps at q=12: %.2f\n", block_steps(coarse));
    check(block_steps(coarse) <= b_on + 0.01, "a coarser quantiser smooths no less");
    s_q16 = 6u * 16u;

    /* chroma */
    frame_chroma();
    gl_renderer_set_fmv_enhance(0);
    uint8_t *c0 = present(1);
    gl_renderer_set_fmv_enhance(1);
    uint8_t *c1 = present(1);
    double s0, s1, l0, l1;
    chroma_stats(c0, &s0, &l0);
    chroma_stats(c1, &s1, &l1);
    printf("cb step off=%.2f on=%.2f luma err off=%.1f on=%.1f\n", s0, s1, l0, l1);
    check(s1 < s0 / 3.0, "chroma block steps shrink");
    check(l1 < 6.0, "luma kept");

    /* a flat colour in letterbox bars keeps its value away from the bars */
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++)
            src[y * SW + x] = (y < 24 || y >= SH - 24) ? rgb(0.0, 0.0, 0.0) : rgb(0.45, 0.12, -0.10);
    uint8_t *flat_on = present(1);
    gl_renderer_set_fmv_enhance(0);
    uint8_t *flat_off = present(1);
    int worst = 0;
    for (int y = 3 * s_cap_h / 8; y < 5 * s_cap_h / 8; y++)
        for (int x = (s_px0 + 16) * 3; x < (s_px0 + s_pw - 16) * 3; x++) {
            const size_t i = (size_t)y * s_cap_w * 3 + x;
            const int d = abs((int)flat_on[i] - (int)flat_off[i]);
            if (d > worst) worst = d;
        }
    printf("flat colour worst channel delta=%d\n", worst);
    check(worst <= 2, "a flat colour is kept");

    frame_blocks();
    uint8_t *off2 = present(1);
    check(memcmp(off, off2, bytes) == 0, "off again is byte-identical");
    free(off); free(not24); free(on); free(coarse); free(c0); free(c1);
    free(flat_on); free(flat_off); free(off2);
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
