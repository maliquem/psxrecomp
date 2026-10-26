/* video_texture_filter.c - see video_texture_filter.h. */
#include "video_texture_filter.h"

#include <string.h>

int psx_texture_filter_from_ui(int value) {
    return (value >= PSX_TEXTURE_FILTER_NEAREST && value < PSX_TEXTURE_FILTER_COUNT)
               ? value : PSX_TEXTURE_FILTER_NEAREST;
}

static const char *const k_filter_names[PSX_TEXTURE_FILTER_COUNT] = {
    "nearest", "bilinear", "stable", "xbr", "sharp_bilinear", "jinc2",
    "monotonic_cubic", "dcci", "scale2x", "scale3x", "mmpx",
};
static const char *const k_edge_names[PSX_TEXTURE_EDGE_COUNT] = {"ps1", "clean", "soft"};

int psx_texture_filter_parse(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < PSX_TEXTURE_FILTER_COUNT; i++)
        if (!strcmp(name, k_filter_names[i])) return i;
    if (!strcmp(name, "stable_world")) return PSX_TEXTURE_FILTER_STABLE_WORLD;
    if (!strcmp(name, "epx")) return PSX_TEXTURE_FILTER_SCALE2X;
    if (!strcmp(name, "cubic")) return PSX_TEXTURE_FILTER_MONOTONIC_CUBIC;
    return -1;
}
const char *psx_texture_filter_name(int value) {
    return value >= 0 && value < PSX_TEXTURE_FILTER_COUNT ? k_filter_names[value] : "nearest";
}
int psx_texture_edge_parse(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < PSX_TEXTURE_EDGE_COUNT; i++)
        if (!strcmp(name, k_edge_names[i])) return i;
    return -1;
}
const char *psx_texture_edge_name(int value) {
    return value >= 0 && value < PSX_TEXTURE_EDGE_COUNT ? k_edge_names[value] : "ps1";
}
