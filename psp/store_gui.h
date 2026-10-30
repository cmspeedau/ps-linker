/*
 * PS Linker - Store pixel view (PSP), public entry points
 *
 * The pixel twin of store.c's debug-screen rendering. Both draw the same
 * ui/coverflow.c state; store.c picks a backend and never mixes them.
 */

#ifndef PSP_STORE_GUI_H
#define PSP_STORE_GUI_H

#include "../ui/coverflow.h"

/* Draw one browse frame (header, tab rail, flow or grid, hint bar).
   `ids` and `descs` are per-item-slot arrays parallel to cf->items. */
void store_gui_draw(const coverflow_state_t *cf,
                    const char *const *ids, const char *const *descs);

/* Status screen: header bar text, one body row, optional hint row. */
void store_gui_status(const char *header, const char *body, const char *hint);

/* Install screen with a progress bar; pct < 0 before the first byte. */
void store_gui_progress(const char *title, const char *path, int pct, long total);

#endif /* PSP_STORE_GUI_H */
