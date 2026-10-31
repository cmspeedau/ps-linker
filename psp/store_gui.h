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

/* Utilities page: the switch home. One labelled toggle - the gclite
   feature (CAT_ name stripping + START categories popup) - plus whether
   the plugin was found in ms0:/seplugins/vsh.txt. The caller owns keys. */
void store_gui_utilities(int enabled, int installed);

/* Install screen with a progress bar; pct < 0 before the first byte. */
void store_gui_progress(const char *title, const char *path, int pct, long total);

#endif /* PSP_STORE_GUI_H */
