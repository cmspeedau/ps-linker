/*
 * PS Linker - Store catalog parser. See catalog.h.
 *
 * Moved out of psp/store.c unchanged in behaviour so the PS3 Store reads
 * the catalog the same way; `platform` is the one field added.
 */

#include "catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Find the value of "key" at or after *pos; advances *pos past it.
   Returns 1 and fills out on success, 0 if not found. */
static int json_find_string(const char *json, const char *key, size_t *pos,
                            char *out, size_t outlen)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json + *pos, pat);
    if (!p) return 0;
    p += strlen(pat);

    /* skip whitespace and colon */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')
        p++;
    if (*p != '"') return 0;
    p++;                                    /* opening quote */

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < outlen) {
        if (*p == '\\' && p[1]) {           /* basic escapes */
            char c = p[1];
            p += 2;
            switch (c) {
                case 'n': out[o++] = '\n'; break;
                case 't': out[o++] = '\t'; break;
                default:  out[o++] = c;    break;   /* \" \\ \/ etc. */
            }
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    *pos = (size_t)(p - json);              /* caller resumes from here */
    return 1;
}

static uint64_t json_find_uint(const char *json, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') p++;
    if (*p < '0' || *p > '9') return 0;
    return (uint64_t)strtoull(p, NULL, 10);
}

/* Parse one item object starting at *pos (points at '{'). */
static int parse_item(const char *json, size_t *pos, catalog_item_t *it)
{
    const char *p = strstr(json + *pos, "{");
    if (!p) return 0;
    const char *end = strstr(p, "}");
    if (!end) return 0;

    memset(it, 0, sizeof(*it));
    size_t local = 0;
    json_find_string(p, "id",            &local, it->id,            sizeof(it->id));
    local = 0; json_find_string(p, "title",        &local, it->title,        sizeof(it->title));
    local = 0; json_find_string(p, "category",     &local, it->category,     sizeof(it->category));
    local = 0; json_find_string(p, "description",  &local, it->description,  sizeof(it->description));
    local = 0; json_find_string(p, "version",      &local, it->version,      sizeof(it->version));
    local = 0; json_find_string(p, "url",          &local, it->url,          sizeof(it->url));
    local = 0; json_find_string(p, "install_path", &local, it->install_path, sizeof(it->install_path));
    local = 0; json_find_string(p, "size_hint",    &local, it->size_hint,    sizeof(it->size_hint));
    /* "platform" only counts inside this object: a later item's value
       must not leak into one that has none. */
    local = 0;
    if (json_find_string(p, "platform", &local, it->platform, sizeof(it->platform)) &&
        p + local + 1 > end)   /* +1: pos rests on the value's closing quote */
        it->platform[0] = '\0';
    it->size = json_find_uint(p, "size");
    if (!it->size_hint[0] && it->size)
        store_format_size(it->size, it->size_hint, sizeof(it->size_hint));

    *pos = (size_t)(end - json) + 1;

    /* An item without a download URL is not useful; skip it. */
    return it->url[0] != '\0' && it->title[0] != '\0';
}

int catalog_parse(catalog_t *cat, const char *json)
{
    if (!cat) return 0;
    cat->count = 0;
    cat->category_count = 0;
    if (!json || !cat->items || cat->cap <= 0) return 0;

    /* Read declared category order first (falls back to discovery). */
    size_t pos = 0;
    char c[STORE_CATEGORY_LEN];
    while (cat->category_count < STORE_CATEGORY_LIMIT &&
           json_find_string(json, "category", &pos, c, sizeof(c))) {
        /* json_find_string also matches "category" inside items; only
           the first pass here seeds the tab order, and store_category_add
           de-duplicates either way. */
        store_category_add(cat->categories, &cat->category_count, c);
    }

    const char *items = strstr(json, "\"items\"");
    if (!items) items = strstr(json, "\"entries\"");
    if (!items) return 0;

    size_t ipos = (size_t)(items - json);
    catalog_item_t it;
    while (cat->count < cat->cap && parse_item(json, &ipos, &it)) {
        cat->items[cat->count++] = it;
        store_category_add(cat->categories, &cat->category_count, it.category);
    }
    return cat->count;
}
