/*
 * PS Linker - Store catalog parser (shared by the PSP and PS3 apps)
 *
 * Reads <NAS_BASE>/catalog.json in either of its two forms: the
 * hand-written `items` catalog (docs/STORE.md) or generate_catalog.py's
 * `entries` list (integer `size`, formatted here). No device headers, so
 * the host test drives it directly.
 *
 * The catalog is machine-generated and well-formed, so this is a tolerant
 * scan ("key": then the quoted value) rather than a full JSON parser.
 */

#ifndef PS_LINKER_CATALOG_H
#define PS_LINKER_CATALOG_H

#include <stddef.h>
#include <stdint.h>

#include "store_logic.h"

#define CATALOG_MAX_STR 256

typedef struct {
    char id[CATALOG_MAX_STR];
    char title[CATALOG_MAX_STR];
    char category[CATALOG_MAX_STR];
    char description[CATALOG_MAX_STR];
    char version[32];
    char platform[16];            /* sidecar "platform" ("PSP", "PS3"), or "" */
    char url[CATALOG_MAX_STR];
    char install_path[CATALOG_MAX_STR];
    char size_hint[16];
    uint64_t size;
} catalog_item_t;

typedef struct {
    catalog_item_t *items;        /* caller-owned storage of `cap` items */
    int             cap;
    int             count;
    char            categories[STORE_CATEGORY_LIMIT][STORE_CATEGORY_LEN];
    int             category_count;
} catalog_t;

/* Parse `json` into `cat` (whose items/cap the caller set). Items without
 * a url or title are skipped; categories are first-seen and unique.
 * Returns the item count. */
int catalog_parse(catalog_t *cat, const char *json);

#endif /* PS_LINKER_CATALOG_H */
