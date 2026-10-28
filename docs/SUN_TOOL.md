# Sun tool: the sun and the moon per area

For titles that use the GI shadow passes (`[video.shadow]`, `shaders/hd2d/sun*.pfx`): a small
program to line up the shadows with each area's painted ones while the game runs.

## Pieces

- **Live lights** (`gpu.h`): `gpu_shadow_light_set(area, which, mode, az, el)` — `which` 0 = the
  sun, 1 = the moon (the light at night, `gl_renderer_set_night`); `mode` `GPU_LIGHT_NONE`
  (drop: the config's `sun_areas`, else the pass's `u_sun` / `u_moon`), `GPU_LIGHT_AT`,
  `GPU_LIGHT_OFF` (no shadows there; for the moon, at night). They win over `sun_areas`. A
  moon of its own is absolute (the pass's `u_moon` is a turn from the sun); the night share
  moves the light from the sun to it the short way round.
- **File**: `sun_areas.txt` next to the exe, read at start after the config
  (`gpu_shadow_lights_load`). One area per line, `#` starts a comment:

  ```
  0xa0fa3035 sun 150.0 25.0 moon 120.0 40.0   # a note
  0x6984c883 sun off
  ```

- **TCP `sun_area`** (build with the debug tools): `{}` reads the current area, the night
  share, the area's sun and moon (live and config layers: `[mode, az, el]`), the light last
  used (`used`: az, el, presents since that shadow build), the pass's `u_sun`/`u_moon`
  (`pass`) and the camera's heading and pitch in the sun's azimuths (`camera`).
  `{"set":"sun"|"moon","v":"AZ EL"|"off"|"none"}` changes the live one first (`"area":"0x..."`
  for another area). Allowed in the restricted mode (`psx_mod_set_debug_tools_restricted`:
  it only moves the light) and it does not switch the passive recorders on (a tool polls it).
- **`tools/sun_tool`** (`psx_sun_tool.exe`, SDL3 + Dear ImGui from the game's recomp-ui):
  shows the area and whether it is night, a 3D scene (the light dragged on a dome, the
  shadows of a few boxes, the view following the game's camera), sliders, Apply (writes the
  area's line in `sun_areas.txt`), Discard, Default (drops the area's entry), "off", a note
  per area. Ctrl+S (system-wide) shows or hides it; the window stays on top. What was not
  applied goes back to the file's when the area changes or the tool closes.

  ```
  cmake -S psxrecomp/tools/sun_tool -B build-suntool -G Ninja -DCMAKE_BUILD_TYPE=Release
  psx_sun_tool.exe --port 4370 [--dir <game folder>]
  ```

  Tests: `--shot out.png [--frames N] [--test-set "sun AZ EL"|"moon off"|"sun none"] [--test-apply]`
  draws off screen and saves a PNG.

Azimuth: 0 = the world's x axis as the camera matrix gives it, 90 = x × up; elevation in
degrees (1..89). The UI text is Portuguese (written for its first user).
