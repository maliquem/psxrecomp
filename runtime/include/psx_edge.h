/* psx_edge.h — the block-edge interrupt check, inline in compiled code.
 *
 * The emitters write psx_check_interrupts_at(cpu, resume_pc) at every basic
 * block edge (after psx_cyc_bb_defer_flush). In the common case nothing can
 * be delivered (no unmasked IRQ, no software interrupt, outside the
 * exception handler) and interrupts.c's psx_check_interrupts did only
 * bookkeeping, behind three or four calls: the idle-skip detector's note of
 * the edge, and every 16K such edges the host polls. This header does that
 * common case here, writing the same state, and hands every other case to
 * the out-of-line function, which runs from the start exactly as before:
 *   - something pending or deliverable, the exception handler, a pending
 *     cooperative thread switch, frozen guest time, netplay, the self-check,
 *     a lockstep suppression count, the 16K-th edge (its polls);
 *   - the idle detector seeing anything but a new loop candidate: the same
 *     PC as its anchor (a loop: it compares and may skip), an audited
 *     store-counter anchor, or any of its inactive states.
 * For a new candidate PC the detector only restarts: anchor = this PC,
 * progress reset, no snapshot, and the counters it compares against. That is
 * what the inline writes (psx_cycles.c psx_idle_note_check's last branch).
 * (Edge statistics: the inline does not count s_irq_path_entry/fast_none.)
 *
 * Not in overlay DLLs (they call the host through their callback table) nor
 * in interrupts.c/psx_cycles.c themselves. The DuckStation recompiler folds
 * the same question into its event downcount: one compare per block. */
#ifndef PSX_EDGE_H
#define PSX_EDGE_H
#if !defined(PSX_OVERLAY_DLL_BUILD)

#include <stdint.h>
#include "cpu_state.h"
#include "psx_cycles.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The idle-skip detector's restart state (psx_cycles.c). */
typedef struct PsxIdleEdge {
    uint32_t pc;               /* anchor: the PC being watched for a loop */
    uint32_t quantum;
    uint32_t streak;
    int      have_snap;
    int      progress_reg;
    int32_t  progress_delta;
    int      mem_progress;
    uint32_t edges;
    uint32_t store_addr;
    uint64_t last_cycle;
    uint64_t last_stores;
    uint64_t last_mmio;
    int      ext_wired;        /* host writer and store tracker installed */
    int      site_count;       /* audited store-counter anchors */
    uint32_t sites[16];
} PsxIdleEdge;
extern PsxIdleEdge g_psx_idle_edge;

/* interrupts.c */
extern uint32_t i_stat, i_mask;
extern int      g_psx_in_exception;
extern int      g_psx_defer_switch_pending;
extern uint32_t g_psx_irq_fast_maintenance;
extern uint32_t g_psx_last_interrupt_check_pc;
extern uint64_t g_psx_last_interrupt_check_cycle;
/* elsewhere */
extern int      g_psx_guest_time_frozen;   /* psx_cycles.c */
extern int      g_idle_skip_enabled, g_idle_skip_ext, g_idle_note_suppress;
extern int      g_ls_suppress_record, g_ls_replay_active, g_ls_mode, g_precise_mode;
extern int      g_psx_call_bail;
extern uint32_t g_dirty_safe_resume_pc;
extern int      g_psx_netplay_seen;        /* psx_netplay.c: set once it starts */
extern int      g_psx_selfcheck_phase;     /* psx_selfcheck.c: 0 = off */
extern uint64_t g_guest_store_count, g_mmio_access_count;

void psx_check_interrupts_at(struct CPUState* cpu, uint32_t resume_pc);

static inline int psx_edge_idle_site(uint32_t pc) {
    for (int i = 0; i < g_psx_idle_edge.site_count; i++)
        if (g_psx_idle_edge.sites[i] == pc) return 1;
    return 0;
}

static inline void psx_check_interrupts_at_inline(CPUState* cpu, uint32_t resume_pc) {
    if (!g_psx_guest_time_frozen) {
        psx_cyc_batch_flush();
        const uint32_t sw = cpu->cop0[13] & cpu->cop0[12] & 0x0300u;
        if (((i_stat & i_mask) | sw) == 0u &&
            !(g_psx_in_exception | g_psx_defer_switch_pending | g_psx_netplay_seen |
              g_ls_suppress_record) &&
            ((g_psx_irq_fast_maintenance + 1u) & 0x3FFFu) != 0u) {
            if (g_idle_skip_enabled == 0) {
                g_psx_irq_fast_maintenance++;
                return;
            }
            if (g_idle_skip_enabled > 0) {
                PsxIdleEdge *e = &g_psx_idle_edge;
                const uint32_t pc = g_dirty_safe_resume_pc ? g_dirty_safe_resume_pc : resume_pc;
                if (pc != 0u && pc != e->pc && g_idle_skip_ext >= 0 &&
                    !(g_ls_replay_active | g_psx_selfcheck_phase | g_idle_note_suppress |
                      g_psx_call_bail | g_precise_mode | g_ls_mode) &&
                    !(e->pc != 0u && g_idle_skip_ext > 0 && e->ext_wired &&
                      psx_edge_idle_site(e->pc))) {
                    g_psx_last_interrupt_check_pc = pc;
                    g_psx_last_interrupt_check_cycle = psx_cycle_count;
                    e->pc = pc;
                    e->streak = 0;
                    e->quantum = 0;
                    e->progress_reg = -2;
                    e->progress_delta = 0;
                    e->mem_progress = 0;
                    e->have_snap = 0;
                    e->store_addr = 0xFFFFFFFFu;
                    e->edges = 0;
                    e->last_cycle = psx_cycle_count;
                    e->last_stores = g_guest_store_count;
                    e->last_mmio = g_mmio_access_count;
                    g_psx_irq_fast_maintenance++;
                    return;
                }
            }
        }
    }
    psx_check_interrupts_at(cpu, resume_pc);
}

#if !defined(PSX_EDGE_IMPL)
#define psx_check_interrupts_at(cpu, pc) psx_check_interrupts_at_inline((cpu), (pc))
#endif

/* The emitters' cycle charge, about one for every two guest instructions.
 * psx_cyc_charge reads five mode flags before its common case (inside a
 * generated function: add to the batch); here four of them are one word,
 * g_psx_cyc_gate (psx_cycles.c, kept by their writers), and any other case
 * takes psx_cyc_charge itself, which decides exactly as before. */
#if !defined(PSX_COSIM)
extern uint32_t g_psx_cyc_gate;
static inline void psx_cyc_charge_edge(uint32_t cycles) {
    const uint32_t prior = g_psx_cyc_batch, sum = prior + cycles;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect((g_psx_cyc_gate == 0u) & (g_psx_cyc_local_acc == NULL) &
                         (g_psx_cyc_bb_defer > 0) & (sum >= prior), 1)) {
#else
    if ((g_psx_cyc_gate == 0u) & (g_psx_cyc_local_acc == NULL) &
        (g_psx_cyc_bb_defer > 0) & (sum >= prior)) {
#endif
        g_psx_cyc_batch = sum;
        return;
    }
    psx_cyc_charge(cycles);
}
#if !defined(PSX_EDGE_IMPL)
#define psx_cyc_charge(c) psx_cyc_charge_edge(c)
#endif

/* The cpu_timing = "block" loads (psx_cyc.h psx_blk_load_*, psx_blk_lwc2_read)
 * with the same main RAM and scratchpad paths, forced inline: in the large
 * generated functions the compiler kept them out of line, a call per load. */
#if defined(__GNUC__) || defined(__clang__)
#define PSX_EDGE_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define PSX_EDGE_ALWAYS_INLINE inline
#endif
#define PSX_BLK_LOAD_EDGE(T, name, size)                                          \
    static PSX_EDGE_ALWAYS_INLINE T name##_edge(uint32_t packed, CPUState* cpu,   \
                                                uint32_t addr, uint32_t rt,       \
                                                uint32_t reg_mask) {              \
        (void)rt; (void)reg_mask;                                                 \
        if (g_ls_mode == 0 && !g_ds_recording && addr < 0xC0000000u) {            \
            const uint32_t phys = addr & 0x1FFFFFFFu;                             \
            T v;                                                                  \
            if (phys < 0x00800000u) {                                             \
                psx_cyc_charge_edge((packed & 3u) + 5u);                          \
                memcpy(&v, g_psx_ram + (phys & g_psx_ram_mask), sizeof(v));       \
                return v;                                                         \
            }                                                                     \
            if (phys - (uint32_t)PSX_SCRATCH_PHYS <= (uint32_t)PSX_SCRATCH_BYTES - size) { \
                psx_cyc_charge_edge((packed >> 8) & 0xFFu);                       \
                memcpy(&v, g_psx_scratch + (phys - (uint32_t)PSX_SCRATCH_PHYS), sizeof(v)); \
                return v;                                                         \
            }                                                                     \
        }                                                                         \
        return (T)psx_blk_load_slow(packed, cpu, addr, size, 2u);                 \
    }
PSX_BLK_LOAD_EDGE(uint32_t, psx_blk_load_word, 4u)
PSX_BLK_LOAD_EDGE(uint16_t, psx_blk_load_half, 2u)
PSX_BLK_LOAD_EDGE(uint8_t,  psx_blk_load_byte, 1u)
#undef PSX_BLK_LOAD_EDGE
static PSX_EDGE_ALWAYS_INLINE uint32_t psx_blk_lwc2_read_edge(uint32_t packed, CPUState* cpu,
                                                              uint32_t addr) {
    if (g_ls_mode == 0 && !g_ds_recording && addr < 0xC0000000u) {
        const uint32_t phys = addr & 0x1FFFFFFFu;
        uint32_t v;
        if (phys < 0x00800000u) {
            psx_cyc_charge_edge((packed & 3u) + 4u);
            memcpy(&v, g_psx_ram + (phys & g_psx_ram_mask), sizeof(v));
            return v;
        }
        if (phys - (uint32_t)PSX_SCRATCH_PHYS <= (uint32_t)PSX_SCRATCH_BYTES - 4u) {
            memcpy(&v, g_psx_scratch + (phys - (uint32_t)PSX_SCRATCH_PHYS), sizeof(v));
            return v;
        }
    }
    return psx_blk_load_slow(packed, cpu, addr, 4u, 1u);
}
#if !defined(PSX_EDGE_IMPL)
#define psx_blk_load_word psx_blk_load_word_edge
#define psx_blk_load_half psx_blk_load_half_edge
#define psx_blk_load_byte psx_blk_load_byte_edge
#define psx_blk_lwc2_read psx_blk_lwc2_read_edge
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* !PSX_OVERLAY_DLL_BUILD */
#endif
