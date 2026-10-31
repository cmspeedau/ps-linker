/*
 * catalog_test.c — host test for the shared Store catalog parser.
 *
 * The parser is shared by both device Stores (psp/store.c and ps3/store.c
 * parse nothing themselves), so the format handling is pinned here:
 * both catalog shapes, the integer `size` formatting, category seeding and
 * the per-object bound on the `platform` field.
 */

#include "catalog.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MAXI 8
static catalog_item_t items[MAXI];

int main(void)
{
    catalog_t cat = { items, MAXI, 0, {{0}}, 0 };

    /* --- generate_catalog.py form: "entries", integer size -------------- *
     * (the generator emits no category list; tabs come from item order) */
    static const char *gen =
        "{\"generated\":\"now\",\"entries\":["
        "  {\"id\":\"Games_Doom_EBOOT\",\"title\":\"EBOOT\",\"category\":\"Games\","
        "   \"url\":\"/Games/Doom/EBOOT.PBP\",\"size\":1572864},"
        "  {\"id\":\"PS3_Quake\",\"title\":\"Quake\",\"category\":\"PS3 Games\","
        "   \"url\":\"/PS3 Games/quake.pkg\",\"size\":39845888,"
        "   \"platform\":\"PS3\",\"version\":\"0.9\","
        "   \"rap\":\"quake.rap\","
        "   \"description\":\"port\\nsecond line\"}"
        " ]}";
    assert(catalog_parse(&cat, gen) == 2);
    assert(strcmp(cat.items[0].id, "Games_Doom_EBOOT") == 0);
    assert(strcmp(cat.items[0].size_hint, "2 MB") == 0);   /* formatted here */
    assert(cat.items[1].size == 39845888ULL);
    assert(strcmp(cat.items[1].version, "0.9") == 0);
    assert(strcmp(cat.items[1].description, "port\nsecond line") == 0);
    assert(strcmp(cat.items[1].platform, "PS3") == 0);
    assert(cat.items[0].platform[0] == '\0');   /* no leak from item 2 */
    assert(strcmp(cat.items[1].rap, "quake.rap") == 0);
    assert(cat.items[0].rap[0] == '\0');         /* no leak from item 2  */
    /* tabs are first-seen item categories, in document order */
    assert(cat.category_count == 2 &&
           strcmp(cat.categories[0], "Games") == 0 &&
           strcmp(cat.categories[1], "PS3 Games") == 0);

    /* --- hand-written form: "items", size_hint pre-formatted ------------ */
    static const char *hand =
        "{\"items\":["
        " {\"id\":\"a\",\"title\":\"A\",\"category\":\"Games\","
        "  \"url\":\"/a.pkg\",\"size_hint\":\"12 MB\"},"
        " {\"id\":\"b\",\"title\":\"B\",\"url\":\"/b.iso\"},"        /* no category */
        " {\"id\":\"c\",\"title\":\"C\",\"category\":\"Games\"}"     /* no url: skipped */
        "]}";
    assert(catalog_parse(&cat, hand) == 2);
    assert(strcmp(cat.items[1].size_hint, "") == 0);
    assert(cat.category_count == 1 && strcmp(cat.categories[0], "Games") == 0);

    /* --- bounds and refusals -------------------------------------------- */
    assert(catalog_parse(&cat, "") == 0);
    assert(catalog_parse(&cat, "[]") == 0);
    assert(catalog_parse(&cat, "{\"items\":[{\"title\":\"x\"}]}") == 0);
    catalog_t small = { items, 1, 0, {{0}}, 0 };
    assert(catalog_parse(&small, gen) == 1);       /* cap respected */
    assert(catalog_parse(NULL, gen) == 0);

    /* platform must stay inside its object: the next object's value must
       not count when this object has none (the "}" between them bounds it) */
    static const char *adj =
        "{\"entries\":["
        " {\"id\":\"a\",\"title\":\"A\",\"url\":\"/a\",\"category\":\"X\"},"
        " {\"id\":\"b\",\"title\":\"B\",\"url\":\"/b\",\"category\":\"X\","
        "  \"platform\":\"PSP\",\"rap\":\"B.rap\"}"
        "]}";
    assert(catalog_parse(&cat, adj) == 2);
    assert(cat.items[0].platform[0] == '\0');
    assert(strcmp(cat.items[1].platform, "PSP") == 0);
    assert(cat.items[0].rap[0] == '\0');
    assert(strcmp(cat.items[1].rap, "B.rap") == 0);

    puts("catalog: all assertions passed");
    return 0;
}
