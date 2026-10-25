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

/* The same canvas for a title's own page (psx_mod_open_title_page: shown
 * while it is set, the menu closed) and HUD (psx_mod_set_title_hud: shown
 * when neither the menu nor a page is, while the renderer draws something).
 * Renderers run when redraw was asked (or the window changed); return 0 =
 * nothing to show. NULL removes. */
typedef int (*PsxTitleCanvasRenderer)(uint32_t *argb, int w, int h);
void psx_savestate_menu_set_page(PsxTitleCanvasRenderer page);
void psx_savestate_menu_page_redraw(void);
void psx_savestate_menu_set_hud(PsxTitleCanvasRenderer hud);
void psx_savestate_menu_hud_redraw(void);
/* A title's notices over all of it (psx_mod_set_title_toast): blended over
 * the menu's or page's canvas or the HUD, or shown alone; a renderer that
 * returns 2 is drawn again at the next present. */
void psx_savestate_menu_set_toast(PsxTitleCanvasRenderer toast);
void psx_savestate_menu_toast_redraw(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_MENU_H */
