# Post-effect presets (shipped with the framework)

`.pfx` chains (docs/POST_EFFECTS.md) any 3D title can load as `[video]
post_shader` or from a mod (`gl_renderer_set_extra_shader`). They are written
for psxrecomp (no third-party code) and staged next to every game runtime as
`shaders/<folder>/` (runtime/runtime.cmake).

| Preset | What it does | Needs |
|---|---|---|
| `hd2d/ao.pfx` | ambient occlusion of the 3D scenery from the PGXP depth | PGXP depth |
| `hd2d/sun.pfx` | sun/moon: faces lit by direction, cast shadows from a shadow map of the scenery seen so far, fixed in the world | PGXP depth, `[video.shadow]` |
| `hd2d/light.pfx` | glowing effects (spells) light the 3D scenery near them; a steady glow (a campfire, a lamp) becomes a light with its own colour, flicker and shadows on the scenery and the sprites (`firemap`) | PGXP depth, bloom mask, camera |
| `hd2d/grade.pfx` | colour grade and vignette on the 3D scenery | mask3d |
| `downsample/box.pfx`, `adaptive.pfx` | native-looking downsample (Adaptive: 2D only) | mask3d |
| `debug/depth.pfx`, `mask.pfx`, `shadowmap.pfx` | views of the depth, masks and shadow map | |

Live tuning: TCP `post_uniform` / `extra_shader`; GPU cost per pass: TCP
`postfx_timing`.
