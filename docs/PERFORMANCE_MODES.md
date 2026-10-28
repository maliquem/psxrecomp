# Performance modes (opt-in, per game)

The default path is the faithful one (CLAUDE.md Rule -1): the R3000A pipeline
timing per instruction, every guest store through the full memory path, every
written register's PGXP shadow updated. The options below trade some of that
for host speed, or change only when work is done. Each is **off by default**; a
title turns one on in its `game.toml` only after checking it (see "Checking a
mode" below). A game that misbehaves with one simply leaves it off.

| Key | Values | Exact? | Needs regen | What it removes |
|---|---|---|---|---|
| `[recompiler] ram_fast_path` | `false` (default) / `true` | yes | yes | the full memory path for plain main-RAM stores and byte loads |
| `[video] pgxp_alu` | `"full"` (default) / `"moves"` | no (sub-pixel only) | no (runtime) | the PGXP engine call on most ALU instructions |
| `[recompiler] cpu_timing` | `"pipeline"` (default) / `"block"` | no (block edges) | yes | the per-instruction pipeline bookkeeping at run time |
| `[video] pgxp_mem` | `"all"` (default) / `"words"` | no (sub-pixel only) | no (runtime) | the PGXP engine call on byte and halfword loads/stores |
| `[runtime] overlay_precompile` | `false` (default) / `true` | yes | no | nothing: compiles the captured overlays at boot instead of at first visit |

All three change the generator's output, so the codegen hash moves once when
this code lands (savestates of the old exe need the hash re-stamped; their
contents are unaffected). `ram_fast_path` and `cpu_timing` also enter the
overlay config hash, only when turned on: a title on the default path keeps its
overlay cache.

## `[recompiler] ram_fast_path` (exact)

The game emitter calls `psx_store_word/half/byte` (memory.c) instead of the
`cpu->write_*` pointers, and `psx_cyc_load_byte_fast` (psx_cyc.h) instead of
the out-of-line byte load. A store goes straight to RAM only when
`psx_write_*` would do nothing but write the bytes, count the store and note it
for the idle-skip detector:

- the CPU's own store pointer (a mod's guest view drops stores);
- aligned main RAM above the 64 KiB kernel window;
- no overlay cycles to publish first, no render pass, lockstep, data-shard
  recording, DMA or host store, cache isolation, armed parity/card recorder or
  live debug write trace;
- a page no code watcher acts on: no executed-code evidence (a write clears
  it), no watched overlay page, no game text not yet seen modified.

Everything else takes `cpu->write_*` unchanged. The byte load is the same
Beetle sequence the word and half loads already inline. Overlay DLLs keep the
usual path.

Also exact, and unconditional (debug builds only): the per-block-leader
observer call (`debug_server_cyc_observe`) is skipped while nothing it feeds is
armed (`g_debug_block_observe_live`: passive recorders, cyc_watch, pc_probe,
a lockstep window, a title debug knob from the environment).

## `[video] pgxp_alu = "moves"` (sub-pixel only)

Every ALU hook carries its class (`pgxp_hooks.h` `PGXP_ALU_K_*`: move, LUI,
ORI, shift, add, logic, set-less-than, HI/LO, MFC0). `g_pgxp_alu_mask` picks the
classes that reach the engine: all of them by default (the generated code calls
the engine exactly as before), or only moves, LUI and ORI. A register that a
skipped class wrote keeps its old shadow; the value check on later use drops it
unless the new word equals the old one, in which case the old fraction rides
along on an equal integer (at most a sub-pixel difference). This is how a
memory-only PGXP behaves. CPU mode always takes every class. `PSX_PGXP_ALU`
(`full`, `moves` or a class mask) overrides the key for a run.

## `[recompiler] cpu_timing = "block"` (exact inside a block)

The game emitter runs the R3000A load-delay pipeline of `psx_cyc.h` (Beetle's
ReadAbsorb / ReadFudge / LDAbsorb / LDWhich, the model `"pipeline"` runs per
instruction) over each block **at generation time**, from a clean pipeline at
the block's entry and with every load in main RAM (`blk_pipe_run` in
code_generator.cpp). Each instruction's base cycle (0 when a load's give-back
absorbs it) joins a pending count that the block charges in one
`psx_cyc_charge` before any body that can see the clock (memory, GTE, mult/div,
COP0, calls, hooks), before a mid-block label and at the exit. No pipeline
state is kept at run time; I-cache fetches, mult/div and GTE stalls and the
device timing are unchanged.

A load charges its own access in its helper (`psx_blk_load_*`,
`psx_blk_lwc2_read`), from a literal the emitter computed for it (`packed`:
the fudge, `g` = the cycles its main-RAM give-back saves the block's next
instructions, `k` = what they would save with an unbounded one). Main RAM:
fudge + 5. Scratchpad: no fudge, wait or give-back, so it pays back the `g`
cycles the block left out. Other regions: fudge + G + g - min(G, k), with G
the region wait plus completion. The result is the pipeline's cost exactly,
except at block edges: a load pending across one, or a give-back an I-cache
miss would cut. So a game sees the same clock as `"pipeline"` give or take a
few cycles per block, which keeps iteration-count loops (below) on time.

This replaces an earlier `"block"` that charged calibrated constants per
instruction and per load (`cpu_timing_insn`, `cpu_timing_ram_load`, removed):
no pair of constants fit every scene of BoF4 (see the lesson below).

### Lesson from Breath of Fire IV: iteration-count timeouts

This lesson is what the per-block pipeline cost above answers. PsyQ's
`VSync(n)` waits in a loop with an iteration-count timeout
(`(n-1) << 15` iterations: 32768 for `VSync(2)`). Under the pipeline model the
loop costs ~32 cycles per iteration (three dependent main-RAM loads), so the
timeout is ~1.85 VBlanks away. A block-mode load cost of 0..1 cycle makes the
loop 14..17 cycles: the timeout fires before the VBlank arrives, VSync returns
early and the game runs above its normal rate (BoF4: 40-45 displayed frames
per second where the hardware shows 30). Too high a cost tips the heavy scenes
(BoF4's night field at 93% of the frame) into 20 fps. No single pair of
constants matched every scene of BoF4 closely enough. The per-block pipeline
cost charges the loop what the pipeline does; still, any title using PsyQ VSync
(most of them) should check `display_flips` (TCP `perf_summary`) in every
scene, not only the busy fraction.

## `[video] pgxp_mem = "words"` (sub-pixel only)

The PGXP load/store hooks of byte and halfword accesses (LB, LH, LBU, LHU, SB,
SH) are left out (`pgxp_hooks.h` `g_pgxp_mem_skip`; the generated code, the
dirty-RAM interpreter and the overlay DLLs' hook table all apply it). What
they write keeps its old shadow, which the engine's value check turns into the
plain integer position on use, never a wrong one: the same coverage DuckStation's
recompiler gives (its PGXP hooks words, LWC2/SWC2 and moves). Word accesses,
LWL/LWR/SWL/SWR and the GTE transfers keep theirs. CPU mode always takes every
hook. `PSX_PGXP_MEM` (`all`, `words` or a skip mask) overrides the key for a
run.

## `[runtime] overlay_precompile` (exact)

A captured overlay runs in the dirty-RAM interpreter until its DLL is built,
which the autocapture asks for after a visit; after an update that moves the
codegen hash, every area's first visit runs interpreted. With this on, ~120
VBlanks after boot the runtime asks the code provider once for every captured
overlay missing from the cache (the request `autocompile_run` makes; it skips
what is built). The same DLLs, built earlier. `PSX_OVERLAY_PRECOMPILE=0/1`
overrides it for a run.

## Exact host work removed (no option: always on)

These change only how the host does the same work; every one was checked
against the build before it (frame fingerprints for 630 frames in 8 scenes,
main RAM and scratchpad byte for byte with the debug recorders off, pictures
and sound):

- scratchpad loads and LWC2 (main RAM and scratchpad) inline in `psx_cyc.h`,
  the same pipeline sequence the slow path ran;
- overlay DLL loads go to host entries that run those inline paths
  (`psx_cyc_load_*_host`), not the slow path;
- the plain main-RAM store (`psx_store_fast_ptr`) is tried inside
  `psx_write_word/half/byte` too, after the overlay's pending cycles are
  published, so overlay DLL stores and the executable's stores made while an
  overlay is active take it;
- `psx_cyc_charge`'s common case (a running batch under its limit, or a
  deferred block) is a few inline instructions; everything else goes to the
  out-of-line `psx_cyc_charge_full`, the whole previous function;
- `gpu_get_display_info` keeps its last answer per thread while its twelve
  GP1 inputs are unchanged (the widescreen code asks per primitive);
- the render thread's ring: the producer keeps the consumer's last position and
  publishes records with a release store (a full fence only where it checks
  whether the consumer sleeps);
- the turbo presents at most once per refresh of the host display and never
  while the render thread is behind (`PSX_TURBO_PRESENT_SKIP=0`: the old
  fixed cadence); the debug display ring copies VRAM only while a recorder is
  on.

Cycle 5 (2026-10-27), the same checks plus a cold boot through the opening
movie. Profiled first: on Breath of Fire IV only ~7% of the emulation thread
was the game's own compiled code; the rest was work around it.

- **GPU linked-list hold in one span** (`dma.c`): a GP0 linked-list kick holds
  the CPU until the walk ends. The walk used one device event per list word,
  and every event advanced all six devices and recomputed the deadline
  (4-11 thousand device services per VBlank). With the CPU held and no other
  DMA moving RAM the list cannot change, so `dma_gpu_ll_cycles_to_finish` walks
  a copy (`psx_peek_word`, no side effects) to find the end, the hold advances
  there at once, and while it does the walker's words are not device events:
  the other devices advance in spans bounded by their own events, as the
  deadline path does between events. GPU word handling does not read the guest
  clock. `PSX_DMA_LL_BULK=0`: one word per span.
- **Block-edge interrupt check inline** (`psx_edge.h`, included last by
  `psx_runtime.h`): the edge's common case (nothing deliverable, outside the
  handler, no thread switch, no netplay/self-check, not the 16K-th edge) and the
  idle detector's restart on a new candidate PC are written in place; anything
  else calls `psx_check_interrupts_at`, which starts from the top as before.
- **I-cache tag compare inline** (`psx_icache.h`): a hit changes nothing, so
  only a miss calls `psx_icache_fetch_miss`; the overlay callback compares
  first too.
- **Scratchpad stores** take the direct store path (they went through the
  whole slow path: ~17 thousand per VBlank in a 3D scene); main-RAM stores into
  game text not yet seen modified take it when they write the image's own
  bytes (`text_guard_note_write` would not mark the page).
- **Renderer VRAM reads** (`gpu_vram_peek`): `gpu.c` keeps the 16x16 cells the
  GPU may have drawn since the last read through the renderer (drawing area of
  each drawing command, fill and copy destinations); elsewhere its own array
  holds the same pixels and is read without a render-thread sync. A title that
  polled VRAM every VBlank waited for the whole render queue each time. TCP
  `vram_drawn_check` proves the cells (pixels outside them that a renderer read
  changes: 0 in every scene tried).
- **Dispatcher**: the second range validation of the same entry, right after
  the first with no store, charged cycle or code change between, is answered
  from the first.
- Debug builds: the per-function-entry recorders return at once when nothing
  records (passive policy, cyc_watch, call focus, function trace).
- `PSX_FAST_FORWARD_SPEED` now wins over a title's own turbo speed (measuring
  with "max": a title cap of 8x had hidden the field's real speed).
- TCP `core_rates`: per-event work counters (device services, MMIO syncs and
  register accesses by region, stores by path, dispatches, I-cache misses).

Measured on BoF4 (16:9, 8x, turbo uncapped), PGO + ThinLTO with the same
profile, against the build before: barco +15-29%, battle +29-35%, dungeon
+24-30%, magic +34-40%, field 7.8x (held at the title's 8x cap) -> 13.3x.

Cycle 6 (2026-10-28), the same checks (frame fingerprints for 630 frames in 8
scenes with effects on, sound, pictures, cold boot through the opening movie):

- **SPU voice by voice** (`spu.c`): `spu_render_block` mixed frame by frame,
  every voice in each frame. Within a frame the voices touch only their own
  state, except through SPU RAM: the frame writes the capture buffers
  (0x000-0xFFF) and, with SPUCNT.7, the reverb work area, and a block fetch
  reads SPU RAM. When the IRQ compare is off and no active voice can fetch from
  those areas during the block (`spu_voice_reads_written` walks each voice's
  fetches exactly: the headers it reads lie outside the written areas), each
  voice runs the block on its own in slices of 128 frames, leaving its
  contribution per frame, and the frame loop mixes those as before. A voice's
  registers are read once per block (no guest access can land inside a render).
  The noise LFSR is replayed ahead. Only the debug SPU event ring sees a
  different order inside a block. TCP `core_rates` `spu_blocks*`.
- SPU: the SPUCNT.6 test of `spu_irq_check` inline, the reverb work-area
  address without a 64-bit division, direct-mode volume sweeps tested inline.
- **Cycle charge gate** (`psx_edge.h`, `g_psx_cyc_gate`): the emitted charge
  read five mode flags; four (guest cycle scale, lockstep replay, conservative
  stepping, device service) are now one word kept by their writers
  (`psx_cyc_gate_refresh`); any other case calls `psx_cyc_charge` as before.
  TCP `core_rates` `cyc_gate_stale` counts a service finding it stale (0).
- **Block-mode loads inline** (`psx_edge.h`): `psx_blk_load_*` and
  `psx_blk_lwc2_read` with the same main RAM and scratchpad paths, forced
  inline (the large generated functions kept them out of line).
- **Game text validation by 64-byte line** (`memory.c`): a range on a page
  marked modified or dirty was compared whole on every dispatch. BoF4 has one
  large function on pages it shares with data written every frame: ~800 KB
  compared per frame in battle. A line bitmap now marks the lines that may
  differ from the reference (a guarded write that differs, a plain store to a
  modified page, a page or range marked dirty, a blessed patch; recomputed from
  the bytes wherever the page marks are reset) and only those are compared; a
  line found equal is unmarked. Battle: 160 bytes per frame. TCP `core_rates`
  `text_cmp`, `text_cmp_bytes`.

All in files outside the codegen hash and the execution identity: no re-stamp,
the overlay cache stays valid, overlay DLLs unchanged. Measured on BoF4 (16:9,
8x, turbo uncapped, PGO + ThinLTO each with its own profile, 3 rounds) against
cycle 5: battle +5-10%, barco +3-10%, dungeon +3-7%, magic +4-8%, field
-2..+8%; with effects on +1-6%.

## Measured on Breath of Fire IV (2026-10-26)

Turbo speed of the emulation thread (median of 2 alternated rounds; campo and
batalha sit at the turbo's 4x ceiling):

| Scene | default | ram_fast_path | + pgxp_alu "moves" | + cpu_timing "block" (1.0/3) |
|---|---|---|---|---|
| dungeon | 1.80x | +2.8% | +17.5% | +33% |
| night field | 1.89x | -2.0% | +13.7% | +35% |
| magic | 2.73x | +0.8% | +10.9% | +30% |

- `ram_fast_path`: main RAM and scratchpad identical byte for byte at 34 points
  in 6 scenes against the default build, with the fast path live (debug
  recorders off); frame fingerprints identical in 8 scenes (recorders on).
- `pgxp_alu = "moves"`: frame fingerprints identical (PGXP never touches guest
  state), PGXP coverage identical in every scene (97.7-99.7% precise vertices),
  pictures identical except where the shots caught a spinning cursor at
  different angles.
- `cpu_timing = "block"` with calibrated constants: rejected for BoF4 (above).
  The per-block pipeline cost: see "Measured on Breath of Fire IV, cycle 4".

## Measured on Breath of Fire IV, cycle 4 (2026-10-26)

BoF4 now runs `cpu_timing = "block"` (the per-block pipeline cost) and
`pgxp_mem = "words"`, with `overlay_precompile` and the exact host work above.

- Block timing against the pipeline, 8 scenes with every effect on: the same
  30 displayed frames per second, the same busy fraction per frame (within 0.2
  of a point), PGXP coverage unchanged, pictures unchanged, sound correlated
  0.999-1.000 (a few effects start a few samples apart), the cold boot through
  the opening movie identical picture for picture. The codegen hash does not
  move (the mode is in the overlay config hash): savestates load unchanged.
- `pgxp_mem = "words"`: PGXP coverage unchanged (97.7-99.7% precise vertices).
- Turbo, PGO + ThinLTO, the same compiled-overlay coverage on both sides:

| Scene | pipeline + exact work | + block timing + PGXP words |
|---|---|---|
| dungeon | 2.55x | 3.15x (+23%) |
| night field | 2.55x | 3.2x (+25%) |
| magic | 3.55x | 4.3x (+21%) |
| battle | 4.7x | 5.3-5.4x (+14%) |
| field | 4.65x | 5.45x (+17%) |

  Without the present skip, battle and field were held at the old 4x ceiling.
  The exact host work alone, with PGO, is within noise in the heavy scenes (PGO
  already optimized those paths); without PGO it measured 8-10%.
- After a codegen-hash or overlay-config change the overlay cache starts again:
  a region runs in the dirty-RAM interpreter until a capture of it is compiled
  (`overlay_precompile` compiles the ones in the current capture list at boot).

## Checking a mode

Against the same title built with the default path, from the same savestates:

- exact modes: the debug server's frame fingerprints (cycles and hashes of
  every RAM/MMIO/scratchpad write per frame) identical for hundreds of frames
  in several scenes, the same pictures and sound. The fingerprints need the
  debug recorders, which turn `ram_fast_path`'s store path off; so also
  compare main RAM and the scratchpad with the recorders off
  (`PSX_DEBUG_PASSIVE=0`), each exe paused (`host_pause`) at the same values of
  the game's VBlank counter;
- `pgxp_alu`: the PGXP coverage counters (`geom_correction`) and pictures at
  the same game moment;
- `cpu_timing`: the busy fraction per frame, the game's displayed frames per
  second, sound, a cold boot through the opening movie, and play-through of the
  title's timing-sensitive parts;
- `pgxp_mem`: as `pgxp_alu`.

Pictures with effects on vary between two runs of the same exe in some scenes
(an animated element, the clock): compare against a run of the reference exe
before blaming a mode.
