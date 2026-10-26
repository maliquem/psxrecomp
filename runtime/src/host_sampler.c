/* host_sampler.c - always-on host CPU sampler for the emulation thread
 * (debug-tools builds; TCP host_profile, tools/host_profile.py).
 *
 * A background thread samples the emulation thread's instruction pointer
 * every millisecond into a ring, tagged with the guest frame and whether a
 * render pass was running. The ring covers the last ~65 seconds, so a probe
 * asks for a window that already happened instead of arming a profiler
 * before a workload. Addresses are stored relative to the executable's load
 * base; tools/host_profile.py maps them to functions with the image's own
 * symbol table (nm), so no debug info is needed.
 *
 * Windows: SuspendThread / GetThreadContext / ResumeThread from a sampler
 * thread. POSIX: the process CPU-time timer (SIGPROF) delivered to the
 * emulation thread, which reads its own interrupted PC from the ucontext.
 * Other hosts, and builds without debug tools, report the sampler as
 * unsupported. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1   /* REG_RIP, dladdr */
#endif
#include "host_sampler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int g_psx_render_pass_active;
extern uint64_t s_frame_count;

static HostSample s_ring[HOST_SAMPLER_CAP];
static volatile uint64_t s_seq;
static uintptr_t s_image_base;
static int s_supported;

#ifndef PSX_NO_DEBUG_TOOLS
static void record(uintptr_t pc, uintptr_t caller, uintptr_t caller2, int outside) {
    const uint64_t n = s_seq;
    HostSample *e = &s_ring[n % HOST_SAMPLER_CAP];
    e->rva = (pc >= s_image_base) ? (uint64_t)(pc - s_image_base) : 0;
    e->caller = (caller >= s_image_base) ? (uint64_t)(caller - s_image_base) : 0;
    e->caller2 = (caller2 >= s_image_base) ? (uint64_t)(caller2 - s_image_base) : 0;
    e->outside = (uint16_t)(outside != 0);
    e->frame = (uint32_t)s_frame_count;
    e->in_pass = g_psx_render_pass_active ? 1u : 0u;
    s_seq = n + 1;
}
#endif

#if defined(PSX_NO_DEBUG_TOOLS)
/* Production builds carry no sampler: it reports unsupported. */
void host_sampler_start(void) {}
void host_sampler_register_render_thread(void) {}

#elif defined(_WIN32)
#include <windows.h>

static HANDLE s_target;
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
/* kernel32 since Windows 8; the headers declare it only for that target */
WINBASEAPI VOID WINAPI GetCurrentThreadStackLimits(PULONG_PTR LowLimit, PULONG_PTR HighLimit);
#endif
static uintptr_t s_image_end;              /* the executable's last byte + 1 */
static ULONG_PTR s_stack_lo, s_stack_hi;   /* the sampled thread's stack */
/* The render thread, registered when it starts (host_sampler_register_render_thread).
 * PSX_HOST_SAMPLER_THREAD=render samples it instead of the emulation thread. */
static HANDLE s_render_target;
static ULONG_PTR s_render_stack_lo, s_render_stack_hi;

void host_sampler_register_render_thread(void) {
    HANDLE h;
    if (s_render_target) return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0))
        return;
    GetCurrentThreadStackLimits(&s_render_stack_lo, &s_render_stack_hi);
    s_render_target = h;
}

static int sample_render_thread(void) {
    static int s_want = -1;
    if (s_want < 0) {
        const char *e = getenv("PSX_HOST_SAMPLER_THREAD");
        s_want = (e && strcmp(e, "render") == 0) ? 1 : 0;
    }
    return s_want;
}

#if defined(_M_X64) || defined(__x86_64__)
/* The modules whose unwind tables (.pdata) the walk below reads itself: the
 * executable, the system DLLs and the GL drivers, listed once when sampling
 * starts (on the emulation thread). RtlLookupFunctionEntry is not usable
 * here: it takes the loader's function-table lock, and the thread it would
 * walk is suspended, possibly inside LoadLibrary holding it (the area code's
 * DLLs load after a save state loads): the sampler would wait for a lock the
 * suspended thread cannot release, and neither would run again. */
typedef struct { uintptr_t base, end; const RUNTIME_FUNCTION *fn; DWORD n; } UnwindModule;
static UnwindModule s_umod[24];
static int s_numod;

static void umod_add(HMODULE h) {
    if (!h || s_numod >= (int)(sizeof s_umod / sizeof s_umod[0])) return;
    const uintptr_t b = (uintptr_t)h;
    for (int m = 0; m < s_numod; m++) if (s_umod[m].base == b) return;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)b;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + (uintptr_t)dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!d->VirtualAddress || d->Size < sizeof(RUNTIME_FUNCTION)) return;
    s_umod[s_numod++] = (UnwindModule){b, b + nt->OptionalHeader.SizeOfImage,
                                       (const RUNTIME_FUNCTION *)(b + d->VirtualAddress),
                                       (DWORD)(d->Size / sizeof(RUNTIME_FUNCTION))};
}

static void umod_init(void) {
    static const wchar_t *const k_names[] = {
        L"ntdll.dll", L"kernelbase.dll", L"kernel32.dll", L"ucrtbase.dll", L"msvcrt.dll",
        L"win32u.dll", L"user32.dll", L"gdi32.dll", L"gdi32full.dll", L"opengl32.dll",
        L"ws2_32.dll", L"dwmapi.dll", L"nvoglv64.dll", L"atio6axx.dll", L"amdxx64.dll",
        L"ig9icd64.dll", L"ig12icd64.dll", L"igxelpicd64.dll",
    };
    umod_add(GetModuleHandleW(NULL));
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) umod_add(GetModuleHandleW(k_names[i]));
}

/* The unwind entry of pc (a binary search of its module's sorted table);
 * NULL with *base set: a leaf in a known module; *base 0: an unknown module. */
static const RUNTIME_FUNCTION *umod_lookup(uintptr_t pc, uintptr_t *base) {
    *base = 0;
    for (int m = 0; m < s_numod; m++) {
        const UnwindModule *u = &s_umod[m];
        if (pc < u->base || pc >= u->end) continue;
        const DWORD rva = (DWORD)(pc - u->base);
        DWORD lo = 0, hi = u->n;
        while (lo < hi) {
            const DWORD mid = lo + (hi - lo) / 2;
            if (u->fn[mid].EndAddress <= rva) lo = mid + 1;
            else hi = mid;
        }
        *base = u->base;
        return lo < u->n && u->fn[lo].BeginAddress <= rva ? &u->fn[lo] : NULL;
    }
    return NULL;
}

/* A sample outside the executable (waiting in ntdll, inside the driver):
 * walk the suspended thread's stack by the unwind tables to the first return
 * address back in the executable, the code that called out, and the one
 * after it (*up: who called that). Reads only the thread's own stack
 * (checked against its bounds at every frame) and the modules' tables; takes
 * no lock. A frame in a module not listed ends the walk. */
static uintptr_t unwind_to_image(const CONTEXT *start, uintptr_t *up) {
    CONTEXT c = *start;
    uintptr_t first = 0;
    *up = 0;
    for (int i = 0; i < 64; i++) {
        if (c.Rsp < s_stack_lo || c.Rsp + 8 > s_stack_hi) return first;
        uintptr_t img = 0;
        const RUNTIME_FUNCTION *f = umod_lookup((uintptr_t)c.Rip, &img);
        if (!img) return first;
        if (f) {
            PVOID handler = NULL;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, (DWORD64)img, c.Rip, (PRUNTIME_FUNCTION)f, &c, &handler, &frame,
                             NULL);
        } else {   /* a leaf function: the return address is at the stack pointer */
            c.Rip = *(const DWORD64 *)(uintptr_t)c.Rsp;
            c.Rsp += 8;
        }
        if (!c.Rip) return first;
        if (c.Rip >= s_image_base && c.Rip < s_image_end) {
            if (first) { *up = (uintptr_t)c.Rip; return first; }
            first = (uintptr_t)c.Rip;
        }
    }
    return first;
}
#endif

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static DWORD WINAPI sampler_main(LPVOID arg) {
    (void)arg;
    /* A 1 ms period needs a high-resolution timer; Sleep(1) would follow
     * whatever timer resolution the process last asked for. */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -10000;   /* 1 ms, relative */
        if (!SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE)) {
            CloseHandle(timer);
            timer = NULL;
        }
    }
    for (;;) {
        if (timer) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        HANDLE target = s_target;
        if (sample_render_thread()) {
            if (!s_render_target) continue;   /* not started yet */
            target = s_render_target;
            s_stack_lo = s_render_stack_lo;
            s_stack_hi = s_render_stack_hi;
        }
        if (SuspendThread(target) == (DWORD)-1) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(target, &ctx)) {
#if defined(_M_X64) || defined(__x86_64__)
            const uintptr_t pc = (uintptr_t)ctx.Rip;
            const int out = pc < s_image_base || pc >= s_image_end;
            uintptr_t up = 0;
            const uintptr_t caller = out ? unwind_to_image(&ctx, &up) : 0;
            record(pc, caller, up, out);
#elif defined(_M_ARM64) || defined(__aarch64__)
            record((uintptr_t)ctx.Pc, 0, 0, 0);
#endif
        }
        ResumeThread(target);
    }
    return 0;
}

void host_sampler_start(void) {
    if (s_supported) return;
    s_image_base = (uintptr_t)GetModuleHandleW(NULL);
    {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)s_image_base;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(s_image_base + (uintptr_t)dos->e_lfanew);
        s_image_end = s_image_base + nt->OptionalHeader.SizeOfImage;
    }
    GetCurrentThreadStackLimits(&s_stack_lo, &s_stack_hi);
#if defined(_M_X64) || defined(__x86_64__)
    umod_init();
#endif
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &s_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_QUERY_INFORMATION,
                         FALSE, 0))
        return;
    HANDLE t = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    if (!t) return;
    SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(t);
    s_supported = 1;
}

#elif defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#include <sys/time.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>   /* <ucontext.h> errors without _XOPEN_SOURCE */
#else
#include <ucontext.h>
#endif
#include <pthread.h>
#include <dlfcn.h>

static pthread_t s_thread;

#if defined(__APPLE__)
/* The emulation thread's stack, [lo, hi). SIGPROF can land on a thread that
 * is still being created; pthread_self() there traps (arm64e pointer
 * authentication), so the handler tells threads apart by the interrupted
 * stack pointer instead, which needs no thread state at all. */
static uintptr_t s_stack_lo, s_stack_hi;
#endif

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)uc_;
#if defined(__APPLE__)
    if (!uc || !uc->uc_mcontext) return;
#if defined(__aarch64__)
    const uintptr_t sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
#else
    const uintptr_t sp = (uintptr_t)uc->uc_mcontext->__ss.__rsp;
#endif
    if (sp < s_stack_lo || sp >= s_stack_hi) return;
#else
    if (!pthread_equal(pthread_self(), s_thread)) return;
#endif
#if defined(__linux__) && defined(__x86_64__)
    record((uintptr_t)uc->uc_mcontext.gregs[REG_RIP], 0, 0, 0);
#elif defined(__linux__) && defined(__aarch64__)
    record((uintptr_t)uc->uc_mcontext.pc, 0, 0, 0);
#elif defined(__APPLE__) && defined(__x86_64__)
    record((uintptr_t)uc->uc_mcontext->__ss.__rip, 0, 0, 0);
#elif defined(__APPLE__) && defined(__aarch64__)
    record((uintptr_t)uc->uc_mcontext->__ss.__pc, 0, 0, 0);
#else
    (void)uc;
#endif
}

void host_sampler_start(void) {
    if (s_supported) return;
    Dl_info info;
    if (dladdr((void *)&host_sampler_start, &info) && info.dli_fbase)
        s_image_base = (uintptr_t)info.dli_fbase;
    s_thread = pthread_self();
#if defined(__APPLE__)
    s_stack_hi = (uintptr_t)pthread_get_stackaddr_np(s_thread);
    s_stack_lo = s_stack_hi - (uintptr_t)pthread_get_stacksize_np(s_thread);
#endif
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) return;
    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000;
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_PROF, &it, NULL) != 0) return;
    s_supported = 1;
}
/* The emulation thread only here (the SIGPROF handler keys on it). */
void host_sampler_register_render_thread(void) {}

#else
void host_sampler_start(void) {}
void host_sampler_register_render_thread(void) {}
#endif

int host_sampler_supported(void) { return s_supported; }

uint64_t host_sampler_seq(void) { return s_seq; }

uint64_t host_sampler_image_base(void) { return (uint64_t)s_image_base; }

int host_sampler_get(uint64_t seq, HostSample *out) {
    const uint64_t now = s_seq;
    if (seq >= now || now - seq > HOST_SAMPLER_CAP) return 0;
    *out = s_ring[seq % HOST_SAMPLER_CAP];
    return 1;
}
