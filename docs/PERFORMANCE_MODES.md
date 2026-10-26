# Performance modes (opt-in, per game)

The default path is the faithful one (CLAUDE.md Rule -1): the R3000A pipeline
timing per instruction, every guest store through the full memory path, every
written register's PGXP shadow updated. Three options trade some of that for
host speed. Each is **off by default**; a title turns one on in its `game.toml`
only after checking it (see "Checking a mode" below). A game that misbehaves
with one simply leaves it off.

| Key | Values | Exact? | Needs regen | What it removes |
|---|---|---|---|---|
| `[recompiler] ram_fast_path` | `false` (default) / `true` | yes | yes | the full memory path for plain main-RAM stores and byte loads |
| `[video] pgxp_alu` | `"full"` (default) / `"moves"` | no (sub-pixel only) | no (runtime) | the PGXP engine call on most ALU instructions |
| `[recompiler] cpu_timing` | `"pipeline"` (default) / `"block"` | no | yes | the per-instruction pipeline/I-cache timing model |

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

## `[recompiler] cpu_timing = "block"` (not exact)

The emitter skips the per-instruction steps (load-delay interlock, I-cache
fetches) and charges each block `n x cpu_timing_insn` cycles at its head
(mid-block entries charge the rest of the block), batched like the
per-instruction charges. A main-RAM load adds `cpu_timing_ram_load`
(`psx_blk_load_*`); scratchpad and device loads keep the exact load, and
mult/div, GTE and device timing are unchanged. The guest sees a slightly
different clock inside each frame, so the two constants are calibrated per
title against the pipeline mode (the fraction of each frame the game spends
outside its VSync wait, scene by scene). Titles whose logic depends on CPU
timing inside a frame (busy-wait polling with tight windows, cycle-counted
effects) should stay on `"pipeline"`.

### Lesson from Breath of Fire IV: iteration-count timeouts

PsyQ's `VSync(n)` waits in a loop with an iteration-count timeout
(`(n-1) << 15` iterations: 32768 for `VSync(2)`). Under the pipeline model the
loop costs ~32 cycles per iteration (three dependent main-RAM loads), so the
timeout is ~1.85 VBlanks away. A block-mode load cost of 0..1 cycle makes the
loop 14..17 cycles: the timeout fires before the VBlank arrives, VSync returns
early and the game runs above its normal rate (BoF4: 40-45 displayed frames
per second where the hardware shows 30). Too high a cost tips the heavy scenes
(BoF4's night field at 93% of the frame) into 20 fps. No single pair of
constants matched every scene of BoF4 closely enough, so BoF4 stays on
`"pipeline"`. Any title using PsyQ VSync (most of them) needs the loop's cost
per iteration near the pipeline's; check `display_flips` (TCP `perf_summary`)
in every scene, not only the busy fraction.

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
- `cpu_timing = "block"`: rejected for BoF4 (above).

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
  title's timing-sensitive parts.
