/* video_texture_filter.h - the one texture-filter enum for settings.toml,
 * the launcher handoff and live apply. */
#ifndef PSX_VIDEO_TEXTURE_FILTER_H
#define PSX_VIDEO_TEXTURE_FILTER_H
#ifdef __cplusplus
extern "C" {
#endif

/* Values match [video] texture_filtering and gr_set_texture_filter().
 * STABLE_WORLD is set by mods, not by settings. Past XBR: the filters of
 * gl_texfilter.inc (OpenGL renderer; the others draw them as before). */
#define PSX_TEXTURE_FILTER_NEAREST         0
#define PSX_TEXTURE_FILTER_BILINEAR        1
#define PSX_TEXTURE_FILTER_STABLE_WORLD    2
#define PSX_TEXTURE_FILTER_XBR             3
#define PSX_TEXTURE_FILTER_SHARP_BILINEAR  4
#define PSX_TEXTURE_FILTER_JINC2           5
#define PSX_TEXTURE_FILTER_MONOTONIC_CUBIC 6
#define PSX_TEXTURE_FILTER_DCCI            7
#define PSX_TEXTURE_FILTER_SCALE2X         8
#define PSX_TEXTURE_FILTER_SCALE3X         9
#define PSX_TEXTURE_FILTER_MMPX            10
#define PSX_TEXTURE_FILTER_COUNT           11

/* A filtered texture's outline ([video] texture_edges and the class ones):
 * PS1   the nearest texel decides which pixels are drawn (the PS1 cutout);
 * CLEAN the filter decides it (its opacity cut at one half): a smoother
 *       outline, hard-edged ("no edge blending");
 * SOFT  that outline blended by the filtered opacity. */
#define PSX_TEXTURE_EDGE_PS1   0
#define PSX_TEXTURE_EDGE_CLEAN 1
#define PSX_TEXTURE_EDGE_SOFT  2
#define PSX_TEXTURE_EDGE_COUNT 3

/* The classes a primitive's filter comes from (gl_renderer_set_texture_filter_class):
 * WORLD textured polygons PGXP proved 3D; SPRITE rectangles and untracked
 * polygons; UI what the game puts in its ordering table's front. */
#define PSX_TEXTURE_CLASS_WORLD  0
#define PSX_TEXTURE_CLASS_SPRITE 1
#define PSX_TEXTURE_CLASS_UI     2

/* A launcher value to the enum: in-range values pass through (xBR included),
 * anything else is nearest, the native default. Older launchers send 0/1. */
int psx_texture_filter_from_ui(int value);

/* Config names: "nearest", "bilinear", "stable", "xbr", "sharp_bilinear",
 * "jinc2", "monotonic_cubic", "dcci", "scale2x" (or "epx"), "scale3x",
 * "mmpx"; edges "ps1", "clean", "soft". Parse: the value, or -1. */
int psx_texture_filter_parse(const char *name);
const char *psx_texture_filter_name(int value);
int psx_texture_edge_parse(const char *name);
const char *psx_texture_edge_name(int value);

#ifdef __cplusplus
}
#endif
#endif
