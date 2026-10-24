#ifndef PSX_SAVESTATE_MENU_H
#define PSX_SAVESTATE_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void psx_savestate_menu_set_state(int open, int selected_slot);
void psx_savestate_menu_note_slots_changed(void);
int  psx_savestate_menu_needs_present(void);
int  psx_savestate_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

/* A title's own look for the menu (psx_mod_set_savestate_menu_renderer): the
 * framework keeps the slots, the keys and saving / loading; the renderer only
 * draws. Its canvas is 240 lines (the PSX picture's height) by as many columns
 * as the window's aspect gives (at least 320), ARGB, top row first, stretched
 * over the window; alpha is honoured, so the frozen game shows through where it
 * is below 0xFF. Called on the main thread whenever something shown changed;
 * returns 0 to have the framework's own panel this time. NULL = the
 * framework's panel. */
typedef int (*PsxSaveStateMenuRenderer)(int selected_slot, uint32_t *argb, int w, int h);
void psx_savestate_menu_set_renderer(PsxSaveStateMenuRenderer renderer);
/* The window's size, for the canvas' aspect (redrawn when it changes). */
void psx_savestate_menu_set_window(int w, int h);
/* The current image has transparency (a title's canvas): blend it. */
int  psx_savestate_menu_overlay_blend(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_MENU_H */
