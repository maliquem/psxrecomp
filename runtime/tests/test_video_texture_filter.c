/* video_texture_filter_test: launcher texture-filter values to the enum. */
#include "video_texture_filter.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main(void) {
    CHECK(psx_texture_filter_from_ui(0) == PSX_TEXTURE_FILTER_NEAREST);
    CHECK(psx_texture_filter_from_ui(1) == PSX_TEXTURE_FILTER_BILINEAR);
    CHECK(psx_texture_filter_from_ui(2) == PSX_TEXTURE_FILTER_STABLE_WORLD);
    CHECK(psx_texture_filter_from_ui(3) == PSX_TEXTURE_FILTER_XBR);  /* was collapsed to bilinear */
    CHECK(psx_texture_filter_from_ui(PSX_TEXTURE_FILTER_MMPX) == PSX_TEXTURE_FILTER_MMPX);
    CHECK(psx_texture_filter_from_ui(PSX_TEXTURE_FILTER_COUNT) == PSX_TEXTURE_FILTER_NEAREST);
    CHECK(psx_texture_filter_from_ui(-1) == PSX_TEXTURE_FILTER_NEAREST);
    /* config names round-trip; aliases; unknown names */
    for (int i = 0; i < PSX_TEXTURE_FILTER_COUNT; i++)
        CHECK(psx_texture_filter_parse(psx_texture_filter_name(i)) == i);
    CHECK(psx_texture_filter_parse("epx") == PSX_TEXTURE_FILTER_SCALE2X);
    CHECK(psx_texture_filter_parse("stable_world") == PSX_TEXTURE_FILTER_STABLE_WORLD);
    CHECK(psx_texture_filter_parse("lanczos") == -1);
    for (int i = 0; i < PSX_TEXTURE_EDGE_COUNT; i++)
        CHECK(psx_texture_edge_parse(psx_texture_edge_name(i)) == i);
    CHECK(psx_texture_edge_parse("blend") == -1);
    if (!fails) printf("video_texture_filter_test: ok\n");
    return fails ? 1 : 0;
}
