/*
 * browser_model.h — turns a directory listing into Cover Flow items + tabs.
 *
 * The device browsers (psp/main.c, ps3/main.c) parse the share's HTML
 * listing into their own FileEntry tables, then hand the model a view of
 * it (name/category/size/is_dir) plus the local Downloads listing. The
 * model owns everything the Cover Flow frame needs beyond that:
 *
 *   - coverflow_item_t rows: title = display name (folders carry a
 *     trailing '/'), size_human from store_format_size, category verbatim.
 *   - the tab rail: a wildcard "" tab (renders as "All"), one tab per
 *     share category in first-appearance order, then "Downloads".
 *   - skip list: the PSP passes {"Licenses","PS3"} — categories the PSP
 *     cannot use never become covers or tabs.
 *   - a slot -> source-entry map, so the caller can recover its own
 *     entry (raw name, url) from coverflow_current().
 *
 * No allocation, no globals, host-testable (see browser_model_test.c).
 */

#ifndef PS_LINKER_BROWSER_MODEL_H
#define PS_LINKER_BROWSER_MODEL_H

#include "coverflow.h"

typedef struct {
    const char *name;       /* display name; no brackets, no trailing slash */
    const char *category;   /* share data-cat; "" when the share says none  */
    const char *platform;   /* share data-platform ("PSP"/"PS3"); NULL/""
                               when the share says nothing                  */
    long long   size;       /* bytes, or -1 when unknown                    */
    int         is_dir;
} bm_entry_t;

#define BM_DOWNLOADS_LABEL "Downloads"

/* Fill `items` (capacity `cap`) from the share listing, or from the
 * Downloads listing when `downloads_active`, and build the tab rail.
 *
 *   items           out: coverflow rows, filled in entry order
 *   map             out: map[i] = index into the active source array
 *   share/share_n   the parsed listing
 *   dl/dl_n         local Downloads entries (dl == NULL removes the tab)
 *   skip/skip_n     category names to hide (case-insensitive); entries in
 *                   them, and entries whose name starts with '.', are not
 *                   shown at all
 *   hide/hide_n     category names that get NO tab but keep their entries
 *                   (visible on the All tab; the PS3 passes {"PS3"} — the
 *                   machine is that category already)
 *   console         "PSP"/"PS3": files that machine cannot use
 *                   (store_platform_ok) are dropped from items AND from
 *                   tab derivation; directories and Downloads entries pass
 *                   untouched; NULL disables the filter
 *   downloads_active  non-zero: the items are built from dl instead
 *   tabs/tabs_n     out: tab names, first entry "" (the All tab)
 *
 * Tabs are capped at COVERFLOW_MAX_DYNAMIC_CATEGORIES: "" and "Downloads"
 * are kept, share categories beyond the limit are dropped (their covers
 * still live in the All tab). Returns the number of items written. */
int bm_build(coverflow_item_t *items, int cap,
             const bm_entry_t *share, int share_n,
             const bm_entry_t *dl, int dl_n,
             const char *const *skip, int skip_n,
             const char *const *hide, int hide_n,
             const char *console,
             int downloads_active,
             char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES]
                     [COVERFLOW_DYNAMIC_CATEGORY_LEN],
             int *tabs_n,
             int *map);

#endif /* PS_LINKER_BROWSER_MODEL_H */
