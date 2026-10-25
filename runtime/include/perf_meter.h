/*
 * perf_meter.h — where each VBlank's time goes, on the emulation thread.
 *
 * Off by default; on while the FPS readout (F) shows, while a title records
 * (psx_perf_meter_set_recording, the Shift+F hotkey, env PSX_PERF_CSV=1) or
 * once TCP perf_frames / perf_summary asked. Off, every hook is one branch
 * on a cached int.
 *
 * Each guest VBlank becomes one record: the wall time since the previous
 * VBlank split into the time spent in each section (exclusive: a section
 * inside another counts only for itself), the rest being the recompiled
 * game's own code (with what it calls inline: memory, cycles, PGXP, GTE).
 * Sections:
 *   GPU     — the emulated GPU: DMA channel 2 (the ordering-table walk the
 *             CPU waits on, block uploads) and what it drives (the renderer's
 *             draws, or their recording for the render thread)
 *   PRESENT — handing a finished frame to the display (post effects and the
 *             swap without a render thread, the frame's records with one)
 *   AUDIO   — the SPU catching up and the host audio pump
 *   WAIT    — the frame pacer and waits on the render thread's queue
 *   DEBUG   — the debug server's per-VBlank work (debug-tools builds)
 *   VBLANK  — the rest of the VBlank epilogue (input, OSD, bookkeeping)
 * Each record also carries the render thread's busy time over the interval
 * and the GPU time of the frames read back in it (frame_perf's timer
 * queries, debug-tools builds, a few frames late: the post effects and the
 * present exactly; the scene's draws as the span between two presents, an
 * upper bound that includes the GPU's idle gaps).
 */
#ifndef PSX_PERF_METER_H
#define PSX_PERF_METER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PM_GPU = 0,
    PM_PRESENT,
    PM_AUDIO,
    PM_WAIT,
    PM_DEBUG,
    PM_VBLANK,
    PM_COUNT
};

typedef struct PsxPerfFrame {
    uint64_t seq;            /* record number (monotonic) */
    uint64_t frame;          /* guest VBlank count at its end */
    float    wall_ms;        /* since the previous VBlank */
    float    game_ms;        /* the recompiled code (wall less every section) */
    float    sec_ms[PM_COUNT];
    float    render_cpu_ms;  /* the render thread's busy time over the interval */
    float    gpu_ms;         /* GPU time of the frames read back this interval:
                              * scene span (an upper bound) + present */
    float    gpu_post_ms;    /* of it, the present: post effects, blit (exact) */
    uint8_t  render_frames;  /* render-thread frames finished this interval */
    uint8_t  gpu_frames;     /* frames whose GPU time was read back (debug builds) */
    uint8_t  presents;       /* frames handed to the display this VBlank */
    uint8_t  fast_forward;   /* the manual turbo was on */
} PsxPerfFrame;

/* Per-second summary over the last full window (perf_summary, the OSD). */
typedef struct PsxPerfWindow {
    int    valid, frames;
    double wall_avg, wall_min, wall_max, wall_p99, wall_low1;   /* low1: mean of the worst 1% */
    double busy_avg, busy_max, busy_p99, busy_low1;             /* wall less WAIT */
    double game_avg, sec_avg[PM_COUNT];
    double emu_cpu_avg, render_cpu_avg;                         /* per VBlank */
    double gpu_avg, gpu_post_avg, gpu_max;                      /* per frame read back */
    int    render_frames, gpu_frames, presents;
} PsxPerfWindow;

extern int g_perf_meter_on;   /* read by the hooks; changes only at a VBlank */

void perf_meter_push_(int section);
void perf_meter_pop_(int section);
#define PERF_BEGIN(s) do { if (g_perf_meter_on) perf_meter_push_(s); } while (0)
#define PERF_END(s)   do { if (g_perf_meter_on) perf_meter_pop_(s); } while (0)

/* The VBlank boundary (start of the epilogue): closes one record, applies
 * on/off. want: the FPS readout or a TCP reader wants it this VBlank. */
void perf_meter_vblank(int want, int fast_forward);
void perf_meter_note_present(void);

/* Records since `since` (oldest first, at most cap); *next = the seq to ask
 * for next time. Returns the count. The ring keeps the last 4096. */
int  psx_perf_meter_records(uint64_t since, PsxPerfFrame *out, int cap, uint64_t *next);
void psx_perf_meter_window(PsxPerfWindow *out);
int  psx_perf_meter_active(void);
/* Recording (a title writes the records, e.g. as CSV): asks the meter on. */
int  psx_perf_meter_recording(void);
void psx_perf_meter_set_recording(int on);
/* A reader (TCP) keeps the meter on for the next ~10 s. */
void psx_perf_meter_keepalive(void);
int  psx_perf_meter_frames_json(uint64_t since, char *out, int cap);
int  psx_perf_meter_window_json(char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_PERF_METER_H */
