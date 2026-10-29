#ifndef PSXRECOMP_FREEZE_DUMP_POLICY_H
#define PSXRECOMP_FREEZE_DUMP_POLICY_H

#include <stddef.h>
#include <stdint.h>

/* Full freeze dumps contain several large trace rings. Keep automatic
 * watchdog output bounded while leaving deliberate fatal dumps independent. */
#define FREEZE_DUMP_ORDINARY_SLOTS 1u
#define FREEZE_DUMP_HARD_SLOTS 1u
#define FREEZE_DUMP_AUTO_LIMIT \
    (FREEZE_DUMP_ORDINARY_SLOTS + FREEZE_DUMP_HARD_SLOTS)
#define FREEZE_DUMP_MAX_FAILED_ATTEMPTS 3u
/* Policy hysteresis, independent of the detector's sample window. The total
 * time before rearm can therefore be longer than this tick count alone. */
#define FREEZE_DUMP_REARM_HEALTHY_TICKS 20u

typedef struct FreezeDumpPolicy {
    uint32_t automatic_dumps;
    uint32_t failed_dumps;
    uint32_t suppressed_events;
    uint32_t healthy_ticks;
    uint32_t wedge_kind;
    uint32_t pending_kind;
    uint32_t ordinary_failed_attempts;
    uint32_t hard_failed_attempts;
    int ordinary_slot_consumed;
    int hard_slot_consumed;
    int in_wedge;
} FreezeDumpPolicy;

/* Observe one watchdog sample. Returns nonzero when the caller must write an
 * automatic full dump. A new event requires a sustained healthy interval.
 * Fatal state bypasses this automatic policy and does not change its budget. */
int freeze_dump_policy_observe(FreezeDumpPolicy *policy, uint32_t wedge_kind,
                               int fatal_active);

/* Record whether the authorized full dump reached disk. A slot permits a
 * bounded number of failed attempts before the policy consumes that slot. */
void freeze_dump_policy_record_result(FreezeDumpPolicy *policy, int written);

/* Wedge classification of one detector window (kinds: 0 healthy, 1 hard
 * freeze, 2 exception re-entry storm, 3 slow frames, 5 logical spin). */
#define FREEZE_WEDGE_EXC_REENTRY_PER_FRAME 20000u /* ~10x chronic 2K/frame */
#define FREEZE_WEDGE_SLOW_FRAMES_MAX_DELTA 10u    /* <5 fps over the 2s window */
/* A logical spin also needs the guest's own output to have been still this
 * long: no display flip, no GP0 command, no CD sector. A guest that keeps
 * presenting is alive however its sampled PCs look, and a guest that only
 * waits a fixed count of VSyncs (Breath of Fire IV waits ~2.6 s at boot with
 * nothing drawn) finishes well inside it. A real hang never does. */
#define FREEZE_SPIN_STILL_TICKS 100u              /* 10 s at the 100 ms tick */

typedef struct FreezeWedgeWindow {
    uint64_t frame_delta;        /* host frames over the window */
    uint64_t exc_reentry_delta;  /* exception re-entry blocks over the window */
    int logic_pinned;            /* cur_fn, store_pc, dirty insns unchanged */
    int mdec_active;             /* an MDEC decode inside the window (FMV) */
    uint32_t output_still_ticks; /* freeze_output_observe() */
} FreezeWedgeWindow;

uint32_t freeze_wedge_classify(const FreezeWedgeWindow *w);

/* Ticks the guest's output has been still. `output` is any count that grows
 * when the guest produces something (flips + GP0 commands + CD sectors); it
 * is only compared for change. The first sample starts the count at zero. */
typedef struct FreezeOutputTracker {
    uint64_t last_output;
    uint32_t still_ticks;
    int primed;
} FreezeOutputTracker;

uint32_t freeze_output_observe(FreezeOutputTracker *t, uint64_t output);

/* Format a unique dump path. The sequence prevents same-second truncation. */
int freeze_dump_format_path(char *out, size_t cap, const char *backend,
                            long long wall, uint32_t sequence);

#endif /* PSXRECOMP_FREEZE_DUMP_POLICY_H */
