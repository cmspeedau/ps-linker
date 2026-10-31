/*
 * PS Linker - Store module (PSP)
 *
 * Implements a homebrew-store browser on top of the existing PS Linker
 * infrastructure. Fetches <NAS_BASE>/catalog.json and renders it through
 * the shared Cover Flow module. Download destinations are derived locally.
 *
 * LEGAL: this module is a generic catalog client. It must only be used
 * with catalogs pointing at legally distributable content (homebrew,
 * freeware, demos, or the user's own purchases).
 *
 * Rendering is the pixel view (gui.c/store_gui.c, sceGu); the whole app
 * went pixel-only with the Cover Flow browser redesign. The ui_*
 * dispatch functions draw the same ui/coverflow.c state the browser
 * drives.
 *
 * Integrates with psp/main.c — see store.h for the one-line hookup.
 */

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <psphttp.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "store.h"
#include "catalog.h"
#include "gui.h"
#include "store_gui.h"
#include "../ui/coverflow.h"
#include "resume.h"

#define printf pspDebugScreenPrintf

#define SCREEN_ROWS 33
/* The debug screen is the 480x272 framebuffer on an 8x8 cell: 60 columns.
   Cover Flow sizes its tiles from this (side 14 + 2 + centre 20 + 2 + 14). */
#define SCREEN_COLS 60

/* Store limits (kept modest for PSP RAM; catalog is parsed in place) */
#define STORE_MAX_ITEMS 64
#define STORE_MAX_STR   256

/* ------------------------------------------------------------------ */
/* Catalog model                                                       */
/* ------------------------------------------------------------------ */

/* The parser itself lives in ui/catalog.c (host-tested in ui/) so the
   PS3 Store reads the very same catalog shapes. */

typedef catalog_item_t StoreItem;

static StoreItem g_items[STORE_MAX_ITEMS];
static catalog_t g_catalog = { g_items, STORE_MAX_ITEMS, 0, {{0}}, 0 };
static int g_item_count = 0;

static char g_categories[STORE_CATEGORY_LIMIT][STORE_CATEGORY_LEN];
static int g_category_count = 0;

static void parse_catalog(const char *json) {
    catalog_parse(&g_catalog, json);
    g_item_count = g_catalog.count;
    g_category_count = g_catalog.category_count;
    memcpy(g_categories, g_catalog.categories, sizeof(g_categories));
}

/* ------------------------------------------------------------------ */
/* HTTP: fetch catalog + download installer (same stack as main.c)     */
/* ------------------------------------------------------------------ */

static char *store_fetch_to_memory(const char *url, int *out_len) {
    int tpl = sceHttpCreateTemplate("PSLinkerStore", 1, 1);
    if (tpl < 0) return NULL;

    char *result = NULL;
    int conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    if (conn >= 0) {
        int req = sceHttpCreateRequestWithURL(conn, PSP_HTTP_METHOD_GET, url, 0);
        if (req >= 0 && sceHttpSendRequest(req, NULL, 0) >= 0) {
            SceULong64 clen64 = 0;
            sceHttpGetContentLength(req, &clen64);
            int clen = (clen64 > 0 && clen64 <= 1024 * 1024)
                ? (int)clen64 : 128 * 1024;   /* catalog is small */

            char *buf = (char *)malloc((size_t)clen + 1);
            if (buf) {
                int total = 0, n;
                while (total < clen &&
                       (n = sceHttpReadData(req, buf + total, clen - total)) > 0)
                    total += n;
                buf[total] = '\0';
                result = buf;
                if (out_len) *out_len = total;
            }
        }
        if (req >= 0) sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
    }
    sceHttpDeleteTemplate(tpl);
    return result;
}

/* ------------------------------------------------------------------ */
/* Store UI                                                            */
/* ------------------------------------------------------------------ */

#define STORE_STATE_PATH "ms0:/PSP/GAME/NASDL/store_state.txt"
#define STORE_MAX_STATE 64
#define STORE_ID_LEN 256

static char g_favorites[STORE_MAX_STATE][STORE_ID_LEN];
static int g_favorite_count;
static char g_recent[STORE_MAX_STATE][STORE_ID_LEN];
static int g_recent_count;
static coverflow_item_t g_covers[STORE_MAX_ITEMS];

/* Per-slot views the renderers need next to coverflow_item_t (which has
   no room for description/id). Filled once after parse. */
static const char *g_ids[STORE_MAX_ITEMS];
static const char *g_descs[STORE_MAX_ITEMS];

static int state_has(char values[][STORE_ID_LEN], int count, const char *id) {
    for (int i = 0; i < count; i++) if (strcmp(values[i], id) == 0) return 1;
    return 0;
}

static void state_add(char values[][STORE_ID_LEN], int *count, const char *id,
                      int newest_first) {
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

static void load_store_state(void) {
    g_favorite_count = g_recent_count = 0;
    SceUID fd = sceIoOpen(STORE_STATE_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    char line[STORE_ID_LEN + 4];
    int used = 0;
    char ch;
    while (sceIoRead(fd, &ch, 1) == 1) {
        if (ch == '\n' || used + 1 >= (int)sizeof(line)) {
            line[used] = '\0';
            if (used > 2 && line[1] == ':') {
                if (line[0] == 'F') state_add(g_favorites, &g_favorite_count, line + 2, 0);
                if (line[0] == 'R') state_add(g_recent, &g_recent_count, line + 2, 1);
            }
            used = 0;
        } else if (ch != '\r') line[used++] = ch;
    }
    if (used > 2 && line[1] == ':') {
        line[used] = '\0';
        if (line[0] == 'F') state_add(g_favorites, &g_favorite_count, line + 2, 0);
        if (line[0] == 'R') state_add(g_recent, &g_recent_count, line + 2, 1);
    }
    sceIoClose(fd);
}

static int save_store_state(void) {
    SceUID fd = sceIoOpen(STORE_STATE_PATH,
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;
    int ok = 1;
    for (int kind = 0; kind < 2 && ok; kind++) {
        char (*values)[STORE_ID_LEN] = kind ? g_recent : g_favorites;
        int count = kind ? g_recent_count : g_favorite_count;
        for (int i = 0; i < count; i++) {
            char line[STORE_ID_LEN + 4];
            int n = snprintf(line, sizeof(line), "%c:%s\n", kind ? 'R' : 'F', values[i]);
            if (sceIoWrite(fd, line, n) != n) { ok = 0; break; }
        }
    }
    sceIoClose(fd);
    return ok ? 0 : -1;
}

static int store_prepare_destination(const char *path) {
    char dir[STORE_MAX_STR];
    if (strlen(path) >= sizeof(dir)) return -1;
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return -1;
    *slash = '\0';
    /* PSP mkdir is not recursive; these are the only derived path roots. */
    if (strncmp(dir, "ms0:/PSP/GAME/", 14) == 0) {
        sceIoMkdir("ms0:/PSP/GAME", 0777);
        sceIoMkdir(dir, 0777);
    } else if (strcmp(dir, "ms0:/ISO") == 0) {
        sceIoMkdir(dir, 0777);
    }
    return 0;
}

/* ---- Render backends ------------------------------------------------
 * The screens below speak ui_*; exactly one backend compiles. The pixel
 * backend ends by handing the framebuffer back to the debug screen, so
 * browser() in main.c keeps drawing as before when the Store closes.  */

#ifdef NASDL_TEXT_UI

/* Shared Media Go palette with main.c */
#define COL_BG        0xFF1D1D1D
#define COL_HEADER    0xFFCC7A00
#define COL_TEXT      0xFFCCCCCC
#define COL_TEXT_DIM  0xFF808080
#define COL_STATUS    0xFF333333

typedef struct { u32 bg, fg; } Style;

static const Style STYLE_HEADER = { COL_HEADER,   0xFF000000 };
static const Style STYLE_ROW    = { COL_BG,       COL_TEXT };
static const Style STYLE_STATUS = { COL_STATUS,   COL_TEXT };

static void store_set_style(const Style *s) {
    pspDebugScreenSetBackColor(s->bg);
    pspDebugScreenSetTextColor(s->fg);
}

/* Blank field fill, straight to the debug screen: putchar() would write to
   stdout, which on the PSP never reaches the display. */
static void store_pad(int n) {
    char buf[SCREEN_COLS + 1];
    int i;
    for (i = 0; i < n && i < (int)sizeof(buf) - 1; i++) buf[i] = ' ';
    pspDebugScreenPrintData(buf, i);
}

static void store_field(int col, int row, int width, const Style *s, const char *text) {
    pspDebugScreenSetXY(col, row);
    store_set_style(s);
    int len = (int)strlen(text);
    printf("%s", text);
    if (len < width) store_pad(width - len);
}

static void ui_init(void) { }
static void ui_exit(void) { }

static void ui_status(const char *head, const char *body, const char *hint) {
    pspDebugScreenClear();
    store_field(0, 0, SCREEN_COLS, &STYLE_HEADER, head);
    if (body) store_field(1, 3, SCREEN_COLS - 2, &STYLE_ROW, body);
    if (hint) store_field(0, SCREEN_ROWS - 2, SCREEN_COLS, &STYLE_STATUS, hint);
}

static void ui_browse(const coverflow_state_t *cf) {
    pspDebugScreenClear();
    store_set_style(&STYLE_ROW);   /* frame text on the dark background */
    coverflow_render_default(cf);
    const coverflow_item_t *selected = coverflow_current(cf);
    if (selected) {
        int idx = (int)(selected - g_covers);
        char line[SCREEN_COLS + 1];
        snprintf(line, sizeof(line), "%.*s", SCREEN_COLS, g_descs[idx]);
        store_field(0, SCREEN_ROWS - 3, SCREEN_COLS, &STYLE_ROW, line);
        snprintf(line, sizeof(line), "X Install  Sq Fav  O Back");
        store_field(0, SCREEN_ROWS - 2, SCREEN_COLS, &STYLE_STATUS, line);
    }
}

static void ui_install(const char *title, const char *path) {
    (void)title; (void)path;
    pspDebugScreenClear();
    store_field(0, 0, SCREEN_COLS, &STYLE_HEADER, " Installing...");
}

static void ui_progress(int pct, long total) {
    char msg[40];
    if (pct >= 0) snprintf(msg, sizeof(msg), "%d%% (%ld KB)", pct, total / 1024);
    else snprintf(msg, sizeof(msg), "%ld KB", total / 1024);
    store_field(0, SCREEN_ROWS - 2, SCREEN_COLS, &STYLE_STATUS, msg);
}

#else /* pixel backend (default) */

static char g_inst_title[STORE_MAX_STR];
static char g_inst_path[STORE_MAX_STR];

static void ui_init(void) { (void)gui_init(); }

/* Hand the display back to the debug screen at a VRAM offset the GE never
 * rendered to (0x155000: past the GU depth buffer, which sits at 2x frame
 * size but only writes 16-bit depth, ending ~0x154280). Emulators running
 * buffered rendering keep a GPU-side copy of every framebuffer the GE has
 * drawn, and present THAT for those addresses - debug-screen writes are
 * CPU-only and would never reach the screen there (verified in PPSSPP
 * 1.20: the store's last frame stays frozen on screen otherwise). With no
 * GE history at the new address the display falls back to reading RAM,
 * which is exactly how the browser drew before the Store ever started. */
#define DEBUG_VRAM_AFTER_GU ((void *)(intptr_t)(0x44000000 | 0x00155000))

static void ui_exit(void) {
    gui_term();
    pspDebugScreenInitEx(DEBUG_VRAM_AFTER_GU, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
    pspDebugScreenClear();    /* the browser owns the screen again */
}

static void ui_status(const char *head, const char *body, const char *hint) {
    store_gui_status(head, body, hint);
}

static void ui_browse(const coverflow_state_t *cf) {
    store_gui_draw(cf, g_ids, g_descs);
}

static void ui_install(const char *title, const char *path) {
    snprintf(g_inst_title, sizeof(g_inst_title), "%s", title ? title : "");
    snprintf(g_inst_path, sizeof(g_inst_path), "%s", path ? path : "");
    store_gui_progress(g_inst_title, g_inst_path, -1, 0);
}

static void ui_progress(int pct, long total) {
    store_gui_progress(g_inst_title, g_inst_path, pct, total);
}

#endif /* NASDL_TEXT_UI */

static coverflow_key_t store_key(unsigned pressed) {
    if (pressed & PSP_CTRL_LEFT) return COVERFLOW_KEY_LEFT;
    if (pressed & PSP_CTRL_RIGHT) return COVERFLOW_KEY_RIGHT;
    if (pressed & PSP_CTRL_UP) return COVERFLOW_KEY_UP;
    if (pressed & PSP_CTRL_DOWN) return COVERFLOW_KEY_DOWN;
    if (pressed & PSP_CTRL_LTRIGGER) return COVERFLOW_KEY_SHOULDER_L;
    if (pressed & PSP_CTRL_RTRIGGER) return COVERFLOW_KEY_SHOULDER_R;
    if (pressed & PSP_CTRL_SELECT) return COVERFLOW_KEY_TAB;
    if (pressed & PSP_CTRL_SQUARE) return COVERFLOW_KEY_FAVORITE;
    if (pressed & PSP_CTRL_CROSS) return COVERFLOW_KEY_ENTER;
    if (pressed & PSP_CTRL_CIRCLE) return COVERFLOW_KEY_BACK;
    return COVERFLOW_KEY_NONE;
}

int store_run(const char *nas_base) {
    if (!nas_base || !nas_base[0]) nas_base = "http://192.168.1.50:8000";
    ui_init();
    ui_status("PS Linker Store", "Loading catalog...", NULL);

    char url[STORE_MAX_STR + 32];
    snprintf(url, sizeof(url), "%s/catalog.json", nas_base);
    char *json = store_fetch_to_memory(url, NULL);
    if (!json) {
        ui_status("PS Linker Store", "Could not fetch catalog. Check server / network.", NULL);
        sceKernelDelayThread(1500000);
        ui_exit();
        return -1;
    }
    parse_catalog(json);
    free(json);
    if (g_item_count == 0) {
        ui_status("PS Linker Store", "Catalog is empty.", NULL);
        sceKernelDelayThread(1500000);
        ui_exit();
        return -2;
    }

    load_store_state();
    memset(g_covers, 0, sizeof(g_covers));
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

    coverflow_state_t cf;
    if (coverflow_init(&cf, g_covers, (size_t)g_item_count, SCREEN_COLS, SCREEN_ROWS, 0) != 0) {
        ui_exit();
        return -3;
    }
    if (g_category_count > 0) {
        /* Tabs a PSP cannot act on are useless here: PS3 installs and
           .rap licenses stay hidden (their covers have no tab to live
           under either, since the tab list is the only filter the Store
           rail exposes). */
        const char *categories[STORE_CATEGORY_LIMIT];
        int n = 0;
        for (int i = 0; i < g_category_count && n < STORE_CATEGORY_LIMIT; i++) {
            if (strcasecmp(g_categories[i], "PS3") == 0 ||
                strcasecmp(g_categories[i], "Licenses") == 0)
                continue;
            categories[n++] = g_categories[i];
        }
        if (n > 0) coverflow_set_categories(&cf, categories, (size_t)n);
    }
    unsigned prev = 0;
    while (1) {
        ui_browse(&cf);
        SceCtrlData pad;
        unsigned pressed;
        do {
            sceCtrlReadBufferPositive(&pad, 1);
            pressed = pad.Buttons & ~prev;
            prev = pad.Buttons;
            if (!pressed) sceKernelDelayThread(16000);
        } while (!pressed);

        if (pressed & PSP_CTRL_CIRCLE) {
            coverflow_shutdown(&cf);
            ui_exit();
            return 0;
        }
        if (pressed & PSP_CTRL_CROSS) {
            const coverflow_item_t *selected = coverflow_current(&cf);
            if (!selected) continue;
            int idx = (int)(selected - g_covers);
            int drc = store_derive_path(g_items[idx].url, g_items[idx].title,
                                        g_items[idx].install_path,
                                        sizeof(g_items[idx].install_path));
            if (drc != 0) {
                ui_status("PS Linker Store",
                          drc == -2 ? "Install path too long"
                                    : "Not an installable file type",
                          "Press any button to continue");
                sceKernelDelayThread(1200000);
                continue;
            }
            if (store_prepare_destination(g_items[idx].install_path) != 0) continue;
            ui_install(g_items[idx].title, g_items[idx].install_path);
            char download_url[STORE_MAX_STR + 32];
            const char *item_url = g_items[idx].url;
            if (item_url[0] == '/')
                snprintf(download_url, sizeof(download_url), "%s%s", nas_base, item_url);
            else
                snprintf(download_url, sizeof(download_url), "%s", item_url);
            int rc = psp_resume_download(download_url, g_items[idx].install_path, ui_progress);
            if (rc == 0) {
                state_add(g_recent, &g_recent_count, g_items[idx].id, 1);
                g_covers[idx].is_recent = 1;
                save_store_state();
                coverflow_recompute_filter(&cf);  /* refresh a live Recent view */
            }
            char msg[SCREEN_COLS + 1];
            /* " (" + ")" + a worst-case int leaves SCREEN_COLS-14 for the title,
               so the line can never be cut mid-number. */
            snprintf(msg, sizeof(msg), "%.*s (%d)", SCREEN_COLS - 14,
                     g_items[idx].title, rc);
            ui_status(rc == 0 ? "Installed" : "Install failed", msg,
                      "Press any button to continue");
            SceCtrlData wait;
            do { sceCtrlReadBufferPositive(&wait, 1); sceKernelDelayThread(16000); }
            while (!wait.Buttons);
            prev = wait.Buttons;
            continue;
        }

        coverflow_action_t action;
        coverflow_key_t key = store_key(pressed);
        const coverflow_item_t *before_toggle =
            key == COVERFLOW_KEY_FAVORITE ? coverflow_current(&cf) : NULL;
        int toggle_idx = before_toggle ? (int)(before_toggle - g_covers) : -1;
        int changed = coverflow_handle_key(&cf, key, &action);
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
            changed = 1;
        }
        (void)changed;
    }
}
