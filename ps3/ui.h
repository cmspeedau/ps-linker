/*
 * PS Linker - PS3 app UI backends (public contract)
 *
 * main.c drives one of two interchangeable renderers: the pixel RSX
 * screens in ui_pix.c (default, what the TV shows) and the ANSI
 * netdebug/TTY renderer in ui_text.c (-DNASDL_TEXT_UI, and the fallback
 * when gfx_init() fails). Both draw exactly what main.c describes; all
 * navigation logic lives there.
 *
 * The Store's own browse/install screens are the same pair one level
 * down (ps3/store.c picks store_gui_* pixels or coverflow's ANSI
 * renderer); they share the gfx layer but not these functions.
 */

#ifndef PS3_UI_H
#define PS3_UI_H

/* One directory-listing entry, as parsed by main.c. */
typedef struct {
    char name[256];
    char url[256];
    int  is_dir;
} FileEntry;

/* Open the display (pixel when gfx_init() worked, else ANSI) and, for
 * the pixel path, take over the screen. Returns 1 when the pixel
 * renderer is live, 0 for ANSI. */
int ui_init(void);

/* Hand the screen back (pixel: RSX off, XMB owns it again at exit). */
void ui_exit(void);

/* 1 when drawing on the RSX pixel display, 0 when on the ANSI TTY. */
int ui_pixel(void);

/* Splash/loading panel: header, sidebar, big title, one message. */
void ui_splash(const char *msg);

/* Full-screen error panel; caller waits for a button afterwards. */
void ui_error(const char *line1, const char *line2);

/* Browser chrome + one listing page. path = cwd for the panel header,
 * files[0..count), selection sel, first visible row top. */
void ui_browser(const char *path, const FileEntry *files, int count,
                int sel, int top, const char *status);

/* Visible list rows for the active backend (scrolling math in main.c). */
int ui_browser_rows(void);

/* Settings: URL field plus the whole character-picker grid. Slot order
 * is SETTINGS_CHARS, then <delete>, then save-and-exit. */
extern const char SETTINGS_CHARS[];
#define SETTINGS_NCHARS 41   /* length of SETTINGS_CHARS; ui_text.c pins it */

void ui_settings(const char *url, int pick, int nslots);

/* Slots per picker row for the active backend (Up/Down jump by this). */
int ui_settings_cols(void);

/* Download progress frame for file `name`; pct < 0 when the size is
 * unknown, total = bytes so far. Redraw at will (cheap on ANSI, the
 * caller throttles on the pixel path). */
void ui_download(const char *name, const char *path, int pct, long total);

/* Result line after a download (ok=1 green, ok=0 red), drawn over the
 * browser frame's status area; caller waits for a button afterwards. */
void ui_result(int ok, const char *msg);

#endif /* PS3_UI_H */
