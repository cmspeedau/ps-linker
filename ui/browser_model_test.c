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

#define ENTRY(name_, cat_, plat_, size_, dir_) \
    { name_, cat_, plat_, size_, dir_ }

static void test_items_and_map(void) {
    bm_entry_t share[] = {
        ENTRY("Games", "Games", "", -1, 1),
        ENTRY("Doom.wad", "Games", "", 3072, 0),
        ENTRY("notes.txt", "", "", 12, 0),
    };
    coverflow_item_t items[8];
    int map[8];
    char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES][COVERFLOW_DYNAMIC_CATEGORY_LEN];
    int tabs_n = 0;

    int n = bm_build(items, 8, share, 3, NULL, 0, NULL, 0, NULL, 0, NULL,
                     0, tabs, &tabs_n, map);
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
        ENTRY("Games", "Games", "", -1, 1),
        ENTRY("PS3", "PS3", "", -1, 1),
        ENTRY("Quake.pkg", "PS3", "PS3", 2500000, 0),
        ENTRY("Licenses", "Licenses", "", -1, 1),
        ENTRY("BLES00001.rap", "Licenses", "", 16, 0),
        ENTRY("README.txt", "", "", 100, 0),
    };
    const char *skip[] = { "Licenses", "PS3" };
    coverflow_item_t items[8];
    int map[8];
    char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES][COVERFLOW_DYNAMIC_CATEGORY_LEN];
    int tabs_n = 0;

    int n = bm_build(items, 8, share, 6, NULL, 0, skip, 2, NULL, 0, NULL,
                     0, tabs, &tabs_n, map);
    check(n == 2, "PS3/Licenses entries hidden on PSP");
    check(map[0] == 0 && map[1] == 5, "map skips over the hidden entries");
    check(tabs_n == 2 && strcmp(tabs[1], "Games") == 0,
          "hidden categories never become tabs");
}

/* The PSP/PS3 consoles pass their own name: a file the machine cannot
 * use is dropped from items and from tab derivation; directories and
 * Downloads rows pass untouched. */
static void test_console_filter(void) {
    bm_entry_t share[] = {
        ENTRY("Games", "Games", "", -1, 1),
        ENTRY("Doom", "Games", "", -1, 1),
        ENTRY("Quake.pkg", "Games", "PS3", 2500000, 0),
        ENTRY("UT2004.iso", "Games", "PSP", 2000000, 0),
        ENTRY("Mixed.iso", "Games", "", 3000000, 0),
        ENTRY("notes.txt", "", "", 10, 0),
    };
    coverflow_item_t items[8];
    int map[8];
    char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES][COVERFLOW_DYNAMIC_CATEGORY_LEN];
    int tabs_n = 0;

    int n = bm_build(items, 8, share, 6, NULL, 0, NULL, 0, NULL, 0, "PSP",
                     0, tabs, &tabs_n, map);
    check(n == 5, "a PSP keeps folders, its own iso and unlabelled files");
    check(map[2] == 3 && map[3] == 4 && map[4] == 5,
          "the .pkg is gone and the map closes the gap");
    check(strcmp(items[2].title, "UT2004.iso") == 0,
          "a PSP-labelled .iso survives on a PSP");
    check(tabs_n == 2 && strcmp(tabs[1], "Games") == 0,
          "tabs stay because the category still has a visible entry");

    n = bm_build(items, 8, share, 6, NULL, 0, NULL, 0, NULL, 0, "PS3",
                 0, tabs, &tabs_n, map);
    check(n == 5, "a PS3 keeps the .pkg and drops the PSP-labelled iso");
    check(map[2] == 2 && map[3] == 4, "the foreign .iso slot is gone, map closes");

    /* Downloads rows are local facts, not offers: never filtered. */
    bm_entry_t dl[] = { ENTRY("Quake.pkg", "", "PS3", 2500000, 0) };
    n = bm_build(items, 8, share, 6, dl, 1, NULL, 0, NULL, 0, "PSP",
                 1, tabs, &tabs_n, map);
    check(n == 1 && map[0] == 0, "Downloads rows are never console-filtered");
    check(tabs_n == 3 && strcmp(tabs[2], "Downloads") == 0,
          "the Downloads tab exists whatever the console filter");
}

/* A hidden tab keeps its entries (they answer to the All wildcard); only
   the rail loses the tab — the PS3 client's shape for its own category. */
static void test_hide_tabs(void) {
    bm_entry_t share[] = {
        ENTRY("Games", "Games", "", -1, 1),
        ENTRY("PS3", "PS3", "", -1, 1),
        ENTRY("Quake.pkg", "PS3", "PS3", 2500000, 0),
    };
    const char *hide[] = { "PS3" };
    coverflow_item_t items[8];
    int map[8];
    char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES][COVERFLOW_DYNAMIC_CATEGORY_LEN];
    int tabs_n = 0;

    int n = bm_build(items, 8, share, 3, NULL, 0, NULL, 0, hide, 1, "PS3",
                     0, tabs, &tabs_n, map);
    check(n == 3, "hidden-tab categories keep their entries");
    check(tabs_n == 2 && strcmp(tabs[1], "Games") == 0, "\"PS3\" gets no tab");
    check(strcmp(items[2].category, "PS3") == 0,
          "the .pkg still carries its category, so All shows it");

    /* A category whose files are all console-filtered away keeps its tab
       while its folder is still visible — the folder is the tab's entry. */
    n = bm_build(items, 8, share, 3, NULL, 0, NULL, 0, NULL, 0, "PSP",
                 0, tabs, &tabs_n, map);
    check(n == 2, "on a PSP both folders survive, the .pkg does not");
    check(tabs_n == 3 && strcmp(tabs[1], "Games") == 0 &&
          strcmp(tabs[2], "PS3") == 0,
          "the PS3 folder keeps its tab while it is visible");
}

static void test_downloads(void) {
    bm_entry_t share[] = {
        ENTRY("Games", "Games", "", -1, 1),
        ENTRY("Utilities", "Utilities", "", -1, 1),
    };
    bm_entry_t dl[] = {
        ENTRY("Doom.wad", "", "", 3072, 0),
        ENTRY(".hidden.part", "", "", 512, 0),
    };
    coverflow_item_t items[8];
    int map[8];
    char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES][COVERFLOW_DYNAMIC_CATEGORY_LEN];
    int tabs_n = 0;

    int n = bm_build(items, 8, share, 2, dl, 2, NULL, 0, NULL, 0, NULL,
                     0, tabs, &tabs_n, map);
    check(n == 2, "share view shows share entries");
    check(tabs_n == 4 && strcmp(tabs[0], "") == 0 &&
          strcmp(tabs[1], "Games") == 0 && strcmp(tabs[2], "Utilities") == 0 &&
          strcmp(tabs[3], "Downloads") == 0,
          "Downloads is the last tab and the share tabs keep their order");

    n = bm_build(items, 8, share, 2, dl, 2, NULL, 0, NULL, 0, NULL,
                 1, tabs, &tabs_n, map);
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

/* The PS3 shape end-to-end: hide={PS3} + console=PS3 over a mixed share.
   Everything visible under All, no PS3 tab, no PSP files. */
static void test_ps3_shape(void) {
    bm_entry_t share[] = {
        ENTRY("Games", "Games", "", -1, 1),
        ENTRY("Doom.iso", "Games", "PS3", 4000000, 0),
        ENTRY("Homebrew", "Homebrew", "", -1, 1),
        ENTRY("UT2004.iso", "Homebrew", "PSP", 2000000, 0),
        ENTRY("PS3", "PS3", "", -1, 1),
        ENTRY("Quake.pkg", "PS3", "PS3", 2500000, 0),
        ENTRY("Utilities", "Utilities", "", -1, 1),
    };
    const char *hide[] = { "PS3" };
    coverflow_item_t items[8];
    int map[8];
    char tabs[COVERFLOW_MAX_DYNAMIC_CATEGORIES][COVERFLOW_DYNAMIC_CATEGORY_LEN];
    int tabs_n = 0;

    int n = bm_build(items, 8, share, 7, NULL, 0, NULL, 0, hide, 1, "PS3",
                     0, tabs, &tabs_n, map);
    check(n == 6, "the PSP iso is the only casualty");
    check(tabs_n == 4 && strcmp(tabs[1], "Games") == 0 &&
          strcmp(tabs[2], "Homebrew") == 0 && strcmp(tabs[3], "Utilities") == 0,
          "rail is All + live categories, no PS3 tab");

    /* Homebrew keeps its tab: its folder itself is still visible. */
    static coverflow_item_t cf_items[6];
    for (int i = 0; i < n; i++) {
        strcpy(cf_items[i].title, items[i].title);
        strcpy(cf_items[i].category, items[i].category);
    }
    coverflow_state_t s;
    check(coverflow_init(&s, cf_items, (size_t)n, 60, 28, 0) == 0, "init for shape test");
    const char *cats[4];
    for (int i = 0; i < tabs_n; i++) cats[i] = tabs[i];
    coverflow_set_categories(&s, cats, (size_t)tabs_n);
    check(s.filtered_len == 6, "All shows every kept entry incl. the PS3 folder");
    coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_R, NULL);
    coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_R, NULL);
    check(s.category == 2 && s.filtered_len == 1, "Homebrew tab holds just the folder");
    coverflow_shutdown(&s);
}

int main(void) {
    test_items_and_map();
    test_skip_list();
    test_console_filter();
    test_hide_tabs();
    test_downloads();
    test_wildcard_tab_in_coverflow();
    test_ps3_shape();
    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("browser_model: all checks passed\n");
    return 0;
}
