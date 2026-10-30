/*
 * coverflow_test.c — host test for the shared Cover Flow module.
 *
 * No device headers, so the state machine and the renderer can be checked on
 * the build host before either app is wired to it.
 *
 *   gcc -std=c11 -Wall -Wextra -o /tmp/coverflow_test \
 *       coverflow.c coverflow_test.c && /tmp/coverflow_test
 *
 * Pass --frames to also print rendered frames (flow, grid, empty category)
 * for eyeballing the layout; the assertions run either way.
 */

#include "coverflow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* Writable on purpose: the favorite toggle updates items in place, so a const
   table here would live in .rodata and fault on the first toggle. */
static coverflow_item_t kItems[] = {
    {"Lumines",                "PSP", "120 MB", "v1.0",  1, 0, 0},
    {"God of War: Chains",     "PSP", "1.4 GB", "v1.05", 0, 1, 0},
    {"Wipeout Pure",           "PSP", "480 MB", "",      0, 0, 0},
    {"Patapon",                "PSP", "210 MB", "",      1, 0, 0},
    {"Crisis Core",            "PSP", "1.6 GB", "v2.0",  0, 0, 0},
    {"LittleBigPlanet PSP",    "PSP", "1.1 GB", "",      0, 1, 0},
    {"Final Fantasy Tactics",  "PSP", "320 MB", "",      0, 0, 0},
    {"Ratchet & Clank: SizeM", "PS3", "12 GB",  "v1.01", 1, 0, 0},
    {"Demon's Souls",          "PS3", "9.2 GB", "v1.03", 1, 1, 1},
    {"Uncharted 1",            "PS3", "14 GB",  "",      0, 0, 0},
    {"Journey",                "PS3", "2.1 GB", "",      0, 0, 0},
    {"Flower",                 "PS3", "600 MB", "",      0, 0, 0},
};
static const size_t kCount = sizeof(kItems) / sizeof(kItems[0]);

static int g_fails;
static int g_checks;

static void check(int cond, const char *what) {
    g_checks++;
    if (!cond) {
        /* stderr: test_render_no_crash() redirects stdout while it sweeps. */
        fprintf(stderr, "FAIL: %s\n", what);
        fflush(stderr);
        g_fails++;
    }
}

#define CF_LINE_TEST 512

/* Drop ANSI escape sequences so a rendered row can be measured and searched. */
static void strip_ansi(char *dst, const char *src, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; src[i] != '\0' && n + 1 < cap; i++) {
        if (src[i] == '\x1b') { while (src[i] != '\0' && src[i] != 'm') i++; continue; }
        dst[n++] = src[i];
    }
    dst[n] = '\0';
}

static int g_saved_out = -1;

/* Rendering tests throw their frames away: the point is that nothing crashes
   or overruns, not what the bytes look like. Failures go to stderr (see
   check()), so they are never silenced by this. */
static void quiet_stdout(void) {
    g_saved_out = dup(STDOUT_FILENO);
    const int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
}

static void loud_stdout(void) {
    fflush(stdout);
    if (g_saved_out >= 0) { dup2(g_saved_out, STDOUT_FILENO); close(g_saved_out); }
    g_saved_out = -1;
}

static coverflow_state_t new_state(int w, int h, int colors) {
    coverflow_state_t s;
    if (coverflow_init(&s, kItems, kCount, w, h, colors) != 0) {
        fprintf(stderr, "FAIL: coverflow_init\n");
        exit(1);
    }
    return s;
}

static size_t count_of(coverflow_state_t *s, coverflow_category_t c) {
    s->category = c;
    coverflow_recompute_filter(s);
    return s->filtered_len;
}

static void test_filters(void) {
    coverflow_state_t s = new_state(80, 28, 0);

    check(count_of(&s, COVERFLOW_CAT_ALL)       == 12, "All shows every item");
    check(count_of(&s, COVERFLOW_CAT_PSP)       == 7,  "PSP category");
    check(count_of(&s, COVERFLOW_CAT_PS3)       == 5,  "PS3 category");
    check(count_of(&s, COVERFLOW_CAT_RECENT)    == 3,  "Recently Added");
    check(count_of(&s, COVERFLOW_CAT_FAVORITES) == 4,  "Favorites");
    check(count_of(&s, COVERFLOW_CAT_DOWNLOADS) == 1,  "Downloads");
    check(count_of(&s, COVERFLOW_CAT_SETTINGS)  == 0,  "Settings has no items");

    /* Filtered slots must be real item indices, in increasing order. */
    count_of(&s, COVERFLOW_CAT_ALL);
    for (size_t i = 0; i < s.filtered_len; i++) {
        check(s.filtered[i] < s.item_count, "filtered index in range");
        if (i) check(s.filtered[i] > s.filtered[i - 1], "filtered indices ascend");
    }
    coverflow_shutdown(&s);
}

static void test_navigation(void) {
    coverflow_state_t s = new_state(60, 34, 0);
    coverflow_action_t act;

    check(coverflow_handle_key(&s, COVERFLOW_KEY_LEFT, &act) == 0,
          "LEFT at the first cover does not move");
    check(s.index == 0, "index stays 0");

    for (int i = 0; i < 3; i++)
        coverflow_handle_key(&s, COVERFLOW_KEY_RIGHT, &act);
    check(s.index == 3, "RIGHT advances one cover per press");

    for (int i = 0; i < 50; i++)
        coverflow_handle_key(&s, COVERFLOW_KEY_RIGHT, &act);
    check(s.index == s.filtered_len - 1, "RIGHT clamps at the last cover");

    coverflow_handle_key(&s, COVERFLOW_KEY_UP, &act);
    check(s.index == s.filtered_len - 1 - 5, "UP jumps back by 5");

    coverflow_handle_key(&s, COVERFLOW_KEY_DOWN, &act);
    check(s.index == s.filtered_len - 1, "DOWN clamps at the last cover");

    s.category = COVERFLOW_CAT_ALL; s.index = 0;
    coverflow_handle_key(&s, COVERFLOW_KEY_UP, &act);
    check(s.index == 0, "UP at the start clamps to 0, does not wrap");

    coverflow_shutdown(&s);
}

static void test_categories(void) {
    coverflow_state_t s = new_state(80, 28, 0);
    coverflow_action_t act;

    check(coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_L, &act) == 0,
          "SHOULDER_L cannot go before All Games");
    for (int i = 0; i < 20; i++)
        coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_R, &act);
    check(s.category == COVERFLOW_CAT_SETTINGS, "SHOULDER_R clamps at Settings");
    check(s.filtered_len == 0, "Settings category is empty");
    check(coverflow_current(&s) == NULL, "no current item in an empty category");
    check(coverflow_handle_key(&s, COVERFLOW_KEY_RIGHT, &act) == 0,
          "RIGHT on an empty category is a no-op");

    coverflow_handle_key(&s, COVERFLOW_KEY_TAB, &act);
    check(s.view == COVERFLOW_VIEW_GRID, "TAB switches to grid");
    coverflow_handle_key(&s, COVERFLOW_KEY_TAB, &act);
    check(s.view == COVERFLOW_VIEW_FLOW, "TAB switches back to flow");

    coverflow_shutdown(&s);
}

static void test_dynamic_categories(void) {
    coverflow_state_t s = new_state(60, 34, 0);
    const char *cats[] = {"Games", "Homebrew", "Games", "Utilities"};
    coverflow_action_t act;
    char row[CF_LINE_TEST], plain[CF_LINE_TEST];

    coverflow_set_categories(&s, cats, sizeof cats / sizeof cats[0]);
    check(s.dynamic_category_count == 3, "dynamic categories are deduplicated");
    check(s.filtered_len == 0, "catalog categories filter exact folder names");
    /* Reuse a private item set whose categories match the catalog. */
    coverflow_shutdown(&s);
    coverflow_item_t items[] = {
        {"One", "Games", "", "", 0, 0, 0},
        {"Two", "Homebrew", "", "", 0, 0, 0},
        {"Three", "Utilities", "", "", 0, 0, 0},
        {"Four", "Other", "", "", 0, 0, 0},
    };
    check(coverflow_init(&s, items, 4, 60, 34, 0) == 0,
          "dynamic category test state initializes");
    coverflow_set_categories(&s, cats, sizeof cats / sizeof cats[0]);
    check(s.filtered_len == 1 && coverflow_current(&s) == &items[0],
          "first category shows only its matching catalog item");
    coverflow_menu_row(&s, row, sizeof row);
    strip_ansi(plain, row, sizeof plain);
    check(strstr(plain, "[Games]") != NULL, "dynamic category label is displayed");
    check(coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_R, &act) == 1 &&
          s.category == 1 && s.filtered_len == 1 &&
          coverflow_current(&s) == &items[1], "R switches to next catalog category");
    check(coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_L, &act) == 1 &&
          s.category == 0, "L switches back to previous catalog category");
    coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_L, &act);
    check(s.category == 0, "L clamps at first catalog category");
    coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_R, &act);
    coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_R, &act);
    check(s.category == 2 && s.filtered_len == 1 &&
          coverflow_current(&s) == &items[2], "last dynamic category filters correctly");
    check(coverflow_handle_key(&s, COVERFLOW_KEY_SHOULDER_R, &act) == 0,
          "R clamps at last catalog category");
    coverflow_set_categories(&s, NULL, 0);
    check(s.dynamic_category_count == 0 && s.category == COVERFLOW_CAT_ALL &&
          s.filtered_len == 4, "empty category list falls back to shared menu");
    coverflow_shutdown(&s);
}

static void test_actions(void) {
    coverflow_state_t s = new_state(80, 28, 0);
    coverflow_action_t act;

    coverflow_handle_key(&s, COVERFLOW_KEY_ENTER, &act);
    check(act == COVERFLOW_ACTION_SELECT, "ENTER on a cover selects");
    check(coverflow_current(&s) == &kItems[0], "current item is the first cover");

    coverflow_handle_key(&s, COVERFLOW_KEY_BACK, &act);
    check(act == COVERFLOW_ACTION_BACK, "BACK returns BACK");

    s.category = COVERFLOW_CAT_SETTINGS;
    coverflow_recompute_filter(&s);
    coverflow_handle_key(&s, COVERFLOW_KEY_ENTER, &act);
    check(act == COVERFLOW_ACTION_SETTINGS, "ENTER on Settings reports SETTINGS");

    coverflow_handle_key(&s, COVERFLOW_KEY_NONE, &act);
    check(act == COVERFLOW_ACTION_NONE, "unknown key reports no action");

    coverflow_shutdown(&s);
}

static void test_favorite(void) {
    coverflow_state_t s = new_state(80, 28, 0);
    coverflow_action_t act;

    /* Item 2 ("Wipeout Pure") starts unloved: move onto it and toggle. */
    coverflow_handle_key(&s, COVERFLOW_KEY_RIGHT, &act);
    coverflow_handle_key(&s, COVERFLOW_KEY_RIGHT, &act);
    check(coverflow_current(&s) == &kItems[2], "selection is Wipeout Pure");
    check(kItems[2].is_favorite == 0, "starts unfavourited");

    check(coverflow_handle_key(&s, COVERFLOW_KEY_FAVORITE, &act) == 1,
          "FAVORITE is handled");
    check(kItems[2].is_favorite == 1, "toggle writes caller storage");
    check(s.favorite_toggle_pending == 1, "pending flag set for the caller");

    check(count_of(&s, COVERFLOW_CAT_FAVORITES) == 5, "Favorites grew to 5");

    /* Toggling while viewing Favorites must drop the item from the list. */
    s.category = COVERFLOW_CAT_FAVORITES;
    coverflow_recompute_filter(&s);
    size_t slot = 0;
    for (size_t i = 0; i < s.filtered_len; i++)
        if (s.filtered[i] == 2) slot = i;
    s.index = slot;
    coverflow_handle_key(&s, COVERFLOW_KEY_FAVORITE, &act);
    check(kItems[2].is_favorite == 0, "second toggle clears the flag");
    check(s.filtered_len == 4, "list shrinks inside the Favorites filter");
    check(s.index < s.filtered_len, "selection stays inside the new list");

    coverflow_shutdown(&s);
}

static void test_selection_survives_refilter(void) {
    coverflow_state_t s = new_state(80, 28, 0);
    coverflow_action_t act;

    /* `index` is a slot in `filtered`, not an item id, so the item has to be
       carried across a re-filter. Selecting "Uncharted 1" (item 9) inside the
       5-item PS3 list puts it in slot 2; switching to All Games must follow
       item 9, not slot 2. */
    s.category = COVERFLOW_CAT_PS3;
    coverflow_recompute_filter(&s);
    check(s.filtered_len == 5, "PS3 list has 5 items");
    coverflow_handle_key(&s, COVERFLOW_KEY_RIGHT, &act);
    coverflow_handle_key(&s, COVERFLOW_KEY_RIGHT, &act);
    check(s.index == 2, "slot 2 inside the PS3 list");
    check(coverflow_current(&s) == &kItems[9], "Uncharted 1 selected");

    s.category = COVERFLOW_CAT_ALL;
    coverflow_recompute_filter(&s);
    check(s.index == 9, "same item keeps its cover in All Games");
    check(coverflow_current(&s) == &kItems[9], "still Uncharted 1");

    /* Documented fallback: when the selected item is not in the new category,
       the list starts at its first entry. */
    s.category = COVERFLOW_CAT_DOWNLOADS;
    coverflow_recompute_filter(&s);
    check(s.filtered_len == 1, "one download");
    check(s.index == 0, "falls back to the first entry");
    check(coverflow_current(&s) == &kItems[8], "Demon's Souls is the download");

    coverflow_shutdown(&s);
}

static void test_menu_row(void) {
    /* Two properties:
         1. the row never spills past the screen, at any width;
         2. the selected category is always on screen. The delivered version
            dropped "Settings" entirely at 80 columns (the 7 labels need 91),
            so the user could reach a category they could never see.
       The current label plus the two markers needs 22 columns for the longest
       label ("Recently Added"); below that it is truncated rather than shown
       whole, so property 2 is asserted from that width up. */
    static const int widths[] = { 1, 8, 20, 24, 40, 60, 80, 120 };
    const int visibility_min = 22;

    for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
        coverflow_state_t s = new_state(widths[wi], 28, 0);
        for (int c = 0; c < COVERFLOW_CAT_COUNT; c++) {
            s.category = (coverflow_category_t)c;
            coverflow_recompute_filter(&s);

            const char *label = coverflow_category_label((coverflow_category_t)c);
            char bracketed[64];
            char row[CF_LINE_TEST];
            char plain[CF_LINE_TEST];
            char why[128];
            snprintf(bracketed, sizeof bracketed, "[%s]", label);
            coverflow_menu_row(&s, row, sizeof row);
            strip_ansi(plain, row, sizeof plain);

            snprintf(why, sizeof why, "menu row fits %d columns (category '%s')",
                     widths[wi], label);
            check((int)strlen(plain) <= widths[wi], why);

            if (widths[wi] >= visibility_min) {
                snprintf(why, sizeof why,
                         "category '%s' visible at %d columns", label, widths[wi]);
                /* Assert on the brackets too: a frame that prints the label but
                   forgets to close "[..]" would pass a bare-label search. */
                check(strstr(plain, bracketed) != NULL, why);

                /* The full menu needs 91 columns. Whenever the row cannot show
                   all seven labels, it must mark what is off-screen. */
                if (widths[wi] < 91) {
                    int shown = 0;
                    for (int k = 0; k < COVERFLOW_CAT_COUNT; k++)
                        if (strstr(plain, coverflow_category_label(
                                (coverflow_category_t)k)))
                            shown++;
                    snprintf(why, sizeof why,
                             "hidden categories marked at %d columns (category '%s')",
                             widths[wi], label);
                    check(shown == COVERFLOW_CAT_COUNT ||
                          strchr(plain, '<') != NULL ||
                          strchr(plain, '>') != NULL, why);
                }
            }
        }
        coverflow_shutdown(&s);
    }
}

static void test_render_no_crash(void) {
    /* Every screen size we might be pointed at, in both views and both colour
       modes, including sizes far outside the PSP/PS3 defaults. Output goes to
       /dev/null; the point is that no frame crashes or overruns a buffer. */
    static const int ws[] = { 1, 8, 20, 40, 60, 80, 120, 200, 400 };
    static const int hs[] = { 1, 10, 28, 34, 40 };

    quiet_stdout();

    for (size_t wi = 0; wi < sizeof ws / sizeof ws[0]; wi++) {
        for (size_t hi = 0; hi < sizeof hs / sizeof hs[0]; hi++) {
            for (int colors = 0; colors <= 1; colors++) {
                coverflow_state_t s = new_state(ws[wi], hs[hi], colors);
                coverflow_render_default(&s);            /* flow */
                s.view = COVERFLOW_VIEW_GRID;
                coverflow_render_default(&s);            /* grid */
                s.category = COVERFLOW_CAT_SETTINGS;     /* empty */
                coverflow_recompute_filter(&s);
                coverflow_render_default(&s);
                s.category = COVERFLOW_CAT_FAVORITES;
                coverflow_recompute_filter(&s);
                s.index = s.filtered_len - 1;            /* last cover */
                coverflow_render_default(&s);
                coverflow_shutdown(&s);
            }
        }
    }

    loud_stdout();

    check(1, "render sweep completed without crashing");
}

static void test_bogus_inputs(void) {
    /* A caller with a stale or uninitialised category must not be able to walk
       off the label table, and NULL handling must stay quiet. */
    coverflow_state_t s = new_state(60, 28, 0);
    char row[CF_LINE_TEST];
    char plain[CF_LINE_TEST];

    s.category = (coverflow_category_t)99;
    coverflow_recompute_filter(&s);
    coverflow_menu_row(&s, row, sizeof row);
    strip_ansi(plain, row, sizeof plain);
    check((int)strlen(plain) <= 60, "bogus category still renders a sane row");
    quiet_stdout();
    coverflow_render_default(&s);
    loud_stdout();

    check(coverflow_category_label((coverflow_category_t)-1) != NULL,
          "label lookup survives an out-of-range category");

    /* Small destination buffers: clip, never write past the caller's array. */
    char tiny[4];
    check(coverflow_menu_row(&s, tiny, sizeof tiny) < (size_t)sizeof tiny,
          "menu row clips to a 4-byte buffer");
    check(tiny[3] == '\0', "4-byte buffer ends with its own NUL");
    check(coverflow_menu_row(&s, tiny, 1) == 0, "1-byte buffer writes nothing");
    check(tiny[0] == '\0', "1-byte buffer only gets the NUL");

    check(coverflow_init(NULL, kItems, kCount, 60, 28, 0) == -1,
          "init rejects a NULL state");
    coverflow_shutdown(NULL);
    check(1, "shutdown(NULL) is a no-op");

    coverflow_shutdown(&s);

    /* Zero items: init succeeds, nothing is selectable, rendering is safe. */
    coverflow_state_t empty;
    check(coverflow_init(&empty, kItems, 0, 60, 28, 0) == 0, "zero items inits");
    check(empty.filtered_len == 0, "zero items filter empty");
    coverflow_action_t act;
    check(coverflow_handle_key(&empty, COVERFLOW_KEY_RIGHT, &act) == 0,
          "no movement with no items");
    quiet_stdout();
    coverflow_render_default(&empty);
    loud_stdout();
    coverflow_shutdown(&empty);
}

int main(int argc, char **argv) {
    const int frames = argc > 1 && strcmp(argv[1], "--frames") == 0;

    test_filters();
    test_navigation();
    test_categories();
    test_dynamic_categories();
    test_actions();
    test_menu_row();
    test_bogus_inputs();
    test_favorite();
    test_selection_survives_refilter();
    test_render_no_crash();

    if (frames) {
        coverflow_state_t s = new_state(80, 28, 1);
        coverflow_action_t act;
        coverflow_handle_key(&s, COVERFLOW_KEY_RIGHT, &act);
        coverflow_handle_key(&s, COVERFLOW_KEY_RIGHT, &act);
        coverflow_handle_key(&s, COVERFLOW_KEY_FAVORITE, &act);
        printf("--- flow ---\n");
        coverflow_render_default(&s);
        printf("\n--- grid ---\n");
        coverflow_handle_key(&s, COVERFLOW_KEY_TAB, &act);
        coverflow_render_default(&s);
        coverflow_shutdown(&s);
    }

    if (g_fails) {
        printf("%d assertion(s) failed\n", g_fails);
        return 1;
    }
    printf("all %d coverflow assertions passed\n", g_checks);
    return 0;
}
