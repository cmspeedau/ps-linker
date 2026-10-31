/*
 * PS Linker - PS3 app UI backends (public contract)
 *
 * The app is pixel-only: ui_pix.c draws the splash/download/result
 * frames on the RSX renderer in gfx.c, the browser frame is the shared
 * Cover Flow state (ui/coverflow.c + ui/browser_model.c) drawn through
 * store_gui_browse, and SELECT opens the pixel Utilities page
 * (store_gui_utilities + store_gui_url_edit in store_gui.c). One frame
 * language app-wide - the old ANSI/netdebug UI and the Media Go
 * sidebar chrome (its own Settings screen) were removed.
 */

#ifndef PS3_UI_H
#define PS3_UI_H

#include "../ui/coverflow.h"

/* One directory-listing entry, as parsed by main.c. size/category come from
 * the share's anchor data-size/data-cat attributes; -1/"" when the share
 * does not report them. */
typedef struct {
    char name[256];
    char url[256];
    long long size;
    char category[64];
    char platform[16];    /* data-platform ("PSP"/"PS3"), "" if none */
    int  is_dir;
} FileEntry;

/* Open the RSX display. Returns 1 when the pixel renderer is live. */
int ui_init(void);

/* Hand the screen back (RSX off, XMB owns it again at exit). */
void ui_exit(void);

/* 1 when the RSX pixel display is live. */
int ui_pixel(void);

/* Splash/loading panel: header, big title, one message. */
void ui_splash(const char *msg);

/* Full-screen error panel; caller waits for a button afterwards. */
void ui_error(const char *line1, const char *line2);

/* One browser Cover Flow frame: the state describes selection, tabs and
 * view; ids/descs are per-item-slot arrays (cover tile seed / detail row);
 * path is the cwd shown in the header. `progress` (per-slot pct or NULL)
 * draws the Downloads row list instead of the covers. */
void ui_browse(const coverflow_state_t *cf, const char *const *ids,
               const char *const *descs, const char *path,
               const int *progress);

/* Settings live on the pixel Utilities page: main.c drives
 * store_gui_utilities + store_gui_url_edit (ps3/store_gui.h) directly. */

/* Download progress frame for file `name`; pct < 0 when the size is
 * unknown, total = bytes so far. Redraw at will (cheap on ANSI, the
 * caller throttles on the pixel path). */
void ui_download(const char *name, const char *path, int pct, long total);

/* Result line after a download (ok=1 green, ok=0 red), drawn over the
 * browser frame's status area; caller waits for a button afterwards. */
void ui_result(int ok, const char *msg);

#endif /* PS3_UI_H */
