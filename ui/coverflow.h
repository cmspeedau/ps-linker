/*
 * coverflow.h — on-device Cover Flow UI for PSP/PS3 ANSI console apps.
 *
 * Shared by the PSL1GHT (PS3) and pspdev (PSP) apps, which today render ANSI
 * console output (the RSX pixel renderer is still a roadmap item).
 *
 * Design constraints:
 *   - One allocation: the filtered-index buffer created by coverflow_init().
 *   - No graphics primitives — works on plain stdout (netdebug/TTY on PS3,
 *     the debug screen on PSP).
 *   - Explicit state machine, no globals; the caller owns the item storage.
 *   - Keys are abstracted (left/right/enter/back/shoulder/tab/favorite) so the
 *     PSP dpad and the Sixaxis map onto the same handler.
 *
 * NOTE: `items` is NOT const. COVERFLOW_KEY_FAVORITE writes through it
 * (is_favorite lives in the item), so the caller must pass writable storage
 * — a `static const` table lives in .rodata and the toggle will fault.
 *
 * See coverflow.c for the implementation and coverflow_test.c for the host
 * test.
 */

#ifndef PS_LINKER_COVERFLOW_H
#define PS_LINKER_COVERFLOW_H

#include <stddef.h>

/* ---- Item model -------------------------------------------------------- */

typedef enum {
    COVERFLOW_CAT_ALL = 0,
    COVERFLOW_CAT_PSP,
    COVERFLOW_CAT_PS3,
    COVERFLOW_CAT_RECENT,
    COVERFLOW_CAT_FAVORITES,
    COVERFLOW_CAT_DOWNLOADS,
    COVERFLOW_CAT_SETTINGS,
    COVERFLOW_CAT_COUNT
} coverflow_category_t;

typedef struct {
    char        title[64];
    char        category[24];     /* "PSP", "PS3", "Homebrew", ... */
    char        size_human[16];   /* e.g. "1.2 GB" */
    char        version[16];      /* e.g. "v1.05" — empty if unknown */
    int         is_favorite;
    int         is_recent;        /* set by caller from mtime */
    int         is_download;      /* currently in progress */
} coverflow_item_t;

/* ---- View state -------------------------------------------------------- */

typedef enum {
    COVERFLOW_VIEW_FLOW = 0,      /* horizontal carousel (default) */
    COVERFLOW_VIEW_GRID           /* 3xN text grid */
} coverflow_view_t;

typedef struct {
    /* Inputs. NOT const: the favorite toggle updates is_favorite in place,
       so the caller must supply writable storage. */
    coverflow_item_t *items;
    size_t            item_count;
    int               screen_w;   /* columns; e.g. PSP=60, PS3=80 */
    int               screen_h;   /* rows;    e.g. PSP=34, PS3=40 */
    int               has_colors; /* 0=plain, 1=ANSI color ok */

    /* Selection / filter state */
    size_t               index;      /* slot in `filtered` (the selection) */
    coverflow_category_t category;
    coverflow_view_t     view;
    int                  favorite_toggle_pending; /* set here, cleared by the
                                                     caller once persisted  */

    /* Derived (computed by coverflow_recompute_filter) */
    size_t *filtered;      /* heap-owned indices into items[]; size=item_count */
    size_t  filtered_len;
} coverflow_state_t;

/* ---- Keys -------------------------------------------------------------- */

typedef enum {
    COVERFLOW_KEY_NONE = 0,
    COVERFLOW_KEY_LEFT,
    COVERFLOW_KEY_RIGHT,
    COVERFLOW_KEY_UP,
    COVERFLOW_KEY_DOWN,
    COVERFLOW_KEY_ENTER,
    COVERFLOW_KEY_BACK,
    COVERFLOW_KEY_SHOULDER_L,   /* prev category */
    COVERFLOW_KEY_SHOULDER_R,   /* next category */
    COVERFLOW_KEY_TAB,          /* toggle view (flow ⇄ grid) */
    COVERFLOW_KEY_FAVORITE      /* toggle favorite on current */
} coverflow_key_t;

/* ---- Lifecycle --------------------------------------------------------- */

typedef void (*coverflow_render_fn)(const coverflow_state_t *s);

/* Initialise state. `items` must be writable (see the note at the top of this
 * header). `filtered` is heap-owned by the module; the caller must call
 * coverflow_shutdown() to free it. Returns 0 on success, -1 on OOM. */
int  coverflow_init(coverflow_state_t *s,
                    coverflow_item_t *items, size_t n,
                    int screen_w, int screen_h, int has_colors);

/* Release internal buffers. Safe to call on a zero-initialised state. */
void coverflow_shutdown(coverflow_state_t *s);

/* Re-derive the filtered index list for the current category. Call after
 * changing category, after items change, or after toggling favorite/recent. */
void coverflow_recompute_filter(coverflow_state_t *s);

/* Feed a key. Returns 1 if the key caused a state change that requires a
 * redraw, 0 otherwise. COVERFLOW_KEY_ENTER / COVERFLOW_KEY_BACK are also
 * delivered to the caller via *out_action (0 = no action). */
typedef enum {
    COVERFLOW_ACTION_NONE = 0,
    COVERFLOW_ACTION_SELECT,    /* user pressed Enter on current item */
    COVERFLOW_ACTION_BACK,      /* user pressed Back / O */
    COVERFLOW_ACTION_SETTINGS   /* user navigated into Settings */
} coverflow_action_t;

int  coverflow_handle_key(coverflow_state_t *s,
                          coverflow_key_t key,
                          coverflow_action_t *out_action);

/* The selected item, or NULL when the current category is empty. Callers use
 * this on COVERFLOW_ACTION_SELECT to know what the user picked. */
const coverflow_item_t *coverflow_current(const coverflow_state_t *s);

/* Build the category menu row (ANSI-marked, padded to screen_w columns) into
 * dst. Returns the number of bytes written. When the full menu is too wide
 * for the screen it windows around the current category and marks what is
 * off-screen with '<' / '>'. Exposed so a caller with its own frame layout can
 * reuse it (and so the host test can assert on it without scraping stdout). */
size_t coverflow_menu_row(const coverflow_state_t *s, char *dst, size_t cap);

/* Default renderer. Writes the Cover Flow frame to stdout. Respects
 * `has_colors` so PSP (no ANSI) gets a clean monochrome layout. */
void coverflow_render_default(const coverflow_state_t *s);

/* Category labels — shared string table for menu bar / settings. */
const char *coverflow_category_label(coverflow_category_t c);

#endif /* PS_LINKER_COVERFLOW_H */
