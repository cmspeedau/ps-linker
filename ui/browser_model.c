/*
 * browser_model.c — see browser_model.h.
 *
 * Build (host smoke test):
 *   gcc -std=c11 -Wall -Wextra -Werror -o bm_test browser_model.c \
 *       coverflow.c store_logic.c browser_model_test.c
 */

#include "browser_model.h"
#include "store_logic.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static int in_skip(const char *const *skip, int skip_n, const char *cat)
{
    if (!cat || !cat[0]) return 0;                  /* uncategorised: always
                                                       visible under "All"  */
    for (int i = 0; i < skip_n; i++)
        if (skip[i] && strcasecmp(cat, skip[i]) == 0) return 1;
    return 0;
}

/* Add `name` as a tab unless empty or already present (case-insensitive,
   like the categories users see on the share). Returns nothing; `tabs`
   fills up to COVERFLOW_MAX_DYNAMIC_CATEGORIES. */
static void add_tab(char tabs[][COVERFLOW_DYNAMIC_CATEGORY_LEN], int *tabs_n,
                    const char *name)
{
    if (!name || !name[0]) return;
    if (*tabs_n >= COVERFLOW_MAX_DYNAMIC_CATEGORIES) return;
    for (int i = 0; i < *tabs_n; i++)
        if (strcasecmp(tabs[i], name) == 0) return;
    snprintf(tabs[*tabs_n], COVERFLOW_DYNAMIC_CATEGORY_LEN, "%s", name);
    (*tabs_n)++;
}

int bm_build(coverflow_item_t *items, int cap,
             const bm_entry_t *share, int share_n,
             const bm_entry_t *dl, int dl_n,
             const char *const *skip, int skip_n,
             int downloads_active,
             char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES]
                     [COVERFLOW_DYNAMIC_CATEGORY_LEN],
             int *tabs_n,
             int *map)
{
    if (tabs_n) *tabs_n = 0;

    /* Tab rail, always derived from the SHARE listing so it does not change
       when the Downloads view is open: All, then share categories in
       first-appearance order, then Downloads. "" is the wildcard tab
       (renders as "All"); coverflow_set_categories keeps it. */
    if (tabs && tabs_n) {
        tabs[0][0] = '\0';                     /* the All tab */
        (*tabs_n) = 1;
        for (int i = 0; i < share_n && *tabs_n < COVERFLOW_MAX_DYNAMIC_CATEGORIES - 1; i++)
            if (!in_skip(skip, skip_n, share[i].category))
                add_tab(tabs, tabs_n, share[i].category);
        if (dl)
            add_tab(tabs, tabs_n, BM_DOWNLOADS_LABEL);
    }

    const bm_entry_t *src = downloads_active ? dl : share;
    int n = downloads_active ? dl_n : share_n;
    if (downloads_active && !src) { src = NULL; n = 0; }

    int out = 0;
    for (int i = 0; i < n && out < cap; i++) {
        const bm_entry_t *e = &src[i];
        if (!e->name || !e->name[0] || e->name[0] == '.') continue;
        if (!downloads_active && in_skip(skip, skip_n, e->category)) continue;

        coverflow_item_t *it = &items[out];
        memset(it, 0, sizeof *it);
        snprintf(it->title, sizeof it->title, "%.63s", e->name);
        if (e->is_dir) {
            size_t tl = strlen(it->title);
            if (tl + 1 < sizeof it->title) {      /* folders read "Name/" */
                it->title[tl] = '/';
                it->title[tl + 1] = '\0';
            }
        }
        if (downloads_active)
            snprintf(it->category, sizeof it->category, "%s",
                     BM_DOWNLOADS_LABEL);
        else
            snprintf(it->category, sizeof it->category, "%.31s",
                     e->category ? e->category : "");
        if (e->size >= 0) {
            char sz[24];
            store_format_size((uint64_t)e->size, sz, sizeof sz);
            snprintf(it->size_human, sizeof it->size_human, "%.15s", sz);
        }
        it->is_download = downloads_active;
        if (map) map[out] = i;
        out++;
    }
    return out;
}
