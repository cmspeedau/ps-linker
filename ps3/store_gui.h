/*
 * PS Linker - Store pixel view (PS3), public entry points
 *
 * The pixel twin of ps3/store.c's ANSI rendering, on the gfx layer.
 * Both draw the same ui/coverflow.c state; store.c picks a backend and
 * never mixes them. Layout and colours match psp/store_gui.c at the
 * virtual 1280x720 grid.
 */

#ifndef PS3_STORE_GUI_H
#define PS3_STORE_GUI_H

#include "../ui/coverflow.h"

/* Draw one browse frame (header, tab rail, flow or grid, hint bar).
   `ids` and `descs` are per-item-slot arrays parallel to cf->items. */
void store_gui_draw(const coverflow_state_t *cf,
                    const char *const *ids, const char *const *descs);

/* Browser variant: same frame, browser hint keys, cwd in the header.
   `progress` is a per-item-slot pct array (100 = finished, 0..99 mid
   transfer, -1 unknown) that draws the Downloads row list - name plus
   progress bar - instead of the covers; pass NULL for the cover views. */
void store_gui_browse(const coverflow_state_t *cf,
                      const char *const *ids, const char *const *descs,
                      const char *path, const int *progress);

/* Status screen: header bar text, one body row, optional hint row. */
void store_gui_status(const char *header, const char *body, const char *hint);

/* Characters of the URL editor's key strip. Slot indexes are
   0..(chars-1), then delete, then OK (same numbering in main.c). */
#define STORE_URL_CHARS "abcdefghijklmnopqrstuvwxyz0123456789-.:/_"

/* Utilities page (the browser's SELECT screen): the NAS server URL row.
   No PSP-style feature switches live here - gclite is a PSP plugin.
   The caller owns keys. */
void store_gui_utilities(const char *url);

/* URL editor: full-screen pixel keyboard showing `url` with the cursor
   on key slot `pick`. The caller owns keys. */
void store_gui_url_edit(const char *url, int pick);

/* Key slots per keyboard row (Up/Down jump by this). */
int store_gui_url_edit_cols(void);

/* Install screen with a progress bar; pct < 0 before the first byte. */
void store_gui_progress(const char *title, const char *path, int pct, long total);

#endif /* PS3_STORE_GUI_H */
