# HDR output (OpenGL, Windows)

Opt-in and off by default. `PSX_HDR=1` asks for it; with it unset nothing in
`runtime/src/gl_hdr.inc` runs and the window keeps its usual 8-bit back buffer.

## How the picture reaches the display

- The window is created with a float16 back buffer (`SDL_GL_FLOATBUFFERS`,
  `WGL_ARB_pixel_format_float`), set in `configure_core_gl_context_attributes`
  before the window exists. If the driver offers no such pixel format the
  window is created again without it and HDR stays off.
- On Windows with HDR on in the display settings, the desktop compositor shows
  a float16 window as **scRGB**: linear light, Rec.709 primaries, 1.0 = 80 nits,
  values above the SDR white level reaching the display's highlights. Measured
  on an RTX 4070 SUPER (driver 616.92, HDR10 TV, Windows 11, windowed and
  borderless): values written 0.5 / 1.0 / 3.5 / 10.0 were composed as exactly
  0.5 / 1.0 / 3.5 / 10.0 (desktop duplication in float16); an 8-bit window is
  clamped at 1.0 and shown at the SDR white level (sRGB curve × the
  "SDR content brightness" slider).
- Everything up to the swap composes the usual SDR picture (game, post
  effects, CRT, OSD, captures). `hdr_finish`, called from `gl_swap_now` (the
  one owned swap, real and generated frames alike), turns it into scRGB as the
  last step: clamp to 0..1, decode (sRGB curve, as Windows decodes SDR
  windows, or `PSX_HDR_GAMMA=2.2`), times the paper white. With the present
  thread on its slots are RGBA16F.
- `present_shot` (and the mod screenshot) reads the picture before that step:
  PNGs stay SDR.

## Settings (environment, prototype)

| variable | meaning | default |
|---|---|---|
| `PSX_HDR` | 1 = HDR output | off |
| `PSX_HDR_PAPER` | paper white, nits | Windows' SDR white level |
| `PSX_HDR_PEAK` | highlight peak, nits | the display's (DXGI MaxLuminance via SDL) |
| `PSX_HDR_BOOST` | 0..1, how far the scene's lights go toward the peak | 0 |
| `PSX_HDR_KNEE` | encoded level where the lift starts | 0.6 |
| `PSX_HDR_GAMMA` | `2.2` = gamma 2.2 decode instead of the sRGB curve | sRGB |
| `PSX_HDR_DYNAMIC` | 1 = dynamic tone mapping | off |
| `PSX_HDR_ADAPT` | 0..1, how much the paper white follows the scene | 0.5 |
| `PSX_HDR_AREA` | lifted share of the picture that keeps the full boost | 0.015 |
| `PSX_HDR_SPEED` | following time constant, seconds | 0.5 |
| `PSX_HDR_EXPOSURE_MIN` / `_MAX` | range of the dynamic exposure | 0.85 / 1.0 |
| `PSX_HDR_CONTRAST` | 0..1.5, sets both of the next two | 0 |
| `PSX_HDR_SHADOWS` | 0..1.5, deeper shadows (whole picture) | 0 |
| `PSX_HDR_SCENE` | 0..1.5, bright 3D scenery over the paper white | 0 |
| `PSX_HDR_SCENE_AREA` | lifted scenery share that keeps the full lift (dynamic) | 0.3 |

Boost 0 with Windows' paper white is, pixel for pixel, what an SDR window
shows in HDR mode. On an SDR display the paper white is 1 and nothing is
lifted.

## The boost

Only the scene's lights are lifted: the frame's glowing (additive) pieces the
fire light already counts (`pfx_lights_update`: not black, not dim grey, and
3D or with 3D scenery drawn after them, so the HUD's never are). Touching
pieces make one light, drawn as a soft ellipse over its box into an R8 mask
(`hdr_lights_build`); the last pass multiplies it by the 3D mark (`mask3d`
green, cleared by opaque 2D drawn later: sprites, text boxes) and lifts only
the bright part (from the knee). It needs geometry correction (PGXP) for the
3D mark and turns on the scene capture (`s_sc_on`) for the light pieces. A big
overlay (save-state menu, a title's page or HUD, rewind) switches it off for
that present.

## Dynamic tone mapping

Each frame is measured on the GPU (64x36 cells of the picture: log-average
light, lifted share, mean light), reduced to one texel and followed in time
(ping-pong 1x1 RGBA32F; light at `PSX_HDR_SPEED`, four times faster on a cut
of more than a stop and a half; lifted share at once up, slowly down). The
last pass reads it directly (no CPU read-back):

- exposure: paper white × clamp((0.18 / light)^(0.2 × adapt), 0.85, 1.3);
- highlight budget: boost × min(1, sqrt(area / lifted share));
- a soft shoulder (tanh) from 80% of the display's peak to the peak, also
  when `PSX_HDR_PEAK` asks for more than the display has.

## Contrast

In stops around mid grey (0.18 of the paper white), luma-scaled so hues
stay:

- shadows (whole picture): under mid grey the stops stretch by
  0.4 × shadows, from none at mid grey to all of it 4 stops under (smoothstep):
  whites never move, the HUD's darks deepen a little with the rest;
- scenery: where the 3D mark is and nothing see-through is over it (mask3d
  green × (1 − blue)), the stops over mid grey stretch by 0.45 × scene, fully
  at white: 2^(2.47 × 0.45) = 2.2 × paper at 1, capped at the peak; sprites,
  text boxes and the HUD are not marked and keep the paper white. With the
  lights' boost the larger of the two gains wins. Dynamic: × min(1,
  sqrt(scene_area / lifted scenery share)).

Measured (2986x1680, boost 0.6): stops between the 5th and 95th light
percentile, field 4.95 → 5.85 (0.5) → 6.70 (1); battle 4.50 → 5.10 → 5.75;
bright sunlit scenery up to ~410 / ~600 nits; at 1.4: field 7.40, battle 6.25,
scenery ~810 nits; HUD unchanged. Cost with
everything on: 0.46 ms.

## Debug server

`{"cmd":"hdr"}` reports the state (float buffer, display HDR, SDR white and
headroom as SDL reports them, paper and peak in use, lights, followed
measure). Live settings: `boost`, `paper`, `peak`, `knee`, `gamma`, `debug`,
`contrast` / `shadows` / `scene`, `exp_min` / `exp_max`
(1 tints lifted pixels), `dynamic`, `adapt`, `area`. `"probe":"<png>"` reads
the next finished frame back (half floats): max / mean nits, light percentiles
(5/50/95/99, contrast in stops), share over the
paper white, a false-colour PNG at half size (grey = SDR range, yellow to red
= lifted) and the fenced GPU cost of the pass (`cost_us`; ~0.35-0.45 ms at
2986x1680 on the GPU above at a low clock).

## Not covered

- Only Windows with SDL3. Elsewhere `PSX_HDR` is ignored.
- No HDR10 (PQ) swap chain of our own: the float16 window relies on the
  compositor. A DXGI swap chain through `WGL_NV_DX_interop2` would give
  explicit colour space control (and flip-model presentation) at the cost of
  owning the present.
- Lights drawn before opaque 3D that covers them still count where they are
  covered.
