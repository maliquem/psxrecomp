# Post effects

Elective effects on the composed game image (OpenGL). All of them are off by
default. With none active, nothing below runs and the frame is the stock one.

Every effect is a **chain** of full-screen fragment passes run by one
executor (`runtime/src/gl_postfx.inc`). Three stages run in order, each from
a fresh copy of the image:

| stage | switch | chain |
|---|---|---|
| bloom | `[video] bloom = 0..2` (`PSX_BLOOM`) | built-in `bloom` |
| user | `[video] post_shader = "<file>.pfx"` (`PSX_POST_SHADER`) | your preset |
| AA | `[video] antialiasing_mode = "fxaa" \| "fxaa_hq"` | built-in `fxaa` / `fxaa_hq` |

They run on whichever thread presents (the render thread included), before
hold-last capture, screenshots and the OSD. Smooth motion's generated frames
run bloom and the user chain; AA stays on real frames, as before. Passes
marked `generated reuse` (bloom's bright and blur passes) are not run again
on generated frames: those reuse the last real frame's glow and mask and run
only the rest (bloom's masked composite, over their own picture).

`source` is a full-size copy of the image, made only when a pass that runs
reads it; `source_down` is the image blitted straight to that pass's size.
Bloom reads only `source_down` and composites with `blend screen`, so it never
copies the full image. The executor
owns targets, programs, uniforms and GL state, and puts back everything it
touches.

## Preset format (`.pfx`)

```
pfx 1                              # required first line
pass <name>                        # the last pass writes the window
  shader <file.glsl|builtin:name>  # file paths resolve next to the preset
  input <sampler> <source|source_down|mask|mask3d|depth|shadow|sprshadow|sprite|firemap|previous|<earlier pass>>   # up to 6
  scale <f>                        # target size as a fraction of the game rect (default 1)
  format rgba8|rgba16f             # default rgba8
  filter linear|nearest            # how later passes sample this target
  uniform <name> <f> [f [f [f]]]   # float .. vec4, up to 12 per pass
  blend replace|screen             # last pass: write, or screen o + dst*(1-o)
                                   # onto the window (discard keeps dst exactly)
  generated reuse|run              # Smooth motion generated frames: keep this pass's
                                   # target from the last real frame (default: run)
```

A shader file is a GLSL 330 fragment body. It is appended to this header:

```glsl
in vec2 v_uv; out vec4 o;      // v_uv: 0..1 over the game rect, bottom-up
uniform vec2 u_rcp, u_size;    // this pass's target: 1/size and size in px
uniform ivec2 u_org;           // window origin of the game rect
uniform float u_time;          // wall-clock seconds (a flame's flicker)
uniform sampler2D <sampler>;   // one per input
```

Built-in shaders: `blur` (9-tap Gaussian along `u_dir`, in px), `fxaa`,
`bloom_bright` and `bloom_composite`. The built-in presets are listed in
`runtime/src/postfx_chain.c`. A preset that fails to parse, or a pass that
fails to compile, is logged and its stage skipped.

Example: invert the image.

```
pfx 1
pass invert
  shader invert.glsl
  input u_tex source
```

with `invert.glsl` containing:

```glsl
void main(){ o = vec4(1.0 - texelFetch(u_tex, ivec2(gl_FragCoord.xy) - u_org, 0).rgb, 1.0); }
```

## The mask input

`mask` is 1 where the last thing drawn was a 3D primitive (proven from GTE
data, so it needs PGXP geometry) or an additive semi-transparent primitive
(blend modes 1 and 3). It is 0 where an opaque 2D primitive drew last, such as
a sprite, a fill or the HUD. Only while an active chain reads `mask` does every
draw keep that mark in stencil bit 1 (bit 0 is the PS1 mask-bit mirror). The
mark follows the picture through native-wide centre copies and Smooth motion
surfaces. At a real frame's present it is resolved in the presented surface's
own pixels (an R8 target sharing that surface's depth-stencil renderbuffer,
so no depth-stencil blit, which costs the CPU milliseconds on macOS) and mapped
to a window-sized R8 texture. Native-wide takes the centre's marks from the
hr surface directly. Generated frames reuse the newer real frame's mask. A chain that
does not read `mask` costs none of this.

Bloom uses the mask at both ends: only lit pixels feed its bright pass, and
only lit pixels take the glow back. HUD pixels are therefore never changed,
even next to a lamp.

## The shadow input and `[video.shadow]`

`shadow` is a depth map of the scene seen from a light the reading pass
gives (`u_sun` azimuth/elevation in degrees, or `u_shadowdir`), 2048² or the
size the pass's `u_smsize` asks (1024..8192, a power of two). A pass that
reads it also gets `u_smx`, `u_smy`, `u_smz`, `u_sms` (where a view-space
point lands in the map) and, when the camera is known, `u_world`,
`u_worldup` and `u_shadowdir` (the world's axes and the light in view space,
so the light stays fixed in the world when the camera turns). Nothing of this
runs unless an active chain reads `shadow`. `sprshadow` is the same map
(same light, size and mapping) holding only the sprites (below), so a reader
can soften the scenery's shadow edges and keep the sprites' pixel-art
silhouettes sharp; the reading pass's `u_ss` x 0 leaves it empty and turns
sprites off altogether (no casters, no shading, the game's own blobs stay).
`u_ss` y is the shaders' own: the Sun presets light the scenery at 1 and, at
0, keep its colours and only darken where a sprite's shadow falls (the
sprites' shadows alone, a menu's "GI Off, SS On").

While a texture `off_textures` names was drawn in the last few VBlanks (a
world map's paper or frame: no place under the sun), every stage whose chain
reads `shadow` is skipped (`gpu_shadow_off_active`), whatever loaded it.
Only opaque primitives count: a glow sharing the palette (BoF4's sun glare
in the dragon's cutscene, CLUT 0x3e00 on an additive page) switched the
effects off and on every few frames.

The sun can be set per place: `sun_areas = ["0xAREA:az:el", "0xAREA:off"]`
gives an area its own azimuth and elevation (degrees), over the pass's
`u_sun`, or no sun at all ("off": a cave; the stages reading `shadow` are
skipped and the game's own blobs stay). The area is the id a game mod gives
(`gl_renderer_shadow_area`; BoF4: a hash of the map data, so each screen and
each battle arena has its own), else a hash of `scene_keys`. `shadow_cache`
prints the current one (`scene`) and its sun (`scene_sun`), to fill the list
as a player goes. The camera and the sprites' foot points are followed in an
area with no sun too, for the fire lights below.

The casters, best source first:

1. **The scene's own geometry** (`gl_renderer_shadow_world`, world-space
   triangles a game mod reads from the game's map data) plus the picture's
   own triangles, placed with the camera that picture was drawn with. Nothing
   is guessed: the whole map casts, on screen or not, and a still picture
   gives the same map every frame.
2. **The scenery memory**: each frame's opaque PGXP triangles taken to world
   space and kept per scene (static ones only off screen), when the camera
   is known but no geometry is given.
3. **The frame's own triangles** only.

Faces the game culls (back-facing after RTPT) are kept as shadow-only casters
with the frame's triangles, so a closed object casts a whole shadow. The
winding alone says culled, so a routine that draws faces either way round (a
bush's leaf cards) would cast each card seen from behind whole, over its
cut-out, and each card it does not draw (off the picture) whole too. The
faces taken are kept for the last 3 VBlanks that took any, by their GTE
screen corners and the return address of their projections; a textured
polygon drawn with all three corners of one marks that routine as drawing
both windings, and its faces are not taken again (learned for the session;
`shadow_cache` reports `two_sided`). A closed object's routine never draws
its far side, so it keeps them.

**Sprites** (with `sprite_anchors` and `camera = "ram:..."`): a 2D game draws
a character as flat pieces around a foot point it projects with one RTPS.
The calls that do it are named by their return address; at each one the
foot point is taken in the view space of the camera projecting it and sent
in drawing order, so the pieces drawn next (that frame's) stand on it. One
sprite = its opaque pieces from one texture page and CLUT that touch, in any
drawing order (games sort by depth, so two sprites' pieces interleave); it
goes to the foot point at its bottom, in its columns (a sword held out puts
the point near one end). Grouping keeps each run's root at its lowest piece,
so a run is walked whole whatever the drawing order. A run that is not at
one (a part on another texture page) joins a sprite only when it has that
sprite's CLUT (HUD bars, panels and texts use the UI's palettes), touches one
of its runs or is up to 64 px over it (eyes floating over a body, a body in
the air over its feet in a leap), its middle is within that sprite's columns
and it is at most twice as wide. A joined part stands on its sprite's plane,
as drawn (its own bottom is no floor), and may be small; a sprite's own run
under 12 px tall does not stand (a world map's markers) nor keeps its foot
point. A foot point no run is at takes the run over it (up to 64 px, its
middle within half its width + 16 px), in a CLUT seen standing on the floor
in the last 600 builds (a jump, an attack's step; never a UI palette): it
stands where its lowest row meets the floor, so its shadow stays at its
feet, its feet not bent. A sprite is
stood upright where its lowest drawn pixel meets the floor (a plane facing
the camera across the floor), each piece along its own view ray. When the
light comes across the picture (in that plane, as in BoF4's battles) a
sprite would cast a line: the plane then turns about the up axis just enough
that the level light meets it at a cosine of 0.7 (its shadow keeps 70% of
its width). Feet drawn off that plane (a 3/4 pose's back foot, feet to one
side of a turned plane) would float or sink: every column whose bottom is a
foot's (within the bottom fifth, 3 columns wide or more, so not a staff's
tip) gets its pixels bent, smoothly over the rows above (3/4 of the sprite's
height: the whole leg and the hips), to where that bottom meets the floor,
as extra casters (a pixel wider each side, no gaps) and in the `sprite`
depth. Bending the foot's end alone left the back leg's top on the plane,
its shadow apart from the bent bottom's: under a low sun, four legs.
`u_ss` z 1 (TCP) turns the bend off and `u_ss` w sets its height in native
px, to compare. Its cut-out is copied from VRAM as it is drawn (a
game may load the next animation's pieces before the picture is shown).
Translucent pieces (dust, glows) neither cast nor take shadows. The game's
own blob is not drawn: a translucent flat quad centred on a foot point, or
(blobs painted into the sprite's own picture, BoF4's battles) the near-black
CLUT entry that is a quarter or more of an opaque quad holding a foot point.
That palette's entry is learnt, and every quad using that palette within the
band round that sprite's foot rows draws with it see-through (written to 0
in VRAM for that one draw, then put back), so the feet away from the point
in an attack pose or a tail lose it too, while eyes higher up keep it.
Blobs under anything else (a boat, a dragon) are found by their look: a
textured quad drawn subtractive (it darkens the floor) whose texture box has
its four corners transparent and its centre drawn (a disc) is not drawn
while our sun or a fire casts (each texture box judged once; TCP
`shadow_cache` `discs`). A
chain that reads `sprite` gets each pixel's sprite view depth (60000 =
none), so the sprites themselves can be shaded: `sun-out.glsl` gives them,
in shadow, the darkening the ground takes, and nothing in the sun; they
take shadow from the scenery only (never from a sprite, their own bent feet
included).

```toml
[video.shadow]
camera = "ram:0x801381BC"   # the game's camera MATRIX (9 x s16 R, pad, 3 x s32 T);
                            # "gte" = the GTE at VBlank (a guess), "none" = a mod feeds it
overdraw = [64, 64]         # px past the picture the game's screen culls keep; the polygons there
                            # are clipped as before and cast (a mod may ask for more: psx_mod_set_overdraw_extra)
scene_keys = ["0xADDR:4"]   # guest RAM whose change starts a new scene (memory only)
hide_textures = ["0x46:0x7c43", "0x149:*"]   # page:CLUT of the game's own blob shadows
off_textures = ["0x1e:0x3dc0", "*:0x3e00"]   # page:CLUT only a sunless screen draws (no shadow effects)
sprite_anchors = ["0x80141FA0", "0x80141FD8"]   # return addresses of the foot-point RTPS calls
```

The Sun presets (`shaders/hd2d/sun*.pfx`) differ only in shadow detail:
`sun-low` (480 lines), `sun` (960 lines), `sun-high` (1.5x the picture, a
4096 map). All soften the scenery's shadow edges over `u_soft` x world units
(5x5 samples, the bias grown with the face's tilt to the light), the spread
growing with the distance to what casts it (`u_soft` y = 0.04 world units per
unit, percentage-closer soft shadows as in Fernando 2005, from a texel up to
x): a bush's leaves stay sharp at its foot, a cliff's far edge soft. The
sprites' soften the same way, by `u_soft` z (at most, world units) and w
(per unit of distance: 4 and 0.1, sharp at the feet, softer at the head),
5x5 samples turned per pixel; z 0 keeps them sharp (3x3 texels of
`sprshadow`). A sun higher than `u_sprsun` x (degrees; 50 in the presets)
would cast an upright sprite as a line at its feet: the sprites then cast
along a sun at x (same azimuth) in `sprshadow` with its own mapping (the
pass gets `u_ssx`, `u_ssy`, `u_ssz`, `u_sss`, as `u_smx`..`u_sms`; w 0 = the
scenery's), each foot point dropped onto the surface under it (a ray down
through the frame's 3D triangles, up to 128 units: the game's point can sit
above a deck), and each sprite's silhouette laid along that light onto the
floor at its feet, so its shadow lies there whole; it reaches `u_sprsun` y
world units down at most (40: none on a floor far below, the sand under a
deck) and is `u_sprsun` z units soft (3). The scenery keeps the real sun.
The bias that keeps a face from shading itself (tens of world
units on a floor under a low sun) also lifts a wall's shadow off its foot,
a lit line where wall and floor meet: `u_shadow` w (2 in the presets, 0 =
off) fills it, a few steps from a lit point toward the sun over w x the bias,
shaded where the scenery stands in front of that path (it only adds shadow,
and only that close). Their faces' normals come from
two pixels on the side that stays in the pixel's plane (the least bend in
1/Z), so creases between faces show no seams; a normal is turned to face the
camera along the ray to its point (not by its z: a floor seen almost edge-on,
a cutscene's camera at the horizon, has z ~ 0 and flipped in zigzag stripes;
the same in AO and the light passes). A pass may read up to 6 inputs.

With `camera = "ram:..."`, before each projection a GTE rotation +
translation not met yet is compared with the MATRIX in RAM; equal, it is the
camera, handed to the renderer at that point, so the triangles the GPU draws
next (that frame's) carry it. When no projection has used it for a few
VBlanks (a map screen, a different mode) there is no camera and source 1 is
not used.

Finding them for a new game (TCP, see TCP_COMMANDS.md): `gte_ring_dump` on a
full frame groups projections by `ra` and matrix; the matrix most projections
share is the camera, and searching RAM for its R (9 x s16) and T (3 x s32)
gives the MATRIX address. `disasm` at the busiest site shows how the vertices
are built (BoF4: a height grid, see the game's `src/bof4_world.c`). The
sprite foot points are the single RTPS with the camera whose `ra` repeats
once per character (BoF4: two calls per sprite, same point).

TCP: `shadow_cache` (state: camera, matches, world triangles, memory, sprite
runs: at their foot point / touching one that is / none, `sprite_bent`: sprites
with feet bent down; `blobs`: blob quads hidden and draws with a blob colour
see-through), `sprite_dump` (the picture's
foot points and 2D pieces; `stood`: the last shadow build's foot points and
the pieces stood at each; `runs`: its runs [box, foot point, at it, CLUT
code y*64+x/16; 9 = too big]),
`shadow_hide {"list":"0xPAGE:0xCLUT,..."}`, `postfx_timing {"on":1}` (GPU
microseconds per stage and pass).

## The fire lights (`firemap`)

A steady glow the game draws (a campfire, a lamp, a staff's orb) becomes a
point light with its own shadows. The glowing pieces are the additive ones
(blend modes 1 and 3), 3D or 2D, kept while a chain reads a map of the 3D
triangles (`shadow`, `sprshadow` or `firemap`). Pieces within 24 px join; a
2D group takes its depth from the nearest point the game projected with the
camera (any RTPS), and is not a light when it has none, or when less than 4
triangles and 3% of the frame's 3D are drawn after it: the HUD is drawn over
the finished scene, a piece in it is sorted among it (a battle's 3D cursor
after its HUD: 2 of 538 triangles; after the cave's campfire: 12 of 68). A
light is followed in world space (joined within 160 units), lights after 30
presents in a row and goes after 20 unseen. Its colour is its pieces' as
drawn (4x4 texels through the CLUT, bright ones counting most, times the
vertex colour; flat: the vertices'), brightest channel 1; its strength and
reach go with its glow's size, full at 384 px (a campfire), a quarter or
more below (a staff's orb, 64 px: 0.4).

`firemap` holds, for the two biggest lights, the six 512 px faces of a cube
round each (distance to the light over the reach): the scenery, the frame's
triangles and the sprites stood as for the sun. A pass reading it gets
`u_fl0`/`u_fl1` (view space, y up; w the reach, the map's depth scale),
`u_fls0`/`u_fls1` (place on the picture), `u_flc0`/`u_flc1` (colour; w the
size factor) and `u_flmap` (face px, lights, row pairs). The light sits
`u_firecfg` y units over the glow's base. `light.pfx` (`light-out.glsl`)
adds it to the scenery and the sprites (5x5 rotated samples over
`u_firecfg` w units: soft edges), darkens its shadows by `u_firecfg` z, and
flickers it (`u_flicker`, over `u_time`). `u_firecol` w is the strength (0:
off, nothing built); its rgb is the colour of a light whose glow gives none.
While a light casts, the game's own blobs are hidden in an area with no sun
too. A light whose `u_firecol` w is 0 casts nothing.

TCP `shadow_cache` adds `lights` (each: world, screen, z, size, 3D after it,
rgb, hits, active), `glow_pieces`, `glow_now`, `points_now`, `tris_now` and
this frame's `pieces` (x, y, z, area, 3D before it).

## Tests

- `postfx_chain_test`: the format, built-in presets, errors and file import.
- `gl_bloom_test`: bloom on 2D, additive and 3D primitives, plus a HUD beside
  and over a lamp. Also an imported chain (byte-exact invert), the mask input
  (the HUD unlit, the 3D scene lit), a bad preset, a pass that does not
  compile, and GL state restored with every stage on. It runs on the caller
  thread and on the render thread.
- `gl_post_aa_test`: FXAA on the pipeline. Its output is byte-identical to
  the former fixed pass.
