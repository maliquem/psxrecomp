#ifndef PSX_BUG_REPORT_H
#define PSX_BUG_REPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Player bug reports (docs/BUG_REPORT.md; the title API is in mod_plugins.h).
 *
 * bug_report_begin (the BugReport hotkey) takes the moment while the game
 * keeps running: the next presented picture (present_shot), the PSX picture
 * without effects, a save state in SAVESTATE_BUG_SLOT and the presented-image
 * ring frozen (the clip). bug_report_tick, every host frame on the main
 * thread, opens the form once those landed (or after a few seconds); the form
 * is a title page (psx_mod_open_title_page) run by main's page loop, which
 * also feeds it typed text. Saving writes the folder; discarding deletes it. */

int  bug_report_begin(void);
/* 1 when the form should open now (main opens it as a title page). */
int  bug_report_tick(void);
int  bug_report_page_render(uint32_t *argb, int w, int h);
int  bug_report_page_input(uint32_t pressed, uint32_t held, int key, int key_mods);
/* Typed text (SDL text input, UTF-8) while the form is open. */
void bug_report_page_text(const char *utf8);
/* 1 while the form is the open title page (main: text input on). */
int  bug_report_page_active(void);
/* 1 from begin until the form closes. */
int  bug_report_busy(void);
/* Back to nothing (session activation): a report being made is discarded. */
void bug_report_reset(void);

/* A freeze / crash dump was written at dump_path (any thread): leave a
 * report pointing to it. */
void bug_report_note_freeze(const char *dump_path, unsigned wedge_kind);

/* Debug server (bug_report command): {"op":"status"|"open"|"text"|"key"|
 * "set"|"save"|"discard", ...}. Writes the answer's members into out. */
int  bug_report_debug(const char *op, const char *text, int key, int row,
                      int value, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_BUG_REPORT_H */
