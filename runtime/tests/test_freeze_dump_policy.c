#include "freeze_dump_policy.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define EXPECT_EQ(actual, expected) do {                                      \
    unsigned got_ = (unsigned)(actual);                                       \
    unsigned want_ = (unsigned)(expected);                                    \
    if (got_ != want_) {                                                      \
        fprintf(stderr, "%s:%d: got %u, expected %u\n",                     \
                __FILE__, __LINE__, got_, want_);                             \
        failures++;                                                           \
    }                                                                         \
} while (0)

static int observe(FreezeDumpPolicy *policy, uint32_t kind) {
    return freeze_dump_policy_observe(policy, kind, 0);
}

static void observe_healthy(FreezeDumpPolicy *policy, uint32_t ticks) {
    for (uint32_t i = 0; i < ticks; i++) EXPECT_EQ(observe(policy, 0), 0);
}

/* One 2 s window at ~60 host frames/s with the PCs pinned at both ends. */
static uint32_t classify_pinned(uint32_t output_still_ticks) {
    FreezeWedgeWindow w = {0};
    w.frame_delta = 120;
    w.exc_reentry_delta = 120u * 1600u;
    w.logic_pinned = 1;
    w.output_still_ticks = output_still_ticks;
    return freeze_wedge_classify(&w);
}

/* Feed the tracker one heartbeat tick per output sample; the window's
 * classification after the last one (PCs pinned, frames healthy). */
static uint32_t run_ticks(FreezeOutputTracker *t, uint64_t output, uint32_t ticks) {
    uint32_t still = 0;
    for (uint32_t i = 0; i < ticks; i++) still = freeze_output_observe(t, output);
    return classify_pinned(still);
}

static void check_wedge_classification(void) {
    /* Kinds 1-3 keep their precedence and thresholds. */
    FreezeWedgeWindow w = {0};
    w.logic_pinned = 1;
    w.output_still_ticks = FREEZE_SPIN_STILL_TICKS;
    EXPECT_EQ(freeze_wedge_classify(&w), 1);
    w.frame_delta = 120;
    w.exc_reentry_delta = 120u * (FREEZE_WEDGE_EXC_REENTRY_PER_FRAME + 1u);
    EXPECT_EQ(freeze_wedge_classify(&w), 2);
    w.frame_delta = FREEZE_WEDGE_SLOW_FRAMES_MAX_DELTA - 1u;
    w.exc_reentry_delta = 0;
    EXPECT_EQ(freeze_wedge_classify(&w), 3);
    w.frame_delta = 120;
    w.mdec_active = 1;            /* FMV: parked in BIOS MDEC code */
    EXPECT_EQ(freeze_wedge_classify(&w), 0);

    /* Breath of Fire IV in play: the VSync wait is sampled at both ends of
     * the window (PCs "pinned") while the game draws and flips every frame.
     * Its output moves every tick, so this is never a spin. */
    FreezeOutputTracker playing = {0};
    uint64_t out = 1000;
    for (uint32_t tick = 0; tick < 600; tick++) {
        uint32_t still = freeze_output_observe(&playing, out += 37);
        EXPECT_EQ(classify_pinned(still), 0);
    }

    /* Its boot: a fixed ~2.6 s VSync wait (frames ~278-432 at 60 Hz) with
     * nothing drawn, no flip and no CD sector, then the logo starts. */
    FreezeOutputTracker boot = {0};
    EXPECT_EQ(run_ticks(&boot, 525, 26), 0);
    EXPECT_EQ(run_ticks(&boot, 530, 1), 0);   /* first GP0 command */
    EXPECT_EQ(boot.still_ticks, 0);

    /* A real logical hang (Tomba dwarf-village dialogue): the game spins on
     * a flag that never flips; nothing new reaches the GPU or the CD. It is
     * still reported, once the output has been still for the whole span. */
    FreezeOutputTracker hang = {0};
    EXPECT_EQ(run_ticks(&hang, 77, FREEZE_SPIN_STILL_TICKS), 0);
    EXPECT_EQ(run_ticks(&hang, 77, 1), 5);
    EXPECT_EQ(run_ticks(&hang, 77, 1000), 5);
    /* ...and a hang whose PCs are not pinned is still not a spin. */
    w.mdec_active = 0;
    w.logic_pinned = 0;
    w.output_still_ticks = hang.still_ticks;
    EXPECT_EQ(freeze_wedge_classify(&w), 0);

    /* Output that resumes clears the stillness at once. */
    EXPECT_EQ(run_ticks(&hang, 78, 1), 0);
    EXPECT_EQ(hang.still_ticks, 0);

    /* The first sample only primes: a tracker starts still at zero. */
    FreezeOutputTracker fresh = {0};
    EXPECT_EQ(freeze_output_observe(&fresh, 0), 0);
    EXPECT_EQ(freeze_output_observe(&fresh, 0), 1);
    fresh.primed = 0;   /* pause resets it */
    EXPECT_EQ(freeze_output_observe(&fresh, 0), 0);
}

int main(void) {
    FreezeDumpPolicy policy = {0};

    /* Authorization does not consume a slot until the file reaches disk. */
    EXPECT_EQ(observe(&policy, 5), 1);
    EXPECT_EQ(policy.automatic_dumps, 0);
    freeze_dump_policy_record_result(&policy, 1);
    EXPECT_EQ(policy.automatic_dumps, 1);
    EXPECT_EQ(policy.ordinary_slot_consumed, 1);
    EXPECT_EQ(observe(&policy, 5), 0);

    /* A short healthy gap does not turn one oscillating wedge into two. */
    observe_healthy(&policy, FREEZE_DUMP_REARM_HEALTHY_TICKS - 1u);
    EXPECT_EQ(observe(&policy, 5), 0);

    /* Lesser severity changes share the ordinary slot. */
    observe_healthy(&policy, FREEZE_DUMP_REARM_HEALTHY_TICKS);
    EXPECT_EQ(observe(&policy, 3), 0);
    EXPECT_EQ(policy.suppressed_events, 1);

    /* A hard freeze owns the reserved second slot during the same event. */
    EXPECT_EQ(observe(&policy, 1), 1);
    freeze_dump_policy_record_result(&policy, 1);
    EXPECT_EQ(policy.automatic_dumps, FREEZE_DUMP_AUTO_LIMIT);
    EXPECT_EQ(policy.hard_slot_consumed, 1);
    EXPECT_EQ(observe(&policy, 1), 0);

    /* Re-entry storms are known events and are counted after the limit. */
    observe_healthy(&policy, FREEZE_DUMP_REARM_HEALTHY_TICKS);
    EXPECT_EQ(observe(&policy, 2), 0);
    EXPECT_EQ(policy.suppressed_events, 2);

    /* Fatal state bypasses the automatic policy without consuming state. */
    FreezeDumpPolicy fatal = {0};
    EXPECT_EQ(freeze_dump_policy_observe(&fatal, 5, 1), 0);
    EXPECT_EQ(fatal.in_wedge, 0);
    EXPECT_EQ(fatal.automatic_dumps, 0);
    EXPECT_EQ(observe(&fatal, 5), 1);

    /* A failed write keeps its slot for a bounded retry. */
    FreezeDumpPolicy retry = {0};
    EXPECT_EQ(observe(&retry, 5), 1);
    freeze_dump_policy_record_result(&retry, 0);
    EXPECT_EQ(retry.automatic_dumps, 0);
    EXPECT_EQ(retry.failed_dumps, 1);
    EXPECT_EQ(observe(&retry, 5), 1);
    freeze_dump_policy_record_result(&retry, 1);
    EXPECT_EQ(retry.automatic_dumps, 1);
    EXPECT_EQ(retry.failed_dumps, 1);

    /* Three failed attempts consume the ordinary slot and stop retries. */
    FreezeDumpPolicy exhausted = {0};
    for (uint32_t i = 0; i < FREEZE_DUMP_MAX_FAILED_ATTEMPTS; i++) {
        EXPECT_EQ(observe(&exhausted, 5), 1);
        freeze_dump_policy_record_result(&exhausted, 0);
    }
    EXPECT_EQ(exhausted.failed_dumps, FREEZE_DUMP_MAX_FAILED_ATTEMPTS);
    EXPECT_EQ(exhausted.ordinary_slot_consumed, 1);
    EXPECT_EQ(observe(&exhausted, 5), 0);
    EXPECT_EQ(exhausted.suppressed_events, 1);

    /* An exhausted ordinary slot does not consume the hard-freeze slot. */
    EXPECT_EQ(observe(&exhausted, 1), 1);
    freeze_dump_policy_record_result(&exhausted, 1);
    EXPECT_EQ(exhausted.hard_slot_consumed, 1);
    EXPECT_EQ(exhausted.automatic_dumps, 1);

    /* Unknown future kinds do not consume or suppress an existing slot. */
    FreezeDumpPolicy unknown = {0};
    EXPECT_EQ(observe(&unknown, 7), 0);
    EXPECT_EQ(unknown.in_wedge, 0);
    EXPECT_EQ(unknown.suppressed_events, 0);

    /* A hard-first event leaves the ordinary evidence slot available. */
    FreezeDumpPolicy hard_first = {0};
    EXPECT_EQ(observe(&hard_first, 1), 1);
    freeze_dump_policy_record_result(&hard_first, 1);
    observe_healthy(&hard_first, FREEZE_DUMP_REARM_HEALTHY_TICKS);
    EXPECT_EQ(observe(&hard_first, 5), 1);
    freeze_dump_policy_record_result(&hard_first, 1);
    EXPECT_EQ(hard_first.automatic_dumps, FREEZE_DUMP_AUTO_LIMIT);

    /* Equal wall seconds still produce distinct file names. */
    char path0[128], path1[128];
    EXPECT_EQ(freeze_dump_format_path(path0, sizeof(path0),
                                      "psx-runtime", 1234, 0), 1);
    EXPECT_EQ(freeze_dump_format_path(path1, sizeof(path1),
                                      "psx-runtime", 1234, 1), 1);
    EXPECT_EQ(strcmp(path0, path1) != 0, 1);
    EXPECT_EQ(freeze_dump_format_path(path0, 4, "psx-runtime", 1234, 0), 0);

    check_wedge_classification();

    if (failures != 0) {
        fprintf(stderr, "freeze dump policy: %d failure(s)\n", failures);
        return 1;
    }
    puts("freeze dump policy: all checks passed");
    return 0;
}
