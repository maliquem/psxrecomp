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
  input <sampler> <source|source_down|mask|mask3d|depth|shadow|sprshadow|sprite|previous|<earlier pass>>   # up to 6
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
in 2 and 3, so a closed object casts a whole shadow.

**Sprites** (with `sprite_anchors` and `camera = "ram:..."`): a 2D game draws
a character as flat pieces around a foot point it projects with one RTPS.
The calls that do it are named by their return address; at each one the
foot point is taken in the view space of the camera projecting it and sent
in drawing order, so the pieces drawn next (that frame's) stand on it. One
sprite = its opaque pieces from one texture page and CLUT that touch, in any
drawing order (games sort by depth, so two sprites' pieces interleave); it
goes to the foot point at its bottom, in its columns. A run that is not at
one (a part in another page or CLUT) joins a sprite only when it touches one
of that sprite's runs and its middle is within that sprite's columns, so HUD
panels and texts above or beside a character never stand there. A sprite is
stood upright where its lowest drawn pixel meets the floor (a plane facing
the camera across the floor), each piece along its own view ray. When the
light comes across the picture (in that plane, as in BoF4's battles) a
sprite would cast a line: the plane then turns about the up axis just enough
that the level light meets it at a cosine of 0.7 (its shadow keeps 70% of
its width). Feet drawn off that plane (a 3/4 pose's back foot, feet to one
side of a turned plane) would float or sink: every column whose bottom is a
foot's (within the bottom fifth, 3 columns wide or more, so not a staff's
tip) gets its lower pixels bent, smoothly over the rows above, to where that
bottom meets the floor, as extra casters (a pixel wider each side, no gaps)
and in the `sprite` depth. Its cut-out is copied from VRAM as it is drawn (a
game may load the next animation's pieces before the picture is shown).
Translucent pieces (dust, glows) neither cast nor take shadows. The game's
own blob is not drawn: a translucent flat quad centred on a foot point, or
(blobs painted into the sprite's own picture, BoF4's battles) the near-black
CLUT entry that is a quarter or more of an opaque quad holding a foot point.
That palette's entry is learnt, and every quad using that palette within the
band round that sprite's foot rows draws with it see-through (written to 0
in VRAM for that one draw, then put back), so the feet away from the point
in an attack pose or a tail lose it too, while eyes higher up keep it. A
chain that reads `sprite` gets each pixel's sprite view depth (60000 =
none), so the sprites themselves can be shaded: `sun-out.glsl` gives them,
in shadow, the darkening the ground takes, and nothing in the sun; they
take shadow from the scenery only (never from a sprite, their own bent feet
included).

```toml
[video.shadow]
camera = "ram:0x801381BC"   # the game's camera MATRIX (9 x s16 R, pad, 3 x s32 T);
                            # "gte" = the GTE at VBlank (a guess), "none" = a mod feeds it
overdraw = [64, 64]         # px past the picture the game's screen culls keep (memory only)
scene_keys = ["0xADDR:4"]   # guest RAM whose change starts a new scene (memory only)
hide_textures = ["0x46:0x7c43", "0x149:*"]   # page:CLUT of the game's own blob shadows
sprite_anchors = ["0x80141FA0", "0x80141FD8"]   # return addresses of the foot-point RTPS calls
```

The Sun presets (`shaders/hd2d/sun*.pfx`) differ only in shadow detail:
`sun-low` (480 lines), `sun` (960 lines), `sun-high` (1.5x the picture, a
4096 map). All soften the scenery's shadow edges over `u_soft` x world units
(5x5 samples, the bias grown with the face's tilt to the light) and keep the
sprites' sharp (3x3 texels of `sprshadow`). Their faces' normals come from
two pixels on the side that stays in the pixel's plane (the least bend in
1/Z), so creases between faces show no seams. A pass may read up to 6 inputs.

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
foot points and 2D pieces),
`shadow_hide {"list":"0xPAGE:0xCLUT,..."}`, `postfx_timing {"on":1}` (GPU
microseconds per stage and pass).

## Tests

- `postfx_chain_test`: the format, built-in presets, errors and file import.
- `gl_bloom_test`: bloom on 2D, additive and 3D primitives, plus a HUD beside
  and over a lamp. Also an imported chain (byte-exact invert), the mask input
  (the HUD unlit, the 3D scene lit), a bad preset, a pass that does not
  compile, and GL state restored with every stage on. It runs on the caller
  thread and on the render thread.
- `gl_post_aa_test`: FXAA on the pipeline. Its output is byte-identical to
  the former fixed pass.
