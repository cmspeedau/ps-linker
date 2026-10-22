/*
 * PS Linker - Store module (PSP)
 *
 * Implements a homebrew-store browser on top of the existing PS Linker
 * infrastructure. Fetches <NAS_BASE>/catalog.json, renders a Media Go-
 * style paginated list with category tabs, and downloads each item to
 * the install_path declared in the catalog.
 *
 * LEGAL: this module is a generic catalog client. It must only be used
 * with catalogs pointing at legally distributable content (homebrew,
 * freeware, demos, or the user's own purchases).
 *
 * Integrates with psp/main.c — see store.h for the one-line hookup.
 */

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <psphttp.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store.h"

#define printf pspDebugScreenPrintf

#define SCREEN_ROWS 33
#define SCREEN_COLS 68

/* Store limits (kept modest for PSP RAM; catalog is parsed in place) */
#define STORE_MAX_ITEMS 64
#define STORE_MAX_STR   256

/* Shared Media Go palette with main.c */
#define COL_BG        0xFF1D1D1D
#define COL_PANEL     0xFF262626
#define COL_HEADER    0xFFCC7A00
#define COL_SELECT    0xFF714706
#define COL_TEXT      0xFFCCCCCC
#define COL_TEXT_DIM  0xFF808080
#define COL_TEXT_SEL  0xFFFFFFFF
#define COL_STATUS    0xFF333333
#define COL_OK        0xFF3EC13E
#define COL_ERR       0xFF3D3DD5

typedef struct { u32 bg, fg; } Style;

static const Style STYLE_HEADER = { COL_HEADER,   0xFF000000 };
static const Style STYLE_SIDE   = { COL_PANEL,    COL_TEXT_DIM };
static const Style STYLE_ROW    = { COL_BG,       COL_TEXT };
static const Style STYLE_SEL    = { COL_SELECT,   COL_TEXT_SEL };
static const Style STYLE_STATUS = { COL_STATUS,   COL_TEXT_DIM };

static void store_set_style(const Style *s) {
    pspDebugScreenSetBackColor(s->bg);
    pspDebugScreenSetTextColor(s->fg);
}

static void store_pad(int n) {
    for (int i = 0; i < n; i++) putchar(' ');
}

static void store_field(int col, int row, int width, const Style *s, const char *text) {
    pspDebugScreenSetXY(col, row);
    store_set_style(s);
    int len = (int)strlen(text);
    printf("%s", text);
    if (len < width) store_pad(width - len);
}

/* ------------------------------------------------------------------ */
/* Catalog model                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char id[STORE_MAX_STR];
    char title[STORE_MAX_STR];
    char category[STORE_MAX_STR];
    char description[STORE_MAX_STR];
    char version[32];
    char url[STORE_MAX_STR];
    char install_path[STORE_MAX_STR];
    char size_hint[32];
} StoreItem;

static StoreItem g_items[STORE_MAX_ITEMS];
static int g_item_count = 0;

static char g_categories[8][32];
static int g_category_count = 0;

/* ------------------------------------------------------------------ */
/* Minimal JSON string-field extractor                                 */
/*                                                                     */
/* The catalog is machine-generated and well-formed, so this does a    */
/* tolerant scan: find "key": then the following quoted value. It      */
/* avoids pulling in a full JSON parser on the PSP.                    */
/* ------------------------------------------------------------------ */

/* Find the value of "key" at or after *pos; advances *pos past it.
   Returns 1 and fills out on success, 0 if not found. */
static int json_find_string(const char *json, const char *key, size_t *pos,
                            char *out, size_t outlen) {
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

/* Parse one item object starting at *pos (points at '{'). */
static int parse_item(const char *json, size_t *pos, StoreItem *it) {
    const char *p = strstr(json + *pos, "{");
    if (!p) return 0;
    const char *end = strstr(p, "}");
    if (!end) return 0;

    memset(it, 0, sizeof(*it));
    size_t local = 0;
    json_find_string(p, "id",            &local, it->id,            sizeof(it->id));
    json_find_string(p, "title",         &local, it->title,         sizeof(it->title));
    json_find_string(p, "category",      &local, it->category,      sizeof(it->category));
    json_find_string(p, "description",   &local, it->description,   sizeof(it->description));
    json_find_string(p, "version",       &local, it->version,       sizeof(it->version));
    json_find_string(p, "url",           &local, it->url,           sizeof(it->url));
    json_find_string(p, "install_path",  &local, it->install_path,  sizeof(it->install_path));
    json_find_string(p, "size_hint",     &local, it->size_hint,     sizeof(it->size_hint));

    *pos = (size_t)(end - json) + 1;

    /* An item without a download URL is not useful; skip it. */
    return it->url[0] != '\0' && it->title[0] != '\0';
}

static void register_category(const char *cat) {
    if (cat[0] == '\0') return;
    for (int i = 0; i < g_category_count; i++)
        if (strcmp(g_categories[i], cat) == 0) return;
    if (g_category_count < 8)
        snprintf(g_categories[g_category_count++], 32, "%s", cat);
}

static void parse_catalog(const char *json) {
    g_item_count = 0;
    g_category_count = 0;

    /* Read declared category order first (falls back to discovery). */
    size_t pos = 0;
    char cat[32];
    while (g_category_count < 8 &&
           json_find_string(json, "category", &pos, cat, sizeof(cat))) {
        /* json_find_string also matches "category" inside items; only
           the first pass here seeds the tab order, and register_category
           de-duplicates either way. */
        register_category(cat);
    }

    /* Parse items array. */
    const char *items = strstr(json, "\"items\"");
    if (!items) return;

    size_t ipos = (size_t)(items - json);
    StoreItem it;
    while (g_item_count < STORE_MAX_ITEMS && parse_item(json, &ipos, &it)) {
        g_items[g_item_count++] = it;
        register_category(it.category);
    }
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
            int clen = 0;
            sceHttpGetContentLength(req, &clen);
            if (clen <= 0) clen = 128 * 1024;   /* catalog is small */

            char *buf = (char *)malloc(clen + 1);
            if (buf) {
                int total = 0, n;
                while ((n = sceHttpReadData(req, buf + total, clen - total)) > 0)
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

/* Download to install_path with a Media Go-style progress bar.
   Mirrors download_file() in main.c but takes a caller-supplied
   destination (install_path from the catalog). */
#define STORE_DL_BUF (64 * 1024)

static int store_download(const char *url, const char *dest_path) {
    int tpl = sceHttpCreateTemplate("PSLinkerStore", 1, 1);
    if (tpl < 0) return -1;

    int status = -1;
    int conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    if (conn >= 0) {
        int req = sceHttpCreateRequestWithURL(conn, PSP_HTTP_METHOD_GET, url, 0);
        if (req >= 0 && sceHttpSendRequest(req, NULL, 0) >= 0) {
            int clen = 0;
            sceHttpGetContentLength(req, &clen);

            /* Ensure the parent directory exists. */
            char dir[STORE_MAX_STR];
            snprintf(dir, sizeof(dir), "%s", dest_path);
            char *slash = strrchr(dir, '/');
            if (slash && slash != dir) {
                *slash = '\0';
                sceIoMkdir(dir, 0777);
            }

            SceUID fd = sceIoOpen(dest_path,
                                  PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (fd >= 0) {
                static char buf[STORE_DL_BUF];
                int n, total = 0, ok = 1;
                char label[96];
                snprintf(label, sizeof(label), "Installing %s", dest_path);

                while (ok && (n = sceHttpReadData(req, buf, sizeof(buf))) > 0) {
                    if (sceIoWrite(fd, buf, n) != n) { ok = 0; break; }
                    total += n;

                    int pct = clen > 0 ? (total * 100) / clen : 0;
                    if (pct > 100) pct = 100;
                    int bar_w = SCREEN_COLS - 16;
                    int fill = clen > 0 ? (total * bar_w) / clen : 0;
                    char info[48];
                    snprintf(info, sizeof(info), "%d%%  (%d KB)", pct, total / 1024);

                    Style st = { COL_BG, COL_TEXT };
                    store_field(0, SCREEN_ROWS - 6, SCREEN_COLS, &st, label);
                    pspDebugScreenSetXY(0, SCREEN_ROWS - 5);
                    store_set_style(&STYLE_ROW);
                    store_pad(bar_w);
                    pspDebugScreenSetXY(0, SCREEN_ROWS - 5);
                    Style sp = { COL_HEADER, COL_HEADER };
                    store_field(0, SCREEN_ROWS - 5, fill, &sp, "");
                    Style si = { COL_BG, COL_TEXT_DIM };
                    store_field(bar_w + 1, SCREEN_ROWS - 5, 15, &si, info);
                }
                sceIoClose(fd);
                status = ok ? 0 : -2;
            } else {
                status = -3;   /* could not open destination */
            }
        }
        if (req >= 0) sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
    }
    sceHttpDeleteTemplate(tpl);
    return status;
}

/* ------------------------------------------------------------------ */
/* Store UI                                                            */
/* ------------------------------------------------------------------ */

static void store_wait_button(void) {
    SceCtrlData pad;
    do { sceCtrlReadBufferPositive(&pad, 1); sceKernelDelayThread(16000); }
    while (pad.Buttons == 0);
}

static void store_draw_header(const char *subtitle) {
    char buf[SCREEN_COLS + 1];
    snprintf(buf, sizeof(buf), "  PS Linker Store  %s", subtitle);
    store_field(0, 0, SCREEN_COLS, &STYLE_HEADER, buf);
}

static void store_draw_status(const char *msg) {
    char buf[SCREEN_COLS + 1];
    snprintf(buf, sizeof(buf),
             " Up/Down:Item  Left/Right:Category  X:Install  O:Exit | %s",
             msg ? msg : "");
    store_field(0, SCREEN_ROWS - 1, SCREEN_COLS, &STYLE_STATUS, buf);
}

/* Items visible for the current category (indices into g_items). */
static int item_matches(int idx, int cat) {
    if (cat <= 0) return 1;                 /* 0 = "All" */
    return strcmp(g_items[idx].category, g_categories[cat - 1]) == 0;
}

int store_run(const char *nas_base) {
    if (!nas_base || !nas_base[0]) nas_base = "http://192.168.1.50:8000";

    /* --- fetch catalog -------------------------------------------- */
    pspDebugScreenClear();
    store_draw_header("Loading catalog...");
    store_draw_status("Fetching");

    char url[STORE_MAX_STR + 32];
    snprintf(url, sizeof(url), "%s/catalog.json", nas_base);

    char *json = store_fetch_to_memory(url, NULL);
    if (!json) {
        Style err = { COL_BG, COL_ERR };
        char msg[128];
        snprintf(msg, sizeof(msg), "Could not fetch %s", url);
        store_field(4, 4, SCREEN_COLS - 8, &err, msg);
        store_field(4, 5, SCREEN_COLS - 8, &err,
                    "Check that server/catalog.json is in the HTTP share.");
        store_wait_button();
        return -1;
    }

    parse_catalog(json);
    free(json);

    if (g_item_count == 0) {
        Style err = { COL_BG, COL_ERR };
        store_field(4, 4, SCREEN_COLS - 8, &err,
                    "Catalog is empty or malformed.");
        store_wait_button();
        return -2;
    }

    /* Prepend the implicit "All" tab. */
    for (int i = g_category_count; i > 0; i--)
        memcpy(g_categories[i], g_categories[i - 1], 32);
    snprintf(g_categories[0], 32, "All");
    g_category_count++;

    /* --- browser loop ---------------------------------------------- */
    int cat = 0, sel = 0, top = 0;
    unsigned prev = 0;

    while (1) {
        pspDebugScreenClear();

        const int list_top = 2;
        const int list_rows = SCREEN_ROWS - 8 - list_top;

        /* Current category tab strip. */
        char tabs[SCREEN_COLS + 1];
        int used = 0;
        tabs[0] = '\0';
        for (int c = 0; c < g_category_count && used < SCREEN_COLS - 4; c++) {
            int n = snprintf(tabs + used, sizeof(tabs) - used, "%s[%s]",
                             c == cat ? "> " : " ", g_categories[c]);
            if (n < 0) break;
            used += n;
        }
        store_field(1, 1, SCREEN_COLS, &STYLE_SIDE, tabs);

        /* Visible rows: iterate items, filtering by category. */
        int visible[STORE_MAX_ITEMS];
        int vcount = 0;
        for (int i = 0; i < g_item_count; i++)
            if (item_matches(i, cat)) visible[vcount++] = i;

        if (sel >= vcount) { sel = vcount - 1; top = 0; }
        if (sel < 0) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + list_rows) top = sel - list_rows + 1;

        for (int r = 0; r < list_rows && top + r < vcount; r++) {
            int idx = visible[top + r];
            StoreItem *it = &g_items[idx];
            char line[SCREEN_COLS];
            snprintf(line, sizeof(line), "%-28.28s %-9.9s %s",
                     it->title, it->size_hint, it->version);
            store_field(2, list_top + r, SCREEN_COLS - 4,
                        idx == sel ? &STYLE_SEL : &STYLE_ROW, line);
        }

        /* Detail line for the selected item. */
        if (vcount > 0) {
            StoreItem *it = &g_items[visible[sel]];
            char detail[SCREEN_COLS];
            snprintf(detail, sizeof(detail), "%s", it->description);
            store_field(2, SCREEN_ROWS - 5, SCREEN_COLS - 4, &STYLE_ROW, detail);
            snprintf(detail, sizeof(detail), "To: %s", it->install_path);
            store_field(2, SCREEN_ROWS - 4, SCREEN_COLS - 4, &STYLE_ROW, detail);
        }

        char status[64];
        snprintf(status, sizeof(status), "%d of %d items", vcount, g_item_count);
        store_draw_status(status);

        /* Edge-detected input. */
        SceCtrlData pad;
        unsigned pressed = 0;
        do {
            sceCtrlReadBufferPositive(&pad, 1);
            pressed = pad.Buttons & ~prev;
            prev = pad.Buttons;
            if (!pressed) sceKernelDelayThread(16000);
        } while (!pressed);

        if (pressed & PSP_CTRL_UP) {
            sel = (sel - 1 + vcount) % vcount;
        } else if (pressed & PSP_CTRL_DOWN) {
            sel = (sel + 1) % vcount;
        } else if (pressed & PSP_CTRL_LEFT) {
            cat = (cat - 1 + g_category_count) % g_category_count;
            sel = 0; top = 0;
        } else if (pressed & PSP_CTRL_RIGHT) {
            cat = (cat + 1) % g_category_count;
            sel = 0; top = 0;
        } else if (pressed & PSP_CTRL_CROSS) {
            if (vcount == 0) continue;
            StoreItem *it = &g_items[visible[sel]];

            int rc = store_download(it->url, it->install_path);
            Style res = { COL_BG, rc == 0 ? COL_OK : COL_ERR };
            char msg[128];
            if (rc == 0) snprintf(msg, sizeof(msg), "Installed: %s", it->title);
            else         snprintf(msg, sizeof(msg), "Install failed (%d): %s",
                                  rc, it->title);
            pspDebugScreenClear();
            store_draw_header(rc == 0 ? "Installed" : "Install failed");
            store_field(4, SCREEN_ROWS - 4, SCREEN_COLS - 8, &res, msg);
            store_draw_status(rc == 0 ? "Installed - launch from XMB"
                                      : "Install failed");
            store_wait_button();
        } else if (pressed & PSP_CTRL_CIRCLE) {
            return 0;   /* back to the file browser */
        }
    }
}
