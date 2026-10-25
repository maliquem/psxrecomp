/* perf_meter.c — where each VBlank's time goes (perf_meter.h).
 *
 * Emulation thread only: the hooks, the VBlank boundary and every reader
 * (the debug server polls at the VBlank, titles read from their VBlank
 * plugins) run there, so the ring needs no lock. */
#include "perf_meter.h"

#include "dma.h"
#include "gpu_gl_renderer.h"
#include "host_time.h"
#include "psx_sdl.h"
#include "render_thread.h"
#include "spu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_perf_meter_on = 0;

#define PM_RING  4096u
#define PM_STACK 16

typedef struct { int sec; uint64_t t0, child; } PmOpen;

static PmOpen   s_stack[PM_STACK];
static int      s_depth = 0;
static uint64_t s_acc[PM_COUNT];        /* exclusive ticks this interval */
static uint64_t s_freq = 0, s_last = 0;
static int      s_presents = 0;
static int      s_recording = -1;       /* -1 = env not read yet */
static uint64_t s_keep_until = 0;       /* a TCP reader: on until this tick */
/* the render thread's busy time, frame_perf's GPU totals (last VBlank) */
static uint64_t s_rt_busy_last = 0, s_rt_frames_last = 0;
static uint64_t s_gf_last = 0, s_gs_last = 0, s_gp_last = 0;
static uint64_t s_xf_last = 0, s_xn_last = 0;   /* the post effects' GPU totals */

static PsxPerfFrame s_ring[PM_RING];
static uint64_t s_seq = 0;

/* the per-second window */
static PsxPerfWindow s_win;
static uint64_t s_win_t0 = 0, s_win_seq0 = 0, s_win_cpu0 = 0, s_win_rcpu0 = 0;

static uint64_t now_ticks(void) { return SDL_GetPerformanceCounter(); }
static double ticks_ms(uint64_t t) { return (double)t * 1000.0 / (double)s_freq; }

void perf_meter_push_(int section) {
    if (s_depth >= PM_STACK) { s_depth++; return; }   /* too deep: counted to the parent */
    PmOpen *o = &s_stack[s_depth++];
    o->sec = section;
    o->t0 = now_ticks();
    o->child = 0;
}

void perf_meter_pop_(int section) {
    if (s_depth <= 0) return;   /* opened before the meter came on */
    if (s_depth > PM_STACK) { s_depth--; return; }
    PmOpen *o = &s_stack[s_depth - 1];
    if (o->sec != section) return;   /* unbalanced (the meter came on inside): ignore */
    s_depth--;
    const uint64_t dur = now_ticks() - o->t0;
    s_acc[section] += dur > o->child ? dur - o->child : 0;
    if (s_depth > 0 && s_depth <= PM_STACK) s_stack[s_depth - 1].child += dur;
}

void perf_meter_note_present(void) { if (g_perf_meter_on) s_presents++; }

static void obs_gpu(int b)  { if (g_perf_meter_on) { if (b) perf_meter_push_(PM_GPU);   else perf_meter_pop_(PM_GPU); } }
static void obs_spu(int b)  { if (g_perf_meter_on) { if (b) perf_meter_push_(PM_AUDIO); else perf_meter_pop_(PM_AUDIO); } }
static void obs_wait(int b) { if (g_perf_meter_on) { if (b) perf_meter_push_(PM_WAIT);  else perf_meter_pop_(PM_WAIT); } }

int psx_perf_meter_recording(void) {
    if (s_recording < 0) {
        const char *e = getenv("PSX_PERF_CSV");
        s_recording = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s_recording;
}
void psx_perf_meter_set_recording(int on) { s_recording = on ? 1 : 0; }
int psx_perf_meter_active(void) { return g_perf_meter_on; }
void psx_perf_meter_keepalive(void) {
    if (!s_freq) s_freq = SDL_GetPerformanceFrequency();
    s_keep_until = now_ticks() + s_freq * 10u;
}

static int cmp_f(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* The window's stats from the records [seq0, s_seq). */
static void window_close(uint64_t now) {
    static float wall[PM_RING], busy[PM_RING];
    PsxPerfWindow w;
    memset(&w, 0, sizeof w);
    uint64_t n = s_seq - s_win_seq0;
    if (n > PM_RING) n = PM_RING;
    if (n == 0) { s_win.valid = 0; return; }
    double gpu_sum = 0, gpost_sum = 0, rcpu_sum = 0;
    w.wall_min = 1e9;
    for (uint64_t i = 0; i < n; i++) {
        const PsxPerfFrame *f = &s_ring[(s_seq - n + i) % PM_RING];
        wall[i] = f->wall_ms;
        busy[i] = f->wall_ms - f->sec_ms[PM_WAIT];
        w.wall_avg += f->wall_ms;
        if (f->wall_ms < w.wall_min) w.wall_min = f->wall_ms;
        if (f->wall_ms > w.wall_max) w.wall_max = f->wall_ms;
        w.busy_avg += busy[i];
        if (busy[i] > w.busy_max) w.busy_max = busy[i];
        w.game_avg += f->game_ms;
        for (int s = 0; s < PM_COUNT; s++) w.sec_avg[s] += f->sec_ms[s];
        w.render_frames += f->render_frames;
        w.gpu_frames += f->gpu_frames;
        w.presents += f->presents;
        rcpu_sum += f->render_cpu_ms;
        gpu_sum += f->gpu_ms;
        gpost_sum += f->gpu_post_ms;
        if (f->gpu_frames && f->gpu_ms / f->gpu_frames > w.gpu_max) w.gpu_max = f->gpu_ms / f->gpu_frames;
    }
    w.frames = (int)n;
    w.wall_avg /= (double)n; w.busy_avg /= (double)n; w.game_avg /= (double)n;
    for (int s = 0; s < PM_COUNT; s++) w.sec_avg[s] /= (double)n;
    w.render_cpu_avg = rcpu_sum / (double)n;
    w.gpu_avg = w.gpu_frames ? gpu_sum / w.gpu_frames : 0.0;
    w.gpu_post_avg = w.gpu_frames ? gpost_sum / w.gpu_frames : 0.0;
    qsort(wall, (size_t)n, sizeof(float), cmp_f);
    qsort(busy, (size_t)n, sizeof(float), cmp_f);
    const uint64_t k99 = (n * 99u) / 100u < n ? (n * 99u) / 100u : n - 1;
    w.wall_p99 = wall[k99];
    w.busy_p99 = busy[k99];
    {   /* the worst 1% (at least one frame) */
        const uint64_t m = n / 100u ? n / 100u : 1u;
        double a = 0, b = 0;
        for (uint64_t i = n - m; i < n; i++) { a += wall[i]; b += busy[i]; }
        w.wall_low1 = a / (double)m;
        w.busy_low1 = b / (double)m;
    }
    {   /* this thread's CPU over the window, as ms per VBlank */
        const uint64_t cpu = psx_host_thread_cpu_ns();
        const uint64_t rcpu = gl_renderer_gl_thread_cpu_ns();
        w.emu_cpu_avg = (double)(cpu - s_win_cpu0) * 1e-6 / (double)n;
        s_win_cpu0 = cpu;
        s_win_rcpu0 = rcpu;
    }
    w.valid = 1;
    s_win = w;
    s_win_t0 = now;
    s_win_seq0 = s_seq;
}

void perf_meter_vblank(int want, int fast_forward) {
    if (!s_freq) s_freq = SDL_GetPerformanceFrequency();
    const uint64_t now = now_ticks();
    const int on = want || psx_perf_meter_recording() || now < s_keep_until;
    if (g_perf_meter_on && s_last) {
        PsxPerfFrame *f = &s_ring[s_seq % PM_RING];
        memset(f, 0, sizeof *f);
        extern uint64_t s_frame_count;
        f->seq = s_seq;
        f->frame = s_frame_count;
        const uint64_t wall = now - s_last;
        uint64_t sum = 0;
        for (int s = 0; s < PM_COUNT; s++) { f->sec_ms[s] = (float)ticks_ms(s_acc[s]); sum += s_acc[s]; }
        f->wall_ms = (float)ticks_ms(wall);
        f->game_ms = (float)ticks_ms(wall > sum ? wall - sum : 0);
        f->presents = (uint8_t)(s_presents > 255 ? 255 : s_presents);
        f->fast_forward = (uint8_t)(fast_forward != 0);
        RtStats rs;
        rt_get_stats(&rs);
        const uint64_t rf = rs.frames_consumed - s_rt_frames_last;
        f->render_frames = (uint8_t)(rf > 255 ? 255 : rf);
        f->render_cpu_ms = rs.running ? (float)((double)(rs.render_busy_ns - s_rt_busy_last) * 1e-6) : 0.0f;
        s_rt_busy_last = rs.render_busy_ns;
        s_rt_frames_last = rs.frames_consumed;
        uint64_t gf, gs, gp, xf, xn;
        gl_renderer_frame_gpu_totals(&gf, &gs, &gp);
        gl_renderer_postfx_gpu_totals(&xf, &xn);
        f->gpu_frames = (uint8_t)(xf - s_xf_last > 255 ? 255 : xf - s_xf_last);
        f->gpu_ms = (float)((double)(gs - s_gs_last + gp - s_gp_last) * 1e-6);
        f->gpu_post_ms = (float)((double)(xn - s_xn_last) * 1e-6);
        s_gf_last = gf; s_gs_last = gs; s_gp_last = gp;
        s_xf_last = xf; s_xn_last = xn;
        s_seq++;
        if (now - s_win_t0 >= s_freq) window_close(now);
    }
    /* the boundary: a fresh interval */
    memset(s_acc, 0, sizeof s_acc);
    s_presents = 0;
    s_depth = 0;
    s_last = now;
    if (on && !g_perf_meter_on) {
        dma_gpu_perf_observer = obs_gpu;
        spu_perf_observer = obs_spu;
        rt_wait_observer = obs_wait;
        RtStats rs;
        rt_get_stats(&rs);
        s_rt_busy_last = rs.render_busy_ns;
        s_rt_frames_last = rs.frames_consumed;
        gl_renderer_frame_gpu_totals(&s_gf_last, &s_gs_last, &s_gp_last);
        gl_renderer_postfx_gpu_totals(&s_xf_last, &s_xn_last);
        s_win_t0 = now;
        s_win_seq0 = s_seq;
        s_win_cpu0 = psx_host_thread_cpu_ns();
        s_win_rcpu0 = gl_renderer_gl_thread_cpu_ns();
        s_win.valid = 0;
    }
    if (on != g_perf_meter_on) gl_renderer_postfx_timing_auto(on);
    g_perf_meter_on = on;
}

int psx_perf_meter_records(uint64_t since, PsxPerfFrame *out, int cap, uint64_t *next) {
    uint64_t lo = s_seq > PM_RING ? s_seq - PM_RING : 0;
    if (since < lo) since = lo;
    if (since > s_seq) since = s_seq;   /* "from now on" */
    int n = 0;
    while (since < s_seq && n < cap) out[n++] = s_ring[since++ % PM_RING];
    if (next) *next = since;
    return n;
}

void psx_perf_meter_window(PsxPerfWindow *out) { *out = s_win; }

static const char *const k_sec_name[PM_COUNT] = {"gpu_emu", "present", "audio", "wait", "debug", "vblank"};

int psx_perf_meter_window_json(char *out, int cap) {
    const PsxPerfWindow *w = &s_win;
    int n = snprintf(out, (size_t)cap,
        "\"meter\":{\"on\":%d,\"valid\":%d,\"frames\":%d,\"wall_avg\":%.3f,\"wall_min\":%.3f,"
        "\"wall_max\":%.3f,\"wall_p99\":%.3f,\"wall_low1\":%.3f,\"busy_avg\":%.3f,\"busy_max\":%.3f,"
        "\"busy_p99\":%.3f,\"busy_low1\":%.3f,\"game\":%.3f",
        g_perf_meter_on, w->valid, w->frames, w->wall_avg, w->wall_min, w->wall_max, w->wall_p99,
        w->wall_low1, w->busy_avg, w->busy_max, w->busy_p99, w->busy_low1, w->game_avg);
    for (int s = 0; s < PM_COUNT && n < cap; s++)
        n += snprintf(out + n, (size_t)(cap - n), ",\"%s\":%.3f", k_sec_name[s], w->sec_avg[s]);
    if (n < cap)
        n += snprintf(out + n, (size_t)(cap - n),
            ",\"emu_cpu\":%.3f,\"render_cpu\":%.3f,\"render_frames\":%d,\"gpu_ms\":%.3f,\"gpu_post_ms\":%.3f,"
            "\"gpu_max\":%.3f,\"gpu_frames\":%d,\"presents\":%d,\"recording\":%d}",
            w->emu_cpu_avg, w->render_cpu_avg, w->render_frames, w->gpu_avg, w->gpu_post_avg, w->gpu_max,
            w->gpu_frames, w->presents, psx_perf_meter_recording());
    return n;
}

int psx_perf_meter_frames_json(uint64_t since, char *out, int cap) {
    static PsxPerfFrame tmp[512];
    uint64_t next = since;
    const int k = psx_perf_meter_records(since, tmp, 512, &next);
    int n = snprintf(out, (size_t)cap, "\"next\":%llu,\"on\":%d,\"frames\":[",
                     (unsigned long long)next, g_perf_meter_on);
    for (int i = 0; i < k && n < cap - 256; i++) {
        const PsxPerfFrame *f = &tmp[i];
        n += snprintf(out + n, (size_t)(cap - n),
            "%s[%llu,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d]",
            i ? "," : "", (unsigned long long)f->seq, (unsigned long long)f->frame,
            f->wall_ms, f->game_ms, f->sec_ms[PM_GPU], f->sec_ms[PM_PRESENT], f->sec_ms[PM_AUDIO],
            f->sec_ms[PM_WAIT], f->sec_ms[PM_DEBUG], f->sec_ms[PM_VBLANK], f->render_cpu_ms, f->gpu_ms,
            f->gpu_post_ms, f->render_frames, f->gpu_frames, f->presents, f->fast_forward);
    }
    if (n < cap) n += snprintf(out + n, (size_t)(cap - n), "]");
    return n;
}
