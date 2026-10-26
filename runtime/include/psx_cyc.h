/* psx_cyc.h — R3000A load-delay pipeline interlock (ReadAbsorb / ReadFudge /
 * LDAbsorb / LDWhich).  TIMING ONLY — the load VALUE delay is handled by the
 * existing load-delay correctness path; this models only the CPU cycle cost.
 *
 * FAITHFUL_TIMING_PLAN.md / accuracy/load_readfudge_ldabsorb.md. Transcribed
 * (facts, not code) from the in-tree Beetle oracle
 * psxrecomp/beetle-psx/mednafen/psx/cpu.cpp (RunReal §1 base 795-798, GPR_DEPRES
 * 702-705, DO_LDS 800, ReadMemory 364-451) and libretro.cpp (main-RAM read wait
 * = +3, libretro.cpp:884). Every backend — the dirty-RAM interpreter and BOTH
 * static emitters (code_generator.cpp, full_function_emitter.cpp/strict_translator.cpp)
 * — accounts guest CPU cycles THROUGH THESE HELPERS so they can never disagree.
 *
 * Model (per retired instruction, in program order):
 *   §1   base : if(ReadAbsorb[ReadAbsorbWhich]) ReadAbsorb[ReadAbsorbWhich]--;
 *               else timestamp++;            -- a pending load's give-back is
 *               consumed INSTEAD of the +1 base (pipeline write-back overlap).
 *   deps      : for each GPR this opcode reads/writes, ReadAbsorb[reg]=0 (ends a
 *               give-back when the loaded value is used). ReadAbsorb[0] preserved.
 *   DO_LDS    : commit the PREVIOUS instruction's pending load TIMING:
 *               ReadAbsorb[LDWhich]=LDAbsorb; ReadFudge=LDWhich; ...; LDWhich=0x20.
 *   load only : ReadMemory — clear the current give-back slot, charge the +2 fudge
 *               iff the predecessor committed no load (ReadFudge==0x20), charge the
 *               region wait (main RAM +3) + completion (+2 CPU / +1 LWC2); the
 *               (region+completion) becomes this load's LDAbsorb give-back, armed on
 *               LDWhich=rt for the NEXT instruction's DO_LDS to commit.
 */
#ifndef PSX_CYC_H
#define PSX_CYC_H

#include <stdint.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>       /* MSVC intrinsics: _BitScanForward (no __builtin_ctz) */
#endif
#include "cpu_state.h"   /* CPUState (guard-safe: cpu_state.h includes us last) */
#include "psx_cycles.h"  /* inline psx_advance_cycles */
#include "psx_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load-charge batching (MotK VLC): cache the nearer of the next device
 * deadline or a 64-cycle soft publication limit once per batch. Generated
 * GCC/Clang blocks may defer to their IRQ edge; MMIO also flushes. Pipeline
 * state still updates per instruction, and guest totals at barriers match. */
enum { PSX_CYC_BATCH_SOFT = 64u };

#if defined(PSX_OVERLAY_DLL_BUILD)
/* The overlay shim already batches cycle charges until a host callback edge. */
static inline void psx_cyc_bb_defer_begin(void) { }
static inline void psx_cyc_bb_defer_end(void) { }
static inline void psx_cyc_bb_defer_flush(void) { overlay_flush_cycles(); }
static inline void psx_cyc_local_begin(uint32_t *acc) { (void)acc; }
static inline void psx_cyc_local_end(void) { }
#else
static inline void psx_cyc_bb_defer_begin(void) { g_psx_cyc_bb_defer++; }
static inline void psx_cyc_bb_defer_end(void) {
    if (g_psx_cyc_bb_defer > 0) g_psx_cyc_bb_defer--;
    if (g_psx_cyc_bb_defer == 0) psx_cyc_batch_flush();
}
static inline void psx_cyc_bb_defer_flush(void) { psx_cyc_batch_flush(); }

/* Install a function-local charge sink (MotK VLC leaves). Soft-cap publish
 * at 1<<28 so uint32 cannot wrap; IRQ/MMIO still go through batch_flush. */
static inline void psx_cyc_local_begin(uint32_t *acc) {
#if !defined(PSX_COSIM)
    if (acc) *acc = 0;
    g_psx_cyc_local_acc = acc;
#else
    (void)acc;
#endif
}
static inline void psx_cyc_local_end(void) {
#if !defined(PSX_COSIM)
    psx_cyc_local_publish();
    g_psx_cyc_local_acc = NULL;
#endif
}
#endif

#if defined(__GNUC__) || defined(__clang__)
static inline void psx_cyc_bb_defer_cleanup(int *guard) {
    (void)guard;
    psx_cyc_bb_defer_end();
}
static inline void psx_cyc_local_cleanup(uint32_t **guard) {
    (void)guard;
    psx_cyc_local_end();
}
#endif

/* The whole charge: every mode. psx_cyc_charge below takes its common case
 * inline and hands the rest here; out of line, as the whole thing was too big
 * to be inlined into the generated code and every charge paid a call. */
#if defined(__GNUC__) || defined(__clang__)
static inline __attribute__((noinline)) void psx_cyc_charge_full(uint32_t cycles) {
#else
static inline void psx_cyc_charge_full(uint32_t cycles) {
#endif
    if (cycles == 0u) return;
#if defined(PSX_OVERLAY_DLL_BUILD)
    psx_cpu_charge(cycles);
#else
    /* [timing] guest_cycle_scale: a scaled charge is the CPU charge itself
     * (the one place CPU work is scaled); the batch below is the scale-1
     * production path, unchanged. */
    if (PSX_GCS_ACTIVE()) {
        /* Opt-in ([timing] guest_cycle_scale_batch, PSX_GCS_BATCH=0/1): scale
         * here, exactly as psx_cpu_charge would (same fractional carry, same
         * order), then batch the scaled cycles like the scale-1 path instead
         * of publishing every 1-cycle step to the devices. The batch is
         * flushed at the same observation points (stores, MMIO, interrupt
         * checks, deadlines) as at scale 1. */
        if (!g_psx_gcs_batch) {
            psx_cpu_charge(cycles);
            return;
        }
        cycles = psx_gcs_scale(cycles);
        if (cycles == 0u) return;
#if !defined(PSX_COSIM)
        /* Publish on the guest clock alone: the device deadline or the soft
         * cap. The scale-1 path below also defers while g_psx_cyc_bb_defer is
         * nonzero and routes through the VLC local sink; both are host call-
         * stack state (longjmp escapes leak or reset the depth), so which
         * charge published the batch -- and so which device saw which clock
         * when -- differed between a Mac and a Windows peer, and the scaled
         * race split by a few cycles (R4 2P Mac<->PC: clk/tim fork at ~3296
         * with cpu/ram equal; matching with the batch off). */
        if (!g_ls_replay_active && !g_event_step_conservative &&
            !psx_in_device_service) {
            uint32_t prior = g_psx_cyc_batch;
            uint32_t sum = prior + cycles;
            if (sum >= prior) {
                if (prior == 0u) {
                    uint64_t room = 0u;
                    if (psx_next_service_cycle > psx_cycle_count)
                        room = psx_next_service_cycle - psx_cycle_count;
                    g_psx_cyc_batch_limit = room == 0u ? 1u :
                        (room < (uint64_t)PSX_CYC_BATCH_SOFT
                            ? (uint32_t)room : (uint32_t)PSX_CYC_BATCH_SOFT);
                }
                if (sum < g_psx_cyc_batch_limit) {
                    g_psx_cyc_batch = sum;
                    return;
                }
            }
            psx_advance_cycles(cycles);
            return;
        }
#endif
    }
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(g_ls_replay_active | g_event_step_conservative, 0)) {
#else
    if (g_ls_replay_active || g_event_step_conservative) {
#endif
        psx_advance_cycles(cycles);
        return;
    }
    if (psx_in_device_service) {
        psx_cycle_count += (uint64_t)cycles;
        return;
    }
#if !defined(PSX_COSIM)
    /* Emitter-gated VLC path: stack local, no deadline probe. */
    if (g_psx_cyc_local_acc) {
        uint32_t prior = *g_psx_cyc_local_acc;
        if (cycles <= UINT32_MAX - prior && (prior + cycles) < (1u << 28)) {
            *g_psx_cyc_local_acc = prior + cycles;
            return;
        }
        *g_psx_cyc_local_acc = 0;
        {
            uint32_t *saved = g_psx_cyc_local_acc;
            g_psx_cyc_local_acc = NULL;
            if (prior) psx_advance_cycles(prior);
            g_psx_cyc_local_acc = saved;
        }
        *g_psx_cyc_local_acc = cycles;
        return;
    }
    {
        uint32_t prior = g_psx_cyc_batch;
        uint32_t sum = prior + cycles;
        if (sum >= prior) { /* no uint32 wrap */
            if (g_psx_cyc_bb_defer > 0) {
                g_psx_cyc_batch = sum;
                return;
            }
            if (prior == 0u) {
                uint64_t room = 0u;
                if (psx_next_service_cycle > psx_cycle_count)
                    room = psx_next_service_cycle - psx_cycle_count;
                g_psx_cyc_batch_limit = room == 0u ? 1u :
                    (room < (uint64_t)PSX_CYC_BATCH_SOFT
                        ? (uint32_t)room : (uint32_t)PSX_CYC_BATCH_SOFT);
            }
            if (sum < g_psx_cyc_batch_limit) {
                g_psx_cyc_batch = sum;
                return;
            }
        }
    }
#endif
    psx_advance_cycles(cycles); /* publishes any pending batch first */
#endif
}

#if !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_COSIM)
/* psx_cyc_charge_full's common case, the same steps: no guest cycle scale,
 * replay, conservative stepping, device service or local sink, and the sum
 * joins the batch (deferred block, or a running batch still under its
 * limit). Anything else, including a batch's first charge (its limit is
 * computed there), goes to psx_cyc_charge_full. */
static inline void psx_cyc_charge(uint32_t cycles) {
    if (!(PSX_GCS_ACTIVE() | g_ls_replay_active | g_event_step_conservative |
          psx_in_device_service) && !g_psx_cyc_local_acc) {
        const uint32_t prior = g_psx_cyc_batch, sum = prior + cycles;
        if (sum >= prior &&
            (g_psx_cyc_bb_defer > 0 || (prior != 0u && sum < g_psx_cyc_batch_limit))) {
            g_psx_cyc_batch = sum;
            return;
        }
    }
    psx_cyc_charge_full(cycles);
}
#else
static inline void psx_cyc_charge(uint32_t cycles) { psx_cyc_charge_full(cycles); }
#endif

/* §1 base (Beetle cpu.cpp:795-798). */
static inline void psx_cyc_base(CPUState* cpu) {
    uint8_t w = cpu->read_absorb_which;
    if (cpu->read_absorb[w]) cpu->read_absorb[w]--;
    else                     psx_cyc_charge(1u);
}

/* GPR_DEPRES (Beetle cpu.cpp:702-705): zero ReadAbsorb[n] for every source/dest
 * GPR of this instruction, preserving ReadAbsorb[0] (skipping bit 0 == Beetle's
 * save/restore of ReadAbsorb[0]). */
static inline void psx_cyc_deps(CPUState* cpu, uint32_t reg_mask) {
    reg_mask &= 0xFFFFFFFEu;   /* never touch ReadAbsorb[0] */
    if (reg_mask && (reg_mask & (reg_mask - 1u)) == 0u) {
#if defined(_MSC_VER)
        unsigned long n;
        _BitScanForward(&n, reg_mask);
        cpu->read_absorb[(unsigned)n] = 0u;
#else
        cpu->read_absorb[(unsigned)__builtin_ctz(reg_mask)] = 0u;
#endif
        return;
    }
    while (reg_mask) {
#if defined(_MSC_VER)
        unsigned long _psx_ctz_idx;
        _BitScanForward(&_psx_ctz_idx, reg_mask);
        unsigned n = (unsigned)_psx_ctz_idx;
#else
        unsigned n = (unsigned)__builtin_ctz(reg_mask);
#endif
        cpu->read_absorb[n] = 0u;
        reg_mask &= reg_mask - 1u;
    }
}

/* DO_LDS timing-commit (Beetle cpu.cpp:800). LDWhich==0x20 (no pending) writes the
 * dummy slot read_absorb[32] and sets read_fudge=0x20 (=> next load gets +2 fudge). */
static inline void psx_cyc_lds(CPUState* cpu) {
    uint8_t lw = cpu->ld_which_t;
    cpu->read_absorb[lw]    = (uint8_t)cpu->ld_absorb;
    cpu->read_fudge         = lw;
    cpu->read_absorb_which  = (uint8_t)(cpu->read_absorb_which | (lw & 0x1Fu));
    cpu->ld_which_t         = 0x20u;
}

/* Full per-instruction interlock for a NON-CPU-load instruction (ALU, shift,
 * branch, jump, store, COP control, LWC2/SWC2 pre-step, mult/div, mfhi/mflo, ...).
 * reg_mask from psx_cyc_dep_res_mask(). MUST be emitted BEFORE the instruction
 * body so §1 precedes any muldiv/GTE deadline stall in the body (Beetle order). */
static inline void psx_cyc_step(CPUState* cpu, uint32_t reg_mask) {
    psx_cyc_base(cpu);
    psx_cyc_deps(cpu, reg_mask);
    psx_cyc_lds(cpu);
}

/* A run of plain instructions' steps in one call (the game emitter folds
 * them: recompiler/src/code_generator.cpp, "cycle-step folding"): each
 * instruction's §1 base, GPR_DEPRES and DO_LDS exactly as psx_cyc_step does
 * them, in program order, with their +1s added up into one charge. Nothing
 * in such a run observes the clock (pure ALU and branches; a store may be
 * the last: its body comes after every step), so one charge at the head is
 * the same guest time at every observation point; the pipeline state ends
 * byte for byte as the per-instruction steps leave it. */
/* One call per run, not the run's body at every site: tens of thousands of
 * runs inlined made the game's code a third bigger. */
#if defined(__GNUC__) || defined(__clang__)
#define PSX_CYC_RUN_FN static __attribute__((noinline))
#else
#define PSX_CYC_RUN_FN static
#endif
static inline uint32_t psx_cyc_base_take(CPUState* cpu) {
    uint8_t w = cpu->read_absorb_which;
    if (cpu->read_absorb[w]) { cpu->read_absorb[w]--; return 0u; }
    return 1u;
}
PSX_CYC_RUN_FN void psx_cyc_steps2(CPUState* cpu, uint32_t m0, uint32_t m1) {
    uint32_t c = psx_cyc_base_take(cpu); psx_cyc_deps(cpu, m0); psx_cyc_lds(cpu);
    c += psx_cyc_base_take(cpu); psx_cyc_deps(cpu, m1); psx_cyc_lds(cpu);
    psx_cyc_charge(c);
}
PSX_CYC_RUN_FN void psx_cyc_steps3(CPUState* cpu, uint32_t m0, uint32_t m1, uint32_t m2) {
    uint32_t c = psx_cyc_base_take(cpu); psx_cyc_deps(cpu, m0); psx_cyc_lds(cpu);
    c += psx_cyc_base_take(cpu); psx_cyc_deps(cpu, m1); psx_cyc_lds(cpu);
    c += psx_cyc_base_take(cpu); psx_cyc_deps(cpu, m2); psx_cyc_lds(cpu);
    psx_cyc_charge(c);
}
PSX_CYC_RUN_FN void psx_cyc_steps4(CPUState* cpu, uint32_t m0, uint32_t m1, uint32_t m2,
                                  uint32_t m3) {
    uint32_t c = psx_cyc_base_take(cpu); psx_cyc_deps(cpu, m0); psx_cyc_lds(cpu);
    c += psx_cyc_base_take(cpu); psx_cyc_deps(cpu, m1); psx_cyc_lds(cpu);
    c += psx_cyc_base_take(cpu); psx_cyc_deps(cpu, m2); psx_cyc_lds(cpu);
    c += psx_cyc_base_take(cpu); psx_cyc_deps(cpu, m3); psx_cyc_lds(cpu);
    psx_cyc_charge(c);
}

/* The GPR dep+res bitmask used by psx_cyc_step lives in psx_instr_cost.h
 * (psx_cyc_dep_res_mask) — a standalone pure function shared by the emitters
 * (gen-time literal) and the interpreter (runtime), with no CPUState dependency. */

/* Main-RAM base + load-delay gate (memory.c). Inlined load helpers use these
 * so MotK VLC / decode hot paths avoid an out-of-line call per LW/LH. */
extern uint8_t *g_psx_ram;
extern int      g_psx_load_delay;
extern int      g_ls_mode;
extern volatile int g_ds_recording;
int psx_load_delay_enabled(void);
uint32_t psx_cyc_load_word_slow(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);
uint16_t psx_cyc_load_half_slow(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);

enum { PSX_SCRATCH_PHYS = 0x1F800000u, PSX_SCRATCH_BYTES = 0x400u };

/* Beetle MemRW device-region READ wait (libretro.cpp:859-1131), the device-
 * dependent part of a load's access cost (added before the +completion). size =
 * access width in bytes (the SPU and CDC waits depend on it). phys = masked
 * physical address. memory.c adds its PSX_MMIO_WAIT bisect gate on top. */
static inline uint32_t psx_cyc_region_wait(uint32_t phys, uint32_t size) {
    /* Main RAM, mirrored across phys 0..0x7FFFFF (libretro.cpp:874 `A < 0x00800000`). */
    if (phys < 0x00800000u) return 3u;
    /* BIOS ROM (905) and Expansion 1 / PIO (1134): no extra device wait. */
    if (phys >= 0x1FC00000u && phys <= 0x1FC7FFFFu) return 0u;
    if (phys >= 0x1F000000u && phys <= 0x1F7FFFFFu) return 0u;
    /* Hardware MMIO window 0x1F801000..0x1F802FFF (921). */
    if (phys >= 0x1F801000u && phys <= 0x1F802FFFu) {
        if (phys >= 0x1F801C00u && phys <= 0x1F801FFFu)            /* SPU (929) */
            return (size == 4u) ? 36u : 16u;
        if (phys >= 0x1F801800u && phys <= 0x1F80180Fu)            /* CDC (979) */
            return 6u * size;
        if (phys >= 0x1F801810u && phys <= 0x1F801817u) return 1u; /* GPU (994) */
        if (phys >= 0x1F801820u && phys <= 0x1F801827u) return 1u; /* MDEC (1007) */
        if (phys >= 0x1F801000u && phys <= 0x1F801023u) return 1u; /* SysControl (1020) */
        if (phys >= 0x1F801040u && phys <= 0x1F80104Fu) return 1u; /* FrontIO/pad (1043) */
        if (phys >= 0x1F801050u && phys <= 0x1F80105Fu) return 1u; /* SIO (1055) */
        if (phys >= 0x1F801070u && phys <= 0x1F801077u) return 1u; /* IRQ (1094) */
        if (phys >= 0x1F801080u && phys <= 0x1F8010FFu) return 1u; /* DMA (1106) */
        if (phys >= 0x1F801100u && phys <= 0x1F80113Fu) return 1u; /* Timers (1119) */
        return 0u;   /* unmatched MMIO: Beetle adds no device wait */
    }
    return 0u;       /* unknown / open-bus region */
}

#if !defined(PSX_OVERLAY_DLL_BUILD)
/* The 1 KiB scratchpad (D-cache, memory.c). A CPU load from it is Beetle
 * ReadMemory's scratchpad case (cpu.cpp 414-422): after §1/deps/DO_LDS, no
 * wait, no fudge, no give-back. The same sequence the slow path runs
 * (psx_cyc_load_timing + psx_cyc_readmem), inlined: the game's work buffers
 * and GTE scratch live there. */
extern uint8_t *g_psx_scratch;
static inline int psx_cyc_scratch_off(uint32_t addr, uint32_t width, uint32_t *off) {
    const uint32_t o = (addr & 0x1FFFFFFFu) - (uint32_t)PSX_SCRATCH_PHYS;
    if (addr >= 0xC0000000u || o > (uint32_t)PSX_SCRATCH_BYTES - width) return 0;
    *off = o;
    return 1;
}
static inline void psx_cyc_scratch_timing(CPUState* cpu, uint32_t rt, uint32_t reg_mask) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (g_psx_load_delay < 0) (void)psx_load_delay_enabled();
    if (!g_psx_load_delay) return;
    psx_cyc_base(cpu);
    psx_cyc_deps(cpu, reg_mask);
    if (cpu->ld_which_t == rt) cpu->ld_which_t = 0u;
    psx_cyc_lds(cpu);
    cpu->read_absorb[cpu->read_absorb_which] = 0u;
    cpu->read_absorb_which = 0u;
    cpu->ld_absorb = 0u;
    cpu->ld_which_t = (uint8_t)rt;
#else
    (void)cpu; (void)rt; (void)reg_mask;
#endif
}
#endif

/* Overlay DLLs route timed loads through the host callback shim so they share
 * runtime timing and memory state. Normal generated code keeps the main-RAM
 * fast path inline. */
#if defined(PSX_OVERLAY_DLL_BUILD)
uint32_t psx_cyc_load_word(CPUState* cpu, uint32_t addr,
                           uint32_t rt, uint32_t reg_mask);
uint16_t psx_cyc_load_half(CPUState* cpu, uint32_t addr,
                           uint32_t rt, uint32_t reg_mask);
#else
/* CPU data load value+timing. Full Beetle sequence for main RAM is inlined;
 * MMIO / lockstep / data-shard fall through to *_slow in memory.c. */
static inline uint32_t psx_cyc_load_word(CPUState* cpu, uint32_t addr,
                                          uint32_t rt, uint32_t reg_mask) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (g_ls_mode == 0 && !g_ds_recording && phys < 0x00800000u) {
        if (g_psx_load_delay < 0) (void)psx_load_delay_enabled();
        if (g_psx_load_delay) {
            psx_cyc_base(cpu);
            psx_cyc_deps(cpu, reg_mask);
            if (cpu->ld_which_t == rt) cpu->ld_which_t = 0u;
            psx_cyc_lds(cpu);
            cpu->read_absorb[cpu->read_absorb_which] = 0u;
            cpu->read_absorb_which = 0u;
            uint32_t fudge = (uint32_t)((cpu->read_fudge >> 4) & 2u);
            cpu->ld_absorb = 5u; /* main-RAM wait 3 + completion 2 */
            psx_cyc_charge(fudge + 5u);
            cpu->ld_which_t = (uint8_t)rt;
        }
        uint32_t value;
        memcpy(&value, g_psx_ram + (phys & g_psx_ram_mask), sizeof(value));
        return value;
    }
    if (g_ls_mode == 0 && !g_ds_recording) {
        uint32_t off;
        if (psx_cyc_scratch_off(addr, 4u, &off)) {
            uint32_t value;
            psx_cyc_scratch_timing(cpu, rt, reg_mask);
            memcpy(&value, g_psx_scratch + off, sizeof(value));
            return value;
        }
    }
    return psx_cyc_load_word_slow(cpu, addr, rt, reg_mask);
#else
    (void)cpu; (void)rt; (void)reg_mask;
    extern uint32_t psx_read_word(uint32_t a);
    return psx_read_word(addr);
#endif
}

static inline uint16_t psx_cyc_load_half(CPUState* cpu, uint32_t addr,
                                          uint32_t rt, uint32_t reg_mask) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (g_ls_mode == 0 && !g_ds_recording && phys < 0x00800000u) {
        if (g_psx_load_delay < 0) (void)psx_load_delay_enabled();
        if (g_psx_load_delay) {
            psx_cyc_base(cpu);
            psx_cyc_deps(cpu, reg_mask);
            if (cpu->ld_which_t == rt) cpu->ld_which_t = 0u;
            psx_cyc_lds(cpu);
            cpu->read_absorb[cpu->read_absorb_which] = 0u;
            cpu->read_absorb_which = 0u;
            uint32_t fudge = (uint32_t)((cpu->read_fudge >> 4) & 2u);
            cpu->ld_absorb = 5u;
            psx_cyc_charge(fudge + 5u);
            cpu->ld_which_t = (uint8_t)rt;
        }
        uint16_t value;
        memcpy(&value, g_psx_ram + (phys & g_psx_ram_mask), sizeof(value));
        return value;
    }
    if (g_ls_mode == 0 && !g_ds_recording) {
        uint32_t off;
        if (psx_cyc_scratch_off(addr, 2u, &off)) {
            uint16_t value;
            psx_cyc_scratch_timing(cpu, rt, reg_mask);
            memcpy(&value, g_psx_scratch + off, sizeof(value));
            return value;
        }
    }
    return psx_cyc_load_half_slow(cpu, addr, rt, reg_mask);
#else
    (void)cpu; (void)rt; (void)reg_mask;
    extern uint16_t psx_read_half(uint32_t a);
    return psx_read_half(addr);
#endif
}
#endif

extern uint8_t psx_cyc_load_byte(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);

/* [recompiler] cpu_timing = "block" (docs/PERFORMANCE_MODES.md). The game
 * emitter runs the pipeline above over each block at generation time (a clean
 * pipeline at the block's entry, every load in main RAM) and charges the
 * instructions in one psx_cyc_charge before anything that can see the clock;
 * no pipeline state is kept at run time. A load charges its own access here,
 * from the emitter's `packed`:
 *   bits 0-1   fudge: 2 when the previous instruction committed no load
 *   bits 8-15  g: the cycles a main-RAM load's give-back saves the block's next
 *              instructions (already left out of their charge)
 *   bits 16-23 k: what they would save with an unbounded give-back
 * Main RAM: fudge + wait 3 + completion 2. Scratchpad: no fudge, wait or give-
 * back, so it pays the g cycles the block left out. Other regions: fudge + G
 * (region wait + completion) + g - min(G, k). LWC2: completion 1 and no give-
 * back (g = k = 0). Not exact at block edges (a load pending across one, an
 * I-cache miss cutting a give-back): an option for speed. */
static inline uint32_t psx_blk_access_cost(uint32_t packed, uint32_t phys,
                                           uint32_t size, uint32_t compl_cost) {
    const uint32_t g = (packed >> 8) & 0xFFu, k = (packed >> 16) & 0xFFu;
    if (phys - (uint32_t)PSX_SCRATCH_PHYS < (uint32_t)PSX_SCRATCH_BYTES) return g;
    {
        const uint32_t big = psx_cyc_region_wait(phys, size) + compl_cost;
        return (packed & 3u) + big + g - (big < k ? big : k);
    }
}

#if defined(PSX_OVERLAY_DLL_BUILD)
/* Overlay DLLs: the access cost joins the DLL's pending cycles, which are
 * published (devices brought up to date) before the untimed read, as the
 * exact path does. */
#define PSX_BLK_LOAD_FN(T, name, read, size)                                    \
    static inline T name(uint32_t packed, CPUState* cpu, uint32_t addr,        \
                         uint32_t rt, uint32_t reg_mask) {                     \
        (void)rt; (void)reg_mask;                                              \
        psx_cpu_charge(psx_blk_access_cost(packed, addr & 0x1FFFFFFFu, size, 2u)); \
        overlay_flush_cycles();                                                \
        return (T)cpu->read(addr);                                             \
    }
PSX_BLK_LOAD_FN(uint32_t, psx_blk_load_word, read_word, 4u)
PSX_BLK_LOAD_FN(uint16_t, psx_blk_load_half, read_half, 2u)
PSX_BLK_LOAD_FN(uint8_t,  psx_blk_load_byte, read_byte, 1u)
#undef PSX_BLK_LOAD_FN
static inline uint32_t psx_blk_lwc2_read(uint32_t packed, CPUState* cpu, uint32_t addr) {
    psx_cpu_charge(psx_blk_access_cost(packed, addr & 0x1FFFFFFFu, 4u, 1u));
    overlay_flush_cycles();
    return cpu->read_word(addr);
}
#else
/* Main RAM and scratchpad inline; everything else (devices, BIOS, the
 * recording modes) through memory.c. */
uint32_t psx_blk_load_slow(uint32_t packed, CPUState* cpu, uint32_t addr,
                           uint32_t size, uint32_t compl_cost);
#define PSX_BLK_LOAD_FN(T, name, size)                                          \
    static inline T name(uint32_t packed, CPUState* cpu, uint32_t addr,        \
                         uint32_t rt, uint32_t reg_mask) {                     \
        (void)rt; (void)reg_mask;                                              \
        if (g_ls_mode == 0 && !g_ds_recording && addr < 0xC0000000u) {          \
            const uint32_t phys = addr & 0x1FFFFFFFu;                           \
            T v;                                                               \
            if (phys < 0x00800000u) {                                          \
                psx_cyc_charge((packed & 3u) + 5u);                            \
                memcpy(&v, g_psx_ram + (phys & g_psx_ram_mask), sizeof(v));    \
                return v;                                                      \
            }                                                                  \
            if (phys - (uint32_t)PSX_SCRATCH_PHYS <= (uint32_t)PSX_SCRATCH_BYTES - size) { \
                psx_cyc_charge((packed >> 8) & 0xFFu);                         \
                memcpy(&v, g_psx_scratch + (phys - (uint32_t)PSX_SCRATCH_PHYS), sizeof(v)); \
                return v;                                                      \
            }                                                                  \
        }                                                                      \
        return (T)psx_blk_load_slow(packed, cpu, addr, size, 2u);              \
    }
PSX_BLK_LOAD_FN(uint32_t, psx_blk_load_word, 4u)
PSX_BLK_LOAD_FN(uint16_t, psx_blk_load_half, 2u)
PSX_BLK_LOAD_FN(uint8_t,  psx_blk_load_byte, 1u)
#undef PSX_BLK_LOAD_FN
static inline uint32_t psx_blk_lwc2_read(uint32_t packed, CPUState* cpu, uint32_t addr) {
    if (g_ls_mode == 0 && !g_ds_recording && addr < 0xC0000000u) {
        const uint32_t phys = addr & 0x1FFFFFFFu;
        uint32_t v;
        if (phys < 0x00800000u) {
            psx_cyc_charge((packed & 3u) + 4u);
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
#endif

/* [recompiler] ram_fast_path (docs/PERFORMANCE_MODES.md): the entries the game
 * emitter uses in place of cpu->write_* and psx_cyc_load_byte. Main RAM takes
 * the short path (the byte load: the same Beetle sequence the word and half
 * loads above inline; the stores: memory.c, only when psx_write_* would do
 * nothing more than write); anything else takes the usual path. Overlay DLLs
 * keep the usual path. */
#if defined(PSX_OVERLAY_DLL_BUILD)
static inline uint8_t psx_cyc_load_byte_fast(CPUState* cpu, uint32_t addr,
                                             uint32_t rt, uint32_t reg_mask) {
    return psx_cyc_load_byte(cpu, addr, rt, reg_mask);
}
static inline void psx_store_word(CPUState* cpu, uint32_t addr, uint32_t val) { cpu->write_word(addr, val); }
static inline void psx_store_half(CPUState* cpu, uint32_t addr, uint16_t val) { cpu->write_half(addr, val); }
static inline void psx_store_byte(CPUState* cpu, uint32_t addr, uint8_t val) { cpu->write_byte(addr, val); }
#else
void psx_store_word(CPUState* cpu, uint32_t addr, uint32_t val);
void psx_store_half(CPUState* cpu, uint32_t addr, uint16_t val);
void psx_store_byte(CPUState* cpu, uint32_t addr, uint8_t val);
static inline uint8_t psx_cyc_load_byte_fast(CPUState* cpu, uint32_t addr,
                                             uint32_t rt, uint32_t reg_mask) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (g_ls_mode == 0 && !g_ds_recording && phys < 0x00800000u && addr < 0xC0000000u) {
        if (g_psx_load_delay < 0) (void)psx_load_delay_enabled();
        if (g_psx_load_delay) {
            psx_cyc_base(cpu);
            psx_cyc_deps(cpu, reg_mask);
            if (cpu->ld_which_t == rt) cpu->ld_which_t = 0u;
            psx_cyc_lds(cpu);
            cpu->read_absorb[cpu->read_absorb_which] = 0u;
            cpu->read_absorb_which = 0u;
            uint32_t fudge = (uint32_t)((cpu->read_fudge >> 4) & 2u);
            cpu->ld_absorb = 5u; /* main-RAM wait 3 + completion 2 */
            psx_cyc_charge(fudge + 5u);
            cpu->ld_which_t = (uint8_t)rt;
        }
        return g_psx_ram[phys & g_psx_ram_mask];
    }
    if (g_ls_mode == 0 && !g_ds_recording) {
        uint32_t off;
        if (psx_cyc_scratch_off(addr, 1u, &off)) {
            psx_cyc_scratch_timing(cpu, rt, reg_mask);
            return g_psx_scratch[off];
        }
    }
#endif
    return psx_cyc_load_byte(cpu, addr, rt, reg_mask);
}
#endif

/* Charge the exact timing/pipeline effects of a 32-bit CPU load without
 * invoking the memory handler.  Enhancement HLE may use this only when static
 * analysis proves the loaded value and read side effects are unobservable. */
extern void psx_cyc_load_word_timing_only(CPUState* cpu, uint32_t addr,
                                           uint32_t rt, uint32_t reg_mask);

/* LWC2 (GTE load) ReadMemory timing only (completion +1, NO LDWhich arm — the dest
 * is a GTE register). Call AFTER psx_cyc_step(cpu,0) (§1+DO_LDS) and psx_gte_stall.
 * Returns the raw 32-bit value. */
#if defined(PSX_OVERLAY_DLL_BUILD)
extern uint32_t psx_cyc_lwc2_read(CPUState* cpu, uint32_t addr);
#else
uint32_t psx_cyc_lwc2_read_slow(CPUState* cpu, uint32_t addr);
/* Main RAM and scratchpad inline (psx_cyc_readmem's sequence with completion
 * +1 and no arm; no PSX_LOAD_DELAY gate, as in the slow path); devices, BIOS
 * and the recording modes take the slow path. */
static inline uint32_t psx_cyc_lwc2_read(CPUState* cpu, uint32_t addr) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (g_ls_mode == 0 && !g_ds_recording && addr < 0xC0000000u) {
        const uint32_t phys = addr & 0x1FFFFFFFu;
        uint32_t value, off;
        if (phys < 0x00800000u) {
            cpu->read_absorb[cpu->read_absorb_which] = 0u;
            cpu->read_absorb_which = 0u;
            const uint32_t fudge = (uint32_t)((cpu->read_fudge >> 4) & 2u);
            cpu->ld_absorb = 4u; /* main-RAM wait 3 + completion 1 */
            psx_cyc_charge(fudge + 4u);
            cpu->ld_which_t = 0x20u;
            memcpy(&value, g_psx_ram + (phys & g_psx_ram_mask), sizeof(value));
            return value;
        }
        if (psx_cyc_scratch_off(addr, 4u, &off)) {
            cpu->read_absorb[cpu->read_absorb_which] = 0u;
            cpu->read_absorb_which = 0u;
            cpu->ld_absorb = 0u;
            cpu->ld_which_t = 0x20u;
            memcpy(&value, g_psx_scratch + off, sizeof(value));
            return value;
        }
    }
#endif
    return psx_cyc_lwc2_read_slow(cpu, addr);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* PSX_CYC_H */
