/* host_gpu_stats.c — see host_gpu_stats.h. A sampler thread, started on the
 * first read, refreshes the figures once a second (driver queries can take
 * tens of milliseconds; the caller only copies the last sample):
 *   NVML's C API (whole GPU), loaded at run time: no link dependency, its
 *     structs and calls are stable across drivers;
 *   Windows: DXGI (this process's video memory) and the "GPU Engine"
 *     performance counters through PDH (this process's 3D engine use, and
 *     every process's), both from their DLLs at run time. */
#include "host_gpu_stats.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <dxgi1_4.h>
#include <pdh.h>
#define NVML_CALL __cdecl
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#define NVML_CALL
#endif

/* ---- NVML (sampler thread only) ---- */
typedef void *NvmlDevice;
typedef struct { unsigned int gpu, memory; } NvmlUtilization;
typedef struct { unsigned long long total, free, used; } NvmlMemory;
typedef int (NVML_CALL *NvmlInitFn)(void);
typedef int (NVML_CALL *NvmlHandleFn)(unsigned int, NvmlDevice *);
typedef int (NVML_CALL *NvmlUtilFn)(NvmlDevice, NvmlUtilization *);
typedef int (NVML_CALL *NvmlMemFn)(NvmlDevice, NvmlMemory *);
typedef int (NVML_CALL *NvmlClockFn)(NvmlDevice, int, unsigned int *);
typedef int (NVML_CALL *NvmlPstateFn)(NvmlDevice, int *);
typedef int (NVML_CALL *NvmlNameFn)(NvmlDevice, char *, unsigned int);

static int          s_state;   /* 0 untried, 1 ready, -1 none */
static NvmlDevice   s_dev;
static NvmlUtilFn   s_util;
static NvmlMemFn    s_mem;
static NvmlClockFn  s_clock;
static NvmlPstateFn s_pstate;
static char         s_name[64];

static void *sym(void *lib, const char *n)
{
#if defined(_WIN32)
    return (void *)GetProcAddress((HMODULE)lib, n);
#else
    return dlsym(lib, n);
#endif
}

static void nvml_open(void)
{
    s_state = -1;
#if defined(_WIN32)
    void *lib = (void *)LoadLibraryW(L"nvml.dll");
    if (!lib) lib = (void *)LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
#else
    void *lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!lib) return;
    NvmlInitFn init = (NvmlInitFn)sym(lib, "nvmlInit_v2");
    NvmlHandleFn handle = (NvmlHandleFn)sym(lib, "nvmlDeviceGetHandleByIndex_v2");
    NvmlNameFn name = (NvmlNameFn)sym(lib, "nvmlDeviceGetName");
    s_util = (NvmlUtilFn)sym(lib, "nvmlDeviceGetUtilizationRates");
    s_mem = (NvmlMemFn)sym(lib, "nvmlDeviceGetMemoryInfo");
    s_clock = (NvmlClockFn)sym(lib, "nvmlDeviceGetClockInfo");
    s_pstate = (NvmlPstateFn)sym(lib, "nvmlDeviceGetPerformanceState");
    if (!init || !handle || !s_util || !s_mem) return;
    if (init() != 0 || handle(0, &s_dev) != 0) return;   /* the first GPU: the one a single-GPU desktop draws on */
    if (name && name(s_dev, s_name, sizeof s_name) != 0) s_name[0] = 0;
    s_state = 1;
}

static void nvml_read(PsxGpuStats *out)
{
    if (!s_state) nvml_open();
    if (s_state != 1) return;
    NvmlUtilization u;
    NvmlMemory m;
    if (s_util(s_dev, &u) != 0 || s_mem(s_dev, &m) != 0) return;
    out->util_pct = u.gpu;
    out->mem_util_pct = u.memory;
    out->vram_used_mb = (unsigned)(m.used >> 20);
    out->vram_total_mb = (unsigned)(m.total >> 20);
    unsigned int mhz = 0;
    if (s_clock && s_clock(s_dev, 0 /* graphics */, &mhz) == 0) out->clock_mhz = mhz;
    int ps = -1;
    if (s_pstate && s_pstate(s_dev, &ps) == 0) out->pstate = ps;
    memcpy(out->name, s_name, sizeof out->name);
    out->ok = 1;
}

#if defined(_WIN32)
/* ---- DXGI: this process's video memory (IDXGIAdapter3::QueryVideoMemoryInfo,
 * local segment group, summed over the hardware adapters; the interfaces'
 * IIDs here: no dxguid link) ---- */
static const IID k_iid_factory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const IID k_iid_adapter3 = { 0x645967a4, 0x1392, 0x4310, { 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd } };
typedef HRESULT (WINAPI *DxgiFactoryFn)(REFIID, void **);
enum { DXGI_ADAPTERS = 4 };
static int            s_dx_state;   /* 0 untried, 1 ready, -1 none */
static IDXGIAdapter3 *s_dx_ad[DXGI_ADAPTERS];
static int            s_dx_n;

static void dxgi_open(void)
{
    s_dx_state = -1;
    HMODULE lib = LoadLibraryW(L"dxgi.dll");
    if (!lib) return;
    DxgiFactoryFn create = (DxgiFactoryFn)(void *)GetProcAddress(lib, "CreateDXGIFactory1");
    IDXGIFactory1 *f = NULL;
    if (!create || FAILED(create(&k_iid_factory1, (void **)&f)) || !f) return;
    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; s_dx_n < DXGI_ADAPTERS && IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter3 *a3 = NULL;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(IDXGIAdapter1_QueryInterface(a, &k_iid_adapter3, (void **)&a3)) && a3)
            s_dx_ad[s_dx_n++] = a3;
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    if (s_dx_n > 0) s_dx_state = 1;
}

static void dxgi_read(PsxGpuStats *out)
{
    if (!s_dx_state) dxgi_open();
    if (s_dx_state != 1) return;
    unsigned long long used = 0, budget = 0;
    for (int i = 0; i < s_dx_n; i++) {
        DXGI_QUERY_VIDEO_MEMORY_INFO m;
        if (FAILED(IDXGIAdapter3_QueryVideoMemoryInfo(s_dx_ad[i], 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &m)))
            continue;
        used += m.CurrentUsage;
        if (m.Budget > budget) budget = m.Budget;
    }
    out->proc_vram_mb = (unsigned)(used >> 20);
    out->proc_budget_mb = (unsigned)(budget >> 20);
    out->proc_ok = 1;
}

/* ---- PDH "GPU Engine" counters: utilization per process and engine, as
 * Task Manager shows them (any GPU vendor). Rates: the first collection only
 * primes them. ---- */
typedef PDH_STATUS (WINAPI *PdhOpenFn)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS (WINAPI *PdhAddFn)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS (WINAPI *PdhCollectFn)(PDH_HQUERY);
typedef PDH_STATUS (WINAPI *PdhArrayFn)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
static int          s_pdh_state;   /* 0 untried, 1 ready, -1 none */
static PdhCollectFn s_pdh_collect;
static PdhArrayFn   s_pdh_array;
static PDH_HQUERY   s_pdh_q;
static PDH_HCOUNTER s_pdh_mine, s_pdh_all;
static int          s_pdh_primed;
static double       s_pdh_buf[32768];  /* PDH_FMT_COUNTERVALUE_ITEM_W array (+ names), aligned */

static void pdh_open(void)
{
    s_pdh_state = -1;
    {   /* PSX_GPU_ENGINE_STATS=0: skip these counters (their collection walks
         * every process's GPU engines in the kernel) */
        const char *e = getenv("PSX_GPU_ENGINE_STATS");
        if (e && e[0] == '0') return;
    }
    HMODULE lib = LoadLibraryW(L"pdh.dll");
    if (!lib) return;
    PdhOpenFn open = (PdhOpenFn)(void *)GetProcAddress(lib, "PdhOpenQueryW");
    PdhAddFn add = (PdhAddFn)(void *)GetProcAddress(lib, "PdhAddEnglishCounterW");
    s_pdh_collect = (PdhCollectFn)(void *)GetProcAddress(lib, "PdhCollectQueryData");
    s_pdh_array = (PdhArrayFn)(void *)GetProcAddress(lib, "PdhGetFormattedCounterArrayW");
    if (!open || !add || !s_pdh_collect || !s_pdh_array) return;
    if (open(NULL, 0, &s_pdh_q) != ERROR_SUCCESS) return;
    wchar_t path[128];
    _snwprintf(path, sizeof path / sizeof path[0] - 1,
               L"\\GPU Engine(pid_%lu_*)\\Utilization Percentage", (unsigned long)GetCurrentProcessId());
    path[sizeof path / sizeof path[0] - 1] = 0;
    if (add(s_pdh_q, path, 0, &s_pdh_mine) != ERROR_SUCCESS) return;
    if (add(s_pdh_q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &s_pdh_all) != ERROR_SUCCESS)
        s_pdh_all = NULL;
    s_pdh_state = 1;
}

/* Sum of the counter's instances whose name contains want (NULL: all). -1
 * when unread or none has a value yet (an instance new since the last
 * collection has no rate). */
static double pdh_sum(PDH_HCOUNTER c, const wchar_t *want)
{
    DWORD size = sizeof s_pdh_buf, count = 0;
    PPDH_FMT_COUNTERVALUE_ITEM_W it = (PPDH_FMT_COUNTERVALUE_ITEM_W)(void *)s_pdh_buf;
    if (s_pdh_array(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, it) != ERROR_SUCCESS) return -1.0;
    double sum = 0.0;
    int valid = 0;
    for (DWORD i = 0; i < count; i++)
        if (it[i].FmtValue.CStatus == ERROR_SUCCESS && (!want || wcsstr(it[i].szName, want))) {
            sum += it[i].FmtValue.doubleValue;
            valid++;
        }
    return valid ? sum : -1.0;
}

static void pdh_read(PsxGpuStats *out)
{
    if (!s_pdh_state) pdh_open();
    if (s_pdh_state != 1 || s_pdh_collect(s_pdh_q) != ERROR_SUCCESS) return;
    if (!s_pdh_primed) { s_pdh_primed = 1; return; }
    const double mine = pdh_sum(s_pdh_mine, L"engtype_3D");
    if (mine < 0.0) return;
    out->proc_3d_pct = mine > 100.0 ? 100.0 : mine;
    out->all_3d_pct = -1.0;
    if (s_pdh_all) {
        const double all = pdh_sum(s_pdh_all, NULL);
        if (all >= 0.0) out->all_3d_pct = all > 100.0 ? 100.0 : all;
    }
    out->engine_ok = 1;
}
#endif

/* ---- the sampler ---- */
static PsxGpuStats s_last;
static int         s_thread;   /* 0 not started, 1 running, -1 could not start */
#if defined(_WIN32)
static SRWLOCK     s_lock = SRWLOCK_INIT;
#define STATS_LOCK()   AcquireSRWLockExclusive(&s_lock)
#define STATS_UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define STATS_LOCK()   pthread_mutex_lock(&s_lock)
#define STATS_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

static void sample_once(void)
{
    PsxGpuStats s;
    memset(&s, 0, sizeof s);
    s.pstate = -1;
    s.all_3d_pct = -1.0;
    nvml_read(&s);
#if defined(_WIN32)
    dxgi_read(&s);
    pdh_read(&s);
#endif
    STATS_LOCK();
    s_last = s;
    STATS_UNLOCK();
}

#if defined(_WIN32)
static DWORD WINAPI sampler_main(LPVOID arg)
{
    (void)arg;
    for (;;) { sample_once(); Sleep(1000); }
    return 0;
}
#else
static void *sampler_main(void *arg)
{
    (void)arg;
    for (;;) { sample_once(); sleep(1); }
    return NULL;
}
#endif

int psx_host_gpu_stats(PsxGpuStats *out)
{
    if (!s_thread) {
#if defined(_WIN32)
        HANDLE h = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
        s_thread = h ? 1 : -1;
        if (h) {
            SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(h);
        }
#else
        pthread_t t;
        s_thread = pthread_create(&t, NULL, sampler_main, NULL) == 0 ? 1 : -1;
        if (s_thread > 0) pthread_detach(t);
#endif
    }
    STATS_LOCK();
    *out = s_last;
    STATS_UNLOCK();
    return out->ok || out->proc_ok || out->engine_ok;
}
