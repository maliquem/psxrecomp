/* gpu.c — PS1 GPU hardware simulation (Phase 3, Step 1).
 *
 * Implements:
 *   - GPUSTAT register with correct bit semantics
 *   - GP1 commands 00h-08h (reset, display config)
 *   - VRAM storage (1024x512 x 16-bit)
 *   - GP0 command write — ABORTS (not yet implemented)
 *   - GPUREAD — returns last latched value
 *
 * Reference: nocash PSX specs, DuckStation src/core/gpu.cpp
 */

#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "mod_texture_banks.h"
#include "display_scanout.h"
#include "pgxp.h"
#include "mod_memory.h"
#include "psx_memory.h"
#include "gpu_primitive_reject.h"
#include "gpu_sw_renderer.h"
#include "gpu_vram_dirty.h"
#include "gpu_render.h"
#include "ws_view_anchor.h"
#include "text_xlate.h"
#include "crash_trace.h"
#include "debug_server.h"
#include "cpu_state.h"
#include "event_ring.h"
#include "psx_video_timing.h"
#include "color_lut.h"
#include "mod_runtime.h"
#include "mod_plugins.h"
#include "ws_scene_hold.h"
#include "sio.h"
#include "ws_cull_detect.h"
#include "ws_cull_edge.h"
#include "ws_backdrop_margin.h"
#include "ws_aspect_cone_math.h"
#include "ws_ui_group.h"
#include "ws_primitive_roles.h"
#include "ws_scene_latch.h"
#include "ws_backdrop_extent.h"
#include "ws_prepass_guard.h"
#include "ws_hud_anchor.h"
#include "ws_repeat_rect.h"
#include "ws_screen_mask.h"
#include "ws_radial_screen_mask.h"
#include <stdint.h>
#include <math.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_timeline.h"

/* Packet identity of a primitive/OT address: main RAM through the live
 * geometry (retail: the DMAC's 0x1FFFFC fold), the enhancement aperture by its
 * own address (mod_memory.h). */
#define GPU_RAM_KEY(a) psx_gpu_packet_key(a)

extern uint16_t psx_read_half(uint32_t addr);
extern uint8_t  psx_read_byte(uint32_t addr);
extern uint32_t psx_read_word(uint32_t addr);

/* ---- VRAM ---- */
static uint16_t vram[1024 * 512];

/* ---- GP0 state machine ---- */

typedef enum {
    GP0_IDLE,
    GP0_COLLECTING,
    GP0_VRAM_WRITE,
    GP0_POLYLINE_MONO,    /* collecting mono polyline vertices */
    GP0_POLYLINE_SHADED   /* collecting shaded polyline color+vertex pairs */
} Gp0State;

static Gp0State gp0_state;
static uint64_t gp0_write_count;
static uint64_t gp0_nop_count, gp0_fill_count, gp0_draw_count, gp0_env_count, gp0_copy_count;
static uint32_t gp0_cmd_buf[16];   /* max fixed-length command is 12 words */
static int      gp0_words_collected;
static int      gp0_words_needed;
static uint32_t gp0_next_source_addr = 0xFFFFFFFFu;
static uint32_t gp0_cmd_source_addr  = 0xFFFFFFFFu;
static uint16_t gp0_ot_rank = 0xFFFFu;

/* ---- Widescreen proportion correction --------------------------------------
 * Active only when [video] aspect_ratio != 4:3 AND the game's [widescreen]
 * block opts in (see config_loader.h). Two mechanisms, both driven from the
 * game.toml so nothing here is game-specific:
 *
 *  1. Tagged character/billboard prims. The recompiler emits a
 *     psx_ws_sprite_tag(cpu) call at the entry of each configured
 *     sprite-tag function ($a0 = prim pointer, scratchpad holds the prim's
 *     GTE-projected anchor). The game computes the prim's screen-pixel
 *     offsets/widths itself — which our GTE X-squash never sees — so tagged
 *     prims get every X re-squashed around their own anchor at execution
 *     time, restoring correct proportions on the stretched present.
 *
 *  2. Untagged textured rects (SPRT family). These never went through the
 *     GTE at all (HUD, menus — pure screen space), so they're squashed
 *     around the display centre, presenting at native proportions.
 *     Untextured TILEs are never touched: full-screen fades/flashes must
 *     keep covering the whole frame.
 *
 * The tag table is keyed by the prim's guest address (the DMA linked-list
 * walk reports each word's source address) and stamped with the frame
 * counter, so stale entries age out without an explicit clear. */
static int32_t  ws_xnum = 1, ws_xden = 1;   /* X squash factor; 1/1 = off */
static uint32_t ws_anchor_addr = 0;          /* scratchpad addr of anchor SXY */
static int      ws_hud_sprt = 0;             /* edge-anchor untagged HUD SPRTs */
static int      ws_auto_ui_squash;
static int      ws_auto_ui_in_place;
static int      ws_auto_ui_dense;
static int      ws_active(void);
static int ws_local_viewport_cfg = 0;
/* [netplay] local_viewport_renderer = "native_wide": split frames are always
 * world frames for the per-camera wide surfaces. Projection views keep the
 * title's own scene classification. */
static int ws_local_viewport_native = 0;
static int ws_vertical_split_active(void);
static int ws_local_native_split(void) {
    return ws_local_viewport_cfg && ws_local_viewport_native && ws_vertical_split_active();
}
static int      gp0_command_word_count(uint8_t opcode);
static uint64_t ws_auto_ui_candidate_count;
static uint64_t ws_auto_ui_transform_count;
#define WS_UI_PREPASS_MAX 2048u
typedef struct {
    WsUiGroupItem group;
    uint32_t src_addr;
    uint16_t ot_rank;
    /* Diagnostic only (ws_ui_groups): the key is a hash, so a dump of keys
     * alone says two prims differ without saying WHICH component differed.
     * Keep the raw inputs so an observer can attribute a split to the CLUT,
     * the texpage, the 24px Y band, or the poly-vs-rect family. */
    int32_t  y;
    int32_t  h;
    uint8_t  op;
    uint8_t  full_draw_area;
    uint8_t  shared_split_ui;
    /* A triangle or a rotated quad: UI only as part of a widget it lies
     * inside (ws_ui_prepass_admit_enclosed), never on its own. */
    uint8_t  enclosed_only;
    /* Vertical anchor of the item's run (auto_ui_size proportional): the
     * run's top edge in the upper half of the display, its bottom edge in
     * the lower half, so a shrunken widget keeps hugging its screen edge. */
    int32_t  anchor_y;
    WsPrepassPacketGuard packet_guard;
} WsUiPrepassItem;
static WsUiPrepassItem ws_ui_prepass[WS_UI_PREPASS_MAX];
static uint32_t ws_ui_prepass_count;
static uint16_t ws_ui_prepass_rank = 0xFFFFu;
/* Shared HUD packets execute once in each half. Keep the original packet
 * intact for provenance guards, textures, diagnostics and guest RAM. */
static int ws_shared_ui_copy = -1;

#define WS_UI_PREPASS_NODE_MAX 8192u
typedef struct {
    uint32_t addr;
    uint32_t header;
    WsPrepassPacketGuard payload_guard;
} WsUiPrepassNode;
static WsUiPrepassNode ws_ui_prepass_nodes[WS_UI_PREPASS_NODE_MAX];
/* Why the last prepass was discarded (ws_ui_groups "stale_why"): 1 packet
 * guard at draw, 2 DMA node not in the prepass, 3 node header changed,
 * 4 node word count changed, 5 node payload changed. */
static uint32_t ws_ui_stale_why[6];
static uint32_t ws_ui_stale_addr;
static uint32_t ws_ui_prepass_node_count;
/* The DMA walk validates every node it visits against the prepass record:
 * thousands of nodes per list, so the lookup is a binary search over an
 * address-sorted index, built on the first lookup after the record changes
 * (generation), never a scan of the whole record per node. */
static uint16_t ws_ui_prepass_order[WS_UI_PREPASS_NODE_MAX];
static uint32_t ws_ui_prepass_gen = 1, ws_ui_prepass_order_gen;
static uint32_t ws_ui_prepass_order_count;

static void ws_ui_prepass_nodes_reset(void) {
    ws_ui_prepass_node_count = 0;
    ws_ui_prepass_gen++;
}

/* Why a UI-looking primitive did NOT reach the squash partition.
 *
 * Every rejection here leaves a primitive at its raw 4:3 X while the cluster
 * around it is squashed toward an anchor -- which is precisely how a HUD mark
 * ends up stranded in open screen at a wide aspect. The gates are silent
 * `return`s, so a stranded primitive is indistinguishable from one that was
 * never drawn at all. These counters let ws_ui_groups name the responsible gate
 * from a capture instead of it being guessed at. Diagnostic only; nothing in
 * the transform path reads them. */
static struct {
    uint32_t opcode;      /* not a 4-vertex polygon or rectangle           */
    uint32_t not_axis;    /* quad, but not axis-aligned                    */
    uint32_t degenerate;  /* zero or negative extent                       */
    uint32_t too_big;     /* full-screen or large-primitive reject         */
    uint32_t cap;         /* WS_UI_PREPASS_MAX reached                     */
    uint32_t rank;        /* admitted, then dropped by the max_rank filter */
    uint32_t stale;       /* live packet no longer matches cached bytes    */
    uint32_t backing;     /* NOT a reject: lower-rank backing panels kept  */
    uint32_t enclosed;    /* NOT a reject: needles/hands inside a widget   */
} ws_ui_reject;

/* Geometry of the primitives the max_rank filter discarded. A count alone
 * cannot say whether those are stray world geometry (fine to drop) or HUD
 * marks belonging to a cluster that IS being squashed (not fine -- they are
 * left behind at their 4:3 X). Small fixed ring; the filter typically drops a
 * handful. */
#define WS_UI_RANKDROP_MAX 8
static struct { int32_t x, y, w, h; uint16_t rank; uint8_t op; }
    ws_ui_rankdrop[WS_UI_RANKDROP_MAX];
static uint32_t ws_ui_rankdrop_count;

/* Wide-aspect mode: 0 = off (4:3 identity), 1 = squash (legacy hack — compress
 * a wider FOV into the 320 frame, present stretched), 2 = native-wide (render
 * the wider FOV into an actually wider frame and present 1:1; the GTE is NOT
 * squashed — the frame is widened via draw_offset/draw_area and the display
 * read). ws_cfg_num/den hold the configured aspect that drives the wide extent
 * in native-wide mode. */
static int      ws_mode    = 0;
static int      ws_cfg_num = 4, ws_cfg_den = 3;
static void ws_nw_sync_target(void);

#define WS_TAG_BUCKETS 4096                  /* power of two */
#define WS_TAG_PROBES  8
#define WS_FMV_HYSTERESIS 30                 /* frames a colour MDEC decode pins 4:3 */
typedef struct {
    uint32_t key, stamp;
    int32_t anchor_x, repeat_period, source_width;
    int screen_space;
    WsPrepassPacketGuard packet_guard;
    int32_t stretch_left, stretch_right, left_anchor, right_anchor;
} WsTag;
static WsTag    ws_tags[WS_TAG_BUCKETS];
static WsHudAnchorTag ws_hud_anchor_tags[WS_HUD_ANCHOR_TABLE_SIZE];
static WsHudAnchorTag ws_background_tags[WS_HUD_ANCHOR_TABLE_SIZE];
static uint32_t ws_background_tag_frame;
static int ws_background_tags_used;
static WsHudAnchorTag ws_reveal_clear_tags[WS_HUD_ANCHOR_TABLE_SIZE];
static WsHudAnchorTag ws_screen_mask_tags[WS_HUD_ANCHOR_TABLE_SIZE];
static WsRadialScreenMaskTag ws_radial_screen_mask_tags[WS_RADIAL_MASK_TAG_COUNT];
static WsRepeatRectTag ws_repeat_rect_tags[WS_REPEAT_RECT_TAG_TABLE_SIZE];
static uint32_t ws_last_tag_stamp = (uint32_t)-1000; /* frame of newest tag */
static uint32_t ws_last_3d_stamp  = (uint32_t)-1000; /* frame of newest shaded prim (diagnostic) */
static uint32_t ws_phase_stretch_stamp = (uint32_t)-1000; /* frame of newest stretched phase backdrop */
static uint32_t ws_prim_pred_stamp = (uint32_t)-1000;     /* ... of a polygon the title's predicate stretched */
static int (*s_ws_wide_prim_predicate)(uint32_t op, uint32_t ot_rank, uint32_t flags);
static int ws_nw_phase_backdrop;   /* [widescreen] nw_phase_backdrop (set below) */
extern uint64_t s_frame_count;               /* defined in debug_server.c */
extern int      mdec_recently_active(uint32_t within_frames);  /* mdec.c */

static int ws_configured(void) { return ws_xnum != ws_xden; }

/* Any wide mode engaged (squash or native-wide). Drives the FMV/menu 4:3
 * pillarbox + the game-vs-2D detector, which both modes share. */
static int ws_engaged(void) { return ws_mode != 0; }

/* Forward decls: defined later but used by psx_ws_backdrop_x above them. */
static int32_t ws_scale_about(int32_t x, int32_t ax);
static int32_t ws_disp_x(void);
static int32_t ws_disp_w(void);
static void prim_sx_extent(uint8_t op, int32_t *xmin, int32_t *xmax);
static int32_t ws_disp_h(void);
static void ws_clear_all_reveal_margins(void);

/* Gameplay vs full-2D screen. Character/billboard prims tag (psx_ws_sprite_tag)
 * within the last couple of frames => the actor render funnel is running =>
 * gameplay. A menu/title/save screen draws no tagged prims. The 2-frame window
 * makes this frame-stable regardless of per-prim ordering within a frame. */
/* Full-2D widescreen ([widescreen] full_2d). A pure 2D sprite game (e.g. MMX6)
 * never emits the sprite-tag hook the 3D gameplay detector keys on, so every
 * in-game frame would be misclassified as a "full-2D menu" and pillarboxed 4:3.
 * When this is set, treat every in-game frame as gameplay so native-wide engages
 * and the 2D scene presents widescreen. Set once from game.toml at widescreen-
 * engage (gpu_ws_set_full_2d); PSX_WS_FORCE_2D=1 forces it on for testing. */
static int ws_full_2d = 0;
void gpu_ws_set_full_2d(int on) { ws_full_2d = on ? 1 : 0; }
void gpu_ws_set_auto_ui_in_place(int on) {
    ws_auto_ui_in_place = on ? 1 : 0;
}
/* [widescreen] auto_ui_size (gpu.c ws_auto_ui_size). */
static int ws_auto_ui_proportional = 0;
void gpu_ws_set_auto_ui_proportional(int on) {
    ws_auto_ui_proportional = on ? 1 : 0;
}

void gpu_ws_set_auto_ui_squash(int on) {
    ws_auto_ui_squash = on ? 1 : 0;
    ws_ui_prepass_count = 0;
    ws_ui_prepass_nodes_reset();
    ws_ui_prepass_rank = 0xFFFFu;
    ws_auto_ui_dense = 0;
    ws_auto_ui_candidate_count = 0;
    ws_auto_ui_transform_count = 0;
}
static int ws_clear_reveal = 0;
static int g_mmx6_void_sides = 0;
static uint32_t g_mmx6_void_generation = 1;
void gpu_ws_set_clear_reveal(int on) { ws_clear_reveal = on ? 1 : 0; }

/* GTE-activity gameplay detector ([widescreen] gte_game_mode) — the generic
 * 3D-title analog of the sprite-tag stamp. A fully-3D game (e.g. Ape Escape)
 * has no per-prim tag helper to hook, but every gameplay frame projects a
 * meaningful number of vertices through RTPS/RTPT, while a full-2D screen
 * (save/options/memory-card) projects none (or a token few). gte.cpp notes
 * every projection here; when a frame accumulates >= the threshold the frame
 * is stamped as gameplay, with the same 2-frame hysteresis the tag path uses.
 * Off unless the game opts in, so existing titles are untouched. */
static int      ws_gte_game_mode_cfg = 0;
static uint32_t ws_gte_frame = (uint32_t)-1;
static uint32_t ws_gte_count = 0;
static uint32_t ws_last_gte_stamp = (uint32_t)-1000;
static int ws_precise_nclip_cfg = 0;
#define WS_GAMEPLAY_STATE_VALUES_MAX 16
static uint32_t ws_gameplay_state_addr = 0;
static uint32_t ws_gameplay_state_values[WS_GAMEPLAY_STATE_VALUES_MAX];
static int ws_gameplay_state_value_count = 0;
/* Any frame that projects a handful of vertices is "3D" (a low threshold so a
 * sparse close-up cutscene frame still counts — the flicker was frames dipping
 * below a high 16-vert bar and pillarboxing for a frame or two). */
#define WS_GTE_GAME_MODE_MIN_VERTS 3u
/* STICKY: stay in native-wide for ~0.75s after the last 3D frame, so brief
 * low-poly frames in a real-time 3D cutscene never flip to a 4:3 pillarbox (the
 * intro-cutscene flicker). Only a genuine full-2D screen — no GTE projection for
 * this many consecutive frames (save/options/memory-card) — reverts to 4:3. */
#define WS_GTE_GAME_MODE_HYSTERESIS 45u
void gpu_ws_set_gte_game_mode(int on) { ws_gte_game_mode_cfg = on ? 1 : 0; }
void gpu_pgxp_rederive_enable(void);
void gpu_ws_set_precise_nclip(int on) {
    ws_precise_nclip_cfg = on ? 1 : 0;
    /* Exact NCLIP consumes PGXP transport shadows even when every visual
     * PGXP correction is disabled. Re-derive the internal transport arm. */
    gpu_pgxp_rederive_enable();
}
/* Same "reveals extra world" test the exact-NCLIP branch consumers use
 * (psx_ws_x_margin() > 0): squash AND native-wide. ws_active() alone is the
 * squash factor, which native-wide never sets, so the producer was dead there. */
int gpu_ws_precise_nclip_enabled(void) {
    return ws_precise_nclip_cfg && psx_ws_x_margin() > 0;
}
void gpu_ws_set_gameplay_state_gate(uint32_t addr,
                                    const uint32_t *values, int nvalues) {
    if (nvalues < 0) nvalues = 0;
    if (nvalues > WS_GAMEPLAY_STATE_VALUES_MAX)
        nvalues = WS_GAMEPLAY_STATE_VALUES_MAX;
    ws_gameplay_state_addr = addr;
    ws_gameplay_state_value_count = nvalues;
    for (int i = 0; i < nvalues; i++)
        ws_gameplay_state_values[i] = values[i];
}

static int ws_gameplay_state_matches(void) {
    if (!ws_gameplay_state_addr || ws_gameplay_state_value_count == 0)
        return -1;
    uint32_t state = psx_read_word(ws_gameplay_state_addr);
    for (int i = 0; i < ws_gameplay_state_value_count; i++)
        if (state == ws_gameplay_state_values[i]) return 1;
    return 0;
}

/* World-scale 3D signal for the 2D-only-scene classifier (sprite-tag titles).
 * Shaded-prim presence proved to be a FALSE world signal: task-clear /
 * new-task title cards are gouraud-shaded screen-space tiles animated over
 * many consecutive frames, which defeated both the shaded test and its
 * sustained guard (room re-stretched whenever a card played). Projection
 * volume can't be fooled that way: a real 3D world projects hundreds of
 * verts per frame through RTPS/RTPT, a room interior projects only its
 * character anchors (a handful), and screen-space overlays project ZERO.
 * Sustained rule as elsewhere: 2+ consecutive frames over threshold, so an
 * isolated projection burst can't flip a 2D scene wide. */
#define WS_WORLD3D_MIN_VERTS 48u
static uint32_t ws_gte_prev_verts     = 0;  /* final count of last completed frame */
static uint32_t ws_last_world3d_stamp = (uint32_t)-1000;
static uint32_t ws_sust_world3d_stamp = (uint32_t)-1000;

/* Natural-overhang world signal — the classifier's actual input. GTE volume
 * above proved to be a SECOND false world signal: the task-found/task-clear
 * jingle projects world-scale vert counts inside a room (stamped world3d at
 * ≥48 while the room idles at 4). What can't be faked is CONTENT: a real 3D
 * scene always submits polygons whose raw vertex X extent crosses outside
 * the canonical display window (that overhang is the very content native-
 * wide reveals — the working outdoor reveal proves it exists), while rooms,
 * dialogs, HUDs and jingle popups compose everything inside it. Polygons
 * only (sprites/rects slide in from edges routinely — the task icon enters
 * from the right edge), ≥3 prims per frame, sustained 2+ consecutive
 * frames. Counted at gp0_execute_command via prim_sx_extent (raw SX,
 * pre-draw_offset, so our native-wide offset injection can't feed back). */
/* Depth qualifier: only polys crossing DEEP past the canonical edge count.
 * Census-measured (SCUS-94236): outdoor terrain = 55/frame at >=24px (depths
 * to 128px); the task-jingle sparkles poke <=20px and its sliding icon is a
 * single prim at 34px — so >=24px depth + >=4 prims excludes every observed
 * screen-edge effect with ~14x headroom to the real world's count. */
#define WS_OVERHANG_DEEP_PX   24
#define WS_OVERHANG_MIN_PRIMS 4u
static uint32_t ws_ovh_frame      = (uint32_t)-1;
static uint32_t ws_ovh_count      = 0;
static uint32_t ws_ovh_prev       = 0;  /* final count of last completed frame */
static uint32_t ws_last_ovh_stamp = (uint32_t)-1000;
static uint32_t ws_sust_ovh_stamp = (uint32_t)-1000;

static void shadow_note_projection(int nverts);   /* below */
void psx_ws_note_gte_project(int nverts) {
    shadow_note_projection(nverts);
    uint32_t f = (uint32_t)s_frame_count;
    if (f != ws_gte_frame) {
        if (f == ws_gte_frame + 1u) ws_gte_prev_verts = ws_gte_count;
        else                        ws_gte_prev_verts = 0;
        ws_gte_frame = f; ws_gte_count = 0;
    }
    ws_gte_count += (uint32_t)nverts;
    if (ws_gte_game_mode_cfg && ws_gte_count >= WS_GTE_GAME_MODE_MIN_VERTS)
        ws_last_gte_stamp = f;
    if (ws_gte_count >= WS_WORLD3D_MIN_VERTS && ws_last_world3d_stamp != f) {
        if (f == ws_last_world3d_stamp + 1u) ws_sust_world3d_stamp = f;
        ws_last_world3d_stamp = f;
    }
}

/* Full-2D tile-engine mode (MMX6): config [widescreen] full_2d, or the PSX_WS_FORCE_2D
 * test override. Distinct from ws_game_mode (which also fires on the 3D sprite-tag path,
 * e.g. Tomba) — only true full-2D games get the BG tile-budget reveal cap. */
static int ws_full_2d_mode(void) {
    static int env = -1;
    if (env < 0) { const char *e = getenv("PSX_WS_FORCE_2D"); env = (e && e[0] == '1') ? 1 : 0; }
    return ws_full_2d || env;
}
static PSXModWorldScenePredicate s_ws_world_scene_predicate;
void psx_mod_set_world_scene_predicate(PSXModWorldScenePredicate predicate) {
    s_ws_world_scene_predicate = predicate;
}
static int ws_mod_world_scene(void) {
    return ws_mode == 2 && s_ws_world_scene_predicate && s_ws_world_scene_predicate();
}
static int ws_game_mode(void) {
    if (ws_mod_world_scene()) return 1;
    if (ws_local_native_split()) return 1;
    int state_match = ws_gameplay_state_matches();
    if (state_match >= 0) return state_match;
    if (ws_full_2d_mode()) return 1;
    if (ws_gte_game_mode_cfg &&
        (uint32_t)s_frame_count - ws_last_gte_stamp <= WS_GTE_GAME_MODE_HYSTERESIS) return 1;
    return (uint32_t)s_frame_count - ws_last_tag_stamp <= 2;
}

/* True when the current frame is presented at native 4:3 (NOT stretched), so
 * ALL squash must be suppressed and the content rendered pixel-native:
 *   - FMV video (24-bit, or streamed 15-bit colour MDEC), and
 *   - full-2D screens (menus/title/save — no character billboards this frame).
 * Coupling squash to this exact predicate keeps content and present in lock-
 * step: we squash IFF we stretch. Per-prim center-squash mangles composite 2D
 * UI (dialog boxes built from tiled cap/middle pieces), so such screens get
 * zero squash + a 4:3 pillarbox instead. The FMV check is cached per frame;
 * game_mode is a cheap live check. */
/* 2D-only gameplay scenes (sprite-tag titles). A room interior or a sky-only
 * fall is gameplay-classified (the character prims tag every frame) but has
 * no world-scale GTE projection — there is no 3D world and nothing beyond
 * the canonical frame to reveal, so wide presents can only stretch flat art
 * (the backdrop stretch fires wholesale because the background phase never
 * ends). Present those scenes native 4:3 instead: the canonical buffer is
 * always faithful. The signal is natural polygon OVERHANG past the canonical
 * window (ws_sust_ovh_stamp) — content that would actually be revealed.
 * Shaded-prim presence and GTE projection volume both proved to be false
 * world signals (title-card letter tiles; the task-jingle's GTE effect) —
 * see the overhang block for the full lineage. Scoped to the tag-classified
 * (2.5D) mechanism: full-2D titles (MMX6) are 2D-only by definition and
 * genuinely reveal more, and GTE-detector titles (Ape) already classify 2D
 * screens by projection count. Once overhang confirms a world, active world
 * projection keeps it alive between terrain-submission bursts. A real 2D
 * scene with neither signal crosses the grace period in ~0.1 s. */
#define WS_2D_SCENE_HYSTERESIS 6u
static WsSceneLatch ws_scene_latch;
/* Finite 2D backdrop (ws_backdrop_extent.h): a full-screen pre-rendered image
 * that ends inside the native-wide reveal outranks polygon overhang. Widened
 * 2D actor culls place sprite quads past the canonical edge, so such a scene
 * passes the overhang test while its ground stops at the image edge (Tomba's
 * overhead village: a 384px image, fence/sign quads out to x=428). */
static WsBackdropExtent ws_bdx;
static int ws_2d_only_scene(void) {
    if (ws_mod_world_scene()) return 0;
    if (ws_local_native_split()) return 0;
    if (ws_full_2d_mode() || ws_gte_game_mode_cfg) return 0;
    uint32_t f = (uint32_t)s_frame_count;
    if (ws_bdx_veto(&ws_bdx, f, WS_2D_SCENE_HYSTERESIS)) {
        ws_scene_latch.confirmed = 0;   /* affirmatively 2D: relearn on exit */
        return 1;
    }
    return ws_scene_is_2d(&ws_scene_latch, f,
        f - ws_sust_ovh_stamp <= WS_2D_SCENE_HYSTERESIS,
        f - ws_last_world3d_stamp <= 2u, WS_2D_SCENE_HYSTERESIS);
}

/* Host-derived presentation evidence is not serialized guest state. Relearn it
 * after reset/load instead of carrying the old scene's classification over. */
static void ws_reset_scene_history(void) {
    uint32_t expired = (uint32_t)s_frame_count - 1000u;
    memset(&ws_scene_latch, 0, sizeof ws_scene_latch);
    ws_bdx_reset(&ws_bdx);
    ws_last_tag_stamp = ws_last_3d_stamp = ws_last_gte_stamp = expired;
    ws_phase_stretch_stamp = ws_prim_pred_stamp = expired;
    ws_last_world3d_stamp = ws_sust_world3d_stamp = expired;
    ws_last_ovh_stamp = ws_sust_ovh_stamp = expired;
    ws_gte_frame = ws_ovh_frame = (uint32_t)-1;
    ws_gte_count = ws_gte_prev_verts = ws_ovh_count = ws_ovh_prev = 0;
}

static uint32_t s_ws_fmv_frame_cache = 0xFFFFFFFFu;
static int      s_ws_fmv_cached = 0;
static PSXModRetainedScenePredicate s_ws_retained_scene_predicate;
static int (*s_ws_native_scene_predicate)(void);
static WsSceneHold s_ws_scene_hold;
static uint32_t ws_display_origin(void);

void psx_mod_set_retained_scene_predicate(PSXModRetainedScenePredicate predicate) {
    s_ws_retained_scene_predicate = predicate;
    ws_scene_hold_reset(&s_ws_scene_hold);
}

void gpu_ws_set_native_scene_predicate(int (*predicate)(void)) {
    s_ws_native_scene_predicate = predicate;
}

int gpu_ws_present_native_43(void) {
    if (!ws_engaged()) return 0;
    if (s_ws_native_scene_predicate && s_ws_native_scene_predicate())
        return 1;
    int game_mode = ws_game_mode();
    if (!game_mode) ws_scene_latch.confirmed = 0;
    int native_43 = !game_mode || ws_2d_only_scene();
    int hold_enabled = ws_mode == 2 && s_ws_retained_scene_predicate != NULL;
    if (!hold_enabled && native_43) return 1;      /* unchanged default */
    uint32_t f = (uint32_t)s_frame_count;
    if (f != s_ws_fmv_frame_cache) {
        s_ws_fmv_frame_cache = f;
        GpuDisplayInfo di; gpu_get_display_info(&di);
        s_ws_fmv_cached = di.depth24 || mdec_recently_active(WS_FMV_HYSTERESIS);
    }
    if (hold_enabled)
        return ws_scene_hold_classify(&s_ws_scene_hold,
            s_ws_retained_scene_predicate(), native_43, s_ws_fmv_cached,
            ws_display_origin());
    return s_ws_fmv_cached;
}

/* Squash applies only when configured AND the frame is being stretched. */
static int ws_active(void) { return ws_configured() && !gpu_ws_present_native_43(); }

/* ----- Native-wide (mode 2) ------------------------------------------------
 * Render the wider FOV into an actually wider frame instead of squashing it.
 * Engaged on game frames only (FMV/menu frames pillarbox 4:3 like squash). The
 * frame grows symmetrically: each side by OFFSET display-pixels, total by
 * EXTRA = 2*OFFSET. OFFSET = round(W*(3*num-4*den)/(8*den)) where W is the live
 * display width — derived from the aspect so it generalises past 16:9 and
 * tracks display-mode changes; EXTRA is forced even so OFFSET is integral
 * (e.g. W=320 @ 16:9 -> OFFSET=53, EXTRA=106, frame 320 -> 426). The shift is
 * applied via the GPU draw_offset (so 3D and 2D move together and stay
 * aligned), the draw-area widens by EXTRA so the shifted content is not clipped
 * on the right, and the present widens the display read by EXTRA. 0 when
 * native-wide is inactive (4:3 / boot / squash mode / FMV / full-2D). */
int ws_native_wide_active(void) {
    return ws_mode == 2 && !gpu_ws_present_native_43();
}
/* Cull/spawn setup often runs while a scene is loading, before the first GTE
 * frame can classify it as gameplay.  Keep that pre-render setup aware of the
 * configured native-wide viewport; presentation itself remains gated by
 * ws_native_wide_active(). */
static int ws_native_wide_configured(void) {
    return ws_mode == 2 && ws_cfg_num * 3 > ws_cfg_den * 4;
}


static int ws_local_viewport_slot = 0;
static int ws_local_viewport_draw_target(int *base_x);
static int ws_vertical_split_active(void);
#define WS_LOCAL_WIDTH_SITES_MAX 16
static uint32_t ws_local_width_sites[WS_LOCAL_WIDTH_SITES_MAX];
static int ws_local_width_count;
void gpu_ws_set_local_viewport_width_sites(const uint32_t *sites, int count) {
    ws_local_width_count = count < 0 ? 0 : count > WS_LOCAL_WIDTH_SITES_MAX ? WS_LOCAL_WIDTH_SITES_MAX : count;
    for (int i = 0; i < ws_local_width_count; ++i) ws_local_width_sites[i] = sites[i] & 0x1FFFFFFFu;
}
int psx_ws_is_local_viewport_width_site(uint32_t pc) {
    for (int i = 0; i < ws_local_width_count; ++i)
        if (ws_local_width_sites[i] == (pc & 0x1FFFFFFFu)) return 1;
    return 0;
}
int32_t psx_ws_local_viewport_width(int32_t vanilla) {
    GpuDisplayInfo di; gpu_get_display_info(&di);
    if (!ws_local_viewport_cfg || ws_mode != 1 || !ws_vertical_split_active() ||
        vanilla <= 0 || (int64_t)vanilla * 2 != (int)di.width) return vanilla;
    return (int32_t)(((int64_t)vanilla * ws_xden + ws_xnum - 1) / ws_xnum);
}
void gpu_ws_set_local_viewport_native_wide(int enabled) {
    ws_local_viewport_native = enabled ? 1 : 0;
}
void gpu_ws_set_netplay_local_viewport(int enabled, int slot) {
    ws_local_viewport_cfg = enabled ? 1 : 0;
    ws_local_viewport_slot = slot == 1 ? 1 : 0;
    ws_nw_sync_target();
}

static int ws_local_viewport_layout(int *base_x, int *source_w,
                                    int *wide_w, int *offset) {
    if (!ws_local_viewport_cfg || !ws_native_wide_configured() ||
        !ws_vertical_split_active())
        return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.depth24 || di.width < 2 || di.height == 0)
        return 0;
    if ((di.width & 1u) != 0)
        return 0;

    /* The full display spans 4:3 at whatever pixel aspect its mode has, so
     * the target keeps that pixel aspect: a 512-wide split (THPS2 2P) has
     * pixels 0.625 wide and needs 683 columns for 16:9, not the 427 a
     * square-pixel height*aspect gives (which stretched each half 1.6x).
     * For 4:3 modes (320x240, 640x480) both forms are identical. */
    int src_w = (int)di.width / 2;
    int64_t tw_num = (int64_t)di.width * 3 * ws_cfg_num;
    int64_t tw_den = (int64_t)4 * ws_cfg_den;
    int target_w = (int)((tw_num + tw_den / 2) / tw_den);
    if (target_w < src_w)
        target_w = src_w;
    int off = (target_w - src_w) / 2;
    int final_w = src_w + off * 2;

    if (base_x) *base_x = (int)di.display_x + (ws_local_viewport_slot ? src_w : 0);
    if (source_w) *source_w = src_w;
    if (wide_w) *wide_w = final_w;
    if (offset) *offset = off;
    return final_w > src_w && off > 0;
}

static int ws_nw_configured_offset(void) {
    if (!ws_native_wide_configured()) return 0;
    int local_offset = 0;
    if (ws_local_viewport_layout(NULL, NULL, NULL, &local_offset))
        return local_offset;
    int64_t numr = (int64_t)3 * ws_cfg_num - (int64_t)4 * ws_cfg_den;
    int w = (int)ws_disp_w();
    return (int)((w * numr + (int64_t)4 * ws_cfg_den) /
                 ((int64_t)8 * ws_cfg_den));
}
static int ws_nw_offset(void) {
    if (!ws_native_wide_active()) return 0;
    return ws_nw_configured_offset();
}
int gpu_ws_configured_x_reveal(void) { return ws_nw_configured_offset(); }
int ws_nw_extra(void) { return 2 * ws_nw_offset(); }

static uint32_t ws_view_camera_addr, ws_view_min_addr, ws_view_max_addr, ws_view_active_addr;
static int ws_view_bounds_override, ws_view_bounds_min, ws_view_bounds_max;
static WsViewAnchor ws_view;
static uint32_t ws_view_frame = UINT32_MAX;
void gpu_ws_set_view_anchor(uint32_t camera, uint32_t min, uint32_t max, uint32_t active) {
    ws_view_camera_addr = camera;
    ws_view_min_addr = min;
    ws_view_max_addr = max;
    ws_view_active_addr = active;
    ws_view_bounds_override = 0;
    ws_view_frame = UINT32_MAX;
    memset(&ws_view, 0, sizeof(ws_view));
    gr_wide_set_view(0, 0, 0, 0);
}
void gpu_ws_set_view_bounds_override(int enabled, int minimum, int maximum) {
    ws_view_bounds_override = enabled && minimum <= maximum;
    ws_view_bounds_min = minimum;
    ws_view_bounds_max = maximum;
}
static int ws_view_enabled(void) {
    return ws_view_camera_addr && ws_view_min_addr && ws_view_max_addr &&
           ws_view_active_addr && ws_native_wide_active() && ws_disp_w() <= 384;
}
static WsViewAnchor ws_view_current(void) {
    WsViewAnchor centered = {ws_nw_offset(), ws_nw_offset(), 0, 0, 0};
    if (!ws_view_enabled() || (uint32_t)s_frame_count - ws_view_frame > 1u)
        return centered;
    return ws_view;
}
/* Sample at the BG renderer setup, after guest camera clamp and before either
 * ring refill or packet generation. Intermediate camera writes are ignored. */
static void ws_view_sample(void) {
    if (!ws_view_enabled()) return;
    ws_view = ws_view_anchor(ws_nw_offset(),
        (int16_t)psx_read_half(ws_view_camera_addr),
        ws_view_bounds_override ? ws_view_bounds_min : (int16_t)psx_read_half(ws_view_min_addr),
        ws_view_bounds_override ? ws_view_bounds_max : (int16_t)psx_read_half(ws_view_max_addr));
    if (!psx_read_byte(ws_view_active_addr)) {
        ws_view.left = ws_view.right = ws_nw_offset();
        ws_view.shift = ws_view.pad_left = ws_view.pad_right = 0;
    }
    ws_view_frame = (uint32_t)s_frame_count;
}

int ws_nw_present_width(void) {
    int wide_w = 0;
    if (ws_local_viewport_layout(NULL, NULL, &wide_w, NULL))
        return wide_w;
    return (int)ws_disp_w() + ws_nw_extra();
}

int gpu_ws_netplay_local_viewport_base_x(void) {
    int base = 0;
    return ws_local_viewport_layout(&base, NULL, NULL, NULL) ? base : 0;
}

int gpu_ws_netplay_local_viewport_width(void) {
    int wide_w = 0;
    return ws_local_viewport_layout(NULL, NULL, &wide_w, NULL) ? wide_w : 0;
}

/* Per-side X cull margin in screen/world units (the game's draw classifier
 * works in objX-camX where 1 unit ~= 1 native-4:3 screen pixel). The squash
 * shows a half-view of 160/s pixels (s = squash factor = ws_xnum/ws_xden), so
 * the visible edge moves out by 160*(1/s - 1) = 160*(xden-xnum)/xnum. Widening
 * the cull window by this restores the original off-screen margin at the new
 * edge. 0 whenever squash is inactive (4:3/boot/menu/FMV) → original cull. */
/* Diagnostic override (8C): when >= 0, psx_ws_x_margin() returns this value
 * unconditionally so a probe can sweep the cull margin live (0 = force 4:3
 * cull while still stretching; large = over-draw) at a fixed camera position.
 * -1 = normal computed margin. */
static int ws_margin_override = -1;
static int ws_cull_guard_pixels = 0;
/* Overdraw (psx_mod_set_overdraw): screen px past each edge that the game's
 * per-primitive screen culls ([widescreen.cull] auto_screen_x and its paired
 * height compares, the explicit X sites) still keep, so geometry just off
 * screen is submitted; the drawing area clips it, so the picture is unchanged.
 * For effects that need what lies around the picture (shadow casters). Only
 * the cull compares move: native-wide placement, activation and every other
 * user of psx_ws_x_margin() keep their value. 0 = vanilla (default). */
static int ws_overdraw_x = 0, ws_overdraw_y = 0;
void gpu_set_overdraw(int x, int y) {
    ws_overdraw_x = x < 0 ? 0 : x > 256 ? 256 : x;
    ws_overdraw_y = y < 0 ? 0 : y > 256 ? 256 : y;
}
/* More of the band for part of a frame (psx_mod_set_overdraw_extra): while a
 * title's own routine draws casters it culls by itself near the edges (props:
 * a bush, a rock), x / y px past the overdraw, so one whose shadow reaches the
 * picture is drawn before it shows. Added only while the overdraw is on; the
 * title clears it after that routine. 0, 0 = none. */
static int ws_overdraw_ex = 0, ws_overdraw_ey = 0;
static unsigned ws_overdraw_extras = 0;   /* times one was asked for (TCP shadow_cache) */
/* the last extra asked for and when: the polygons it kept reach the GPU when
 * the game sends its ordering table, after that routine (often a VBlank later) */
static int ws_overdraw_lx = 0, ws_overdraw_ly = 0;
static uint32_t ws_overdraw_lframe = 0;
int gpu_set_overdraw_extra(int x, int y) {
    ws_overdraw_ex = x < 0 ? 0 : x > 256 ? 256 : x;
    ws_overdraw_ey = y < 0 ? 0 : y > 256 ? 256 : y;
    if (ws_overdraw_ex || ws_overdraw_ey) {
        ws_overdraw_extras++;
        ws_overdraw_lx = ws_overdraw_ex; ws_overdraw_ly = ws_overdraw_ey;
        ws_overdraw_lframe = (uint32_t)s_frame_count;
    }
    return ws_overdraw_x || ws_overdraw_y;
}
int psx_overdraw_x(void) { return ws_overdraw_x; }
int psx_overdraw_y(void) { return ws_overdraw_y; }
/* The share of the band in use, of 16 (psx_mod_set_overdraw_share): a title
 * that sees its game run out of frame time while drawing the band (more
 * polygons pass its culls) narrows it. 16 = the whole band (the default). */
static int ws_overdraw_share = 16;
void gpu_set_overdraw_share(int of16) { ws_overdraw_share = of16 < 0 ? 0 : of16 > 16 ? 16 : of16; }
static int ws_od_base(int axis) {
    return (axis ? ws_overdraw_y : ws_overdraw_x) * ws_overdraw_share / 16;
}
/* the band the culls keep now: the overdraw and, while it is on, the extra */
static int ws_od(int axis) {
    const int base = ws_od_base(axis);
    return (ws_overdraw_x || ws_overdraw_y) ? base + (axis ? ws_overdraw_ey : ws_overdraw_ex) : base;
}
int psx_overdraw_axis(int axis) { return ws_od(axis); }
static int ws_cull_xm(void) { return psx_ws_x_margin() + ws_od(0); }
/* the band the GPU keeps (draw_area_out_poly): the overdraw and the last extra
 * asked for in the last few VBlanks */
static int ws_od_gpu(int axis) {
    const int base = ws_od_base(axis);
    if (!ws_overdraw_x && !ws_overdraw_y) return base;
    if ((uint32_t)s_frame_count - ws_overdraw_lframe > 8u) return base;
    return base + (axis ? ws_overdraw_ly : ws_overdraw_lx);
}

/* [video.shadow]: while a post-effect chain reads `shadow` (the shadow map of
 * the scenery, gl_postfx.inc), each VBlank hands the renderer the game's
 * camera as the GTE holds it (rotation, translation, screen offset, H): a
 * proper rotation only; the renderer keeps the last three and uses the one
 * that places the frame on the scenery it remembers. A change in the
 * configured scene keys clears that memory. The overdraw guard band is on
 * meanwhile, so the casters just off the picture are drawn too. */
static int      sh_camera = 0, sh_od_x = 0, sh_od_y = 0, sh_nkeys = 0, sh_od_on = 0;
static uint32_t sh_key_addr[8], sh_key_size[8], sh_key_last = 0xFFFFFFFFu;
static uint32_t sh_fed = 0, sh_rejected = 0;   /* VBlank reads handed over / not a rotation */
static uint32_t sh_cam_taken = 0;                /* camera ram: matches with the GTE */
static uint32_t sh_anc_taken = 0, sh_blobs = 0;   /* sprite foot points; game blobs not drawn */
/* [video.shadow] hide_textures: the game's own blob shadows (a dark disc
 * under characters and objects), as texture page (9 bits) + CLUT (0xFFFF =
 * any: some games animate the shadow's palette) of the
 * textured polygons that draw them. While the shadow map is in use they are
 * not drawn (the polygon still sets the texture page, as the hardware does). */
static int      sh_hide_n = 0, sh_hide_on = 0;
static uint16_t sh_hide_tpage[16], sh_hide_clut[16];
static uint32_t sh_hidden = 0;
void gpu_set_hidden_textures(const uint32_t *tpage, const uint32_t *clut, int n) {
    sh_hide_n = n < 0 ? 0 : n > 16 ? 16 : n;
    for (int i = 0; i < sh_hide_n; i++) { sh_hide_tpage[i] = (uint16_t)(tpage[i] & 0x1FFu); sh_hide_clut[i] = (uint16_t)clut[i]; }
}
/* [video.shadow] off_textures: texture page (9 bits) + CLUT, either 0xFFFF =
 * any, of what only a screen with no place under the sun draws (a world
 * map's paper, its frame): while one was drawn in the last few VBlanks the
 * effects that read the shadow map are skipped (gpu_shadow_off_active). */
static int      sh_off_n = 0, sh_off_seen = 0;
static uint16_t sh_off_tpage[16], sh_off_clut[16];
static uint32_t sh_off_frame = 0;
void gpu_set_shadow_off_textures(const uint32_t *tpage, const uint32_t *clut, int n) {
    sh_off_n = n < 0 ? 0 : n > 16 ? 16 : n;
    for (int i = 0; i < sh_off_n; i++) { sh_off_tpage[i] = (uint16_t)tpage[i]; sh_off_clut[i] = (uint16_t)clut[i]; }
    sh_off_seen = 0;
}
/* semi: the primitive is drawn see-through. A screen's paper or frame is
 * opaque; a glow sharing its palette (BoF4: the sun's glare in the dragon's
 * cutscene, CLUT 0x3e00 on an additive page) is not a sunless screen, and
 * counted as one it switched the effects off and on every few frames. */
static void shadow_off_note(uint16_t tpage, uint16_t clut, int semi) {
    if (semi) return;
    for (int i = 0; i < sh_off_n; i++)
        if ((sh_off_tpage[i] == 0xFFFFu || sh_off_tpage[i] == (tpage & 0x1FFu)) &&
            (sh_off_clut[i] == 0xFFFFu || sh_off_clut[i] == clut)) {
            sh_off_frame = (uint32_t)s_frame_count; sh_off_seen = 1;
            return;
        }
}
int gpu_shadow_area_off(void);   /* below (sun_areas) */
int gpu_shadow_off_active(void) {
    return (sh_off_seen && (uint32_t)s_frame_count - sh_off_frame <= 4u) || gpu_shadow_area_off();
}
/* [video.shadow] sun_areas: the sun per area (a mod's map id, or else the
 * scene_keys' number; kept at each VBlank while the shadow map is in use).
 * Two layers: the config's suns (gpu_set_shadow_sun_areas) and the live ones
 * (gpu_shadow_light_set: a title's own file, a tool), a sun and a moon per
 * area, which win. Each light is one 64-bit word (mode, azimuth and
 * elevation in tenths of a degree): the render thread reads it whole while
 * the emulation thread changes it. */
enum { SH_SUN_MAX = 256 };
typedef struct { uint32_t area; _Atomic uint64_t light[2]; } ShLight;
static ShLight          sh_cfg[SH_SUN_MAX], sh_live[SH_SUN_MAX];
static _Atomic int      sh_cfg_n = 0, sh_live_n = 0;
static _Atomic uint32_t sh_area = 0;
static uint64_t sh_light_pack(int mode, float az, float el) {
    const int a = (int)lroundf(az * 10.0f), e = (int)lroundf(el * 10.0f);
    return (uint64_t)(uint8_t)mode | (uint64_t)(uint16_t)(int16_t)a << 16 | (uint64_t)(uint16_t)(int16_t)e << 32;
}
static int sh_light_unpack(uint64_t w, float *az, float *el) {
    if (az) *az = (float)(int16_t)(uint16_t)(w >> 16) / 10.0f;
    if (el) *el = (float)(int16_t)(uint16_t)(w >> 32) / 10.0f;
    return (int)(w & 0xFFu);
}
static ShLight *sh_light_find(ShLight *t, int n, uint32_t area) {
    for (int i = 0; i < n; i++) if (t[i].area == area) return &t[i];
    return NULL;
}
void gpu_set_shadow_sun_areas(const uint32_t *area, const float *az, const float *el, int n) {
    n = n < 0 ? 0 : n > SH_SUN_MAX ? SH_SUN_MAX : n;
    atomic_store(&sh_cfg_n, 0);
    for (int i = 0; i < n; i++) {
        sh_cfg[i].area = area[i];
        atomic_store(&sh_cfg[i].light[0], sh_light_pack(el[i] > 0.0f ? GPU_LIGHT_AT : GPU_LIGHT_OFF, az[i], el[i]));
        atomic_store(&sh_cfg[i].light[1], 0);
    }
    atomic_store(&sh_cfg_n, n);
}
int gpu_shadow_light_set(uint32_t area, int which, int mode, float az, float el) {
    if (which < 0 || which > 1 || mode < GPU_LIGHT_NONE || mode > GPU_LIGHT_OFF) return 0;
    const int n = atomic_load(&sh_live_n);
    ShLight *l = sh_light_find(sh_live, n, area);
    if (!l) {
        if (mode == GPU_LIGHT_NONE) return 1;
        if (n >= SH_SUN_MAX) return 0;
        l = &sh_live[n];
        l->area = area;
        atomic_store(&l->light[0], 0); atomic_store(&l->light[1], 0);
        atomic_store(&sh_live_n, n + 1);
    }
    atomic_store(&l->light[which], mode == GPU_LIGHT_NONE ? 0 : sh_light_pack(mode, az, el));
    return 1;
}
int gpu_shadow_light_get(uint32_t area, int which, int layer, float *az, float *el) {
    if (which < 0 || which > 1) return GPU_LIGHT_NONE;
    ShLight *l = layer ? sh_light_find(sh_cfg, atomic_load(&sh_cfg_n), area)
                       : sh_light_find(sh_live, atomic_load(&sh_live_n), area);
    return l ? sh_light_unpack(atomic_load(&l->light[which]), az, el) : GPU_LIGHT_NONE;
}
uint32_t gpu_shadow_current_area(void) { return atomic_load(&sh_area); }
/* gpu_shadow_lights_load: a lights file (a tuning tool's), one area per line:
 *   0xAREA sun AZ EL moon AZ EL      ("off" for either; '#' starts a comment) */
static int sh_is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static char *sh_next_word(char **p) {
    char *s = *p;
    while (sh_is_space(*s)) s++;
    if (!*s) { *p = s; return NULL; }
    char *w = s;
    while (*s && !sh_is_space(*s)) s++;
    if (*s) *s++ = '\0';
    *p = s;
    return w;
}
int gpu_shadow_lights_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *p = line, *t = sh_next_word(&p);
        if (!t) continue;
        const uint32_t area = (uint32_t)strtoul(t, NULL, 0);
        if (!area) continue;
        while ((t = sh_next_word(&p)) != NULL) {
            const int which = strcmp(t, "moon") == 0 ? 1 : strcmp(t, "sun") == 0 ? 0 : -1;
            char *a = which < 0 ? NULL : sh_next_word(&p);
            if (!a) break;
            if (strcmp(a, "off") == 0) { n += gpu_shadow_light_set(area, which, GPU_LIGHT_OFF, 0.0f, 0.0f); continue; }
            char *b = sh_next_word(&p), *ea, *eb;
            if (!b) break;
            const float az = strtof(a, &ea), el = strtof(b, &eb);
            if (*ea || *eb || !(el > 0.0f && el < 90.0f)) break;
            n += gpu_shadow_light_set(area, which, GPU_LIGHT_AT, az, el);
        }
    }
    fclose(f);
    return n;
}
/* the live layer's light when it has one, else the config's */
static int sh_light_now(int which, float *az, float *el) {
    const uint32_t a = atomic_load(&sh_area);
    const int m = gpu_shadow_light_get(a, which, 0, az, el);
    return m != GPU_LIGHT_NONE ? m : gpu_shadow_light_get(a, which, 1, az, el);
}
int gpu_shadow_sun_area(float *az, float *el) {
    float a, e;
    if (sh_light_now(0, &a, &e) != GPU_LIGHT_AT) return 0;
    *az = a; *el = e;
    return 1;
}
int gpu_shadow_moon_area(float *az, float *el) {
    float a, e;
    if (sh_light_now(1, &a, &e) != GPU_LIGHT_AT) return 0;
    *az = a; *el = e;
    return 1;
}
/* "off": no sun in that area; at night (the night's share past half,
 * gl_renderer_night) the area's moon decides when it has one */
int gpu_shadow_area_off(void) {
    if (gl_renderer_night() > 0.5f) {
        const int m = sh_light_now(1, NULL, NULL);
        if (m != GPU_LIGHT_NONE) return m == GPU_LIGHT_OFF;
    }
    return sh_light_now(0, NULL, NULL) == GPU_LIGHT_OFF;
}
static int gp0_tex_hidden(uint16_t tpage, uint16_t clut) {
    if (!sh_hide_on) return 0;
    for (int i = 0; i < sh_hide_n; i++)
        if (sh_hide_tpage[i] == (tpage & 0x1FFu) && (sh_hide_clut[i] == 0xFFFFu || sh_hide_clut[i] == clut)) {
            sh_hidden++; return 1;
        }
    return 0;
}
/* The game's blob shadows found by their look, under anything (a boat, a
 * dragon), not only at a sprite's foot point: a textured quad drawn
 * subtractive (it darkens the floor) whose texture is a disc (the corners of
 * its texture box drawn nothing, its centre drawn). Each texture box is
 * judged once (sh_disc_cache); not drawn while our shadows stand in for them
 * (sh_disc_on: the sun, or a fire, casting). */
static int sh_disc_on = 0;
static uint32_t sh_discs = 0;
typedef struct { uint32_t key, box; int8_t disc; } ShDisc;
static ShDisc sh_disc_cache[256];
static int disc_texel_drawn(uint16_t tpage, uint16_t clut, int u, int v) {
    const int depth = (tpage >> 7) & 3, bx = (tpage & 15) * 64, by = ((tpage >> 4) & 1) * 256;
    const int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
    u &= 255; v &= 255;
    uint16_t t;
    if (depth == 0) t = vram[cy * 1024 + ((cx + ((vram[(by + v) * 1024 + ((bx + (u >> 2)) & 1023)] >> ((u & 3) * 4)) & 0xF)) & 1023)];
    else if (depth == 1) t = vram[cy * 1024 + ((cx + ((vram[(by + v) * 1024 + ((bx + (u >> 1)) & 1023)] >> ((u & 1) * 8)) & 0xFF)) & 1023)];
    else t = vram[(by + v) * 1024 + ((bx + u) & 1023)];
    return t != 0;
}
/* uv: the quad's four uv words (the low 16 bits: u, v) */
static int shadow_disc_quad(int semi, uint16_t tpage, uint16_t clut, const uint32_t *uv) {
    if (!sh_disc_on || !semi || ((tpage >> 5) & 3) != 2) return 0;
    int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
    for (int i = 0; i < 4; i++) {
        const int u = (int)(uv[i] & 255), v = (int)((uv[i] >> 8) & 255);
        if (u < u0) u0 = u; if (u > u1) u1 = u; if (v < v0) v0 = v; if (v > v1) v1 = v;
    }
    if (u1 - u0 < 8 || v1 - v0 < 8) return 0;
    const uint32_t key = (uint32_t)(tpage & 0x1FFu) | (uint32_t)clut << 16;
    const uint32_t box = (uint32_t)u0 | (uint32_t)v0 << 8 | (uint32_t)u1 << 16 | (uint32_t)v1 << 24;
    ShDisc *c = &sh_disc_cache[(key * 2654435761u ^ box * 40503u) >> 24];
    if (c->key != key || c->box != box || !c->disc) {
        c->key = key; c->box = box;
        /* the texel inside each corner (the box's last row/column may be the
         * next tile's), and the centre */
        const int a = u0 + 1, b = u1 - 2, p = v0 + 1, q = v1 - 2;
        c->disc = (!disc_texel_drawn(tpage, clut, a, p) && !disc_texel_drawn(tpage, clut, b, p) &&
                   !disc_texel_drawn(tpage, clut, a, q) && !disc_texel_drawn(tpage, clut, b, q) &&
                   disc_texel_drawn(tpage, clut, (u0 + u1) / 2, (v0 + v1) / 2)) ? 1 : -1;
    }
    if (c->disc > 0) { sh_discs++; return 1; }
    return 0;
}
static float    sh_last_bad[10];
static int sh_ts_n;   /* routines that draw both windings (below) */
int psx_shadow_state(char *out, int cap) {
    float saz = 0.0f, sel = 0.0f;
    const int sun = gpu_shadow_sun_area(&saz, &sel);
    return snprintf(out, (size_t)cap, "\"scene\":\"0x%08x\",\"scene_sun\":[%d,%.1f,%.1f],\"two_sided\":%d,\"cfg_camera\":%d,\"overdraw\":[%d,%d],\"overdraw_on\":%d,\"overdraw_extra\":[%d,%d,%u],"
                    "\"keys\":%d,\"hide\":%d,\"hidden\":%u,\"discs\":%u,\"fed\":%u,\"rejected\":%u,\"cam_taken\":%u,\"anchors\":%u,\"blobs\":%u,"
                    "\"last_bad\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]",
                    (unsigned)sh_area, sun, saz, sel, sh_ts_n, sh_camera, sh_od_x, sh_od_y, sh_od_on, ws_overdraw_ex, ws_overdraw_ey, ws_overdraw_extras, sh_nkeys, sh_hide_n, sh_hidden, sh_discs, sh_fed, sh_rejected, sh_cam_taken, sh_anc_taken, sh_blobs,
                    sh_last_bad[0], sh_last_bad[1], sh_last_bad[2], sh_last_bad[3], sh_last_bad[4],
                    sh_last_bad[5], sh_last_bad[6], sh_last_bad[7], sh_last_bad[8], sh_last_bad[9]);
}
void psx_shadow_configure(int camera, int od_x, int od_y, const uint32_t *addr,
                          const uint32_t *size, int nkeys) {
    sh_camera = camera; sh_od_x = od_x; sh_od_y = od_y;
    sh_nkeys = nkeys < 0 ? 0 : nkeys > 8 ? 8 : nkeys;
    for (int i = 0; i < sh_nkeys; i++) { sh_key_addr[i] = addr[i]; sh_key_size[i] = size[i]; }
}
static void shadow_backfaces_flush(void);   /* below, after the drawing state */
static int  sh_bf_on = 0;
int psx_shadow_backfaces_on(void) { return sh_bf_on; }
/* Through the mod services (mod_guest_services.c), which see the CPU state
 * as the running build lays it out. */
extern int psx_mod_gte_rotation(int16_t out[9]);
extern int psx_mod_gte_projection(int32_t out[6]);
/* camera = "ram:ADDR" (2): the game's camera MATRIX in RAM names which of the
 * matrices passing through the GTE is the camera. Before each projection, a
 * rotation + translation not met yet is compared with it; equal, it is the
 * camera the frame being built uses, handed to the renderer right then: the
 * triangles the GPU draws from that point on (this frame's, when its list is
 * sent) carry it, so a picture is lit with its own camera, not the next
 * frame's. When no projection used it for a few VBlanks (a battle, a map
 * screen: another camera, or none) the renderer is told there is none. */
static uint32_t sh_cam_addr = 0;
static int16_t  sh_cam_r[2][9];     /* [0] the camera, [1] the last matrix that was not it */
static int32_t  sh_cam_t[2][3];
static int      sh_cam_have[2] = {0, 0};
static float    sh_cam_last[15];    /* r, t, of, h of the camera */
static int      sh_cam_met = 0, sh_cam_age = 99;   /* used since the last VBlank; VBlanks since */
void psx_shadow_camera_ram(uint32_t addr) { sh_cam_addr = addr; }
static void shadow_camera_note(void) {
    int16_t ri[9];
    int32_t pj[6];
    if (!psx_mod_gte_rotation(ri) || !psx_mod_gte_projection(pj)) return;
    for (int k = 0; k < 2; k++)
        if (sh_cam_have[k] && !memcmp(ri, sh_cam_r[k], sizeof ri) && !memcmp(pj, sh_cam_t[k], sizeof sh_cam_t[k])) {
            if (k == 0) sh_cam_met = 1;
            return;   /* the camera again, or the matrix just found not to be it */
        }
    int same = pj[5] > 0;
    for (int i = 0; i < 9 && same; i++) same = (int16_t)psx_read_half(sh_cam_addr + 2u * (uint32_t)i) == ri[i];
    for (int i = 0; i < 3 && same; i++) same = (int32_t)psx_read_word(sh_cam_addr + 20u + 4u * (uint32_t)i) == pj[i];
    const int k = same ? 0 : 1;
    memcpy(sh_cam_r[k], ri, sizeof ri); memcpy(sh_cam_t[k], pj, sizeof sh_cam_t[k]); sh_cam_have[k] = 1;
    if (!same) return;
    for (int i = 0; i < 9; i++) sh_cam_last[i] = (float)ri[i] * (1.0f / 4096.0f);
    for (int i = 0; i < 3; i++) sh_cam_last[9 + i] = (float)pj[i];
    sh_cam_last[12] = (float)pj[3] / 65536.0f; sh_cam_last[13] = (float)pj[4] / 65536.0f;
    sh_cam_last[14] = (float)pj[5];
    sh_cam_met = 1;
    sh_cam_taken++;
    gl_renderer_shadow_frame_camera(sh_cam_last, sh_cam_last + 9, sh_cam_last[12], sh_cam_last[13], sh_cam_last[14]);
}
/* [video.shadow] sprite_anchors: the return addresses of the game's calls that
 * project a sprite's foot point (an RTPS with the camera; BoF4: characters and
 * monsters). Each one is handed to the renderer in stream order, in the view space
 * of the camera that projects it (the one its 2D pieces are drawn with), so the 2D
 * pieces drawn around it that frame are known to stand there (sprite shadows,
 * gl_postfx.inc), and the game's own blob under it, a translucent flat textured
 * quad centred on the point, is not drawn while sprites cast real shadows. */
extern uint32_t psx_mod_cpu_gpr(int r);
extern int psx_mod_gte_data(uint32_t out[32]);
enum { SH_ANC_MAX = 64 };
static uint32_t sh_anc_ra[8];
static int      sh_anc_n = 0, sh_anc_live = 0, sh_blob_on = 0, sh_track = 0;
static int16_t  sh_anc_sxy[SH_ANC_MAX][2];
static uint32_t sh_anc_frame[SH_ANC_MAX];
static int      sh_anc_at = 0;
void psx_shadow_sprite_anchors(const uint32_t *ra, int n) {
    sh_anc_n = n < 0 ? 0 : n > 8 ? 8 : n;
    for (int i = 0; i < sh_anc_n; i++) sh_anc_ra[i] = ra[i];
}
/* Any single point projected with the camera (an RTPS): to the renderer, in
 * that camera's view space; an effect drawn in 2D round it (a fire's flames)
 * is a light there (gl_postfx.inc pfx_lights_update). */
static void shadow_point_note(void) {
    int16_t ri[9];
    int32_t pj[6];
    uint32_t d[32];
    if (!psx_mod_gte_rotation(ri) || !psx_mod_gte_projection(pj) || !psx_mod_gte_data(d)) return;
    if (!sh_cam_have[0] || memcmp(ri, sh_cam_r[0], sizeof ri) || memcmp(pj, sh_cam_t[0], sizeof sh_cam_t[0])) return;
    const float w[3] = {(float)(int16_t)d[0], (float)(int16_t)(d[0] >> 16), (float)(int16_t)d[1]};
    float V[3];
    for (int r = 0; r < 3; r++)
        V[r] = sh_cam_last[r * 3] * w[0] + sh_cam_last[r * 3 + 1] * w[1] + sh_cam_last[r * 3 + 2] * w[2] + sh_cam_last[9 + r];
    if (V[2] < 1.0f) return;
    gl_renderer_shadow_point(V);
}
static void shadow_anchor_note(void) {
    const uint32_t ra = psx_mod_cpu_gpr(31);
    int k = 0;
    while (k < sh_anc_n && sh_anc_ra[k] != ra) k++;
    if (k == sh_anc_n) return;
    int16_t ri[9];
    int32_t pj[6];
    uint32_t d[32];
    if (!psx_mod_gte_rotation(ri) || !psx_mod_gte_projection(pj) || !psx_mod_gte_data(d)) return;
    if (!sh_cam_have[0] || memcmp(ri, sh_cam_r[0], sizeof ri) || memcmp(pj, sh_cam_t[0], sizeof sh_cam_t[0])) return;
    const float w[3] = {(float)(int16_t)d[0], (float)(int16_t)(d[0] >> 16), (float)(int16_t)d[1]};
    float V[3];
    for (int r = 0; r < 3; r++)
        V[r] = sh_cam_last[r * 3] * w[0] + sh_cam_last[r * 3 + 1] * w[1] + sh_cam_last[r * 3 + 2] * w[2] + sh_cam_last[9 + r];
    if (V[2] < 1.0f) return;
    const int sx = (int)lroundf(sh_cam_last[12] + sh_cam_last[14] * V[0] / V[2]);
    const int sy = (int)lroundf(sh_cam_last[13] + sh_cam_last[14] * V[1] / V[2]);
    /* the same point projected again (a second call for the same sprite) */
    const int last = (sh_anc_at + SH_ANC_MAX - 1) % SH_ANC_MAX;
    if (sh_anc_frame[last] == (uint32_t)s_frame_count && sh_anc_sxy[last][0] == sx && sh_anc_sxy[last][1] == sy) return;
    sh_anc_sxy[sh_anc_at][0] = (int16_t)sx; sh_anc_sxy[sh_anc_at][1] = (int16_t)sy;
    sh_anc_frame[sh_anc_at] = (uint32_t)s_frame_count;
    sh_anc_at = (sh_anc_at + 1) % SH_ANC_MAX;
    sh_anc_taken++;
    gl_renderer_shadow_anchor(V);   /* in this projection's own camera space */
}
/* A translucent textured quad, flatter than tall, centred (3 px) on a foot
 * point projected in the last few VBlanks: the game's blob shadow. */
static int shadow_blob_quad(int semi, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3) {
    if (!sh_blob_on || !semi) return 0;
    const uint32_t p[4] = {p0, p1, p2, p3};
    int x0 = 99999, x1 = -99999, y0 = 99999, y1 = -99999;
    for (int i = 0; i < 4; i++) {
        const int x = ((int)(p[i] & 0x7FFu) ^ 0x400) - 0x400, y = ((int)((p[i] >> 16) & 0x7FFu) ^ 0x400) - 0x400;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    const int w = x1 - x0, h = y1 - y0;
    if (h <= 0 || w > 128 || w * 10 < h * 13) return 0;
    const int cx2 = x0 + x1, cy2 = y0 + y1;   /* twice the centre */
    for (int i = 0; i < SH_ANC_MAX; i++) {
        if ((uint32_t)s_frame_count - sh_anc_frame[i] > 3u || !sh_anc_frame[i]) continue;
        if (abs(cx2 - 2 * sh_anc_sxy[i][0]) <= 6 && abs(cy2 - 2 * sh_anc_sxy[i][1]) <= 6) { sh_blobs++; return 1; }
    }
    return 0;
}
/* A blob painted into a sprite's own picture (BoF4's battle sprites: a
 * black ellipse under the feet, in one near-black CLUT entry the sprite uses
 * for nothing else). It is learnt from an opaque 4/8-bit quad holding a foot
 * point projected in the last few VBlanks, a quarter or more of whose drawn
 * texels are one near-black entry: that palette's blob colour. From then on
 * every quad with that palette reaching the ground's band round a foot point
 * (SH_KEY_ABOVE px above its row to SH_KEY_BELOW below: the feet away from
 * the point in an attack or victory pose, a monster's tail; not the head,
 * whose eyes may use the colour) draws it see-through, while the entry
 * still holds the colour learnt and was confirmed in the last SH_KEY_LIFE
 * VBlanks. Out: the entry's VRAM place (the caller draws the quad with it
 * see-through, then puts it back); 0 = none. tc: the 4 texcoord words (the
 * first with the CLUT, the second the page). */
enum { SH_KEY_MAX = 64, SH_KEY_LIFE = 3600, SH_KEY_ABOVE = 12, SH_KEY_BELOW = 16 };
/* a palette's blob colour and the foot points (picture px, the latest first)
 * of the sprites it was learnt from */
static struct { uint16_t clut, col; uint8_t idx, npos; int16_t pos[4][2]; uint32_t seen; } sh_key[SH_KEY_MAX];
static int sh_key_n = 0;
static int shadow_blob_key(int semi, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3, const uint32_t *tc,
                           int *kx, int *ky) {
    if (!sh_blob_on || semi) return 0;
    const uint16_t clut = (uint16_t)(tc[0] >> 16), tpage = (uint16_t)(tc[1] >> 16);
    const int depth = (tpage >> 7) & 3;
    if (depth > 1) return 0;
    const int tx = (tpage & 0xF) * 64, ty = ((tpage >> 4) & 1) * 256;
    const int cx = (clut & 0x3F) * 16, cy = (clut >> 6) & 0x1FF;
    const uint32_t now = (uint32_t)s_frame_count;
    int known = -1;
    for (int i = 0; i < sh_key_n && known < 0; i++) {
        if (sh_key[i].clut != clut) continue;
        if (now - sh_key[i].seen > SH_KEY_LIFE ||
            vram[(cy & 511) * 1024 + ((cx + sh_key[i].idx) & 1023)] != sh_key[i].col) { sh_key[i] = sh_key[--sh_key_n]; break; }
        known = i;
    }
    const uint32_t p[4] = {p0, p1, p2, p3};
    int x0 = 99999, x1 = -99999, y0 = 99999, y1 = -99999;
    for (int i = 0; i < 4; i++) {
        const int x = ((int)(p[i] & 0x7FFu) ^ 0x400) - 0x400, y = ((int)((p[i] >> 16) & 0x7FFu) ^ 0x400) - 0x400;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    int at = 0, ai = -1;
    if (x1 - x0 <= 64 && y1 - y0 <= 64 && x1 > x0 && y1 > y0)
        for (int i = 0; i < SH_ANC_MAX && !at; i++) {
            if (now - sh_anc_frame[i] > 3u || !sh_anc_frame[i]) continue;
            at = sh_anc_sxy[i][0] >= x0 && sh_anc_sxy[i][0] <= x1 && sh_anc_sxy[i][1] >= y0 && sh_anc_sxy[i][1] <= y1;
            if (at) ai = i;
        }
    if (at) {   /* (re)learn: the feet's tile */
        int u0 = 255, u1 = 0, v0 = 255, v1 = 0;
        for (int i = 0; i < 4; i++) {
            const int u = (int)(tc[i] & 0xFF), v = (int)((tc[i] >> 8) & 0xFF);
            if (u < u0) u0 = u; if (u > u1) u1 = u; if (v < v0) v0 = v; if (v > v1) v1 = v;
        }
        uint16_t n[256];
        memset(n, 0, sizeof n);
        int drawn = 0;
        for (int v = v0; v < v1; v++)
            for (int u = u0; u < u1; u++) {
                const uint16_t w = vram[((ty + v) & 511) * 1024 + ((tx + (depth ? u >> 1 : u >> 2)) & 1023)];
                const int idx = depth ? (w >> ((u & 1) * 8)) & 0xFF : (w >> ((u & 3) * 4)) & 0xF;
                if (vram[(cy & 511) * 1024 + ((cx + idx) & 1023)]) { n[idx]++; drawn++; }
            }
        int best = -1;
        for (int k = 0; k < (depth ? 256 : 16); k++) {
            const uint16_t c = vram[(cy & 511) * 1024 + ((cx + k) & 1023)];
            if (!c || (c & 31) > 3 || ((c >> 5) & 31) > 3 || ((c >> 10) & 31) > 3) continue;
            if (best < 0 || n[k] > n[best]) best = k;
        }
        if (best >= 0 && n[best] >= 12 && n[best] * 4 >= drawn) {
            if (known < 0) {
                if (sh_key_n == SH_KEY_MAX) {   /* the least recently confirmed goes */
                    int o = 0;
                    for (int i = 1; i < sh_key_n; i++) if (now - sh_key[i].seen > now - sh_key[o].seen) o = i;
                    sh_key[o] = sh_key[--sh_key_n];
                }
                known = sh_key_n++;
                sh_key[known].npos = 0;
            }
            sh_key[known].clut = clut; sh_key[known].idx = (uint8_t)best;
            sh_key[known].col = vram[(cy & 511) * 1024 + ((cx + best) & 1023)];
            sh_key[known].seen = now;
            int16_t (*ps)[2] = sh_key[known].pos;
            int j = 0;
            while (j < sh_key[known].npos && (abs(ps[j][0] - sh_anc_sxy[ai][0]) > 2 || abs(ps[j][1] - sh_anc_sxy[ai][1]) > 2)) j++;
            if (j == sh_key[known].npos && j < 4) sh_key[known].npos++;
            if (j == 4) j = 3;
            for (; j > 0; j--) { ps[j][0] = ps[j - 1][0]; ps[j][1] = ps[j - 1][1]; }
            ps[0][0] = sh_anc_sxy[ai][0]; ps[0][1] = sh_anc_sxy[ai][1];
        }
    }
    if (known < 0) return 0;
    /* only near the ground: a quad reaching the band round the row of a foot
     * point of a sprite this palette was learnt from (the colour may also
     * draw its eyes or outlines higher up; another sprite's feet may be at
     * that height) */
    int near = 0;
    for (int j = 0; j < sh_key[known].npos && !near; j++) {
        const int16_t *q = sh_key[known].pos[j];
        near = y1 >= q[1] - SH_KEY_ABOVE && y0 <= q[1] + SH_KEY_BELOW && x1 >= q[0] - 64 && x0 <= q[0] + 64;
    }
    if (!near) return 0;
    *kx = (cx + sh_key[known].idx) & 1023; *ky = cy & 511;
    sh_blobs++;
    return 1;
}
/* The key entry see-through (0) for one draw, in the guest's VRAM and the
 * renderer's, in drawing order; shadow_blob_key_put gives it back at once. */
static uint16_t shadow_blob_key_hide(int x, int y) {
    const uint16_t c = vram[y * 1024 + x];
    vram[y * 1024 + x] = 0;
    gr_vram_write(x, y, 0);
    return c;
}
static void shadow_blob_key_put(int x, int y, uint16_t c) {
    vram[y * 1024 + x] = c;
    gr_vram_write(x, y, c);
}
static void shadow_vblank(void) {
    const int chain = gl_renderer_shadow_sun_wanted();
    /* the camera and the sprites' foot points are followed while a chain
     * reads the shadow map or the sprites (a light that also lights them),
     * in an area with no sun too */
    const int track = (chain || gl_renderer_shadow_sprites_wanted()) && sh_camera;
    sh_track = track;
    uint32_t key = 0;
    if (track) {
        for (int i = 0; i < sh_nkeys; i++) {
            uint32_t v = psx_read_byte(sh_key_addr[i]);
            if (sh_key_size[i] >= 2) v |= (uint32_t)psx_read_byte(sh_key_addr[i] + 1) << 8;
            if (sh_key_size[i] == 4) v |= (uint32_t)psx_read_byte(sh_key_addr[i] + 2) << 16
                                        | (uint32_t)psx_read_byte(sh_key_addr[i] + 3) << 24;
            key = key * 2654435761u ^ v;
        }
        if (sh_nkeys && key != sh_key_last) { sh_key_last = key; gl_renderer_shadow_cache_clear(); }
        /* the area: a mod's (its map) when it gives one, else the scene_keys' */
        const uint32_t mod_area = gl_renderer_shadow_area_id();
        sh_area = mod_area ? mod_area : sh_nkeys ? key : 0;
    }
    /* an area with no sun (sun_areas "off": a cave, a room) is as if no chain
     * read the shadow map: the game's own blob shadows are drawn there */
    const int wanted = chain && !gpu_shadow_area_off();
    /* our shadows stand in for the game's blobs: under the sun, or a fire's */
    const int ours = wanted || (track && gl_renderer_fire_casting());
    sh_hide_on = ours && sh_hide_n > 0;
    sh_disc_on = ours;
    if (wanted != sh_od_on && (sh_od_x || sh_od_y)) {
        gpu_set_overdraw(wanted ? sh_od_x : 0, wanted ? sh_od_y : 0);
        sh_od_on = wanted;
    }
    sh_bf_on = wanted && sh_camera;
    sh_anc_live = track && sh_camera == 2 && sh_anc_n > 0 && gl_renderer_shadow_sprites_wanted();
    sh_blob_on = sh_anc_live && ours;   /* the game's blobs hidden: only under our sun or a fire */
    if (sh_bf_on) shadow_backfaces_flush();   /* this frame's culled faces, before the camera takes the frame in */
    if (!track) return;
    if (sh_camera == 2) {   /* the camera met lately (a 30 fps game projects every other VBlank) */
        sh_cam_age = sh_cam_met ? 0 : sh_cam_age < 99 ? sh_cam_age + 1 : 99;
        sh_cam_met = 0;
        if (sh_cam_have[0] && sh_cam_age <= 2) {
            gl_renderer_shadow_camera(sh_cam_last, sh_cam_last + 9, sh_cam_last[12], sh_cam_last[13], sh_cam_last[14]);
        } else if (sh_cam_have[0]) {   /* gone: none until the camera is met again */
            sh_cam_have[0] = 0;
            gl_renderer_shadow_frame_camera(NULL, NULL, 0.0f, 0.0f, 0.0f);
        }
        return;
    }
    int16_t ri[9];
    int32_t pj[6];
    if (!psx_mod_gte_rotation(ri) || !psx_mod_gte_projection(pj)) return;
    float r[9];
    for (int i = 0; i < 9; i++) r[i] = (float)ri[i] * (1.0f / 4096.0f);
    /* a proper rotation: unit rows, orthogonal, det +1 (4.12 rounding allowed) */
    for (int a = 0; a < 3; a++)
        for (int b = a; b < 3; b++) {
            const float d = r[a * 3] * r[b * 3] + r[a * 3 + 1] * r[b * 3 + 1] + r[a * 3 + 2] * r[b * 3 + 2];
            if (fabsf(d - (a == b ? 1.0f : 0.0f)) > 0.02f) {
                sh_rejected++; memcpy(sh_last_bad, r, sizeof r); sh_last_bad[9] = d; return;
            }
        }
    const float det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6])
                    + r[2] * (r[3] * r[7] - r[4] * r[6]);
    if (det < 0.9f) { sh_rejected++; return; }
    const float t[3] = {(float)pj[0], (float)pj[1], (float)pj[2]};
    const float h = (float)pj[5];
    if (h <= 0.0f) return;
    sh_fed++;
    gl_renderer_shadow_camera(r, t, (float)pj[3] / 65536.0f, (float)pj[4] / 65536.0f, h);
}
static int ws_activation_guard_pixels = 0;
void gpu_ws_set_margin_override(int v) { ws_margin_override = v; }
void gpu_ws_set_cull_guard_pixels(int pixels) {
    if (pixels < 0) pixels = 0;
    if (pixels > 256) pixels = 256;
    ws_cull_guard_pixels = pixels;
}
void gpu_ws_set_activation_guard_pixels(int pixels) {
    if (pixels < 0) pixels = 0;
    if (pixels > 256) pixels = 256;
    ws_activation_guard_pixels = pixels;
}

/* Runtime copies of the explicit [widescreen.cull] site lists, consulted by
 * the dirty-RAM interpreter (native code has the sites compiled in). Stored
 * sorted and de-duplicated so a lookup is a binary search: a title may list
 * dozens of sites, and the interpreter asks on every candidate instruction.
 * A list longer than the cap is logged, never silently truncated. */
#define WS_EXPLICIT_CULL_SITES_MAX 256
static int ws_explicit_cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
static int ws_explicit_store(uint32_t *dst, const uint32_t *src, int n,
                             const char *key) {
    int count = 0;
    if (n < 0 || !src) n = 0;
    if (n > WS_EXPLICIT_CULL_SITES_MAX) {
        fprintf(stderr,
                "[widescreen] %s: %d sites exceed the interpreter cap of %d; "
                "sites past the cap stay vanilla in interpreted code\n",
                key, n, WS_EXPLICIT_CULL_SITES_MAX);
        n = WS_EXPLICIT_CULL_SITES_MAX;
    }
    for (int i = 0; i < n; i++) dst[i] = src[i] & 0x1FFFFFFFu;
    qsort(dst, (size_t)n, sizeof(dst[0]), ws_explicit_cmp_u32);
    for (int i = 0; i < n; i++)
        if (count == 0 || dst[count - 1] != dst[i]) dst[count++] = dst[i];
    return count;
}
static int ws_explicit_site(const uint32_t *sites, int n, uint32_t pc) {
    uint32_t p = pc & 0x1FFFFFFFu;
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (sites[mid] < p) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && sites[lo] == p;
}
#define WS_EXPLICIT_LIST(name)                                              \
    static uint32_t ws_explicit_##name##_sites[WS_EXPLICIT_CULL_SITES_MAX]; \
    static int ws_explicit_##name##_n = 0
WS_EXPLICIT_LIST(bias);
WS_EXPLICIT_LIST(slti);
WS_EXPLICIT_LIST(range);
WS_EXPLICIT_LIST(slti_lower);
WS_EXPLICIT_LIST(negsub);
WS_EXPLICIT_LIST(vxrange);
WS_EXPLICIT_LIST(depth);
WS_EXPLICIT_LIST(plane_nx);
WS_EXPLICIT_LIST(xclip_load);
WS_EXPLICIT_LIST(bltz);
WS_EXPLICIT_LIST(bgez);
WS_EXPLICIT_LIST(branch_keep);
WS_EXPLICIT_LIST(clip_edge_x_load);
static uint32_t ws_clip_edge_width = 320;

void gpu_ws_set_explicit_cull_sites(const uint32_t *bias, int nbias,
                                    const uint32_t *slti, int nslti,
                                    const uint32_t *range, int nrange) {
    ws_explicit_bias_n = ws_explicit_store(ws_explicit_bias_sites, bias,
                                           nbias, "bias_sites");
    ws_explicit_slti_n = ws_explicit_store(ws_explicit_slti_sites, slti,
                                           nslti, "slti_sites");
    ws_explicit_range_n = ws_explicit_store(ws_explicit_range_sites, range,
                                            nrange, "range_sites");
}
int psx_ws_is_cull_bias_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_bias_sites, ws_explicit_bias_n, pc);
}
static uint32_t ws_explicit_bias_lower_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_explicit_bias_lower_n = 0;
void gpu_ws_set_bias_lower_cull_sites(const uint32_t *sites, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX) nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_explicit_bias_lower_n = nsites;
    for (int i = 0; i < nsites; i++)
        ws_explicit_bias_lower_sites[i] = sites[i] & 0x1FFFFFFFu;
}
int psx_ws_is_cull_bias_lower_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_bias_lower_sites, ws_explicit_bias_lower_n, pc);
}
int psx_ws_is_cull_slti_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_slti_sites, ws_explicit_slti_n, pc);
}
void gpu_ws_set_slti_lower_cull_sites(const uint32_t *sites, int nsites) {
    ws_explicit_slti_lower_n = ws_explicit_store(
        ws_explicit_slti_lower_sites, sites, nsites, "slti_lower_sites");
}
int psx_ws_is_cull_slti_lower_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_slti_lower_sites,
                            ws_explicit_slti_lower_n, pc);
}
void gpu_ws_set_negsub_cull_sites(const uint32_t *sites, int nsites) {
    ws_explicit_negsub_n = ws_explicit_store(
        ws_explicit_negsub_sites, sites, nsites, "negsub_sites");
}
int psx_ws_is_cull_negsub_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_negsub_sites, ws_explicit_negsub_n, pc);
}
void gpu_ws_set_vxrange_cull_sites(const uint32_t *sites, int nsites) {
    ws_explicit_vxrange_n = ws_explicit_store(
        ws_explicit_vxrange_sites, sites, nsites, "vxrange_sites");
}
int psx_ws_is_cull_vxrange_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_vxrange_sites, ws_explicit_vxrange_n, pc);
}
void gpu_ws_set_depth_cull_sites(const uint32_t *sites, int nsites) {
    ws_explicit_depth_n = ws_explicit_store(
        ws_explicit_depth_sites, sites, nsites, "depth_sites");
}
int psx_ws_is_cull_depth_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_depth_sites, ws_explicit_depth_n, pc);
}
int32_t psx_ws_depth_bound(int32_t imm) {
    if (psx_ws_x_margin() <= 0) return imm;
    int64_t numerator = (int64_t)imm * 3 * ws_cfg_num;
    int64_t denominator = 4 * ws_cfg_den;
    if (denominator <= 0) return imm;
    int64_t result = numerator >= 0
        ? (numerator + denominator / 2) / denominator
        : -((-numerator + denominator / 2) / denominator);
    return (int32_t)result;
}
int psx_ws_is_cull_range_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_range_sites, ws_explicit_range_n, pc);
}
void gpu_ws_set_plane_nx_sites(const uint32_t *sites, int nsites) {
    ws_explicit_plane_nx_n = ws_explicit_store(
        ws_explicit_plane_nx_sites, sites, nsites, "plane_nx_sites");
}
int psx_ws_is_cull_plane_nx_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_plane_nx_sites, ws_explicit_plane_nx_n, pc);
}
/* Side frustum-plane normal-X scale ([widescreen.cull] plane_nx_sites). The
 * plane half-angle goes as atan(nz/nx), so widening the cone by the aspect
 * ratio means scaling nx by the INVERSE factor (4*den)/(3*num):
 * atan(nz/(nx*4*den/(3*num))) == atan((3*num)/(4*den)*tan(theta)). Identity
 * at 4:3 (margin 0). */
int32_t psx_ws_plane_nx(int32_t nx) {
    if (psx_ws_x_margin() <= 0) return nx;
    /* Scale by source/final width. A full 4:3 view widens by 4:3 -> target;
     * a split-screen local viewport widens its own half (2:3 of the display)
     * to the target, so the frustum must open by final_w / src_w. */
    int64_t numerator = (int64_t)nx * 4 * ws_cfg_den;
    int64_t denominator = 3 * ws_cfg_num;
    int local_src = 0, local_final = 0;
    if (ws_local_viewport_layout(NULL, &local_src, &local_final, NULL)) {
        numerator = (int64_t)nx * local_src;
        denominator = local_final;
    }
    if (denominator <= 0) return nx;
    int64_t result = numerator >= 0
        ? (numerator + denominator / 2) / denominator
        : -((-numerator + denominator / 2) / denominator);
    return (int32_t)result;
}

void gpu_ws_set_xclip_load_sites(const uint32_t *sites, int nsites) {
    ws_explicit_xclip_load_n = ws_explicit_store(
        ws_explicit_xclip_load_sites, sites, nsites, "xclip_load_sites");
}
int psx_ws_is_cull_xclip_load_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_xclip_load_sites, ws_explicit_xclip_load_n, pc);
}

/* Explicit left-edge branch kinds and forced-keep branches for interpreted
 * code. Native code compiles the same lists in (code_generator.cpp); these
 * keep a dirty-RAM copy of a listed site on the same semantics. */
void gpu_ws_set_branch_cull_sites(const uint32_t *bltz, int nbltz,
                                  const uint32_t *bgez, int nbgez,
                                  const uint32_t *keep, int nkeep) {
    ws_explicit_bltz_n = ws_explicit_store(ws_explicit_bltz_sites, bltz,
                                           nbltz, "bltz_sites");
    ws_explicit_bgez_n = ws_explicit_store(ws_explicit_bgez_sites, bgez,
                                           nbgez, "bgez_sites");
    ws_explicit_branch_keep_n = ws_explicit_store(
        ws_explicit_branch_keep_sites, keep, nkeep, "branch_keep_sites");
}
int psx_ws_is_cull_bltz_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_bltz_sites, ws_explicit_bltz_n, pc);
}
int psx_ws_is_cull_bgez_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_bgez_sites, ws_explicit_bgez_n, pc);
}
int psx_ws_is_cull_branch_keep_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_branch_keep_sites,
                            ws_explicit_branch_keep_n, pc);
}
void gpu_ws_set_clip_edge_x_load_sites(const uint32_t *sites, int nsites,
                                       uint32_t width) {
    ws_explicit_clip_edge_x_load_n = ws_explicit_store(
        ws_explicit_clip_edge_x_load_sites, sites, nsites,
        "clip_edge_x_load_sites");
    ws_clip_edge_width = width ? width : 320u;
}
int psx_ws_is_cull_clip_edge_x_load_site(uint32_t pc) {
    return ws_explicit_site(ws_explicit_clip_edge_x_load_sites,
                            ws_explicit_clip_edge_x_load_n, pc);
}
uint32_t psx_ws_clip_edge_width(void) { return ws_clip_edge_width; }
/* [widescreen.cull] bgez_sites: `bgez SX, keep` keeps while SX >= -margin.
 * Returns the branch predicate. Identity at margin 0 (4:3). */
int psx_ws_cull_bgez(uint32_t v) {
    return psx_ws_cull_bgez_value((int32_t)v, ws_cull_xm());
}
/* [widescreen.cull] clip_edge_x_load_sites: a loaded clip bound equal to the
 * screen's left edge (0) becomes -margin and one equal to its right edge (w)
 * becomes w+margin; interior bounds are unchanged. Identity at margin 0. */
uint32_t psx_ws_clip_edge_x(uint32_t v, uint32_t w) {
    return psx_ws_clip_edge_x_value(v, w, psx_ws_x_margin());
}
/* [widescreen.cull] screen_x_sites on a screen X kept in the high half of a
 * register: `lui rt, W` becomes (W + margin) << 16, and `bltz` is taken only
 * while SX < -margin. Identity at margin 0 (4:3). */
uint32_t psx_ws_cull_lui_hi(uint32_t imm) {
    return psx_ws_cull_lui_hi_value(imm, psx_ws_x_margin());
}
int psx_ws_cull_bltz_hi(uint32_t v) {
    return psx_ws_cull_bltz_hi_value(v, psx_ws_x_margin());
}
/* Per-primitive X-reject bound ([widescreen.cull] xclip_load_sites). While
 * the margins are revealed the reject is disabled (INT32_MAX passes every
 * ANDI-masked u16 screen X, including wrapped off-left coords at 655xx); the
 * wide-surface scissor clips the overflow. Vanilla loaded value at 4:3. */
uint32_t psx_ws_xclip_bound(uint32_t vanilla) {
    return psx_ws_x_margin() > 0 ? 0x7FFFFFFFu : vanilla;
}

#include "ws_masked_reject.h"
typedef struct {
    uint32_t address, expected, mask;
} WsMaskedRejectSite;
static WsMaskedRejectSite ws_masked_reject_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_masked_reject_n;
void gpu_ws_set_masked_reject_sites(const uint32_t *addresses,
    const uint32_t *expected, const uint32_t *masks, int count) {
    if (count < 0 || count > WS_EXPLICIT_CULL_SITES_MAX) {
        fprintf(stderr, "psxrecomp: invalid masked reject site count %d\n", count);
        abort();
    }
    ws_masked_reject_n = count;
    for (int i = 0; i < count; ++i) {
        ws_masked_reject_sites[i].address = addresses[i] & 0x1FFFFFFFu;
        ws_masked_reject_sites[i].expected = expected[i];
        ws_masked_reject_sites[i].mask = masks[i];
    }
}
int psx_ws_masked_reject(uint32_t flags, uint32_t mask) {
    return psx_ws_masked_reject_value(flags, mask, psx_ws_x_margin());
}
int psx_ws_masked_reject_site(uint32_t pc, uint32_t instr,
    uint32_t flags, int vanilla) {
    for (int i = 0; i < ws_masked_reject_n; ++i) {
        const WsMaskedRejectSite *s = &ws_masked_reject_sites[i];
        if (s->address == (pc & 0x1FFFFFFFu) && s->expected == instr)
            return psx_ws_masked_reject(flags, s->mask);
    }
    return vanilla;
}

static WsMaskedRejectSite ws_nclip_sites[WS_EXPLICIT_CULL_SITES_MAX];
static uint8_t ws_nclip_previous[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_nclip_site_count;
static uint64_t ws_nclip_rescues;
uint64_t gpu_ws_native_wide_nclip_rescues(void) { return ws_nclip_rescues; }
void psx_mod_set_native_wide_nclip_sites(const uint32_t* addresses,
    const uint32_t* expected, int count) {
    if (count < 0 || count > WS_EXPLICIT_CULL_SITES_MAX) abort();
    for (int i = 0; i < count; ++i) {
        const uint32_t op = expected[i] >> 26, rt = (expected[i] >> 16) & 31;
        if (!((op == 1 && rt <= 1) || ((op == 6 || op == 7) && rt == 0))) abort();
        ws_nclip_sites[i].address = addresses[i] & 0x1FFFFFFFu;
        ws_nclip_sites[i].expected = expected[i];
        ws_nclip_previous[i] = 0;
    }
    ws_nclip_site_count = count;
    ws_nclip_rescues = 0;
}
void psx_mod_set_native_wide_nclip_previous_site(uint32_t address, uint32_t expected) {
    for (int i = 0; i < ws_nclip_site_count; ++i) {
        if (ws_nclip_sites[i].address == (address & 0x1FFFFFFFu) &&
            ws_nclip_sites[i].expected == expected) {
            ws_nclip_previous[i] = 1;
            return;
        }
    }
    abort();
}
extern int gte_nclip_native_wide_sign(int32_t mac0, int* sign);
extern int gte_nclip_native_wide_previous_sign(int32_t mac0, int* sign);
int psx_ws_nclip_branch(uint32_t pc, uint32_t instr, int32_t mac0, int vanilla) {
    if (psx_ws_x_margin() <= 0 || !gpu_ws_precise_nclip_enabled()) return vanilla;
    for (int i = 0; i < ws_nclip_site_count; ++i) {
        if (ws_nclip_sites[i].address != (pc & 0x1FFFFFFFu) ||
            ws_nclip_sites[i].expected != instr) continue;
        int sign;
        const int valid = ws_nclip_previous[i] ?
            gte_nclip_native_wide_previous_sign(mac0, &sign) :
            gte_nclip_native_wide_sign(mac0, &sign);
        if (!valid) return vanilla;
        const uint32_t op = instr >> 26;
        const int result = op == 6 ? sign <= 0 : op == 7 ? sign > 0 :
            ((instr >> 16) & 1) ? sign >= 0 : sign < 0;
        if (result != vanilla) ++ws_nclip_rescues;
        return result;
    }
    return vanilla;
}

typedef struct {
    uint32_t address;
    uint32_t expected;
    uint32_t result;
} WsCullKeepSite;
static WsCullKeepSite ws_cull_keep_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_cull_keep_n = 0;
void gpu_ws_set_cull_keep_sites(const uint32_t *addresses,
                                const uint32_t *expected,
                                const uint32_t *results, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX) nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_cull_keep_n = nsites;
    for (int i = 0; i < nsites; i++) {
        ws_cull_keep_sites[i].address = addresses[i] & 0x1FFFFFFFu;
        ws_cull_keep_sites[i].expected = expected[i];
        ws_cull_keep_sites[i].result = results[i] ? 1u : 0u;
    }
}
uint32_t psx_ws_cull_keep_result(uint32_t vanilla, uint32_t forced) {
    return psx_ws_x_margin() > 0 ? (forced ? 1u : 0u) : vanilla;
}
int psx_ws_cull_keep_site(uint32_t pc, uint32_t instr, uint32_t vanilla,
                          uint32_t *out) {
    const uint32_t phys = pc & 0x1FFFFFFFu;
    for (int i = 0; i < ws_cull_keep_n; i++) {
        const WsCullKeepSite *site = &ws_cull_keep_sites[i];
        if (site->address != phys || site->expected != instr) continue;
        if (out) *out = psx_ws_cull_keep_result(vanilla, site->result);
        return 1;
    }
    return 0;
}

typedef struct {
    uint32_t address;
    uint32_t expected;
    uint32_t operand;      /* 0 = rs, 1 = rt */
    int32_t  half_extent;
} WsCullScaleSite;
static WsCullScaleSite ws_cull_scale_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_cull_scale_n = 0;
void gpu_ws_set_cull_scale_sites(const uint32_t *addresses,
                                 const uint32_t *expected,
                                 const uint32_t *operands,
                                 const uint32_t *half_extents, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX) nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_cull_scale_n = nsites;
    for (int i = 0; i < nsites; i++) {
        ws_cull_scale_sites[i].address = addresses[i] & 0x1FFFFFFFu;
        ws_cull_scale_sites[i].expected = expected[i];
        ws_cull_scale_sites[i].operand = operands[i] ? 1u : 0u;
        ws_cull_scale_sites[i].half_extent = (int32_t)half_extents[i];
    }
}
/* A camera-space frustum edge authored for a half_extent-pixel half-screen
 * (x < half*z/H) widens to the live view by scaling the bound by
 * (half + margin) / half. 64-bit intermediate; saturates to int32. */
int32_t psx_ws_cull_scale(int32_t bound, int32_t half_extent) {
    const int32_t margin = psx_ws_x_margin();
    if (margin <= 0 || half_extent <= 0) return bound;
    int64_t v = (int64_t)bound * (int64_t)(half_extent + margin) / half_extent;
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    return (int32_t)v;
}
int psx_ws_cull_scale_site(uint32_t pc, uint32_t instr, uint32_t rs_value,
                           uint32_t rt_value, uint32_t *out) {
    const uint32_t phys = pc & 0x1FFFFFFFu;
    for (int i = 0; i < ws_cull_scale_n; i++) {
        const WsCullScaleSite *site = &ws_cull_scale_sites[i];
        if (site->address != phys || site->expected != instr) continue;
        if (site->operand == 0)
            rs_value = (uint32_t)psx_ws_cull_scale((int32_t)rs_value, site->half_extent);
        else
            rt_value = (uint32_t)psx_ws_cull_scale((int32_t)rt_value, site->half_extent);
        const uint32_t funct = instr & 0x3Fu;
        if (out)
            *out = funct == 0x2Au
                ? (((int32_t)rs_value < (int32_t)rt_value) ? 1u : 0u)
                : ((rs_value < rt_value) ? 1u : 0u);
        return 1;
    }
    return 0;
}

typedef struct {
    uint32_t address;
    uint32_t expected;
} WsAngleSite;
static WsAngleSite ws_angle_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_angle_n = 0;
static uint64_t ws_angle_calls = 0;
static uint64_t ws_angle_43_identity = 0;
static uint32_t ws_angle_max_vanilla = 0;
static uint32_t ws_angle_max_widened = 0;

void gpu_ws_set_angle_sites(const uint32_t *addresses,
                            const uint32_t *expected, int nsites) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX)
        nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_angle_n = nsites;
    for (int i = 0; i < nsites; i++) {
        ws_angle_sites[i].address = addresses[i] & 0x1FFFFFFFu;
        ws_angle_sites[i].expected = expected[i];
    }
    ws_angle_calls = 0;
    ws_angle_43_identity = 0;
    ws_angle_max_vanilla = 0;
    ws_angle_max_widened = 0;
}

uint32_t psx_ws_angle_widen(uint32_t vanilla) {
    ws_angle_calls++;
    const int margin = psx_ws_x_margin();
    if (margin <= 0) {
        ws_angle_43_identity++;
        return vanilla;
    }
    const uint32_t widened =
        psx_ws_widen_angle_q12(vanilla, margin);
    if (vanilla > ws_angle_max_vanilla)
        ws_angle_max_vanilla = vanilla;
    if (widened > ws_angle_max_widened)
        ws_angle_max_widened = widened;
    return widened;
}

int psx_ws_angle_site(uint32_t pc, uint32_t instr, uint32_t *out) {
    const uint32_t phys = pc & 0x1FFFFFFFu;
    for (int i = 0; i < ws_angle_n; i++) {
        const WsAngleSite *site = &ws_angle_sites[i];
        if (site->address != phys || site->expected != instr) continue;
        if (out)
            *out = psx_ws_angle_widen(
                (uint32_t)(int32_t)(int16_t)(instr & 0xFFFFu));
        return 1;
    }
    return 0;
}

typedef struct {
    uint32_t address;
    uint32_t expected;
    uint32_t cosine_threshold;
    uint8_t object_reg;
    uint8_t x_reg;
    uint8_t z_reg;
    uint8_t y_reg;
    uint8_t queue_guard;
    uint64_t calls;
    uint64_t identity_43;
    uint64_t vanilla_keep;
    uint64_t visible_keep;
    uint64_t guard_keep;
    uint64_t hysteresis_keep;
    uint64_t outside_reject;
    uint64_t queue_reject;
} WsAspectConeSite;

typedef struct {
    uint32_t site;
    uint32_t object;
    uint32_t last_seen_frame;
    uint8_t active;
} WsAspectConeState;

#define WS_ASPECT_CONE_STATE_CAP 512
static WsAspectConeSite ws_aspect_cone_sites[WS_EXPLICIT_CULL_SITES_MAX];
static int ws_aspect_cone_n = 0;
static uint32_t ws_aspect_cone_forward_addr = 0;
static uint32_t ws_aspect_cone_object_type_offset = 0;
static uint32_t ws_aspect_cone_hysteresis_pixels = 0;
static uint32_t ws_aspect_cone_queue_reserve = 0;
static uint32_t ws_aspect_cone_queue_count_addrs[3];
static uint32_t ws_aspect_cone_queue_capacities[3];
static uint32_t ws_aspect_cone_queue_type_masks[3];
static WsAspectConeState ws_aspect_cone_state[WS_ASPECT_CONE_STATE_CAP];
static int ws_aspect_cone_wide_latched = 0;
static uint64_t ws_aspect_cone_calls = 0;
static uint64_t ws_aspect_cone_43_identity = 0;
static uint64_t ws_aspect_cone_vanilla_keep = 0;
static uint64_t ws_aspect_cone_visible_keep = 0;
static uint64_t ws_aspect_cone_guard_keep = 0;
static uint64_t ws_aspect_cone_hysteresis_keep = 0;
static uint64_t ws_aspect_cone_outside_reject = 0;
static uint64_t ws_aspect_cone_queue_reject = 0;
static uint32_t ws_aspect_cone_queue_highwater[3];

void gpu_ws_set_aspect_cone(const uint32_t *addresses,
                            const uint32_t *expected,
                            const uint32_t *cosine_thresholds,
                            const uint32_t *object_regs,
                            const uint32_t *x_regs,
                            const uint32_t *z_regs,
                            const uint32_t *y_regs,
                            const uint32_t *queue_guards,
                            int nsites,
                            uint32_t forward_addr,
                            uint32_t object_type_offset,
                            uint32_t hysteresis_pixels,
                            uint32_t queue_reserve,
                            const uint32_t queue_count_addrs[3],
                            const uint32_t queue_capacities[3],
                            const uint32_t queue_type_masks[3]) {
    if (nsites < 0) nsites = 0;
    if (nsites > WS_EXPLICIT_CULL_SITES_MAX)
        nsites = WS_EXPLICIT_CULL_SITES_MAX;
    ws_aspect_cone_n = nsites;
    memset(ws_aspect_cone_sites, 0, sizeof(ws_aspect_cone_sites));
    for (int i = 0; i < nsites; i++) {
        ws_aspect_cone_sites[i].address = addresses[i] & 0x1FFFFFFFu;
        ws_aspect_cone_sites[i].expected = expected[i];
        ws_aspect_cone_sites[i].cosine_threshold =
            cosine_thresholds ? cosine_thresholds[i] : 0;
        ws_aspect_cone_sites[i].object_reg =
            (uint8_t)(object_regs ? object_regs[i] & 31u : 0u);
        ws_aspect_cone_sites[i].x_reg =
            (uint8_t)(x_regs ? x_regs[i] & 31u : 0u);
        ws_aspect_cone_sites[i].z_reg =
            (uint8_t)(z_regs ? z_regs[i] & 31u : 0u);
        ws_aspect_cone_sites[i].y_reg =
            (uint8_t)(y_regs ? y_regs[i] & 31u : 0u);
        ws_aspect_cone_sites[i].queue_guard =
            (uint8_t)(!queue_guards || queue_guards[i] != 0);
    }
    ws_aspect_cone_forward_addr = forward_addr;
    ws_aspect_cone_object_type_offset = object_type_offset;
    ws_aspect_cone_hysteresis_pixels = hysteresis_pixels;
    ws_aspect_cone_queue_reserve = queue_reserve;
    for (int i = 0; i < 3; i++) {
        ws_aspect_cone_queue_count_addrs[i] =
            queue_count_addrs ? queue_count_addrs[i] : 0;
        ws_aspect_cone_queue_capacities[i] =
            queue_capacities ? queue_capacities[i] : 0;
        ws_aspect_cone_queue_type_masks[i] =
            queue_type_masks ? queue_type_masks[i] : 0;
    }
    memset(ws_aspect_cone_state, 0, sizeof(ws_aspect_cone_state));
    ws_aspect_cone_wide_latched = 0;
    ws_aspect_cone_calls = 0;
    ws_aspect_cone_43_identity = 0;
    ws_aspect_cone_vanilla_keep = 0;
    ws_aspect_cone_visible_keep = 0;
    ws_aspect_cone_guard_keep = 0;
    ws_aspect_cone_hysteresis_keep = 0;
    ws_aspect_cone_outside_reject = 0;
    ws_aspect_cone_queue_reject = 0;
    memset(ws_aspect_cone_queue_highwater, 0,
           sizeof(ws_aspect_cone_queue_highwater));
}

static WsAspectConeSite *ws_aspect_cone_find(uint32_t address,
                                             uint32_t expected) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    for (int i = 0; i < ws_aspect_cone_n; i++) {
        WsAspectConeSite *site = &ws_aspect_cone_sites[i];
        if (site->address == phys &&
            (expected == 0 || site->expected == expected))
            return site;
    }
    return NULL;
}

static WsAspectConeState *ws_aspect_cone_object_state(uint32_t site,
                                                       uint32_t object) {
    const uint32_t now = (uint32_t)s_frame_count;
    uint32_t slot = (((object >> 2) * 2654435761u) ^
                     ((site >> 2) * 2246822519u)) &
                    (WS_ASPECT_CONE_STATE_CAP - 1u);
    WsAspectConeState *oldest = NULL;
    for (uint32_t probe = 0; probe < 8; probe++) {
        WsAspectConeState *entry =
            &ws_aspect_cone_state[(slot + probe) &
                                  (WS_ASPECT_CONE_STATE_CAP - 1u)];
        if (entry->site == site && entry->object == object) {
            entry->last_seen_frame = now;
            return entry;
        }
        if (entry->object == 0 || now - entry->last_seen_frame > 120u) {
            entry->site = site;
            entry->object = object;
            entry->last_seen_frame = now;
            entry->active = 0;
            return entry;
        }
        if (!oldest ||
            entry->last_seen_frame < oldest->last_seen_frame)
            oldest = entry;
    }
    oldest->site = site;
    oldest->object = object;
    oldest->last_seen_frame = now;
    oldest->active = 0;
    return oldest;
}

static int ws_aspect_cone_queue_has_guard_room(uint32_t object) {
    if (object == 0) return 0;
    const uint8_t type =
        psx_read_byte(object + ws_aspect_cone_object_type_offset);
    if (type >= 32) return 1;
    const uint32_t bit = 1u << type;
    for (int i = 0; i < 3; i++) {
        if ((ws_aspect_cone_queue_type_masks[i] & bit) == 0) continue;
        const uint32_t cap = ws_aspect_cone_queue_capacities[i];
        const uint32_t reserve = ws_aspect_cone_queue_reserve;
        if (cap == 0 || ws_aspect_cone_queue_count_addrs[i] == 0)
            return 1;
        const uint32_t count =
            psx_read_half(ws_aspect_cone_queue_count_addrs[i]);
        return count < ((cap > reserve) ? cap - reserve : 0u);
    }
    return 1;
}

static void ws_aspect_cone_sample_queues(void) {
    /* The game clears each queue at frame start and appends as candidates are
     * accepted. Sample throughout the producer loop so the high-water mark
     * sees the final occupancy rather than only the first zero count. */
    for (int i = 0; i < 3; i++) {
        if (ws_aspect_cone_queue_count_addrs[i] == 0) continue;
        const uint32_t count =
            psx_read_half(ws_aspect_cone_queue_count_addrs[i]);
        if (count > ws_aspect_cone_queue_highwater[i])
            ws_aspect_cone_queue_highwater[i] = count;
    }
}

uint32_t psx_ws_aspect_cone_result(uint32_t site_address, uint32_t vanilla,
                                   uint32_t object, int32_t x, int32_t z,
                                   int32_t y) {
    WsAspectConeSite *site = ws_aspect_cone_find(site_address, 0);
    if (!site) return vanilla;

    ws_aspect_cone_calls++;
    site->calls++;
    const int total_margin = psx_ws_x_margin();
    if (total_margin <= 0) {
        ws_aspect_cone_43_identity++;
        site->identity_43++;
        if (ws_aspect_cone_wide_latched) {
            memset(ws_aspect_cone_state, 0, sizeof(ws_aspect_cone_state));
            ws_aspect_cone_wide_latched = 0;
        }
        return vanilla;
    }
    ws_aspect_cone_wide_latched = 1;
    ws_aspect_cone_sample_queues();

    WsAspectConeState *state =
        ws_aspect_cone_object_state(site->address, object);
    /* The guarded Tomba sites are signed reject predicates: zero falls
     * through to model participation, while one branches to rejection. */
    if (!vanilla) {
        state->active = 1;
        ws_aspect_cone_vanilla_keep++;
        site->vanilla_keep++;
        return 0;
    }

    const int visible_margin =
        (total_margin > ws_cull_guard_pixels)
            ? total_margin - ws_cull_guard_pixels : 0;
    const int32_t fx =
        (int16_t)psx_read_half(ws_aspect_cone_forward_addr + 0u);
    const int32_t fz =
        (int16_t)psx_read_half(ws_aspect_cone_forward_addr + 2u);
    const int32_t fy =
        (int16_t)psx_read_half(ws_aspect_cone_forward_addr + 4u);
    const uint32_t threshold = site->cosine_threshold;

    const int in_visible =
        psx_ws_aspect_cone_contains(x, z, y, fx, fz, fy, threshold,
                                    visible_margin);
    const int in_guard =
        !in_visible && psx_ws_aspect_cone_contains(
            x, z, y, fx, fz, fy, threshold, total_margin);
    const int in_hysteresis =
        !in_visible && !in_guard && state->active &&
        psx_ws_aspect_cone_contains(
            x, z, y, fx, fz, fy, threshold,
            total_margin + (int)ws_aspect_cone_hysteresis_pixels);

    if (!in_visible && !in_guard && !in_hysteresis) {
        state->active = 0;
        ws_aspect_cone_outside_reject++;
        site->outside_reject++;
        return 1;
    }
    if (!in_visible && site->queue_guard &&
        !ws_aspect_cone_queue_has_guard_room(object)) {
        state->active = 0;
        ws_aspect_cone_queue_reject++;
        site->queue_reject++;
        return 1;
    }
    state->active = 1;
    if (in_visible) {
        ws_aspect_cone_visible_keep++;
        site->visible_keep++;
    } else if (in_guard) {
        ws_aspect_cone_guard_keep++;
        site->guard_keep++;
    } else {
        ws_aspect_cone_hysteresis_keep++;
        site->hysteresis_keep++;
    }
    return 0;
}

int psx_ws_aspect_cone_site(CPUState *cpu, uint32_t pc, uint32_t instr,
                            uint32_t vanilla, uint32_t *out) {
    const WsAspectConeSite *site = ws_aspect_cone_find(pc, instr);
    if (!site) return 0;
    if (out) {
        *out = psx_ws_aspect_cone_result(
            pc, vanilla, cpu->gpr[site->object_reg],
            (int32_t)(int16_t)cpu->gpr[site->x_reg],
            (int32_t)(int16_t)cpu->gpr[site->z_reg],
            (int32_t)(int16_t)cpu->gpr[site->y_reg]);
    }
    return 1;
}

/* Netplay presentation-only widescreen (gpu_ws_set_local_view_only): the
 * shared simulation keeps the stock 4:3 cull everywhere; the widened margin
 * exists only while this peer's sandboxed own view is drawn, whose guest-side
 * effects the sandbox discards. */
static int ws_local_view_only = 0;
static int ws_local_view_scope = 0;
void gpu_ws_set_local_view_only(int on) { ws_local_view_only = on ? 1 : 0; }
void gpu_ws_set_local_view_scope(int on) { ws_local_view_scope = on ? 1 : 0; }
int gpu_ws_local_view_only(void) { return ws_local_view_only; }

int psx_ws_x_margin(void) {
    if (ws_local_view_only && !ws_local_view_scope) return 0;
    if (ws_margin_override >= 0) return ws_margin_override;
    /* Native-wide: widen the world-space draw cull by the per-side reveal
     * (== the centering OFFSET in screen px) so the game SUBMITS the geometry
     * that previously fell outside the 4:3 cull window; the wide compositor then
     * rasterizes it into the revealed margins. Same recompiler emit sites as the
     * squash path ([widescreen.cull]); 0 at 4:3 so the cull stays byte-identical. */
    /* Unlike rendering/presentation, do not wait for game-mode detection here.
     * Tomba 2 builds its terrain-cell and actor spawn lists during scene load;
     * returning zero until the first 3D frame permanently bakes a 4:3 frustum
     * into those lists. */
    if (ws_native_wide_configured())
        /* A constant envelope covers both possible edge origins. Do not shrink
         * actor activation as the view slides, or prebuilt spawn lists go stale. */
        return ws_nw_configured_offset() * (ws_view_camera_addr ? 2 : 1)
               + ws_cull_guard_pixels;
    if (!ws_active()) return 0;
    return (160 * (ws_xden - ws_xnum) + ws_xnum / 2) / ws_xnum
           + ws_cull_guard_pixels;
}

int psx_ws_activation_margin(void) {
    const int margin = psx_ws_x_margin();
    return margin > 0 ? margin + ws_activation_guard_pixels : 0;
}

int32_t psx_ws_player_x_bound(int32_t vanilla)
{
    const int margin = psx_ws_x_margin();
    if (margin <= 0) return vanilla;
    return (int32_t)(((int64_t)vanilla * (320 + 2 * margin)) / 320);
}

#define WS_SIGNED_BOUND_MAX 64
static uint32_t ws_signed_bound_addr[WS_SIGNED_BOUND_MAX];
static uint32_t ws_signed_bound_expected[WS_SIGNED_BOUND_MAX];
static int ws_signed_bound_count = 0;
void gpu_ws_set_signed_x_bound_sites(const uint32_t *addresses,
                                     const uint32_t *expected, int count) {
    if (count < 0) count = 0;
    if (count > WS_SIGNED_BOUND_MAX) count = WS_SIGNED_BOUND_MAX;
    ws_signed_bound_count = count;
    for (int i = 0; i < count; i++) {
        ws_signed_bound_addr[i] = addresses[i] & 0x1FFFFFFFu;
        ws_signed_bound_expected[i] = expected[i];
    }
}
int psx_ws_is_signed_x_bound_site(uint32_t pc, uint32_t instr) {
    const uint32_t phys = pc & 0x1FFFFFFFu;
    for (int i = 0; i < ws_signed_bound_count; i++)
        if (ws_signed_bound_addr[i] == phys && ws_signed_bound_expected[i] == instr)
            return 1;
    return 0;
}

/* ---- Capcom 2D background tile-loop widen ([widescreen.bg2d]) --------------
 * Mega Man X5/X6 use a pure-2D sprite engine that renders only a 4:3 (320px) field
 * of view — there is no overscan to "reveal." Its per-layer background renderer
 * (FUN_800270d0) draws `count` 16px tile columns × 16 rows from a start tile
 * column / start screen-x derived from the camera scroll. To produce a TRUE
 * wider FOV we widen that loop so it draws extra columns on BOTH sides of the
 * 320 view, filling the 16:9 reveal margins with real adjacent stage. The three
 * helpers below are hooked at the renderer's count / start-col / start-x
 * instructions by the recompiler (gen-time). They are IDENTITY unless native-
 * wide is engaged on a ~320 screen, so 4:3 (and the engine's own 512 hi-res
 * mode, which already draws 33 columns) stay byte-identical.
 *
 * LEFT = per-side reveal in whole 16px tile columns = ceil(nw_offset / 16). The
 * loop start moves LEFT columns earlier (tile col -LEFT in the 64-col ring,
 * screen-x -LEFT*16) and the count grows by 2*LEFT, so existing columns keep
 * their screen positions (stay aligned with objects/player) while new columns
 * are prepended left and appended right. The 64-column tile ring (mask 0x3f) is
 * far wider than the visible window, so the prepended columns fetch the correct
 * already-streamed adjacent tiles. */
static uint32_t g_bg2d_layer_base = 0x800971F8u;
static uint32_t g_bg2d_ring_base = 0x800A21B8u;
static uint32_t g_bg2d_map_size_addr = 0x800CD338u;
static uint32_t g_bg2d_layer_stride_addr = 0x8008EC10u;
static uint32_t g_bg2d_ring_cols = 64;
static uint32_t g_bg2d_layer_count = 3;
static uint32_t g_bg2d_layer_struct_stride = 0x54;
static uint32_t g_bg2d_packet_cap = 1000;
static int g_bg2d_native_cols = 21;
static int g_bg2d_parent_links = 1;
static uint32_t g_bg2d_host_base, g_bg2d_host_size;
static uint32_t g_bg2d_host_generation;

void gpu_ws_bg2d_set_host_arena(uint32_t base, uint32_t size) {
    g_bg2d_host_base = base & 0x1fffffffu;
    g_bg2d_host_size = size >= 32u && !(base & 31u) ? size : 0u;
}

static uint32_t ws_bg2d_host_packet(void) {
    uint32_t command = gp0_cmd_source_addr & 0x1fffffffu;
    if (!g_bg2d_host_size || command < g_bg2d_host_base + 4u) return 0;
    uint32_t offset = command - g_bg2d_host_base - 4u;
    if ((offset & 31u) || offset > g_bg2d_host_size - 32u) return 0;
    uint32_t packet = command - 4u;
    uint32_t magic = psx_read_word(packet + 28u) & ~GPU_WS_BG2D_MIRROR_X;
    return magic == GPU_WS_BG2D_PACKET_MAGIC || magic == GPU_WS_BG2D_BANK_PACKET_MAGIC ? packet : 0;
}
#define WS_VIEW_LAYERS 8
static int ws_view_layer = -1;
static unsigned ws_view_independent_mask;
static struct {
    uint32_t begin, end, frame;
    WsViewAnchor view;
} ws_view_layers[WS_VIEW_LAYERS];

static WsViewAnchor ws_bg2d_layer_view(int layer) {
    WsViewAnchor v = ws_view_current();
    if (!ws_view_enabled() || layer < 0 || layer >= WS_VIEW_LAYERS ||
        !(ws_view_independent_mask & (1u << layer))) return v;
    uint32_t b = g_bg2d_layer_base + (unsigned)layer * g_bg2d_layer_struct_stride;
    if ((int8_t)psx_read_byte(b + 0x52) >= 0) return v;
    int sx = (int16_t)psx_read_half(b + 0xa);
    int lo = psx_read_byte(b + 0x4d) * 256;
    int hi = (psx_read_byte(b + 0x4e) + 1) * 256;
    int width = ws_disp_w(), extra = ws_nw_offset();
    int frame_width = width + 2 * extra;
    /* Keep the foreground's desired origin until it would expose void in
     * this slower layer. Shift that layer as a whole; do not stretch tiles. */
    if (hi - lo >= frame_width && sx >= lo && sx + width <= hi) {
        int origin = extra + v.shift;
        int max_origin = sx - lo;
        int min_origin = frame_width - (hi - sx);
        if (origin > max_origin) origin = max_origin;
        if (origin < min_origin) origin = min_origin;
        v.shift = origin - extra;
        v.left = origin;
        v.right = 2 * extra - origin;
    } else if (g_bg2d_host_size && hi > lo && hi - lo < frame_width) {
        int pad = frame_width - (hi - lo);
        v.pad_left = pad / 2;
        v.pad_right = pad - v.pad_left;
        v.shift = sx - lo + v.pad_left - extra;
        v.left = sx > lo ? sx - lo : 0;
        v.right = hi > sx + width ? hi - sx - width : 0;
    }
    return v;
}

void gpu_ws_bg2d_begin_view_layer(unsigned layer, uint32_t packet, unsigned independent_mask) {
    if (g_bg2d_host_size) {
        ws_view_sample();
        if (layer == 0) ++g_bg2d_host_generation;
    }
    if (!ws_view_enabled() || layer >= WS_VIEW_LAYERS) return;
    ws_view_independent_mask = independent_mask;
    ws_view_layer = (int)layer;
    ws_view_layers[layer].begin = packet & 0x1fffffffu;
    ws_view_layers[layer].end = ws_view_layers[layer].begin;
    ws_view_layers[layer].frame = (uint32_t)s_frame_count;
}
int gpu_ws_bg2d_get_view(unsigned layer, WsViewAnchor *view) {
    if (!view || !ws_native_wide_active() || ws_disp_w() > 384) return 0;
    *view = ws_bg2d_layer_view((int)layer);
    return ws_disp_w();
}
void gpu_ws_bg2d_end_view_layer(unsigned layer, uint32_t packet) {
    if (!ws_view_enabled() || layer >= WS_VIEW_LAYERS) return;
    ws_view_layers[layer].end = packet & 0x1fffffffu;
    ws_view_layers[layer].view = ws_bg2d_layer_view((int)layer);
    ws_view_layer = -1;
}
static WsViewAnchor ws_view_packet(void) {
    uint32_t packet = ws_bg2d_host_packet();
    if (packet) {
        WsViewAnchor v = ws_view_current();
        uint32_t metadata = psx_read_word(packet + 16u);
        v.shift = (psx_read_word(packet + 28u) & ~GPU_WS_BG2D_MIRROR_X) ==
            GPU_WS_BG2D_BANK_PACKET_MAGIC ? (int16_t)metadata : (int32_t)metadata;
        v.pad_left = (int32_t)psx_read_word(packet + 20u);
        v.pad_right = (int32_t)psx_read_word(packet + 24u);
        v.left = ws_nw_offset() + v.shift - v.pad_left;
        v.right = 2 * ws_nw_offset() - v.left - v.pad_left - v.pad_right;
        return v;
    }
    if (!ws_view_enabled()) return ws_view_current();
    uint32_t addr = gp0_cmd_source_addr & 0x1fffffffu;
    for (unsigned i = 0; i < WS_VIEW_LAYERS; i++)
        if ((uint32_t)s_frame_count - ws_view_layers[i].frame <= 1u &&
            addr >= ws_view_layers[i].begin && addr < ws_view_layers[i].end)
            return ws_view_layers[i].view;
    return ws_view_current();
}

void gpu_ws_bg2d_configure(uint32_t layer_base, uint32_t ring_base,
                           uint32_t map_size_addr, uint32_t layer_stride_addr,
                           uint32_t ring_cols, uint32_t layer_count,
                           uint32_t layer_struct_stride, uint32_t packet_cap) {
    g_bg2d_layer_base = layer_base;
    g_bg2d_ring_base = ring_base;
    g_bg2d_map_size_addr = map_size_addr;
    g_bg2d_layer_stride_addr = layer_stride_addr;
    g_bg2d_ring_cols = ring_cols;
    g_bg2d_layer_count = layer_count;
    g_bg2d_layer_struct_stride = layer_struct_stride;
    g_bg2d_packet_cap = packet_cap;
}

void gpu_ws_bg2d_set_parent_links(int on) {
    g_bg2d_parent_links = on ? 1 : 0;
}

static int ws_bg2d_left_cols(void) {
    if (g_bg2d_host_size) return 0;
    if (!ws_native_wide_active()) return 0;
    /* Only the ~320 gameplay mode; the engine's 512 hi-res mode (title) draws
     * its own 33 columns and centres itself — never double-shift it. */
    if (ws_disp_w() > 384) return 0;
    int off = ws_bg2d_layer_view(ws_view_layer).left;
    if (off <= 0) return 0;
    return (off + 15) / 16;             /* ceil to whole tile columns */
}
static int ws_bg2d_right_cols(void) {
    if (g_bg2d_host_size) return 0;
    if (!ws_native_wide_active() || ws_disp_w() > 384) return 0;
    return (ws_bg2d_layer_view(ws_view_layer).right + 15) / 16;
}
/* Column count: base + both-side reveal. */
static void mmx6_bg_refill_tick(void);   /* defined below (ring-freshness fix) */
int psx_ws_bg2d_cols(int base) {
    g_bg2d_native_cols = base;
    mmx6_bg_refill_tick();
    return base + ws_bg2d_left_cols() + ws_bg2d_right_cols();
}
/* Start tile column: refill before the first column is consumed, then begin LEFT
 * earlier in the ring. The count hook retains the same tick for layouts that load
 * their loop bound before calculating the starting column. */
int psx_ws_bg2d_startcol(int col, unsigned mask) {
    mmx6_bg_refill_tick();
    return (col - ws_bg2d_left_cols()) & (int)mask;
}
/* Start screen-x: LEFT*16 further left. */
int psx_ws_bg2d_startx(int x)        { return x - ws_bg2d_left_cols() * 16; }

/* Tile-RING STREAMER widen (same [widescreen.bg2d], FUN_800273e4). The renderer
 * above now draws LEFT extra columns each side, but the engine's 64-column tile
 * ring is only refreshed at its leading edge: the streamer re-streams ONE column
 * per side per frame at world-X scrollX-16 (left) and scrollX+16+width (right),
 * so the columns the widened renderer reaches are never populated (they show
 * empty=black or stale=old tiles, the "stale margins"). Push the streamed left/
 * right edge out by LEFT*16 so the ring stays valid across the widened window as
 * the camera scrolls (the incremental streamer fills each newly-leading column).
 * Identity (no extra streaming) at 4:3 / 512 hi-res, so the ring is byte-identical
 * there. The 64-col ring has ample slack (visible ~21 cols) for ±LEFT more. */
int psx_ws_bg2d_stream_left(int x)  { return x - ws_bg2d_left_cols() * 16; }
int psx_ws_bg2d_stream_right(int x) { return x + ws_bg2d_right_cols() * 16; }
int psx_ws_bg2d_undercap(int counter, int native_cap) {
    int cap = ws_bg2d_left_cols() + ws_bg2d_right_cols() > 0 ? (int)g_bg2d_packet_cap : native_cap;
    return counter < cap;
}

/* NOT MMX6-only, and NOT dead -- see the survey note on the declarations in
 * runtime/include/gpu.h. Live generated code in CrashBash, MegaManX5 and Tsumu
 * calls these, and seven repos call psx_ws_mmx6_bg_undercap below. The mmx6 in
 * the name is historical; the recompiler emits these names for any title with
 * a [widescreen.bg2d] block. Renaming means touching code_generator.cpp, which
 * rolls the overlay cache tag for every game. */
int psx_ws_mmx6_bg_cols(int base)       { return psx_ws_bg2d_cols(base); }
int psx_ws_mmx6_bg_startcol(int col)    { return psx_ws_bg2d_startcol(col, 0x3fu); }
int psx_ws_mmx6_bg_startx(int x)        { return psx_ws_bg2d_startx(x); }
int psx_ws_mmx6_bg_stream_left(int x)   { return psx_ws_bg2d_stream_left(x); }
int psx_ws_mmx6_bg_stream_right(int x)  { return psx_ws_bg2d_stream_right(x); }

/* Called at entry to MMX6's full tile-ring initializer (FUN_800269F4). The
 * independent layers invoke it only when stage/background data is dirty; gate
 * by frame because up to three layers initialize together. This is the exact
 * point where pixels retained from the old stage cease to be meaningful. */
void psx_ws_mmx6_bg_stage_init(void) {
    static uint32_t last_frame = 0xFFFFFFFFu;
    uint32_t frame = (uint32_t)s_frame_count;
    if (!ws_clear_reveal || ws_mode != 2 || frame == last_frame) return;
    last_frame = frame;
    g_mmx6_void_generation++;
    /* A host tile arena rebuilds its backbuffer every submission. A scrolling
     * effect can initialize a ring every frame (MMX6 rain and museum layers);
     * clearing BOTH display bands here erases the already-rendered frontbuffer.
     * Its next draw already clears reveal strips, using host_generation. */
    if (g_bg2d_host_size) return;
    ws_clear_all_reveal_margins();
}

/* Retired packet-buffer relocation hooks. These symbols remain part of the
 * generated-code ABI; their bodies preserve the committed guest-widen path. */
int psx_ws_mmx6_bg_bufbase(int addr) {
    return addr;
}

int psx_ws_mmx6_bg_undercap(int counter) {
    return psx_ws_bg2d_undercap(counter, 1000);
}

/* ===== MMX6 BG tile-ring freshness fix ([widescreen.bg2d]) ================== *
 * The widened renderer (FUN_800270d0) draws LEFT extra tile columns on each side of
 * the 320 view, but the engine's streamer (FUN_800273e4 -> FUN_800274a0) only
 * refreshes ONE column per side per frame at the leading edge. On scene entry or
 * fast horizontal scroll the inner reveal columns therefore hold STALE tiles from a
 * previous scroll position (different scrollY) -> real-but-misplaced tiles spilling
 * up-and-left (the "staircase"). 4:3 is unaffected (it draws only the 21 columns the
 * engine keeps streamed). Fix: before the renderer draws, re-stream EVERY tile column
 * of the widened window for all 3 layers, so the render window and the ring-refresh
 * window are the same window. This is a faithful clone of FUN_800274a0's inner
 * metatile->ring fill, MINUS the scrollX wrap-bookkeeping writes (those drive the
 * engine's own incremental streamer; perturbing them would desync its leading edge).
 * Off-map columns fill tile 0 (the renderer skips them) = the engine's own void; at a
 * horizontal map-loop seam (engine early-return) we skip the column. Gated on
 * native-wide + ~320 mode, so 4:3 and the 512 hi-res title stay byte-identical. */
extern void psx_write_half(uint32_t addr, uint16_t val);

static void bg2d_clear_column(int layer, int ringcol, int worldY) {
    int rows = 0x12;
    if (worldY < 0) { worldY = 0; rows = 0x11; }
    int wrapY = worldY - (((worldY < 0) ? worldY + 0x1ff : worldY) >> 9) * 0x200;
    int ringrow = ((wrapY < 0) ? wrapY + 0xf : wrapY) >> 4;
    uint32_t ringbase = g_bg2d_ring_base
                      + (uint32_t)layer * (g_bg2d_ring_cols * 64u);
    for (int i = 0; i < rows; i++) {
        uint32_t cell = ringbase
                      + (uint32_t)((ringcol & (int)(g_bg2d_ring_cols - 1u)) * 2)
                      + (uint32_t)((ringrow & 0x1f) * (int)(g_bg2d_ring_cols * 2u));
        psx_host_write_half(cell, 0);
        ringrow = (ringrow + 1) & 0x1f;
    }
}

/* Faithful clone of FUN_800274a0's inner fill: stream one tile column at guest pixel
 * (worldX, worldY) of `layer` into the 64x32 ring. Compares mirror the engine's sltu
 * (UNSIGNED). When write!=0 the tiles are written to the ring; when write==0 they are
 * compared against the live ring (validation), counting *cmp_total / *cmp_bad. Returns
 * 1 if the column was streamed, 0 if skipped (map-loop seam early-return). */
static int bg2d_fill_column(int layer, int worldX, int worldY, int scrollX, int write,
                             int *cmp_total, int *cmp_bad) {
    /* The guest column streamer returns immediately for negative world X. Do
     * not wrap it into the far edge of the map, which produces repeated blocks
     * in the left reveal near a level boundary. */
    if (worldX < 0) return 0;
    uint32_t lbase = g_bg2d_layer_base + (uint32_t)layer * g_bg2d_layer_struct_stride;
    int rows = 0x12;
    if (worldY < 0) { worldY = 0; rows = 0x11; }
    int32_t metaCol = ((worldX < 0) ? worldX + 0xff : worldX) >> 8;     /* t1 */
    int32_t metaRow = ((worldY < 0) ? worldY + 0xff : worldY) >> 8;     /* t4 */
    int worldPeriod = (int)g_bg2d_ring_cols * 16;
    int worldShift = 0;
    for (int n = worldPeriod; n > 1; n >>= 1) worldShift++;
    int wrapX = worldX
              - (((worldX < 0) ? worldX + worldPeriod - 1 : worldX) >> worldShift)
                * worldPeriod;
    int colsh = ((wrapX < 0) ? wrapX + 0xf : wrapX) >> 4;               /* t7 = wrapX>>4 */
    int ringcol = colsh & (int)(g_bg2d_ring_cols - 1u);
    int tcolInMeta = colsh & 0xf;                                       /* s0 = t7 & 0xf */
    int wrapY = worldY - (((worldY < 0) ? worldY + 0x1ff : worldY) >> 9) * 0x200;
    int ringrow = ((wrapY < 0) ? wrapY + 0xf : wrapY) >> 4;             /* a2 */
    int trowInMeta = ringrow & 0xf;                                     /* t3 */

    int mapLeft  = psx_read_byte(lbase + 0x4d);
    int mapRight = psx_read_byte(lbase + 0x4e);
    /* C integer division toward zero makes worldX -255..-1 resolve to metatile
     * column 0 below. The native 4:3 streamer never requests those coordinates,
     * but the widened left window does at a stage starting on mapLeft. Treat
     * them as finite-map void before the engine's loop/wrap bookkeeping. */
    if (worldX < mapLeft * 0x100)
        return 2;
    if ((uint32_t)metaCol < (uint32_t)mapLeft) {
        metaCol = metaCol + 1 + (mapRight - mapLeft);
        if (scrollX <= (mapLeft - 1) * 0x100) {
            /* The native renderer never sees this column, so the engine leaves
             * its ring slot alone. Report finite-map void to the DMA-start
             * cleanup without mutating ring slots canonical draws can alias. */
            return 2;   /* finite-map void, not merely a normal streamed column */
        }
    }
    if ((uint32_t)mapRight < (uint32_t)metaCol) {
        metaCol = (metaCol - 1 - mapRight) + mapLeft;
        if ((mapRight + 1) * 0x100 <= scrollX) {
            return 2;
        }
    }
    int mapW = psx_read_byte(g_bg2d_map_size_addr);
    uint32_t mapBase  = psx_read_word(0x1F800004u);
    uint32_t metaBase = psx_read_word(0x1F800008u);
    if (mapBase == 0 || metaBase == 0 || mapW <= 0) return 0;
    int layerStride = (uint16_t)psx_read_half(g_bg2d_layer_stride_addr);

    /*
     * The Capcom streamer has no map-height field or vertical bounds check.
     * After normalizing negative worldY to zero it indexes rows as
     *   layer*stride + mapW*metaRow + metaCol
     * directly. Treating map_size_addr+1 as a guessed height rejects every
     * MMX4 column because that adjacent byte is zero.
     */
    int metaIdx = psx_read_byte(
        mapBase + (uint32_t)(layer * layerStride + mapW * metaRow + metaCol));

    uint32_t ringbase = g_bg2d_ring_base
                      + (uint32_t)layer * (g_bg2d_ring_cols * 64u);
    for (int i = 0; i < rows; i++) {
        uint32_t cell = ringbase + (uint32_t)(ringcol * 2)
                      + (uint32_t)((ringrow & 0x1f) * (int)(g_bg2d_ring_cols * 2u));
        uint16_t tile = psx_read_half(metaBase + (uint32_t)metaIdx * 0x200u
                                      + (uint32_t)(trowInMeta * 0x20) + (uint32_t)(tcolInMeta * 2));
        if (write) {
            psx_host_write_half(cell, tile);
        } else if (cmp_total) {
            (*cmp_total)++;
            if (psx_read_half(cell) != tile && cmp_bad) (*cmp_bad)++;
        }
        ringrow = (ringrow + 1) & 0x1f;
        if (++trowInMeta == 0x10) {
            trowInMeta = 0;
            metaRow++;
            metaIdx = psx_read_byte(
                mapBase +
                (uint32_t)(layer * layerStride + mapW * metaRow + metaCol));
        }
    }
    return 1;
}

static int  g_mmx6_freshfix   = 1;   /* re-stream the widened window each frame (the fix) */
static long g_mmx6_refill_cols = 0;  /* columns streamed in the last refill (diag) */
static uint32_t g_mmx6_refill_frame = 0xFFFFFFFFu;  /* frame of the last refill (once/frame) */
void gpu_ws_mmx6_set_freshfix(int on) { g_mmx6_freshfix = on ? 1 : 0; }
int  gpu_ws_mmx6_freshfix_get(void)   { return g_mmx6_freshfix; }
long gpu_ws_mmx6_refill_cols(void)    { return g_mmx6_refill_cols; }

/* Re-stream every tile column of the widened window for every BG layer. Independent
 * layers use their own scroll; parent-linked layers use the same combined scroll
 * that the renderer uses to index their ring. The guest's incremental streamer
 * skips linked layers because their native 4:3 window is prefilled, but the newly
 * revealed columns still need to be populated here. worldY = scrollY - 0x10 mirrors
 * the driver. ci spans the renderer's widened span [-LEFT, 20+LEFT].
 * No-op at 4:3 / 512 hi-res (left==0). Idempotent over the native columns the engine
 * already streamed correctly, so re-streaming them is harmless. */
void psx_ws_mmx6_bg_refill_all(void) {
    if (g_bg2d_host_size) return;
    if (!g_mmx6_freshfix) return;
    if (!ws_native_wide_active() || ws_disp_w() > 384) return;
    if (ws_nw_offset() <= 0) return;
    g_mmx6_refill_cols = 0;
    for (uint32_t layer = 0; layer < g_bg2d_layer_count; layer++) {
        uint32_t lbase = g_bg2d_layer_base + layer * g_bg2d_layer_struct_stride;
        int sx = (int16_t)psx_read_half(lbase + 0xa);
        int sy = (int16_t)psx_read_half(lbase + 0xe);
        int8_t parent = (int8_t)psx_read_byte(lbase + 0x52);
        if (g_bg2d_parent_links &&
            parent >= 0 && (uint32_t)(uint8_t)parent < g_bg2d_layer_count) {
            uint32_t pbase = g_bg2d_layer_base
                           + (uint32_t)(uint8_t)parent * g_bg2d_layer_struct_stride;
            sx += (int16_t)psx_read_half(pbase + 0xa);
            sy += (int16_t)psx_read_half(pbase + 0xe);
        }
        WsViewAnchor layer_view = ws_bg2d_layer_view((int)layer);
        int left = (layer_view.left + 15) / 16;
        int right = (layer_view.right + 15) / 16;
        int sxr = sx;
        if (sxr < 0) sxr += 0xf;
        int start_col = sxr >> 4;
        for (int ci = -left; ci < g_bg2d_native_cols + right; ci++) {
            int world_x = sx + ci * 16;
            if (world_x < 0) {
                bg2d_clear_column((int)layer, start_col + ci, sy - 0x10);
                continue;
            }
            if (bg2d_fill_column((int)layer, world_x, sy - 0x10,
                                 sx, 1, NULL, NULL))
                g_mmx6_refill_cols++;
        }
    }
}

/* Trigger the once-per-frame widened-window refill. Both renderer setup hooks call
 * this so differing instruction order between games still refreshes the ring before
 * the first widened column is consumed. */
static void mmx6_bg_refill_tick(void) {
    uint32_t f = (uint32_t)s_frame_count;
    if (f == g_mmx6_refill_frame) return;
    g_mmx6_refill_frame = f;
    ws_view_sample();
    psx_ws_mmx6_bg_refill_all();
}

/* Validation harness (debug mmx6_freshfix {validate:1}): for the NATIVE 21 columns —
 * which the engine streams correctly — run the clone in compare-only mode against the
 * live ring. Proves the clone is byte-exact before the refill (which overwrites the
 * ring) is trusted. Returns total cells compared; *bad = mismatches. */
int gpu_ws_mmx6_validate(int *bad_out) {
    int total = 0, bad = 0;
    for (uint32_t layer = 0; layer < g_bg2d_layer_count; layer++) {
        uint32_t lbase = g_bg2d_layer_base + layer * g_bg2d_layer_struct_stride;
        int sx = (int16_t)psx_read_half(lbase + 0xa);
        int sy = (int16_t)psx_read_half(lbase + 0xe);
        int8_t parent = (int8_t)psx_read_byte(lbase + 0x52);
        if (g_bg2d_parent_links &&
            parent >= 0 && (uint32_t)(uint8_t)parent < g_bg2d_layer_count) {
            uint32_t pbase = g_bg2d_layer_base
                           + (uint32_t)(uint8_t)parent * g_bg2d_layer_struct_stride;
            sx += (int16_t)psx_read_half(pbase + 0xa);
            sy += (int16_t)psx_read_half(pbase + 0xe);
        }
        for (int ci = 0; ci < g_bg2d_native_cols; ci++)
            bg2d_fill_column((int)layer, sx + ci * 16, sy - 0x10,
                             sx, 0, &total, &bad);
    }
    if (bad_out) *bad_out = bad;
    return total;
}

/* Guarded signed pixel bounds expand by one live margin, with identity at 4:3. */
int32_t psx_ws_screen_x_bound(int32_t vanilla) {
    int32_t margin = psx_ws_x_margin();
    return vanilla < 0 ? vanilla - margin : vanilla > 0 ? vanilla + margin : 0;
}

/* Shared render-funnel screen-X cull widening ([widescreen.cull] auto_screen_x),
 * called identically by the gcc emit and the interpreter so every
 * overlay execution path widens the same way. sx = the GTE screen-X as loaded by
 * the guest's lhu (low 16 bits significant); imm = the original bound (0x140 /
 * 0x141). Returns the sltiu verdict (1 = on-screen/keep). Sign-extends sx and
 * shifts by +margin so BOTH 16:9 margins pass; at 4:3 margin==0 so it reduces
 * bit-for-bit to the vanilla `(uint16)sx < imm`. */
int psx_ws_cull_sltiu(uint32_t sx, uint32_t imm) {
    int m = ws_cull_xm();
    return ((uint32_t)((int32_t)(int16_t)(uint16_t)sx + m)
            < (uint32_t)((int32_t)imm + 2 * m)) ? 1 : 0;
}

/* Signed right-edge widen for the min/max funnel idiom (`slti v, minSX, W`):
 * the paired LEFT edge is a separate bltz (psx_ws_cull_bltz below), so this
 * bound moves out by ONE margin only. Operand is an already sign-extended /
 * computed 32-bit screen X. Identity at margin 0 (4:3). */
int psx_ws_cull_slti(uint32_t sx, uint32_t imm) {
    return ((int32_t)sx < (int32_t)imm + ws_cull_xm()) ? 1 : 0;
}

/* Signed fixed lower-bound widen (`slti v, x, -W`): move the reject edge left
 * by one live reveal margin. The encoded immediate must be sign-extended. */
int psx_ws_cull_slti_lower(uint32_t sx, uint32_t imm) {
    int32_t bound = (int32_t)(int16_t)(uint16_t)imm;
    return ((int32_t)sx < bound - psx_ws_x_margin()) ? 1 : 0;
}

/* Signed left-edge widen for the funnel's `bltz maxSX, reject`: reject only
 * when the prim ends left of the REVEALED edge (maxSX < -margin). Returns the
 * branch predicate. Identity at margin 0 (4:3). */
int psx_ws_cull_bltz(uint32_t v) {
    return ((int32_t)v < -ws_cull_xm()) ? 1 : 0;
}
/* The height compare paired with an auto_screen_x width compare (`sltiu rt,
 * SY, H`, H from screen_h_imms): widened by the overdraw on both edges only
 * (widescreen never moves Y). Identity at 0. */
int psx_ws_cull_sltiu_y(uint32_t sy, uint32_t imm) {
    const int m = ws_od(1);
    return ((uint32_t)((int32_t)(int16_t)(uint16_t)sy + m)
            < (uint32_t)((int32_t)imm + 2 * m)) ? 1 : 0;
}
int psx_ws_cull_vxrange(uint32_t x, uint32_t imm) {
    int32_t margin = psx_ws_x_margin();
    uint32_t bound = (uint32_t)(int32_t)(int16_t)(uint16_t)imm;
    return (((x + (uint32_t)margin) & 0xFFFFu) <
            (bound + 2u * (uint32_t)margin)) ? 1 : 0;
}

/* ---- Cull signature configuration ([widescreen.cull] screen_w_imms /
 * screen_h_imms). The width/height immediates are per-game (Tomba: 0x140/0x141
 * + 0xE0/0xF1 on a 320 display; Ape Escape: 0x181 + 0xF1 on 368). Defaults
 * keep the original Tomba signature so existing configs are unchanged. The
 * sets are consulted by the shared detector on every backend (interp;
 * the recompiler reads the same config at gen time). */
static uint32_t ws_cull_w_imms[8] = { 0x140, 0x141 };
static int      ws_cull_w_n = 2;
static uint32_t ws_cull_h_imms[8] = { 0xE0, 0xF1 };
static int      ws_cull_h_n = 2;
void gpu_ws_set_cull_imms(const uint32_t *w, int nw, const uint32_t *h, int nh) {
    if (w && nw > 0) {
        if (nw > 8) nw = 8;
        for (int i = 0; i < nw; i++) ws_cull_w_imms[i] = w[i];
        ws_cull_w_n = nw;
    }
    if (h && nh > 0) {
        if (nh > 8) nh = 8;
        for (int i = 0; i < nh; i++) ws_cull_h_imms[i] = h[i];
        ws_cull_h_n = nh;
    }
}
int psx_ws_is_cull_w_imm(uint32_t imm) {
    return psx_ws_cull_imm_in(imm, ws_cull_w_imms, ws_cull_w_n);
}
int psx_ws_is_cull_h_imm(uint32_t imm) {
    return psx_ws_cull_imm_in(imm, ws_cull_h_imms, ws_cull_h_n);
}

/* ---- Runtime gates for the pattern-scanned widescreen hooks. The interp
 * derives widen sites by scanning live code; that derivation must honor
 * the SAME per-game [widescreen.cull] opt-ins the recompiler emit does. These
 * default OFF: a title that never opted in must never have its code
 * pattern-scanned and rewritten (an ungated backdrop false positive rewrites a
 * live GPR = wild-jump fatal — the exact class this gate closes). Set from
 * game.toml at startup (main.cpp). */
static int ws_auto_cull_on_cfg = 0;
static int ws_auto_backdrop_on_cfg = 0;
void gpu_ws_set_auto_hooks(int cull_on, int backdrop_on) {
    ws_auto_cull_on_cfg     = cull_on ? 1 : 0;
    ws_auto_backdrop_on_cfg = backdrop_on ? 1 : 0;
}
int psx_ws_auto_cull_on(void) { return ws_auto_cull_on_cfg; }

/* Detect the GTE screen-extent trivial-reject signature in a run of
 * instruction words: at least one width compare AND one height compare
 * (slti or sltiu, immediates from the configured sets). Lets the
 * interpreter gate the cull-widening to real render funnels (a lone width
 * compare elsewhere must stay vanilla). Same shared scan the recompiler's
 * func_has_screen_extent_cull uses (ws_cull_detect.h). */
int psx_ws_func_has_screen_cull(const uint32_t *words, int n) {
    return psx_ws_cull_scan(words, n, ws_cull_w_imms, ws_cull_w_n,
                            ws_cull_h_imms, ws_cull_h_n);
}

/* Classify words[idx] as an X left-edge reject bltz (shared structural
 * detector, runtime imm sets). Caller qualifies the window first. */
int psx_ws_cull_bltz_at(const uint32_t *words, int n, int idx) {
    return psx_ws_cull_bltz_here(words, n, idx, ws_cull_w_imms, ws_cull_w_n);
}

/* Widescreen backdrop screen-X correction ([widescreen.backdrop] x_sites).
 * The parallax 2D backdrop layer (ocean/cloud/mountain/grass — overlay actor
 * handlers e.g. 0x801216BC) computes its screen-X in pure integer math
 * (screenX = (worldX - camX) >> parallax) and NEVER goes through the GTE, so
 * the GTE X-squash (gte_set_display_aspect) that gives 3D the wider 16:9 FOV
 * does not touch it. The recompiler emits this on each handler's final
 * screenX store so the backdrop is squashed by the SAME factor around the
 * screen centre: a far piece whose 4:3 screenX sat past the 320px edge (and
 * was GPU-clipped → the blue void / half-rectangles at the edges) is pulled
 * in to cover the revealed FOV. Identity at 4:3 / boot / FMV / full-2D (the
 * exact ws_active() predicate the GTE squash uses), so one build serves both.
 * In native-wide mode there is nothing to squash (the 4:3 frame is presented
 * with side reveal instead), so the same sites are STRETCHED about the screen
 * centre by (disp_w + nw_extra) / disp_w: a 4:3-authored backdrop then covers
 * the widened frame instead of leaving unpainted margins (Xenogears battle
 * mountain panels — pre-calculated POLY_FT4 screen coords stored by main-EXE
 * `sh` sites, wtrace-evidenced).
 * x is the int16 screenX the handler was about to store. */
int psx_ws_backdrop_x(int x) {
    if (ws_native_wide_active()) {
        int32_t X = ws_disp_x();
        int32_t W = (int32_t)ws_disp_w();
        int32_t extra = ws_nw_extra();
        int32_t cx = W / 2;
        int32_t d = (int16_t)x - X - cx;
        return (int)(X + cx + (d * (W + extra) + (d >= 0 ? W / 2 : -W / 2)) / W);
    }
    if (!ws_active()) return (int16_t)x;
    int32_t cx = ws_disp_x() + ws_disp_w() / 2;   /* screen centre (=160 @ 320) */
    return ws_scale_about((int16_t)x, cx);
}

/* Backdrop PRELOAD predicate + value substitution ([widescreen.cull]
 * auto_backdrop). The recompiler/interp detect each scrolling-backdrop
 * column-window generator (see ws_backdrop_detect.h) and route its window START
 * and END bounds through psx_ws_backdrop_value(). When native-wide is engaged we
 * WIDEN the camera-tracked window by the live viewport reveal: the START (left) bound
 * moves left by `margin`, the END (right) bound moves right by `margin`, where
 * margin is the per-side reveal in COLUMNS. The window of window_cols columns
 * spans (at least) the full display width, so reveal_cols = window_cols *
 * reveal_px / display_px = window_cols * nw_offset / disp_w (+slack). This draws
 * ONLY the now-visible columns (no whole-row overdraw, no fixed cap), and the
 * generator's own low/high clamps still bound it at the level edges. Gated on
 * native-wide only (NOT squash: that path uses psx_ws_backdrop_x()). Returns
 * `orig` unchanged at 4:3 / squash / boot / FMV, so 4:3 stays byte-identical. */
int psx_ws_backdrop_preload(void) {
    return ws_auto_backdrop_on_cfg && ws_native_wide_active();
}

/* Live-tunable widen amount (set via the `ws_backdrop_margin` debug command):
 *   -2  => adaptive: live viewport reveal plus the screen-space guard
 *   -1  => WHOLE-ROW preload: START->0, END->extent-1 (diagnostic)
 *    0  => identity: no widening, native-wide still on (A/B the effect)
 *   >0  => widen the camera-tracked window by N columns each side (bounded)
 * Keep the default inert: a title must opt in after validating its producers.
 * The debug override permits A/B testing without a rebuild. */
int g_ws_bd_margin = 0;
void gpu_ws_set_adaptive_backdrop_preload(int enabled) {
    g_ws_bd_margin = enabled ? -2 : 0;
}
/* Set to 1 by the interpreter around its psx_ws_backdrop_value call so the value
 * function does NOT also ring-note: the interp records a richer entry (live
 * extent / camera / DL count). Native cache-DLL calls leave it 0, so the value
 * function notes them itself -- that is how the ring sees native execution. */
int g_ws_bd_from_interp = 0;

uint32_t psx_ws_backdrop_value(uint32_t orig, int is_end, int window_cols) {
    if (!psx_ws_backdrop_preload()) return orig;   /* 4:3 -> byte-identical */
    if (window_cols < 0) window_cols = -window_cols;
    int      m   = g_ws_bd_margin;
    int      adaptive = m == -2;
    if (adaptive)
        m = psx_ws_backdrop_columns(window_cols, psx_ws_x_margin(),
                                    (int)ws_disp_w());
    int      from_interp = g_ws_bd_from_interp;
    g_ws_bd_from_interp = 0;                        /* consume one-shot flag */
    uint32_t finalv;
    if (adaptive) {
        finalv = psx_ws_backdrop_bound(orig, is_end, m);
    } else if (m < 0) {
        /* Whole-row: the generator's shared clamps (START<0 -> 0; END>=extent ->
         * extent-1) pin the loop to [0, extent-1]. Bounded because `extent` is a
         * byte and the DL count is a byte (row <= 255 cols). */
        enum { WS_BD_END_SENTINEL = 0x7FFF };
        finalv = is_end ? (uint32_t)WS_BD_END_SENTINEL : 0u;
    } else if (m == 0) {
        finalv = orig;                              /* effect off (still recorded) */
    } else {
        finalv = is_end ? (orig + (uint32_t)m) : (orig - (uint32_t)m);
    }
    /* Single chokepoint for every path (interp hook, native cache-DLL via the
     * overlay callback). The interp records its own richer entry, so skip
     * here when it set the flag; native-DLL calls (flag 0) are recorded here with
     * pc/extent/camera = 0 -- which is how the ring sees native execution. */
    if (!from_interp)
        psx_ws_backdrop_ring_note(0u, is_end ? 2 /*WS_BD_END*/ : 1 /*WS_BD_START*/,
                                  window_cols, orig, finalv, 0, 0, 0, 0u, 0u);
    return finalv;
}

/* ---- auto_backdrop diagnostic ring (always-on; see BACKDROP_PRELOAD.md) -----
 * Every backdrop-window rewrite (interp or native) is recorded here so the live
 * tile-row extent, the camera-tracked bounds, and exactly WHICH of a scene's
 * windows fire can be read back via the `ws_backdrop_ring` debug command --
 * query the ring for the window of interest, never pause/step. The recorder is
 * fed by the interpreter (which has the live CPU state: s7=extent, the DL count,
 * and the scratchpad camera-X) at the rewrite site. */
typedef struct {
    uint32_t frame, pc, orig, finalv, base, dl;
    int      kind, wcols, extent, camx, count;
} WsBdRingEnt;
#define WS_BD_RING_CAP 512
static WsBdRingEnt s_bd_ring[WS_BD_RING_CAP];
static uint32_t    s_bd_ring_seq = 0;

void psx_ws_backdrop_ring_note(uint32_t pc, int kind, int wcols, uint32_t orig,
                               uint32_t finalv, int extent, int camx, int count,
                               uint32_t base, uint32_t dl) {
    WsBdRingEnt *e = &s_bd_ring[s_bd_ring_seq & (WS_BD_RING_CAP - 1)];
    e->frame  = (uint32_t)s_frame_count; e->pc = pc; e->kind = kind; e->wcols = wcols;
    e->orig   = orig; e->finalv = finalv; e->extent = extent; e->camx = camx; e->count = count;
    e->base   = base; e->dl = dl;
    s_bd_ring_seq++;
}

int psx_ws_backdrop_ring_json(char *buf, int cap) {
    int n = (int)(s_bd_ring_seq < WS_BD_RING_CAP ? s_bd_ring_seq : WS_BD_RING_CAP);
    uint32_t start = s_bd_ring_seq - (uint32_t)n;
    int off = snprintf(buf, (size_t)cap, "\"seq\":%u,\"n\":%d,\"ents\":[", s_bd_ring_seq, n);
    for (int i = 0; i < n && off < cap - 200; i++) {
        WsBdRingEnt *e = &s_bd_ring[(start + (uint32_t)i) & (WS_BD_RING_CAP - 1)];
        off += snprintf(buf + off, (size_t)(cap - off),
            "%s{\"f\":%u,\"pc\":\"%08x\",\"k\":%d,\"wc\":%d,\"o\":%d,\"fin\":%d,\"ext\":%d,\"cam\":%d,\"cnt\":%d,\"base\":\"%08x\",\"dl\":\"%08x\"}",
            i ? "," : "", e->frame, e->pc, e->kind, e->wcols,
            (int)(int16_t)e->orig, (int)(int16_t)e->finalv, e->extent, e->camx, e->count,
            e->base, e->dl);
    }
    off += snprintf(buf + off, (size_t)(cap - off), "]");
    return off;
}

/* ws_ui_groups — dump the auto_ui_squash partition for the LAST prepass.
 *
 * auto_ui_squash squashes each spatial run about its own anchor, so a HUD
 * element that lands in two runs gets two anchors and comes apart as the frame
 * widens. Nothing exposed which run a primitive ended up in, which made that
 * failure mode guesswork: the key is a hash of CLUT/texpage/Y-band/family, so
 * comparing keys tells you two prims differ without telling you why, and the
 * anchor takes only three values so anchor equality cannot prove co-grouping.
 *
 * This reports both the raw key inputs and the union-find root, which together
 * answer "did these two merge, and if not, which component split them". */
int psx_ws_ui_groups_json(char *buf, int cap) {
    int off = snprintf(buf, (size_t)cap,
        "\"active\":%d,\"squash\":%d,\"dense\":%d,\"rank\":%d,"
        "\"disp_x\":%d,\"disp_w\":%d,\"join_gap\":%d,"
        "\"rejected\":{\"opcode\":%u,\"not_axis\":%u,\"degenerate\":%u,"
        "\"too_big\":%u,\"cap\":%u,\"rank\":%u,\"stale\":%u},"
        "\"stale_why\":[%u,%u,%u,%u,%u],\"stale_addr\":\"0x%08X\","
        "\"backing_panels\":%u,\"enclosed_parts\":%u,\"n\":%u,",
        ws_active(), ws_auto_ui_squash, ws_auto_ui_dense,
        ws_ui_prepass_rank != 0xFFFFu ? (int)ws_ui_prepass_rank : -1,
        ws_disp_x(), ws_disp_w(), WS_UI_GROUP_JOIN_GAP,
        ws_ui_reject.opcode, ws_ui_reject.not_axis, ws_ui_reject.degenerate,
        ws_ui_reject.too_big, ws_ui_reject.cap, ws_ui_reject.rank,
        ws_ui_reject.stale, ws_ui_stale_why[1], ws_ui_stale_why[2],
        ws_ui_stale_why[3], ws_ui_stale_why[4], ws_ui_stale_why[5],
        ws_ui_stale_addr, ws_ui_reject.backing, ws_ui_reject.enclosed,
        ws_ui_prepass_count);
    off += snprintf(buf + off, (size_t)(cap - off), "\"rank_dropped\":[");
    for (uint32_t i = 0; i < ws_ui_rankdrop_count && off < cap - 120; i++) {
        off += snprintf(buf + off, (size_t)(cap - off),
            "%s{\"op\":\"%02x\",\"rank\":%u,\"x\":%d,\"w\":%d,\"y\":%d,\"h\":%d}",
            i ? "," : "", ws_ui_rankdrop[i].op, ws_ui_rankdrop[i].rank,
            ws_ui_rankdrop[i].x, ws_ui_rankdrop[i].w,
            ws_ui_rankdrop[i].y, ws_ui_rankdrop[i].h);
    }
    off += snprintf(buf + off, (size_t)(cap - off), "],\"items\":[");
    for (uint32_t i = 0; i < ws_ui_prepass_count && off < cap - 220; i++) {
        const WsUiPrepassItem *it = &ws_ui_prepass[i];
        /* Recover the key components so a split is attributable. Mirrors
         * ws_auto_ui_group_key_words; kept in step with it by construction. */
        int32_t centre_y = it->y + it->h / 2;
        unsigned band = (unsigned)((centre_y < 0 ? 0 : centre_y / 24) & 0x1F);
        unsigned family = it->op < 0x60u ? 1u : 2u;
        off += snprintf(buf + off, (size_t)(cap - off),
            "%s{\"i\":%u,\"op\":\"%02x\",\"key\":\"%08x\",\"root\":%u,"
            "\"x\":%d,\"w\":%d,\"y\":%d,\"h\":%d,"
            "\"band\":%u,\"family\":%u,\"anchor\":%d,\"src\":\"%08x\"}",
            i ? "," : "", i, it->op, it->group.key, it->group.root,
            it->group.x, it->group.width, it->y, it->h,
            band, family, it->group.anchor, it->src_addr);
    }
    off += snprintf(buf + off, (size_t)(cap - off), "]");
    return off;
}

/* Backdrop store-site registry. The [widescreen.backdrop] x_sites are emitted
 * into native cache-DLL code by the recompiler, but overlay code very often
 * runs INTERPRETED (no DLL loaded), where the emit can't reach. So the runtime
 * also registers the same site PCs here and the dirty-RAM interpreter applies
 * psx_ws_backdrop_x() at those `sh` PCs — same transform, both paths. Tiny set;
 * a linear scan per matching SH is negligible. */
#define WS_BACKDROP_SITES_MAX 16
static uint32_t ws_backdrop_sites[WS_BACKDROP_SITES_MAX];
static int      ws_backdrop_site_n = 0;

void psx_ws_set_backdrop_sites(const uint32_t* pcs, int n) {
    ws_backdrop_site_n = 0;
    if (!pcs) return;
    for (int i = 0; i < n && ws_backdrop_site_n < WS_BACKDROP_SITES_MAX; i++)
        ws_backdrop_sites[ws_backdrop_site_n++] = pcs[i] & 0x1FFFFFFFu;
}

int psx_ws_is_backdrop_site(uint32_t pc) {
    if (!ws_backdrop_site_n) return 0;
    pc &= 0x1FFFFFFFu;
    for (int i = 0; i < ws_backdrop_site_n; i++)
        if (ws_backdrop_sites[i] == pc) return 1;
    return 0;
}

/* Snapshot live widescreen state for the TCP gpu_state diagnostic. Mirrors
 * the predicates above so a probe can see, mid-gameplay, whether the squash
 * and the cull-margin are actually engaged (8C). */
void gpu_ws_get_debug(GpuWsDebug* out) {
    if (!out) return;
    out->configured        = ws_configured();
    out->active            = ws_active();
    out->game_mode         = ws_game_mode();
    out->present_native_43 = gpu_ws_present_native_43();
    out->x_margin          = psx_ws_x_margin();
    out->activation_margin = psx_ws_activation_margin();
    out->xnum              = ws_xnum;
    out->xden              = ws_xden;
    out->mode              = ws_mode;
    out->nw_extra          = ws_nw_extra();
    WsViewAnchor view = ws_view_current();
    out->view_anchor = ws_view_enabled();
    out->view_left = view.left; out->view_right = view.right;
    out->view_shift = view.shift;
    out->view_pad_left = view.pad_left; out->view_pad_right = view.pad_right;
    out->cur_frame         = s_frame_count;
    out->bg2d_generations  = g_bg2d_host_generation;
    out->last_tag_frame    = ws_last_tag_stamp;
    out->last_3d_frame     = ws_last_3d_stamp;
    out->gte_verts         = ws_gte_prev_verts;
    out->last_world3d_frame = ws_sust_world3d_stamp;
    out->ovh_prims         = ws_ovh_prev;
    out->last_ovh_frame    = ws_sust_ovh_stamp;
    out->auto_ui_squash    = ws_auto_ui_squash;
    out->auto_ui_in_place  = ws_auto_ui_in_place;
    out->auto_ui_dense     = ws_auto_ui_dense;
    out->auto_ui_ot_rank   =
        ws_ui_prepass_rank != 0xFFFFu ? ws_ui_prepass_rank : UINT32_MAX;
    out->auto_ui_candidates = ws_auto_ui_candidate_count;
    out->auto_ui_transforms = ws_auto_ui_transform_count;
    out->aspect_cone_calls = ws_aspect_cone_calls;
    out->aspect_cone_43_identity = ws_aspect_cone_43_identity;
    out->aspect_cone_vanilla_keep = ws_aspect_cone_vanilla_keep;
    out->aspect_cone_visible_keep = ws_aspect_cone_visible_keep;
    out->aspect_cone_guard_keep = ws_aspect_cone_guard_keep;
    out->aspect_cone_hysteresis_keep =
        ws_aspect_cone_hysteresis_keep;
    out->aspect_cone_outside_reject =
        ws_aspect_cone_outside_reject;
    out->aspect_cone_queue_reject = ws_aspect_cone_queue_reject;
    for (int i = 0; i < 3; i++)
        out->aspect_cone_queue_highwater[i] =
            ws_aspect_cone_queue_highwater[i];
    out->angle_calls = ws_angle_calls;
    out->angle_43_identity = ws_angle_43_identity;
    out->angle_max_vanilla = ws_angle_max_vanilla;
    out->angle_max_widened = ws_angle_max_widened;
    out->bd_veto = ws_bdx_veto(&ws_bdx, (uint32_t)s_frame_count,
                               WS_2D_SCENE_HYSTERESIS);
    out->bd_eval_frame = ws_bdx.eval_frame;
    out->bd_rects = ws_bdx.eval_rects;
    out->bd_last_short = ws_bdx.last_short;
    out->bd_full = ws_bdx.eval_full;
    out->bd_short = ws_bdx.eval_short;
    out->bd_min_x = ws_bdx.eval_rects ? ws_bdx.eval_min_x : 0;
    out->bd_max_x = ws_bdx.eval_rects ? ws_bdx.eval_max_x : 0;
    out->bd_reveal = ws_bdx.eval_reveal;
    out->bd_canon_pct = ws_bdx.canon_pct;
    out->bd_left_pct = ws_bdx.left_pct;
    out->bd_right_pct = ws_bdx.right_pct;
    out->bd_evaluations = ws_bdx.evaluations;
    out->bd_short_frames = ws_bdx.short_frames;
}

int gpu_ws_get_aspect_cone_site_debug(
    uint32_t address, GpuWsAspectConeSiteDebug* out) {
    WsAspectConeSite *site = ws_aspect_cone_find(address, 0);
    if (!site || !out) return 0;
    out->address = site->address | 0x80000000u;
    out->calls = site->calls;
    out->identity_43 = site->identity_43;
    out->vanilla_keep = site->vanilla_keep;
    out->visible_keep = site->visible_keep;
    out->guard_keep = site->guard_keep;
    out->hysteresis_keep = site->hysteresis_keep;
    out->outside_reject = site->outside_reject;
    out->queue_reject = site->queue_reject;
    return 1;
}

void gpu_ws_configure(int aspect_num, int aspect_den,
                      uint32_t sprite_anchor_addr, int hud_sprt_squash, int mode) {
    ws_cfg_num = aspect_num > 0 ? aspect_num : 4;
    ws_cfg_den = aspect_den > 0 ? aspect_den : 3;
    ws_mode    = mode;
    if (mode == 1) {
        /* Squash factor = (4*den)/(3*num) — the same factor the GTE applies. */
        int32_t n = 4 * ws_cfg_den, d = 3 * ws_cfg_num;
        int32_t a = n, b = d;
        while (b) { int32_t t = a % b; a = b; b = t; }
        ws_xnum = n / a;
        ws_xden = d / a;
    } else {
        /* Native-wide (2) and off (0): the GTE is NOT squashed. */
        ws_xnum = ws_xden = 1;
    }
    ws_anchor_addr = sprite_anchor_addr;
    ws_hud_sprt    = hud_sprt_squash;
    /* Adaptive view can change mode/extent without the guest reissuing E3/E4
     * immediately. Keep the active mirror target and its scissor in lockstep
     * with the new live aspect instead of waiting for another draw-env packet. */
    ws_nw_sync_target();
}

/* Called from generated code at the entry of each [widescreen]
 * sprite_tag_funcs function: record prim pointer ($a0) -> projected anchor.
 * Gated on ws_configured (NOT ws_active): ws_active depends on game_mode which
 * THIS function sets, so gating it on ws_active would never let gameplay
 * start (the first character of a frame would be suppressed forever). */
void psx_ws_sprite_tag(CPUState* cpu) {
    if (!ws_engaged() || !ws_anchor_addr) return;
    uint32_t key = GPU_RAM_KEY(cpu->gpr[4]);
    if (!key) return;
    uint32_t sxy = cpu->read_word(ws_anchor_addr);
    int32_t  ax  = (int32_t)(int16_t)(sxy & 0xFFFFu);
    uint32_t now = (uint32_t)s_frame_count;
    uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
    uint32_t victim = idx;
    for (int i = 0; i < WS_TAG_PROBES; i++) {
        uint32_t j = (idx + i) & (WS_TAG_BUCKETS - 1);
        WsTag *t = &ws_tags[j];
        if (t->key == key || t->key == 0) { victim = j; break; }
        if (now - t->stamp > 2) victim = j;  /* stale — reusable */
    }
    ws_tags[victim].key      = key;
    ws_tags[victim].stamp    = now;
    ws_tags[victim].anchor_x = ax;
    ws_tags[victim].repeat_period = 0;
    ws_tags[victim].source_width = 0;
    ws_tags[victim].screen_space = 0;
    ws_last_tag_stamp = now;
}

static int ws_hud_command_words(uint32_t command_addr, uint32_t *words,
                                uint32_t *out_count) {
    if (!words || !out_count) return 0;
    if ((command_addr & 3u) != 0) return 0;
    uint32_t phys = command_addr & 0x1FFFFFFFu;
    if (phys > psx_ram_live_bytes() - 4u &&
        !psx_mod_gpu_dma_memory_contains(command_addr, 4u)) return 0;

    words[0] = psx_read_word(command_addr);
    uint8_t op = (uint8_t)(words[0] >> 24);
    if (op < 0x20u || op > 0x7Fu) return 0;
    int count = gp0_command_word_count(op);
    if (count <= 0 || count > 12) return 0;
    uint32_t bytes = (uint32_t)count * 4u;
    if (phys > psx_ram_live_bytes() - bytes &&
        !psx_mod_gpu_dma_memory_contains(command_addr, bytes)) return 0;

    for (int i = 1; i < count; i++)
        words[i] = psx_read_word(command_addr + (uint32_t)i * 4u);
    *out_count = (uint32_t)count;
    return 1;
}

static GpuWsTagStats ws_hud_tag_stats;
static GpuWsTagStats ws_background_tag_stats;

void gpu_ws_get_tag_stats(GpuWsTagStats *hud, GpuWsTagStats *background) {
    if (hud) *hud = ws_hud_tag_stats;
    if (background) *background = ws_background_tag_stats;
}

static void ws_tag_stats_note_lookup(GpuWsTagStats *stats, int result) {
    switch (result) {
    case WS_HUD_ANCHOR_HIT:
        stats->hit++;
        stats->last_hit_frame = (uint32_t)s_frame_count;
        break;
    case WS_HUD_ANCHOR_STALE:          stats->stale++; break;
    case WS_HUD_ANCHOR_GUARD_MISMATCH: stats->guard_mismatch++; break;
    default: break;
    }
}

void gpu_ws_tag_hud_prim(uint32_t prim, int anchor) {
    ws_hud_tag_stats.tag_calls++;
    uint32_t command_addr = prim + 4u;
    uint32_t words[12];
    uint32_t word_count = 0;
    if (!(ws_native_wide_configured() || ws_configured()) ||
        (prim & 3u) != 0 || prim > UINT32_MAX - 4u ||
        !ws_hud_command_words(command_addr, words, &word_count)) {
        ws_hud_tag_stats.tag_rejected++;
        return;
    }

    WsPrepassPacketGuard guard =
        ws_prepass_packet_guard(words, word_count);
    ws_hud_anchor_insert(ws_hud_anchor_tags, WS_HUD_ANCHOR_TABLE_SIZE,
                         GPU_RAM_KEY(command_addr), anchor, &guard,
                         (uint32_t)s_frame_count);
    ws_hud_tag_stats.last_tag_frame = (uint32_t)s_frame_count;
}

void gpu_ws_tag_background_prim(uint32_t prim) {
    ws_background_tag_stats.tag_calls++;
    uint32_t words[12], count = 0;
    if (!ws_native_wide_configured() || (prim & 3u) ||
        prim > UINT32_MAX - 4u ||
        !ws_hud_command_words(prim + 4u, words, &count)) {
        ws_background_tag_stats.tag_rejected++;
        return;
    }
    uint32_t op = words[0] >> 24;
    if (!((op >= 0x20u && op <= 0x3Fu && !(op & 2u)) ||
          op == 0x64u || op == 0x65u)) {
        ws_background_tag_stats.tag_rejected++;
        return;
    }
    WsPrepassPacketGuard guard = ws_prepass_packet_guard(words, count);
    ws_hud_anchor_insert(ws_background_tags, WS_HUD_ANCHOR_TABLE_SIZE,
                         GPU_RAM_KEY((prim + 4u)), 0, &guard,
                         (uint32_t)s_frame_count);
    ws_background_tags_used = 1;
    ws_background_tag_frame = (uint32_t)s_frame_count;
    ws_background_tag_stats.last_tag_frame = (uint32_t)s_frame_count;
}

int gpu_ws_background_stretch_active(void) {
    uint32_t frame = (uint32_t)s_frame_count;
    return ws_background_tags_used && ws_native_wide_active() &&
           frame >= ws_background_tag_frame &&
           frame - ws_background_tag_frame <= WS_HUD_ANCHOR_FRESH_FRAMES;
}

int gpu_ws_background_requires_full_composite(void) {
    /* Freshness governs reuse of a packet address, not the lifetime of the
     * pixels it produced. A slow game frame or a held display buffer can keep
     * those pixels on screen indefinitely. Conservatively keep full mirroring
     * once a composite has been tagged; GPU reset/snapshot restore clears the
     * latch together with the tags. Never modify the packet freshness guard. */
    if (ws_background_tags_used && ws_native_wide_configured()) return 1;
    /* A stretched phase backdrop (psx_ws_prim_in_backdrop) differs inside the
     * canonical view too: the centre is drawn again, not copied, so the
     * stretched picture has no seam at the 4:3 edges. */
    if (!ws_native_wide_configured()) return 0;
    if (ws_nw_phase_backdrop && (uint32_t)s_frame_count - ws_phase_stretch_stamp <= 2u) return 1;
    /* so does a polygon the title's predicate stretched (gpu_ws_set_wide_prim_predicate) */
    return (uint32_t)s_frame_count - ws_prim_pred_stamp <= 2u;
}

static int ws_nw_explicit_background(void) {
    if (!gpu_ws_background_stretch_active() ||
        gp0_cmd_source_addr == 0xFFFFFFFFu ||
        gp0_words_needed <= 0 || gp0_words_needed > 12) return 0;
    uint32_t addr = gp0_cmd_source_addr & 0x1FFFFFFFu;
    if (addr >= 0x00200000u || (addr & 3u)) return 0;
    int result = ws_hud_anchor_lookup_result(
        ws_background_tags, WS_HUD_ANCHOR_TABLE_SIZE, addr, gp0_cmd_buf,
        (uint32_t)gp0_words_needed, (uint32_t)s_frame_count, NULL);
    ws_tag_stats_note_lookup(&ws_background_tag_stats, result);
    return result == WS_HUD_ANCHOR_HIT;
}

void gpu_ws_tag_black_reveal_rect(uint32_t prim) {
    if (!ws_native_wide_configured()) return;
    if ((prim & 3u) != 0 || prim > UINT32_MAX - 4u) return;
    uint32_t words[12], count = 0;
    if (!ws_hud_command_words(prim + 4u, words, &count)) return;
    /* Only opaque variable-size textured rectangles define this clear band. */
    if ((words[0] >> 24) != 0x64u && (words[0] >> 24) != 0x65u) return;
    WsPrepassPacketGuard guard = ws_prepass_packet_guard(words, count);
    ws_hud_anchor_insert(ws_reveal_clear_tags, WS_HUD_ANCHOR_TABLE_SIZE,
                         GPU_RAM_KEY((prim + 4u)), 0, &guard,
                         (uint32_t)s_frame_count);
}

void gpu_ws_tag_repeat_rect(uint32_t prim, int32_t period) {
    if (!ws_native_wide_configured() || period <= 0 || period > 4096 ||
        (prim & 3u) || prim > UINT32_MAX - 4u) return;
    uint32_t words[12], count = 0;
    if (!ws_hud_command_words(prim + 4u, words, &count)) return;
    if ((words[0] >> 24) != 0x64u && (words[0] >> 24) != 0x65u) return;
    uint32_t w = words[3] & 0xFFFFu;
    uint32_t h = words[3] >> 16;
    if (!w || w > 256u || (words[2] & 255u) + w > 256u || !h || h > 511u)
        return;
    WsPrepassPacketGuard guard = ws_prepass_packet_guard(words, count);
    ws_repeat_rect_tag_insert(ws_repeat_rect_tags, GPU_RAM_KEY((prim + 4u)),
                              period, &guard, (uint32_t)s_frame_count);
}

void gpu_ws_tag_screen_mask_quad(uint32_t prim) {
    if (!ws_native_wide_configured() || (prim & 3u) || prim > UINT32_MAX - 4u)
        return;
    uint32_t words[12], count = 0;
    if (!ws_hud_command_words(prim + 4u, words, &count) || count != 5u ||
        ((words[0] >> 24) & 0xfdu) != 0x28u) return;
    WsPrepassPacketGuard guard = ws_prepass_packet_guard(words, count);
    ws_hud_anchor_insert(ws_screen_mask_tags, WS_HUD_ANCHOR_TABLE_SIZE,
                         GPU_RAM_KEY(prim + 4u), 0, &guard,
                         (uint32_t)s_frame_count);
}

void gpu_ws_tag_radial_screen_mask_quad(uint32_t prim, float scale) {
    if (!ws_native_wide_configured() || (prim & 3u) || prim > UINT32_MAX - 4u)
        return;
    uint32_t words[12], count=0;
    if (!ws_hud_command_words(prim+4u, words, &count)) return;
    unsigned op=words[0]>>24;
    if (!((count==5u && (op&0xfdu)==0x28u) ||
          (count==8u && (op&0xfdu)==0x38u))) return;
    WsPrepassPacketGuard guard=ws_prepass_packet_guard(words,count);
    ws_radial_mask_insert(ws_radial_screen_mask_tags,GPU_RAM_KEY(prim+4u),
                          &guard,(uint32_t)s_frame_count,scale);
}

static int ws_nw_radial_mask_transform(int32_t x[4], int32_t y[4]) {
    if (!ws_native_wide_active() || psx_ws_x_margin()<=0 ||
        gp0_cmd_source_addr==UINT32_MAX) return 0;
    float scale;
    if (!ws_radial_mask_lookup(ws_radial_screen_mask_tags,
        GPU_RAM_KEY(gp0_cmd_source_addr),gp0_cmd_buf,(uint32_t)gp0_words_needed,
        (uint32_t)s_frame_count,&scale)) return 0;
    ws_radial_mask_transform(x,y,ws_disp_w(),ws_disp_h(),scale);
    return 1;
}

static int ws_explicit_hud_anchor(int *out_anchor) {
    if (gp0_cmd_source_addr == 0xFFFFFFFFu)
        return 0;
    if (gp0_words_needed <= 0 || gp0_words_needed > 12)
        return 0;
    uint32_t command_addr = gp0_cmd_source_addr & 0x1FFFFFFFu;
    if ((command_addr & 3u) ||
        (command_addr >= psx_ram_live_bytes() &&
         !psx_mod_gpu_dma_memory_contains(command_addr, 4u))) return 0;
    int anchor = 0;
    int result = ws_hud_anchor_lookup_result(
        ws_hud_anchor_tags, WS_HUD_ANCHOR_TABLE_SIZE, GPU_RAM_KEY(command_addr),
        gp0_cmd_buf, (uint32_t)gp0_words_needed, (uint32_t)s_frame_count,
        &anchor);
    ws_tag_stats_note_lookup(&ws_hud_tag_stats, result);
    if (result != WS_HUD_ANCHOR_HIT)
        return 0;
    if (out_anchor) *out_anchor = anchor;
    return 1;
}

static int ws_nw_explicit_hud_delta(int32_t *out_delta) {
    if (out_delta) *out_delta = 0;
    if (!ws_native_wide_active()) return 0;
    int anchor;
    if (!ws_explicit_hud_anchor(&anchor)) return 0;
    if (out_delta)
        *out_delta = ws_hud_anchor_native_delta(1, ws_nw_offset(), anchor);
    return 1;
}

/* A title can identify one widget whose pieces arrive in separate DMA lists.
 * Per-list auto grouping cannot see that ownership. Use the same guarded
 * edge/centre role as native-wide, expressed as a squash pivot here. */
static int ws_projection_hud_anchor(int32_t *out_anchor) {
    if (!ws_active()) return 0;
    int edge;
    if (!ws_explicit_hud_anchor(&edge)) return 0;
    if (out_anchor)
        *out_anchor = ws_disp_x() + (edge < 0 ? 0 : edge > 0 ? ws_disp_w() : ws_disp_w() / 2);
    return 1;
}

static int ws_projection_hud_transform(int32_t *x, int count) {
    int32_t anchor;
    if (!ws_projection_hud_anchor(&anchor)) return 0;
    for (int i = 0; i < count; i++) x[i] = ws_scale_about(x[i], anchor);
    return 1;
}

static WsTag *ws_tag_screen_packet(uint32_t prim, int32_t anchor,
                                 int32_t period, int32_t source_width) {
    uint32_t words[12], count = 0;
    if (!ws_engaged() || (prim & 3u) || prim > UINT32_MAX - 4u ||
        !ws_hud_command_words(prim + 4u, words, &count)) return NULL;
    uint32_t key = GPU_RAM_KEY(prim);
    if (!key) return NULL;
    uint32_t now = (uint32_t)s_frame_count;
    uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
    uint32_t victim = idx;
    for (int i = 0; i < WS_TAG_PROBES; ++i) {
        uint32_t j = (idx + i) & (WS_TAG_BUCKETS - 1);
        WsTag *t = &ws_tags[j];
        if (t->key == key || !t->key) { victim = j; break; }
        if (now - t->stamp > 2) victim = j;
    }
    WsTag *tag = &ws_tags[victim];
    *tag = (WsTag){0};
    tag->key = key;
    tag->stamp = now;
    tag->anchor_x = anchor;
    tag->repeat_period = period;
    tag->source_width = source_width;
    tag->screen_space = 1;
    tag->packet_guard = ws_prepass_packet_guard(words, count);
    return tag;
}

void gpu_ws_tag_tiled_strip(uint32_t prim, int32_t anchor,
                            int32_t period, int32_t source_width) {
    if (source_width <= 0 || source_width > 256 ||
        period < source_width || period > 4096)
        return;
    ws_tag_screen_packet(prim, anchor, period, source_width);
}

void gpu_ws_tag_screen_prim(uint32_t prim, int32_t anchor) {
    ws_tag_screen_packet(prim, anchor, 0, 0);
}

static int ws_screen_packet_matches(const WsTag *tag) {
    return ws_prepass_packet_matches(&tag->packet_guard, gp0_cmd_buf,
                                     (uint32_t)gp0_words_needed);
}

void gpu_ws_tag_stretched_prim(uint32_t prim, int32_t left, int32_t right,
                               int32_t left_anchor, int32_t right_anchor) {
    if (right <= left) return;
    WsTag *t = ws_tag_screen_packet(prim, left_anchor, 0, 0);
    if (!t) return;
    t->stretch_left = left;
    t->stretch_right = right;
    t->left_anchor = left_anchor;
    t->right_anchor = right_anchor;
}

static const WsTag *ws_screen_tag(void) {
    if (ws_mode != 1 || !ws_active() || gp0_cmd_source_addr == 0xFFFFFFFFu)
        return NULL;
    uint32_t now = (uint32_t)s_frame_count;
    /* Explicit tags use the P_TAG address, never the first command address. */
    uint32_t key = GPU_RAM_KEY(gp0_cmd_source_addr - 4u);
    uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
    for (int i = 0; i < WS_TAG_PROBES; ++i) {
        const WsTag *t = &ws_tags[(idx + i) & (WS_TAG_BUCKETS - 1)];
        if (t->key == key && t->screen_space && now - t->stamp <= 2 &&
            ws_screen_packet_matches(t))
            return t;
    }
    return NULL;
}

static void ws_screen_transform_quad(int32_t vx[4]) {
    const WsTag *t = ws_screen_tag();
    if (!t) return;
    if (t->stretch_right > t->stretch_left) {
        int32_t left = ws_scale_about(t->stretch_left, t->left_anchor);
        int32_t right = ws_scale_about(t->stretch_right, t->right_anchor);
        int32_t span = t->stretch_right - t->stretch_left;
        for (int i = 0; i < 4; ++i) {
            int64_t n = (int64_t)(vx[i] - t->stretch_left) * (right - left);
            vx[i] = left + (int32_t)((n + (n >= 0 ? span / 2 : -span / 2)) / span);
        }
    } else {
        for (int i = 0; i < 4; ++i)
            vx[i] = ws_scale_about(vx[i], t->anchor_x);
    }
}

/* Look up the executing GP0 command's prim in the tag table. The command's
 * first word lives at prim+4 (the PsyQ P_TAG header precedes it), but accept
 * a direct hit too in case a tag site passes the colour-word address. */
static int ws_tagged_anchor(int32_t *out_ax) {
    if (!ws_active() || gp0_cmd_source_addr == 0xFFFFFFFFu) return 0;
    /* Explicit screen-space ownership wins over a recycled billboard tag.
     * The regular UI transform below will apply it exactly once. */
    if (ws_projection_hud_anchor(NULL)) return 0;
    uint32_t now = (uint32_t)s_frame_count;
    for (int variant = 0; variant < 2; variant++) {
        uint32_t key = GPU_RAM_KEY((gp0_cmd_source_addr - (variant ? 0u : 4u)));
        uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
        for (int i = 0; i < WS_TAG_PROBES; i++) {
            WsTag *t = &ws_tags[(idx + i) & (WS_TAG_BUCKETS - 1)];
            if (t->key == key && now - t->stamp <= 2 &&
                (!t->screen_space || (ws_mode == 1 && variant == 0 &&
                                      ws_screen_packet_matches(t)))) {
                *out_ax = t->anchor_x;
                return 1;
            }
        }
    }
    return 0;
}

/* Native-wide-compatible "is the current GP0 prim sprite-tagged?" — same ws_tags
 * lookup as ws_tagged_anchor but gated on ws_engaged() (squash OR native-wide)
 * instead of ws_active() (squash only). Used by the GL native-wide 2D-backdrop
 * stretch to EXCLUDE foreground sprites: characters / Tomba / HUD are tagged by
 * psx_ws_sprite_tag (their prim ptr -> ws_tags), the 2D backdrop tiles are not.
 * Reads the live gp0_cmd_source_addr (the source addr of the prim being drawn). */
/* Flower-field backdrop data-structure address range — set by the dirty-RAM
 * interpreter when the backdrop generator (overlay 0x80116808) runs; its tile
 * packets live in [lo, hi]. The GL native-wide 2D-stretch gate matches the prim
 * being drawn (gp0_cmd_source_addr) against this to PRECISELY identify the
 * flower-field tiles — the 3D rock/foreground is untagged AND has narrow prims,
 * so tag/narrow heuristics alone mis-stretch and tear it. */
uint32_t g_ws_backdrop_lo = 0, g_ws_backdrop_hi = 0;
static int ws_nw_phase_backdrop = 0;
void gpu_ws_set_nw_phase_backdrop(int on) { ws_nw_phase_backdrop = on ? 1 : 0; }
void gpu_ws_set_wide_prim_predicate(int (*predicate)(uint32_t op, uint32_t ot_rank, uint32_t flags)) {
    s_ws_wide_prim_predicate = predicate;
}
static int ws_nw_textured_edges = 0;
int g_ws_tex_edge_pct = 0;
void gpu_ws_set_nw_textured_edges(int on, int scale_pct) {
    ws_nw_textured_edges = on ? 1 : 0;
    g_ws_tex_edge_pct = scale_pct;
}
/* diag: per-frame min/max of the prim source addrs the GL gate sees */
uint32_t g_bdg_src_lo = 0xFFFFFFFFu, g_bdg_src_hi = 0;
static uint32_t bdg_src_frame = 0xFFFFFFFFu;

/* ---- prim<->pixel correlation debug gate (ws_dbg_stretch command) ------------
 * To identify WHICH drawn prims constitute the native-wide 2D-backdrop void layer
 * (vs Tomba/HUD/3D), this lets a probe stretch an exact SELECTABLE prim set and
 * screenshot the result. When g_dbg_mode != 0 the GL stretch gate uses this
 * instead of the flower-struct range. Modes (buffer/frame-independent except 1):
 *   1 OT-range   : gp0_cmd_source_addr in [g_dbg_lo,g_dbg_hi]
 *   2 clut       : textured prim whose clut == g_dbg_clut
 *   3 tpage      : textured prim whose tpage == (g_dbg_clut & 0x1FF)
 *   4 tagged     : prim is sprite-tagged (psx_ws_prim_is_tagged)
 *   5 untag-tex  : textured AND not sprite-tagged
 *   6 all-tex    : every textured prim (sanity: should fill void + distort 3D)
 *   7 all        : every primitive (coarse correlation sanity check)
 *   8 all-flat   : every untextured primitive
 * Counters (per-frame, snapshotted by the present hook) report how many prims
 * matched and how many of those were sprite-tagged. */
int      g_dbg_mode = 0;
uint32_t g_dbg_lo = 0, g_dbg_hi = 0;
uint16_t g_dbg_clut = 0;
int      g_dbg_match_n = 0, g_dbg_match_tagged = 0;        /* snapshot (reported) */
static int s_dbg_match_n = 0, s_dbg_match_tagged = 0;      /* accumulate per frame */
int psx_ws_prim_is_tagged(void);   /* defined below */

static int dbg_gate_match(void) {
    uint32_t op   = (gp0_cmd_buf[0] >> 24) & 0xFFu;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint32_t a    = gp0_cmd_source_addr & 0x1FFFFFFFu;
    int textured  = ((op >= 0x20 && op <= 0x3F) && (op & 0x04)) || (op >= 0x64 && op <= 0x67);
    int tagged    = psx_ws_prim_is_tagged();
    int m = 0;
    switch (g_dbg_mode) {
        case 1: m = (g_dbg_hi > g_dbg_lo && a >= g_dbg_lo && a <= g_dbg_hi); break;
        case 2: m = (textured && clut == g_dbg_clut); break;
        case 3: m = (textured && ((uint16_t)(gp0_cmd_buf[4] >> 16) & 0x1FF) == (g_dbg_clut & 0x1FF)); break;
        case 4: m = tagged; break;
        case 5: m = (textured && !tagged); break;
        case 6: m = textured; break;
        case 7: m = 1; break;
        case 8: m = !textured; break;
        default: m = 0; break;
    }
    if (m) { s_dbg_match_n++; if (tagged) s_dbg_match_tagged++; }
    return m;
}

/* Called by the present hook each frame to publish the dbg match counters. */
void psx_ws_dbg_gate_frame_snapshot(void) {
    g_dbg_match_n = s_dbg_match_n; g_dbg_match_tagged = s_dbg_match_tagged;
    s_dbg_match_n = 0; s_dbg_match_tagged = 0;
}

/* Background-phase latch for the native-wide 2D-backdrop stretch. The far 2D
 * backdrop (a sprite-tagged tile grid: sky gradient + flower-ball field) draws
 * FIRST each frame, before the GTE 3D world. Once a shaded/gouraud prim (the 3D
 * world's signature -- the backdrop is sprites/flat) is drawn, the backdrop phase
 * is over: later TAGGED prims (HUD, characters) must NOT be stretched. Latched
 * per-frame at GP0 decode (ws_bg_phase_note, sees every prim in draw order). */
static uint32_t s_bg_phase_frame = 0xFFFFFFFFu;
static int      s_bg_phase_over  = 0;
void ws_bg_phase_note(uint32_t op) {
    uint32_t f = (uint32_t)s_frame_count;
    if (f != s_bg_phase_frame) { s_bg_phase_frame = f; s_bg_phase_over = 0; }
    if (op >= 0x30u && op <= 0x3Fu) {
        /* nw_phase_backdrop: an untextured shaded polygon from one edge of the
         * picture exactly to the other is the backdrop's own gradient (the
         * horizon haze under a cutscene's painted mountains), not the world:
         * it stays in the background phase and is stretched with it. */
        if (ws_nw_phase_backdrop && !s_bg_phase_over && !(op & 0x04u)) {
            int32_t xmn, xmx;
            prim_sx_extent((uint8_t)op, &xmn, &xmx);
            const int32_t W = ws_disp_w();
            if (xmn <= 0 && xmn >= -2 && xmx >= W && xmx <= W + 2) return;
        }
        s_bg_phase_over = 1;   /* shaded = 3D world (within-frame draw order) */
        ws_last_3d_stamp = f;  /* diagnostic only — see ws_2d_only_scene() for
                                  why shaded prims are NOT the scene classifier */
    }
}
static int ws_bg_phase_over(void) {
    uint32_t f = (uint32_t)s_frame_count;
    if (f != s_bg_phase_frame) { s_bg_phase_frame = f; s_bg_phase_over = 0; }
    return s_bg_phase_over;
}

static int ws_tagged_world_primitive(void);
static int bdi_gate(int marked);   /* backdrop images, with the linked-list pre-pass */
int psx_ws_prim_in_backdrop(void) {
    /* Explicit title proof is independent of heuristic draw-order phases. */
    if (ws_nw_explicit_background()) return 1;
    if (gp0_cmd_source_addr != 0xFFFFFFFFu) {
        uint32_t f = (uint32_t)s_frame_count;
        if (f != bdg_src_frame) { bdg_src_frame = f; g_bdg_src_lo = 0xFFFFFFFFu; g_bdg_src_hi = 0; }
        if (gp0_cmd_source_addr < g_bdg_src_lo) g_bdg_src_lo = gp0_cmd_source_addr;
        if (gp0_cmd_source_addr > g_bdg_src_hi) g_bdg_src_hi = gp0_cmd_source_addr;
    }
    if (g_dbg_mode != 0) return dbg_gate_match();   /* correlation override */
    /* The title's own answer for this primitive (gpu_ws_set_wide_prim_predicate):
     * 4 = stretched, unmarked; 3 = only the vertices on or past the edges;
     * KEEP = none of the rules below. */
    if (s_ws_wide_prim_predicate && ws_native_wide_active()) {
        const uint32_t op = (gp0_cmd_buf[0] >> 24) & 0xFFu;
        if (op >= 0x20u && op <= 0x7Fu) {
            const uint32_t f = (uint32_t)s_frame_count;
            const uint32_t flags = (ws_bg_phase_over() ? 0u : GPU_WS_WIDE_PRIM_BACKDROP) |
                (f - ws_last_gte_stamp <= 4u || f - ws_last_world3d_stamp <= 4u ? GPU_WS_WIDE_PRIM_WORLD3D : 0u);
            const int r = s_ws_wide_prim_predicate(op, gp0_ot_rank, flags);
            if (r == GPU_WS_WIDE_PRIM_KEEP) return 0;
            if ((r == GPU_WS_WIDE_PRIM_STRETCH || r == GPU_WS_WIDE_PRIM_EDGES) && op <= 0x3Fu) {
                ws_prim_pred_stamp = f;
                return r == GPU_WS_WIDE_PRIM_STRETCH ? 4 : 3;
            }
            if (r == GPU_WS_WIDE_PRIM_IMAGE) {
                /* the backdrop-image rule (unmarked), else the stretch as before */
                const int g = bdi_gate(0);
                if (g >= 0) {
                    if (g) ws_prim_pred_stamp = f;
                    return g;
                }
                if (op > 0x3Fu) return 0;
                ws_prim_pred_stamp = f;
                return 4;
            }
        }
    }
    /* Explicit title provenance outranks the legacy draw-order heuristic.
     * Early-sorted world sprites must not be stretched into the reveal. */
    if (ws_tagged_world_primitive()) return 0;
    /* a backdrop image (gpu_ws_set_backdrop_images): as it is, or stretched by what
     * it needs; marked as a painted backdrop while a 3D world is drawn */
    {
        const uint32_t f = (uint32_t)s_frame_count;
        const int g = bdi_gate(f - ws_last_3d_stamp <= 2u);
        if (g >= 0) {
            if (g) ws_prim_pred_stamp = f;
            return g;
        }
    }
    if (ws_nw_textured_edges) {
        uint32_t op = (gp0_cmd_buf[0] >> 24) & 0xFFu;
        if (op >= 0x20u && op <= 0x3Fu && (op & 0x04u))
            return 2; /* GL gate: expand only vertices beyond canonical edges */
    }
    /* [widescreen] nw_phase_backdrop: what a frame draws before its 3D world
     * (a cutscene's painted sky and mountains) is its backdrop, stretched to
     * the wide view; only while the last frames drew a 3D world, so a 2D-only
     * screen (a title card's text on black) is left as it is. */
    if (ws_nw_phase_backdrop && !ws_bg_phase_over() &&
        (uint32_t)s_frame_count - ws_last_3d_stamp <= 2u) {
        ws_phase_stretch_stamp = (uint32_t)s_frame_count;
        return 1;
    }
    /* Real gate: stretch the 2D backdrop = sprite-tagged prims drawn in the
     * background phase (before the 3D world). Fills the native-wide void for both
     * the flower grid and the sky band; HUD/characters (tagged but post-3D) and the
     * GTE world (untagged) are excluded. */
    if (ws_bg_phase_over()) return 0;
    return psx_ws_prim_is_tagged();
}

int psx_ws_prim_is_tagged(void) {
    if (!ws_engaged() || gp0_cmd_source_addr == 0xFFFFFFFFu) return 0;
    uint32_t now = (uint32_t)s_frame_count;
    for (int variant = 0; variant < 2; variant++) {
        uint32_t key = GPU_RAM_KEY((gp0_cmd_source_addr - (variant ? 0u : 4u)));
        uint32_t idx = (key >> 2) & (WS_TAG_BUCKETS - 1);
        for (int i = 0; i < WS_TAG_PROBES; i++) {
            WsTag *t = &ws_tags[(idx + i) & (WS_TAG_BUCKETS - 1)];
            if (t->key == key && now - t->stamp <= 2 &&
                (!t->screen_space || (ws_mode == 1 && variant == 0 &&
                                      ws_screen_packet_matches(t))))
                return 1;
        }
    }
    return 0;
}

/* Squash x around pivot ax by ws_xnum/ws_xden, round to nearest. */
static int32_t ws_scale_about(int32_t x, int32_t ax) {
    int32_t d = x - ax;
    int32_t s = (d * ws_xnum + (d >= 0 ? ws_xden / 2 : -ws_xden / 2)) / ws_xden;
    return ax + s;
}

/* Squash a width; never below 1px so nothing vanishes. */
static int32_t ws_scale_len(int32_t w) {
    int32_t s = (w * ws_xnum + ws_xden / 2) / ws_xden;
    return s < 1 ? 1 : s;
}

/* Horizontal display viewport in drawing space (e.g. 320). For a selected
 * netplay-local split viewport this is that player's authored half, not the
 * whole composed display. */
static int32_t ws_disp_x(void) {
    int base = 0;
    if (ws_local_viewport_layout(&base, NULL, NULL, NULL))
        return (int32_t)base;
    return 0;
}

static int32_t ws_disp_w(void) {
    int src_w = 0;
    if (ws_local_viewport_layout(NULL, &src_w, NULL, NULL))
        return (int32_t)src_w;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    return di.width ? (int32_t)di.width : 320;
}
int gpu_ws_display_width(void) { return (int)ws_disp_w(); }

static int32_t ws_disp_h(void) {
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    return di.height ? (int32_t)di.height : 240;
}

/* Full-screen fades and environmental filters are authored as 320x240 TILEs.
 * In native-wide mode, grow only primitives that cover the complete native
 * display; ordinary world-space rectangles remain untouched. */
static void ws_expand_fullscreen_rect(int32_t *x, int32_t y, int *w, int h) {
    if (!ws_native_wide_active()) return;
    int X = (int)ws_disp_x(), W = (int)ws_disp_w(), H = (int)ws_disp_h();
    if (*x <= X && *x + *w >= X + W && y <= 0 && y + h >= H) {
        int off = ws_nw_offset();
        *x -= off;
        *w += 2 * off;
    }
}

/* In-game HUD pivot for an untagged screen-space SPRT spanning [x, x+w).
 * Only reached during gameplay (ws_active true). HUD elements are sparse and
 * corner-anchored, so pivot by thirds: outer-third elements anchor to their
 * screen edge (keeping the wide-screen corner position at native proportions),
 * the middle third to centre. A composite (counter box + digits) sits inside
 * one zone, so its pieces share a pivot and stay aligned. (Full-2D menu
 * screens never reach here — they get zero squash + 4:3 pillarbox instead.) */
static int32_t ws_hud_pivot(int32_t x, int32_t w) {
    int32_t X = ws_disp_x();
    int32_t W = ws_disp_w();
    int32_t cx = 2 * (x - X) + w;      /* 2*centre, avoids losing the half */
    if (3 * cx < 2 * W) return X;
    if (3 * cx > 4 * W) return X + W;
    return X + W / 2;
}

/* Automatic UI correction is deliberately tied to draw provenance, not to
 * primitive size or "does this look 2D?" guesses. Ape's ordering table submits
 * HUD/font/icon packets in the final layer, after every depth-sorted world
 * bucket. A read-only prepass finds that final rank and complete spatial groups
 * before the list streams through GP0. This excludes CPU-built characters (the
 * source of the old squashed-Spike regression) even when their packets are
 * axis-aligned, and gives animated glyphs a shared anchor on their first frame. */
static void ws_ui_prepass_invalidate_stale_why(int why, uint32_t addr) {
    if (why > 0 && why < 6) ws_ui_stale_why[why]++;
    ws_ui_stale_addr = addr;
}
static void ws_ui_prepass_invalidate_stale(void) {
    ws_ui_prepass_count = 0;
    ws_ui_prepass_nodes_reset();
    ws_ui_prepass_rank = 0xFFFFu;
    ws_auto_ui_dense = 0;
    ws_ui_reject.stale++;
}

static const WsUiPrepassItem *ws_auto_ui_item(void) {
    if (!ws_auto_ui_squash || !ws_active() ||
        gp0_cmd_source_addr == 0xFFFFFFFFu)
        return NULL;
    uint32_t src = GPU_RAM_KEY(gp0_cmd_source_addr);
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
        if (ws_ui_prepass[i].src_addr != src) continue;
        if (!ws_prepass_packet_matches(&ws_ui_prepass[i].packet_guard,
                                       gp0_cmd_buf,
                                       (uint32_t)gp0_words_needed)) {
            ws_ui_prepass_invalidate_stale_why(1, src);
            ws_ui_prepass_invalidate_stale();
            return NULL;
        }
        ws_auto_ui_candidate_count++;
        return &ws_ui_prepass[i];
    }
    return NULL;
}

static int ws_auto_ui_anchor(int32_t *out_anchor) {
    const WsUiPrepassItem *it = ws_auto_ui_item();
    if (!it) return 0;
    if (out_anchor) *out_anchor = it->group.anchor;
    return 1;
}

/* auto_ui_size = "proportional": the HUD keeps its authored size up to 16:9
 * and shrinks by sqrt((16:9) / aspect) beyond it (0.87 at 21:9, 0.71 at
 * 32:9), on both axes about the widget's anchors. The squash factor
 * xnum/xden is (4:3) / aspect, so (16:9) / aspect = (4/3) * xnum/xden.
 * Projection widescreen only: native-wide does not squash the HUD. */
static double ws_auto_ui_size(void) {
    if (!ws_auto_ui_proportional || ws_mode == 2 || ws_xden <= 0) return 1.0;
    const double r = (4.0 / 3.0) * (double)ws_xnum / (double)ws_xden;
    return r >= 1.0 ? 1.0 : sqrt(r);
}
static int32_t ws_ui_x(int32_t x, int32_t ax, double s) {
    if (s >= 1.0) return ws_scale_about(x, ax);
    return ax + (int32_t)lround((double)(x - ax) * ws_xnum / ws_xden * s);
}
static int32_t ws_ui_y(int32_t y, int32_t ay, double s) {
    if (s >= 1.0) return y;
    return ay + (int32_t)lround((double)(y - ay) * s);
}

static int ws_shared_ui_packet(void) {
    uint32_t op = gp0_cmd_buf[0] >> 24;
    if (!((op >= 0x28u && op <= 0x3Fu && (op & 8u)) ||
          (op >= 0x60u && op <= 0x7Fu))) return 0;
    if (!ws_local_viewport_cfg || ws_mode == 2 || !ws_vertical_split_active() ||
        psx_ws_prim_is_tagged()) return 0;
    int32_t anchor;
    if (!ws_auto_ui_anchor(&anchor)) return 0;
    uint32_t src = GPU_RAM_KEY(gp0_cmd_source_addr);
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++)
        if (ws_ui_prepass[i].src_addr == src)
            return ws_ui_prepass[i].shared_split_ui;
    return 0;
}

static int32_t ws_shared_ui_delta(void) {
    if (ws_shared_ui_copy < 0) return 0;
    /* Projection mode still renders the complete native framebuffer. Both
     * peers draw both copies identically, then present their selected half. */
    int32_t W = ws_disp_w();
    return (ws_shared_ui_copy ? 3 * W / 4 : W / 4) - W / 2;
}

static uint32_t ws_auto_ui_group_key_words(const uint32_t *words,
                                           uint32_t op,
                                           int32_t y, int32_t h) {
    /* An untextured polygon's word 2 is a vertex or colour, not a CLUT. */
    uint32_t clut = (op < 0x60u && !(op & 0x04u)) ? 0u : words[2] >> 16;
    uint32_t tpage = 0;
    if (op >= 0x20u && op <= 0x3Fu && (op & 0x04u)) {
        int tp_index = (op & 0x10u) ? 5 : 4;
        tpage = (words[tp_index] >> 16) & 0x1FFu;
    }
    int32_t centre_y = y + h / 2;
    uint32_t band = (uint32_t)(centre_y < 0 ? 0 : centre_y / 24) & 0x1Fu;
    uint32_t family = op < 0x60u ? 1u : 2u;
    uint32_t key = (clut * 2654435761u) ^ (tpage << 11) ^
                   (band << 3) ^ family;
    return key ? key : 1u;
}

static int ws_axis_aligned_quad(const int32_t vx[4], const int32_t vy[4]) {
    int32_t min_x = vx[0], max_x = vx[0], min_y = vy[0], max_y = vy[0];
    for (int i = 1; i < 4; i++) {
        if (vx[i] < min_x) min_x = vx[i];
        if (vx[i] > max_x) max_x = vx[i];
        if (vy[i] < min_y) min_y = vy[i];
        if (vy[i] > max_y) max_y = vy[i];
    }
    unsigned corners = 0;
    for (int i = 0; i < 4; i++) {
        if      (vx[i] == min_x && vy[i] == min_y) corners |= 1u;
        else if (vx[i] == max_x && vy[i] == min_y) corners |= 2u;
        else if (vx[i] == min_x && vy[i] == max_y) corners |= 4u;
        else if (vx[i] == max_x && vy[i] == max_y) corners |= 8u;
        else return 0;
    }
    return corners == 15u;
}

/* A widget part that is not an axis-aligned rectangle (a triangle, a rotated
 * quad): squashed with its widget when the prepass admitted it as lying
 * inside that widget (ws_ui_prepass_finish). */
static int ws_auto_ui_transform_part(int32_t *vx, int32_t *vy, int n) {
    if (ws_projection_hud_transform(vx, n)) return 1;
    if (ws_nw_explicit_hud_delta(NULL) || psx_ws_prim_is_tagged()) return 0;
    const WsUiPrepassItem *it = ws_auto_ui_item();
    if (!it) return 0;
    const double s = ws_auto_ui_size();
    for (int i = 0; i < n; i++) {
        vx[i] = ws_ui_x(vx[i], it->group.anchor, s) + ws_shared_ui_delta();
        vy[i] = ws_ui_y(vy[i], it->anchor_y, s);
    }
    ws_auto_ui_transform_count++;
    return 1;
}

static int ws_auto_ui_transform_quad(int32_t vx[4], int32_t vy[4]) {
    if (ws_projection_hud_transform(vx, 4)) return 1;
    if (ws_nw_explicit_hud_delta(NULL) || psx_ws_prim_is_tagged()) return 0;
    if (!ws_axis_aligned_quad(vx, vy)) return ws_auto_ui_transform_part(vx, vy, 4);

    int32_t min_x = vx[0], max_x = vx[0], min_y = vy[0], max_y = vy[0];
    for (int i = 1; i < 4; i++) {
        if (vx[i] < min_x) min_x = vx[i];
        if (vx[i] > max_x) max_x = vx[i];
        if (vy[i] < min_y) min_y = vy[i];
        if (vy[i] > max_y) max_y = vy[i];
    }
    int32_t width = max_x - min_x, height = max_y - min_y;
    int32_t X = ws_disp_x(), W = ws_disp_w(), H = ws_disp_h();
    if ((min_x <= X && max_x >= X + W && min_y <= 0 && max_y >= H) ||
        (width > W / 2 && height > H / 4))
        return 0;

    const WsUiPrepassItem *it = ws_auto_ui_item();
    if (!it) return 0;
    const double s = ws_auto_ui_size();
    for (int i = 0; i < 4; i++) {
        vx[i] = ws_ui_x(vx[i], it->group.anchor, s) + ws_shared_ui_delta();
        vy[i] = ws_ui_y(vy[i], it->anchor_y, s);
    }
    ws_auto_ui_transform_count++;
    return 1;
}

static int ws_auto_ui_transform_rect(int32_t *x, int32_t *y, int *w, int *h) {
    if (!x || !y || !w || !h || *w <= 0) return 0;
    int32_t explicit_anchor;
    if (ws_projection_hud_anchor(&explicit_anchor)) {
        *x = ws_scale_about(*x, explicit_anchor);
        *w = (int)ws_scale_len(*w);
        return 1;
    }
    if (ws_nw_explicit_hud_delta(NULL) || psx_ws_prim_is_tagged())
        return 0;
    int32_t X = ws_disp_x(), W = ws_disp_w(), H = ws_disp_h();
    if ((*x <= X && *x + *w >= X + W && *y <= 0 && *y + *h >= H) ||
        (*w > W / 2 && *h > H / 4))
        return 0;
    const WsUiPrepassItem *it = ws_auto_ui_item();
    if (!it) return 0;
    const double s = ws_auto_ui_size();
    if (s >= 1.0) {
        *x = ws_scale_about(*x, it->group.anchor) + ws_shared_ui_delta();
        *w = (int)ws_scale_len(*w);
    } else {
        /* Scale both edges so neighbouring parts of a widget stay butted. */
        const int32_t x0 = ws_ui_x(*x, it->group.anchor, s);
        const int32_t x1 = ws_ui_x(*x + *w, it->group.anchor, s);
        const int32_t y0 = ws_ui_y(*y, it->anchor_y, s);
        const int32_t y1 = ws_ui_y(*y + *h, it->anchor_y, s);
        *x = x0 + ws_shared_ui_delta();
        *w = x1 - x0 < 1 ? 1 : (int)(x1 - x0);
        *y = y0;
        *h = y1 - y0 < 1 ? 1 : (int)(y1 - y0);
    }
    ws_auto_ui_transform_count++;
    return 1;
}

/* Shared transform for fixed-size textured sprites (8x8 / 16x16 / 1x1 dot):
 * squash *x0 in place (around the tagged anchor, else the HUD pivot) and
 * return the squashed draw width, or 0 = no change. */
static int ws_sprt_fixed_transform(int32_t *x0, int32_t *y0, int w, int *h) {
    if (!ws_active()) return 0;
    int32_t ax;
    if (ws_tagged_anchor(&ax)) {
        *x0 = ws_scale_about(*x0, ax);
        return (int)ws_scale_len(w);
    }
    int auto_w = w;
    if (ws_auto_ui_transform_rect(x0, y0, &auto_w, h))
        return auto_w;
    if (ws_hud_sprt) {
        *x0 = ws_scale_about(*x0, ws_hud_pivot(*x0, w));
        return (int)ws_scale_len(w);
    }
    return 0;
}

/* ---- Native-wide HUD corner re-anchoring ([widescreen] nw_hud_corners) ------
 * In native-wide the whole frame is composited into a wider surface centred by
 * ws_nw_offset() per side (the reveal). Screen-space 2D HUD (drawn with fixed
 * rect/sprite GP0 commands, never through the GTE — a 3D title's world is all
 * polygons) therefore lands inset from the true wide edges by exactly the
 * reveal. This pushes an outer-third HUD primitive the rest of the way to its
 * wide corner with an additive thirds shift: left third −offset, right third
 * +offset, middle unchanged (matches the squash-path ws_hud_pivot geometry, but
 * as a translate since native-wide does not squash). Composite pieces in one
 * zone share a shift and stay aligned. A configured command-source range lets
 * pure-2D games opt in only their HUD packet arena, leaving world primitives
 * untouched. Identity unless native-wide is engaged AND the game opts in, so
 * 4:3 and non-opted titles are byte-identical.
 * Returns the signed x delta to add to the prim's x before draw_offset. */
static int ws_nw_hud_corners = 0;
static int ws_nw_hud_tag_rects = 0;   /* rects shift even when tagged (A/B) */
static uint32_t ws_nw_left_hud_packet_lo = 0;
static uint32_t ws_nw_left_hud_packet_hi = 0;
/* Explicit title-owned roles. Separate from sprite tags: world sprites and
 * HUD can share the same builder, and either entry hook may run first. Clear
 * tags for recycled non-HUD packets; frame expiry protects unused addresses. */
static WsPrimitiveRole ws_primitive_roles[WS_ROLE_BUCKETS];
void gpu_ws_tag_hud_primitive(uint32_t primitive, int edge) {
    WsPrimitiveRole* tag = ws_role_write(ws_primitive_roles, primitive,
                                        (uint32_t)s_frame_count);
    if (tag) tag->hud_edge = edge < 0 ? -1 : edge > 0 ? 1 : 0;
}
void gpu_ws_tag_world_primitive(uint32_t primitive, int is_world) {
    WsPrimitiveRole* tag = ws_role_write(ws_primitive_roles, primitive,
                                        (uint32_t)s_frame_count);
    if (tag) tag->world = is_world != 0;
}
static int ws_tagged_world_primitive(void) {
    const WsPrimitiveRole* tag = ws_role_read(ws_primitive_roles,
        gp0_cmd_source_addr, (uint32_t)s_frame_count);
    return tag && tag->world;
}
static int ws_tagged_hud_edge(void) {
    const WsPrimitiveRole* tag = ws_role_read(ws_primitive_roles,
        gp0_cmd_source_addr, (uint32_t)s_frame_count);
    return tag ? tag->hud_edge : 0;
}
void gpu_ws_set_nw_hud_corners(int on) { ws_nw_hud_corners = on ? 1 : 0; }
void gpu_ws_set_nw_hud_tag_rects(int on) { ws_nw_hud_tag_rects = on ? 1 : 0; }
void gpu_ws_set_nw_left_hud_packet_range(uint32_t lo, uint32_t hi) {
    ws_nw_left_hud_packet_lo = lo & 0x1FFFFFFFu;
    ws_nw_left_hud_packet_hi = hi & 0x1FFFFFFFu;
}
static int ws_nw_left_hud_packet(void) {
    if (!ws_native_wide_active() || gp0_cmd_source_addr == 0xFFFFFFFFu ||
        ws_nw_left_hud_packet_hi <= ws_nw_left_hud_packet_lo)
        return 0;
    uint32_t a = gp0_cmd_source_addr & 0x1FFFFFFFu;
    int matched = a >= ws_nw_left_hud_packet_lo && a < ws_nw_left_hud_packet_hi;
    return matched;
}
static int32_t ws_nw_hud_shift(int32_t x, int32_t w) {
    if (!ws_native_wide_active()) return 0;
    if (g_bg2d_host_size && ws_tagged_world_primitive()) return 0;
    int32_t off = ws_nw_offset();
    if (off <= 0) return 0;
    int hud_edge = ws_tagged_hud_edge();
    if (hud_edge) return hud_edge * off;
    int32_t explicit_delta = 0;
    if (ws_nw_explicit_hud_delta(&explicit_delta)) return explicit_delta;
    if (!ws_nw_left_hud_packet() && !ws_nw_hud_corners) return 0;
    /* Sprite-tag titles (anchor configured): HUD ≡ UNTAGGED rect-family prims
     * — the same discriminator the squash path's hud_sprt_squash used. Tagged
     * prims are character billboards positioned from their GTE anchor; they
     * must never re-anchor. (Poly/line sites are excluded wholesale for tag
     * titles in ws_nw_hud_shift_vertices — their polys are the world.)
     * ws_nw_hud_tag_rects (TCP ws_hud_mode) lifts the exclusion for rects,
     * for live A/B: some HUD composites (Tomba's AP counter) render through
     * the tagged sprite funnel and stay inset without it. */
    if (ws_anchor_addr && !ws_nw_hud_tag_rects && psx_ws_prim_is_tagged()) return 0;
    int32_t X  = ws_disp_x();
    int32_t W  = ws_disp_w();
    int32_t cx = 2 * (x - X) + w;      /* 2*centre, avoids losing the half */
    if (3 * cx < 2 * W) return -off;   /* left third  -> pull to left edge  */
    if (3 * cx > 4 * W) return  off;   /* right third -> push to right edge */
    return 0;                          /* middle third -> stay centred      */
}

/* Re-anchor every X coordinate of a HUD polygon as one rigid composite. The
 * command-source filter above is what makes this safe for pure-2D titles: only
 * the game's dedicated HUD packet arena reaches here, never world polygons. */
static void ws_nw_hud_shift_vertices(int32_t *vx, int count) {
    if (count <= 0) return;
    int32_t explicit_delta = 0;
    if (ws_nw_explicit_hud_delta(&explicit_delta)) {
        if (explicit_delta)
            for (int i = 0; i < count; i++) vx[i] += explicit_delta;
        return;
    }
    /* Sprite-tag titles: polygon/line prims are the GTE world and the tagged
     * character billboards, never HUD — only the rect-family sites (which
     * call ws_nw_hud_shift directly, with the untagged filter) re-anchor. */
    if (ws_anchor_addr) return;
    int32_t lo = vx[0], hi = vx[0];
    for (int i = 1; i < count; i++) {
        if (vx[i] < lo) lo = vx[i];
        if (vx[i] > hi) hi = vx[i];
    }
    int32_t d = ws_nw_hud_shift(lo, hi - lo);
    if (d) for (int i = 0; i < count; i++) vx[i] += d;
}

/* ---- Native-wide full-frame 2D backdrop stretch ([widescreen] nw_backdrop) ---
 * A screen-space background plane (sky gradient / backdrop image) is drawn as an
 * axis-aligned quad covering the whole 4:3 framebuffer [0,W]x[0,H]. It is NOT
 * GTE-projected, so native-wide leaves it at its 4:3 span, composited centred =
 * black bars in the revealed side margins (the pillarboxed sky). Detect exactly
 * that shape — a 4-vertex quad whose corners form a rectangle spanning ~the full
 * display width from ~the left edge — and stretch its X vertices about the
 * display centre by the wide ratio so it fills the wider frame; the texture/UV
 * (or gradient) simply stretches horizontally (invisible on a sky). GTE-drawn
 * world quads are perspective-distorted (not axis-aligned) and partial-width, so
 * they never match. Identity unless native-wide is engaged AND the game opts in.
 * Transforms vx[0..3] IN PLACE (pre-draw_offset); returns 1 if it applied. */
static int ws_nw_backdrop = 0;
void gpu_ws_set_nw_backdrop(int on) { ws_nw_backdrop = on ? 1 : 0; }
static int ws_nw_flat_backdrop = 0;
void gpu_ws_set_nw_flat_backdrop(int on) { ws_nw_flat_backdrop = on ? 1 : 0; }
int gpu_ws_nw_flat_backdrop_enabled(void) { return ws_nw_flat_backdrop; }
static int ws_nw_backdrop_stretch_quad(int32_t *vx, const int32_t *vy) {
    if (!ws_nw_backdrop || !ws_native_wide_active() || ws_nw_explicit_hud_delta(NULL)) return 0;
    int32_t extra = ws_nw_extra();
    if (extra <= 0) return 0;
    int32_t X = ws_disp_x();
    int32_t W = ws_disp_w();
    const int32_t EDGE = 24;                 /* slack for "touches the frame edge" */
    int32_t minx = vx[0], maxx = vx[0], miny = vy[0], maxy = vy[0];
    for (int i = 1; i < 4; i++) {
        if (vx[i] < minx) minx = vx[i];
        if (vx[i] > maxx) maxx = vx[i];
        if (vy[i] < miny) miny = vy[i];
        if (vy[i] > maxy) maxy = vy[i];
    }
    /* Must span the full display width and a real vertical extent. */
    if (minx > X + EDGE || maxx < X + W - EDGE || (maxy - miny) < 64) return 0;
    /* Axis-aligned: every vertex X sits at either the min or the max edge, and
     * every Y at the top or bottom edge (a true screen-space rectangle). */
    for (int i = 0; i < 4; i++) {
        int xe = (vx[i] - minx <= EDGE) || (maxx - vx[i] <= EDGE);
        int ye = (vy[i] - miny <= EDGE) || (maxy - vy[i] <= EDGE);
        if (!xe || !ye) return 0;
    }
    /* Stretch X about the display centre by (W+extra)/W so [0,W] -> [-off, W+off],
     * which the wide compositor (+off) maps onto the full [0, W+extra] surface. */
    int32_t cx = X + W / 2;
    for (int i = 0; i < 4; i++) {
        int32_t d = vx[i] - cx;
        vx[i] = cx + (d * (W + extra) + (d >= 0 ? W / 2 : -W / 2)) / W;
    }
    return 1;
}

/* Polyline state */
static uint16_t polyline_color;       /* mono polyline: current color */
static int32_t  polyline_prev_x, polyline_prev_y;  /* previous vertex */
static uint16_t polyline_prev_c;      /* shaded polyline: previous color */
static int      polyline_semi_trans;  /* semi-transparency flag from command word */
static int      polyline_has_prev;    /* have we seen at least one vertex? */

/* VRAM write transfer state (CPU→VRAM, command 0xA0) */
static uint16_t vram_write_x, vram_write_y;   /* start coords */
static uint16_t vram_write_w, vram_write_h;   /* dimensions */
static uint16_t vram_write_col, vram_write_row; /* current offset */
static uint32_t vram_write_remaining;          /* words remaining */
/* Stage one complete GP0(A0) transfer so renderer backends receive one bulk
 * rectangle instead of hundreds of thousands of single-pixel callbacks. The
 * CPU-visible transfer remains ordered because GP0 accepts no next command
 * until this payload is complete. Maximum PS1 transfer = full VRAM (1 MiB). */
static uint16_t vram_write_pixels[1024 * 512];

/* Depth24 CPU→VRAM upload span (halfwords, exclusive end). See
 * gpu_depth24_rgb_limit — declared early so gpu_reset_state can clear it. */
static uint32_t s_d24_upload_x1 = 0;
static int      s_d24_present_hold = 0; /* vblanks to skip Swap after GP1(07h) */
/* Whether the latest guest VBlank was held: presents after it skip their
 * Swap. Both the count and this flag are guest state, set only at guest
 * VBlank and carried in the GPU snap (packed into the hold word), and a
 * present only reads them, so a windowed, headless or frame-skipping run and
 * a rollback replay hold the same span and restore the same output (#636: a
 * present-ticked hold stuck at 3 in headless live play and was zeroed by a
 * rollback load, so replay != live). */
static int      s_d24_present_skip = 0;
static uint32_t s_d24_prev_disp_h = 0;  /* last GP1(07h) band height */
static void depth24_note_upload(uint32_t x, uint32_t w);

static void gp0_commit_cpu_to_vram(void) {
    for (uint32_t row = 0; row < vram_write_h; row++)
        for (uint32_t col = 0; col < vram_write_w; col++)
            vram_write_pixels[row * vram_write_w + col] =
                vram[((vram_write_y + row) & 511u) * 1024u +
                     ((vram_write_x + col) & 1023u)];
    gr_vram_upload_commit(vram_write_x, vram_write_y,
                          vram_write_w, vram_write_h, vram_write_pixels);
    depth24_note_upload(vram_write_x, vram_write_w);
    gp0_state = GP0_IDLE;
    vram_write_remaining = 0;
    text_xlate_vram_upload(vram_write_x, vram_write_y,
                           vram_write_w, vram_write_h);
}

/* VRAM read transfer state (VRAM→CPU, command 0xC0) */
static int      vram_read_active;
static uint16_t vram_read_x, vram_read_y;
static uint16_t vram_read_w, vram_read_h;
static uint16_t vram_read_col, vram_read_row;

/* ---- GPU internal state ---- */

/* Texture page / draw mode (set by GP0(E1h), reflected in GPUSTAT bits 0-10) */
static uint32_t texpage_x;         /* bits 0-3: texture page X base (N*64) */
static uint32_t texpage_y;         /* bit 4: texture page Y base (0 or 256) */
static uint32_t semi_transparency; /* bits 5-6 */
static uint32_t texpage_colors;    /* bits 7-8: 4bit/8bit/15bit */
static uint32_t dither_enabled;    /* bit 9 */
static uint32_t draw_to_display;   /* bit 10: drawing to display area allowed */
static uint32_t set_mask_bit;      /* bit 11 */
static uint32_t check_mask_bit;    /* bit 12 */
static uint32_t interlace_field;   /* bit 13 */
static uint32_t reverse_flag;      /* bit 14 */
static uint32_t texture_disable;   /* bit 15 */

/* Draw area (set by GP0(E3h)/GP0(E4h)) */
static uint32_t draw_area_left, draw_area_top;
static uint32_t draw_area_right, draw_area_bottom;

/* Where the GPU may have drawn since the last read through the renderer, in
 * 16x16 cells (64 x 32, one bit each): the drawing area of every drawing
 * command, fill and copy destinations. CPU uploads write this array and the
 * renderer alike, so outside these cells the array already holds what the
 * renderer has, and gpu_vram_peek there needs no renderer read. With the
 * render thread, a renderer read is a sync point: the emulation thread waits
 * for the whole queue (a title polling VRAM every VBlank waited 1-3 ms). */
static uint64_t s_gpu_drawn[32];
static int      s_gpu_drawn_area = 1;   /* the drawing area is not marked yet */

static void gpu_drawn_mark(int x0, int y0, int x1, int y1) {
    if (x1 < x0 || y1 < y0) return;
    uint64_t cols;
    if (x1 > 1023 || (x1 >> 4) - (x0 >> 4) >= 63) cols = ~0ull;   /* wraps or all */
    else {
        const int c0 = x0 >> 4, c1 = x1 >> 4;
        cols = ((1ull << (c1 - c0 + 1)) - 1ull) << c0;
    }
    if (y0 > 511 || y1 > 511) { y0 = 0; y1 = 511; }   /* wraps */
    for (int r = y0 >> 4; r <= (y1 >> 4); r++) s_gpu_drawn[r] |= cols;
}

/* A drawing command: the GPU clips it to the drawing area. */
static inline void gpu_drawn_note_draw(void) {
    if (!s_gpu_drawn_area) return;
    s_gpu_drawn_area = 0;
    gpu_drawn_mark((int)draw_area_left, (int)draw_area_top,
                   (int)draw_area_right, (int)draw_area_bottom);
}

static int draw_area_intersects_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return 0;
    int r = x + w - 1;
    int b = y + h - 1;
    return !((int)draw_area_right < x ||
             (int)draw_area_left > r ||
             (int)draw_area_bottom < y ||
             (int)draw_area_top > b);
}

static int ws_local_viewport_draw_target(int *base_x) {
    int base = 0, src_w = 0;
    if (!ws_local_viewport_layout(&base, &src_w, NULL, NULL))
        return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    /* Native double buffering draws the next frame in the other Y band.
     * The per-camera wide surface spans every band, just like the 1P surface. */
    if (draw_area_intersects_rect(base, (int)draw_area_top,
                                  src_w, (int)di.height)) {
        if (base_x) *base_x = base;
        return 1;
    }
    return 0;
}

static int ws_display_viewport_draw_target(int *base_x) {
    if (!ws_native_wide_active())
        return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.depth24 || di.width == 0 || di.height == 0)
        return 0;
    if (!draw_area_intersects_rect((int)di.display_x, (int)di.display_y,
                                   (int)di.width, (int)di.height))
        return 0;
    if (base_x) *base_x = (int)di.display_x;
    return 1;
}

typedef struct GpuVerticalSplitTrace {
    uint8_t left_seen;
    uint8_t right_seen;
    uint16_t display_w;
    uint16_t display_h;
} GpuVerticalSplitTrace;

static GpuVerticalSplitTrace split_trace_this;
static GpuVerticalSplitTrace split_trace_last;
static uint8_t split_recent_left_age = 255;
static uint8_t split_recent_right_age = 255;
static uint16_t split_recent_display_w = 0;
static uint16_t split_recent_display_h = 0;

/* Draw offset (set by GP0(E5h)) */
static int32_t draw_offset_x, draw_offset_y;

/* Faces the game culls (gte.cpp NCLIP), as shadow-only casters: VRAM px and
 * SZ per corner, in the drawing area they were meant for. A screen corner
 * the GTE saturated, or one too near the camera, drops the face. */
enum { SH_BF_MAX = 16384 };
static float    sh_bf_v[SH_BF_MAX][9];
static int      sh_bf_area[SH_BF_MAX][4];
static uint32_t sh_bf_vra[SH_BF_MAX];   /* the routine that projected it (below) */
static int      sh_bf_n = 0;
void psx_shadow_backface(const int32_t sxy[3], const uint16_t sz[3]) {
    if (sh_bf_n >= SH_BF_MAX) return;
    float *v = sh_bf_v[sh_bf_n];
    for (int i = 0; i < 3; i++) {
        const int x = (int16_t)(sxy[i] & 0xFFFF), y = (int16_t)(sxy[i] >> 16);
        if (sz[i] < 1024u || x <= -1024 || x >= 1023 || y <= -1024 || y >= 1023) return;
        v[i * 3] = (float)(x + draw_offset_x); v[i * 3 + 1] = (float)(y + draw_offset_y); v[i * 3 + 2] = (float)sz[i];
    }
    sh_bf_area[sh_bf_n][0] = (int)draw_area_left; sh_bf_area[sh_bf_n][1] = (int)draw_area_top;
    sh_bf_area[sh_bf_n][2] = (int)draw_area_right; sh_bf_area[sh_bf_n][3] = (int)draw_area_bottom;
    sh_bf_vra[sh_bf_n] = 0;
    sh_bf_n++;
}
/* Routines that draw both windings. The winding alone takes a face as
 * culled; a routine that draws its faces either way round (a bush's leaf
 * cards) would then cast each card it shows from behind twice, once cut out
 * by its texture and once whole, and the whole outline of each card it does
 * not draw (off the picture). So the faces taken from a routine (the return
 * address its GTE projections run under) are kept for the last SH_BFM_GEN
 * VBlanks that took any (a 30 fps game projects a frame, then draws it a
 * VBlank or two later), by their screen corners (the GTE's own, which the
 * game copies into its polygons); a textured polygon drawn with all three
 * corners of one marks its routine as one that does not cull, and no face
 * of it is taken again (learned for the session). A closed object's routine
 * never draws its far side, so its faces stay. */
enum { SH_TS_MAX = 64, SH_BFM_HASH = 2048, SH_BFM_GEN = 3 };
static uint32_t sh_ts_ra[SH_TS_MAX];
static int16_t  sh_bfm_xy[SH_BFM_GEN][SH_BF_MAX][6];   /* [cur] the faces of this VBlank, the others of earlier ones */
static uint32_t sh_bfm_ra[SH_BFM_GEN][SH_BF_MAX];
static int      sh_bfm_next[SH_BFM_GEN][SH_BF_MAX];
static int      sh_bfm_head[SH_BFM_GEN][SH_BFM_HASH];
static int      sh_bfm_n[SH_BFM_GEN], sh_bfm_cur = 0, sh_bfm_init = 0;
static unsigned sh_bfm_key(int x, int y) { return ((unsigned)x * 73856093u ^ (unsigned)y * 19349663u) & (SH_BFM_HASH - 1); }
static int sh_ts_has(uint32_t ra) {
    for (int i = 0; i < sh_ts_n; i++) if (sh_ts_ra[i] == ra) return 1;
    return 0;
}
static void sh_bfm_reset(int l) {
    for (int b = 0; b < SH_BFM_HASH; b++) sh_bfm_head[l][b] = -1;
    sh_bfm_n[l] = 0;
}
static void sh_bfm_add(const int32_t sxy[3], uint32_t ra) {
    if (!sh_bfm_init) { for (int l = 0; l < SH_BFM_GEN; l++) sh_bfm_reset(l); sh_bfm_init = 1; }
    const int l = sh_bfm_cur, k = sh_bfm_n[l];
    if (k >= SH_BF_MAX) return;
    for (int i = 0; i < 3; i++) {
        sh_bfm_xy[l][k][i * 2] = (int16_t)(sxy[i] & 0xFFFF); sh_bfm_xy[l][k][i * 2 + 1] = (int16_t)(sxy[i] >> 16);
    }
    sh_bfm_ra[l][k] = ra;
    const unsigned h = sh_bfm_key(sh_bfm_xy[l][k][0], sh_bfm_xy[l][k][1]);
    sh_bfm_next[l][k] = sh_bfm_head[l][h]; sh_bfm_head[l][h] = k;
    sh_bfm_n[l] = k + 1;
}
/* A textured polygon the game draws (n corners, before the draw offset). */
static void shadow_bf_drawn(const int32_t *vx, const int32_t *vy, int n) {
    if (!sh_bf_on || !sh_bfm_init) return;
    for (int l = 0; l < SH_BFM_GEN; l++)
        for (int c = 0; c < n; c++)
            for (int k = sh_bfm_head[l][sh_bfm_key((int16_t)vx[c], (int16_t)vy[c])]; k >= 0; k = sh_bfm_next[l][k]) {
                const int16_t *f = sh_bfm_xy[l][k];
                if (f[0] != (int16_t)vx[c] || f[1] != (int16_t)vy[c]) continue;
                int m = 1;
                for (int i = 1; i < 3; i++)
                    for (int j = 0; j < n; j++)
                        if (j != c && f[i * 2] == (int16_t)vx[j] && f[i * 2 + 1] == (int16_t)vy[j]) { m++; break; }
                if (m == 3 && !sh_ts_has(sh_bfm_ra[l][k]) && sh_ts_n < SH_TS_MAX) sh_ts_ra[sh_ts_n++] = sh_bfm_ra[l][k];
            }
}
/* Called before every RTPS/RTPT (psx_ws_note_gte_project), when the GTE still
 * holds the previous projection's results: a triangle an RTPT left whose
 * winding is not the one the game draws is a face it will cull (its NCLIP
 * test), and when an RTPS (the 4th corner of a quad) followed it, the FIFO
 * now holds that quad's second half. Read through the mod services (the
 * running build's CPU layout). gte.cpp is part of the savestate execution
 * contract, so the hook lives here. */
extern int psx_mod_gte_data(uint32_t out[32]);
static int sh_bf_prev = 0, sh_bf_prev_back = 0;   /* last projection: 3 = RTPT, 1 = RTPS */
static uint32_t sh_bf_prev_ra = 0;                /* its return address */
static void sh_bf_take(const uint32_t *d, uint32_t ra) {
    if (sh_ts_has(ra)) return;
    const int32_t sxy[3] = {(int32_t)d[12], (int32_t)d[13], (int32_t)d[14]};
    const uint16_t sz[3] = {(uint16_t)d[17], (uint16_t)d[18], (uint16_t)d[19]};
    const int n0 = sh_bf_n;
    psx_shadow_backface(sxy, sz);
    if (sh_bf_n > n0) sh_bf_vra[n0] = ra;
    sh_bfm_add(sxy, ra);
}
static void shadow_note_projection(int nverts) {
    if (sh_track && sh_camera == 2) {
        shadow_camera_note();   /* the matrix this projection uses */
        if (nverts == 1 && sh_anc_live) shadow_anchor_note();
        if (nverts == 1) shadow_point_note();
    }
    const int prev = sh_bf_prev;
    const uint32_t pra = sh_bf_prev_ra;
    sh_bf_prev = nverts;
    if (!sh_bf_on) return;
    sh_bf_prev_ra = psx_mod_cpu_gpr(31);
    if (!prev) return;
    uint32_t d[32];
    if (!psx_mod_gte_data(d)) return;
    if (prev == 3) {
        const int front = gl_renderer_shadow_front_sign();
        const int32_t x0 = (int16_t)d[12], y0 = (int16_t)(d[12] >> 16);
        const int32_t x1 = (int16_t)d[13], y1 = (int16_t)(d[13] >> 16);
        const int32_t x2 = (int16_t)d[14], y2 = (int16_t)(d[14] >> 16);
        const int64_t a = (int64_t)(x1 - x0) * (y2 - y0) - (int64_t)(x2 - x0) * (y1 - y0);
        sh_bf_prev_back = front != 0 && a != 0 && (a > 0 ? 1 : -1) != front;
        if (sh_bf_prev_back) sh_bf_take(d, pra);
    } else if (prev == 1 && sh_bf_prev_back) {
        sh_bf_take(d, pra);       /* v1 v2 v3 of the culled quad */
        sh_bf_prev_back = 0;
    } else {
        sh_bf_prev_back = 0;
    }
}
static void shadow_backfaces_flush(void) {
    int n = 0;   /* less the faces of a routine learned since they were taken */
    for (int i = 0; i < sh_bf_n; i++) {
        if (sh_ts_n && sh_ts_has(sh_bf_vra[i])) continue;
        if (n != i) {
            memcpy(sh_bf_v[n], sh_bf_v[i], sizeof sh_bf_v[0]);
            memcpy(sh_bf_area[n], sh_bf_area[i], sizeof sh_bf_area[0]);
        }
        n++;
    }
    if (n) gl_renderer_shadow_add_backfaces(&sh_bf_v[0][0], &sh_bf_area[0][0], n);
    sh_bf_n = 0;
    if (sh_bfm_init && sh_bfm_n[sh_bfm_cur]) { sh_bfm_cur = (sh_bfm_cur + 1) % SH_BFM_GEN; sh_bfm_reset(sh_bfm_cur); }
}
/* Instrumentation: per-vblank range/count of GP0(E5) draw-offset-Y sets. If a
 * single frame sets offsets in BOTH the top (y<128) and bottom (y>=128) buffer
 * bands, the game is drawing different parts of the scene into different display
 * buffers in the same frame (candidate root of a character-in-one-buffer
 * strobe). Snapshotted each vblank into the *_last copies, exposed via debug. */
int32_t  g_doff_min_this = 0x7fffffff, g_doff_max_this = -0x7fffffff;
uint32_t g_doff_cnt_this = 0;
int32_t  g_doff_min_last = 0, g_doff_max_last = 0;
uint32_t g_doff_cnt_last = 0;

/* Texture window raw value (set by GP0(E2h), readback via GP1(10h)) */
static uint32_t texture_window_value;

/* Display mode (set by GP1(08h), reflected in GPUSTAT bits 16-22) */
static uint32_t hres2;            /* bit 16: horizontal resolution 2 (368 mode) */
static uint32_t hres1;            /* bits 17-18: horizontal resolution 1 */
static uint32_t vres;             /* bit 19: vertical resolution (0=240, 1=480) */
static uint32_t video_mode;       /* bit 20: 0=NTSC, 1=PAL */
/* Video standard at power-on, before any GP1(08h): the disc region stands in
 * for a region BIOS's choice (gpu_set_power_on_video_mode). */
static uint32_t s_power_on_video_mode;
static uint32_t display_depth;    /* bit 21: 0=15bit, 1=24bit */
static uint32_t vertical_interlace; /* bit 22 */

/* Display enable (set by GP1(03h)) */
static uint32_t display_disabled; /* bit 23: 0=enabled, 1=disabled */

/* IRQ1 (set by GP0(1Fh), acked by GP1(02h)) */
static uint32_t irq1_flag;       /* bit 24 */

/* DMA direction (set by GP1(04h)) */
static uint32_t dma_direction;   /* bits 29-30 in GPUSTAT */

/* LCF (even/odd line in interlace, toggles per vblank) */
static uint32_t lcf;             /* bit 31 */

/* I_STAT — defined in memory.c; declared early so gpu_read_gpustat can
 * raise the VBLANK bit when a synthetic vblank fires from a tight
 * BIOS-shell GPUSTAT poll loop, without going through the
 * vblank-callback path (which would call SDL_Delay/Sleep). */
extern uint32_t i_stat;
/* Central IRQ-raise choke point (interrupts.c) — also records the device ring. */
extern void psx_irq_raise(uint32_t bit, uint32_t detail);

/* Display area start (GP1(05h)) */
static uint32_t display_area_x;
static uint32_t display_area_y;
static uint32_t ws_display_origin(void) {
    return display_area_x | (display_area_y << 10);
}

/* ----- Native-wide compositor driving (see runtime/src/gpu_sw_renderer.c) ----
 * The renderer keeps a separate wide surface per framebuffer; we tell it which
 * back buffer each draw targets and present from the displayed buffer's surface.
 * A "framebuffer base" is a VRAM x-origin the display has scanned out from — we
 * learn that set from GP1(05h) so the mirror only engages for real display
 * buffers (not offscreen texture builds). Generic: no per-game constants. */
#define WS_FB_BASES 4
static uint32_t ws_fb_base[WS_FB_BASES];
static int      ws_fb_n = 0;
static void ws_note_display_base(uint32_t bx) {
    for (int i = 0; i < ws_fb_n; i++) if (ws_fb_base[i] == bx) return;
    if (ws_fb_n < WS_FB_BASES) { ws_fb_base[ws_fb_n++] = bx; return; }
    for (int i = 1; i < WS_FB_BASES; i++) ws_fb_base[i - 1] = ws_fb_base[i];
    ws_fb_base[WS_FB_BASES - 1] = bx;
}
static int ws_is_fb_base(uint32_t bx) {
    for (int i = 0; i < ws_fb_n; i++) if (ws_fb_base[i] == bx) return 1;
    return 0;
}

static void split_trace_reset(GpuVerticalSplitTrace *trace) {
    memset(trace, 0, sizeof(*trace));
}

static void split_trace_note_draw_area(void) {
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.depth24 || di.width < 256 || di.height < 128)
        return;
    if ((di.width & 1u) != 0)
        return;
    if (di.display_x + di.width > 1024u || di.display_y + di.height > 512u)
        return;
    if (draw_area_right <= draw_area_left || draw_area_bottom <= draw_area_top)
        return;

    const int disp_l = (int)di.display_x;
    const int disp_r = (int)(di.display_x + di.width - 1u);
    const int mid = disp_l + (int)(di.width / 2u);
    const int tol = (int)di.width / 32 > 8 ? (int)di.width / 32 : 8;

    const int area_l = (int)draw_area_left;
    const int area_r = (int)draw_area_right;

    if (split_trace_this.display_w != 0 &&
        (split_trace_this.display_w != (uint16_t)di.width ||
         split_trace_this.display_h != (uint16_t)di.height)) {
        split_trace_reset(&split_trace_this);
    }
    split_trace_this.display_w = (uint16_t)di.width;
    split_trace_this.display_h = (uint16_t)di.height;

    if (area_l <= disp_l + tol &&
        area_r >= mid - 1 - tol && area_r <= mid - 1 + tol) {
        split_trace_this.left_seen = 1;
    }
    if (area_l >= mid - tol && area_l <= mid + tol &&
        area_r >= disp_r - tol) {
        split_trace_this.right_seen = 1;
    }
}

#define WS_LOCAL_VIEWPORT_STATE_VALUES_MAX 16
static uint32_t ws_local_viewport_state_addr = 0;
static uint32_t ws_local_viewport_state_values[WS_LOCAL_VIEWPORT_STATE_VALUES_MAX];
static int ws_local_viewport_state_value_count = 0;
void gpu_ws_set_local_viewport_state_gate(uint32_t addr,
                                          const uint32_t *values, int nvalues) {
    if (nvalues < 0) nvalues = 0;
    if (nvalues > WS_LOCAL_VIEWPORT_STATE_VALUES_MAX)
        nvalues = WS_LOCAL_VIEWPORT_STATE_VALUES_MAX;
    ws_local_viewport_state_addr = values && nvalues ? addr : 0;
    ws_local_viewport_state_value_count = values ? nvalues : 0;
    for (int i = 0; i < ws_local_viewport_state_value_count; i++)
        ws_local_viewport_state_values[i] = values[i];
}

static int ws_local_viewport_state_ok(void) {
    if (!ws_local_viewport_state_addr || ws_local_viewport_state_value_count == 0)
        return 1;
    const uint32_t state = psx_read_word(ws_local_viewport_state_addr);
    for (int i = 0; i < ws_local_viewport_state_value_count; i++)
        if (state == ws_local_viewport_state_values[i]) return 1;
    return 0;
}

static int ws_vertical_split_active(void) {
    return split_recent_left_age <= 8 && split_recent_right_age <= 8 &&
           ws_local_viewport_state_ok();
}

int gpu_last_frame_vertical_split_screen(void) {
    return ws_vertical_split_active();
}

void gpu_vertical_split_debug(int *active, int *left_age, int *right_age) {
    if (active) *active = gpu_last_frame_vertical_split_screen();
    if (left_age) *left_age = split_recent_left_age;
    if (right_age) *right_age = split_recent_right_age;
}

/* Point the renderer's wide mirror at the current back buffer (draw_area_left)
 * when native-wide is active and that buffer is a known display buffer; else
 * disable mirroring for this draw. Called when the draw env changes. */
static void ws_nw_sync_target(void) {
    if (!ws_native_wide_active()) {
        gr_wide_set_view(0, 0, 0, 0);
        gr_wide_disable_target(); return;
    }
    gr_wide_configure(ws_nw_present_width(), ws_nw_offset());
    int local_base = 0;
    if (ws_local_viewport_draw_target(&local_base)) {
        gr_wide_set_target(local_base);
        return;
    }
    if (ws_local_viewport_cfg) { gr_wide_disable_target(); return; }
    int display_base = 0;
    if (ws_display_viewport_draw_target(&display_base)) {
        gr_wide_set_target(display_base);
        return;
    }
    uint32_t base = draw_area_left;
    if (ws_is_fb_base(base)) gr_wide_set_target((int)base);
    else                     gr_wide_disable_target();
}

static void ws_clear_all_reveal_margins(void) {
    for (int i = 0; i < ws_fb_n; i++)
        gr_wide_clear_margins((int)ws_fb_base[i], 0, 512, 0, 3);
}

static void ws_clear_tagged_rect_reveal(int y, int h) {
    if (!ws_native_wide_active() || h <= 0 || gp0_cmd_source_addr == UINT32_MAX)
        return;
    uint32_t address = gp0_cmd_source_addr & 0x1FFFFFFFu;
    if (address >= psx_ram_live_bytes() || (address & 3u) ||
        !ws_hud_anchor_lookup(ws_reveal_clear_tags, WS_HUD_ANCHOR_TABLE_SIZE,
                              address, gp0_cmd_buf, (uint32_t)gp0_words_needed,
                              (uint32_t)s_frame_count, NULL)) return;
    int base = 0;
    if (!ws_local_viewport_draw_target(&base)) {
        if (ws_local_viewport_cfg) return;
        if (!ws_display_viewport_draw_target(&base)) {
            if (!ws_is_fb_base(draw_area_left)) return;
            base = (int)draw_area_left;
        }
    }
    int bottom = y + h;
    if (y < (int)draw_area_top) y = (int)draw_area_top;
    if (bottom > (int)draw_area_bottom + 1) bottom = (int)draw_area_bottom + 1;
    if (bottom > y) gr_wide_clear_margins(base, y, bottom - y, 0, 3);
}

/* Stage-init already clears both synthetic margins once. Do not keep clearing
 * a guessed finite-map side here: MMX6's authored layers enter the reveal at
 * different times, so the side guess produced a moving black trim over valid
 * stage art. A stale reveal tile is safer than deleting submitted content. */
void gpu_ws_begin_linked_list(void) {
    gp0_ot_rank = 0xFFFFu;
}

void gpu_set_gp0_linked_list_node(uint32_t addr, uint32_t word_count) {
    (void)addr;
    if (word_count == 0) {
        gp0_ot_rank = gp0_ot_rank == 0xFFFFu ? 0u
                                             : (uint16_t)(gp0_ot_rank + 1u);
    }
}

void gpu_ws_end_linked_list(void) {
    gp0_ot_rank = 0xFFFFu;
}

/* The ordering table's front: its last GPU_OT_FRONT entries, where a game puts
 * what it draws over everything (BoF4: menus, a target's HP bar, the markers
 * by each character, at entries 1031 and 1033 of 1036); its world's sprites
 * are sorted by depth into the entries before them. s_ot_top: the linked list
 * being drawn's last entry (its empty nodes, counted by the prepass). */
enum { GPU_OT_FRONT = 6 };
static uint16_t s_ot_top = 0xFFFFu;
int gpu_ot_front_now(void) {
    return gp0_ot_rank != 0xFFFFu && s_ot_top != 0xFFFFu && s_ot_top >= 64u &&
           (uint32_t)gp0_ot_rank + GPU_OT_FRONT > s_ot_top;
}

void gpu_ws_restore_linked_list_rank(uint32_t rank) {
    gp0_ot_rank = rank == UINT32_MAX ? 0xFFFFu : (uint16_t)rank;
}


/* Horizontal display range (GP1(06h)) */
static uint32_t h_display_x1;
static uint32_t h_display_x2;

/* Vertical display range (GP1(07h)) */
static uint32_t v_display_y1;
static uint32_t v_display_y2;

/* GPUREAD latch (GP1(10h) get-info result, or VRAM read data) */
static uint32_t gpuread_latch;

/* C0 (VRAM→CPU) history: an always-on ring of the most recent reads
 * (it used to keep only the first 32 since boot, so gameplay readbacks were
 * invisible). Forward declaration for gpu_read_gpuread. */
#define C0_HISTORY_CAP_FWD 256
static struct C0HistEntry {
    uint16_t x, y, w, h;
    uint32_t func_addr, sp_val, s1_val;
    uint32_t first_words[4];
    int read_count;
    uint32_t frame;
} c0_history_fwd[C0_HISTORY_CAP_FWD];
static uint64_t c0_history_count_fwd = 0;   /* total reads since boot */
static int c0_capture_slot_fwd = -1;

/* Vblank presentation callback */
static gpu_vblank_cb vblank_callback;

/* Shaded quad vertex capture (Phase 4.5 debug) — forward declarations
 * so gpu_vblank_tick can reference sq_cap_armed. */
#define SQ_CAP_MAX 32
static GpuSqCapEntry sq_cap_buf[SQ_CAP_MAX];
static int sq_cap_count;
static int sq_cap_armed;

/* GPUSTAT poll counter: in v4, recompiled code runs as native C without
 * per-instruction stepping. The BIOS contains tight VSYNC wait loops that
 * poll GPUSTAT bit 31 (LCF) waiting for it to toggle. LCF only changes
 * via gpu_vblank_tick(), which normally fires from psx_check_interrupts()
 * at dispatch boundaries. A polling loop within a single recompiled
 * function would spin forever.
 *
 * Fix: count GPUSTAT reads and trigger a vblank when the BIOS has polled
 * enough times, approximating the real hardware timing where the field
 * flips every ~33,868 GPU clocks (one NTSC frame). */
#define GPUSTAT_POLL_VBLANK_THRESHOLD 1000
static uint32_t gpustat_poll_count;
uint64_t g_pollhack_vblank_count = 0;  /* instrumentation: poll-fallback VBlank IRQ count */

/* ---- Initialization ---- */

static void gpu_reset_state(int clear_vram) {
    ws_reset_scene_history();
    if (clear_vram) {
        memset(vram, 0, sizeof(vram));
        gpu_vram_dirty_mark_all();
    }
    gr_init(vram);

    /* Reset GP0 state machine (+ leftover cmd/xfer crumbs that survive in
     * gpu_snapshot and fork av digests on rematch). */
    gp0_state = GP0_IDLE;
    gp0_words_collected = 0;
    gp0_words_needed = 0;
    memset(gp0_cmd_buf, 0, sizeof(gp0_cmd_buf));
    gp0_next_source_addr = 0xFFFFFFFFu;
    gp0_cmd_source_addr = 0xFFFFFFFFu;
    ws_hud_anchor_clear(ws_hud_anchor_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    ws_hud_anchor_clear(ws_background_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    ws_background_tags_used = 0;
    ws_hud_anchor_clear(ws_reveal_clear_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    ws_hud_anchor_clear(ws_screen_mask_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    ws_radial_mask_clear(ws_radial_screen_mask_tags);
    ws_repeat_rect_tag_clear(ws_repeat_rect_tags);
    polyline_color = 0;
    polyline_prev_x = polyline_prev_y = 0;
    polyline_prev_c = 0;
    polyline_semi_trans = 0;
    polyline_has_prev = 0;
    vram_write_x = vram_write_y = 0;
    vram_write_w = vram_write_h = 0;
    vram_write_col = vram_write_row = 0;
    vram_write_remaining = 0;
    vram_read_active = 0;
    vram_read_x = vram_read_y = 0;
    vram_read_w = vram_read_h = 0;
    vram_read_col = vram_read_row = 0;

    /* Reset all state to power-on defaults */
    texpage_x = 0;
    texpage_y = 0;
    semi_transparency = 0;
    texpage_colors = 0;
    dither_enabled = 0;
    gr_set_dither(0);
    draw_to_display = 0;
    set_mask_bit = 0;
    check_mask_bit = 0;
    interlace_field = 0;
    reverse_flag = 0;
    texture_disable = 0;

    draw_area_left = 0;
    draw_area_top = 0;
    draw_area_right = 0;
    draw_area_bottom = 0;
    split_trace_reset(&split_trace_this);
    split_trace_reset(&split_trace_last);
    split_recent_left_age = 255;
    split_recent_right_age = 255;
    split_recent_display_w = 0;
    split_recent_display_h = 0;
    draw_offset_x = 0;
    draw_offset_y = 0;
    texture_window_value = 0;

    hres2 = 0;
    hres1 = 0;
    vres = 0;
    video_mode = 0;   /* GP1(00h) resets GP1(08h) to 0 = NTSC */
    (void)psx_video_timing_set_pal(0);
    display_depth = 0;
    vertical_interlace = 0;

    display_disabled = 1;  /* display is OFF after reset */
    irq1_flag = 0;
    dma_direction = 0;
    lcf = 0;

    display_area_x = 0;
    display_area_y = 0;
    h_display_x1 = 0x200;
    h_display_x2 = 0xC00;
    v_display_y1 = 0x010;
    v_display_y2 = 0x100;

    gpuread_latch = 0;
    gpustat_poll_count = 0;
    s_ws_fmv_frame_cache = 0xFFFFFFFFu;
    s_ws_fmv_cached = 0;
    ws_scene_hold_reset(&s_ws_scene_hold);
    s_d24_upload_x1 = 0;
    s_d24_present_hold = 0;
    s_d24_present_skip = 0;
    s_d24_prev_disp_h = 0;
}

void gpu_init(void) {
    gpu_reset_state(1);
    video_mode = s_power_on_video_mode;
    (void)psx_video_timing_set_pal((int)video_mode);
}

void gpu_set_power_on_video_mode(int pal) {
    s_power_on_video_mode = pal ? 1u : 0u;
    video_mode = s_power_on_video_mode;
    (void)psx_video_timing_set_pal((int)video_mode);
}

/* ---- GPUSTAT read (0x1F801814) ---- */

uint32_t gpu_read_gpustat(void) {
    /* Advance vblank when polled enough times from within a single function.
     * This handles BIOS VSYNC wait loops that poll LCF in tight loops.
     *
     * IMPORTANT: only update emulation state here (LCF + I_STAT). Do NOT
     * fire the vblank callback — that calls sdl_vblank_present, which
     * runs SDL_RenderPresent and SDL_Delay (Sleep). Entering Sleep from
     * an MMIO-read code path means recompiled MIPS code spends real wall
     * time inside Sleep — wrong context, hard to reason about, and
     * accumulates host stack frames inside the recompiled call tree.
     * The callback fires from the proper VBLANK trigger in
     * psx_check_interrupts (cycle-paced). */
    gpustat_poll_count++;
    if (gpustat_poll_count >= GPUSTAT_POLL_VBLANK_THRESHOLD) {
        gpustat_poll_count = 0;
        /* The recompiled BIOS + game carry per-block interrupt checks
         * (psx_check_interrupts_at) and per-block cycle charging, so tight
         * LCF/VSync poll loops DO advance guest cycles and let the ONE
         * cycle-paced VBlank authority (interrupts.c) fire on schedule. The old
         * "spin forever" premise that justified raising VBlank+LCF from a raw
         * GPUSTAT read-count is stale — and it injected ~38 fake VBlanks/s
         * (measured on Crash Bash), delivering ~96/s to the game instead of 60,
         * over-advancing the game's VSync frame counter and jittering animation
         * (character strobe). Firing is now OFF by default; PSX_POLLHACK_VBLANK=1
         * restores the legacy behavior for A/B. */
        static int poll_fire = -1;
        if (poll_fire < 0) {
            const char* e = getenv("PSX_POLLHACK_VBLANK");
            poll_fire = (e && e[0] == '1') ? 1 : 0;
        }
        if (poll_fire) {
            g_pollhack_vblank_count++;   /* count only when actually firing */
            lcf ^= 1;
            psx_irq_raise(0, 0); /* IRQ_VBLANK (GPUSTAT-poll fallback path) */
            /* DEQUEUE: VBlank fired via the GPUSTAT-poll fallback path (distinct
             * from the cycle-paced VBlank in psx_check_interrupts). */
            event_ring_record_aux(EV_DEQ, (uint8_t)SRC_VBLANK, 0xFFFFFFFFu);
        }
    }

    uint32_t stat = 0;

    stat |= (texpage_x & 0xF);
    stat |= (texpage_y & 1) << 4;
    stat |= (semi_transparency & 3) << 5;
    stat |= (texpage_colors & 3) << 7;
    stat |= (dither_enabled & 1) << 9;
    stat |= (draw_to_display & 1) << 10;
    stat |= (set_mask_bit & 1) << 11;
    stat |= (check_mask_bit & 1) << 12;
    stat |= (interlace_field & 1) << 13;
    stat |= (reverse_flag & 1) << 14;
    stat |= (texture_disable & 1) << 15;
    stat |= (hres2 & 1) << 16;
    stat |= (hres1 & 3) << 17;
    stat |= (vres & 1) << 19;
    stat |= (video_mode & 1) << 20;
    stat |= (display_depth & 1) << 21;
    stat |= (vertical_interlace & 1) << 22;
    stat |= (display_disabled & 1) << 23;
    stat |= (irq1_flag & 1) << 24;

    /* Bit 25: DMA request — depends on DMA direction.
     * Direction 0: always 0
     * Direction 1: FIFO not full (always 1 for now — we process instantly)
     * Direction 2: same as bit 28 (ready to receive DMA block)
     * Direction 3: same as bit 27 (ready to send VRAM to CPU)
     */
    switch (dma_direction) {
        case 0: break; /* bit 25 = 0 */
        case 1: stat |= (1u << 25); break;
        case 2: stat |= (1u << 25); break; /* mirrors ready-to-receive */
        case 3: stat |= (1u << 25); break; /* mirrors ready-to-send */
    }

    /* Bit 26: ready to receive cmd word — 1 when not busy */
    stat |= (1u << 26);

    /* Bit 27: ready to send VRAM to CPU — 1 when VRAM read is active */
    if (vram_read_active)
        stat |= (1u << 27);

    /* Bit 28: ready to receive DMA block — 1 when not busy */
    stat |= (1u << 28);

    /* Bits 29-30: DMA direction */
    stat |= (dma_direction & 3) << 29;

    /* Bit 31: LCF — drawing even/odd lines in interlace mode */
    stat |= (lcf & 1) << 31;

    return stat;
}

/* ---- GPUREAD (0x1F801810 read) ---- */

uint32_t gpu_read_gpuread(void) {
    if (!vram_read_active)
        return gpuread_latch;

    /* Read two 16-bit pixels from VRAM and pack into one 32-bit word.
     * Routed through the renderer facade: under the GL backend the GPU-side
     * framebuffer is authoritative and must be synced down before the CPU
     * array is read (no-op cost on the software backend). */
    uint32_t value = 0;
    for (int i = 0; i < 2; i++) {
        uint16_t rx = (vram_read_x + vram_read_col) % 1024;
        uint16_t ry = (vram_read_y + vram_read_row) % 512;
        value |= (uint32_t)gr_vram_read((int)rx, (int)ry) << (i * 16);

        if (++vram_read_col == vram_read_w) {
            vram_read_col = 0;
            if (++vram_read_row == vram_read_h) {
                /* Transfer complete */
                vram_read_active = 0;
                break;
            }
        }
    }

    /* Capture first words for C0 debug */
    if (c0_capture_slot_fwd >= 0 && c0_capture_slot_fwd < C0_HISTORY_CAP_FWD) {
        int rc = c0_history_fwd[c0_capture_slot_fwd].read_count++;
        if (rc < 4)
            c0_history_fwd[c0_capture_slot_fwd].first_words[rc] = value;
        if (!vram_read_active)
            c0_capture_slot_fwd = -1;  /* transfer complete */
    }

    gpuread_latch = value;
    return value;
}

/* ---- GP0 command helpers ---- */

/* Convert 24-bit RGB888 to 16-bit RGB555 (PS1 VRAM format) */
static uint16_t rgb888_to_rgb555(uint32_t color24) {
    uint32_t r = (color24 >>  0) & 0xFF;
    uint32_t g = (color24 >>  8) & 0xFF;
    uint32_t b = (color24 >> 16) & 0xFF;
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

/* i_stat extern declared earlier in this file (above gpu_read_gpustat). */

/* Netplay: present/finish_frame deferred from mid-psx_cyc_step to the next
 * psx_check_interrupts BB edge. MotK menu wait-loop (0x8006CDA0) was digesting
 * peers at different instr points (post-lw v0 vs post-slt v0=1) → cpu+ram fork
 * on idle sealed resim. Guest VBlank raise / LCF stay immediate. */
static int s_present_pending;
static int s_flushing_present;

void gpu_vblank_clear_deferred_present(void) {
    s_present_pending = 0;
    /* longjmp from flush_resume abandons the flush_present stack frame —
     * must drop the reentrancy guard or every later flush no-ops forever. */
    s_flushing_present = 0;
}

void gpu_vblank_arm_deferred_present(void) {
    /* Coalesce: at most one deferred present. Stacking (≥2) drained in one
     * flush as double finish_frame at the same guest cycle (MotK soak:
     * fin@N and fin@N+1 share dig/cyc → episode skew + clk/tim ±9). */
    if (s_present_pending < 1)
        s_present_pending = 1;
}

int gpu_vblank_present_pending(void) {
    return s_present_pending > 0;
}

void gpu_vblank_release_present_flush_guard(void) {
    s_flushing_present = 0;
}

void gpu_vblank_flush_present(void) {
    if (s_flushing_present || s_present_pending <= 0)
        return;
    /* Belt-and-suspenders with interrupts.c: never finish_frame inside the
     * exception handler (IEc-clear BB edges used to drain present_pending). */
    {
        extern int psx_get_in_exception(void);
        if (psx_get_in_exception())
            return;
    }
    /* Hold finish_frame while native memcard SIO is in flight. MotK needs
     * BB-edge commit for menu-wait determinism, but draining mid card
     * busy-wait wedges save/load on Ape Escape (empty starfield) and the
     * same class of titles. Keep s_present_pending; retry after card idle
     * (sio_hold_present_for_card has a stale escape). */
    {
        extern int psx_netplay_active(void);
        if (psx_netplay_active() && sio_hold_present_for_card())
            return;
    }
    /* MotK menu wait (0x8006CD54↔0x8006CDA0) and post-FMV overlay wait
     * (0x800768C8↔0x80076880): present ONLY at an explicit B edge. Never
     * present on an A edge (sticky B must not allow that). Non-wait edges
     * (FMV / cutover) must present even if sticky still names the wait loop. */
    {
        extern int psx_netplay_active(void);
        extern uint32_t psx_compiled_irq_resume_pc(void);
        extern uint32_t psx_last_irq_check_pc(void);
        extern uint32_t psx_netplay_rb_sticky_bb_pc(void);
        if (psx_netplay_active()) {
            const uint32_t wait_a = 0x8006CD54u;
            const uint32_t wait_b = 0x8006CDA0u;
            const uint32_t wait2_a = 0x800768C8u;
            const uint32_t wait2_b = 0x80076880u;
            uint32_t pc = psx_compiled_irq_resume_pc();
            uint32_t last = psx_last_irq_check_pc();
            uint32_t sticky = psx_netplay_rb_sticky_bb_pc();
            uint32_t edge = pc ? pc : last;
            if (edge == wait_a || edge == wait2_a)
                return;
            if (edge == wait_b || edge == wait2_b || edge != 0u) {
                /* B edge, or non-wait (FMV/cutover) — present; ignore sticky */
            } else if (sticky == wait_a || sticky == wait2_a) {
                return; /* latch cleared, sticky A — defer */
            }
            /* sticky B or unrelated/0 — present */
        }
    }
    s_flushing_present = 1;
    while (s_present_pending > 0) {
        s_present_pending--;
        if (vblank_callback)
            vblank_callback();
    }
    s_flushing_present = 0;
}

void gpu_vblank_tick(void) {
    lcf ^= 1;
    gpu_timeline_note(GTL_VBLANK, lcf, 0);
    /* GPUSTAT.13 (interlace FIELD): on real hardware this alternates per
     * field while GP1(08h) vertical interlace is on, in antiphase with the
     * even/odd-lines bit 31 during active display. Modeled here at vblank
     * granularity: field = !LCF while interlaced, pinned to the legacy 0 in
     * progressive (GP1(08h) clears it) so titles see identical GPUSTAT.
     * First consumer
     * is OpenBIOS's shell waitVSync, which polls for the alternating
     * (bit31,bit13) = (1,0)/(0,1) pattern on real hardware — under
     * PCSX-Redux it never runs this path (pcsx_present() short-circuits to
     * the vblank-IRQ wait, and Redux holds bit13 constant at 1), so this
     * poll first became reachable in this runtime. */
    if (vertical_interlace)
        interlace_field = (lcf ^ 1) & 1;
    s_d24_present_skip = s_d24_present_hold > 0;
    if (s_d24_present_hold > 0)
        s_d24_present_hold--;
    /* snapshot per-frame draw-offset-Y range for the strobe instrumentation */
    if (g_doff_cnt_this) {
        g_doff_min_last = g_doff_min_this;
        g_doff_max_last = g_doff_max_this;
        g_doff_cnt_last = g_doff_cnt_this;
    }
    g_doff_min_this = 0x7fffffff; g_doff_max_this = -0x7fffffff; g_doff_cnt_this = 0;
    split_trace_last = split_trace_this;
    if (split_trace_this.display_w != 0 &&
        (split_recent_display_w != split_trace_this.display_w ||
         split_recent_display_h != split_trace_this.display_h)) {
        split_recent_left_age = 255;
        split_recent_right_age = 255;
        split_recent_display_w = split_trace_this.display_w;
        split_recent_display_h = split_trace_this.display_h;
    }
    if (split_trace_this.left_seen) {
        split_recent_left_age = 0;
    } else if (split_recent_left_age < 255) {
        split_recent_left_age++;
    }
    if (split_trace_this.right_seen) {
        split_recent_right_age = 0;
    } else if (split_recent_right_age < 255) {
        split_recent_right_age++;
    }
    split_trace_reset(&split_trace_this);
    gpustat_poll_count = 0;
    /* Trusted package-selected plugins run on guest VBlank, independent of
     * host presentation, pacing, turbo, or skipped frames. */
    shadow_vblank();   /* [video.shadow]: before the plugins, which may override */
    mod_runtime_on_vblank();
    psx_irq_raise(0, 0); /* IRQ_VBLANK (gpu_vblank_tick) */
    if (!vblank_callback)
        return;
    {
        extern int psx_netplay_active(void);
        /* Offline selfcheck keeps immediate present: BB-edge defer +
         * post-IRQ flush reintroduces clk/tim/csv phase skew between warm
         * resim peers (selfcheck soak: many FAILs with d_cyc≠0). Netplay
         * defers — both peers share the same present contract from boot. */
        if (psx_netplay_active()) {
            /* Coalesce to one deferred present (see arm_deferred_present). */
            if (s_present_pending < 1)
                s_present_pending = 1;
        } else {
            vblank_callback();
        }
    }
}

const uint16_t* gpu_get_vram(void) {
    return vram;
}

/* Depth24: note/query/reset the CPU→VRAM upload span tracked above. Used to
 * hide trailing RGB columns when a movie blit doesn't fill the full CRTC
 * width — MotK's Star Wars crawl leaves ~8px of stale VRAM on the right.
 * Only FB-class A0s (w >= 256 halfwords) grow the span; texture uploads must
 * not collapse it. During present-hold, ignore updates entirely. */
static void depth24_note_upload(uint32_t x, uint32_t w) {
    if (!(display_depth & 1u) || w < 256u) return;
    if (s_d24_present_hold > 0) return;
    uint32_t x1 = x + w;
    if (x1 > 1024u) x1 = 1024u;
    if (x1 > s_d24_upload_x1) s_d24_upload_x1 = x1;
}

uint32_t gpu_depth24_rgb_limit(uint32_t display_x, uint32_t crtc_w) {
    if (!(display_depth & 1u) || crtc_w == 0u)
        return crtc_w;
    /* No uploads yet → treat as uncovered (present blanks until first blit). */
    if (s_d24_upload_x1 == 0u)
        return 0u;
    uint32_t dx = display_x & 1023u;
    if (s_d24_upload_x1 <= dx) return 0u;
    uint32_t hw = s_d24_upload_x1 - dx;
    uint32_t rgb = (hw * 2u) / 3u;
    if (rgb == 0u) return 0u;
    if (rgb >= crtc_w) return crtc_w;
    return rgb;
}

void gpu_depth24_upload_span_reset(void) {
    s_d24_upload_x1 = 0;
}

int gpu_depth24_present_hold_tick(void) {
    return s_d24_present_skip;
}

void gpu_depth24_on_savestate_loaded(void) {
    /* Hold skips Swap — after restore we want the restored VRAM visible now.
     * Upload span / prev_h were restored from the GPU snap. */
    s_d24_present_hold = 0;
    s_d24_present_skip = 0;
}

/* ---- Present-time screen-colour LUT (verified-enhancement, opt-in) -------
 *
 * PRESENT-TIME ONLY. This sits on the 15-bit-scanout -> RGB888 conversion that
 * feeds the SDL/GL present path. It never touches VRAM and never runs on the
 * depth24 (FMV) scanout (see gpu_display_pixel_rgb). It defaults to SCREEN_RAW
 * (the original exact 5->3-replicated expansion below), so with the feature off
 * the conversion — and therefore every hashed/oracle-diffed frame — is
 * byte-identical to upstream. Opt in via PSX_SCREEN={crt,composite,trinitron};
 * any other value (or unset) keeps the raw path.
 *
 * Built lazily on first use because env is read once and the GPU has no
 * settings plumb yet. The raw fast-path below is preserved verbatim so the
 * default never even consults the LUT. */
static ColorLut* s_screen_lut = NULL;
static int       s_screen_lut_init = 0;
static int       s_screen_kind_cfg = SCREEN_RAW;  /* config/launcher-set; env overrides */

void gpu_set_screen_kind(int kind) {
    if (kind < SCREEN_RAW || kind > SCREEN_TRINITRON) kind = SCREEN_RAW;
    if (kind == s_screen_kind_cfg) return;
    s_screen_kind_cfg = kind;
    s_screen_lut_init = 0;  /* rebuild on next scanout */
}

static void screen_lut_ensure(void) {
    if (s_screen_lut_init) return;
    s_screen_lut_init = 1;
    if (s_screen_lut) { color_lut_destroy(s_screen_lut); s_screen_lut = NULL; }
    /* Precedence: PSX_SCREEN env (debug override) wins if set+valid; otherwise
     * the config/launcher value. Default (no env, raw config) = passthrough. */
    ScreenKind kind = (ScreenKind)s_screen_kind_cfg;
    const char* name = getenv("PSX_SCREEN");
    ScreenKind envk;
    if (name && screen_kind_from_name(name, &envk)) kind = envk;
    if (kind == SCREEN_RAW) {
        s_screen_lut = NULL;  /* raw fast-path; passthrough */
        return;
    }
    ColorSettings s;
    s.screen = kind;
    s.darken = -1.0;          /* per-screen default */
    s.target = DISPLAY_SRGB;
    s_screen_lut = color_lut_create(&s);
    if (s_screen_lut && color_lut_is_passthrough(s_screen_lut)) {
        color_lut_destroy(s_screen_lut);
        s_screen_lut = NULL;
    }
}

static void gpu_rgb555_to_rgb888(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
    screen_lut_ensure();
    if (s_screen_lut) {
        color_lut_map555(s_screen_lut, c, r, g, b);
        return;
    }
    /* Default raw path — byte-identical to upstream. */
    *r = (uint8_t)((c & 0x1Fu) << 3);
    *g = (uint8_t)(((c >> 5) & 0x1Fu) << 3);
    *b = (uint8_t)(((c >> 10) & 0x1Fu) << 3);
}

void gpu_display_pixel_rgb_in(const uint16_t* image, const GpuDisplayInfo* di,
                              uint32_t x, uint32_t y,
                              uint8_t* r, uint8_t* g, uint8_t* b) {
    if (di->depth24) {
        uint32_t byte_x = ((di->display_x & 1023u) * 2u) + x * 3u;
        uint32_t vy = (di->display_y + y) & 511u;
        /* No horizontal wrap for 24-bit DAC scanout: bytes past the 2048-byte
         * VRAM row are blank on hardware / accurate emulators, not sheared
         * from X=0. Wrapping here produced a flickering right-edge strip. */
        if (byte_x + 2u >= 2048u) {
            *r = *g = *b = 0;
            return;
        }
        const uint16_t* row = image + (size_t)vy * 1024u;
        uint8_t px[3];
        for (uint32_t i = 0; i < 3u; i++) {
            uint16_t hw = row[((byte_x + i) >> 1) & 1023u];
            px[i] = ((byte_x + i) & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
        }
        *r = px[0]; *g = px[1]; *b = px[2];
        return;
    }

    uint32_t vx = (di->display_x + x) & 1023u;
    uint32_t vy = (di->display_y + y) & 511u;
    gpu_rgb555_to_rgb888(image[vy * 1024u + vx], r, g, b);
}

void gpu_display_pixel_rgb(const GpuDisplayInfo* di, uint32_t x, uint32_t y,
                           uint8_t* r, uint8_t* g, uint8_t* b) {
    gpu_display_pixel_rgb_in(vram, di, x, y, r, g, b);
}

uint32_t gpu_display_pixel_argb(const GpuDisplayInfo* di, uint32_t x, uint32_t y) {
    uint8_t r, g, b;
    gpu_display_pixel_rgb(di, x, y, &r, &g, &b);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Batch depth24 (FMV) scanline → ARGB. Same semantics as calling
 * gpu_display_pixel_argb(di, x, y) for x in [0, count) (byte-identical
 * output, including the black-fill past the 2048-byte VRAM row), but hoists
 * the per-row invariants (vy, base_byte_x, the "how many pixels are past the
 * row edge" test) out of the per-pixel path instead of recomputing them
 * `count` times and paying three byte extractions per pixel. The
 * per-pixel path was the dominant present-side cost of FMV frames (3x the
 * VRAM touches of the 16-bit path, done as a function-call chain instead of
 * a straight-line loop). Still scalar C — no host-endianness assumptions,
 * same byte-order shifts as gpu_display_pixel_rgb_in. */
void gpu_depth24_present_row(const GpuDisplayInfo* di, uint32_t y, uint32_t* out,
                             uint32_t count) {
    uint32_t vy = (di->display_y + y) & 511u;
    uint32_t base_byte_x = (di->display_x & 1023u) * 2u;
    const uint16_t* row = vram + (size_t)vy * 1024u;
    uint32_t valid = 0u;
    uint32_t x;

    if (base_byte_x <= 2045u)
        valid = (2045u - base_byte_x) / 3u + 1u;
    if (valid > count)
        valid = count;

    for (x = 0; x < valid; x++) {
        uint32_t byte_x = base_byte_x + x * 3u;
        uint32_t bx1 = byte_x + 1u;
        uint32_t bx2 = byte_x + 2u;
        uint32_t hw0 = row[byte_x >> 1];
        uint32_t hw1 = row[bx1 >> 1];
        uint32_t hw2 = row[bx2 >> 1];
        uint8_t r = (byte_x & 1u) ? (uint8_t)(hw0 >> 8) : (uint8_t)hw0;
        uint8_t g = (bx1 & 1u) ? (uint8_t)(hw1 >> 8) : (uint8_t)hw1;
        uint8_t b = (bx2 & 1u) ? (uint8_t)(hw2 >> 8) : (uint8_t)hw2;
        out[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    for (; x < count; x++)
        out[x] = 0xFF000000u;
}

/* Depth24 present staging, shared by every CPU present path: compose the
 * GP1(07h) intersection with active video into the PAL/NTSC canvas (rows
 * off-screen are clipping, not a shift), then black-fill the trailing RGB
 * columns no CPU->VRAM upload has covered since the last span reset, so stale
 * VRAM from a prior movie never shows past the new blit. With no span yet
 * (lim == 0) only the last `margin` columns are blanked: blanking [0..w) left
 * MotK's FMV permanently black after the GP1(07h) hold reset the span.
 * Returns the source rows staged. */
uint32_t gpu_depth24_stage_scanout(const GpuDisplayInfo *di, uint32_t *buf,
                                   uint32_t w, uint32_t margin) {
    if (!di || !buf || w == 0u || di->screen_height == 0u)
        return 0u;
    const size_t count = (size_t)w * di->screen_height;
    for (size_t i = 0; i < count; i++)
        buf[i] = 0xFF000000u;
    if (di->screen_origin_y >= di->screen_height)
        return 0u;
    uint32_t rows = di->height;
    if (rows > di->screen_height - di->screen_origin_y)
        rows = di->screen_height - di->screen_origin_y;
    uint32_t *source = buf + (size_t)di->screen_origin_y * w;
    for (uint32_t y = 0; y < rows; y++)
        gpu_depth24_present_row(di, di->screen_source_skip_y + y,
                                source + (size_t)y * w, w);
    if (rows == 0u || w < 8u || margin == 0u || margin >= w)
        return rows;
    uint32_t start = w - margin;
    uint32_t lim = gpu_depth24_rgb_limit(di->display_x, w);
    if (lim > 0u && lim < w && lim < start)
        start = lim;
    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = start; x < w; x++)
            source[(size_t)y * w + x] = 0xFF000000u;
    return rows;
}

int gpu_display_is_depth24(void) {
    return (int)(display_depth & 1u);
}

static void gpu_compute_display_info(GpuDisplayInfo* out);

/* The display info is a pure function of these GP1 registers; the widescreen
 * code asks for it per primitive. Each thread keeps the last answer and its
 * inputs (the render thread asks too). */
void gpu_get_display_info(GpuDisplayInfo* out) {
    static _Thread_local GpuDisplayInfo s_memo;
    static _Thread_local uint32_t s_key[12];
    static _Thread_local int s_have;
    const uint32_t key[12] = {
        display_area_x, display_area_y, display_depth, display_disabled,
        hres1, hres2, h_display_x1, h_display_x2,
        video_mode, v_display_y1, v_display_y2, vres,
    };
    if (s_have && memcmp(key, s_key, sizeof(key)) == 0) {
        *out = s_memo;
        return;
    }
    gpu_compute_display_info(out);
    memcpy(s_key, key, sizeof(key));
    s_memo = *out;
    s_have = 1;
}

static void gpu_compute_display_info(GpuDisplayInfo* out) {
    out->display_x = display_area_x;
    out->display_y = display_area_y;
    out->depth24   = (int)(display_depth & 1u);
    out->disabled  = (int)display_disabled;

    /* Dot-clock divider from GP1(08h) hres (psx-spx / DuckStation). */
    uint32_t cycles;
    uint32_t mode_w;
    if (hres2) {
        cycles = 7;
        mode_w = 368;
    } else {
        static const uint32_t cyc_lut[4] = { 10, 8, 5, 4 };
        static const uint32_t hres_lut[4] = { 256, 320, 512, 640 };
        cycles = cyc_lut[hres1 & 3];
        mode_w = hres_lut[hres1 & 3];
    }

    /* Visible pixel width from GP1(06h): (((X2-X1)/cycles)+2) & ~3 (psx-spx).
     * Fall back to the mode width when the range is unset/degenerate. */
    uint32_t dots = (h_display_x2 > h_display_x1) ? (h_display_x2 - h_display_x1) : 0;
    uint32_t w = mode_w;
    if (dots > 0u && cycles > 0u) {
        w = ((dots / cycles) + 2u) & ~3u;
        if (w == 0u) w = 4u;
    }

    /* DuckStation GetFullDisplayResolution: clamp Y1/Y2 to the broadcast
     * active region before taking the difference. Unclamped Y2 past the
     * active end (common overscan programming) includes a flickering junk
     * line at the bottom of present that DuckStation crops away. */
    PsxDisplayVerticalLayout vertical = psx_display_vertical_layout(
        video_mode != 0, v_display_y1, v_display_y2);
    uint32_t h = psx_display_source_height(vertical, 240u);
    uint32_t screen_h = vertical.range_set ? vertical.canvas_height : h;
    uint32_t screen_origin_y = vertical.valid ? vertical.canvas_origin_y : 0u;
    uint32_t screen_source_skip_y = vertical.valid ? vertical.source_skip_y : 0u;
    if (vres) {
        h = psx_display_interlaced_rows(h, 1);
        screen_h = psx_display_interlaced_rows(screen_h, 1);
        screen_origin_y = psx_display_interlaced_rows(screen_origin_y, 1);
        screen_source_skip_y = psx_display_interlaced_rows(
            screen_source_skip_y, 1);
    }

    /* 24-bit scanout uses the same CRTC pixel width as 15-bit (DuckStation /
     * Beetle: coordinates stay 16-bit-based; W RGB occupies W*3/2 halfwords).
     * MotK FMV: GP1(06h) yields 512; the logo is centered in that RGB line.
     * A blanket (W*2)/3 (512→341) left-shifts the frame and clips the right
     * of the video — do not reintroduce it. Right-edge junk is a separate
     * present/filter issue, not a reason to shrink CRTC width. */

    /* Clamp to sane maximums */
    if (w > 640) w = 640;
    if (out->depth24) {
        if (screen_h > PSX_DISPLAY_PRESENT_MAX_HEIGHT)
            screen_h = PSX_DISPLAY_PRESENT_MAX_HEIGHT;
        if (screen_origin_y > screen_h)
            screen_origin_y = screen_h;
        h = psx_display_clip_source_height(
            1, h, screen_h, screen_origin_y);
    } else {
        /* The active canvas is a depth24 staging contract. Keep direct 15-bit
         * source rectangles independent from its origin and 576-row limit. */
        if (h > 512u) h = 512u;
        screen_h = h;
        screen_origin_y = 0u;
        screen_source_skip_y = 0u;
    }

    out->width  = w;
    out->height = h;
    out->screen_height = screen_h;
    out->screen_origin_y = screen_origin_y;
    out->screen_source_skip_y = screen_source_skip_y;
    out->screen_offset_y = vertical.valid ? vertical.offset_y : 0;
    if (vres) out->screen_offset_y *= 2;
}

/* Debug accessors for GP1 display-range / mode (TCP gpu_state). */
void gpu_get_crtc_debug(uint32_t *x1, uint32_t *x2, uint32_t *y1, uint32_t *y2,
                        uint32_t *hres1_out, uint32_t *hres2_out) {
    if (x1) *x1 = h_display_x1;
    if (x2) *x2 = h_display_x2;
    if (y1) *y1 = v_display_y1;
    if (y2) *y2 = v_display_y2;
    if (hres1_out) *hres1_out = hres1;
    if (hres2_out) *hres2_out = hres2;
}

void gpu_set_vblank_callback(gpu_vblank_cb cb) {
    vblank_callback = cb;
}

/* Sign-extend an N-bit value to int32_t */
static int32_t sign_extend(uint32_t val, int bits) {
    uint32_t sign_bit = 1u << (bits - 1);
    return (int32_t)((val ^ sign_bit) - sign_bit);
}

/* ---- Polygon rasterizer ---- */

/* Parse a vertex position word: signed 11-bit X and Y */
static void parse_vertex(uint32_t word, int32_t* x, int32_t* y) {
    int extended = ws_bg2d_host_packet() || (g_bg2d_host_size &&
        ws_native_wide_active() && ws_tagged_world_primitive());
    *x = extended ? (int16_t)word : sign_extend(word & 0x7FFu, 11);
    *y = sign_extend((word >> 16) & 0x7FFu, 11);
}

extern int gte_geometry_correction_enabled(void);
static int s_texture_correction_enabled = 0;
static int s_native_wide_projection_correction = 0;
static uint64_t s_native_wide_projection_vertices = 0;
extern int gte_precision_load_word(uint32_t addr, uint32_t packed,
                                   int32_t *x16, int32_t *y16, uint16_t *z);

/* Arm the PGXP dataflow transport for any consumer. Exact NCLIP is an internal
 * sign source only; this does not enable geometry or texture correction. */
void gpu_pgxp_rederive_enable(void) {
    pgxp_set_enabled(s_texture_correction_enabled ||
                     gte_geometry_correction_enabled() ||
                     ws_precise_nclip_cfg ||
                     s_native_wide_projection_correction);
}

void psx_mod_set_native_wide_projection_correction(int enabled) {
    s_native_wide_projection_correction = enabled ? 1 : 0;
    s_native_wide_projection_vertices = 0;
    /* Signed camera-space provenance is required for correct native-wide
     * faces, including corners behind the camera. Arm it with the mode. */
    pgxp_set_projection_tracking(s_native_wide_projection_correction);
    gpu_pgxp_rederive_enable();
}

int gpu_ws_native_wide_projection_correction(uint64_t *vertices) {
    if (vertices) *vertices = s_native_wide_projection_vertices;
    return s_native_wide_projection_correction;
}

static int gpu_ws_projective_enabled(void) {
    return pgxp_projection_tracking() && s_native_wide_projection_correction && ws_native_wide_active() &&
        ws_nw_extra() > 0 && gr_backend() == GR_BACKEND_OPENGL &&
        gl_renderer_projective_supported();
}
void psx_mod_set_native_wide_near_clip(int enabled) {
    /* Compatibility entry point: clipping follows projection correction.
     * A separate toggle must not disable this mode's correctness fix. */
    (void)enabled;
}

/* The PS1 clamps each projected X independently. At a wider FOV that can
 * bend an otherwise planar quad, even while both clamped vertices remain
 * outside the viewport. Recover only a proven saturated projection. An
 * ordinary CPU coordinate, a modified packet, and a missing depth stay stock.
 * The +/-4096 transport endpoints themselves are clamped, so reject them. */
static int native_wide_projection_x(uint32_t addr, uint32_t word,
                                    int32_t raw_x, int32_t raw_y,
                                    int32_t *x16) {
    if (!s_native_wide_projection_correction || ws_nw_extra() <= 0 ||
        (raw_x != -1024 && raw_x != 1023) || addr == UINT32_MAX)
        return 0;
    int32_t px, py;
    uint16_t z;
    if (!gte_precision_load_word(addr, word, &px, &py, &z) || !z ||
        (py >> 16) != raw_y || px <= -4096 * 65536 ||
        px >= 4096 * 65536 - 1)
        return 0;
    if ((raw_x == -1024 && (px >> 16) >= -1024) ||
        (raw_x == 1023 && (px >> 16) <= 1023))
        return 0;
    *x16 = px;
    s_native_wide_projection_vertices++;
    return 1;
}

/* Original, unsubdivided world faces can exceed the PS1 size limit after the
 * wider view admits them. Proven projected triangles use normal GL clipping,
 * with the SAME geometry in canonical and wide passes: margin-only recovery
 * splits a face at the presentation center-copy edge. Missing/stale provenance,
 * depth behind the camera, saturated Y and UI retain the hardware reject. */
static int gpu_triangle_rejected(const int32_t* x, const int32_t* y,
                                 int a, int b, int c) {
    if (!psx_gpu_triangle_oversize(x, y, a, b, c)) return 0;
    if (!s_native_wide_projection_correction || !ws_native_wide_active() ||
        ws_nw_extra() <= 0 || gr_backend() != GR_BACKEND_OPENGL ||
        gp0_cmd_source_addr == UINT32_MAX) return 1;
    const uint32_t op = gp0_cmd_buf[0] >> 24;
    const unsigned stride = 1u + ((op & 4u) != 0) + ((op & 16u) != 0);
    const int vertices[3] = {a, b, c};
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned index = 1u + stride * (unsigned)vertices[i];
        const uint32_t word = gp0_cmd_buf[index];
        int32_t px, py, rx, ry;
        uint16_t z;
        parse_vertex(word, &rx, &ry);
        if (!gte_precision_load_word(gp0_cmd_source_addr + 4u * index,
                                      word, &px, &py, &z) || !z ||
            (py >> 16) != ry || ry == -1024 || ry == 1023 ||
            px <= -4096 * 65536 ||
            px >= 4096 * 65536 - 1) return 1;
        const int exact_x = px >> 16;
        if (rx == -1024 ? exact_x > -1024 :
            rx == 1023 ? exact_x < 1023 : exact_x != rx) return 1;
    }
    return 0;
}

void gpu_texture_correction_set(int enabled) {
    s_texture_correction_enabled = enabled ? 1 : 0;
    gpu_pgxp_rederive_enable();
}

int gpu_texture_correction_enabled(void) {
    return s_texture_correction_enabled;
}

uint32_t gpu_texture_correction_hits(void) {
    return sw_perspective_triangle_count();
}

/* Per-vertex precise positions (PGXP, docs/ENHANCEMENTS.md G1). Each of the three
 * packet words is resolved independently: the address-keyed dataflow shadow
 * first (validated against the actual word — exact provenance, survives
 * ordering-table reordering), the ambiguity-gated position cache second, the
 * parsed integers last. A vertex left native is exactly where the
 * uncorrected pipeline put it. Where it meets a neighbour's precise copy of
 * the same vertex, their shared edge disagrees by that vertex's precise-minus-
 * native offset: under a fraction of a pixel on the IR path, but up to the
 * PGXP_PPP_AGREE_* window (several pixels) under preserve-projection — that
 * is the G1.1 crack the tri_mixed counter measures. The integer delta folds
 * in draw offsets and any widescreen adjustment already applied by the
 * caller. */
static void prepare_precise_triangle(int i0, int i1, int i2,
                                     const int32_t vx[3], const int32_t vy[3]) {
    if (gr_backend() == GR_BACKEND_OPENGL) {
        const int indices[3] = {i0,i1,i2};
        int32_t raw_x[3], raw_y[3];
        for (unsigned i=0; i<3; ++i)
            parse_vertex(gp0_cmd_buf[indices[i]], &raw_x[i], &raw_y[i]);
        gl_renderer_note_wide_triangle_recovery(s_native_wide_projection_correction &&
            ws_native_wide_active() &&
            psx_gpu_triangle_oversize(raw_x,raw_y,0,1,2));
    }
    if (gr_backend() == GR_BACKEND_OPENGL &&
        gl_renderer_frame_generation() != gte_fg_source_enabled())
        gte_fg_source_set(gl_renderer_frame_generation());   /* from the next projection */
    if (gr_backend() == GR_BACKEND_OPENGL && gte_fg_source_enabled()) {
        /* Frame generation re-projects vertices from their camera-space source. */
        uint32_t id[3] = { 0, 0, 0 };
        int32_t pc[9] = { 0 }, hd[3] = { 0, 0, 0 };
        const int ix[3] = { i0, i1, i2 };
        GteFgSrc c[3][2];
        int nc[3];
        for (int k = 0; k < 3; k++) nc[k] = gte_fg_source_lookup(gp0_cmd_buf[ix[k]], c[k]);
        /* Two points on one pixel: the one projected by the same function as
         * a vertex with one source, else the one nearest its depth. */
        for (int k = 0; k < 3; k++) {
            int pick = nc[k] == 1 ? 0 : -1;
            if (nc[k] == 2) {
                for (int m = 0; m < 3 && pick < 0; m++)
                    if (m != k && nc[m] == 1)
                        for (int q = 0; q < 2; q++)
                            if (c[k][q].ra == c[m][0].ra) { pick = q; break; }
                if (pick < 0)
                    for (int m = 0; m < 3 && pick < 0; m++)
                        if (m != k && nc[m] == 1) {
                            const int32_t z = c[m][0].p[2];
                            pick = abs(c[k][0].p[2] - z) <= abs(c[k][1].p[2] - z) ? 0 : 1;
                        }
            }
            if (pick < 0) continue;
            id[k] = c[k][pick].id; hd[k] = c[k][pick].h;
            for (int q = 0; q < 3; q++) pc[3 * k + q] = c[k][pick].p[q];
        }
        gl_renderer_fg_source(id, pc, hd, vx, vy);
    }
    gr_set_perspective_triangle(0, 0.0f, 0.0f, 0.0f);
    /* PGXP depth (G1.14) only reaches the renderer while a PGXP renderer
     * feature is on: with them off no depth command is sent per triangle. */
    const int want_depth = gl_renderer_pgxp_render_wanted();
    if (want_depth) gr_set_depth_triangle(0, 0.0f, 0.0f, 0.0f);
    const int geometry = gte_geometry_correction_enabled();
    if (!geometry && !s_native_wide_projection_correction) {
        gr_set_precise_triangle(0, 0,0, 0,0, 0,0);
        return;
    }
    const int idx[3] = { i0, i1, i2 };
    int32_t fx[3], fy[3];
    uint16_t vz[3] = {0, 0, 0};
    int any_precise = 0, n_precise = 0;
    PGXPTriRecord rec;
    memset(&rec, 0, sizeof rec);
    for (int i = 0; i < 3; i++) {
        uint32_t word = gp0_cmd_buf[idx[i]];
        int32_t raw_x, raw_y;
        parse_vertex(word, &raw_x, &raw_y);
        uint32_t addr = (gp0_cmd_source_addr == 0xFFFFFFFFu)
                            ? 0xFFFFFFFFu
                            : gp0_cmd_source_addr + (uint32_t)idx[i] * 4u;
        int32_t px, py;
        uint16_t sz;
        int src = PGXP_SRC_NATIVE;
        px = raw_x * 65536;
        py = raw_y * 65536;
        sz = 0;
        if (geometry)
            src = pgxp_get_precise_vertex(addr, word, raw_x, raw_y, &px, &py, &sz);
        if (src != PGXP_SRC_NATIVE) {
            any_precise = 1;
            n_precise++;
        }
        /* PGXP depth (G1.14): only a value-validated dataflow shadow carries
         * SZ, also when the seam clamp kept the native position. */
        vz[i] = sz;
        /* Native-wide edge recovery is not PGXP: it does not count in n_precise. */
        if (native_wide_projection_x(addr, word, raw_x, raw_y, &px))
            any_precise = 1;
        fx[i] = (int32_t)((int64_t)px + (int64_t)(vx[i] - raw_x) * 65536);
        fy[i] = (int32_t)((int64_t)py + (int64_t)(vy[i] - raw_y) * 65536);
        rec.src[i] = (uint8_t)src;
        rec.vidx[i] = (uint8_t)idx[i];
        rec.raw_x[i] = (int16_t)raw_x;
        rec.raw_y[i] = (int16_t)raw_y;
        rec.word[i] = word;
        rec.x16[i] = fx[i];
        rec.y16[i] = fy[i];
        rec.z[i] = vz[i];
    }
    if (geometry) {
        pgxp_note_triangle(n_precise);   /* G1.1 crack exposure: mixed share */
        rec.frame = (uint32_t)s_frame_count;
        rec.src_addr = gp0_cmd_source_addr;
        rec.op = (uint8_t)(gp0_cmd_buf[0] >> 24);
        pgxp_note_triangle_detail(&rec);
    }
    const int depth = want_depth && vz[0] && vz[1] && vz[2];
    if (!any_precise && !depth) {
        gr_set_precise_triangle(0, 0,0, 0,0, 0,0);
        return;
    }
    /* A triangle with validated depth on every vertex is proven projected
     * geometry even when the seam clamp left all three at their integers:
     * fx/fy then are exactly those integers (plus offsets), so the position
     * is unchanged and only the depth test is added. */
    gr_set_precise_triangle(1, fx[0],fy[0], fx[1],fy[1], fx[2],fy[2]);
    if (depth)
        gr_set_depth_triangle(1, (float)vz[0], (float)vz[1], (float)vz[2]);
}

/* A textured quad whose four packet vertices form an axis-aligned rectangle
 * (in integer screen space and in UV) is drawn by the 2D rectangle shortcut,
 * at the native integer position and without perspective. That is right for a
 * CPU-built sprite, but a GTE-projected world quad can land on such a
 * rectangle too (a facade seen straight on), and its neighbours are drawn
 * precise, so the shortcut would open a sub-pixel seam along every shared
 * edge (docs/ENHANCEMENTS.md G1.11). Returns how many of the four vertices
 * carry a dataflow-precise position (0 while PGXP is not correcting). Only a
 * quad with all four precise leaves the shortcut and is drawn as its two
 * triangles, like any other world quad: one with some corners precise (say a
 * CPU-built sprite with one corner copied from SXY) would otherwise become
 * mixed triangles, so it keeps the shortcut and counts as rect_partial. */
static int textured_quad_precise_vertices(const int idx[4]) {
    if (!gte_geometry_correction_enabled() && !s_texture_correction_enabled)
        return 0;
    if (gp0_cmd_source_addr == 0xFFFFFFFFu) return 0;
    int n = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t word = gp0_cmd_buf[idx[i]];
        int32_t raw_x, raw_y;
        parse_vertex(word, &raw_x, &raw_y);
        if (pgxp_probe_precise_vertex(gp0_cmd_source_addr + (uint32_t)idx[i] * 4u,
                                      word, raw_x, raw_y) == PGXP_SRC_DATAFLOW)
            n++;
    }
    return n;
}

/* Arming rate for perspective-correct UVs, per condition.
 *
 * perspective_triangles alone cannot answer "how much texture warp is left",
 * because it has no denominator: comparing it to gp0_draw mixes in untextured
 * mono and gouraud primitives that are correctly never armed, and comparing it
 * to a vertex-lookup count divided by three is the same error. `attempts`
 * counts exactly the textured triangles that reach this predicate, so
 * armed/attempts IS the perspective coverage, and the three reject counters
 * say which condition is spending it. Diagnostic only. */
static struct {
    uint64_t attempts;      /* textured triangles submitted                 */
    uint64_t armed;         /* got perspective-correct UVs                  */
    uint64_t no_correction; /* texture correction off                       */
    uint64_t no_source;     /* CPU-built primitive, no packet address       */
    uint64_t no_depth;      /* a vertex had no recorded Z, or Z == 0        */
} s_texcorr;

void gpu_texture_correction_stats(uint64_t *attempts, uint64_t *armed,
                                  uint64_t *no_correction,
                                  uint64_t *no_source, uint64_t *no_depth) {
    if (attempts)      *attempts      = s_texcorr.attempts;
    if (armed)         *armed         = s_texcorr.armed;
    if (no_correction) *no_correction = s_texcorr.no_correction;
    if (no_source)     *no_source     = s_texcorr.no_source;
    if (no_depth)      *no_depth      = s_texcorr.no_depth;
}

/* Enable perspective UVs only when every position word came from an exact
 * SWC2 projection store at that same DMA packet address. This preserves the
 * association through ordering-table reordering and rejects CPU-built UI. */
static void prepare_texture_triangle(int i0, int i1, int i2) {
    gr_set_perspective_triangle(0, 0.0f, 0.0f, 0.0f);
    s_texcorr.attempts++;
    if (!s_texture_correction_enabled) { s_texcorr.no_correction++; return; }
    if (gp0_cmd_source_addr == 0xFFFFFFFFu) { s_texcorr.no_source++; return; }
    int indices[3] = { i0, i1, i2 };
    uint16_t z[3];
    for (int i = 0; i < 3; i++) {
        uint32_t addr = GPU_RAM_KEY((gp0_cmd_source_addr + (uint32_t)indices[i] * 4u));
        if (!gte_precision_load_word(addr, gp0_cmd_buf[indices[i]], NULL, NULL, &z[i]) ||
            z[i] == 0) {
            s_texcorr.no_depth++;
            return;
        }
    }
    float q[3] = { 1.0f / (float)z[0], 1.0f / (float)z[1], 1.0f / (float)z[2] };
    float qmax = q[0];
    if (q[1] > qmax) qmax = q[1];
    if (q[2] > qmax) qmax = q[2];
    if (qmax <= 0.0f) { s_texcorr.no_depth++; return; }
    s_texcorr.armed++;
    gr_set_perspective_triangle(1, q[0] / qmax, q[1] / qmax, q[2] / qmax);
}

/* Write a single pixel to VRAM with draw area clipping and mask bit handling */
static void raster_pixel(int32_t x, int32_t y, uint16_t color) {
    if (x < (int32_t)draw_area_left || x > (int32_t)draw_area_right) return;
    if (y < (int32_t)draw_area_top  || y > (int32_t)draw_area_bottom) return;
    uint32_t vx = (uint32_t)x & 1023u;
    uint32_t vy = (uint32_t)y & 511u;
    uint32_t idx = vy * 1024 + vx;
    if (check_mask_bit && (vram[idx] & 0x8000u)) return;
    vram[idx] = color | (set_mask_bit ? 0x8000u : 0u);
    gpu_vram_dirty_mark_row(vy);
}

/* Inclusive draw-area reject (same predicate as raster_pixel / hardware clip).
 *
 * MotK inter-movie / title / char-select OT drains often set GP0(E3/E4) to
 * (0,0)-(0,0) then submit thousands of 1x1 dots and shaded quads. Real GPU
 * clips those for free; our GL path was building two triangles per clipped
 * prim (gpu_share ~0.9, host FPS ~5–10). Skip the host rasterizer when the
 * post-offset bbox cannot touch the draw area. Side effects that must still
 * run (texpage latch, oversize reject) happen before these checks.
 *
 * Native-wide is intentionally different in X: the mirror renderer translates
 * canonical framebuffer coordinates by the live reveal offset and scissors to
 * the FULL wide surface, not GP0(E3/E4)'s 4:3 X range. Commit 31015ce originally
 * compared every primitive only with the guest draw area here, rejecting the
 * margin geometry before the mirror renderer could see it. Match the mirror's
 * exact X extent while its framebuffer target is active; Y remains the guest
 * draw area because native-wide does not extend vertically. At 4:3, in FMV/
 * menus, in squash mode, and for offscreen texture targets, margin is zero and
 * this remains the original fast reject byte-for-byte.
 *
 * OpokXeno independently identified the same host-side regression on Xenogears
 * and contributed the original generalized fix in psxrecomp PR #73:
 * https://github.com/mstan/psxrecomp/pull/73
 * Keep that credit with this guarded framebuffer-target variant. */
static inline int32_t draw_area_wide_x_margin(void) {
    int local_base = 0;
    if (!ws_native_wide_active()) return 0;
    if (ws_local_viewport_draw_target(&local_base)) return (int32_t)ws_nw_offset();
    if (ws_display_viewport_draw_target(NULL)) return (int32_t)ws_nw_offset();
    if (!ws_is_fb_base(draw_area_left)) return 0;
    return (int32_t)ws_nw_offset();
}

static inline void draw_area_host_x_bounds(int32_t *left, int32_t *right) {
    int32_t margin = draw_area_wide_x_margin();
    *left  = (int32_t)draw_area_left;
    *right = (int32_t)draw_area_right;
    if (margin > 0) {
        /* Use the union of the guest draw area and the widescreen mirror.
         * Wider staging areas may share the framebuffer X origin; clamping them
         * to the mirror width would drop valid canonical VRAM writes. */
        WsViewAnchor v = ws_view_packet();
        int32_t wide_left = (int32_t)draw_area_left - margin - (v.shift > 0 ? v.shift : 0);
        int32_t wide_right = (int32_t)draw_area_left + (int32_t)ws_disp_w() + margin - 1
                           - (v.shift < 0 ? v.shift : 0);
        if (wide_left  < *left)  *left  = wide_left;
        if (wide_right > *right) *right = wide_right;
    }
}

static inline int draw_area_out_point(int32_t x, int32_t y) {
    int32_t left, right;
    draw_area_host_x_bounds(&left, &right);
    return x < left || x > right
        || y < (int32_t)draw_area_top  || y > (int32_t)draw_area_bottom;
}

static inline int draw_area_out_bbox(const int32_t *vx, const int32_t *vy, int n) {
    int32_t minx = vx[0], maxx = vx[0], miny = vy[0], maxy = vy[0];
    for (int i = 1; i < n; i++) {
        if (vx[i] < minx) minx = vx[i];
        if (vx[i] > maxx) maxx = vx[i];
        if (vy[i] < miny) miny = vy[i];
        if (vy[i] > maxy) maxy = vy[i];
    }
    int32_t left, right;
    draw_area_host_x_bounds(&left, &right);
    return maxx < left || minx > right
        || maxy < (int32_t)draw_area_top  || miny > (int32_t)draw_area_bottom;
}

/* A polygon wholly past the drawing area draws nothing. While the overdraw
 * band is on, one inside the band still goes to the renderer: the drawing
 * area clips it there (nothing drawn, as before) and the shadow map takes it
 * as a caster. Dropped here, the casters the game's culls keep in the band
 * (a bush just off the picture) never reached the shadow map, and a shadow
 * appeared on the floor only with its object. */
static inline int draw_area_out_poly(const int32_t *vx, const int32_t *vy, int n) {
    if (!draw_area_out_bbox(vx, vy, n)) return 0;
    if (!ws_overdraw_x && !ws_overdraw_y) return 1;
    int32_t minx = vx[0], maxx = vx[0], miny = vy[0], maxy = vy[0];
    for (int i = 1; i < n; i++) {
        if (vx[i] < minx) minx = vx[i];
        if (vx[i] > maxx) maxx = vx[i];
        if (vy[i] < miny) miny = vy[i];
        if (vy[i] > maxy) maxy = vy[i];
    }
    int32_t left, right;
    draw_area_host_x_bounds(&left, &right);
    const int32_t bx = ws_od_gpu(0), by = ws_od_gpu(1);
    return maxx < left - bx || minx > right + bx
        || maxy < (int32_t)draw_area_top - by || miny > (int32_t)draw_area_bottom + by;
}

static inline int draw_area_out_rect(int32_t x, int32_t y, int w, int h) {
    if (w <= 0 || h <= 0) return 1;
    int32_t left, right;
    draw_area_host_x_bounds(&left, &right);
    return (x + w - 1) < left
        || x > right
        || (y + h - 1) < (int32_t)draw_area_top
        || y > (int32_t)draw_area_bottom;
}

/* Rasterize a flat-shaded triangle using DDA scanline fill.
 * Vertices are in screen coordinates (draw offset already applied). */
static void raster_triangle(int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1,
                            int32_t x2, int32_t y2,
                            uint16_t color)
{
    /* Sort vertices by Y (ascending). */
    int32_t tx, ty;
    if (y0 > y1) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }
    if (y1 > y2) { tx=x1; ty=y1; x1=x2; y1=y2; x2=tx; y2=ty; }
    if (y0 > y1) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }

    /* Reject degenerate (zero-height) or oversized triangles. */
    if (y0 == y2) return;
    if ((x2 - x0) > 1023 || (x0 - x2) > 1023) return;
    if ((y2 - y0) > 511) return;

    /* 64-bit fixed-point DDA (32.32) matching PS1 hardware behavior.
     * Reference: DuckStation gpu_sw_rasterizer.inl makefp_xy / makestep_xy */
    #define FP_ONE  (1LL << 32)
    #define MAKE_FP(v) (((int64_t)(v) << 32) + (FP_ONE - (1 << 11)))
    #define MAKE_STEP(dx, dy) \
        ((((int64_t)(dx) << 32) + ((dx) < 0 ? -((dy)-1) : (((dx) > 0) ? ((dy)-1) : 0))) / (dy))
    #define UNFP(fp) ((int32_t)((uint64_t)(fp) >> 32))

    /* Upper half: y0 to y1 */
    if (y1 > y0) {
        int32_t dy_long  = y2 - y0;
        int32_t dy_short = y1 - y0;
        int64_t base   = MAKE_FP(x0);
        int64_t step_long  = MAKE_STEP(x2 - x0, dy_long);
        int64_t step_short = MAKE_STEP(x1 - x0, dy_short);
        /* Determine which edge is left vs right */
        int64_t lx, rx, ls, rs;
        if (step_long < step_short) {
            lx = base; ls = step_long; rx = base; rs = step_short;
        } else {
            lx = base; ls = step_short; rx = base; rs = step_long;
        }
        for (int32_t y = y0; y < y1; y++) {
            int32_t xl = UNFP(lx);
            int32_t xr = UNFP(rx);
            for (int32_t x = xl; x < xr; x++)
                raster_pixel(x, y, color);
            lx += ls; rx += rs;
        }
    }

    /* Lower half: y1 to y2 */
    if (y2 > y1) {
        int32_t dy_long  = y2 - y0;
        int32_t dy_short = y2 - y1;
        /* Long edge continues from y0 to y2 */
        int64_t step_long  = MAKE_STEP(x2 - x0, dy_long);
        int64_t long_at_y1 = MAKE_FP(x0) + step_long * (y1 - y0);
        int64_t short_start = MAKE_FP(x1);
        int64_t step_short = MAKE_STEP(x2 - x1, dy_short);
        /* Determine left/right from the upper half's orientation */
        int64_t lx, rx, ls, rs;
        int64_t full_step_short_upper = (y1 > y0) ? MAKE_STEP(x1 - x0, y1 - y0) : 0;
        if (step_long < full_step_short_upper ||
            (y1 == y0 && long_at_y1 <= short_start)) {
            lx = long_at_y1; ls = step_long; rx = short_start; rs = step_short;
        } else {
            lx = short_start; ls = step_short; rx = long_at_y1; rs = step_long;
        }
        for (int32_t y = y1; y < y2; y++) {
            int32_t xl = UNFP(lx);
            int32_t xr = UNFP(rx);
            for (int32_t x = xl; x < xr; x++)
                raster_pixel(x, y, color);
            lx += ls; rx += rs;
        }
    }

    #undef FP_ONE
    #undef MAKE_FP
    #undef MAKE_STEP
    #undef UNFP
}

/* Execute mono triangle (GP0 0x20-0x23) */
static void gp0_exec_mono_tri(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t vx[3], vy[3];
    for (int i = 0; i < 3; i++) {
        parse_vertex(gp0_cmd_buf[1 + i], &vx[i], &vy[i]);
    }
    if (gpu_triangle_rejected(vx, vy, 0, 1, 2)) return;
    ws_auto_ui_transform_part(vx, vy, 3);
    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_poly(vx, vy, 3)) return;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    prepare_precise_triangle(1, 2, 3,
                             vx, vy);
    gr_draw_flat_triangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], color);
}

/* Execute mono quad (GP0 0x28-0x2B) — two triangles: (0,1,2) and (2,1,3) */
/* A native split divider belongs to the combined framebuffer. Preserve it in
 * canonical VRAM, but never mirror it into an expanded individual camera. */
static int ws_local_viewport_divider(int x0, int y0, int x1, int y1,
                                     uint16_t color, int semi_trans) {
    int base, src_w;
    if (color || semi_trans || !ws_native_wide_active() ||
        !ws_local_viewport_layout(&base, &src_w, NULL, NULL)) return 0;
    GpuDisplayInfo di; gpu_get_display_info(&di);
    const int seam = (int)di.display_x + (int)di.width / 2;
    return x0 + draw_offset_x >= seam - 1 && x1 + draw_offset_x <= seam + 1 &&
        x1 > x0 && y0 <= 0 && y1 >= (int)di.height;
}

static void gp0_exec_mono_quad(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t vx[4], vy[4];
    for (int i = 0; i < 4; i++)
        parse_vertex(gp0_cmd_buf[1 + i], &vx[i], &vy[i]);
    if (vx[0] == vx[2] && vx[1] == vx[3] && vy[0] == vy[1] && vy[2] == vy[3] &&
        ws_local_viewport_divider(vx[0], vy[0], vx[1], vy[2], color, semi_trans)) {
        gr_wide_disable_target();
        gr_set_semi_transparency(0, (int)semi_transparency);
        gr_draw_flat_rect(vx[0] + draw_offset_x, vy[0] + draw_offset_y,
                          vx[1] - vx[0], vy[2] - vy[0], color);
        ws_nw_sync_target();
        return;
    }
    int rej_a = gpu_triangle_rejected(vx, vy, 0, 1, 2);
    int rej_b = gpu_triangle_rejected(vx, vy, 2, 1, 3);
    if (rej_a && rej_b) return;
    int radial_mask=ws_nw_radial_mask_transform(vx,vy);
    int screen_mask=ws_native_wide_active() && gp0_cmd_source_addr != UINT32_MAX &&
        ws_hud_anchor_lookup(ws_screen_mask_tags, WS_HUD_ANCHOR_TABLE_SIZE,
            GPU_RAM_KEY(gp0_cmd_source_addr), gp0_cmd_buf, 5u,
            (uint32_t)s_frame_count, NULL);
    if (!radial_mask && !screen_mask) ws_auto_ui_transform_quad(vx, vy);

    /* Each added band is outside the original screen and executes in the
     * mask's own OT position/blend mode. Canonical VRAM clips it away. */
    if (screen_mask) {
        WsScreenMaskBand bands[2];
        int count = ws_screen_mask_bands(vx, vy, ws_disp_w(), ws_disp_h(),
                                         draw_area_wide_x_margin(), bands);
        if (count) {
            gr_set_semi_transparency(semi_trans, (int)semi_transparency);
            for (int i = 0; i < count; i++)
                gr_draw_flat_rect(bands[i].x + draw_offset_x,
                                  bands[i].y + draw_offset_y,
                                  bands[i].w, bands[i].h, color);
        }
    }

    ws_screen_transform_quad(vx);

    /* Full-screen filters are commonly encoded as an axis-aligned quad. Drawing
     * a semi-transparent quad as two independent triangles blends their shared
     * diagonal twice; render the equivalent rectangle once to avoid that seam.
     * The full-screen helper also grows the native 320-wide filter across the
     * sidecar surface in native-wide mode. Ordinary world quads are unchanged. */
    if (vx[0] == vx[2] && vx[1] == vx[3] &&
        vy[0] == vy[1] && vy[2] == vy[3] &&
        vx[1] > vx[0] && vy[2] > vy[0] &&
        vx[0] <= 0 && vx[1] >= ws_disp_w() &&
        vy[0] <= 0 && vy[2] >= ws_disp_h()) {
        int32_t x = vx[0];
        int32_t y = vy[0];
        int w = (int)(vx[1] - vx[0]);
        int h = (int)(vy[2] - vy[0]);
        ws_expand_fullscreen_rect(&x, y, &w, h);
        x += draw_offset_x;
        y += draw_offset_y;
        gr_set_semi_transparency(semi_trans, (int)semi_transparency);
        gr_draw_flat_rect(x, y, w, h, color);
        return;
    }
    if (!radial_mask && !screen_mask) {
        ws_nw_backdrop_stretch_quad(vx, vy);
        ws_nw_hud_shift_vertices(vx, 4);
    }
    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_poly(vx, vy, 4)) return;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    /* Semi axis-aligned mono quads (UI boxes/borders): one rect, not two tris.
     * Thin semi borders (e.g. CTR name-entry OT-1144 teal 3×H) otherwise double-
     * blend their shared diagonal — nearly the whole strip — and overpaint 3D. */
    if (semi_trans && ws_axis_aligned_quad(vx, vy)) {
        int32_t min_x = vx[0], max_x = vx[0], min_y = vy[0], max_y = vy[0];
        for (int i = 1; i < 4; i++) {
            if (vx[i] < min_x) min_x = vx[i];
            if (vx[i] > max_x) max_x = vx[i];
            if (vy[i] < min_y) min_y = vy[i];
            if (vy[i] > max_y) max_y = vy[i];
        }
        int w = (int)(max_x - min_x);
        int h = (int)(max_y - min_y);
        if (w > 0 && h > 0) {
            gr_draw_flat_rect(min_x, min_y, w, h, color);
            return;
        }
    }
    if (!rej_a) {
        int32_t tx[3] = { vx[0], vx[1], vx[2] };
        int32_t ty[3] = { vy[0], vy[1], vy[2] };
        prepare_precise_triangle(1, 2, 3, tx, ty);
        gr_draw_flat_triangle(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], color);
    }
    if (!rej_b) {
        int32_t tx[3] = { vx[2], vx[1], vx[3] };
        int32_t ty[3] = { vy[2], vy[1], vy[3] };
        prepare_precise_triangle(3, 2, 4, tx, ty);
        gr_draw_flat_triangle(vx[2], vy[2], vx[1], vy[1], vx[3], vy[3], color);
    }
}

/* Execute shaded triangle (GP0 0x30-0x33) — Gouraud shaded */
static void gp0_exec_shaded_tri(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t vx[3], vy[3];
    uint16_t c[3];
    /* Layout: C0, V0, C1, V1, C2, V2 */
    for (int i = 0; i < 3; i++) {
        c[i] = rgb888_to_rgb555(gp0_cmd_buf[i * 2] & 0xFFFFFFu);
        parse_vertex(gp0_cmd_buf[1 + i * 2], &vx[i], &vy[i]);
    }
    if (gpu_triangle_rejected(vx, vy, 0, 1, 2)) return;
    ws_auto_ui_transform_part(vx, vy, 3);
    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_poly(vx, vy, 3)) return;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    prepare_precise_triangle(1, 3, 5,
                             vx, vy);
    gr_draw_gouraud_triangle(vx[0], vy[0], c[0],
                             vx[1], vy[1], c[1],
                             vx[2], vy[2], c[2]);
}

void gpu_arm_shaded_quad_capture(void) { sq_cap_armed = 1; sq_cap_count = 0; }
int  gpu_get_shaded_quad_capture(const GpuSqCapEntry** out) {
    sq_cap_armed = 0; /* disarm on read */
    *out = sq_cap_buf;
    return sq_cap_count;
}

/* Execute shaded quad (GP0 0x38-0x3B) */
static void gp0_exec_shaded_quad(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int32_t vx[4], vy[4];
    uint16_t c[4];
    /* Layout: C0, V0, C1, V1, C2, V2, C3, V3 */
    for (int i = 0; i < 4; i++) {
        c[i] = rgb888_to_rgb555(gp0_cmd_buf[i * 2] & 0xFFFFFFu);
        parse_vertex(gp0_cmd_buf[1 + i * 2], &vx[i], &vy[i]);
    }
    int rej_a = gpu_triangle_rejected(vx, vy, 0, 1, 2);
    int rej_b = gpu_triangle_rejected(vx, vy, 2, 1, 3);
    if (rej_a && rej_b) return;
    int radial_mask=ws_nw_radial_mask_transform(vx,vy);
    if (!radial_mask) {
        ws_screen_transform_quad(vx);
        ws_auto_ui_transform_quad(vx, vy);
        ws_nw_backdrop_stretch_quad(vx, vy);
        ws_nw_hud_shift_vertices(vx, 4);
    }
    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_poly(vx, vy, 4)) return;
    /* Capture vertex data when armed. */
    if (sq_cap_armed && sq_cap_count < SQ_CAP_MAX) {
        GpuSqCapEntry* e = &sq_cap_buf[sq_cap_count++];
        for (int i = 0; i < 4; i++) {
            e->vx[i] = vx[i]; e->vy[i] = vy[i];
            e->color[i] = gp0_cmd_buf[i * 2] & 0xFFFFFFu;
        }
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    if (!rej_a) {
        int32_t tx[3] = { vx[0], vx[1], vx[2] };
        int32_t ty[3] = { vy[0], vy[1], vy[2] };
        prepare_precise_triangle(1, 3, 5, tx, ty);
        gr_draw_gouraud_triangle(vx[0], vy[0], c[0],
                                 vx[1], vy[1], c[1],
                                 vx[2], vy[2], c[2]);
    }
    if (!rej_b) {
        int32_t tx[3] = { vx[2], vx[1], vx[3] };
        int32_t ty[3] = { vy[2], vy[1], vy[3] };
        prepare_precise_triangle(5, 3, 7, tx, ty);
        gr_draw_gouraud_triangle(vx[2], vy[2], c[2],
                                 vx[1], vy[1], c[1],
                                 vx[3], vy[3], c[3]);
    }
}

/* Helper: build texpage word from GPU state for SW renderer.
 * Format: bits 0-3 = X base, bit 4 = Y base, bits 5-6 = semi-trans, bits 7-8 = color depth */
static uint16_t current_texpage(void) {
    return (uint16_t)(texpage_x | (texpage_y << 4) |
                      (semi_transparency << 5) | (texpage_colors << 7));
}

/* Hardware: the texpage attribute word carried inside every textured polygon
 * (GP0 0x24-0x3F with the texture bit) is copied into the GPU draw-mode state
 * (GPUSTAT bits 0-8) exactly like GP0(E1) bits 0-8 — the poly's own word, not
 * the last E1, decides its semi-transparency mode, and later rectangle/sprite
 * prims (which carry no texpage word) consume the state the poly left behind.
 * Beetle: SetTPage(CB[4 + ((cc>>4)&1)] >> 16) on every textured poly.
 * Bits 9-10 (dither / draw-to-display) exist only in E1; bit 11 (texture
 * disable) only latches when GP1(09h) allowed it, which polys can't grant. */
static void set_tpage_from_poly(uint16_t tpage_word) {
    texpage_x         = tpage_word & 0xF;
    texpage_y         = (tpage_word >> 4) & 1;
    semi_transparency = (tpage_word >> 5) & 3;
    texpage_colors    = (tpage_word >> 7) & 3;
}

/* Helper: set up SW renderer state before a textured draw.
 * semi_trans: whether the primitive has semi-transparency enabled (from opcode bit)
 * raw_texture: 1 if this is a raw-texture opcode (bit 0 of GP0 opcode set) */
static void setup_textured_draw(uint32_t color24, int semi_trans, int raw_texture) {
    uint32_t r = (color24 >> 0) & 0xFF;
    uint32_t g = (color24 >> 8) & 0xFF;
    uint32_t b = (color24 >> 16) & 0xFF;
    gr_set_color_modulation((int)r, (int)g, (int)b, raw_texture);
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
}

/* A close corner may cross the camera plane while the face is still visible.
 * The hardware's clamped H/Z and unsigned SZ cannot describe that polygon.
 * Use only intact GTE-derived words, clip their signed homogeneous projection,
 * and submit through the normal ordered GL passes and VRAM authority path. */
static int native_wide_projective_draw(void) {
    if (!gpu_ws_projective_enabled() || gp0_cmd_source_addr == UINT32_MAX) return 0;
    const unsigned op=gp0_cmd_buf[0]>>24;
    const unsigned stride=(op&16u)?3u:2u, n=(op&8u)?4u:3u;
    PSXProjectedVertex vertices[4];
    int needs_clip=0;
    for (unsigned i=0;i<n;++i) {
        const unsigned index=1+stride*i;
        const uint32_t word=gp0_cmd_buf[index];
        PGXPProjection p;
        if (!pgxp_load_projection(gp0_cmd_source_addr+4*index,word,&p)) return 0;
        int32_t x,y; parse_vertex(word,&x,&y);
        needs_clip |= p.z<p.near_z || x==-1024 || x==1023 || y==-1024 || y==1023;
        const uint32_t color=gp0_cmd_buf[(op&16u)?index-1:0];
        const uint32_t uv=gp0_cmd_buf[index+1];
        vertices[i]=(PSXProjectedVertex){p.x+(double)draw_offset_x*p.z,
            p.y+(double)draw_offset_y*p.z,p.z,uv&255,(uv>>8)&255,
            (color&255)/255.0,((color>>8)&255)/255.0,((color>>16)&255)/255.0};
    }
    if (!needs_clip) return 0;
    const uint16_t clut=gp0_cmd_buf[2]>>16;
    const uint16_t tpage=gp0_cmd_buf[2+stride]>>16;
    set_tpage_from_poly(tpage);
    const int semi=(op&2u)?(int)semi_transparency:-1;
    gl_renderer_draw_projected_triangle(vertices,tpage&0x1ff,
        (clut&63)*16,(clut>>6)&511,op&1u,semi,s_texture_correction_enabled);
    if (n==4) {
        const PSXProjectedVertex second[3]={vertices[2],vertices[1],vertices[3]};
        gl_renderer_draw_projected_triangle(second,tpage&0x1ff,
            (clut&63)*16,(clut>>6)&511,op&1u,semi,s_texture_correction_enabled);
    }
    return 1;
}

/* Execute textured triangle (GP0 0x24-0x27) */
static void gp0_exec_textured_tri(void) {
    if (native_wide_projective_draw()) return;
    if (gp0_tex_hidden((uint16_t)(gp0_cmd_buf[4] >> 16), (uint16_t)(gp0_cmd_buf[2] >> 16))) {
        set_tpage_from_poly((uint16_t)(gp0_cmd_buf[4] >> 16)); return;
    }
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t vx[3], vy[3];
    int u[3], v[3];
    /* Layout: color+v0, texcoord0+clut, v1, texcoord1+tpage, v2, texcoord2 */
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[3], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[5], &vx[2], &vy[2]);
    shadow_bf_drawn(vx, vy, 3);   /* a culled-looking face drawn after all */
    u[0] = gp0_cmd_buf[2] & 0xFF;        v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[4] & 0xFF;        v[1] = (gp0_cmd_buf[4] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[6] & 0xFF;        v[2] = (gp0_cmd_buf[6] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    /* Texpage from word 4 bits 16-31 */
    uint16_t tpage_word = (uint16_t)(gp0_cmd_buf[4] >> 16);
    uint16_t tpage = tpage_word & 0x1FF;
    set_tpage_from_poly(tpage_word);   /* latches even for size-rejected polys */
    if (gpu_triangle_rejected(vx, vy, 0, 1, 2)) return;
    ws_auto_ui_transform_part(vx, vy, 3);

    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_poly(vx, vy, 3)) return;

    setup_textured_draw(color24, semi_trans, raw_texture);
    prepare_precise_triangle(1, 3, 5,
                             vx, vy);
    prepare_texture_triangle(1, 3, 5);
    gr_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                              vx[1], vy[1], u[1], v[1],
                              vx[2], vy[2], u[2], v[2],
                              clut_x, clut_y, tpage);
}

/* Execute textured quad (GP0 0x2C-0x2F) */
static void gp0_exec_textured_quad(void) {
    if (native_wide_projective_draw()) return;
    const uint32_t disc_uv[4] = { gp0_cmd_buf[2], gp0_cmd_buf[4], gp0_cmd_buf[6], gp0_cmd_buf[8] };
    if (gp0_tex_hidden((uint16_t)(gp0_cmd_buf[4] >> 16), (uint16_t)(gp0_cmd_buf[2] >> 16)) ||
        shadow_blob_quad((gp0_cmd_buf[0] >> 25) & 1, gp0_cmd_buf[1], gp0_cmd_buf[3], gp0_cmd_buf[5], gp0_cmd_buf[7]) ||
        shadow_disc_quad((gp0_cmd_buf[0] >> 25) & 1, (uint16_t)(gp0_cmd_buf[4] >> 16), (uint16_t)(gp0_cmd_buf[2] >> 16), disc_uv)) {
        set_tpage_from_poly((uint16_t)(gp0_cmd_buf[4] >> 16)); return;
    }
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t vx[4], vy[4];
    int u[4], v[4];
    /* Layout: color+v0, tc0+clut, v1, tc1+tpage, v2, tc2, v3, tc3 */
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[3], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[5], &vx[2], &vy[2]);
    parse_vertex(gp0_cmd_buf[7], &vx[3], &vy[3]);
    shadow_bf_drawn(vx, vy, 4);   /* a culled-looking face drawn after all */
    u[0] = gp0_cmd_buf[2] & 0xFF;  v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[4] & 0xFF;  v[1] = (gp0_cmd_buf[4] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[6] & 0xFF;  v[2] = (gp0_cmd_buf[6] >> 8) & 0xFF;
    u[3] = gp0_cmd_buf[8] & 0xFF;  v[3] = (gp0_cmd_buf[8] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    uint16_t tpage_word = (uint16_t)(gp0_cmd_buf[4] >> 16);
    uint16_t tpage = tpage_word & 0x1FF;
    set_tpage_from_poly(tpage_word);   /* latches even for size-rejected polys */
    int rej_a = gpu_triangle_rejected(vx, vy, 0, 1, 2);
    int rej_b = gpu_triangle_rejected(vx, vy, 2, 1, 3);
    if (rej_a && rej_b) return;

    /* Widescreen: tagged billboard quads carry CPU-computed pixel offsets the
     * GTE squash never saw — re-squash every X around the prim's anchor. */
    {
        int32_t ws_ax;
        if (ws_tagged_anchor(&ws_ax))
            for (int i = 0; i < 4; i++) vx[i] = ws_scale_about(vx[i], ws_ax);
    }
    ws_auto_ui_transform_quad(vx, vy);
    ws_nw_backdrop_stretch_quad(vx, vy);   /* full-frame 2D backdrop image stretch (no-op else) */
    ws_nw_hud_shift_vertices(vx, 4);

    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_poly(vx, vy, 4)) return;

    setup_textured_draw(color24, semi_trans, raw_texture);

    int as_rect = vy[0] == vy[1] && vy[2] == vy[3] &&
                  vx[0] == vx[2] && vx[1] == vx[3] &&
                  u[0] == u[2] && u[1] == u[3] &&
                  v[0] == v[1] && v[2] == v[3];
    static const int k_quad_words[4] = { 1, 3, 5, 7 };
    if (as_rect) {
        const int n_precise = textured_quad_precise_vertices(k_quad_words);
        if (n_precise == 4) {
            pgxp_note_rect_bypass(1);
            as_rect = 0;
        } else if (n_precise > 0) {
            pgxp_note_rect_bypass(0);
        }
    }
    if (as_rect) {
        int x = vx[0] < vx[1] ? vx[0] : vx[1];
        int y = vy[0] < vy[2] ? vy[0] : vy[2];
        int w = vx[0] < vx[1] ? vx[1] - vx[0] : vx[0] - vx[1];
        int h = vy[0] < vy[2] ? vy[2] - vy[0] : vy[0] - vy[2];
        int left_u  = vx[0] < vx[1] ? u[0] : u[1];
        int right_u = vx[0] < vx[1] ? u[1] : u[0];
        int top_v   = vy[0] < vy[2] ? v[0] : v[2];
        int bot_v   = vy[0] < vy[2] ? v[2] : v[0];
        if (w > 0 && h > 0) {
            if (right_u - left_u == w && bot_v - top_v == h) {
                gr_draw_textured_rect(x, y, w, h, left_u, top_v,
                                      clut_x, clut_y, tpage);
                return;
            }
            gr_draw_textured_rect_scaled(x, y, w, h, left_u, top_v,
                                         right_u, bot_v,
                                         clut_x, clut_y, tpage);
            return;
        }
    }

    if (!rej_a) {
        int32_t tx[3] = { vx[0], vx[1], vx[2] };
        int32_t ty[3] = { vy[0], vy[1], vy[2] };
        prepare_precise_triangle(1, 3, 5, tx, ty);
        prepare_texture_triangle(1, 3, 5);
        gr_draw_textured_triangle(vx[0], vy[0], u[0], v[0],
                                  vx[1], vy[1], u[1], v[1],
                                  vx[2], vy[2], u[2], v[2],
                                  clut_x, clut_y, tpage);
    }
    if (!rej_b) {
        int32_t tx[3] = { vx[2], vx[1], vx[3] };
        int32_t ty[3] = { vy[2], vy[1], vy[3] };
        prepare_precise_triangle(5, 3, 7, tx, ty);
        prepare_texture_triangle(5, 3, 7);
        gr_draw_textured_triangle(vx[2], vy[2], u[2], v[2],
                                  vx[1], vy[1], u[1], v[1],
                                  vx[3], vy[3], u[3], v[3],
                                  clut_x, clut_y, tpage);
    }
}

/* Execute shaded textured triangle (GP0 0x34-0x37) */
static void gp0_exec_shaded_textured_tri(void) {
    if (native_wide_projective_draw()) return;
    if (gp0_tex_hidden((uint16_t)(gp0_cmd_buf[5] >> 16), (uint16_t)(gp0_cmd_buf[2] >> 16))) {
        set_tpage_from_poly((uint16_t)(gp0_cmd_buf[5] >> 16)); return;
    }
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t vx[3], vy[3];
    int u[3], v[3];
    uint32_t c[3];
    /* Layout: C0, V0, TC0+clut, C1, V1, TC1+tpage, C2, V2, TC2 */
    c[0] = gp0_cmd_buf[0] & 0xFFFFFFu;
    c[1] = gp0_cmd_buf[3] & 0xFFFFFFu;
    c[2] = gp0_cmd_buf[6] & 0xFFFFFFu;
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[4], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[7], &vx[2], &vy[2]);
    shadow_bf_drawn(vx, vy, 3);   /* a culled-looking face drawn after all */
    u[0] = gp0_cmd_buf[2] & 0xFF;  v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[5] & 0xFF;  v[1] = (gp0_cmd_buf[5] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[8] & 0xFF;  v[2] = (gp0_cmd_buf[8] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    uint16_t tpage_word = (uint16_t)(gp0_cmd_buf[5] >> 16);
    uint16_t tpage = tpage_word & 0x1FF;
    set_tpage_from_poly(tpage_word);   /* latches even for size-rejected polys */
    if (gpu_triangle_rejected(vx, vy, 0, 1, 2)) return;
    ws_auto_ui_transform_part(vx, vy, 3);

    ws_nw_hud_shift_vertices(vx, 3);
    for (int i = 0; i < 3; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_poly(vx, vy, 3)) return;

    uint16_t host_bank = mod_texture_packet_bank(gp0_cmd_source_addr, gp0_cmd_buf, 9u);
    if (host_bank && (gr_backend() != GR_BACKEND_OPENGL ||
                      !gl_renderer_select_texture_bank(host_bank))) return;

    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    prepare_precise_triangle(1, 4, 7,
                             vx, vy);
    prepare_texture_triangle(1, 4, 7);
    if (host_bank) {
        float q[3], xy[6];
        if (mod_texture_packet_precision(gp0_cmd_source_addr, q, xy)) {
            gr_set_precise_triangle(1,
                (int32_t)((xy[0] + draw_offset_x) * 65536.0f),
                (int32_t)((xy[1] + draw_offset_y) * 65536.0f),
                (int32_t)((xy[2] + draw_offset_x) * 65536.0f),
                (int32_t)((xy[3] + draw_offset_y) * 65536.0f),
                (int32_t)((xy[4] + draw_offset_x) * 65536.0f),
                (int32_t)((xy[5] + draw_offset_y) * 65536.0f));
            gr_set_perspective_triangle(1, q[0], q[1], q[2]);
        }
    }
    gr_draw_shaded_textured_triangle(vx[0], vy[0], u[0], v[0], c[0],
                                     vx[1], vy[1], u[1], v[1], c[1],
                                     vx[2], vy[2], u[2], v[2], c[2],
                                     clut_x, clut_y, tpage, raw_texture);
    if (host_bank) (void)gl_renderer_select_texture_bank(0);
}

int psx_mod_texture_banks_supported(void) {
    return gr_backend() == GR_BACKEND_OPENGL && gl_renderer_texture_banks_supported();
}

/* Execute shaded textured quad (GP0 0x3C-0x3F) */
static void gp0_exec_shaded_textured_quad(void) {
    if (native_wide_projective_draw()) return;
    const uint32_t disc_uv[4] = { gp0_cmd_buf[2], gp0_cmd_buf[5], gp0_cmd_buf[8], gp0_cmd_buf[11] };
    if (gp0_tex_hidden((uint16_t)(gp0_cmd_buf[5] >> 16), (uint16_t)(gp0_cmd_buf[2] >> 16)) ||
        shadow_blob_quad((gp0_cmd_buf[0] >> 25) & 1, gp0_cmd_buf[1], gp0_cmd_buf[4], gp0_cmd_buf[7], gp0_cmd_buf[10]) ||
        shadow_disc_quad((gp0_cmd_buf[0] >> 25) & 1, (uint16_t)(gp0_cmd_buf[5] >> 16), (uint16_t)(gp0_cmd_buf[2] >> 16), disc_uv)) {
        set_tpage_from_poly((uint16_t)(gp0_cmd_buf[5] >> 16)); return;
    }
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t vx[4], vy[4];
    int u[4], v[4];
    uint32_t c[4];
    /* Layout: C0, V0, TC0+clut, C1, V1, TC1+tpage, C2, V2, TC2, C3, V3, TC3 */
    c[0] = gp0_cmd_buf[0] & 0xFFFFFFu;
    c[1] = gp0_cmd_buf[3] & 0xFFFFFFu;
    c[2] = gp0_cmd_buf[6] & 0xFFFFFFu;
    c[3] = gp0_cmd_buf[9] & 0xFFFFFFu;
    parse_vertex(gp0_cmd_buf[1], &vx[0], &vy[0]);
    parse_vertex(gp0_cmd_buf[4], &vx[1], &vy[1]);
    parse_vertex(gp0_cmd_buf[7], &vx[2], &vy[2]);
    parse_vertex(gp0_cmd_buf[10], &vx[3], &vy[3]);
    shadow_bf_drawn(vx, vy, 4);   /* a culled-looking face drawn after all */
    u[0] = gp0_cmd_buf[2] & 0xFF;   v[0] = (gp0_cmd_buf[2] >> 8) & 0xFF;
    u[1] = gp0_cmd_buf[5] & 0xFF;   v[1] = (gp0_cmd_buf[5] >> 8) & 0xFF;
    u[2] = gp0_cmd_buf[8] & 0xFF;   v[2] = (gp0_cmd_buf[8] >> 8) & 0xFF;
    u[3] = gp0_cmd_buf[11] & 0xFF;  v[3] = (gp0_cmd_buf[11] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    uint16_t tpage_word = (uint16_t)(gp0_cmd_buf[5] >> 16);
    uint16_t tpage = tpage_word & 0x1FF;
    set_tpage_from_poly(tpage_word);   /* latches even for size-rejected polys */
    int rej_a = gpu_triangle_rejected(vx, vy, 0, 1, 2);
    int rej_b = gpu_triangle_rejected(vx, vy, 2, 1, 3);
    if (rej_a && rej_b) return;

    ws_screen_transform_quad(vx);
    ws_auto_ui_transform_quad(vx, vy);
    ws_nw_hud_shift_vertices(vx, 4);
    for (int i = 0; i < 4; i++) {
        vx[i] += draw_offset_x;
        vy[i] += draw_offset_y;
    }
    if (draw_area_out_poly(vx, vy, 4)) return;

    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    if (!rej_a) {
        int32_t tx[3] = { vx[0], vx[1], vx[2] };
        int32_t ty[3] = { vy[0], vy[1], vy[2] };
        prepare_precise_triangle(1, 4, 7, tx, ty);
        prepare_texture_triangle(1, 4, 7);
        gr_draw_shaded_textured_triangle(vx[0], vy[0], u[0], v[0], c[0],
                                         vx[1], vy[1], u[1], v[1], c[1],
                                         vx[2], vy[2], u[2], v[2], c[2],
                                         clut_x, clut_y, tpage, raw_texture);
    }
    if (!rej_b) {
        int32_t tx[3] = { vx[2], vx[1], vx[3] };
        int32_t ty[3] = { vy[2], vy[1], vy[3] };
        prepare_precise_triangle(7, 4, 10, tx, ty);
        prepare_texture_triangle(7, 4, 10);
        gr_draw_shaded_textured_triangle(vx[2], vy[2], u[2], v[2], c[2],
                                         vx[1], vy[1], u[1], v[1], c[1],
                                         vx[3], vy[3], u[3], v[3], c[3],
                                         clut_x, clut_y, tpage, raw_texture);
    }
}

/* Execute mono line (GP0 0x40-0x47) — Bresenham */
static void gp0_exec_mono_line(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0, x1, y1;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    parse_vertex(gp0_cmd_buf[2], &x1, &y1);
    if (psx_gpu_line_oversize(x0, y0, x1, y1)) return;
    if (x0 == x1 && ws_local_viewport_divider(x0, y0 < y1 ? y0 : y1,
            x0 + 1, (y0 > y1 ? y0 : y1) + 1, color, semi_trans)) {
        gr_wide_disable_target();
        gr_set_semi_transparency(0, (int)semi_transparency);
        gr_draw_line(x0 + draw_offset_x, y0 + draw_offset_y,
                      x1 + draw_offset_x, y1 + draw_offset_y, color);
        ws_nw_sync_target();
        return;
    }
    int32_t vx[2] = { x0, x1 };
    ws_nw_hud_shift_vertices(vx, 2);
    x0 = vx[0]; x1 = vx[1];
    x0 += draw_offset_x; y0 += draw_offset_y;
    x1 += draw_offset_x; y1 += draw_offset_y;
    {
        int32_t lx[2] = { x0, x1 }, ly[2] = { y0, y1 };
        if (draw_area_out_bbox(lx, ly, 2)) return;
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_line(x0, y0, x1, y1, color);
}

/* Execute shaded line (GP0 0x50-0x57) */
static void gp0_exec_shaded_line(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t c0 = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    uint16_t c1 = rgb888_to_rgb555(gp0_cmd_buf[2] & 0xFFFFFFu);
    int32_t x0, y0, x1, y1;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    parse_vertex(gp0_cmd_buf[3], &x1, &y1);
    if (psx_gpu_line_oversize(x0, y0, x1, y1)) return;
    int32_t vx[2] = { x0, x1 };
    ws_nw_hud_shift_vertices(vx, 2);
    x0 = vx[0]; x1 = vx[1];
    x0 += draw_offset_x; y0 += draw_offset_y;
    x1 += draw_offset_x; y1 += draw_offset_y;
    {
        int32_t lx[2] = { x0, x1 }, ly[2] = { y0, y1 };
        if (draw_area_out_bbox(lx, ly, 2)) return;
    }
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_shaded_line(x0, y0, c0, x1, y1, c1);
}

/* Execute mono rectangle (GP0 0x60-0x63) */
static void gp0_exec_mono_rect(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int w = gp0_cmd_buf[2] & 0xFFFFu;
    int h = (gp0_cmd_buf[2] >> 16) & 0xFFFFu;
    if (w > 1023) w = 1023;
    if (h > 511)  h = 511;
    if (ws_local_viewport_divider(x0, y0, x0 + w, y0 + h, color, semi_trans)) {
        gr_wide_disable_target();
        gr_set_semi_transparency(0, (int)semi_transparency);
        gr_draw_flat_rect(x0 + draw_offset_x, y0 + draw_offset_y, w, h, color);
        ws_nw_sync_target();
        return;
    }
    ws_expand_fullscreen_rect(&x0, y0, &w, h);
    /* Same auto_ui squash the textured rect path gets. Without it a flat
     * -coloured HUD mark keeps its 4:3 X while the textured primitives of the
     * same widget move toward their anchor, so at a wide aspect it is left
     * behind in open screen. Untouched when the prepass did not admit this
     * primitive, and rects never carry GTE output. */
    if (ws_active() && w > 0) {
        int corrected_w = w, corrected_h = h;
        const WsTag *tag = ws_screen_tag();
        if (tag) {
            x0 = ws_scale_about(x0, tag->anchor_x);
            w = ws_scale_len(w);
        } else if (ws_auto_ui_transform_rect(&x0, &y0, &corrected_w, &corrected_h)) {
            w = corrected_w;
            h = corrected_h;
        }
    }
    x0 += ws_nw_hud_shift(x0, w);   /* native-wide HUD corner re-anchor (no-op else) */
    x0 += draw_offset_x; y0 += draw_offset_y;
    if (draw_area_out_rect(x0, y0, w, h)) return;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_rect(x0, y0, w, h, color);
}

static void ws_repeat_textured_rect_reveal(int x, int y, int w, int h,
                                          int u, int v, uint16_t clut_x,
                                          uint16_t clut_y) {
    int margin = draw_area_wide_x_margin();
    int width = (int)ws_disp_w();
    if (margin <= 0 || gp0_cmd_source_addr == UINT32_MAX ||
        draw_area_right < draw_area_left ||
        draw_area_right - draw_area_left + 1u != (uint32_t)width)
        return;
    uint32_t address = gp0_cmd_source_addr & 0x1FFFFFFFu;
    if (address >= psx_ram_live_bytes() || (address & 3u)) return;
    int32_t period = ws_repeat_rect_tag_lookup(ws_repeat_rect_tags, address,
        gp0_cmd_buf, (uint32_t)gp0_words_needed, (uint32_t)s_frame_count);
    if (!period) return;
    WsRepeatSpan spans[128];
    int count = ws_repeat_rect_spans(x, u, w, period, (int)draw_area_left,
                                    width, margin, spans, 128);
    for (int i = 0; i < count; ++i) {
        if (draw_area_out_rect(spans[i].x, y, spans[i].w, h)) continue;
        /* Each span is outside the canonical draw area. Backend scissoring
         * discards canonical writes while the native-wide mirror draws it. */
        gr_draw_textured_rect(spans[i].x, y, spans[i].w, h, spans[i].u, v,
                              clut_x, clut_y, current_texpage());
    }
}

/* Execute textured rectangle (GP0 0x64-0x67) */
static void gp0_exec_textured_rect(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    int w = gp0_cmd_buf[3] & 0x3FF;
    int h = (gp0_cmd_buf[3] >> 16) & 0x1FF;
    if (w > 1023) w = 1023;
    if (h > 511)  h = 511;
    ws_clear_tagged_rect_reveal(y0 + draw_offset_y, h);

    const WsTag *tile_tag = ws_screen_tag();
    if (tile_tag && tile_tag->repeat_period > 0 && w > 0 && h > 0) {
        if (tile_tag->source_width > 0) {
            x0 -= u0;
            u0 = 0;
            w = tile_tag->source_width;
        }
        /* Repeat an authored composite at its original texture density. The
         * renderer knows the actual draw-order texpage; UV endpoints remain
         * integers here, so a full 256-texel strip never wraps to zero. */
        int32_t anchor = tile_tag->anchor_x;
        int32_t period = tile_tag->repeat_period;
        int32_t visible_lo = anchor + (int32_t)(
            (int64_t)(ws_disp_x() - anchor) * ws_xden / ws_xnum);
        int32_t visible_hi = anchor + (int32_t)(
            (int64_t)(ws_disp_x() + ws_disp_w() - anchor) * ws_xden / ws_xnum);
        int32_t first = (visible_lo - x0) / period - 1;
        int32_t last = (visible_hi - x0) / period + 1;
        /* Bounded even if a plugin supplies an impractically tiny period. */
        if (last - first <= 128) {
            setup_textured_draw(color24, semi_trans, raw_texture);
            for (int32_t n = first; n <= last; ++n) {
                int32_t source_x = x0 + n * period;
                if (source_x + w <= visible_lo || source_x >= visible_hi)
                    continue;
                int32_t x = ws_scale_about(source_x, anchor);
                int dw = ws_scale_about(source_x + w, anchor) - x;
                x += draw_offset_x;
                int32_t y = y0 + draw_offset_y;
                if (dw <= 0 || draw_area_out_rect(x, y, dw, h)) continue;
                gr_draw_textured_rect_scaled(x, y, dw, h,
                    u0, v0, u0 + w, v0 + h,
                    clut_x, clut_y, current_texpage());
            }
            return;
        }
    }

    /* Widescreen: tagged sprite parts squash around their projected anchor;
     * untagged SPRTs are screen-space 2D (HUD/menus) and squash around the
     * display centre. Texels keep full coverage via the scaled-rect path. */
    int ws_w = 0, ws_h = h;
    if (ws_active() && w > 0) {
        int32_t ws_ax;
        if (ws_tagged_anchor(&ws_ax)) {
            x0 = ws_scale_about(x0, ws_ax);
            ws_w = (int)ws_scale_len(w);
        } else {
            int corrected_w = w;
            if (ws_auto_ui_transform_rect(&x0, &y0, &corrected_w, &ws_h))
                ws_w = corrected_w;
            else if (ws_hud_sprt) {
                x0 = ws_scale_about(x0, ws_hud_pivot(x0, w));
                ws_w = (int)ws_scale_len(w);
            }
        }
    }
    x0 += ws_nw_hud_shift(x0, w);   /* native-wide HUD corner re-anchor (no-op else) */

    x0 += draw_offset_x; y0 += draw_offset_y;
    const int dw = (ws_w && ws_w != w) ? ws_w : w;
    if (draw_area_out_rect(x0, y0, dw, ws_h)) return;
    setup_textured_draw(color24, semi_trans, raw_texture);
    if (dw != w || ws_h != h)
        gr_draw_textured_rect_scaled(x0, y0, dw, ws_h, u0, v0, u0 + w, v0 + h,
                                     clut_x, clut_y, current_texpage());
    else
        gr_draw_textured_rect(x0, y0, w, h, u0, v0, clut_x, clut_y, current_texpage());
    ws_repeat_textured_rect_reveal(x0, y0, w, h, u0, v0, clut_x, clut_y);
}

/* Execute 1x1 dot (GP0 0x68-0x6B) */
static void gp0_exec_mono_dot(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x, y;
    parse_vertex(gp0_cmd_buf[1], &x, &y);
    { int dot_h = 1; ws_sprt_fixed_transform(&x, &y, 1, &dot_h); }   /* auto_ui squash; no-op when unadmitted */
    x += ws_nw_hud_shift(x, 1);
    x += draw_offset_x; y += draw_offset_y;
    if (draw_area_out_point(x, y)) return;
    gr_set_semi_transparency(semi_trans, (int)semi_transparency);
    gr_draw_flat_rect(x, y, 1, 1, color);
}

/* Execute textured 1x1 dot (GP0 0x6C-0x6F) */
static void gp0_exec_textured_dot(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    { int dot_h = 1; (void)ws_sprt_fixed_transform(&x0, &y0, 1, &dot_h); }  /* position only; 1px stays 1px */
    x0 += ws_nw_hud_shift(x0, 1);
    x0 += draw_offset_x; y0 += draw_offset_y;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;
    setup_textured_draw(color24, semi_trans, raw_texture);
    gr_draw_textured_rect(x0, y0, 1, 1, u0, v0, clut_x, clut_y, current_texpage());
}

/* Execute 8x8 textured sprite (GP0 0x74-0x77) */
static void gp0_exec_textured_8x8(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int ws_h = 8;
    int ws_w = ws_sprt_fixed_transform(&x0, &y0, 8, &ws_h);
    x0 += ws_nw_hud_shift(x0, 8);   /* native-wide HUD corner re-anchor (no-op else) */
    x0 += draw_offset_x; y0 += draw_offset_y;
    const int dw = (ws_w && ws_w != 8) ? ws_w : 8;
    if (draw_area_out_rect(x0, y0, dw, ws_h)) return;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;

    setup_textured_draw(color24, semi_trans, raw_texture);
    if (dw != 8 || ws_h != 8)
        gr_draw_textured_rect_scaled(x0, y0, dw, ws_h, u0, v0, u0 + 8, v0 + 8,
                                     clut_x, clut_y, current_texpage());
    else
        gr_draw_textured_rect(x0, y0, 8, 8, u0, v0, clut_x, clut_y, current_texpage());
}

/* Execute 8x8 sprite (GP0 0x70-0x73) */
static void gp0_exec_mono_8x8(void) {
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    int ws_h = 8;
    int ws_w = ws_sprt_fixed_transform(&x0, &y0, 8, &ws_h);
    x0 += ws_nw_hud_shift(x0, 8);
    x0 += draw_offset_x; y0 += draw_offset_y;
    {
        int dw = (ws_w && ws_w != 8) ? ws_w : 8;
        if (draw_area_out_rect(x0, y0, dw, ws_h)) return;
        gr_set_semi_transparency(semi_trans, (int)semi_transparency);
        gr_draw_flat_rect(x0, y0, dw, ws_h, color);
    }
}

/* Execute 16x16 textured sprite (GP0 0x7C-0x7F) */
static void gp0_exec_textured_16x16(void) {
    uint32_t color24 = gp0_cmd_buf[0] & 0xFFFFFFu;
    int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
    int raw_texture = (gp0_cmd_buf[0] >> 24) & 1;
    int32_t x0, y0;
    parse_vertex(gp0_cmd_buf[1], &x0, &y0);
    /* Never suppress MMX6 BG packets at a guessed finite-map boundary. The
     * classifier cannot distinguish an authored layer entering the reveal from
     * a stale ring slot; suppressing here caused the stage-start black flicker. */
    uint32_t host_packet = ws_bg2d_host_packet();
    int host_tile = host_packet != 0;
    /* A pending full-width OT can outlive a resize back to 4:3. Keep its
     * central tiles; the normal draw area clips the extra columns. Dropping
     * every host packet here would blank that transition frame. */
    int ws_h = 16;
    int ws_w = host_tile ? 0 : ws_sprt_fixed_transform(&x0, &y0, 16, &ws_h);
    if (!host_tile) x0 += ws_nw_hud_shift(x0, 16);
    x0 += draw_offset_x; y0 += draw_offset_y;
    int u0 = gp0_cmd_buf[2] & 0xFF;
    int v0 = (gp0_cmd_buf[2] >> 8) & 0xFF;
    uint16_t clut = (uint16_t)(gp0_cmd_buf[2] >> 16);
    uint16_t clut_x = (clut & 0x3F) * 16;
    uint16_t clut_y = (clut >> 6) & 0x1FF;

    const int dw = (ws_w && ws_w != 16) ? ws_w : 16;
    if (draw_area_out_rect(x0, y0, dw, ws_h)) return;
    uint16_t bank = host_tile &&
        (psx_read_word(host_packet + 28u) & ~GPU_WS_BG2D_MIRROR_X) == GPU_WS_BG2D_BANK_PACKET_MAGIC ?
        (uint16_t)(psx_read_word(host_packet + 16u) >> 16) : 0;
    if (bank && (gr_backend() != GR_BACKEND_OPENGL ||
        !gl_renderer_select_texture_bank_live_clut(bank))) return;
    setup_textured_draw(color24, semi_trans, raw_texture);
    if (host_tile && (psx_read_word(host_packet + 28u) & GPU_WS_BG2D_MIRROR_X))
        gr_draw_textured_rect_scaled(x0, y0, 16, 16, u0 + 15, v0, u0 - 1, v0 + 16,
                                     clut_x, clut_y, current_texpage());
    else if (dw != 16 || ws_h != 16)
        gr_draw_textured_rect_scaled(x0, y0, dw, ws_h, u0, v0, u0 + 16, v0 + 16,
                                     clut_x, clut_y, current_texpage());
    else
        gr_draw_textured_rect(x0, y0, 16, 16, u0, v0, clut_x, clut_y, current_texpage());
    if (bank) (void)gl_renderer_select_texture_bank(0);
}

/* ---- GP0 command execution ---- */

static void gp0_exec_nop(void) {
    /* 0x00 / 0x01 — nothing to do */
}

static void gp0_exec_fill_rect(void) {
    /* 0x02 — Fill Rectangle in VRAM
     * Word 0: 0x02BBGGRR (color)
     * Word 1: YstartXstart (X bits 0-9 aligned to 16, Y bits 16-24)
     * Word 2: YsizXsiz (W bits 0-9 rounded up to 16, H bits 16-24) */
    uint32_t color24 = gp0_cmd_buf[0] & 0x00FFFFFF;
    uint16_t color16 = rgb888_to_rgb555(color24);

    uint32_t dst_x = gp0_cmd_buf[1] & 0x3F0u;          /* X masked to 16-pixel alignment */
    uint32_t dst_y = (gp0_cmd_buf[1] >> 16) & 0x1FFu;
    uint32_t width = ((gp0_cmd_buf[2] & 0x3FFu) + 0xFu) & ~0xFu;  /* round up to 16 */
    uint32_t height = (gp0_cmd_buf[2] >> 16) & 0x1FFu;

    /* Fill ignores draw area, mask bits, and draw offset — writes directly to
     * VRAM. Routed through the renderer so it also fills the hi-res
     * supersampling mirror (no-op cost when supersampling is off). */
    gpu_drawn_mark((int)dst_x, (int)dst_y, (int)(dst_x + width) - 1, (int)(dst_y + height) - 1);
    gr_fill_rect((int)dst_x, (int)dst_y, (int)width, (int)height, color16);
    gpu_timeline_note(GTL_FILL, dst_x | (dst_y << 16), width | (height << 16));

    /* Native-wide: when the game clears a display buffer, clear the full width
     * of that buffer's wide surface over the same rows — refreshing the centred
     * content region and keeping the revealed margins clean. */
    int local_base = 0;
    if (ws_native_wide_active() &&
        (ws_is_fb_base(dst_x) ||
         (ws_local_viewport_draw_target(&local_base) &&
          dst_x == (uint32_t)local_base))) {
        gr_wide_clear((int)dst_x, (int)dst_y, (int)height, color16);
    }
}

static void gp0_exec_draw_mode(void) {
    /* 0xE1 — Draw Mode / Texpage
     * Bits 0-3: texpage X base
     * Bit 4: texpage Y base
     * Bits 5-6: semi-transparency mode
     * Bits 7-8: texture color mode
     * Bit 9: dither
     * Bit 10: draw to display area
     * Bit 11: texture disable (when allowed by GP1) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    texpage_x         = param & 0xF;
    texpage_y         = (param >> 4) & 1;
    semi_transparency = (param >> 5) & 3;
    texpage_colors    = (param >> 7) & 3;
    dither_enabled    = (param >> 9) & 1;
    gr_set_dither((int)dither_enabled);
    draw_to_display   = (param >> 10) & 1;
    texture_disable   = (param >> 11) & 1;
    /* Sync semi-transparency to SW renderer — actual blending is per-primitive */
}

static void gp0_exec_texture_window(void) {
    /* 0xE2 — Texture Window
     * Bits 0-4: mask X (in 8-pixel steps)
     * Bits 5-9: mask Y
     * Bits 10-14: offset X
     * Bits 15-19: offset Y */
    texture_window_value = gp0_cmd_buf[0] & 0x000FFFFFu;
    gr_set_texture_window(texture_window_value);
}

static void gp0_exec_draw_area_tl(void) {
    /* 0xE3 — Set Drawing Area Top-Left
     * Bits 0-9: X
     * Bits 10-19: Y */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_area_left = param & 0x3FF;
    draw_area_top  = (param >> 10) & 0x3FF;
    s_gpu_drawn_area = 1;
    split_trace_note_draw_area();
    gr_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
    ws_nw_sync_target();  /* back buffer (draw_area_left) → wide mirror surface */
    gpu_timeline_note(GTL_DRAW_AREA, draw_area_left, draw_area_top);
}

static void gp0_exec_draw_area_br(void) {
    /* 0xE4 — Set Drawing Area Bottom-Right
     * Bits 0-9: X
     * Bits 10-19: Y */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_area_right  = param & 0x3FF;
    draw_area_bottom = (param >> 10) & 0x3FF;
    s_gpu_drawn_area = 1;
    split_trace_note_draw_area();
    gr_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
    ws_nw_sync_target();  /* back buffer (draw_area_left) → wide mirror surface */
}

static void gp0_exec_draw_offset(void) {
    /* 0xE5 — Set Drawing Offset
     * Bits 0-10: X (signed 11-bit)
     * Bits 11-21: Y (signed 11-bit) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    draw_offset_x = sign_extend(param & 0x7FFu, 11);
    draw_offset_y = sign_extend((param >> 11) & 0x7FFu, 11);
    if (draw_offset_y < g_doff_min_this) g_doff_min_this = draw_offset_y;
    if (draw_offset_y > g_doff_max_this) g_doff_max_this = draw_offset_y;
    g_doff_cnt_this++;
    /* Native-wide mirrors framebuffer draws into a separate wide surface (canonical
     * VRAM stays faithful); the renderer needs the live draw offset to translate
     * into surface-local coords, so keep gr_set_draw_offset current. */
    gr_set_draw_offset(draw_offset_x, draw_offset_y);
}

static void gp0_exec_mask_bit(void) {
    /* 0xE6 — Mask Bit Setting
     * Bit 0: set mask bit when drawing (force bit 15 of pixels)
     * Bit 1: check mask bit (don't draw to pixels with bit 15 set) */
    uint32_t param = gp0_cmd_buf[0] & 0x00FFFFFFu;
    set_mask_bit   = param & 1;
    check_mask_bit = (param >> 1) & 1;
    gr_set_mask_bits((int)set_mask_bit, (int)check_mask_bit);
}

/* A0 upload history for debug inspection */
#define A0_HISTORY_CAP 128
typedef struct {
    uint16_t x, y, w, h;
    uint32_t first_words[4];
    int word_count;
    uint32_t func_addr;
    uint32_t sp_val;       /* CPU $sp at time of A0 header */
    uint32_t ra_val;       /* CPU $ra register at time of A0 header */
    uint32_t s1_val;       /* CPU $s1 = LoadImage RECT ptr (x/y/w/h) in FUN_80069dfc */
    uint32_t s2_val;       /* CPU $s2 = TRUE source pixel ptr (`move s2,a1` at entry) */
    uint32_t a0_val;       /* CPU $a0 at capture (RECT arg; may be clobbered) */
    uint32_t a1_val;       /* CPU $a1 at capture (source arg; may be clobbered) */
    uint32_t frame_stamp;  /* s_frame_count at the upload — for load-vs-upload ordering */
    uint32_t stack[10];    /* first 10 words from sp (sp+32=saved $s1, sp+36=saved $ra) */
} A0HistEntry;
static A0HistEntry a0_history[A0_HISTORY_CAP];
static int a0_history_count = 0;
static int a0_capture_slot = -1;  /* currently capturing data words for this slot */

extern uint32_t psx_read_word(uint32_t addr);

int gpu_get_a0_history(int index, int *x, int *y, int *w, int *h,
                       uint32_t *fw0, uint32_t *fw1, int *wcount) {
    if (index < 0 || index >= a0_history_count) return 0;
    *x = a0_history[index].x; *y = a0_history[index].y;
    *w = a0_history[index].w; *h = a0_history[index].h;
    *fw0 = a0_history[index].first_words[0];
    *fw1 = a0_history[index].first_words[1];
    *wcount = a0_history[index].word_count;
    return 1;
}
int gpu_get_a0_count(void) { return a0_history_count; }
int gpu_get_a0_extra(int index, uint32_t *func, uint32_t *sp, uint32_t *ra,
                     uint32_t *s1, uint32_t *stack10) {
    if (index < 0 || index >= a0_history_count) return 0;
    *func = a0_history[index].func_addr;
    *sp = a0_history[index].sp_val;
    *ra = a0_history[index].ra_val;
    *s1 = a0_history[index].s1_val;
    memcpy(stack10, a0_history[index].stack, 10 * sizeof(uint32_t));
    return 1;
}
/* True source pointer + arg regs + frame for the LoadImage upload (s2 = source). */
int gpu_get_a0_src(int index, uint32_t *s2, uint32_t *a0, uint32_t *a1, uint32_t *frame) {
    if (index < 0 || index >= a0_history_count) return 0;
    *s2 = a0_history[index].s2_val;
    *a0 = a0_history[index].a0_val;
    *a1 = a0_history[index].a1_val;
    *frame = a0_history[index].frame_stamp;
    return 1;
}

static void gp0_exec_cpu_to_vram(void) {
    /* 0xA0 — CPU→VRAM Copy (header: 3 words)
     * Word 0: command
     * Word 1: destination coords (X bits 0-9, Y bits 16-24)
     * Word 2: dimensions (W bits 0-9, H bits 16-24)
     * Followed by pixel data words */
    vram_write_x = gp0_cmd_buf[1] & 0x3FFu;
    vram_write_y = (gp0_cmd_buf[1] >> 16) & 0x1FFu;

    uint32_t w = gp0_cmd_buf[2] & 0x3FFu;
    uint32_t h = (gp0_cmd_buf[2] >> 16) & 0x1FFu;
    /* 0 means max dimension */
    vram_write_w = (w == 0) ? 0x400 : (uint16_t)w;
    vram_write_h = (h == 0) ? 0x200 : (uint16_t)h;
    gr_vram_upload_begin(vram_write_x, vram_write_y, vram_write_w, vram_write_h);

    /* A full history retains old uploads; later transfers must not append
     * to the last slot and overflow its diagnostic word counter. */
    a0_capture_slot = -1;
    if (a0_history_count < A0_HISTORY_CAP) {
        int slot = a0_history_count++;
        a0_history[slot].x = vram_write_x;
        a0_history[slot].y = vram_write_y;
        a0_history[slot].w = vram_write_w;
        a0_history[slot].h = vram_write_h;
        a0_history[slot].word_count = 0;
        a0_history[slot].func_addr = g_debug_current_func_addr;
        memset(a0_history[slot].first_words, 0, sizeof(a0_history[slot].first_words));
        /* Capture CPU context for caller tracing.
         * func_1FC38524 (shell LoadImage) uses $s1 as source data pointer. */
        a0_history[slot].sp_val = 0;
        a0_history[slot].ra_val = 0;
        a0_history[slot].s1_val = 0;
        a0_history[slot].s2_val = 0;
        a0_history[slot].a0_val = 0;
        a0_history[slot].a1_val = 0;
        a0_history[slot].frame_stamp = (uint32_t)s_frame_count;
        memset(a0_history[slot].stack, 0, sizeof(a0_history[slot].stack));
        if (debug_cpu_ptr) {
            uint32_t sp = debug_cpu_ptr->gpr[29];
            a0_history[slot].sp_val = sp;
            a0_history[slot].ra_val = debug_cpu_ptr->gpr[31];
            a0_history[slot].s1_val = debug_cpu_ptr->gpr[17]; /* $s1 */
            a0_history[slot].s2_val = debug_cpu_ptr->gpr[18]; /* $s2 = source ptr */
            a0_history[slot].a0_val = debug_cpu_ptr->gpr[4];  /* $a0 */
            a0_history[slot].a1_val = debug_cpu_ptr->gpr[5];  /* $a1 */
            uint32_t sp_phys = psx_ram_canonical_offset(sp);
            for (int si = 0; si < 10 && sp_phys + (si + 1) * 4 <= psx_ram_live_bytes(); si++)
                a0_history[slot].stack[si] = psx_read_word(sp + si * 4);
        }
        a0_capture_slot = slot;
    }

    vram_write_col = 0;
    vram_write_row = 0;

    uint32_t num_pixels = (uint32_t)vram_write_w * (uint32_t)vram_write_h;
    vram_write_remaining = (num_pixels + 1) / 2;

    if (vram_write_remaining > 0)
        gp0_state = GP0_VRAM_WRITE;
}

/* C0 (VRAM→CPU) history — uses c0_history_fwd declared at top of file */
#define C0_HISTORY_CAP C0_HISTORY_CAP_FWD
#define c0_history c0_history_fwd
#define c0_history_count c0_history_count_fwd
#define c0_capture_slot c0_capture_slot_fwd

/* Total GP0 C0 reads since boot; the ring holds the newest C0_HISTORY_CAP. */
uint64_t gpu_get_c0_total(void) { return c0_history_count; }
int gpu_get_c0_count(void) {
    return c0_history_count < C0_HISTORY_CAP ? (int)c0_history_count : C0_HISTORY_CAP;
}
/* index 0 = oldest entry still in the ring. */
int gpu_get_c0_history(int index, int *x, int *y, int *w, int *h,
                       uint32_t *func, uint32_t *sp, uint32_t *s1,
                       uint32_t *fw0, uint32_t *fw1, int *rcount) {
    if (index < 0 || index >= gpu_get_c0_count()) return 0;
    if (c0_history_count > C0_HISTORY_CAP)
        index = (int)((c0_history_count + (uint64_t)index) % C0_HISTORY_CAP);
    *x = c0_history[index].x; *y = c0_history[index].y;
    *w = c0_history[index].w; *h = c0_history[index].h;
    *func = c0_history[index].func_addr;
    *sp = c0_history[index].sp_val;
    *s1 = c0_history[index].s1_val;
    *fw0 = c0_history[index].first_words[0];
    *fw1 = c0_history[index].first_words[1];
    *rcount = c0_history[index].read_count;
    return 1;
}
uint32_t gpu_get_c0_frame(int index) {
    if (index < 0 || index >= gpu_get_c0_count()) return 0;
    if (c0_history_count > C0_HISTORY_CAP)
        index = (int)((c0_history_count + (uint64_t)index) % C0_HISTORY_CAP);
    return c0_history[index].frame;
}

static void gp0_exec_vram_to_cpu(void) {
    /* 0xC0 — VRAM→CPU Copy (3 words)
     * Word 1: source coords
     * Word 2: dimensions
     * After this, data is read via GPUREAD */
    vram_read_x = gp0_cmd_buf[1] & 0x3FFu;
    vram_read_y = (gp0_cmd_buf[1] >> 16) & 0x1FFu;

    uint32_t w = gp0_cmd_buf[2] & 0x3FFu;
    uint32_t h = (gp0_cmd_buf[2] >> 16) & 0x1FFu;
    vram_read_w = (w == 0) ? 0x400 : (uint16_t)w;
    vram_read_h = (h == 0) ? 0x200 : (uint16_t)h;

    /* Record for debug */
    {
        int slot = (int)(c0_history_count++ % C0_HISTORY_CAP);
        c0_history[slot].frame = (uint32_t)s_frame_count;
        c0_history[slot].x = vram_read_x;
        c0_history[slot].y = vram_read_y;
        c0_history[slot].w = vram_read_w;
        c0_history[slot].h = vram_read_h;
        c0_history[slot].func_addr = g_debug_current_func_addr;
        c0_history[slot].sp_val = debug_cpu_ptr ? debug_cpu_ptr->gpr[29] : 0;
        c0_history[slot].s1_val = debug_cpu_ptr ? debug_cpu_ptr->gpr[17] : 0;
        memset(c0_history[slot].first_words, 0, sizeof(c0_history[slot].first_words));
        c0_history[slot].read_count = 0;
        c0_capture_slot = slot;
    }

    vram_read_col = 0;
    vram_read_row = 0;
    vram_read_active = 1;
}

/* Determine how many words a GP0 command requires (header only, not counting
 * variable-length data for 0xA0). Returns -1 for polylines (terminated by
 * sentinel). Returns 0 for unknown commands (will be fatal). */
static int gp0_command_word_count(uint8_t opcode) {
    switch (opcode) {
        /* NOP / control */
        case 0x00: return 1;
        case 0x01: return 1;  /* clear cache */
        case 0x02: return 3;  /* fill rect */
        case 0x1F: return 1;  /* IRQ request */

        /* Drawing commands — polygons */
        case 0x20: case 0x21: case 0x22: case 0x23: return 4;  /* mono tri */
        case 0x24: case 0x25: case 0x26: case 0x27: return 7;  /* textured tri */
        case 0x28: case 0x29: case 0x2A: case 0x2B: return 5;  /* mono quad */
        case 0x2C: case 0x2D: case 0x2E: case 0x2F: return 9;  /* textured quad */
        case 0x30: case 0x31: case 0x32: case 0x33: return 6;  /* shaded tri */
        case 0x34: case 0x35: case 0x36: case 0x37: return 9;  /* shaded textured tri */
        case 0x38: case 0x39: case 0x3A: case 0x3B: return 8;  /* shaded quad */
        case 0x3C: case 0x3D: case 0x3E: case 0x3F: return 12; /* shaded textured quad */

        /* Lines */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: return 3;  /* mono line */
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: return -1; /* mono polyline */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: return 4;  /* shaded line */
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: return -1; /* shaded polyline */

        /* Rectangles */
        case 0x60: case 0x61: case 0x62: case 0x63: return 3;  /* variable rect */
        case 0x64: case 0x65: case 0x66: case 0x67: return 4;  /* variable textured rect */
        case 0x68: case 0x69: case 0x6A: case 0x6B: return 2;  /* 1x1 dot */
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: return 3;  /* 1x1 textured */
        case 0x70: case 0x71: case 0x72: case 0x73: return 2;  /* 8x8 rect */
        case 0x74: case 0x75: case 0x76: case 0x77: return 3;  /* 8x8 textured */
        case 0x78: case 0x79: case 0x7A: case 0x7B: return 2;  /* 16x16 rect */
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: return 3;  /* 16x16 textured */

        /* VRAM copy / transfer */
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F: return 4;  /* VRAM→VRAM */

        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: return 3;  /* CPU→VRAM (header) */

        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF: return 3;  /* VRAM→CPU (header) */

        /* Environment */
        case 0xE0: return 1;  /* NOP */
        case 0xE1: return 1;  /* draw mode */
        case 0xE2: return 1;  /* texture window */
        case 0xE3: return 1;  /* draw area TL */
        case 0xE4: return 1;  /* draw area BR */
        case 0xE5: return 1;  /* draw offset */
        case 0xE6: return 1;  /* mask bits */

        default:
            /* 0x03-0x1E, 0xE7-0xEF, 0xFF: NOP (1 word) per DuckStation */
            if ((opcode >= 0x03 && opcode <= 0x1E) ||
                (opcode >= 0xE7 && opcode <= 0xEF) ||
                opcode == 0xFF) {
                return 1;
            }
            return 0;  /* unknown */
    }
}

int gpu_gp0_command_word_count(uint8_t opcode) {
    return gp0_command_word_count(opcode);
}

static void ws_ui_prepass_add(const uint32_t *words, uint32_t word_count,
                              uint32_t source_addr, uint16_t rank,
                              int full_draw_area) {
    if (rank == 0xFFFFu) return;
    if (ws_ui_prepass_count >= WS_UI_PREPASS_MAX) { ws_ui_reject.cap++; return; }
    uint32_t op = words[0] >> 24;
    int32_t min_x, max_x, min_y, max_y;
    int enclosed_only = 0;

    if (op >= 0x20u && op <= 0x3Fu && !(op & 0x08u)) {
        /* A triangle is a widget part only when it lies inside the widget:
         * a compass needle, a dial hand (Spider-Man's compass arrow, two
         * flat triangles in the ring's own rank). */
        const int textured = (op & 0x04u) != 0;
        const int shaded = (op & 0x10u) != 0;
        const int step = 1 + (textured ? 1 : 0) + (shaded ? 1 : 0);
        int32_t vx, vy;
        parse_vertex(words[1], &min_x, &min_y);
        max_x = min_x; max_y = min_y;
        for (int i = 1; i < 3; i++) {
            parse_vertex(words[1 + i * step], &vx, &vy);
            if (vx < min_x) min_x = vx;
            if (vx > max_x) max_x = vx;
            if (vy < min_y) min_y = vy;
            if (vy > max_y) max_y = vy;
        }
        enclosed_only = 1;
    } else if (op >= 0x20u && op <= 0x3Fu && (op & 0x08u)) {
        /* Any 4-vertex polygon. Untextured HUD fills (health gradients,
         * meter bars) sit in the same front layer as the textured frames
         * around them; leaving them out kept their raw 4:3 X while the frame
         * squashed, so the fill overhung it at wide aspects (Spider-Man's
         * health and webbing meters). Same rank + axis-aligned gate as the
         * textured quads. */
        const int textured = (op & 0x04u) != 0;
        const int shaded = (op & 0x10u) != 0;
        int indices[4];
        if (textured && shaded) {
            indices[0] = 1; indices[1] = 4;
            indices[2] = 7; indices[3] = 10;
        } else if (textured || shaded) {
            indices[0] = 1; indices[1] = 3;
            indices[2] = 5; indices[3] = 7;
        } else {
            indices[0] = 1; indices[1] = 2;
            indices[2] = 3; indices[3] = 4;
        }
        int32_t vx[4], vy[4];
        for (int i = 0; i < 4; i++)
            parse_vertex(words[indices[i]], &vx[i], &vy[i]);
        if (!ws_axis_aligned_quad(vx, vy)) {
            ws_ui_reject.not_axis++;
            enclosed_only = 1;           /* a rotated piece: see triangles */
        }
        min_x = max_x = vx[0]; min_y = max_y = vy[0];
        for (int i = 1; i < 4; i++) {
            if (vx[i] < min_x) min_x = vx[i];
            if (vx[i] > max_x) max_x = vx[i];
            if (vy[i] < min_y) min_y = vy[i];
            if (vy[i] > max_y) max_y = vy[i];
        }
    } else if (op >= 0x60u && op <= 0x7Fu) {
        /* GP0 rectangle / sprite. Bits 4-3 select the size (00 variable,
         * 01 1x1, 10 8x8, 11 16x16) and bit 2 whether it is textured.
         *
         * Only the TEXTURED families used to be admitted here, which silently
         * dropped every flat-coloured HUD mark. Those then kept their raw 4:3
         * X while the textured primitives beside them were squashed toward an
         * anchor -- so at a wide aspect they were left stranded in open screen,
         * to the left of the cluster they belong to. A GP0 rectangle is always
         * screen-space and never carries GTE output, so admitting the whole
         * range cannot reach world geometry. */
        const unsigned size_sel = (op >> 3) & 3u;
        const int textured = (op >> 2) & 1;
        parse_vertex(words[1], &min_x, &min_y);
        int32_t width, height;
        if (size_sel == 0u) {
            if (textured) {
                width  = (int32_t)(words[3] & 0x3FFu);
                height = (int32_t)((words[3] >> 16) & 0x1FFu);
            } else {
                /* Mirrors gp0_exec_mono_rect: the full 16-bit field, then
                 * clamped to the hardware maximum. */
                width  = (int32_t)(words[2] & 0xFFFFu);
                height = (int32_t)((words[2] >> 16) & 0xFFFFu);
                if (width > 1023) width = 1023;
                if (height > 511)  height = 511;
            }
        } else {
            width = height = size_sel == 1u ? 1 : (size_sel == 2u ? 8 : 16);
        }
        if (width <= 0 || height <= 0) { ws_ui_reject.degenerate++; return; }
        max_x = min_x + width;
        max_y = min_y + height;
    } else {
        ws_ui_reject.opcode++;
        return;
    }

    int32_t width = max_x - min_x, height = max_y - min_y;
    int32_t X = ws_disp_x(), W = ws_disp_w(), H = ws_disp_h();
    /* Anything spanning the display width (to within W/64 of each edge) is a
     * full-width overlay -- letterbox bars, fade strips, menu band rules --
     * not a widget: it widens with the image. Squashed as UI, a cutscene's
     * bars left the revealed margins showing whatever the frame cleared to,
     * and the pause menu's rules (x 2..510 of 512) stopped short of the band
     * they frame (Spider-Man). */
    if ((min_x <= X + W / 64 && max_x >= X + W - W / 64) ||
        (width > W / 2 && height > H / 4)) {
        ws_ui_reject.too_big++;
        return;
    }

    WsUiPrepassItem *item = &ws_ui_prepass[ws_ui_prepass_count++];
    item->group.key =
        ws_auto_ui_group_key_words(words, op, min_y, height);
    item->group.x = min_x - X;
    item->group.width = width;
    item->group.y = min_y;
    item->group.height = height;
    item->group.anchor = 0;
    item->group.root = ws_ui_prepass_count - 1u;
    item->src_addr = GPU_RAM_KEY(source_addr);
    item->ot_rank = rank;
    item->y  = min_y;
    item->h  = height;
    item->op = (uint8_t)op;
    item->full_draw_area = full_draw_area ? 1 : 0;
    item->shared_split_ui = 0;
    item->enclosed_only = (uint8_t)enclosed_only;
    item->packet_guard = ws_prepass_packet_guard(words, word_count);
}

static int ws_ui_untextured_op(uint8_t op) {
    return (op >= 0x28u && op <= 0x2Bu) || (op >= 0x38u && op <= 0x3Bu) ||
           (op >= 0x60u && op <= 0x63u);
}

static int ws_ui_encloses(const WsUiPrepassItem *panel,
                          const WsUiPrepassItem *ui) {
    return panel->group.x <= ui->group.x &&
           ui->group.x + ui->group.width <=
               panel->group.x + panel->group.width &&
           panel->y <= ui->y && ui->y + ui->h <= panel->y + panel->h;
}

/* Part of the same widget: sharing screen columns and overlapping or stacked
 * within WS_UI_GROUP_STACK_GAP rows (ws_ui_group.c same_element()). */
static int ws_ui_attached(const WsUiPrepassItem *a, const WsUiPrepassItem *b) {
    int32_t ax0 = a->group.x, ax1 = a->group.x + a->group.width;
    int32_t bx0 = b->group.x, bx1 = b->group.x + b->group.width;
    if (ax0 >= bx1 || bx0 >= ax1) return 0;
    int32_t gap = a->y + a->h < b->y ? b->y - (a->y + a->h)
                : b->y + b->h < a->y ? a->y - (b->y + b->h) : 0;
    return gap <= WS_UI_GROUP_STACK_GAP;
}

static void bdi_prepass(uint32_t start_addr);   /* backdrop images, below */

void gpu_ws_prepass_linked_list(uint32_t start_addr) {
    {   /* the list's entries: its empty nodes (gpu_ot_front_now) */
        uint32_t a = psx_mod_gpu_dma_resolve_address(start_addr), n = 0;
        for (uint32_t safety = 0; safety < 0x40000u; safety++) {
            const uint32_t h = psx_read_word(a);
            if (((h >> 24) & 0xFFu) == 0) n++;
            if ((h & 0x00FFFFFFu) == 0x00FFFFFFu) break;
            a = psx_mod_gpu_dma_resolve_address(h & 0x00FFFFFFu);
        }
        s_ot_top = n > 0 && n <= 0xFFFFu ? (uint16_t)(n - 1u) : 0xFFFFu;
    }
    bdi_prepass(start_addr);
    ws_ui_prepass_count = 0;
    ws_ui_prepass_nodes_reset();
    ws_ui_prepass_rank = 0xFFFFu;
    ws_auto_ui_dense = 0;
    ws_ui_reject.opcode = ws_ui_reject.not_axis = ws_ui_reject.degenerate =
        ws_ui_reject.too_big = ws_ui_reject.cap = ws_ui_reject.rank =
        ws_ui_reject.stale = ws_ui_reject.backing = ws_ui_reject.enclosed = 0;
    ws_ui_rankdrop_count = 0;
    if (!ws_auto_ui_squash || !ws_active()) return;

    uint32_t addr = psx_mod_gpu_dma_resolve_address(start_addr);
    uint32_t safety = 0;
    uint16_t rank = 0xFFFFu;
    uint16_t max_rank = 0xFFFFu;
    const uint32_t max_nodes = 0x40000u;
    int area_left = (int)draw_area_left, area_right = (int)draw_area_right;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);

    for (;;) {
        if (safety++ > max_nodes) {
            ws_ui_prepass_count = 0;
            ws_ui_prepass_nodes_reset();
            return;
        }
        uint32_t header = psx_read_word(addr);
        uint32_t num_words = (header >> 24) & 0xFFu;
        if (ws_ui_prepass_node_count >= WS_UI_PREPASS_NODE_MAX) {
            ws_ui_reject.cap++;
            ws_ui_prepass_count = 0;
            ws_ui_prepass_nodes_reset();
            return;
        }
        uint32_t payload[255];
        uint32_t first_addr = psx_mod_gpu_dma_resolve_address(addr + 4u);
        for (uint32_t i = 0; i < num_words; i++) {
            payload[i] = psx_read_word(psx_mod_gpu_dma_resolve_address(
                first_addr + i * 4u));
        }
        WsUiPrepassNode *node =
            &ws_ui_prepass_nodes[ws_ui_prepass_node_count++];
        ws_ui_prepass_gen++;
        node->addr = GPU_RAM_KEY(addr);
        node->header = header;
        node->payload_guard =
            ws_prepass_packet_guard(payload, num_words);
        if (num_words == 0) {
            rank = rank == 0xFFFFu ? 0u : (uint16_t)(rank + 1u);
        } else {
            uint32_t word_addr =
                psx_mod_gpu_dma_resolve_address(addr + 4u);
            uint32_t offset = 0;
            while (offset < num_words) {
                uint32_t first = psx_read_word(
                    psx_mod_gpu_dma_resolve_address(
                        word_addr + offset * 4u));
                uint8_t op = (uint8_t)(first >> 24);
                int count = gp0_command_word_count(op);
                if (count <= 0 || offset + (uint32_t)count > num_words)
                    break;
                /* CPU->VRAM data follows its 3-word header and is not a command
                 * stream. Such transfers are not UI draws; stop this node. */
                if (op >= 0xA0u && op <= 0xBFu) break;
                /* Select the frontmost populated drawing layer, including
                 * full-screen fades and non-rectangular world primitives.
                 * Choosing only among UI-shaped candidates promotes a deeper
                 * wall quad to HUD when the real front layer has no widgets. */
                if (op >= 0x20u && op <= 0x7Fu && rank != 0xFFFFu &&
                    (max_rank == 0xFFFFu || rank > max_rank))
                    max_rank = rank;
                uint32_t words[12] = {0};
                for (int i = 0; i < count && i < 12; i++) {
                    words[i] = psx_read_word(
                        psx_mod_gpu_dma_resolve_address(
                            word_addr + (offset + (uint32_t)i) * 4u));
                }
                ws_ui_prepass_add(words, (uint32_t)count,
                    psx_mod_gpu_dma_resolve_address(
                        word_addr + offset * 4u), rank,
                    area_right - area_left + 1 >= (int)di.width);
                if (op == 0xE3u) area_left = (int)(words[0] & 0x3FFu);
                if (op == 0xE4u) area_right = (int)(words[0] & 0x3FFu);
                offset += (uint32_t)count;
            }
        }

        uint32_t next = header & 0xFFFFFFu;
        if (next == 0xFFFFFFu) break;
        addr = psx_mod_gpu_dma_resolve_address(next);
    }
    if (ws_ui_prepass_count == 0) {
        ws_ui_prepass_count = 0;
        return;
    }

    /* Empty trailing OT buckets do not pick the layer. A populated front
     * layer without eligible widgets also must not fall back to a deeper
     * UI-shaped world surface (Ape Escape's intro walls). */
    if (max_rank == 0xFFFFu) {
        ws_ui_reject.rank = ws_ui_prepass_count;
        ws_ui_prepass_count = 0;
        return;
    }
    ws_ui_prepass_rank = max_rank;

    /* HUD pieces one rank back. A HUD often draws parts of a widget one
     * ordering-table rank behind the rest: Spider-Man's banner box (a 0x28
     * quad at rank 365 under rank-366 text), Spider-Man 2's webbing gauge (a
     * textured frame segment and its 0x28 fill at rank 272, stacked on the
     * rank-273 gauge end). Left out, those pieces stretch while the rest of
     * the widget squashes, and the widget comes apart. That rank also carries
     * world geometry, so it is never admitted wholesale. From the next
     * populated rank only, admit an axis-aligned primitive when it
     *   - is untextured and fully encloses an admitted piece (a backing
     *     panel; any size), or
     *   - is small (at most a quarter of the display each way) and overlaps or
     *     stacks directly on an admitted piece (ws_ui_attached).
     * Repeat until nothing changes, so a piece attached to an admitted piece
     * also joins. Admitted pieces then group with their widget's run. */
    uint16_t backing_rank = 0xFFFFu;
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
        uint16_t r = ws_ui_prepass[i].ot_rank;
        if (!ws_ui_prepass[i].enclosed_only && r < max_rank &&
            (backing_rank == 0xFFFFu || r > backing_rank))
            backing_rank = r;
    }
    static uint8_t keep[WS_UI_PREPASS_MAX];
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++)
        keep[i] = ws_ui_prepass[i].ot_rank == max_rank &&
                  !ws_ui_prepass[i].enclosed_only;
    const int32_t small_w = ws_disp_w() / 4, small_h = ws_disp_h() / 4;
    for (int changed = 1; changed && backing_rank != 0xFFFFu;) {
        changed = 0;
        for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
            const WsUiPrepassItem *it = &ws_ui_prepass[i];
            if (keep[i] || it->enclosed_only || it->ot_rank != backing_rank) continue;
            const int small = it->group.width <= small_w && it->h <= small_h;
            const int untextured = ws_ui_untextured_op(it->op);
            for (uint32_t j = 0; j < ws_ui_prepass_count; j++) {
                if (!keep[j]) continue;
                if ((untextured && ws_ui_encloses(it, &ws_ui_prepass[j])) ||
                    (small && ws_ui_attached(it, &ws_ui_prepass[j]))) {
                    keep[i] = 1;
                    changed = 1;
                    ws_ui_reject.backing++;
                    break;
                }
            }
        }
    }

    /* Widget parts that are not axis-aligned rectangles -- a compass needle,
     * a dial hand -- join from the HUD rank when they lie inside the admitted
     * pieces they overlap (the union of their boxes). Left out, they stretch
     * with the wide image while the frame around them squashes. */
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
        const WsUiPrepassItem *it = &ws_ui_prepass[i];
        if (!it->enclosed_only || it->ot_rank != max_rank) continue;
        const int32_t x0 = it->group.x, x1 = it->group.x + it->group.width;
        const int32_t y0 = it->y, y1 = it->y + it->h;
        int32_t ux0 = 0, ux1 = 0, uy0 = 0, uy1 = 0;
        int any = 0;
        for (uint32_t j = 0; j < ws_ui_prepass_count; j++) {
            const WsUiPrepassItem *ui = &ws_ui_prepass[j];
            if (!keep[j] || ui->enclosed_only) continue;
            const int32_t a0 = ui->group.x, a1 = ui->group.x + ui->group.width;
            const int32_t b0 = ui->y, b1 = ui->y + ui->h;
            if (a0 >= x1 || x0 >= a1 || b0 >= y1 || y0 >= b1) continue;
            if (!any) { ux0 = a0; ux1 = a1; uy0 = b0; uy1 = b1; any = 1; }
            else {
                if (a0 < ux0) ux0 = a0;
                if (a1 > ux1) ux1 = a1;
                if (b0 < uy0) uy0 = b0;
                if (b1 > uy1) uy1 = b1;
            }
        }
        if (any && ux0 <= x0 && x1 <= ux1 && uy0 <= y0 && y1 <= uy1) {
            keep[i] = 1;
            ws_ui_reject.enclosed++;
        }
    }

    uint32_t out = 0;
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
        if (keep[i]) {
            ws_ui_prepass[out++] = ws_ui_prepass[i];
        } else if (ws_ui_rankdrop_count < WS_UI_RANKDROP_MAX) {
            const WsUiPrepassItem *it = &ws_ui_prepass[i];
            ws_ui_rankdrop[ws_ui_rankdrop_count].x    = it->group.x;
            ws_ui_rankdrop[ws_ui_rankdrop_count].w    = it->group.width;
            ws_ui_rankdrop[ws_ui_rankdrop_count].y    = it->y;
            ws_ui_rankdrop[ws_ui_rankdrop_count].h    = it->h;
            ws_ui_rankdrop[ws_ui_rankdrop_count].rank = it->ot_rank;
            ws_ui_rankdrop[ws_ui_rankdrop_count].op   = it->op;
            ws_ui_rankdrop_count++;
        }
    }
    ws_ui_reject.rank = ws_ui_prepass_count - out;
    ws_ui_prepass_count = out;
    /*
     * A high final-layer primitive count is a good "dense 2D menu" signal for
     * titles without an explicit gameplay detector, where grouping everything
     * around the centre avoids tearing text grids apart. For GTE-gated 3D
     * titles, though, reaching this path already means a gameplay frame is
     * being stretched. WipEout 3's race HUD is dense enough to trip the old
     * threshold, which pinned every HUD group to the 4:3 centre. Keep edge
     * groups edge-anchored in those frames so the HUD adapts to the wide view.
     */
    ws_auto_ui_dense = ws_ui_prepass_count >= 32u && !ws_gte_game_mode_cfg &&
        !ws_local_native_split();
    if (ws_ui_prepass_count == 0) return;

    const int32_t group_origin = ws_disp_x();
    WsUiGroupItem groups[WS_UI_PREPASS_MAX];
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++)
        groups[i] = ws_ui_prepass[i].group;
    ws_ui_group_assign(groups, ws_ui_prepass_count, ws_disp_w(),
                       ws_auto_ui_dense, ws_auto_ui_in_place);
    for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
        ws_ui_prepass[i].group.anchor = group_origin + groups[i].anchor;
        ws_ui_prepass[i].group.root = groups[i].root;
    }
    /* Vertical anchor per run (auto_ui_size): its top edge in the upper half
     * of the display, its bottom edge in the lower half. */
    {
        static int32_t run_y0[WS_UI_PREPASS_MAX], run_y1[WS_UI_PREPASS_MAX];
        const int32_t half = ws_disp_h() / 2;
        for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
            run_y0[i] = INT32_MAX;
            run_y1[i] = INT32_MIN;
        }
        for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
            const uint32_t r = (uint32_t)groups[i].root;
            if (r >= ws_ui_prepass_count) continue;
            if (groups[i].y < run_y0[r]) run_y0[r] = groups[i].y;
            if (groups[i].y + groups[i].height > run_y1[r])
                run_y1[r] = groups[i].y + groups[i].height;
        }
        for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
            const uint32_t r = (uint32_t)groups[i].root;
            int32_t y0 = groups[i].y, y1 = groups[i].y + groups[i].height;
            if (r < ws_ui_prepass_count && run_y0[r] <= run_y1[r]) {
                y0 = run_y0[r];
                y1 = run_y1[r];
            }
            ws_ui_prepass[i].anchor_y = (y0 + y1) / 2 < half ? y0 : y1;
        }
    }

    /* A shared counter can comprise many glyphs and shadow packets, none of
     * which individually crosses the split. Classify the complete admitted
     * front-layer run, and require full-screen draw provenance. Per-player
     * HUD, world geometry, divider strips and pause menus cannot qualify. */
    if (ws_local_viewport_cfg && ws_mode != 2 && ws_vertical_split_active()) {
        const int32_t W = ws_disp_w(), H = ws_disp_h(), seam = W / 2;
        for (uint32_t i = 0; i < ws_ui_prepass_count; i++) {
            int32_t x0 = W, x1 = 0, y0 = H, y1 = 0;
            int full = 1;
            for (uint32_t j = 0; j < ws_ui_prepass_count; j++) {
                if (groups[j].root != groups[i].root) continue;
                const WsUiGroupItem *g = &groups[j];
                if (g->x < x0) x0 = g->x;
                if (g->x + g->width > x1) x1 = g->x + g->width;
                if (g->y < y0) y0 = g->y;
                if (g->y + g->height > y1) y1 = g->y + g->height;
                full &= ws_ui_prepass[j].full_draw_area;
            }
            if (full && x0 < seam && x1 > seam &&
                x1 - x0 <= W / 2 && y1 - y0 <= H / 4) {
                ws_ui_prepass[i].shared_split_ui = 1;
                ws_ui_prepass[i].group.anchor = group_origin + seam;
            }
        }
    }
}

/* ---- Backdrop images (native-wide; gpu_ws_set_backdrop_images) -------------
 * What a frame draws before its first shaded polygon (the backdrop phase: a
 * cutscene's painted sky, a 2D screen's background), and a picture drawn
 * over the 3D world that covers the screen (a cutscene's still: the moon over
 * the night sky; textured quads and rectangles laid square, the whole width
 * and most of the height), is split into images:
 * the textured primitives of one CLUT, or laid side by side over the same
 * rows; untextured ones that follow each other and touch. An image's
 * primitives whose rows overlap make a band (a sky's top drawn over its
 * panorama in the same palette is another band). A picture whose bands all
 * stop at the screen's edges (a menu's background in tiles) is decided as a
 * whole; when one of its bands runs past the wide edge (a panorama), each
 * band is decided on its own. Each image, or band, on each side where the
 * wide view shows more than the picture:
 *   - reaching past the wide edge: drawn as it is (a panorama's own pixels);
 *   - ending inside the revealed band (at the picture's edge, or a panorama
 *     at the end of its pan): an edge that is mostly one colour goes on into
 *     the band, each row in the edge's colour on that row (the margin filled
 *     at the image's place in the draw order, so what is drawn after it
 *     stays on top); an edge with art
 *     stretches the image about the picture's centre by just what it needs;
 *   - short of the picture's edge: left alone.
 * Decided by a pass over the linked list before any of it is drawn; render
 * only (guest VRAM and the 4:3 picture untouched). */
#define BDI_SLOTS   2048u                  /* primitive lookup (power of two) */
#define BDI_PRIMS   512
#define BDI_IMAGES  128
#define BDI_SAMPLES 96
typedef struct {
    uint8_t op;
    uint16_t tpage, clut;
    int n;                                 /* vertices */
    int32_t x[4], y[4];
    int u[4], v[4];
    uint32_t col[4];
    int img;                               /* its band (after the split; before: its image) */
    uint32_t key;
} BdiPrim;
typedef struct {
    int32_t x0, x1, y0, y1;                /* raw drawing space */
    uint32_t first_key;
    uint8_t textured, semi;
    uint16_t clut;
    uint8_t q;                             /* stretch: 0 none, 1..255 of the full one */
    uint8_t fill_sides, filled;
    uint16_t fill_col[2];
    int16_t parent;                        /* a band's image */
    uint8_t wrap_sides;                    /* sides where tiles seen beyond its end are drawn */
    uint8_t late;                          /* drawn over the 3D world (not a painted backdrop) */
    uint8_t edges;                         /* a still: only its pieces at the sides stretched */
    uint16_t dbg_edge[2][4];               /* TCP: per side samples, within 1, within 2, mode */
} BdiImage;
static int s_bdi_env = -1;                 /* PSX_NW_BACKDROP_IMAGES: 0 / 1 wins, 2 none */
static int s_bdi_on = 0;                   /* gpu_ws_set_backdrop_images */
static int s_bdi_valid = 0;
static uint32_t s_bdi_key[BDI_SLOTS];
static int16_t s_bdi_slot_img[BDI_SLOTS];
static BdiPrim s_bdi_prim[BDI_PRIMS];
static BdiImage s_bdi_img[BDI_IMAGES];     /* bands (after the split) */
static BdiImage s_bdi_par[BDI_IMAGES];     /* the images the bands come from */
static int s_bdi_nprim, s_bdi_nimg, s_bdi_npar;
#define BDI_RUNS 2048
typedef struct {                           /* a filled side's rows of one colour */
    int16_t img;
    uint8_t right;
    int32_t y0, y1;                        /* raw drawing space, end exclusive */
    uint16_t col;
} BdiRun;
static BdiRun s_bdi_run[BDI_RUNS];
static int s_bdi_nrun;
/* A looping panorama of tiles (a sky the game lays out again every tile it
 * scrolls) leaves, after each relayout, a gap at one end that 4:3 never
 * shows: which tile was seen beside which is remembered, and the tile seen
 * beyond the end is drawn in the gap at its own size. */
typedef struct {
    uint16_t tpage, clut;
    int16_t u, v, w, h;
} BdiTile;
typedef struct {
    BdiTile t, nb[2];                      /* the tile seen to its left, to its right */
    uint8_t has[2];
    uint32_t seen;
} BdiTileMem;
#define BDI_TILE_MEM 32
static BdiTileMem s_bdi_tiles[BDI_TILE_MEM];
typedef struct {                           /* a tile drawn in a band's gap */
    int16_t img;
    uint8_t op;
    int32_t x, y;
    BdiTile t;
    uint32_t col;
} BdiWrap;
#define BDI_WRAPS 16
static BdiWrap s_bdi_wrap[BDI_WRAPS];
static int s_bdi_nwrap;
static uint32_t s_bdi_stat[4];             /* images: as is, filled, stretched, prims looked up */

void gpu_ws_set_backdrop_images(int on) { s_bdi_on = on ? 1 : 0; }

static int bdi_enabled(void) {
    if (s_bdi_env < 0) {
        const char *e = getenv("PSX_NW_BACKDROP_IMAGES");
        s_bdi_env = e && (e[0] == '0' || e[0] == '1') ? e[0] - '0' : 2;
    }
    return s_bdi_env == 2 ? s_bdi_on : s_bdi_env;
}

static int bdi_find(uint32_t cmd_addr) {
    if (!s_bdi_valid || cmd_addr == 0xFFFFFFFFu) return -1;
    const uint32_t key = GPU_RAM_KEY(cmd_addr);
    for (uint32_t i = 0, h = (key >> 2) * 2654435761u >> 21; i < 16; i++) {
        const uint32_t s = (h + i) & (BDI_SLOTS - 1u);
        if (s_bdi_slot_img[s] < 0) return -1;
        if (s_bdi_key[s] == key) return s_bdi_slot_img[s];
    }
    return -1;
}

static void bdi_insert(uint32_t key, int img) {
    for (uint32_t i = 0, h = (key >> 2) * 2654435761u >> 21; i < 16; i++) {
        const uint32_t s = (h + i) & (BDI_SLOTS - 1u);
        if (s_bdi_slot_img[s] < 0 || s_bdi_key[s] == key) {
            s_bdi_key[s] = key;
            s_bdi_slot_img[s] = (int16_t)img;
            return;
        }
    }
}

/* One texel as BGR555 with its stp bit, 0 = transparent. */
static uint16_t bdi_texel(uint16_t tpage, uint16_t clut, int u, int v) {
    const int depth = (tpage >> 7) & 3, bx = (tpage & 15) * 64, by = ((tpage >> 4) & 1) * 256;
    const int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
    u &= 255; v &= 255;
    if (depth == 0)
        return vram[cy * 1024 + ((cx + ((vram[(by + v) * 1024 + ((bx + (u >> 2)) & 1023)] >> ((u & 3) * 4)) & 0xF)) & 1023)];
    if (depth == 1)
        return vram[cy * 1024 + ((cx + ((vram[(by + v) * 1024 + ((bx + (u >> 1)) & 1023)] >> ((u & 1) * 8)) & 0xFF)) & 1023)];
    return vram[(by + v) * 1024 + ((bx + u) & 1023)];
}

/* A sample's colour (BGR555, no stp) as drawn: the texel times the vertex
 * colour (0x80 = 1.0) unless raw; untextured: the vertex colour. */
static int bdi_colour(const BdiPrim *p, int u, int v, uint32_t col, uint16_t *out) {
    int r, g, b;
    if (p->op & 0x04u) {
        const uint16_t t = bdi_texel(p->tpage, p->clut, u, v);
        if (!t) return 0;                   /* transparent */
        r = t & 31; g = (t >> 5) & 31; b = (t >> 10) & 31;
        if (!(p->op & 0x01u)) {
            r = r * (int)(col & 0xFF) / 128; g = g * (int)((col >> 8) & 0xFF) / 128;
            b = b * (int)((col >> 16) & 0xFF) / 128;
            if (r > 31) r = 31; if (g > 31) g = 31; if (b > 31) b = 31;
        }
    } else {
        r = (int)(col & 0xFF) >> 3; g = (int)((col >> 8) & 0xFF) >> 3; b = (int)((col >> 16) & 0xFF) >> 3;
    }
    *out = (uint16_t)(r | g << 5 | b << 10);
    return 1;
}

/* One side of a band (whole: of an image, all its bands): its edge (the
 * primitives at that extreme, each by the two vertices furthest that way: a
 * slanted edge, a perspective plane whose top row is narrower, counts by its
 * innermost point) and the strip along it, the outermost texels and a few
 * texels in. *edge: that innermost x of the edge (left: the largest, right:
 * the smallest). */
static int bdi_side_samples(int img, int whole, int right, uint16_t *smp, int32_t *edge) {
    const BdiImage *im = whole ? &s_bdi_par[img] : &s_bdi_img[img];
    int n = 0;
    *edge = right ? im->x1 : im->x0;
    for (int i = 0; i < s_bdi_nprim; i++) {
        const BdiPrim *p = &s_bdi_prim[i];
        if ((whole ? s_bdi_img[p->img].parent : p->img) != img || p->n < 2) continue;
        /* a, b: the two vertices furthest to that side */
        int a = 0;
        for (int k = 1; k < p->n; k++) if (right ? p->x[k] > p->x[a] : p->x[k] < p->x[a]) a = k;
        int b = a == 0 ? 1 : 0;
        for (int k = 0; k < p->n; k++) if (k != a && (right ? p->x[k] > p->x[b] : p->x[k] < p->x[b])) b = k;
        if (right ? p->x[a] < im->x1 - 2 : p->x[a] > im->x0 + 2) continue;   /* not at that extreme */
        if (right ? p->x[b] < *edge : p->x[b] > *edge) *edge = p->x[b];
        if (n >= BDI_SAMPLES - 16) continue;
        int o = -1;   /* a vertex off the edge, for the texels in */
        for (int k = 0; k < p->n; k++) if (k != a && k != b) { o = k; break; }
        const int du = o >= 0 && p->u[o] != p->u[a] ? (p->u[o] > p->u[a] ? 4 : -4) : 0;
        for (int s = 0; s < 8; s++) {
            const float t = (s + 0.5f) / 8.0f;
            const int u = p->u[a] + (int)((p->u[b] - p->u[a]) * t), v = p->v[a] + (int)((p->v[b] - p->v[a]) * t);
            const uint32_t col = p->col[a];
            if (bdi_colour(p, u, v, col, &smp[n])) n++;
            if (du && bdi_colour(p, u + du, v, col, &smp[n])) n++;
        }
    }
    return n;
}

/* The colour most samples sit within `tol` levels of; *count how many do. */
static int bdi_mode(const uint16_t *smp, int n, int tol, int *count) {
    int best = -1, best_n = 0;
    for (int i = 0; i < n; i++) {
        const int r = smp[i] & 31, g = (smp[i] >> 5) & 31, b = (smp[i] >> 10) & 31;
        int c = 0;
        for (int j = 0; j < n; j++) {
            const int dr = (smp[j] & 31) - r, dg = ((smp[j] >> 5) & 31) - g, db = ((smp[j] >> 10) & 31) - b;
            if (dr <= tol && dr >= -tol && dg <= tol && dg >= -tol && db <= tol && db >= -tol) c++;
        }
        if (c > best_n) { best_n = c; best = i; }
    }
    *count = best_n;
    return best;
}

/* A colour's brightest channel at most `lv` levels (of 31). */
static int bdi_dark(uint16_t c, int lv) {
    return (c & 31) <= lv && ((c >> 5) & 31) <= lv && ((c >> 10) & 31) <= lv;
}

/* Mostly one colour: the colour at least 90% of the samples sit within 1
 * level of (a lighter edge with a faint pattern is art, not a flat colour);
 * a very dark edge (a night sky with faint clouds: up to 4 levels), within
 * 2. */
static int bdi_flat(const uint16_t *smp, int n, uint16_t *mode) {
    if (n < 6) return 0;
    int c, best = bdi_mode(smp, n, 1, &c);
    if (best >= 0 && c * 100 >= n * 90) { *mode = smp[best]; return 1; }
    best = bdi_mode(smp, n, 2, &c);
    if (best < 0 || c * 100 < n * 90 || !bdi_dark(smp[best], 4)) return 0;
    *mode = smp[best];
    return 1;
}

static void bdi_prim_parse(const uint32_t *w, uint8_t op, uint16_t tpage, BdiPrim *p) {
    p->op = op; p->tpage = tpage; p->clut = 0; p->n = 0;
    if (op >= 0x20u && op <= 0x3Fu) {
        const int quad = (op & 0x08u) != 0, tex = (op & 0x04u) != 0, gour = (op & 0x10u) != 0;
        int idx = 1;   /* w[0]: command and first colour; then per vertex [colour], xy, [uv] */
        p->n = quad ? 4 : 3;
        uint32_t col = w[0] & 0xFFFFFFu;
        for (int i = 0; i < p->n; i++) {
            if (i > 0 && gour) col = w[idx++] & 0xFFFFFFu;
            p->col[i] = col;
            p->x[i] = sign_extend(w[idx] & 0x7FFu, 11);
            p->y[i] = sign_extend((w[idx] >> 16) & 0x7FFu, 11);
            idx++;
            if (tex) {
                p->u[i] = (int)(w[idx] & 0xFF); p->v[i] = (int)((w[idx] >> 8) & 0xFF);
                if (i == 0) p->clut = (uint16_t)(w[idx] >> 16);
                if (i == 1) p->tpage = (uint16_t)(w[idx] >> 16);
                idx++;
            } else {
                p->u[i] = p->v[i] = 0;
            }
        }
        return;
    }
    /* rectangle: a quad with the texture laid flat */
    const int tex = (op & 0x04u) != 0, sz = (op >> 3) & 3;
    const int32_t x = sign_extend(w[1] & 0x7FFu, 11), y = sign_extend((w[1] >> 16) & 0x7FFu, 11);
    int ww, hh;
    if (sz == 0) { const uint32_t wh = w[tex ? 3 : 2]; ww = (int)(wh & 0x3FF); hh = (int)((wh >> 16) & 0x1FF); }
    else ww = hh = sz == 1 ? 1 : sz == 2 ? 8 : 16;
    const int u = tex ? (int)(w[2] & 0xFF) : 0, v = tex ? (int)((w[2] >> 8) & 0xFF) : 0;
    if (tex) p->clut = (uint16_t)(w[2] >> 16);
    p->n = 4;
    for (int i = 0; i < 4; i++) {
        p->x[i] = x + ((i & 1) ? ww - 1 : 0);
        p->y[i] = y + ((i & 2) ? hh - 1 : 0);
        p->u[i] = u + ((i & 1) ? ww - 1 : 0);
        p->v[i] = v + ((i & 2) ? hh - 1 : 0);
        p->col[i] = w[0] & 0xFFFFFFu;
    }
}

static void bdi_prim_rows(const BdiPrim *p, int32_t *y0, int32_t *y1) {
    *y0 = *y1 = p->y[0];
    for (int k = 1; k < p->n; k++) {
        if (p->y[k] < *y0) *y0 = p->y[k];
        if (p->y[k] > *y1) *y1 = p->y[k];
    }
}

/* Each image into bands: its primitives whose rows overlap (by more than a
 * row: pieces stacked edge to edge are apart). The first band keeps the
 * image's slot; the lookup then gives each primitive its band. */
static void bdi_split_bands(void) {
    memcpy(s_bdi_par, s_bdi_img, sizeof(BdiImage) * (size_t)s_bdi_nimg);
    s_bdi_npar = s_bdi_nimg;
    static int16_t order[BDI_PRIMS];
    static int32_t py0[BDI_PRIMS], py1[BDI_PRIMS];
    for (int i = 0; i < s_bdi_nprim; i++) bdi_prim_rows(&s_bdi_prim[i], &py0[i], &py1[i]);
    for (int par = 0; par < s_bdi_npar; par++) {
        int n = 0;
        for (int i = 0; i < s_bdi_nprim; i++)
            if (s_bdi_prim[i].img == par) {
                int k = n++;   /* by first row */
                while (k > 0 && py0[order[k - 1]] > py0[i]) { order[k] = order[k - 1]; k--; }
                order[k] = (int16_t)i;
            }
        int band = -1;
        int32_t band_y1 = 0;
        for (int k = 0; k < n; k++) {
            const int i = order[k];
            if (band < 0 || py0[i] >= band_y1 - 1) {
                if (band < 0) band = par;
                else if (s_bdi_nimg < BDI_IMAGES) band = s_bdi_nimg++;
                BdiImage *b = &s_bdi_img[band];
                *b = s_bdi_par[par];
                b->parent = (int16_t)par;
                b->x0 = INT32_MAX; b->x1 = INT32_MIN; b->y0 = py0[i]; b->y1 = py1[i];
                b->semi = 0;
                band_y1 = py1[i];
            }
            BdiImage *b = &s_bdi_img[band];
            const BdiPrim *p = &s_bdi_prim[i];
            for (int v = 0; v < p->n; v++) {
                if (p->x[v] < b->x0) b->x0 = p->x[v];
                if (p->x[v] > b->x1) b->x1 = p->x[v];
            }
            if (py1[i] > b->y1) b->y1 = py1[i];
            if (py1[i] > band_y1) band_y1 = py1[i];
            b->semi |= (uint8_t)((p->op & 0x02u) != 0);
            s_bdi_prim[i].img = band;
        }
    }
    /* each band's first primitive in draw order; the lookup by band */
    for (int i = s_bdi_nprim - 1; i >= 0; i--) s_bdi_img[s_bdi_prim[i].img].first_key = s_bdi_prim[i].key;
    for (int i = 0; i < s_bdi_nprim; i++) bdi_insert(s_bdi_prim[i].key, s_bdi_prim[i].img);
}

static int bdi_tile_of(const BdiPrim *p, BdiTile *t) {
    if (p->op < 0x64u || p->op > 0x7Fu || !(p->op & 0x04u)) return 0;   /* textured rectangles */
    t->tpage = (uint16_t)(p->tpage & 0x1FFu); t->clut = p->clut;
    t->u = (int16_t)p->u[0]; t->v = (int16_t)p->v[0];
    t->w = (int16_t)(p->x[1] - p->x[0] + 1); t->h = (int16_t)(p->y[2] - p->y[0] + 1);
    return 1;
}

static int bdi_tile_eq(const BdiTile *a, const BdiTile *b) {
    return a->tpage == b->tpage && a->clut == b->clut && a->u == b->u && a->v == b->v &&
           a->w == b->w && a->h == b->h;
}

/* The tile's memory (created: one fresh in the oldest's place); to look one
 * up, only one seen in the last BDI_TILE_FRESH frames (another picture may
 * later use the same texture place). */
#define BDI_TILE_FRESH 240u
static BdiTileMem *bdi_tile_mem(const BdiTile *t, int create) {
    BdiTileMem *old = &s_bdi_tiles[0];
    for (int i = 0; i < BDI_TILE_MEM; i++) {
        BdiTileMem *mm = &s_bdi_tiles[i];
        if (mm->seen && bdi_tile_eq(&mm->t, t)) {
            if (!create && (uint32_t)s_frame_count + 1u - mm->seen > BDI_TILE_FRESH) return NULL;
            return mm;
        }
        if (mm->seen < old->seen) old = mm;
    }
    if (!create) return NULL;
    memset(old, 0, sizeof *old);
    old->t = *t;
    return old;
}

/* A band laid as one row of textured rectangles: their indices by x. */
static int bdi_strip(int img, int16_t *order, int cap) {
    int n = 0;
    for (int i = 0; i < s_bdi_nprim; i++) {
        const BdiPrim *p = &s_bdi_prim[i];
        if (p->img != img) continue;
        BdiTile t;
        if (!bdi_tile_of(p, &t) || n >= cap) return 0;
        if (n && (p->y[0] != s_bdi_prim[order[0]].y[0] || p->y[2] != s_bdi_prim[order[0]].y[2])) return 0;
        int k = n++;
        while (k > 0 && s_bdi_prim[order[k - 1]].x[0] > p->x[0]) { order[k] = order[k - 1]; k--; }
        order[k] = (int16_t)i;
    }
    return n;
}

/* Each strip's neighbouring tiles, remembered. */
static void bdi_tiles_learn(void) {
    for (int b = 0; b < s_bdi_nimg; b++) {
        int16_t order[16];
        const int n = bdi_strip(b, order, 16);
        for (int k = 0; k + 1 < n; k++) {
            const BdiPrim *a = &s_bdi_prim[order[k]], *c = &s_bdi_prim[order[k + 1]];
            BdiTile ta, tc;
            bdi_tile_of(a, &ta); bdi_tile_of(c, &tc);
            if (c->x[0] != a->x[0] + ta.w) continue;   /* not side by side */
            BdiTileMem *ma = bdi_tile_mem(&ta, 1);
            ma->nb[1] = tc; ma->has[1] = 1; ma->seen = (uint32_t)s_frame_count + 1u;
            BdiTileMem *mc = bdi_tile_mem(&tc, 1);
            mc->nb[0] = ta; mc->has[0] = 1; mc->seen = (uint32_t)s_frame_count + 1u;
        }
    }
}

/* A strip's side short of the wide edge: the tiles seen beyond its end
 * (one after another, up to 3) planned there; 0 (nothing planned) when they
 * are not remembered or do not reach the wide edge. */
static int bdi_wrap_side(int img, int right, int32_t X, int32_t W, int32_t m) {
    int16_t order[16];
    const int n = bdi_strip(img, order, 16);
    if (n < 1) return 0;
    const BdiPrim *end = &s_bdi_prim[order[right ? n - 1 : 0]];
    BdiTile t;
    bdi_tile_of(end, &t);
    int32_t x = end->x[0];
    const int first = s_bdi_nwrap;
    for (int k = 0; k < 3 && s_bdi_nwrap < BDI_WRAPS; k++) {
        const BdiTileMem *mm = bdi_tile_mem(&t, 0);
        if (!mm || !mm->has[right]) break;
        const BdiTile nb = mm->nb[right];
        x = right ? x + t.w : x - nb.w;
        s_bdi_wrap[s_bdi_nwrap++] = (BdiWrap){ (int16_t)img, end->op, x, end->y[0], nb, end->col[0] };
        if (right ? x + nb.w >= X + W + m : x <= X - m) return 1;
        t = nb;
    }
    s_bdi_nwrap = first;
    return 0;
}

/* The narrowest piece at a side of a band (whole: of an image). */
static int32_t bdi_side_piece_w(int img, int whole, int right) {
    const BdiImage *im = whole ? &s_bdi_par[img] : &s_bdi_img[img];
    int32_t best = INT32_MAX;
    for (int i = 0; i < s_bdi_nprim; i++) {
        const BdiPrim *p = &s_bdi_prim[i];
        if ((whole ? s_bdi_img[p->img].parent : p->img) != img || p->n < 2) continue;
        int32_t lo = p->x[0], hi = p->x[0];
        for (int k = 1; k < p->n; k++) { if (p->x[k] < lo) lo = p->x[k]; if (p->x[k] > hi) hi = p->x[k]; }
        if (right ? hi < im->x1 - 2 : lo > im->x0 + 2) continue;
        if (hi - lo < best) best = hi - lo;
    }
    return best;
}

static void bdi_drop_wraps(int img) {
    int k = 0;
    for (int i = 0; i < s_bdi_nwrap; i++)
        if (s_bdi_wrap[i].img != img) s_bdi_wrap[k++] = s_bdi_wrap[i];
    s_bdi_nwrap = k;
    s_bdi_img[img].wrap_sides = 0;
}

/* One band (whole: an image, all its bands together): its stretch, filled
 * sides and (a band) tiles drawn in its gaps into *im. Returns whether a
 * side reaches past the wide edge (the picture's own pixels there: a
 * panorama). */
static int bdi_decide(int img, int whole, BdiImage *im) {
    const BdiImage *src = whole ? &s_bdi_par[img] : &s_bdi_img[img];
    const int32_t X = ws_disp_x(), W = ws_disp_w(), m = ws_nw_configured_offset();
    const float c = (float)X + (float)W * 0.5f, half = (float)W * 0.5f + (float)m;
    const float full = half / ((float)W * 0.5f);
    float s = 1.0f;
    int wide = 0, edges = src->late;   /* a still's pieces at the sides wide enough to stretch alone */
    im->q = 0;
    im->fill_sides = 0;
    im->wrap_sides = 0;
    im->edges = 0;
    for (int right = 0; right < 2; right++) {
        uint16_t smp[BDI_SAMPLES], mode;
        int32_t edge;
        const int n = bdi_side_samples(img, whole, right, smp, &edge);
        const int reach = right ? edge >= X + W - 1 : edge <= X + 1;
        const int32_t beyond = right ? edge - (X + W) : X - edge;
        if (!reach) continue;
        if (beyond >= m) { wide = 1; continue; }
        {
            int c1, c2;
            const int b1 = bdi_mode(smp, n, 1, &c1), b2 = bdi_mode(smp, n, 2, &c2);
            im->dbg_edge[right][0] = (uint16_t)n; im->dbg_edge[right][1] = (uint16_t)c1;
            im->dbg_edge[right][2] = (uint16_t)c2;
            im->dbg_edge[right][3] = b2 >= 0 ? smp[b2] : (b1 >= 0 ? smp[b1] : 0);
        }
        if (!src->semi && bdi_flat(smp, n, &mode)) {
            im->fill_sides |= (uint8_t)(1 << right);
            im->fill_col[right] = mode;
            continue;
        }
        if (!whole && bdi_wrap_side(img, right, X, W, m)) {
            im->wrap_sides |= (uint8_t)(1 << right);
            continue;
        }
        const float d = right ? (float)edge - c : c - (float)edge;
        const float need = d > 1.0f ? half / d : full;
        if (need > s) s = need;
        if (bdi_side_piece_w(img, whole, right) * 2 < m * 3) edges = 0;
    }
    if (s > 1.0005f && full > 1.0005f) {
        const int q = (int)((s - 1.0f) / (full - 1.0f) * 255.0f + 0.5f);
        im->q = (uint8_t)(q < 1 ? 1 : q > 255 ? 255 : q);
        im->fill_sides = 0;   /* the stretch (about the centre) covers both sides */
        if (!whole && im->wrap_sides) bdi_drop_wraps(img);
        /* a still over the 3D world (the moon in a night sky, framed by wide
         * pieces of sky): only the pieces at its sides go out to the wide
         * edges, so what it shows in the middle keeps its shape */
        if (edges) { im->edges = 1; im->q = 0; }
    }
    return wide;
}

/* A filled side, row by row: the colour at the edge on that row (a sky's
 * gradient goes on into the margin with no seam), the side's main colour
 * where no primitive is at the edge; kept as runs of one colour. */
static void bdi_fill_runs(int img, int right) {
    const BdiImage *im = &s_bdi_img[img];
    static uint16_t row[1024];
    const int32_t y0 = im->y0, h = im->y1 - im->y0 + 1;
    if (h <= 0 || h > 1024) return;
    for (int y = 0; y < h; y++) row[y] = im->fill_col[right];
    for (int i = 0; i < s_bdi_nprim; i++) {   /* in draw order: the last one on top */
        const BdiPrim *p = &s_bdi_prim[i];
        if (p->img != img || p->n < 2) continue;
        int a = 0;
        for (int k = 1; k < p->n; k++) if (right ? p->x[k] > p->x[a] : p->x[k] < p->x[a]) a = k;
        int b = a == 0 ? 1 : 0;
        for (int k = 0; k < p->n; k++) if (k != a && (right ? p->x[k] > p->x[b] : p->x[k] < p->x[b])) b = k;
        if (right ? p->x[a] < im->x1 - 2 : p->x[a] > im->x0 + 2) continue;   /* not at that extreme */
        if (p->y[a] == p->y[b]) continue;
        const int lo = p->y[a] < p->y[b] ? a : b, hi = lo == a ? b : a;
        for (int32_t y = p->y[lo]; y <= p->y[hi]; y++) {
            if (y < y0 || y >= y0 + h) continue;
            const float t = ((float)(y - p->y[lo]) + 0.5f) / (float)(p->y[hi] - p->y[lo] + 1);
            const int u = p->u[lo] + (int)((float)(p->u[hi] - p->u[lo]) * t);
            const int v = p->v[lo] + (int)((float)(p->v[hi] - p->v[lo]) * t);
            uint32_t col = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                const float c0 = (float)((p->col[lo] >> sh) & 0xFF), c1 = (float)((p->col[hi] >> sh) & 0xFF);
                col |= (uint32_t)(c0 + (c1 - c0) * t + 0.5f) << sh;
            }
            uint16_t c;
            if (bdi_colour(p, u, v, col, &c)) row[y - y0] = c;
        }
    }
    for (int y = 0; y < h && s_bdi_nrun < BDI_RUNS;) {
        int e = y + 1;
        while (e < h && row[e] == row[y]) e++;
        s_bdi_run[s_bdi_nrun++] = (BdiRun){ (int16_t)img, (uint8_t)right, y0 + y, y0 + e, row[y] };
        y = e;
    }
}

/* A primitive laid square: a rectangle, or a quad whose corners sit on two
 * columns and two rows (to a pixel). */
static int bdi_square(const BdiPrim *p, int32_t x0, int32_t x1, int32_t y0, int32_t y1) {
    if (p->op >= 0x60u) return 1;
    if (p->n != 4) return 0;
    for (int k = 0; k < 4; k++)
        if ((abs(p->x[k] - x0) > 1 && abs(p->x[k] - x1) > 1) || (abs(p->y[k] - y0) > 1 && abs(p->y[k] - y1) > 1))
            return 0;
    return 1;
}

/* A polygon with a corner the GTE projected (PGXP's dataflow shadow of its
 * packet word): 3D world, not a still. 0 when PGXP is not tracking. */
static int bdi_gte_polygon(const uint32_t *w, uint8_t op, uint32_t cmd_addr) {
    if (op > 0x3Fu) return 0;
    const int n = op & 0x08u ? 4 : 3, tex = (op & 0x04u) != 0, gour = (op & 0x10u) != 0;
    int idx = 1;
    for (int i = 0; i < n; i++) {
        if (i > 0 && gour) idx++;
        int32_t x, y;
        parse_vertex(w[idx], &x, &y);
        if (pgxp_probe_precise_vertex(cmd_addr + (uint32_t)idx * 4u, w[idx], x, y) == PGXP_SRC_DATAFLOW)
            return 1;
        idx += tex ? 2 : 1;
    }
    return 0;
}

static void bdi_prepass(uint32_t start_addr) {
    s_bdi_valid = 0;
    if (!bdi_enabled() || ws_mode != 2 || !ws_native_wide_configured()) return;
    s_bdi_nprim = s_bdi_nimg = 0;
    memset(s_bdi_slot_img, 0xFF, sizeof s_bdi_slot_img);
    const int32_t X = ws_disp_x(), W = ws_disp_w(), H = ws_disp_h();
    uint16_t tpage = current_texpage();
    uint32_t addr = psx_mod_gpu_dma_resolve_address(start_addr);
    int late = 0, nlate = 0;   /* past the backdrop phase; its pieces kept */
    for (uint32_t safety = 0; safety < 0x40000u; safety++) {
        const uint32_t header = psx_read_word(addr);
        const uint32_t num_words = (header >> 24) & 0xFFu;
        const uint32_t word_addr = psx_mod_gpu_dma_resolve_address(addr + 4u);
        uint32_t offset = 0;
        while (offset < num_words) {
            const uint32_t cmd_addr = word_addr + offset * 4u;
            const uint32_t first = psx_read_word(psx_mod_gpu_dma_resolve_address(cmd_addr));
            const uint8_t op = (uint8_t)(first >> 24);
            const int count = gp0_command_word_count(op);
            if (count <= 0 || offset + (uint32_t)count > num_words) break;
            if (op >= 0xA0u && op <= 0xBFu) break;
            uint32_t w[12] = {0};
            for (int i = 0; i < count && i < 12; i++)
                w[i] = psx_read_word(psx_mod_gpu_dma_resolve_address(cmd_addr + (uint32_t)i * 4u));
            offset += (uint32_t)count;
            if (op == 0xE1u) { tpage = (uint16_t)(w[0] & 0x1FFu); continue; }
            if (op < 0x20u || op > 0x7Fu || (op >= 0x40u && op <= 0x5Fu)) continue;
            BdiPrim spare, *p = s_bdi_nprim < BDI_PRIMS ? &s_bdi_prim[s_bdi_nprim] : &spare;
            bdi_prim_parse(w, op, tpage, p);
            if ((op & 0x24u) == 0x24u && op <= 0x3Fu) tpage = (uint16_t)(p->tpage & 0x1FFu);
            int32_t x0 = p->x[0], x1 = p->x[0], y0 = p->y[0], y1 = p->y[0];
            for (int k = 1; k < p->n; k++) {
                if (p->x[k] < x0) x0 = p->x[k]; if (p->x[k] > x1) x1 = p->x[k];
                if (p->y[k] < y0) y0 = p->y[k]; if (p->y[k] > y1) y1 = p->y[k];
            }
            /* the phase ends at the first shaded polygon, but the backdrop's own
             * untextured gradient from one edge exactly to the other */
            if (!late && op >= 0x30u && op <= 0x3Fu &&
                !(!(op & 0x04u) && x0 <= X && x0 >= X - 2 && x1 >= X + W && x1 <= X + W + 2))
                late = 1;
            /* over the 3D world: only big textured pieces laid square (a still's) */
            if (late && (!(op & 0x04u) || x1 - x0 < 16 || y1 - y0 < 4 || nlate >= 128 ||
                         !bdi_square(p, x0, x1, y0, y1) || bdi_gte_polygon(w, op, cmd_addr)))
                continue;
            if (s_bdi_nprim >= BDI_PRIMS) continue;
            nlate += late;
            const int tex = (op & 0x04u) != 0, semi = (op & 0x02u) != 0;
            /* textured: the image of its CLUT (a backdrop's tiles come in any
             * order, left and right in turn), else the image it continues as a
             * tile beside it (same rows: a panorama's tiles each have their own
             * CLUT); untextured: the last image, when untextured and touching */
            int img = -1;
            if (tex) {
                const int direct = ((p->tpage >> 7) & 3) >= 2;
                for (int k = 0; k < s_bdi_nimg && img < 0; k++)
                    if (s_bdi_img[k].textured && s_bdi_img[k].late == late && !direct &&
                        s_bdi_img[k].clut == p->clut)
                        img = k;
                for (int k = 0; k < s_bdi_nimg && img < 0; k++) {
                    const BdiImage *im = &s_bdi_img[k];
                    if (im->textured && im->late == late && abs(y0 - im->y0) <= 1 && abs(y1 - im->y1) <= 1 &&
                        x0 <= im->x1 + 2 && x1 >= im->x0 - 2)
                        img = k;
                }
            } else if (s_bdi_nimg > 0) {
                const BdiImage *im = &s_bdi_img[s_bdi_nimg - 1];
                if (!im->textured && !im->late && x0 <= im->x1 + 2 && x1 >= im->x0 - 2 &&
                    y0 <= im->y1 + 2 && y1 >= im->y0 - 2)
                    img = s_bdi_nimg - 1;
            }
            if (img < 0) {
                if (s_bdi_nimg >= BDI_IMAGES) continue;
                img = s_bdi_nimg++;
                BdiImage *im = &s_bdi_img[img];
                memset(im, 0, sizeof *im);
                im->x0 = x0; im->x1 = x1; im->y0 = y0; im->y1 = y1;
                im->first_key = GPU_RAM_KEY(cmd_addr);
                im->textured = (uint8_t)tex; im->clut = p->clut;
                im->late = (uint8_t)late;
            }
            BdiImage *im = &s_bdi_img[img];
            if (x0 < im->x0) im->x0 = x0; if (x1 > im->x1) im->x1 = x1;
            if (y0 < im->y0) im->y0 = y0; if (y1 > im->y1) im->y1 = y1;
            im->semi |= (uint8_t)semi;
            p->img = img;
            p->key = GPU_RAM_KEY(cmd_addr);
            s_bdi_nprim++;
        }
        if ((header & 0x00FFFFFFu) == 0x00FFFFFFu) break;
        addr = psx_mod_gpu_dma_resolve_address(header & 0x00FFFFFFu);
    }
    /* a picture over the 3D world stays only when it covers the screen (the
     * whole width, its pieces filling 3/4 of it: not a banner and a ribbon
     * of one palette at two corners) in a few pieces (a floor seen from
     * above, square on the screen, is many) */
    {
        int remap[BDI_IMAGES], count[BDI_IMAGES] = {0}, nk = 0, np = 0;
        int64_t area[BDI_IMAGES] = {0};
        for (int i = 0; i < s_bdi_nprim; i++) {
            const BdiPrim *p = &s_bdi_prim[i];
            int32_t x0 = p->x[0], x1 = p->x[0], y0 = p->y[0], y1 = p->y[0];
            for (int k = 1; k < p->n; k++) {
                if (p->x[k] < x0) x0 = p->x[k]; if (p->x[k] > x1) x1 = p->x[k];
                if (p->y[k] < y0) y0 = p->y[k]; if (p->y[k] > y1) y1 = p->y[k];
            }
            count[p->img]++;
            area[p->img] += (int64_t)(x1 - x0) * (y1 - y0);
        }
        for (int i = 0; i < s_bdi_nimg; i++) {
            const BdiImage *im = &s_bdi_img[i];
            const int keep = !im->late || (im->x0 <= X + 2 && im->x1 >= X + W - 2 &&
                                           area[i] * 4 >= (int64_t)W * H * 3 && count[i] <= 16);
            remap[i] = keep ? nk : -1;
            if (keep) s_bdi_img[nk++] = *im;
        }
        s_bdi_nimg = nk;
        for (int i = 0; i < s_bdi_nprim; i++)
            if (remap[s_bdi_prim[i].img] >= 0) {
                s_bdi_prim[np] = s_bdi_prim[i];
                s_bdi_prim[np++].img = remap[s_bdi_prim[i].img];
            }
        s_bdi_nprim = np;
    }
    bdi_split_bands();
    bdi_tiles_learn();
    /* each band; a picture none of whose bands runs past the wide edge, as a whole */
    s_bdi_nwrap = 0;
    uint8_t wide[BDI_IMAGES] = {0};
    for (int i = 0; i < s_bdi_nimg; i++)
        wide[s_bdi_img[i].parent] |= (uint8_t)bdi_decide(i, 0, &s_bdi_img[i]);
    for (int par = 0; par < s_bdi_npar; par++) {
        if (wide[par]) continue;
        BdiImage whole;
        bdi_decide(par, 1, &whole);
        for (int i = 0; i < s_bdi_nimg; i++) {
            BdiImage *im = &s_bdi_img[i];
            if (im->parent != par) continue;
            if (im->wrap_sides) bdi_drop_wraps(i);
            im->q = whole.q; im->fill_sides = whole.fill_sides; im->edges = whole.edges;
            im->fill_col[0] = whole.fill_col[0]; im->fill_col[1] = whole.fill_col[1];
        }
    }
    s_bdi_nrun = 0;
    for (int i = 0; i < s_bdi_nimg; i++) {
        s_bdi_stat[s_bdi_img[i].q ? 2 : s_bdi_img[i].fill_sides ? 1 : 0]++;
        for (int right = 0; right < 2; right++)
            if (s_bdi_img[i].fill_sides & (1 << right)) bdi_fill_runs(i, right);
    }
    s_bdi_valid = s_bdi_nimg > 0;
}

/* The answer for the primitive being drawn: -1 none (not a backdrop image);
 * 0 drawn as it is; else (marked ? 1 : 4) | q << 8. */
static int bdi_gate(int marked) {
    if (!ws_native_wide_active()) return -1;
    const int img = bdi_find(gp0_cmd_source_addr);
    if (img < 0) return -1;
    s_bdi_stat[3]++;
    const BdiImage *im = &s_bdi_img[img];
    if (im->edges) return 3;   /* vertices on or past the picture's edges out to the wide view's */
    if (!im->q) return 0;
    return (marked && !im->late ? 1 : 4) | (int)im->q << 8;
}

/* At an image's first primitive: its filled margins, before any of it. */
static void bdi_fill_note(uint8_t op) {
    if (!s_bdi_valid || op < 0x20u || op > 0x7Fu || !ws_native_wide_active()) return;
    const int img = bdi_find(gp0_cmd_source_addr);
    if (img < 0) return;
    BdiImage *im = &s_bdi_img[img];
    if ((!im->fill_sides && !im->wrap_sides) || im->filled ||
        GPU_RAM_KEY(gp0_cmd_source_addr) != im->first_key) return;
    im->filled = 1;
    for (int k = 0; k < s_bdi_nrun; k++) {
        const BdiRun *r = &s_bdi_run[k];
        if (r->img != img) continue;
        int32_t y0 = r->y0 + draw_offset_y, y1 = r->y1 + draw_offset_y;
        if (y0 < (int32_t)draw_area_top) y0 = (int32_t)draw_area_top;
        if (y1 > (int32_t)draw_area_bottom + 1) y1 = (int32_t)draw_area_bottom + 1;
        if (y1 > y0)
            gr_wide_clear_margins((int)draw_area_left, (int)y0, (int)(y1 - y0), r->col, 1 << r->right);
    }
    /* the tiles beyond a strip's end, as the strip's own tile beside them is drawn */
    for (int k = 0; k < s_bdi_nwrap; k++) {
        const BdiWrap *wp = &s_bdi_wrap[k];
        if (wp->img != img) continue;
        const int32_t x = wp->x + draw_offset_x, y = wp->y + draw_offset_y;
        if (draw_area_out_rect(x, y, wp->t.w, wp->t.h)) continue;
        gr_set_color_modulation((int)(wp->col & 0xFF), (int)((wp->col >> 8) & 0xFF),
                                (int)((wp->col >> 16) & 0xFF), wp->op & 1);
        gr_set_semi_transparency((wp->op >> 1) & 1, (wp->t.tpage >> 5) & 3);
        gr_draw_textured_rect(x, y, wp->t.w, wp->t.h, wp->t.u, wp->t.v,
                              (uint16_t)((wp->t.clut & 0x3F) * 16), (uint16_t)((wp->t.clut >> 6) & 0x1FF),
                              wp->t.tpage);
    }
}
/* TCP ws_backdrop_images: the last linked list's images. */
int gpu_ws_backdrop_images_json(char *out, size_t cap) {
    size_t n = (size_t)snprintf(out, cap,
        "\"enabled\":%d,\"valid\":%d,\"prims\":%d,\"totals\":[%u,%u,%u,%u],\"disp\":[%d,%d,%d],\"images\":[",
        bdi_enabled(), s_bdi_valid, s_bdi_nprim, s_bdi_stat[0], s_bdi_stat[1], s_bdi_stat[2], s_bdi_stat[3],
        (int)ws_disp_x(), (int)ws_disp_w(), (int)ws_disp_h());
    for (int i = 0; i < s_bdi_nimg && n < cap; i++) {
        const BdiImage *im = &s_bdi_img[i];
        int np = 0;
        for (int k = 0; k < s_bdi_nprim; k++) np += s_bdi_prim[k].img == i;
        n += (size_t)snprintf(out + n, cap - n,
            "%s{\"x\":[%d,%d],\"y\":[%d,%d],\"prims\":%d,\"tex\":%d,\"semi\":%d,\"clut\":%u,\"q\":%u,"
            "\"fill\":%u,\"fill_col\":[%u,%u],\"image\":%d,\"late\":%u,\"wrap\":%u,\"edges\":%u,"
            "\"edge\":[[%u,%u,%u,%u],[%u,%u,%u,%u]]}",
            i ? "," : "", (int)im->x0, (int)im->x1, (int)im->y0, (int)im->y1, np, im->textured, im->semi,
            im->clut, im->q, im->fill_sides, im->fill_col[0], im->fill_col[1], (int)im->parent, im->late,
            im->wrap_sides, im->edges, im->dbg_edge[0][0], im->dbg_edge[0][1], im->dbg_edge[0][2], im->dbg_edge[0][3],
            im->dbg_edge[1][0], im->dbg_edge[1][1], im->dbg_edge[1][2], im->dbg_edge[1][3]);
    }
    if (n < cap) n += (size_t)snprintf(out + n, cap - n, "]");
    return (int)n;
}

static int ws_ui_prepass_order_cmp(const void *x, const void *y) {
    const uint16_t a = *(const uint16_t *)x, b = *(const uint16_t *)y;
    const uint32_t aa = ws_ui_prepass_nodes[a].addr, ba = ws_ui_prepass_nodes[b].addr;
    if (aa != ba) return aa < ba ? -1 : 1;
    return a < b ? -1 : a > b;   /* a repeated address: the first record */
}

/* The first prepass record of `resolved`, as the linear scan found it. */
static const WsUiPrepassNode *ws_ui_prepass_find_node(uint32_t resolved) {
    if (ws_ui_prepass_order_gen != ws_ui_prepass_gen ||
        ws_ui_prepass_order_count != ws_ui_prepass_node_count) {
        for (uint32_t i = 0; i < ws_ui_prepass_node_count; i++)
            ws_ui_prepass_order[i] = (uint16_t)i;
        qsort(ws_ui_prepass_order, ws_ui_prepass_node_count,
              sizeof ws_ui_prepass_order[0], ws_ui_prepass_order_cmp);
        ws_ui_prepass_order_gen = ws_ui_prepass_gen;
        ws_ui_prepass_order_count = ws_ui_prepass_node_count;
    }
    uint32_t lo = 0, hi = ws_ui_prepass_order_count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (ws_ui_prepass_nodes[ws_ui_prepass_order[mid]].addr < resolved) lo = mid + 1;
        else hi = mid;
    }
    if (lo < ws_ui_prepass_order_count &&
        ws_ui_prepass_nodes[ws_ui_prepass_order[lo]].addr == resolved)
        return &ws_ui_prepass_nodes[ws_ui_prepass_order[lo]];
    return NULL;
}

void gpu_ws_validate_linked_list_header(uint32_t addr, uint32_t header) {
    if (ws_ui_prepass_count == 0) return;

    uint32_t resolved =
        GPU_RAM_KEY(psx_mod_gpu_dma_resolve_address(addr));
    const WsUiPrepassNode *node = ws_ui_prepass_find_node(resolved);
    if (!node || node->header != header) {
        ws_ui_prepass_invalidate_stale_why(node ? 3 : 2, resolved);
        ws_ui_prepass_invalidate_stale();
    }
}

void gpu_ws_validate_linked_list_node(uint32_t addr, uint32_t num_words) {
    if (ws_ui_prepass_count == 0) return;

    const uint32_t node_addr = psx_mod_gpu_dma_resolve_address(addr);
    const uint32_t resolved = GPU_RAM_KEY(node_addr);
    const WsUiPrepassNode *node = ws_ui_prepass_find_node(resolved);
    if (!node || node->payload_guard.word_count != num_words) {
        ws_ui_prepass_invalidate_stale_why(node ? 4 : 2, resolved);
        ws_ui_prepass_invalidate_stale();
        return;
    }

    /* Re-read the payload where the walk reads it (the node's own memory),
     * never through the key. */
    uint32_t payload[255];
    uint32_t first_addr =
        psx_mod_gpu_dma_resolve_address(node_addr + 4u);
    for (uint32_t i = 0; i < num_words; i++) {
        payload[i] = psx_read_word(psx_mod_gpu_dma_resolve_address(
            first_addr + i * 4u));
    }
    if (!ws_prepass_packet_matches(&node->payload_guard, payload, num_words)) {
        ws_ui_prepass_invalidate_stale_why(5, resolved);
        ws_ui_prepass_invalidate_stale();
    }
}

/* Per-opcode execution counters (exposed via gpu_get_opcode_stats) */
static uint32_t gp0_opcode_count[256];

uint32_t gpu_get_opcode_count(uint8_t op) { return gp0_opcode_count[op]; }

/* ---- Per-frame GP0 command ring (debug tools only) -----------------------
 * We record every GP0 command with the frame number it was issued in. Polyline
 * / long commands get the current public word cap, enough for diagnosing
 * per-primitive state. Production builds leave the public accessors linkable
 * but report an unavailable/empty capture. */

extern uint64_t s_frame_count;  /* defined in debug_server.c */
#ifndef PSX_NO_DEBUG_TOOLS
extern uint32_t g_debug_last_store_pc;  /* defined in debug_server.c */
extern uint32_t g_debug_current_func_addr;  /* defined in debug_server.c */
uint32_t debug_guest_ra(void);  /* accessor in debug_server.c (guest $ra) */
uint32_t debug_guest_sp(void);  /* accessor in debug_server.c (guest $sp) */
extern uint8_t *memory_get_ram_ptr(void); /* raw 2MB main-RAM base (no lockstep) */

/* Bounded guest-stack unwind for VRAM-copy builder attribution. Scans the live
 * stack for words that are valid return addresses (main-RAM code range, and the
 * instruction at ret-8 is a jal/jalr), innermost first. A consumer skips the
 * libgpu funnel band to name the game-level routine that issued the copy. Pure
 * reads against the raw RAM array — no lockstep/observer pollution. */
uint32_t g_gp0_last_copy_sp = 0;   /* diag: raw guest $sp at last op-0x80 copy */
static void gp0_capture_builder_chain(uint32_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = 0;
    uint8_t *ram = memory_get_ram_ptr();
    if (!ram) return;
    const uint32_t RMASK = g_psx_ram_mask;          /* live RAM, mirror-folded */
    uint32_t rawsp = debug_guest_sp();
    g_gp0_last_copy_sp = rawsp;
    if ((rawsp & 0x1FFFFFFFu) >= 0x00800000u) return; /* not in RAM/mirror */
    uint32_t sp = psx_ram_canonical_offset(rawsp) & ~3u; /* fold, word-align */
    /* Two-pass: prefer words that are genuine return addresses (call at ret-8),
     * but if guest code validation yields none, fall back to any code-range
     * stack word so the chain is never silently empty. */
    int found = 0;
    for (int pass = 0; pass < 2 && found == 0; pass++) {
        for (uint32_t off = 0; off + 4 <= 0x400u && found < 6; off += 4) {
            uint32_t w; memcpy(&w, ram + ((sp + off) & RMASK), 4);
            if ((w & 3u) || w < 0x80010000u || w >= 0x80120000u) continue;
            if (pass == 0) {
                uint32_t ins; memcpy(&ins, ram + ((w - 8u) & RMASK), 4);
                uint32_t op = ins >> 26;
                int is_call = (op == 3u) || (op == 0u && (ins & 0x3Fu) == 9u);
                if (!is_call) continue;
            }
            out[found++] = w;
        }
    }
}

#define GP0_RING_CAP        (1u << 20)  /* 1 048 576 entries */
/* GpuGp0RingEntry + GPU_GP0_RING_MAX_WORDS are public types in gpu.h. */

static GpuGp0RingEntry *gp0_ring = NULL;  /* lazy-alloc on first record */
static uint64_t gp0_ring_seq    = 0;      /* total commands recorded */
static uint32_t gp0_ring_head   = 0;      /* next write slot */

/* Capture mark (debug_server_capture_mark): stop overwriting so the window
 * that led up to an operator-reported glitch survives until it is read. */
static int gp0_ring_frozen = 0;
void gpu_gp0_ring_set_frozen(int frozen) { gp0_ring_frozen = frozen ? 1 : 0; }

static void gp0_ring_record(const uint32_t *words, int n) {
    if (gp0_ring_frozen) return;
    if (!gp0_ring) {
        gp0_ring = (GpuGp0RingEntry *)calloc(GP0_RING_CAP, sizeof(*gp0_ring));
        if (!gp0_ring) return;  /* OOM — capture disabled, no other effect */
    }
    GpuGp0RingEntry *e = &gp0_ring[gp0_ring_head];
    e->frame   = (uint32_t)s_frame_count;
    e->seq     = (uint32_t)gp0_ring_seq;
    e->src_addr = gp0_cmd_source_addr;
    e->pc      = g_debug_last_store_pc;
    e->func    = g_debug_current_func_addr;
    e->ra      = debug_guest_ra();
    e->opcode  = (uint8_t)((words[0] >> 24) & 0xFF);
    e->n_words = (uint8_t)(n > 255 ? 255 : (n < 0 ? 1 : n));
    e->ot_rank = gp0_ot_rank;
    int copy_n = e->n_words > GPU_GP0_RING_MAX_WORDS ? GPU_GP0_RING_MAX_WORDS : e->n_words;
    for (int i = 0; i < copy_n; i++) e->cmd[i] = words[i];
    for (int i = copy_n; i < GPU_GP0_RING_MAX_WORDS; i++) e->cmd[i] = 0;
    if (e->opcode == 0x80) { gp0_capture_builder_chain(e->bld); e->csp = g_gp0_last_copy_sp; }
    else { for (int i = 0; i < 6; i++) e->bld[i] = 0; e->csp = 0; }
    gp0_ring_head = (gp0_ring_head + 1) % GP0_RING_CAP;
    gp0_ring_seq++;
}

/* Public accessors for debug_server.c */
uint64_t gpu_gp0_ring_total(void)    { return gp0_ring_seq; }
uint32_t gpu_gp0_ring_capacity(void) { return GP0_RING_CAP; }
uint32_t gpu_gp0_ring_max_words(void){ return GPU_GP0_RING_MAX_WORDS; }

/* Fill `out[0..max_out-1]` with entries from the requested frame; returns
 * count. Walks from oldest in-buffer to newest so iteration order matches
 * draw order within a frame. */
int gpu_gp0_ring_dump_frame(uint32_t frame, GpuGp0RingEntry *out, int max_out) {
    if (!gp0_ring || max_out <= 0) return 0;
    uint32_t avail = (gp0_ring_seq < GP0_RING_CAP)
                   ? (uint32_t)gp0_ring_seq : GP0_RING_CAP;
    uint32_t start = (gp0_ring_seq < GP0_RING_CAP) ? 0 : gp0_ring_head;
    int n_out = 0;
    for (uint32_t i = 0; i < avail && n_out < max_out; i++) {
        uint32_t idx = (start + i) % GP0_RING_CAP;
        if (gp0_ring[idx].frame == frame) {
            out[n_out++] = gp0_ring[idx];
        }
    }
    return n_out;
}

/* Report the frame range currently held in the ring (oldest..newest). */
void gpu_gp0_ring_frame_span(uint32_t *out_oldest, uint32_t *out_newest) {
    if (out_oldest) *out_oldest = 0;
    if (out_newest) *out_newest = 0;
    if (!gp0_ring || gp0_ring_seq == 0) return;
    uint32_t avail = (gp0_ring_seq < GP0_RING_CAP)
                   ? (uint32_t)gp0_ring_seq : GP0_RING_CAP;
    uint32_t start = (gp0_ring_seq < GP0_RING_CAP) ? 0 : gp0_ring_head;
    uint32_t newest_idx = (start + avail - 1) % GP0_RING_CAP;
    if (out_oldest) *out_oldest = gp0_ring[start].frame;
    if (out_newest) *out_newest = gp0_ring[newest_idx].frame;
}
#else
static void gp0_ring_record(const uint32_t *words, int n) {
    (void)words;
    (void)n;
}

void gpu_gp0_ring_set_frozen(int frozen) { (void)frozen; }
uint64_t gpu_gp0_ring_total(void) { return 0; }
uint32_t gpu_gp0_ring_capacity(void) { return 0; }
uint32_t gpu_gp0_ring_max_words(void) { return GPU_GP0_RING_MAX_WORDS; }
int gpu_gp0_ring_dump_frame(uint32_t frame, GpuGp0RingEntry *out, int max_out) {
    (void)frame;
    (void)out;
    (void)max_out;
    return 0;
}
void gpu_gp0_ring_frame_span(uint32_t *out_oldest, uint32_t *out_newest) {
    if (out_oldest) *out_oldest = 0;
    if (out_newest) *out_newest = 0;
}
#endif

void gpu_set_gp0_source(uint32_t addr) {
    gp0_next_source_addr = addr;
}

/* ---- Draw census ring (ALWAYS-ON) -----------------------------------------
 * Every drawn primitive (GP0 opcode 0x20-0x7F) records: frame, the prim's DMA
 * source address, the live camera (scratchpad camX/camY), and the prim's first
 * vertex in DRAWING space (pre draw_offset). Purpose: see object spawn/despawn
 * and edge-cull in DATA, not screenshots. When a background object despawns,
 * its prim simply stops appearing in the census at a specific camX while its
 * last recorded screen-x shows how far on-screen it still was — i.e. the
 * effective (4:3-sized) despawn margin, which the 16:9 view exceeds. A prim
 * present in the census but absent on screen would instead indict the renderer
 * gate. Query via TCP `ws_census` → CSV file (large dumps mustn't ride TCP). */
#define WS_CENSUS_CAP (1u << 21)            /* 2,097,152 entries * 24B = 48 MiB */
typedef struct {
    uint32_t frame;
    uint32_t src_addr;
    int16_t  cam_x, cam_y;
    int16_t  x, y;        /* first vertex (screen / pre-draw_offset) */
    int16_t  xmin, xmax;  /* screen-X extent across the prim's position verts */
    int16_t  base_x;      /* back-buffer origin (draw_area_left) at draw time */
    uint8_t  opcode;
    uint8_t  tagged;      /* psx_ws_prim_is_tagged() at draw time */
} WsCensusEntry;
static WsCensusEntry *ws_census = NULL;
static uint64_t       ws_census_seq = 0;
static int            ws_census_on  = 1;     /* always-on; toggle via ws_census */
extern uint16_t       psx_read_half(uint32_t addr);

/* Screen-X extent (raw vertex coords = SX, pre-draw_offset) across a prim's
 * position vertices. Polygons (0x20-0x3F) decoded exactly via the packet stride
 * (pos word = 1 + i*(1 + gouraud + textured)); rects/sprites/lines fall back to
 * the first vertex. Lets the census show how far right each path reaches and
 * whether anything is drawn past the 4:3 edge (native-wide margin diagnosis). */
static void prim_sx_extent(uint8_t op, int32_t *xmin, int32_t *xmax) {
    int32_t lo = 0x7FFF, hi = -0x8000;
    if (op >= 0x20 && op <= 0x3F) {
        int nv     = (op & 0x08) ? 4 : 3;
        int stride = 1 + ((op & 0x10) ? 1 : 0) + ((op & 0x04) ? 1 : 0);
        for (int i = 0; i < nv; i++) {
            int32_t vx, vy;
            parse_vertex(gp0_cmd_buf[1 + i * stride], &vx, &vy);
            if (vx < lo) lo = vx;
            if (vx > hi) hi = vx;
        }
    } else {
        int32_t vx, vy;
        parse_vertex(gp0_cmd_buf[1], &vx, &vy);
        lo = hi = vx;
    }
    *xmin = lo; *xmax = hi;
}

/* Natural-overhang noter (see the ws_ovh_* block up top for the rationale).
 * Called for every draw prim; counts POLYGONS whose raw SX extent crosses
 * outside [0, disp_w] by more than the jitter margin, and stamps the frame
 * (with the 2-consecutive-frames sustained rule) when enough do. */
static void ws_note_overhang(uint8_t op) {
    if (op < 0x20 || op > 0x3F) return;      /* polygons only — the world's prims */
    /* Sprite-funnel (tagged) prims slide in from off-screen routinely —
     * title-card letter tiles are tagged poly quads entering from the edges
     * and stamped a sustained overhang inside the hut (observed at frame
     * 20245). The WORLD funnel (Cluster-A RTPT terrain) never tags, so
     * untagged overhang is the uncorrupted "revealable content" signal.
     * Untagged-only also leaves non-tag titles unchanged (is_tagged == 0). */
    if (psx_ws_prim_is_tagged()) return;
    int32_t xmn, xmx;
    prim_sx_extent(op, &xmn, &xmx);
    int32_t W = ws_disp_w();
    if (xmn >= -WS_OVERHANG_DEEP_PX && xmx <= W + WS_OVERHANG_DEEP_PX) return;
    uint32_t f = (uint32_t)s_frame_count;
    if (f != ws_ovh_frame) {
        ws_ovh_prev  = (f == ws_ovh_frame + 1u) ? ws_ovh_count : 0;
        ws_ovh_frame = f; ws_ovh_count = 0;
    }
    ws_ovh_count++;
    if (ws_ovh_count >= WS_OVERHANG_MIN_PRIMS && ws_last_ovh_stamp != f) {
        if (f == ws_last_ovh_stamp + 1u) ws_sust_ovh_stamp = f;
        ws_last_ovh_stamp = f;
    }
}

/* Finite-backdrop noter (ws_backdrop_extent.h). Called for every draw prim in
 * submission order. The frame's leading run of rectangles is its back-most
 * layer; the first polygon or line closes it. Only untagged textured rects
 * that the renderer leaves at their authored position contribute: sprite-
 * funnel prims (actors, HUD, a tagged far backdrop), stretched backdrop prims
 * and host-extended backdrop packets already reach past the canonical edge, so
 * they are neutral, as are untextured rects (full-width fades are extended).
 * Measured against the CONFIGURED reveal, not the active one, so the verdict
 * cannot feed back through its own 4:3 present. */
static void ws_note_backdrop(uint8_t op) {
    if (ws_mode != 2) return;
    int32_t reveal = ws_nw_configured_offset();
    if (reveal <= 0) return;
    ws_bdx_frame(&ws_bdx, (uint32_t)s_frame_count, ws_disp_w(), ws_disp_h(), reveal);
    if (op < 0x60) { ws_bdx_close(&ws_bdx); return; }
    if (!(op & 0x04)) return;
    if (psx_ws_prim_is_tagged() || psx_ws_prim_in_backdrop() || ws_bg2d_host_packet())
        return;
    int32_t x, y, w, h;
    parse_vertex(gp0_cmd_buf[1], &x, &y);
    switch ((op >> 3) & 3u) {
    case 0: w = (int32_t)(gp0_cmd_buf[3] & 0x3FFu);
            h = (int32_t)((gp0_cmd_buf[3] >> 16) & 0x1FFu); break;
    case 1: w = h = 1; break;
    case 2: w = h = 8; break;
    default: w = h = 16; break;
    }
    ws_bdx_rect(&ws_bdx, x, y, x + w, y + h);
}

static void ws_census_record(uint8_t opcode, int32_t x, int32_t y) {
    if (!ws_census_on) return;
    if (!ws_census) {
        ws_census = (WsCensusEntry *)calloc(WS_CENSUS_CAP, sizeof(WsCensusEntry));
        if (!ws_census) { ws_census_on = 0; return; }
    }
    int32_t xmn, xmx;
    prim_sx_extent(opcode, &xmn, &xmx);
    WsCensusEntry *e = &ws_census[ws_census_seq & (WS_CENSUS_CAP - 1)];
    e->frame    = (uint32_t)s_frame_count;
    e->src_addr = gp0_cmd_source_addr;
    e->cam_x    = (int16_t)psx_read_half(0x1F800176);
    e->cam_y    = (int16_t)psx_read_half(0x1F800186);
    e->x        = (int16_t)x;
    e->y        = (int16_t)y;
    e->xmin     = (int16_t)xmn;
    e->xmax     = (int16_t)xmx;
    e->base_x   = (int16_t)draw_area_left;
    e->opcode   = opcode;
    e->tagged   = (uint8_t)psx_ws_prim_is_tagged();
    ws_census_seq++;
}

/* Dump census rows for frames [f0,f1] to a CSV file. Returns row count. */
int gpu_ws_census_dump(uint32_t f0, uint32_t f1, const char *path) {
    if (!ws_census) return 0;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "frame,src_addr,cam_x,cam_y,x,y,xmin,xmax,base_x,opcode,tagged\n");
    uint64_t total = ws_census_seq;
    uint64_t avail = total < WS_CENSUS_CAP ? total : WS_CENSUS_CAP;
    uint64_t start = total - avail;
    int n = 0;
    for (uint64_t s = start; s < total; s++) {
        WsCensusEntry *e = &ws_census[s & (WS_CENSUS_CAP - 1)];
        if (e->frame < f0 || e->frame > f1) continue;
        fprintf(fp, "%u,0x%08X,%d,%d,%d,%d,%d,%d,%d,0x%02X,%u\n",
                e->frame, e->src_addr, e->cam_x, e->cam_y, e->x, e->y,
                e->xmin, e->xmax, e->base_x, e->opcode, e->tagged);
        n++;
    }
    fclose(fp);
    return n;
}

void gpu_ws_census_set(int on) { ws_census_on = on ? 1 : 0; }
uint64_t gpu_ws_census_seq(void) { return ws_census_seq; }

/* Execute a fully-collected GP0 command */
static void gp0_dispatch_command(uint8_t opcode);

static void gp0_execute_command(void) {
    uint8_t opcode = (gp0_cmd_buf[0] >> 24) & 0xFF;
    gp0_opcode_count[opcode]++;
    gp0_ring_record(gp0_cmd_buf, gp0_words_needed);
    extern void ws_bg_phase_note(uint32_t op);
    ws_bg_phase_note(opcode);   /* native-wide 2D-backdrop stretch: background-phase latch */
    bdi_fill_note(opcode);      /* a backdrop image's margins in its edge colour, before it */
    if (ws_view_camera_addr || g_bg2d_host_size) {
        WsViewAnchor v = ws_view_current();
        /* Draw env packets select the back-buffer band after linked-list
         * begin. Clear narrow-room padding at its first actual draw. */
        static uint32_t clear_frame[512];
        static uint32_t clear_generation[512];
        if (opcode >= 0x20 && opcode <= 0x7F &&
            (ws_view_enabled() || (g_bg2d_host_size && ws_native_wide_active() && ws_disp_w() <= 384)) &&
            (g_bg2d_host_size || v.pad_left || v.pad_right) && draw_area_top < 512 &&
            (g_bg2d_host_size
                ? clear_generation[draw_area_top] != g_bg2d_host_generation
                : clear_frame[draw_area_top] != (uint32_t)s_frame_count + 1u) &&
            ws_is_fb_base(draw_area_left)) {
            clear_frame[draw_area_top] = (uint32_t)s_frame_count + 1u;
            clear_generation[draw_area_top] = g_bg2d_host_generation;
            gr_wide_clear_margins((int)draw_area_left, (int)draw_area_top,
                (int)draw_area_bottom - (int)draw_area_top + 1, 0, 3);
        }
        v = ws_view_packet();
        int32_t hud_delta;
        /* Existing HUD placement stays fixed in the physical frame. Only the
         * wide world mirror receives the camera-edge origin and room clip. */
        if (ws_nw_left_hud_packet() ||
            (!(g_bg2d_host_size && ws_tagged_world_primitive()) &&
             ws_nw_explicit_hud_delta(&hud_delta)))
            v.shift = v.pad_left = v.pad_right = 0;
        gr_wide_set_view(ws_view_enabled(), v.shift, v.pad_left, v.pad_right);
    }

    /* Draw-census: capture every drawing primitive's first vertex + camera. */
    if (opcode >= 0x20 && opcode <= 0x7F) {
        gpu_drawn_note_draw();
        int32_t cvx, cvy;
        parse_vertex(gp0_cmd_buf[1], &cvx, &cvy);
        ws_census_record(opcode, cvx, cvy);
        ws_note_overhang(opcode);   /* 2D-only-scene classifier world signal */
        ws_note_backdrop(opcode);   /* ...and its finite-backdrop veto */
    }

    /* Categorize for diagnostics */
    if (opcode <= 0x01) gp0_nop_count++;
    else if (opcode == 0x02) gp0_fill_count++;
    else if (opcode >= 0x20 && opcode <= 0x7F) gp0_draw_count++;
    else if (opcode >= 0x80 && opcode <= 0xDF) gp0_copy_count++;
    else if (opcode >= 0xE1 && opcode <= 0xE6) gp0_env_count++;

    if (opcode >= 0x20 && opcode <= 0x7F && ws_shared_ui_packet()) {
        for (ws_shared_ui_copy = 0; ws_shared_ui_copy < 2; ws_shared_ui_copy++)
            gp0_dispatch_command(opcode);
        ws_shared_ui_copy = -1;
    } else {
        gp0_dispatch_command(opcode);
    }
}

static void gp0_dispatch_command(uint8_t opcode) {
    switch (opcode) {
        case 0x00:
        case 0x01:
            gp0_exec_nop();
            break;

        case 0x02:
            gp0_exec_fill_rect();
            break;

        case 0xE1:
            gp0_exec_draw_mode();
            break;

        case 0xE2:
            gp0_exec_texture_window();
            break;

        case 0xE3:
            gp0_exec_draw_area_tl();
            break;

        case 0xE4:
            gp0_exec_draw_area_br();
            break;

        case 0xE5:
            gp0_exec_draw_offset();
            break;

        case 0xE6:
            gp0_exec_mask_bit();
            break;

        /* Drawing commands — polygons */
        case 0x20: case 0x21: case 0x22: case 0x23:
            gp0_exec_mono_tri();
            break;
        case 0x24: case 0x25: case 0x26: case 0x27:
            if (sh_off_n) shadow_off_note((uint16_t)(gp0_cmd_buf[4] >> 16), (uint16_t)(gp0_cmd_buf[2] >> 16), (gp0_cmd_buf[0] >> 25) & 1);
            gp0_exec_textured_tri();
            break;
        case 0x28: case 0x29: case 0x2A: case 0x2B:
            gp0_exec_mono_quad();
            break;
        case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
            /* layout: colour+v0, tc0+clut, v1, tc1+tpage, v2, tc2, v3, tc3 */
            if (sh_off_n) shadow_off_note((uint16_t)(gp0_cmd_buf[4] >> 16), (uint16_t)(gp0_cmd_buf[2] >> 16), (gp0_cmd_buf[0] >> 25) & 1);
            int kx, ky;
            const int key = shadow_blob_key((gp0_cmd_buf[0] >> 25) & 1, gp0_cmd_buf[1], gp0_cmd_buf[3], gp0_cmd_buf[5],
                                            gp0_cmd_buf[7], (const uint32_t[4]){gp0_cmd_buf[2], gp0_cmd_buf[4],
                                            gp0_cmd_buf[6], gp0_cmd_buf[8]}, &kx, &ky);
            const uint16_t kc = key ? shadow_blob_key_hide(kx, ky) : 0;
            gp0_exec_textured_quad();
            if (key) shadow_blob_key_put(kx, ky, kc);
            break;
        }
        case 0x30: case 0x31: case 0x32: case 0x33:
            gp0_exec_shaded_tri();
            break;
        case 0x34: case 0x35: case 0x36: case 0x37:
            if (sh_off_n) shadow_off_note((uint16_t)(gp0_cmd_buf[5] >> 16), (uint16_t)(gp0_cmd_buf[2] >> 16), (gp0_cmd_buf[0] >> 25) & 1);
            gp0_exec_shaded_textured_tri();
            break;
        case 0x38: case 0x39: case 0x3A: case 0x3B:
            gp0_exec_shaded_quad();
            break;
        case 0x3C: case 0x3D: case 0x3E: case 0x3F: {
            /* layout: colour+v0, tc0+clut, c1, v1, tc1+tpage, c2, v2, tc2, c3, v3, tc3 */
            if (sh_off_n) shadow_off_note((uint16_t)(gp0_cmd_buf[5] >> 16), (uint16_t)(gp0_cmd_buf[2] >> 16), (gp0_cmd_buf[0] >> 25) & 1);
            int kx, ky;
            const int key = shadow_blob_key((gp0_cmd_buf[0] >> 25) & 1, gp0_cmd_buf[1], gp0_cmd_buf[4], gp0_cmd_buf[7],
                                            gp0_cmd_buf[10], (const uint32_t[4]){gp0_cmd_buf[2], gp0_cmd_buf[5],
                                            gp0_cmd_buf[8], gp0_cmd_buf[11]}, &kx, &ky);
            const uint16_t kc = key ? shadow_blob_key_hide(kx, ky) : 0;
            gp0_exec_shaded_textured_quad();
            if (key) shadow_blob_key_put(kx, ky, kc);
            break;
        }

        /* Lines */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            gp0_exec_mono_line();
            break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            gp0_exec_shaded_line();
            break;

        /* Rectangles */
        case 0x60: case 0x61: case 0x62: case 0x63:
            gp0_exec_mono_rect();
            break;
        case 0x64: case 0x65: case 0x66: case 0x67:
            if (sh_off_n) shadow_off_note(current_texpage(), (uint16_t)(gp0_cmd_buf[2] >> 16), (gp0_cmd_buf[0] >> 25) & 1);   /* the draw mode's page */
            gp0_exec_textured_rect();
            break;
        case 0x68: case 0x69: case 0x6A: case 0x6B:
            gp0_exec_mono_dot();
            break;
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:
            gp0_exec_textured_dot();
            break;
        case 0x70: case 0x71: case 0x72: case 0x73:
            gp0_exec_mono_8x8();
            break;
        case 0x74: case 0x75: case 0x76: case 0x77:
            gp0_exec_textured_8x8();
            break;
        case 0x78: case 0x79: case 0x7A: case 0x7B: {
            /* 16x16 mono sprite */
            int semi_trans = (gp0_cmd_buf[0] >> 25) & 1;
            uint16_t color = rgb888_to_rgb555(gp0_cmd_buf[0] & 0xFFFFFFu);
            int32_t x0, y0;
            parse_vertex(gp0_cmd_buf[1], &x0, &y0);
            int ws_h = 16;
            int ws_w = ws_sprt_fixed_transform(&x0, &y0, 16, &ws_h);
            x0 += ws_nw_hud_shift(x0, 16);
            x0 += draw_offset_x; y0 += draw_offset_y;
            gr_set_semi_transparency(semi_trans, (int)semi_transparency);
            gr_draw_flat_rect(x0, y0, (ws_w && ws_w != 16) ? ws_w : 16, ws_h,
                              color);
            break;
        }
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            gp0_exec_textured_16x16();
            break;

        /* VRAM→VRAM copy */
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
            /* Word 1: source coords, Word 2: dest coords, Word 3: dimensions */
            int src_x = gp0_cmd_buf[1] & 0x3FF;
            int src_y = (gp0_cmd_buf[1] >> 16) & 0x1FF;
            int dst_x = gp0_cmd_buf[2] & 0x3FF;
            int dst_y = (gp0_cmd_buf[2] >> 16) & 0x1FF;
            int w = gp0_cmd_buf[3] & 0x3FF;
            int h = (gp0_cmd_buf[3] >> 16) & 0x1FF;
            if (w == 0) w = 0x400;
            if (h == 0) h = 0x200;
            gpu_drawn_mark(dst_x, dst_y, dst_x + w - 1, dst_y + h - 1);
            gr_copy_rect(src_x, src_y, dst_x, dst_y, w, h);
            break;
        }

        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            gp0_exec_cpu_to_vram();
            break;

        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC4: case 0xC5: case 0xC6: case 0xC7:
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            gp0_exec_vram_to_cpu();
            break;

        default:
            /* NOP range: 0x03-0x1E, 0xE0, 0xE7-0xEF, 0xFF */
            if ((opcode >= 0x03 && opcode <= 0x1E) ||
                opcode == 0xE0 ||
                (opcode >= 0xE7 && opcode <= 0xEF) ||
                opcode == 0xFF) {
                break;  /* NOP — silently consume */
            }

            /* Any other command (drawing, VRAM-to-VRAM, etc.) is not yet
             * implemented. Fatal halt so we know exactly what's needed next,
             * with all rings queryable post-mortem. */
            {
                static char reason[96];
                snprintf(reason, sizeof(reason),
                         "GPU GP0 unimplemented command 0x%02X (word 0x%08X)",
                         opcode, gp0_cmd_buf[0]);
                psx_fatal_halt(reason);
            }
    }
}

/* ---- GP0 write (0x1F801810 write) — command state machine ---- */

uint64_t gpu_get_gp0_count(void) { return gp0_write_count; }

void gpu_get_gp0_stats(uint64_t* nop, uint64_t* fill, uint64_t* draw, uint64_t* env, uint64_t* copy) {
    *nop = gp0_nop_count; *fill = gp0_fill_count;
    *draw = gp0_draw_count; *env = gp0_env_count; *copy = gp0_copy_count;
}

void gpu_get_draw_area(GpuDrawArea* out) {
    out->left = draw_area_left;
    out->top = draw_area_top;
    out->right = draw_area_right;
    out->bottom = draw_area_bottom;
    out->offset_x = draw_offset_x;
    out->offset_y = draw_offset_y;
}

uint16_t gpu_vram_peek(int x, int y) {
    if (x < 0 || x >= 1024 || y < 0 || y >= 512) return 0;
    if (!((s_gpu_drawn[y >> 4] >> (x >> 4)) & 1u))
        return vram[y * 1024 + x];
    /* Through the facade so the GL backend syncs its FBO down first: after
     * it every GPU write so far is in this array. */
    const uint16_t c = gr_vram_read(x, y);
    memset(s_gpu_drawn, 0, sizeof s_gpu_drawn);
    s_gpu_drawn_area = 1;
    return c;
}

/* TCP vram_drawn_check: the proof of the cells above. Keeps this array's
 * pixels outside them, reads through the renderer (every GPU write lands
 * here) and counts the kept pixels that changed: a GPU write the cells
 * missed. Also how many cells were marked. */
void gpu_vram_drawn_check(uint32_t *checked, uint32_t *changed, uint32_t *cells) {
    static uint16_t keep[1024 * 512];
    uint32_t n = 0, d = 0, m = 0;
    for (int r = 0; r < 32; r++) m += (uint32_t)__builtin_popcountll(s_gpu_drawn[r]);
    memcpy(keep, vram, sizeof keep);
    (void)gr_vram_read(0, 0);
    for (int y = 0; y < 512; y++)
        for (int x = 0; x < 1024; x++) {
            if ((s_gpu_drawn[y >> 4] >> (x >> 4)) & 1u) continue;
            n++;
            if (keep[y * 1024 + x] != vram[y * 1024 + x]) d++;
        }
    memset(s_gpu_drawn, 0, sizeof s_gpu_drawn);
    s_gpu_drawn_area = 1;
    *checked = n; *changed = d; *cells = m;
}

/* psx_mod_vram_write: a title's own pixels, as an unmasked CPU->VRAM copy,
 * in the guest's VRAM and the renderer's (as shadow_blob_key_put does). */
void psx_mod_vram_write(int x, int y, int w, int h, const uint16_t *px) {
    if (!px || w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const int vx = (x + i) & 1023, vy = (y + j) & 511;
            const uint16_t c = px[j * w + i];
            if (vram[vy * 1024 + vx] == c) continue;
            vram[vy * 1024 + vx] = c;
            gr_vram_write(vx, vy, c);
        }
}

static void gpu_write_gp0_body(uint32_t val) {
    gp0_write_count++;

    /* State: consuming pixel data for CPU→VRAM transfer */
    if (gp0_state == GP0_VRAM_WRITE) {
        /* Capture first few data words for debug */
        if (a0_capture_slot >= 0 && a0_capture_slot < A0_HISTORY_CAP) {
            int wc = a0_history[a0_capture_slot].word_count++;
            if (wc < 4)
                a0_history[a0_capture_slot].first_words[wc] = val;
        }
        /* Each word contains two 16-bit pixels (low halfword first) */
        for (int i = 0; i < 2; i++) {
            uint16_t pixel = (uint16_t)(val >> (i * 16));
            uint16_t wx = (vram_write_x + vram_write_col) % 1024;
            uint16_t wy = (vram_write_y + vram_write_row) % 512;

            /* Respect mask bit settings. The check reads through the facade:
             * under the GL backend GPU-drawn mask bits live in the FBO, not
             * the CPU array (one sync per burst; free when check is off). */
            if (check_mask_bit && (gr_vram_read((int)wx, (int)wy) & 0x8000)) {
                goto next_pixel;
            }

            if (set_mask_bit)
                pixel |= 0x8000;

            /* CPU VRAM remains immediately authoritative, preserving mid-DMA
             * reads/savestates. The renderer mirror is committed in bulk when
             * the GP0 payload completes. */
            vram[(uint32_t)wy * 1024u + wx] = pixel;
            gpu_vram_dirty_mark_row((uint32_t)wy);

        next_pixel:
            if (++vram_write_col == vram_write_w) {
                vram_write_col = 0;
                if (++vram_write_row == vram_write_h) {
                    /* Transfer complete */
                    gp0_commit_cpu_to_vram();
                    return;
                }
            }
        }

        if (--vram_write_remaining == 0) {
            gp0_commit_cpu_to_vram();
        }

        return;
    }

    /* Polyline terminator rule (Beetle mednafen/psx/gpu.c INCMD_PLINE,
     * DuckStation gpu.cpp HandleRenderPolyLineCommand/DrawingPolyLine):
     *
     *  1. A polyline always has at least two vertices. The words of the first
     *     two vertices are consumed unconditionally — mono [V0][V1], shaded
     *     [V0][C1][V1] — and are NEVER tested for the terminator.
     *  2. From the third vertex on, only the FIRST word of each vertex unit
     *     is tested: the vertex word itself for mono, the colour word for
     *     shaded. Shaded vertex words are never tested.
     *  3. The test is (word & 0xF000F000) == 0x50005000 (0x55555555).
     *
     * Testing every word is wrong in a way games actually hit: Psy-Q leaves
     * the top byte of LINE_G* colour words as junk, so a colour such as
     * 0x52545454 satisfies the mask and ended the polyline early here. The
     * leftover words were then parsed as fresh GP0 commands, and one of them,
     * a colour word 0x02010101, became a 341x341 FILL that wiped the terrain
     * texture page (Breath of Fire III item-use effect, 2026-09-03). The
     * earlier `(val & 0xF000F000) != 0` test was worse still — it fired on
     * negative vertex coordinates too (Tomba2 attract garble, "GP0 unknown
     * command 0xFE" fatal). */

    /* State: mono polyline — each word is a vertex (or terminator).
     * polyline_has_prev counts vertices received, clamped at 2. */
    if (gp0_state == GP0_POLYLINE_MONO) {
        if (polyline_has_prev >= 2 && (val & 0xF000F000u) == 0x50005000u) {
            gp0_state = GP0_IDLE;
            return;
        }
        int32_t x, y;
        parse_vertex(val, &x, &y);
        x += draw_offset_x; y += draw_offset_y;
        if (polyline_has_prev &&
            !psx_gpu_line_oversize(polyline_prev_x, polyline_prev_y, x, y)) {
            gr_draw_line(polyline_prev_x, polyline_prev_y, x, y, polyline_color);
        }
        polyline_prev_x = x; polyline_prev_y = y;
        if (polyline_has_prev < 2) polyline_has_prev++;
        return;
    }

    /* State: shaded polyline — alternating colour, vertex words.
     * Sequence: [cmd+C0] [V0] [C1] [V1] [C2] [V2] ... [terminator]
     * polyline_has_prev: 0 = need V0
     *                    1 = need C1 (part of the mandatory second vertex,
     *                        not a terminator candidate)
     *                    2 = need V_n
     *                    3 = need C_n, n >= 2 (terminator candidate)
     * The encoding stays inside the existing int so the GPU savestate
     * section keeps its size. */
    if (gp0_state == GP0_POLYLINE_SHADED) {
        if (polyline_has_prev == 0) {
            /* First vertex */
            int32_t x, y;
            parse_vertex(val, &x, &y);
            x += draw_offset_x; y += draw_offset_y;
            polyline_prev_x = x; polyline_prev_y = y;
            polyline_prev_c = polyline_color;
            polyline_has_prev = 1;
            return;
        }
        if (polyline_has_prev == 1 || polyline_has_prev == 3) {
            /* Colour word. Only the third and later vertices' colour words
             * can be the terminator. */
            if (polyline_has_prev == 3 && (val & 0xF000F000u) == 0x50005000u) {
                gp0_state = GP0_IDLE;
                return;
            }
            polyline_color = rgb888_to_rgb555(val & 0xFFFFFFu);
            polyline_has_prev = 2;
            return;
        }
        /* polyline_has_prev == 2: vertex word — never a terminator */
        {
            int32_t x, y;
            parse_vertex(val, &x, &y);
            x += draw_offset_x; y += draw_offset_y;
            if (!psx_gpu_line_oversize(polyline_prev_x, polyline_prev_y, x, y))
                gr_draw_shaded_line(polyline_prev_x, polyline_prev_y,
                                    polyline_prev_c, x, y, polyline_color);
            polyline_prev_x = x; polyline_prev_y = y;
            polyline_prev_c = polyline_color;
            polyline_has_prev = 3;
        }
        return;
    }

    /* State: collecting words for a multi-word command */
    if (gp0_state == GP0_COLLECTING) {
        gp0_cmd_buf[gp0_words_collected++] = val;
        if (gp0_words_collected >= gp0_words_needed) {
            gp0_state = GP0_IDLE;
            gp0_execute_command();
        }
        return;
    }

    /* State: IDLE — this is the first word of a new command */
    uint8_t opcode = (val >> 24) & 0xFF;
    int word_count = gp0_command_word_count(opcode);

    if (word_count == 0) {
        /* Unknown command — fatal halt with rings queryable post-mortem */
        static char reason[96];
        snprintf(reason, sizeof(reason),
                 "GPU GP0 unknown command 0x%02X (word 0x%08X)", opcode, val);
        psx_fatal_halt(reason);
    }

    if (word_count < 0) {
        /* Variable-length polyline command.
         * Mono  (0x48-0x4F): [cmd+color] [v0] [v1] ... [terminator]
         * Shaded(0x58-0x5F): [cmd+C0] [v0] [C1] [v1] ... [terminator]
         * Terminator: (word & 0xF000F000) == 0x50005000 (0x55555555). */
        int shaded = (opcode & 0x10) != 0;  /* 0x58+ = shaded, 0x48+ = mono */
        polyline_semi_trans = (val >> 25) & 1;
        polyline_color = rgb888_to_rgb555(val & 0xFFFFFFu);
        polyline_prev_c = polyline_color;
        polyline_has_prev = 0;
        gr_set_semi_transparency(polyline_semi_trans, (int)semi_transparency);
        gp0_state = shaded ? GP0_POLYLINE_SHADED : GP0_POLYLINE_MONO;
        gp0_draw_count++;
        /* Record polyline header (variable-length body not captured;
         * just enough so per-frame stream shows the polyline existed). */
        gp0_opcode_count[opcode]++;
        uint32_t hdr_only[1] = { val };
        gp0_ring_record(hdr_only, 1);
        return;
    }

    gp0_cmd_buf[0] = val;
    gp0_cmd_source_addr = gp0_next_source_addr;

    if (word_count == 1) {
        gp0_words_collected = 1;
        gp0_words_needed = 1;
        gp0_execute_command();
    } else {
        gp0_state = GP0_COLLECTING;
        gp0_words_collected = 1;
        gp0_words_needed = word_count;
    }
}

/* Wall-time sampler bracket (phase_profile): tag GP0 command processing —
 * rasterization / batching / VRAM transfer work on the emu thread — as its
 * own phase so it is separable from the guest code that issued the write.
 * Covers both the MMIO store chokepoint and DMA channel-2 feeds. */
void gpu_write_gp0(uint32_t val) {
    extern int g_exec_phase;
    int prev_phase = g_exec_phase;
    g_exec_phase = 4;
    gpu_write_gp0_body(val);
    g_exec_phase = prev_phase;
}

/* ---- GP1 write (0x1F801814 write) ---- */

static void gp1_reset(void) {
    /* GP1(00h): Reset GPU — clears FIFO/control state and disables display.
     * VRAM contents survive a GPU reset on real hardware; only power-on init
     * clears our backing store. */
    gpu_reset_state(0);
}

static void gp1_reset_command_buffer(void) {
    /* GP1(01h): Reset command buffer — clears FIFO, aborts current command */
    if (gp0_state == GP0_VRAM_WRITE) gr_vram_upload_set_open(0);
    gp0_state = GP0_IDLE;
    gp0_words_collected = 0;
    gp0_words_needed = 0;
    vram_write_remaining = 0;
}

static void gp1_ack_irq1(void) {
    /* GP1(02h): Acknowledge IRQ1 */
    irq1_flag = 0;
}

static void gp1_display_enable(uint32_t val) {
    /* GP1(03h): Display enable — bit 0: 0=on, 1=off */
    uint32_t next_disabled = val & 1;
    /* Native-wide reveal pixels have no guest-VRAM backing, so they cannot be
     * reconstructed from the canonical framebuffer after a scene change. Treat
     * the guest's ON->OFF transition as invalidation of those synthetic strips.
     * This is deliberately transition-scoped: MMX6 retains valid background
     * pixels across ordinary frames, so per-frame clearing pillarboxes gameplay. */
    if (ws_clear_reveal && next_disabled && !display_disabled && ws_mode == 2) {
        ws_clear_all_reveal_margins();
    }
    display_disabled = next_disabled;
}

static void gp1_dma_direction(uint32_t val) {
    /* GP1(04h): DMA direction — bits 0-1 */
    dma_direction = val & 3;
}

/* The game's own frames: a display start that moves (a double-buffered flip),
 * for the FPS readout (the VBlank count is the console's rate, not the game's). */
static uint64_t s_display_flips = 0;
uint64_t gpu_display_flips(void) { return s_display_flips; }

static void gp1_display_area_start(uint32_t val) {
    /* GP1(05h): Start of display area in VRAM
     * bits 0-9: X (in halfwords, 0-1023)
     * bits 10-18: Y (0-511) */
    if ((val & 0x3FFu) != display_area_x || ((val >> 10) & 0x1FFu) != display_area_y) s_display_flips++;
    display_area_x = val & 0x3FF;
    display_area_y = (val >> 10) & 0x1FF;
    ws_note_display_base(display_area_x);  /* learn the display buffer set (native-wide) */
    gpu_timeline_note(GTL_DISP_START, display_area_x, display_area_y);
}

static void gp1_h_display_range(uint32_t val) {
    /* GP1(06h): Horizontal display range
     * bits 0-11: X1
     * bits 12-23: X2 */
    h_display_x1 = val & 0xFFF;
    h_display_x2 = (val >> 12) & 0xFFF;
}

static void gp1_v_display_range(uint32_t val) {
    /* GP1(07h): Vertical display range
     * bits 0-9: Y1
     * bits 10-19: Y2 */
    uint32_t y1 = val & 0x3FF;
    uint32_t y2 = (val >> 10) & 0x3FF;
    uint32_t h = (y2 > y1) ? (y2 - y1) : 0u;
    /* MotK intro→crawl retargets the band while staying in depth24. Stale
     * trailing RGB from the prior movie would flash for a frame or two —
     * reset the upload span and hold present (skip Swap) for 3 vblanks. */
    if ((display_depth & 1u) && s_d24_prev_disp_h != 0u && h != s_d24_prev_disp_h) {
        s_d24_upload_x1 = 0;
        s_d24_present_hold = 3;
    }
    v_display_y1 = y1;
    v_display_y2 = y2;
    if (h != 0u) s_d24_prev_disp_h = h;
}

static void gp1_display_mode(uint32_t val) {
    /* GP1(08h): Display mode
     * bit 0-1: horizontal resolution 1 (0=256, 1=320, 2=512, 3=640)
     * bit 2: vertical resolution (0=240, 1=480)
     * bit 3: video mode (0=NTSC, 1=PAL)
     * bit 4: display area color depth (0=15bit, 1=24bit)
     * bit 5: vertical interlace (0=off, 1=on)
     * bit 6: horizontal resolution 2 (0=normal, 1=368)
     * bit 7: "reverseflag" */
    uint32_t new_depth = (val >> 4) & 1;
    hres1 = val & 3;
    vres = (val >> 2) & 1;
    video_mode = (uint32_t)psx_gp1_display_mode_is_pal(val);
    /* VBlank rate, Timer 1 HBlank clock and host pacing follow the live
     * standard, whatever the disc region (psx_video_timing.h). */
    (void)psx_video_timing_set_pal((int)video_mode);
    if (new_depth != display_depth)
        s_d24_upload_x1 = 0; /* rising/falling: drop stale coverage */
    display_depth = new_depth;
    vertical_interlace = (val >> 5) & 1;
    /* GPUSTAT.13 holds the legacy constant 0 in progressive (see the vblank
     * field flip); clear it on the switch so a title that toggles interlace
     * on and back off doesn't leave the field bit latched at 1. */
    if (!vertical_interlace)
        interlace_field = 0;
    hres2 = (val >> 6) & 1;
    reverse_flag = (val >> 7) & 1;
}

static void gp1_get_info(uint32_t val) {
    /* GP1(10h): Get GPU info — writes result to GPUREAD latch.
     * Mednafen-psx masks the subcommand to 4 bits (val & 0x0F) and
     * services cases 2..5, 7, 8. Tomba's ResetGraph() uses param 7 to
     * read the GPU version (must be 2) to pick its video-mode path —
     * the wrong value here lands the game on a no-draw branch. */
    uint32_t which = val & 0x0F;
    switch (which) {
        case 2: /* texture window */
            gpuread_latch = texture_window_value;
            break;
        case 3: /* draw area top-left */
            gpuread_latch = draw_area_left | (draw_area_top << 10);
            break;
        case 4: /* draw area bottom-right */
            gpuread_latch = draw_area_right | (draw_area_bottom << 10);
            break;
        case 5: /* draw offset */
            gpuread_latch = ((uint32_t)draw_offset_x & 0x7FFu) |
                            (((uint32_t)draw_offset_y & 0x7FFu) << 11);
            break;
        case 7: /* GPU version (real-hw + mednafen return 2) */
            gpuread_latch = 2;
            break;
        case 8: /* unknown info index, real hw / mednafen return 0 */
            gpuread_latch = 0;
            break;
        default:
            /* N=0,1,6,9..15: leave latch unchanged */
            break;
    }
}

void gpu_write_gp1(uint32_t val) {
    uint32_t cmd = (val >> 24) & 0x3F;

    switch (cmd) {
        case 0x00: gp1_reset(); break;
        case 0x01: gp1_reset_command_buffer(); break;
        case 0x02: gp1_ack_irq1(); break;
        case 0x03: gp1_display_enable(val); break;
        case 0x04: gp1_dma_direction(val); break;
        case 0x05: gp1_display_area_start(val); break;
        case 0x06: gp1_h_display_range(val); break;
        case 0x07: gp1_v_display_range(val); break;
        case 0x08: gp1_display_mode(val); break;
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
            gp1_get_info(val); break;
        default: {
            static char reason[96];
            snprintf(reason, sizeof(reason),
                     "GPU GP1 unknown command 0x%02X (word 0x%08X)", cmd, val);
            psx_fatal_halt(reason);
        }
    }
}

/* ---- boot snapshot: complete GPU register state (see boot_state.h) ---- */
#define GPU_SNAP_FIELDS(X) \
    X(texpage_x) X(texpage_y) X(semi_transparency) X(texpage_colors) \
    X(dither_enabled) X(draw_to_display) X(texture_disable) X(texture_window_value) \
    X(set_mask_bit) X(check_mask_bit) \
    X(interlace_field) X(reverse_flag) \
    X(draw_area_left) X(draw_area_top) X(draw_area_right) X(draw_area_bottom) \
    X(draw_offset_x) X(draw_offset_y) \
    X(hres1) X(hres2) X(vres) X(video_mode) X(display_depth) X(vertical_interlace) \
    X(display_disabled) X(irq1_flag) X(dma_direction) X(lcf) \
    X(display_area_x) X(display_area_y) \
    X(h_display_x1) X(h_display_x2) X(v_display_y1) X(v_display_y2) \
    X(gpuread_latch) \
    X(gp0_state) \
    X(gp0_cmd_buf) X(gp0_words_collected) X(gp0_words_needed) \
    X(gp0_next_source_addr) X(gp0_cmd_source_addr) \
    X(polyline_color) X(polyline_prev_x) X(polyline_prev_y) X(polyline_prev_c) \
    X(polyline_semi_trans) X(polyline_has_prev) \
    X(vram_write_x) X(vram_write_y) X(vram_write_w) X(vram_write_h) \
    X(vram_write_col) X(vram_write_row) X(vram_write_remaining) \
    X(vram_read_active) X(vram_read_x) X(vram_read_y) X(vram_read_w) X(vram_read_h) \
    X(vram_read_col) X(vram_read_row)
#define GPU_COSIM_SNAP_FIELDS(X) \
    X(texpage_x) X(texpage_y) X(semi_transparency) X(texpage_colors) \
    X(dither_enabled) X(draw_to_display) X(texture_disable) X(texture_window_value) \
    X(set_mask_bit) X(check_mask_bit) \
    X(interlace_field) X(reverse_flag) \
    X(draw_area_left) X(draw_area_top) X(draw_area_right) X(draw_area_bottom) \
    X(draw_offset_x) X(draw_offset_y) \
    X(hres1) X(hres2) X(vres) X(video_mode) X(display_depth) X(vertical_interlace) \
    X(display_disabled) X(irq1_flag) X(dma_direction) X(lcf) \
    X(display_area_x) X(display_area_y) \
    X(h_display_x1) X(h_display_x2) X(v_display_y1) X(v_display_y2) \
    X(gpuread_latch) \
    X(gp0_state) \
    X(gp0_cmd_buf) X(gp0_words_collected) X(gp0_words_needed) \
    X(polyline_color) X(polyline_prev_x) X(polyline_prev_y) X(polyline_prev_c) \
    X(polyline_semi_trans) X(polyline_has_prev) \
    X(vram_write_x) X(vram_write_y) X(vram_write_w) X(vram_write_h) \
    X(vram_write_col) X(vram_write_row) X(vram_write_remaining) \
    X(vram_read_active) X(vram_read_x) X(vram_read_y) X(vram_read_w) X(vram_read_h) \
    X(vram_read_col) X(vram_read_row)
#include "pst_wire.h"

/* GPU snap fields are scalars / u32 arrays — emit as LE u32/i32 (no struct pad). */
static int gpu_snap_emit(PstW *w) {
#define WU(f) do { if (!pst_w_u32(w, (uint32_t)(f))) return 0; } while (0)
#define WI(f) do { if (!pst_w_i32(w, (int32_t)(f))) return 0; } while (0)
#define WH(f) do { if (!pst_w_u16(w, (uint16_t)(f))) return 0; } while (0)
    WU(texpage_x); WU(texpage_y); WU(semi_transparency); WU(texpage_colors);
    WU(dither_enabled); WU(draw_to_display); WU(texture_disable); WU(texture_window_value);
    WU(set_mask_bit); WU(check_mask_bit);
    WU(interlace_field); WU(reverse_flag);
    WU(draw_area_left); WU(draw_area_top); WU(draw_area_right); WU(draw_area_bottom);
    WI(draw_offset_x); WI(draw_offset_y);
    WU(hres1); WU(hres2); WU(vres); WU(video_mode); WU(display_depth); WU(vertical_interlace);
    WU(display_disabled); WU(irq1_flag); WU(dma_direction); WU(lcf);
    WU(display_area_x); WU(display_area_y);
    WU(h_display_x1); WU(h_display_x2); WU(v_display_y1); WU(v_display_y2);
    WU(gpuread_latch);
    WU((uint32_t)gp0_state);
    for (int i = 0; i < 16; i++) WU(gp0_cmd_buf[i]);
    WI(gp0_words_collected); WI(gp0_words_needed);
    WU(gp0_next_source_addr); WU(gp0_cmd_source_addr);
    WH(polyline_color); WI(polyline_prev_x); WI(polyline_prev_y); WH(polyline_prev_c);
    WI(polyline_semi_trans); WI(polyline_has_prev);
    WH(vram_write_x); WH(vram_write_y); WH(vram_write_w); WH(vram_write_h);
    WH(vram_write_col); WH(vram_write_row); WU(vram_write_remaining);
    WI(vram_read_active); WH(vram_read_x); WH(vram_read_y); WH(vram_read_w); WH(vram_read_h);
    WH(vram_read_col); WH(vram_read_row);
    /* Depth24 present helpers (MotK FMV) — must resume with upload span. */
    WU(s_d24_upload_x1); WI(s_d24_present_hold | 0x200 | (s_d24_present_skip ? 0x100 : 0));
    WU(s_d24_prev_disp_h);
#undef WU
#undef WI
#undef WH
    return 1;
}
static int gpu_snap_parse(PstR *r) {
    uint32_t u; int32_t i; uint16_t h;
#define RU(f) do { if (!pst_r_u32(r, &u)) return 0; (f) = u; } while (0)
#define RI(f) do { if (!pst_r_i32(r, &i)) return 0; (f) = i; } while (0)
#define RH(f) do { if (!pst_r_u16(r, &h)) return 0; (f) = h; } while (0)
    RU(texpage_x); RU(texpage_y); RU(semi_transparency); RU(texpage_colors);
    RU(dither_enabled); RU(draw_to_display); RU(texture_disable); RU(texture_window_value);
    RU(set_mask_bit); RU(check_mask_bit);
    RU(interlace_field); RU(reverse_flag);
    RU(draw_area_left); RU(draw_area_top); RU(draw_area_right); RU(draw_area_bottom);
    RI(draw_offset_x); RI(draw_offset_y);
    RU(hres1); RU(hres2); RU(vres); RU(video_mode); RU(display_depth); RU(vertical_interlace);
    RU(display_disabled); RU(irq1_flag); RU(dma_direction); RU(lcf);
    RU(display_area_x); RU(display_area_y);
    RU(h_display_x1); RU(h_display_x2); RU(v_display_y1); RU(v_display_y2);
    RU(gpuread_latch);
    if (!pst_r_u32(r, &u)) return 0;
    gp0_state = (Gp0State)u;
    for (int k = 0; k < 16; k++) RU(gp0_cmd_buf[k]);
    RI(gp0_words_collected); RI(gp0_words_needed);
    RU(gp0_next_source_addr); RU(gp0_cmd_source_addr);
    RH(polyline_color); RI(polyline_prev_x); RI(polyline_prev_y); RH(polyline_prev_c);
    RI(polyline_semi_trans); RI(polyline_has_prev);
    RH(vram_write_x); RH(vram_write_y); RH(vram_write_w); RH(vram_write_h);
    RH(vram_write_col); RH(vram_write_row); RU(vram_write_remaining);
    RI(vram_read_active); RH(vram_read_x); RH(vram_read_y); RH(vram_read_w); RH(vram_read_h);
    RH(vram_read_col); RH(vram_read_row);
    RU(s_d24_upload_x1); RI(s_d24_present_hold); RU(s_d24_prev_disp_h);
    /* Bit 9 marks an explicit skip flag (bit 8). GP1 can arm a positive
     * count before VBlank, when skip is still false: that state must round
     * trip exactly. Older snaps without the marker infer skip from count. */
    s_d24_present_skip = (s_d24_present_hold & 0x200)
        ? (s_d24_present_hold & 0x100) != 0
        : (s_d24_present_hold & 0x100) || (s_d24_present_hold & 0xFF) > 0;
    s_d24_present_hold &= 0xFF;
#undef RU
#undef RI
#undef RH
    return 1;
}

uint32_t gpu_snapshot_bytes(void) {
    PstW w; pst_w_init(&w, NULL, 0);
    (void)gpu_snap_emit(&w);
    return (uint32_t)w.written;
}
void gpu_snapshot_write(uint8_t *p) {
    PstW w; uint32_t n = gpu_snapshot_bytes();
    pst_w_init(&w, p, n);
    (void)gpu_snap_emit(&w);
}
uint32_t gpu_cosim_snapshot_bytes(void) { return gpu_snapshot_bytes(); }
void gpu_cosim_snapshot_write(uint8_t *p) { gpu_snapshot_write(p); }
void gpu_cosim_dump(char *out, int cap) {
    if (!out || cap <= 0) return;
    char *p = out;
    size_t rem = (size_t)cap;
#define APPEND(fmt, ...) do { \
        if (rem <= 1) break; \
        int w = snprintf(p, rem, fmt, __VA_ARGS__); \
        if (w < 0) { out[0] = 0; return; } \
        if ((size_t)w >= rem) { p += rem - 1; rem = 1; } \
        else { p += (size_t)w; rem -= (size_t)w; } \
    } while (0)
#define X(f) APPEND(" %s %llx", #f, (unsigned long long)(uint32_t)(f));
    X(texpage_x) X(texpage_y) X(semi_transparency) X(texpage_colors)
    X(dither_enabled) X(draw_to_display) X(texture_disable) X(texture_window_value)
    X(set_mask_bit) X(check_mask_bit)
    X(interlace_field) X(reverse_flag)
    X(draw_area_left) X(draw_area_top) X(draw_area_right) X(draw_area_bottom)
    X(draw_offset_x) X(draw_offset_y)
    X(hres1) X(hres2) X(vres) X(video_mode) X(display_depth) X(vertical_interlace)
    X(display_disabled) X(irq1_flag) X(dma_direction) X(lcf)
    X(display_area_x) X(display_area_y)
    X(h_display_x1) X(h_display_x2) X(v_display_y1) X(v_display_y2)
    X(gpuread_latch)
    X(gp0_state)
    for (int i = 0; i < 16; i++) {
        APPEND(" gp0_cmd_buf%d %08x", i, gp0_cmd_buf[i]);
    }
    X(gp0_words_collected) X(gp0_words_needed)
    X(gp0_next_source_addr) X(gp0_cmd_source_addr)
    X(polyline_color) X(polyline_prev_x) X(polyline_prev_y) X(polyline_prev_c)
    X(polyline_semi_trans) X(polyline_has_prev)
    X(vram_write_x) X(vram_write_y) X(vram_write_w) X(vram_write_h)
    X(vram_write_col) X(vram_write_row) X(vram_write_remaining)
    X(vram_read_active) X(vram_read_x) X(vram_read_y) X(vram_read_w) X(vram_read_h)
    X(vram_read_col) X(vram_read_row)
#undef X
    APPEND("%s", "\n");
#undef APPEND
}
int gpu_snapshot_read(const uint8_t *p, uint32_t len) {
    PstR r;
    if (len != gpu_snapshot_bytes()) return 0;
    pst_r_init(&r, p, len);
    if (!gpu_snap_parse(&r)) return 0;
    (void)psx_video_timing_set_pal((int)video_mode);
    ws_reset_scene_history();
    ws_scene_hold_reset(&s_ws_scene_hold);
    ws_hud_anchor_clear(ws_hud_anchor_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    ws_hud_anchor_clear(ws_background_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    ws_background_tags_used = 0;
    ws_hud_anchor_clear(ws_reveal_clear_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    ws_hud_anchor_clear(ws_screen_mask_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    ws_radial_mask_clear(ws_radial_screen_mask_tags);
    ws_repeat_rect_tag_clear(ws_repeat_rect_tags);
    /* Sync renderer clip/scissor to restored GP0(E3/E4); vars alone leave GL
     * on a stale draw area after savestate load. */
    gr_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
    /* GP0(E1h) bit 9 is restored with the registers; the renderer only
     * learns it from E1, so push it now (else dithering keeps the pre-load
     * setting until the game's next E1). */
    gr_set_dither((int)dither_enabled);
    /* A state saved mid-A0 resumes streaming its payload into the array. */
    gr_vram_upload_set_open(gp0_state == GP0_VRAM_WRITE);
    ws_nw_sync_target();
    return 1;
}
/* ---- Render-pass GPU checkpoint (render_pass.c) ---------------------------
 * The same register set as a savestate (gpu_snap_emit), restored WITHOUT
 * gpu_snapshot_read's side effects. Packet provenance belongs to the RAM it
 * describes: a synthetic builder can rewrite and retag a canonical packet,
 * so rolling its RAM back must also restore its original tags, not clear them.
 * The renderer's mirrored draw state (area, offset, texture window,
 * mask bits, native-wide target) is re-synced exactly as GP0(E2..E6) would. */
typedef struct {
    WsTag ws_tags[WS_TAG_BUCKETS];
    WsHudAnchorTag ws_hud_anchor_tags[WS_HUD_ANCHOR_TABLE_SIZE];
    WsHudAnchorTag ws_background_tags[WS_HUD_ANCHOR_TABLE_SIZE];
    WsHudAnchorTag ws_reveal_clear_tags[WS_HUD_ANCHOR_TABLE_SIZE];
    WsHudAnchorTag ws_screen_mask_tags[WS_HUD_ANCHOR_TABLE_SIZE];
    WsRadialScreenMaskTag ws_radial_screen_mask_tags[WS_RADIAL_MASK_TAG_COUNT];
    WsRepeatRectTag ws_repeat_rect_tags[WS_REPEAT_RECT_TAG_TABLE_SIZE];
    WsPrimitiveRole ws_primitive_roles[WS_ROLE_BUCKETS];
    WsUiPrepassItem ws_ui_prepass[WS_UI_PREPASS_MAX];
    WsUiPrepassNode ws_ui_prepass_nodes[WS_UI_PREPASS_NODE_MAX];
    uint32_t ws_ui_prepass_count, ws_ui_prepass_node_count;
    uint16_t ws_ui_prepass_rank;
    int ws_auto_ui_dense, ws_shared_ui_copy;
    uint32_t ws_background_tag_frame, ws_last_tag_stamp;
    int ws_background_tags_used;
    uint32_t s_bg_phase_frame;
    int s_bg_phase_over;
    /* Synthetic GTE/GP0 draws update these signals, and native-wide target
     * selection reads them even though no guest frame has elapsed. */
    uint32_t ws_gte_frame, ws_gte_count, ws_gte_prev_verts;
    uint32_t ws_last_gte_stamp, ws_last_world3d_stamp, ws_sust_world3d_stamp;
    uint32_t ws_ovh_frame, ws_ovh_count, ws_ovh_prev;
    uint32_t ws_last_ovh_stamp, ws_sust_ovh_stamp;
    WsSceneLatch ws_scene_latch;
    WsBackdropExtent ws_bdx;
    WsSceneHold s_ws_scene_hold;
    uint32_t s_ws_fmv_frame_cache;
    int s_ws_fmv_cached;
} GpuPassPresentationState;
static GpuPassPresentationState s_pass_presentation;

/* One transfer list keeps save and restore symmetric. The prepass arrays are
 * read only through their counts, so copy just their populated prefixes.
 * Counters/rings remain cumulative; configuration and callbacks are unchanged. */
static void gpu_pass_provenance_transfer(int restore) {
#define PASS_ARRAY(name) do { \
    if (restore) memcpy(name, s_pass_presentation.name, sizeof(name)); \
    else memcpy(s_pass_presentation.name, name, sizeof(name)); \
} while (0)
#define PASS_VALUE(name) do { \
    if (restore) name = s_pass_presentation.name; \
    else s_pass_presentation.name = name; \
} while (0)
    PASS_ARRAY(ws_tags);
    PASS_ARRAY(ws_hud_anchor_tags);
    PASS_ARRAY(ws_background_tags);
    PASS_ARRAY(ws_reveal_clear_tags);
    PASS_ARRAY(ws_screen_mask_tags);
    PASS_ARRAY(ws_radial_screen_mask_tags);
    PASS_ARRAY(ws_repeat_rect_tags);
    PASS_ARRAY(ws_primitive_roles);
    PASS_VALUE(ws_ui_prepass_count);
    PASS_VALUE(ws_ui_prepass_node_count);
    PASS_VALUE(ws_ui_prepass_rank);
    PASS_VALUE(ws_auto_ui_dense);
    PASS_VALUE(ws_shared_ui_copy);
    PASS_VALUE(ws_background_tag_frame);
    PASS_VALUE(ws_background_tags_used);
    PASS_VALUE(ws_last_tag_stamp);
    PASS_VALUE(s_bg_phase_frame);
    PASS_VALUE(s_bg_phase_over);
    if (restore) {
        memcpy(ws_ui_prepass, s_pass_presentation.ws_ui_prepass,
               ws_ui_prepass_count * sizeof(ws_ui_prepass[0]));
        memcpy(ws_ui_prepass_nodes, s_pass_presentation.ws_ui_prepass_nodes,
               ws_ui_prepass_node_count * sizeof(ws_ui_prepass_nodes[0]));
    } else {
        memcpy(s_pass_presentation.ws_ui_prepass, ws_ui_prepass,
               ws_ui_prepass_count * sizeof(ws_ui_prepass[0]));
        memcpy(s_pass_presentation.ws_ui_prepass_nodes, ws_ui_prepass_nodes,
               ws_ui_prepass_node_count * sizeof(ws_ui_prepass_nodes[0]));
    }
#undef PASS_ARRAY
#undef PASS_VALUE
}

static void gpu_pass_scene_transfer(int restore) {
#define PASS_VALUE(name) do { \
    if (restore) name = s_pass_presentation.name; \
    else s_pass_presentation.name = name; \
} while (0)
    PASS_VALUE(ws_gte_frame);
    PASS_VALUE(ws_gte_count);
    PASS_VALUE(ws_gte_prev_verts);
    PASS_VALUE(ws_last_gte_stamp);
    PASS_VALUE(ws_last_world3d_stamp);
    PASS_VALUE(ws_sust_world3d_stamp);
    PASS_VALUE(ws_ovh_frame);
    PASS_VALUE(ws_ovh_count);
    PASS_VALUE(ws_ovh_prev);
    PASS_VALUE(ws_last_ovh_stamp);
    PASS_VALUE(ws_sust_ovh_stamp);
    PASS_VALUE(ws_scene_latch);
    PASS_VALUE(ws_bdx);
    PASS_VALUE(s_ws_scene_hold);
    PASS_VALUE(s_ws_fmv_frame_cache);
    PASS_VALUE(s_ws_fmv_cached);
#undef PASS_VALUE
}

static uint8_t  s_pass_regs[512];
static uint32_t s_pass_regs_len;
static uint32_t s_pass_poll_count;
static int32_t  s_pass_doff_min, s_pass_doff_max;
static uint32_t s_pass_doff_cnt;
static GpuVerticalSplitTrace s_pass_split_this;

int gpu_pass_checkpoint_save(void) {
    PstW w;
    uint32_t n = gpu_snapshot_bytes();
    if (n > sizeof s_pass_regs) return 0;
    pst_w_init(&w, s_pass_regs, n);
    if (!gpu_snap_emit(&w)) return 0;
    s_pass_regs_len = n;
    s_pass_poll_count = gpustat_poll_count;
    s_pass_doff_min = g_doff_min_this;
    s_pass_doff_max = g_doff_max_this;
    s_pass_doff_cnt = g_doff_cnt_this;
    s_pass_split_this = split_trace_this;
    gpu_pass_provenance_transfer(0);
    gpu_pass_scene_transfer(0);
    return 1;
}

void gpu_pass_checkpoint_restore(void) {
    PstR r;
    if (!s_pass_regs_len) return;
    pst_r_init(&r, s_pass_regs, s_pass_regs_len);
    (void)gpu_snap_parse(&r);
    gpustat_poll_count = s_pass_poll_count;
    g_doff_min_this = s_pass_doff_min;
    g_doff_max_this = s_pass_doff_max;
    g_doff_cnt_this = s_pass_doff_cnt;
    split_trace_this = s_pass_split_this;
    gpu_pass_provenance_transfer(1);
    gpu_pass_scene_transfer(1);
    gr_set_texture_window(texture_window_value);
    gr_set_draw_area((int)draw_area_left, (int)draw_area_top,
                     (int)draw_area_right, (int)draw_area_bottom);
    gr_set_draw_offset(draw_offset_x, draw_offset_y);
    gr_set_mask_bits((int)set_mask_bit, (int)check_mask_bit);
    gr_set_dither((int)dither_enabled);
    ws_nw_sync_target();
    /* Target sync may classify the scene before render_pass.c restores RAM.
     * Do not retain history learned from the synthetic draw or that query. */
    gpu_pass_scene_transfer(1);
    s_pass_regs_len = 0;
}

/* FNV-1a over the checkpointed register set (PSX_RENDER_PASS_VERIFY). */
uint64_t gpu_pass_state_hash(void) {
    uint8_t buf[512];
    PstW w;
    uint64_t h = 1469598103934665603ULL;
    uint32_t n = gpu_snapshot_bytes();
    if (n > sizeof buf) return 0;
    pst_w_init(&w, buf, n);
    if (!gpu_snap_emit(&w)) return 0;
    for (uint32_t i = 0; i < n; i++) h = (h ^ buf[i]) * 1099511628211ULL;
    h = (h ^ gpustat_poll_count) * 1099511628211ULL;
    return h;
}

uint16_t* gpu_get_vram_ptr(void){ return vram; }
uint32_t  gpu_get_vram_bytes(void){ return (uint32_t)sizeof(vram); }
