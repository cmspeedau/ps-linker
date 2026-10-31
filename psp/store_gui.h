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

/* One row of the browser's gclite categories popup: a mock cover tile
   generated from `id` (store_cover_pixels, like the flow cards), the
   label and the number of items behind it. */
typedef struct {
    char name[32];
    char id[48];
    int  count;
} store_menu_row_t;

typedef struct {
    const store_menu_row_t *rows;
    int n;
    int sel;
} store_menu_t;

/* Browser variant: same frame, browser hint keys, cwd in the header.
   `progress` is a per-item-slot pct array (100 = finished, 0..99 mid
   transfer, -1 unknown) that draws the Downloads row list - name plus
   progress bar - instead of the covers; pass NULL for the cover views.
   `menu` (NULL when closed) dims the frame and draws the categories
   popup on top; the caller owns its keys. */
void store_gui_browse(const coverflow_state_t *cf,
                      const char *const *ids, const char *const *descs,
                      const char *path, const int *progress,
                      const store_menu_t *menu);

/* Status screen: header bar text, one body row, optional hint row. */
void store_gui_status(const char *header, const char *body, const char *hint);

/* Characters of the URL editor's key strip. Slot indexes are
   0..(chars-1), then delete, then OK (same numbering in main.c). */
#define STORE_URL_CHARS "abcdefghijklmnopqrstuvwxyz0123456789-.:/_"

/* Utilities page (the browser's SELECT screen): two focusable rows -
   the NAS server URL (focus 0, shows `url`) and the gclite pill switch
   (focus 1; `installed` is the ms0:/seplugins/vsh.txt detection). The
   caller owns keys. */
void store_gui_utilities(int enabled, int installed,
                         const char *url, int focus);

/* URL editor: full-screen pixel on-screen keyboard showing `url` with
   the cursor on key slot `pick`. The caller owns keys. */
void store_gui_url_edit(const char *url, int pick);

/* Install screen with a progress bar; pct < 0 before the first byte. */
void store_gui_progress(const char *title, const char *path, int pct, long total);

#endif /* PSP_STORE_GUI_H */
