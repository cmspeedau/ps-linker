/*
 * coverflow.c — on-device Cover Flow implementation.
 *
 * ANSI-only, no GPU, and one allocation (the filtered-index buffer). Renders
 * the carousel + category menu bar the device apps need:
 *
 *   - Horizontal carousel; centered cover highlighted (drawn larger /
 *     brighter), neighbors faded.
 *   - Top menu bar with categories: All Games | PSP Games | PS3 Games |
 *     Recently Added | Favorites | Downloads | Settings.
 *   - Shoulder buttons cycle categories; L/R arrows flip covers.
 *   - Toggle favorite on current item.
 *   - Tab toggles Flow ⇄ Grid view.
 *   - Enter selects; Back/O returns.
 *
 * Build (host smoke test):
 *   gcc -std=c11 -Wall -Wextra -o coverflow_test coverflow.c coverflow_test.c
 */

#include "coverflow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ANSI escapes (only used when caller reports has_colors=1) -------- */

#define ANSI_RESET     "\x1b[0m"
#define ANSI_DIM       "\x1b[2m"
#define ANSI_BOLD      "\x1b[1m"
#define ANSI_REV       "\x1b[7m"
#define ANSI_CLEAR     "\x1b[2J\x1b[H"

static const char *const kCategoryLabels[COVERFLOW_CAT_COUNT] = {
    [COVERFLOW_CAT_ALL]       = "All Games",
    [COVERFLOW_CAT_PSP]       = "PSP Games",
    [COVERFLOW_CAT_PS3]       = "PS3 Games",
    [COVERFLOW_CAT_RECENT]    = "Recently Added",
    [COVERFLOW_CAT_FAVORITES] = "Favorites",
    [COVERFLOW_CAT_DOWNLOADS] = "Downloads",
    [COVERFLOW_CAT_SETTINGS]  = "Settings",
};

const char *coverflow_category_label(coverflow_category_t c) {
    if ((int)c < 0 || (int)c >= COVERFLOW_CAT_COUNT) return "?";
    return kCategoryLabels[c];
}

/* ---- Lifecycle --------------------------------------------------------- */

int coverflow_init(coverflow_state_t *s,
                   coverflow_item_t *items, size_t n,
                   int screen_w, int screen_h, int has_colors) {
    if (!s) return -1;
    memset(s, 0, sizeof(*s));
    s->items      = items;
    s->item_count = n;
    s->screen_w   = screen_w > 0 ? screen_w : 60;
    s->screen_h   = screen_h > 0 ? screen_h : 34;
    s->has_colors = has_colors ? 1 : 0;
    s->index      = 0;
    s->category   = COVERFLOW_CAT_ALL;
    s->view       = COVERFLOW_VIEW_FLOW;
    if (n == 0) return 0;
    s->filtered = (size_t *)calloc(n, sizeof(size_t));
    if (!s->filtered) return -1;
    coverflow_recompute_filter(s);
    return 0;
}

void coverflow_shutdown(coverflow_state_t *s) {
    if (!s) return;
    free(s->filtered);
    s->filtered = NULL;
    s->filtered_len = 0;
}

/* ---- Filtering --------------------------------------------------------- */

static int item_matches_category(const coverflow_item_t *it,
                                 coverflow_category_t c) {
    switch (c) {
        case COVERFLOW_CAT_ALL:       return 1;
        case COVERFLOW_CAT_PSP:       return strncmp(it->category, "PSP", 3) == 0;
        case COVERFLOW_CAT_PS3:       return strncmp(it->category, "PS3", 3) == 0;
        case COVERFLOW_CAT_RECENT:    return it->is_recent;
        case COVERFLOW_CAT_FAVORITES: return it->is_favorite;
        case COVERFLOW_CAT_DOWNLOADS: return it->is_download;
        case COVERFLOW_CAT_SETTINGS:  return 0; /* Settings has no items */
        default:                      return 0;
    }
}

void coverflow_recompute_filter(coverflow_state_t *s) {
    if (!s || !s->filtered) { s->filtered_len = 0; s->index = 0; return; }

    /* `index` is a slot in `filtered`, so the item it points at has to be
       captured before the list is rebuilt — comparing the old slot number
       against new item ids keeps the wrong cover selected. */
    size_t keep = (s->index < s->filtered_len) ? s->filtered[s->index] : 0;

    s->filtered_len = 0;
    for (size_t i = 0; i < s->item_count; i++) {
        if (item_matches_category(&s->items[i], s->category))
            s->filtered[s->filtered_len++] = i;
    }
    if (s->filtered_len == 0) { s->index = 0; return; }

    /* Keep the selection on the same item when re-filtering. */
    s->index = 0;
    for (size_t i = 0; i < s->filtered_len; i++) {
        if (s->filtered[i] == keep) { s->index = i; break; }
    }
    if (s->index >= s->filtered_len) s->index = s->filtered_len - 1;
}

/* ---- Helpers ----------------------------------------------------------- */

const coverflow_item_t *coverflow_current(const coverflow_state_t *s) {
    if (!s || s->filtered_len == 0) return NULL;
    if (s->index >= s->filtered_len) return NULL;
    return &s->items[s->filtered[s->index]];
}

static const char *dim_open (int c) { return c ? ANSI_DIM  : ""; }
static const char *bold_open(int c) { return c ? ANSI_BOLD : ""; }
static const char *rev_open (int c) { return c ? ANSI_REV  : ""; }
static const char *close    (int c) { return c ? ANSI_RESET : ""; }

/* Repeat a char into a stack buffer; truncates safely. */
static void repeat(char *dst, size_t cap, char ch, int n) {
    if (cap == 0) return;
    if (n < 0) n = 0;
    if ((size_t)n >= cap - 1) n = (int)(cap - 1);
    memset(dst, ch, (size_t)n);
    dst[n] = '\0';
}

/* Centred, buffer-bounded tile text: writes exactly inner_w columns (clipped
   to cap) or truncates with "..". Built by hand rather than with "%*s%s%*s"
   so the compiler can see the destination can never overflow — psp-gcc warns
   on the printf form because it cannot bound an incoming `const char *`. */
static void cover_title(char *dst, size_t cap, const char *title, int inner_w) {
    if (cap == 0) return;
    if (inner_w <= 0) { dst[0] = '\0'; return; }

    const size_t max = cap - 1;                    /* leave room for the NUL */
    size_t tlen = strlen(title);
    size_t n = 0;

    if (tlen > (size_t)inner_w) {                  /* does not fit: cut it */
        size_t keep = ((size_t)inner_w < max) ? (size_t)inner_w : max;
        if (keep >= 2) {
            memcpy(dst, title, keep - 2);
            dst[keep - 2] = '.';
            dst[keep - 1] = '.';
            dst[keep] = '\0';
            return;
        }
        tlen = keep;
    }

    const size_t pad = (size_t)inner_w - tlen;
    const size_t left = pad / 2;
    while (n < left && n < max) dst[n++] = ' ';
    for (size_t i = 0; i < tlen && n < max; i++) dst[n++] = title[i];
    while ((int)n < inner_w && n < max) dst[n++] = ' ';
    dst[n] = '\0';
}

/* ---- Rendering: menu bar ---------------------------------------------- */

/* Wide enough for the widest label set plus the ANSI escapes around them. */
#define CF_LINE 256

size_t coverflow_menu_row(const coverflow_state_t *s, char *dst, size_t cap) {
    if (!s || cap == 0) { if (dst) dst[0] = '\0'; return 0; }

    const int w = s->screen_w;
    const int colors = s->has_colors;
    /* Never trust the caller's enum: an out-of-range category must not index
       the label table. */
    int cur = (int)s->category;
    if (cur < 0 || cur >= COVERFLOW_CAT_COUNT) cur = COVERFLOW_CAT_ALL;
    const char *cur_lbl = coverflow_category_label((coverflow_category_t)cur);
    const int cur_need = (int)strlen(cur_lbl) + 3;   /* "[Label] " */

    /* How wide is the full menu? */
    int total = 1;
    for (int c = 0; c < COVERFLOW_CAT_COUNT; c++)
        total += (int)strlen(kCategoryLabels[c]) + 3;

    /* Too wide for the screen (always true for 7 labels at 80 columns): show a
       window around the current category so the user can always see where
       they are, with < / > markers for what is off-screen. ASCII markers,
       because the device consoles are not guaranteed to render anything else. */
    int start = 0, end = COVERFLOW_CAT_COUNT;
    if (total > w) {
        const int span = 3;
        start = cur - span / 2;
        if (start < 0) start = 0;
        end = start + span;
        if (end > COVERFLOW_CAT_COUNT) {
            end = COVERFLOW_CAT_COUNT;
            start = end - span;
            if (start < 0) start = 0;
        }
    }

    /* Columns left for the current label once the leading space and the two
       off-screen markers are accounted for. The current label is drawn inside
       this, truncating if it has to. */
    const int mark_l = (start > 0) ? 1 : 0;
    const int mark_r = (end < COVERFLOW_CAT_COUNT) ? 1 : 0;
    const int avail = w - 1 - mark_l - mark_r;
    const int cur_w = (cur_need <= avail) ? cur_need : (avail >= 6 ? avail : 0);

    /* Neighbours have to leave room for the current label. */
    int budget = w - cur_w - 1 - mark_l - mark_r;
    if (budget < 0) budget = 0;

    size_t n = 0;
    int cols = 0;

    if (n + 1 < cap) { dst[n++] = ' '; cols = 1; }
    if (mark_l && cols < w && n + 1 < cap) { dst[n++] = '<'; cols++; }

    for (int c = start; c < cur; c++) {
        const int need = (int)strlen(kCategoryLabels[c]) + 3;
        if (cols + need > budget) continue;
        if (n + (size_t)need + 16 >= cap) break;
        n += (size_t)snprintf(dst + n, cap - n, " %s ", kCategoryLabels[c]);
        cols += need;
    }

    /* The current category is always drawn — truncated if the screen is too
       narrow for the whole label, but never dropped. */
    if (cur_w >= 6 && n + (size_t)cur_w + 16 < cap) {
        if (cur_need <= avail) {
            n += (size_t)snprintf(dst + n, cap - n, "[%s%s%s] ",
                                  rev_open(colors), cur_lbl, close(colors));
            cols += cur_need;
        } else {
            char cut[64];
            int keep = cur_w - 5;                       /* "[" + "..]" + " " */
            const int fit = (int)sizeof(cut) - 3;       /* room for the dots */
            if (keep > fit) keep = fit;
            if (keep < 1) keep = 1;
            memcpy(cut, cur_lbl, (size_t)keep);
            cut[keep] = '.';
            cut[keep + 1] = '.';
            cut[keep + 2] = '\0';
            n += (size_t)snprintf(dst + n, cap - n, "[%s%s%s] ",
                                  rev_open(colors), cut, close(colors));
            cols += (int)strlen(cut) + 3;
        }
    }

    for (int c = cur + 1; c < end; c++) {
        const int need = (int)strlen(kCategoryLabels[c]) + 3;
        if (cols + need > w) continue;
        if (n + (size_t)need + 16 >= cap) break;
        n += (size_t)snprintf(dst + n, cap - n, " %s ", kCategoryLabels[c]);
        cols += need;
    }

    if (mark_r && cols < w && n + 1 < cap) { dst[n++] = '>'; cols++; }
    while (cols < w && n + 1 < cap) { dst[n++] = ' '; cols++; }
    dst[n] = '\0';
    return n;
}

static void render_menu_bar(const coverflow_state_t *s) {
    char line[CF_LINE];
    char rule[CF_LINE];

    printf("%s%s%s\n", bold_open(s->has_colors), "PS Linker",
           close(s->has_colors));
    coverflow_menu_row(s, line, sizeof line);
    printf("%s\n", line);

    /* Divider */
    repeat(rule, sizeof rule, '-', s->screen_w);
    printf("%s\n", rule);
}

/* ---- Rendering: flow view --------------------------------------------- */

/* One row of a bordered text tile: a "+---+" rule on the first and last row,
   otherwise "|text|" with `text` centred (NULL text = blank row). Everything
   is built into the caller's buffer, so nothing depends on printf width
   specifiers going negative when the screen is narrow. */
static void tile_row(char *dst, size_t cap, const char *text,
                     int inner_w, int row, int height) {
    if (cap == 0) return;
    if (inner_w < 1) inner_w = 1;
    if (height < 3) height = 3;

    size_t n = 0;
    const int edge = (row == 0 || row == height - 1);
    if (n + 1 < cap) dst[n++] = edge ? '+' : '|';

    if (edge) {
        while ((int)n < inner_w + 1 && n + 1 < cap) dst[n++] = '-';
    } else {
        char padded[CF_LINE];
        cover_title(padded, sizeof padded, text ? text : "", inner_w);
        for (int i = 0; i < inner_w && (int)n + 1 < (int)cap; i++)
            dst[n++] = padded[i];
    }
    if (n + 1 < cap) dst[n++] = edge ? '+' : '|';
    if (n < cap) dst[n] = '\0';
}

static void render_flow(const coverflow_state_t *s) {
    if (s->filtered_len == 0) {
        printf("\n  (no items in '%s')\n",
               coverflow_category_label(s->category));
        return;
    }

    const int colors = s->has_colors;
    const int li = (s->index > 0) ? (int)s->index - 1 : -1;
    const int ri = (s->index + 1 < s->filtered_len) ? (int)s->index + 1 : -1;
    const coverflow_item_t *cur = coverflow_current(s);
    const coverflow_item_t *lit = (li >= 0) ? &s->items[s->filtered[li]] : NULL;
    const coverflow_item_t *rit = (ri >= 0) ? &s->items[s->filtered[ri]] : NULL;

    /* Side tiles stay narrow, the centre tile is wider and taller. Both are
       clamped so a 20-column screen cannot make the fields go negative. */
    int side_w = (s->screen_w - 6) / 4;
    if (side_w < 8)  side_w = 8;
    if (side_w > 26) side_w = 26;
    int cen_w = side_w + 6;
    if (cen_w > 40) cen_w = 40;
    const int side_h = 7;
    const int cen_h  = 11;

    char rule[CF_LINE], lcell[CF_LINE], mcell[CF_LINE], rcell[CF_LINE];

    repeat(rule, sizeof rule, ' ', s->screen_w);
    printf("%s\n", rule);

    /* Walk the CENTRE tile's height. The side tiles are shorter and are
       blank-padded underneath: iterating over side_h instead would silently
       drop the centre tile's lower half, bottom border included. */
    for (int row = 0; row < cen_h; row++) {
        if (lit && row < side_h) {
            tile_row(lcell, sizeof lcell,
                     (row == side_h / 2) ? lit->title : NULL,
                     side_w - 2, row, side_h);
            printf("%s%s%s", dim_open(colors), lcell, close(colors));
        } else {
            printf("%*s", side_w, "");
        }

        const char *text = NULL;
        if (row == cen_h / 2 - 1)            text = "[COVER ART]";
        else if (row == cen_h / 2)           text = cur ? cur->title : NULL;
        else if (row == cen_h / 2 + 1 && cur && cur->category[0])
            text = cur->category;
        tile_row(mcell, sizeof mcell, text, cen_w - 2, row, cen_h);
        printf("  %s%s%s", bold_open(colors), mcell, close(colors));

        if (rit && row < side_h) {
            tile_row(rcell, sizeof rcell,
                     (row == side_h / 2) ? rit->title : NULL,
                     side_w - 2, row, side_h);
            printf("  %s%s%s", dim_open(colors), rcell, close(colors));
        } else {
            printf("  %*s", side_w, "");
        }
        printf("\n");
    }

    /* Metadata strip (centred cover) */
    if (cur) {
        char meta[CF_LINE];
        snprintf(meta, sizeof meta, "%s%s%s%s%s  %s%s",
                 cur->title,
                 cur->version[0] ? "  " : "",
                 cur->version,
                 cur->size_human[0] ? "  " : "",
                 cur->size_human,
                 cur->is_favorite ? "[FAV]" : "",
                 cur->is_download ? "[DL]" : "");
        printf("\n%s%s%s\n", bold_open(colors), meta, close(colors));
    }

    /* Position indicator (e.g.  [3 / 12]) */
    char pos[64];
    snprintf(pos, sizeof pos, "[ %zu / %zu ]",
             s->index + 1, s->filtered_len);
    printf("%s\n", pos);
}

/* ---- Rendering: grid view --------------------------------------------- */

static void render_grid(const coverflow_state_t *s) {
    if (s->filtered_len == 0) {
        printf("\n  (no items in '%s')\n",
               coverflow_category_label(s->category));
        return;
    }
    int cols = s->screen_w >= 80 ? 4 : 3;
    int cellw = s->screen_w / cols;
    if (cellw < 8) cellw = 8;              /* never a zero/negative cell */

    /* One screenful at a time, so the highlighted cell is always on screen:
       printing every item scrolls the footer (and the selection) off the
       bottom as soon as the library is longer than a page. */
    const int rows = (s->screen_h > 10) ? s->screen_h - 10 : 4;
    const size_t per_page = (size_t)cols * (size_t)rows;
    const size_t page = s->index / per_page;
    const size_t first = page * per_page;
    size_t last = first + per_page;
    if (last > s->filtered_len) last = s->filtered_len;

    int col = 0;
    char title[80];
    for (size_t i = first; i < last; i++) {
        const coverflow_item_t *it = &s->items[s->filtered[i]];
        const int is_cur = (i == s->index);
        const char *o = is_cur ? rev_open(s->has_colors) : "";
        const char *e = is_cur ? close(s->has_colors)    : "";
        cover_title(title, sizeof title, it->title, cellw - 4);
        printf("%s[ %s ]%s", o, title, e);
        col++;
        if (col == cols) { col = 0; printf("\n"); }
    }
    if (col != 0) printf("\n");

    const size_t pages = (s->filtered_len + per_page - 1) / per_page;
    printf("[ page %zu / %zu ]\n", page + 1, pages);
}

/* ---- Top-level render -------------------------------------------------- */

void coverflow_render_default(const coverflow_state_t *s) {
    if (!s) return;
    if (s->has_colors) printf(ANSI_CLEAR);
    render_menu_bar(s);
    if (s->view == COVERFLOW_VIEW_FLOW) render_flow(s);
    else                                render_grid(s);

    /* Footer */
    printf("\n[L/R] cover  [Up/Dn] jump  [L1/R1] category  "
           "[Tab] view  [%sFav%s] fav  [Enter] open  [Back] exit\n",
           bold_open(s->has_colors), close(s->has_colors));
}

/* ---- Input handling ---------------------------------------------------- */

int coverflow_handle_key(coverflow_state_t *s, coverflow_key_t key,
                         coverflow_action_t *out_action) {
    if (out_action) *out_action = COVERFLOW_ACTION_NONE;
    if (!s) return 0;

    switch (key) {
        case COVERFLOW_KEY_LEFT:
            if (s->filtered_len == 0 || s->index == 0) return 0;
            s->index--;
            return 1;

        case COVERFLOW_KEY_RIGHT:
            if (s->index + 1 >= s->filtered_len) return 0;
            s->index++;
            return 1;

        case COVERFLOW_KEY_UP: {
            /* Jump by "page" — treat as 5 covers back. */
            if (s->filtered_len == 0 || s->index == 0) return 0;
            s->index = (s->index >= 5) ? s->index - 5 : 0;
            return 1;
        }

        case COVERFLOW_KEY_DOWN: {
            if (s->filtered_len == 0) return 0;
            size_t next = s->index + 5;
            if (next >= s->filtered_len) next = s->filtered_len - 1;
            if (next == s->index) return 0;
            s->index = next;
            return 1;
        }

        case COVERFLOW_KEY_SHOULDER_L:
            if ((int)s->category > 0) {
                s->category = (coverflow_category_t)((int)s->category - 1);
                coverflow_recompute_filter(s);
                return 1;
            }
            return 0;

        case COVERFLOW_KEY_SHOULDER_R:
            if ((int)s->category + 1 < COVERFLOW_CAT_COUNT) {
                s->category = (coverflow_category_t)((int)s->category + 1);
                coverflow_recompute_filter(s);
                return 1;
            }
            return 0;

        case COVERFLOW_KEY_TAB:
            s->view = (s->view == COVERFLOW_VIEW_FLOW)
                      ? COVERFLOW_VIEW_GRID : COVERFLOW_VIEW_FLOW;
            return 1;

        case COVERFLOW_KEY_FAVORITE: {
            /* items[] is writable (see coverflow.h): this is the caller's
               copy, so the toggle lands where the caller can persist it. */
            coverflow_item_t *it = s->filtered_len && s->index < s->filtered_len
                                   ? &s->items[s->filtered[s->index]] : NULL;
            if (!it) return 0;
            it->is_favorite = !it->is_favorite;
            s->favorite_toggle_pending = 1;
            /* If we are filtering by favorites, the list just changed. */
            if (s->category == COVERFLOW_CAT_FAVORITES)
                coverflow_recompute_filter(s);
            return 1;
        }

        case COVERFLOW_KEY_ENTER:
            if (s->category == COVERFLOW_CAT_SETTINGS) {
                if (out_action) *out_action = COVERFLOW_ACTION_SETTINGS;
                return 1;
            }
            if (s->filtered_len == 0) return 0;
            if (out_action) *out_action = COVERFLOW_ACTION_SELECT;
            return 1;

        case COVERFLOW_KEY_BACK:
            if (out_action) *out_action = COVERFLOW_ACTION_BACK;
            return 1;

        default:
            return 0;
    }
}
