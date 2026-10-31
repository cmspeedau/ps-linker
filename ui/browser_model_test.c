/*
 * browser_model_test.c — host checks for ui/browser_model.c.
 *
 * Build (or use ui/Makefile: make test):
 *   gcc -std=c11 -Wall -Wextra -Werror -o /tmp/browser_model_test \
 *       browser_model.c coverflow.c store_logic.c browser_model_test.c -lm
 */

#include <stdio.h>
#include <string.h>

#include "browser_model.h"

static int g_fail;

static void check(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); g_fail++; }
    else       { printf("ok:   %s\n", what); }
}

#define ENTRY(name_, cat_, size_, dir_) \
    { name_, cat_, size_, dir_ }

static void test_items_and_map(void) {
    bm_entry_t share[] = {
        ENTRY("Games", "Games", -1, 1),
        ENTRY("Doom.wad", "Games", 3072, 0),
        ENTRY("notes.txt", "", 12, 0),
    };
    coverflow_item_t items[8];
    int map[8];
    char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES][COVERFLOW_DYNAMIC_CATEGORY_LEN];
    int tabs_n = 0;

    int n = bm_build(items, 8, share, 3, NULL, 0, NULL, 0, 0, tabs, &tabs_n, map);
    check(n == 3, "all entries become items");
    check(strcmp(items[0].title, "Games/") == 0, "folder title keeps a trailing slash");
    check(strcmp(items[1].size_human, "3 KB") == 0, "size is human-formatted");
    check(strcmp(items[2].size_human, "0 KB") == 0,
          "tiny files round to 0 KB (store_format_size's rule)");
    check(map[0] == 0 && map[1] == 1 && map[2] == 2, "identity map without skipping");
    check(tabs_n == 2 && tabs[0][0] == '\0' && strcmp(tabs[1], "Games") == 0,
          "tabs = All + first-appearance category, no Downloads tab when dl==NULL");
}

static void test_skip_list(void) {
    bm_entry_t share[] = {
        ENTRY("Games", "Games", -1, 1),
        ENTRY("PS3", "PS3", -1, 1),
        ENTRY("Quake.pkg", "PS3", 2500000, 0),
        ENTRY("Licenses", "Licenses", -1, 1),
        ENTRY("BLES00001.rap", "Licenses", 16, 0),
        ENTRY("README.txt", "", 100, 0),
    };
    const char *skip[] = { "Licenses", "PS3" };
    coverflow_item_t items[8];
    int map[8];
    char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES][COVERFLOW_DYNAMIC_CATEGORY_LEN];
    int tabs_n = 0;

    int n = bm_build(items, 8, share, 6, NULL, 0, skip, 2, 0, tabs, &tabs_n, map);
    check(n == 2, "PS3/Licenses entries hidden on PSP");
    check(map[0] == 0 && map[1] == 5, "map skips over the hidden entries");
    check(tabs_n == 2 && strcmp(tabs[1], "Games") == 0,
          "hidden categories never become tabs");
}

static void test_downloads(void) {
    bm_entry_t share[] = {
        ENTRY("Games", "Games", -1, 1),
        ENTRY("Utilities", "Utilities", -1, 1),
    };
    bm_entry_t dl[] = {
        ENTRY("Doom.wad", "", 3072, 0),
        ENTRY(".hidden.part", "", 512, 0),
    };
    coverflow_item_t items[8];
    int map[8];
    char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES][COVERFLOW_DYNAMIC_CATEGORY_LEN];
    int tabs_n = 0;

    int n = bm_build(items, 8, share, 2, dl, 2, NULL, 0, 0, tabs, &tabs_n, map);
    check(n == 2, "share view shows share entries");
    check(tabs_n == 4 && strcmp(tabs[0], "") == 0 &&
          strcmp(tabs[1], "Games") == 0 && strcmp(tabs[2], "Utilities") == 0 &&
          strcmp(tabs[3], "Downloads") == 0,
          "Downloads is the last tab and the share tabs keep their order");

    n = bm_build(items, 8, share, 2, dl, 2, NULL, 0, 1, tabs, &tabs_n, map);
    check(n == 1, "dot-name files hidden from Downloads");
    check(strcmp(items[0].category, "Downloads") == 0 && items[0].is_download,
          "download items carry the Downloads category");
    check(map[0] == 0, "download map indexes the dl array");
    check(tabs_n == 4 && strcmp(tabs[3], "Downloads") == 0,
          "tab rail is stable while the Downloads view is open");
}

/* The coverflow half the browser relies on: the "" tab must filter to
 * everything and label itself "All". */
static void test_wildcard_tab_in_coverflow(void) {
    static coverflow_item_t items[3];
    strcpy(items[0].category, "Games");
    strcpy(items[1].category, "Utilities");
    strcpy(items[2].category, "");
    strcpy(items[0].title, "a"); strcpy(items[1].title, "b"); strcpy(items[2].title, "c");

    coverflow_state_t s;
    check(coverflow_init(&s, items, 3, 60, 28, 0) == 0, "init for wildcard test");
    const char *cats[] = { "", "Games", "Utilities" };
    coverflow_set_categories(&s, cats, 3);
    check(s.dynamic_category_count == 3, "wildcard tab counts as a category");
    check(s.filtered_len == 3, "the All tab matches every item");

    char line[256];
    coverflow_menu_row(&s, line, sizeof line);
    check(strstr(line, "[All]") != NULL, "wildcard tab renders as [All]");

    coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_R, NULL);
    check(s.category == 1 && s.filtered_len == 1, "next tab filters to Games");

    /* Item swap keeps the tab rail and the position where possible. */
    check(coverflow_set_items(&s, items, 0) == 0, "set_items accepts an empty listing");
    check(s.filtered_len == 0, "empty listing filters empty");
    check(coverflow_set_items(&s, items, 2) == 0, "set_items back to two items");
    check(s.filtered_len == 1, "still filtered by the Games tab");
    coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_L, NULL);
    check(s.filtered_len == 2, "All tab after swap shows the new listing");
    coverflow_shutdown(&s);
}

int main(void) {
    test_items_and_map();
    test_skip_list();
    test_downloads();
    test_wildcard_tab_in_coverflow();
    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("browser_model: all checks passed\n");
    return 0;
}
