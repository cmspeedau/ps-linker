/*
 * PS Linker - Store module (PS3)
 *
 * Homebrew-store browser on the PS3, the twin of psp/store.c: fetches
 * <NAS_BASE>/catalog.json, renders it through the shared Cover Flow
 * module and installs with the app's own resumable downloader.
 *
 * LEGAL: this module is a generic catalog client. It must only be used
 * with catalogs pointing at legally distributable content (homebrew,
 * freeware, demos, or the user's own purchases).
 *
 * Install destinations are derived by store_derive_path_ps3(): pkgs to
 * /dev_hdd0/packages (Package Manager picks them up from the XMB), disc
 * images to PS3ISO/PSPISO for the game manager, licenses to
 * /dev_hdd0/exdata as a staging folder. PSP .PBP homebrew stays in the
 * listing but is refused - a PS3 cannot run it.
 *
 * Rendering runs through the same backend switch as the PSP Store:
 * pixel screens (gfx.c + store_gui.c) by default, -DNASDL_TEXT_UI
 * restores an ANSI Cover Flow frame on the TTY.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "store.h"
#include "ui.h"
#include "gfx.h"
#include "store_gui.h"
#include "http.h"
#include "resume.h"
#include "../ui/catalog.h"
#include "../ui/store_logic.h"
#include "../ui/coverflow.h"

#define SCREEN_ROWS 33
#define SCREEN_COLS 80

#define STORE_MAX_ITEMS 256

#define STORE_STATE_PATH "/dev_hdd0/NASDL/store_state.txt"
#define STORE_MAX_STATE 64
#define STORE_ID_LEN 256

/* ------------------------------------------------------------------ */
/* Catalog model + favourites/recents state                            */
/* ------------------------------------------------------------------ */

static catalog_item_t g_items[STORE_MAX_ITEMS];
static catalog_t g_catalog = { g_items, STORE_MAX_ITEMS, 0, {{0}}, 0 };
static int g_item_count = 0;

static char g_favorites[STORE_MAX_STATE][STORE_ID_LEN];
static int g_favorite_count;
static char g_recent[STORE_MAX_STATE][STORE_ID_LEN];
static int g_recent_count;
static coverflow_item_t g_covers[STORE_MAX_ITEMS];

/* Per-slot views the renderers need next to coverflow_item_t (which has
   no room for description/id). Filled once after parse. */
static const char *g_ids[STORE_MAX_ITEMS];
static const char *g_descs[STORE_MAX_ITEMS];

static int state_has(char values[][STORE_ID_LEN], int count, const char *id)
{
    for (int i = 0; i < count; i++) if (strcmp(values[i], id) == 0) return 1;
    return 0;
}

static void state_add(char values[][STORE_ID_LEN], int *count, const char *id,
                      int newest_first)
{
    if (!id[0]) return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(values[i], id) == 0) {
            if (newest_first && i > 0) {
                char saved[STORE_ID_LEN];
                snprintf(saved, sizeof(saved), "%s", values[i]);
                memmove(values[1], values[0], (size_t)i * sizeof(values[0]));
                snprintf(values[0], STORE_ID_LEN, "%s", saved);
            }
            return;
        }
    }
    int limit = newest_first ? 8 : STORE_MAX_STATE;
    if (*count == limit) (*count)--;
    if (newest_first && *count > 0)
        memmove(values[1], values[0], (size_t)*count * sizeof(values[0]));
    snprintf(values[newest_first ? 0 : *count], STORE_ID_LEN, "%s", id);
    (*count)++;
}

static void load_store_state(void)
{
    g_favorite_count = g_recent_count = 0;
    FILE *f = fopen(STORE_STATE_PATH, "r");
    if (!f) return;
    char line[STORE_ID_LEN + 4];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n > 2 && line[1] == ':') {
            if (line[0] == 'F') state_add(g_favorites, &g_favorite_count, line + 2, 0);
            if (line[0] == 'R') state_add(g_recent, &g_recent_count, line + 2, 1);
        }
    }
    fclose(f);
}

static int save_store_state(void)
{
    FILE *f = fopen(STORE_STATE_PATH, "w");
    if (!f) return -1;
    int ok = 1;
    for (int kind = 0; kind < 2 && ok; kind++) {
        char (*values)[STORE_ID_LEN] = kind ? g_recent : g_favorites;
        int count = kind ? g_recent_count : g_favorite_count;
        for (int i = 0; i < count; i++)
            if (fprintf(f, "%c:%s\n", kind ? 'R' : 'F', values[i]) < 0) ok = 0;
    }
    fclose(f);
    return ok ? 0 : -2;
}

/* The derived roots are single directories under /dev_hdd0; create the
   one this install needs (already existing is fine). */
static void store_prepare_destination(const char *path)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = '\0';
    mkdir(dir, 0777);
}

/* ------------------------------------------------------------------ */
/* Render backends: pixel (default) or ANSI Cover Flow on the TTY      */
/* ------------------------------------------------------------------ */

#ifdef NASDL_TEXT_UI

static void ui_init_backend(void) { }
static void ui_exit_backend(void) { }

static void ui_status(const char *head, const char *body, const char *hint)
{
    printf("\x1b[2J\x1b[1;1H");
    printf("\x1b[48;5;25m\x1b[97m %-78s\x1b[0m\n", head);
    if (body) printf(" %s\n", body);
    if (hint) printf("\x1b[48;5;237m %-78s\x1b[0m", hint);
    fflush(stdout);
}

static void ui_browse(const coverflow_state_t *cf)
{
    printf("\x1b[2J\x1b[1;1H");
    coverflow_render_default(cf);
}

#else /* pixel backend */

static void ui_init_backend(void) { }        /* gfx already up        */
static void ui_exit_backend(void) { }        /* browser owns screen   */

static void ui_status(const char *head, const char *body, const char *hint)
{
    store_gui_status(head, body, hint);
}

static void ui_browse(const coverflow_state_t *cf)
{
    store_gui_draw(cf, g_ids, g_descs);
}

#endif /* NASDL_TEXT_UI */

static char g_inst_title[CATALOG_MAX_STR];
static char g_inst_path[CATALOG_MAX_STR];

static void ui_install(const char *title, const char *path)
{
    snprintf(g_inst_title, sizeof(g_inst_title), "%s", title ? title : "");
    snprintf(g_inst_path, sizeof(g_inst_path), "%s", path ? path : "");
#ifndef NASDL_TEXT_UI
    store_gui_progress(g_inst_title, g_inst_path, -1, 0);
#endif
}

static int g_last_pct = -999;

static void ui_progress(int pct, long total)
{
#ifdef NASDL_TEXT_UI
    char msg[40];
    if (pct >= 0) snprintf(msg, sizeof(msg), "%d%% (%ld KB)", pct, total / 1024);
    else snprintf(msg, sizeof(msg), "%ld KB", total / 1024);
    printf("\x1b[48;5;237m\x1b[38;5;243m%-78s\x1b[0m", msg);
    fflush(stdout);
#else
    /* A present is a full 720p copy + vsync wait: only worth it when
       the percentage actually moved (or before the first byte). */
    if (pct != g_last_pct) {
        g_last_pct = pct;
        store_gui_progress(g_inst_title, g_inst_path, pct, total);
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Pad mapping and the browse loop                                     */
/* ------------------------------------------------------------------ */

static coverflow_key_t store_key(unsigned pressed) {
    if (pressed & PAD_BTN_LEFT)     return COVERFLOW_KEY_LEFT;
    if (pressed & PAD_BTN_RIGHT)    return COVERFLOW_KEY_RIGHT;
    if (pressed & PAD_BTN_UP)       return COVERFLOW_KEY_UP;
    if (pressed & PAD_BTN_DOWN)     return COVERFLOW_KEY_DOWN;
    if (pressed & PAD_BTN_L1)       return COVERFLOW_KEY_SHOULDER_L;
    if (pressed & PAD_BTN_R1)       return COVERFLOW_KEY_SHOULDER_R;
    if (pressed & PAD_BTN_SELECT)   return COVERFLOW_KEY_TAB;
    if (pressed & PAD_BTN_SQUARE)   return COVERFLOW_KEY_FAVORITE;
    if (pressed & PAD_BTN_CROSS)    return COVERFLOW_KEY_ENTER;
    if (pressed & PAD_BTN_CIRCLE)   return COVERFLOW_KEY_BACK;
    return COVERFLOW_KEY_NONE;
}

/* main.c owns the pad; it hands store_run these edge bitmasks. */
/* Wait for any button through the caller's poller. Returns 0 on a
   press, <0 when a system quit came in instead. */
static int wait_any(store_input_cb poll)
{
    unsigned dummy;
    int r;
    do { usleep(16000); r = poll(&dummy); } while (r == -1);
    return r;
}

int store_run(const char *nas_base, store_input_cb poll)
{
    if (!nas_base || !nas_base[0]) nas_base = "http://192.168.1.50:8000";
    ui_init_backend();
    ui_status("PS Linker Store", "Loading catalog...", NULL);

    char url[CATALOG_MAX_STR + 32];
    snprintf(url, sizeof(url), "%s/catalog.json", nas_base);
    char *json = NULL;
    if (http_get_memory(url, &json) < 0 || !json) {
        ui_status("PS Linker Store", "Could not fetch catalog. Check server / network.",
                  "Press any button to return");
        wait_any(poll);
        ui_exit_backend();
        return -1;
    }
    g_item_count = catalog_parse(&g_catalog, json);
    free(json);
    if (g_item_count == 0) {
        ui_status("PS Linker Store", "Catalog is empty.",
                  "Press any button to return");
        wait_any(poll);
        ui_exit_backend();
        return -2;
    }

    load_store_state();
    for (int i = 0; i < g_item_count; i++) {
        snprintf(g_covers[i].title, sizeof(g_covers[i].title), "%s", g_items[i].title);
        snprintf(g_covers[i].category, sizeof(g_covers[i].category), "%s", g_items[i].category);
        snprintf(g_covers[i].version, sizeof(g_covers[i].version), "%s", g_items[i].version);
        snprintf(g_covers[i].size_human, sizeof(g_covers[i].size_human), "%s", g_items[i].size_hint);
        g_covers[i].is_favorite = state_has(g_favorites, g_favorite_count, g_items[i].id);
        g_covers[i].is_recent = state_has(g_recent, g_recent_count, g_items[i].id);
        g_ids[i] = g_items[i].id;
        g_descs[i] = g_items[i].description;
    }

    /* screen_w/h only drive the ANSI renderer's tiles and the grid
       column count; the pixel backend draws its own layout. */
    coverflow_state_t cf;
#ifdef NASDL_TEXT_UI
    if (coverflow_init(&cf, g_covers, (size_t)g_item_count,
                       SCREEN_COLS, SCREEN_ROWS, 1) != 0) {
#else
    if (coverflow_init(&cf, g_covers, (size_t)g_item_count, 120, 27, 1) != 0) {
#endif
        ui_exit_backend();
        return -3;
    }
    if (g_catalog.category_count > 0) {
        const char *categories[STORE_CATEGORY_LIMIT];
        for (int i = 0; i < g_catalog.category_count; i++)
            categories[i] = g_catalog.categories[i];
        coverflow_set_categories(&cf, categories,
                                 (size_t)g_catalog.category_count);
    }

    unsigned pressed;
    while (1) {
        ui_browse(&cf);

        int prc;
        do { usleep(16000); prc = poll(&pressed); }
        while (prc == -1);
        if (prc < 0) break;           /* system quit */

        if (pressed & PAD_BTN_CIRCLE) break;

        if (pressed & PAD_BTN_CROSS) {
            const coverflow_item_t *selected = coverflow_current(&cf);
            if (!selected) continue;
            int idx = (int)(selected - g_covers);
            char dest[CATALOG_MAX_STR];
            int drc = store_derive_path_ps3(g_items[idx].url, g_items[idx].title,
                                            g_items[idx].platform,
                                            dest, sizeof(dest));
            if (drc != 0) {
                ui_status("PS Linker Store",
                          drc == -2 ? "Install path too long"
                                    : "Not an installable file type on PS3",
                          "Press any button to continue");
                wait_any(poll);
                continue;
            }
            store_prepare_destination(dest);
            ui_install(g_items[idx].title, dest);
            char download_url[CATALOG_MAX_STR + 32];
            const char *item_url = g_items[idx].url;
            if (item_url[0] == '/')
                snprintf(download_url, sizeof(download_url), "%s%s", nas_base, item_url);
            else
                snprintf(download_url, sizeof(download_url), "%s", item_url);
            g_last_pct = -999;
            int rc = ps3_resume_download(download_url, dest, ui_progress);
            if (rc == 0) {
                state_add(g_recent, &g_recent_count, g_items[idx].id, 1);
                g_covers[idx].is_recent = 1;
                save_store_state();
                coverflow_recompute_filter(&cf);  /* refresh a live Recent view */
            }
            char msg[300];
            snprintf(msg, sizeof(msg), "%.*s (%d)", 120,
                     g_items[idx].title, rc);
            ui_status(rc == 0 ? "Installed" : "Install failed", msg,
                      "Press any button to continue");
            wait_any(poll);
            continue;
        }

        coverflow_action_t action;
        coverflow_key_t key = store_key(pressed);
        const coverflow_item_t *before_toggle =
            key == COVERFLOW_KEY_FAVORITE ? coverflow_current(&cf) : NULL;
        int toggle_idx = before_toggle ? (int)(before_toggle - g_covers) : -1;
        coverflow_handle_key(&cf, key, &action);
        if (cf.favorite_toggle_pending) {
            if (toggle_idx >= 0 && toggle_idx < g_item_count) {
                int idx = toggle_idx;
                if (g_covers[idx].is_favorite)
                    state_add(g_favorites, &g_favorite_count, g_items[idx].id, 0);
                else {
                    for (int i = 0; i < g_favorite_count; i++)
                        if (strcmp(g_favorites[i], g_items[idx].id) == 0) {
                            memmove(g_favorites[i], g_favorites[i + 1],
                                    (size_t)(g_favorite_count - i - 1) * sizeof(g_favorites[0]));
                            g_favorite_count--;
                            break;
                        }
                }
                save_store_state();
            }
            cf.favorite_toggle_pending = 0;
        }
    }

    coverflow_shutdown(&cf);
    ui_exit_backend();
    return 0;
}
