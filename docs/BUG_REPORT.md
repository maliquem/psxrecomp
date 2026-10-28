# Bug reports (Ctrl+B)

A player who sees something wrong presses **Ctrl+B** (`config.ini [KeyMap] BugReport`; `None`
turns it off). The framework takes the moment, opens a short form over the frozen game and
writes everything a developer needs to reproduce it into one folder.

## What happens

1. **The moment** (the game keeps running for a frame or two):
   - the next presented picture, as the player sees it (effects, window size): `photo.png`;
   - the PSX picture without any post-processing (the native-wide surface when one is
     presented, else the display area): `photo_ps1.png`;
   - a save state in a slot of its own (`SAVESTATE_BUG_SLOT`, never listed in the menu; no
     "Saved slot" notice), moved into the folder: `state.pst`;
   - the presented-image ring (debug-tools builds) frozen, so its last three seconds become
     the clip.
2. **The form**, a title page over the frozen game (it opens once the picture and the state
   landed, or after three seconds): type, severity (Critical / Annoying / Minor), frequency
   (Always / Sometimes / Once) and a description typed on the keyboard (SDL text input: any
   language; Ctrl+V pastes). Keys: arrows / Tab move, left / right change a value or move
   the caret, Enter goes to the next row (Shift+Enter: a new line), **Ctrl+Enter saves**,
   **Esc discards** (twice when something was written). Controller: D-pad, Cross = next /
   press a button, Circle = discard, Start = save.
3. **Save** writes the folder; **discard** deletes it and the state.

## The folder

`<reports>/<YYYY-MM-DD_HHMMSS>_<type>/`, where `<reports>` is `PSX_BUG_REPORT_DIR`, else
`bug_reports/` in the working directory (a junction there can send it to another disk).

| File | |
|---|---|
| `report.json` | format 1: `id`, `created`, `status` ("new"), `auto`, `type`, `severity`, `frequency`, `description`, `files`, `clip` (frames, VBlanks), `runtime` (exe, its date, frame, presentation pacing of the last 5 s), `game` (the title's members), `history` |
| `photo.png`, `photo_ps1.png` | compressed PNGs (zlib) |
| `state.pst` | the save state |
| `clip.gif` | every presented frame of the last 180 VBlanks (at most 360), 120 lines tall, fixed 6x7x6 palette with ordered dither, delays from the VBlanks between frames (at least 2/100 s) |
| `clip_sheet.png` | twelve of those frames in a 4 x 3 grid (for reading without a GIF player) |
| `config/` | settings.toml, config.ini, input.ini, keybinds.ini, game.toml and the title's files |

`status` and `history` belong to whoever triages the reports: the framework writes "new".

A **freeze** (frames stopped: wedge kind 1) or a **fatal halt** (kind 4) leaves a report too:
`<date>_freeze/report.json` with `"auto": true` and the freeze dump's path in
`files.freeze_dump`. The other wedge kinds are not reported (they also fire while the
picture keeps going).

## For a title (mod_plugins.h)

- `psx_mod_bug_report_add_type(name)`: its own bug types, listed before the framework's
  (Graphics, Sound, Crash / freeze, Slow / stutter, Controls, Menu / text, Other).
- `psx_mod_bug_report_add_file(path)`: settings files copied into `config/`. Never a file
  holding a password, token or key.
- `psx_mod_set_bug_report_info(fn)`: `fn` writes `"name": value` members of the report's
  `game` object (where the player was, the title's modes).
- `psx_mod_set_bug_report_renderer(fn)`: the form in the title's look, drawn from a
  `PSXModBugReportView` (rows, values, text and caret, the moment's thumbnail, what was
  taken); return 0 to have the framework's panel. Input stays the framework's.
- `psx_mod_bug_report_open()` (a title's own shortcut), `psx_mod_bug_report_active()`.

All of it is cleared at each session's activation.

## Debug server

`{"cmd":"bug_report","op":"status"|"open"|"save"|"discard"}`,
`{"op":"text","chars":"..."}`, `{"op":"key","sdlk":K,"mods":M}`,
`{"op":"set","row":R,"value":V}` (value -1 selects the row). The answer carries the stage
(0 idle, 1 taking the moment, 2 form), the folder and what was taken.

## Not covered

- Release builds (no debug tools) have no presented-image ring: no clip, and the PSX
  picture is the 4:3 display area.
- Vulkan and headless have no present readback: no `photo.png`.
- Netplay: refused.
