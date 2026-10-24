/* host_gpu_stats.h — the GPU's load, clock and memory, for the performance
 * summary (perf_summary, the performance OSD line). Whole GPU (every process)
 * from NVIDIA's NVML (nvml.dll / libnvidia-ml.so.1); other GPUs read as
 * unknown (ok = 0). On Windows, for any GPU: this process's video memory
 * (DXGI; proc_ok) and the 3D engine's use by this process and by all of them
 * (the "GPU Engine" performance counters Task Manager shows; engine_ok).
 * The first call starts a sampler thread that refreshes them once a second;
 * a call only copies the latest sample (zeros until the first arrives). */
#ifndef HOST_GPU_STATS_H
#define HOST_GPU_STATS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      ok;               /* 1: the fields below are read */
    unsigned util_pct;         /* graphics engine busy, last sample period */
    unsigned mem_util_pct;     /* memory controller busy */
    unsigned vram_used_mb, vram_total_mb;
    unsigned clock_mhz;        /* graphics clock */
    int      pstate;           /* 0 (fastest) .. 15, -1 unknown */
    char     name[64];
    int      proc_ok;          /* 1: the two below are read */
    unsigned proc_vram_mb;     /* this process's dedicated video memory in use */
    unsigned proc_budget_mb;   /* what the OS lets it use before demoting */
    int      engine_ok;        /* 1: the two below are read */
    double   proc_3d_pct;      /* this process's share of the 3D engine's time */
    double   all_3d_pct;       /* every process's (-1 unknown) */
} PsxGpuStats;

int psx_host_gpu_stats(PsxGpuStats *out);

#ifdef __cplusplus
}
#endif

#endif
