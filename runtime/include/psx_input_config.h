#pragma once
/* psx_input_config — a title's own controls menu edits the framework's input
 * configuration through these (main / emulation thread only). A change applies
 * at once; the *_save calls write it where the launcher keeps it, so the
 * launcher's Controls page and a later run see the same.
 *
 *   controller map    input.ini [mapping] (and each [mapping.<guid>])
 *   controller hotkeys settings.toml [hotkeys] *_pad (psx_hotkey_pad.h codes)
 *   keyboard          keybinds.ini (psx_keybinds.h) and config.ini [KeyMap]
 *                     (host_keymap.h) have their own get / set / save. */
#ifdef __cplusplus
extern "C" {
#endif

/* The sources of one PSX input of the controller map as input.ini writes them
 * ("a", "leftshoulder,rightx-", "" = none). psx_input: the input.ini key
 * ("cross", "l1", "rs_up"...). Returns 0 for an unknown key. */
int psx_input_map_get(const char *psx_input, char *out, int cap);
/* Replace them, in the global map and every per-controller one. */
int psx_input_map_set(const char *psx_input, const char *sources);
/* Write the maps to input.ini (other keys, sections and comments kept). */
int psx_input_map_save(void);

/* The controller binding of host shortcut `shortcut` (PSX_MOD_SHORTCUT_*):
 * psx_hotkey_pad.h encoding. set returns 0 for an unknown shortcut or an
 * invalid binding (kept). */
int psx_input_pad_shortcut_get(int shortcut);
int psx_input_pad_shortcut_set(int shortcut, int binding);
/* Write the four bindings to settings.toml [hotkeys]. */
int psx_input_pad_shortcuts_save(void);

/* Write the host hotkeys (host_keymap_set) to the config.ini the runtime
 * read them from ([KeyMap]; next to the exe when there was none). */
int psx_input_host_keys_save(void);

/* P1's host controller for a title's own shortcuts: SDL gamepad button `code`
 * (or PSX_HOTKEY_PAD_LT_BIT / _RT_BIT, a trigger pressed past half) is held,
 * on the assigned gamepad or the debug host layer. 0 without a gamepad. */
int psx_input_host_pad_button(int code);

/* Run a host action as its hotkey would (OSD included). Returns 0 when it is
 * unavailable now (the save state menu while Rewind is open). */
enum {
    PSX_INPUT_ACTION_SAVE_STATE_MENU = 0,   /* open / close the menu */
    PSX_INPUT_ACTION_FAST_FORWARD_LOCK = 1  /* latch / release fast forward */
};
int psx_input_host_action(int action);

/* Save to / load from user slot 0..SAVESTATE_SLOTS-1 as the save state menu
 * does: refused (with an OSD note) where the menu would refuse; the result
 * comes later through the usual notification. 1 when requested. */
int psx_input_savestate_submit(int slot, int save);

#ifdef __cplusplus
}
#endif
