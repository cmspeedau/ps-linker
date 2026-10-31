/*
 * PS Linker - PSP NAS Downloader with Cover Flow browser
 *
 * Connects to Wi-Fi and boots straight into the pixel Cover Flow view
 * (the same frame the Store uses): the share's directory listing is the
 * carousel, the tab rail is All | share categories | Downloads, Cross
 * downloads or opens, and Triangle opens the Store (catalog.json).
 * Files land in ms0:/PSP/GAME/NASDL/<filename>, which is what the
 * Downloads tab lists.
 *
 * Categories a PSP cannot act on (Licenses, PS3) never appear as
 * covers or tabs - see ui/browser_model.c's skip list.
 *
 * UI: everything the user browses draws through gui.c/store_gui.c at
 * 480x272. The pspDebugScreen character grid survives only behind
 * SELECT (Settings), per the design note in store.c's ui_exit.
 *
 * Build with pspdev toolchain: make
 * Requires CFW (ARK-4 / PRO) to run.
 *
 * Controls in browser:  Left/Right = cover, Up/Down = jump five,
 *                       L1/R1 = tab, Square = flow/grid,
 *                       X = open / download,
 *                       O = parent directory (Downloads view off,
 *                           quit at root),
 *                       Select = Settings, Triangle = Store
 * Controls in settings: Left/Right = pick char, X = insert/OK,
 *                       O = delete char, Start = save & back,
 *                       Select = back without saving
 */

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspwlan.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psphttp.h>
#include <pspiofilemgr.h>
#include <psputility_netmodules.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>              /* strcasecmp for the Downloads skips */
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#include "gui.h"
#include "store_gui.h"
#include "resume.h"
#include "store.h"
#include "../ui/coverflow.h"
#include "../ui/browser_model.h"
#include "../ui/store_logic.h"

#define printf pspDebugScreenPrintf
#define SCREEN_ROWS 33
#define SCREEN_COLS 68

PSP_MODULE_INFO("PSLinker", 0, 1, 3);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

/* Default base URL of the NAS share; editable at runtime via Settings
   and persisted to SETTINGS_PATH. */
#define DEFAULT_NAS_URL "http://192.168.1.50:8000"

/* Destination directory on the Memory Stick (also the Downloads view) */
#define DEST_DIR "ms0:/PSP/GAME/NASDL/"

/* Settings file (plain text: one line, the base URL) */
#define SETTINGS_PATH "ms0:/PSP/GAME/NASDL/settings.txt"

#define MAX_FILES     128
#define MAX_PATH_LEN  256
#define MAX_URL_LEN   128

/* ------------------------------------------------------------------ */
/* Debug-screen style helpers (Settings screen only)                   */
/* ------------------------------------------------------------------ */

#define COL_BG        0xFF1D1D1D   /* charcoal background            */
#define COL_HEADER    0xFFCC7A00   /* accent bar                     */
#define COL_TEXT      0xFFCCCCCC   /* normal text                    */
#define COL_TEXT_DIM  0xFF808080   /* muted text                     */
#define COL_TEXT_SEL  0xFFFFFFFF   /* text on selection              */
#define COL_SELECT    0xFF714706   /* selection highlight            */
#define COL_OK        0xFF3EC13E   /* success green                  */
#define COL_ERR       0xFF3D3DD5   /* error red                      */

typedef struct {
    u32 bg, fg;
} Style;

static const Style STYLE_HEADER = { COL_HEADER,   0xFF000000 };
static const Style STYLE_ROW    = { COL_BG,       COL_TEXT };
static const Style STYLE_SEL    = { COL_SELECT,   COL_TEXT_SEL };

static void set_style(const Style *s) {
    pspDebugScreenSetBackColor(s->bg);
    pspDebugScreenSetTextColor(s->fg);
}

/* Print n background-colored spaces (used to draw panels/bars). */
static void pad(int n) {
    for (int i = 0; i < n; i++) putchar(' ');
}

/* Move to (col,row) and print text left-aligned inside a width-w field. */
static void field(int col, int row, int width, const Style *s, const char *text) {
    pspDebugScreenSetXY(col, row);
    set_style(s);
    int len = (int)strlen(text);
    printf("%s", text);
    if (len < width) pad(width - len);
}

/* Full-width header bar, like Media Go's title strip. */
static void draw_header(const char *title, const char *subtitle) {
    char buf[SCREEN_COLS + 1];
    snprintf(buf, sizeof(buf), "  %s  %s", title, subtitle);
    field(0, 0, SCREEN_COLS, &STYLE_HEADER, buf);
}

/* Hand the display back from the GU to the debug screen at a VRAM
   offset the GE never rendered to (same trick and reasoning as
   store.c's ui_exit: buffered-rendering emulators keep a GPU-side copy
   of every GE framebuffer and would present a frozen pixel frame for
   addresses with GE history; 0x155000 has none). */
#define DEBUG_VRAM_AFTER_GU ((void *)(intptr_t)(0x44000000 | 0x00155000))

static void gui_to_debug(void) {
    gui_term();
    pspDebugScreenInitEx(DEBUG_VRAM_AFTER_GU, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
    pspDebugScreenClear();
}

/* ------------------------------------------------------------------ */
/* Settings: runtime NAS URL, persisted to the Memory Stick            */
/* ------------------------------------------------------------------ */

/* Base URL of the NAS share, e.g. "http://192.168.1.50:8000".
   Editable in the Settings view; lives in RAM after load. */
static char g_nas_base[MAX_URL_LEN] = DEFAULT_NAS_URL;

/* The gclite feature switch, owned by the pixel Utilities page
   (Settings -> Triangle). ON (the default) is the whole game-categories
   integration: CAT_ folder names display stripped and START opens the
   categories popup (the popup still requires the plugin registration in
   ms0:/seplugins/vsh.txt). OFF, folders keep their raw names and START
   does nothing. Persisted as line 2 of settings.txt; files without the
   line — every file written before this switch existed — mean ON. */
int g_gclite_on = 1;

/* Load the saved URL and feature switch. Silently keeps defaults for
   absent/invalid fields, including the second line missing. */
static void load_settings(void) {
    SceUID fd = sceIoOpen(SETTINGS_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    char buf[MAX_URL_LEN + 32];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char *line2 = strchr(buf, '\n');            /* split off line 2 */
    if (line2) {
        *line2++ = '\0';
        if (strncmp(line2, "gclite=0", 8) == 0) g_gclite_on = 0;
    }

    /* trim trailing whitespace/newlines from the URL line */
    n = (int)strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    if (n > 7 && strncmp(buf, "http://", 7) == 0)
        snprintf(g_nas_base, sizeof(g_nas_base), "%s", buf);
}

static int save_settings(void) {
    SceUID fd = sceIoOpen(SETTINGS_PATH,
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;
    char buf[MAX_URL_LEN + 32];
    int len = snprintf(buf, sizeof(buf), "%s\ngclite=%d\n",
                       g_nas_base, g_gclite_on ? 1 : 0);
    int w = sceIoWrite(fd, buf, len);
    sceIoClose(fd);
    return (w == len) ? 0 : -2;
}

/* ------------------------------------------------------------------ */
/* File listing state                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[MAX_PATH_LEN];   /* display name (decoded, raw)       */
    char url[MAX_PATH_LEN];    /* absolute URL for fetch/download   */
    long long size;            /* bytes from data-size, -1 unknown  */
    char category[64];         /* data-cat (catalog category), "" if none */
    char platform[16];         /* data-platform ("PSP"/"PS3"), "" if none */
    int  is_dir;               /* entry is a subdirectory (trailing /) */
} FileEntry;

static FileEntry g_files[MAX_FILES];
static int g_file_count = 0;

/* Current directory relative to g_nas_base, "" = root.
   Always stored WITH a trailing slash when non-empty. */
static char g_cwd[MAX_PATH_LEN] = "";

/* ------------------------------------------------------------------ */
/* Simple parser: extract href entries from an HTML directory listing  */
/* Works with python3 -m http.server, nginx autoindex, Synology shares */
/* ------------------------------------------------------------------ */

static void url_decode(char *dst, const char *src, size_t dstlen) {
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < dstlen; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/* Fetch a URL fully into a heap buffer. Returns NULL on failure. */
static char *fetch_to_memory(const char *url, int *out_len) {
    int tpl_id = sceHttpCreateTemplate("PSLinker", 1, 1);
    if (tpl_id < 0) return NULL;

    char *result = NULL;
    int conn = sceHttpCreateConnectionWithURL(tpl_id, url, 0);
    if (conn >= 0) {
        int req = sceHttpCreateRequestWithURL(conn, PSP_HTTP_METHOD_GET,
                                              (char *)url, 0);
        if (req >= 0 && sceHttpSendRequest(req, NULL, 0) >= 0) {
            SceULong64 clen64 = 0;
            sceHttpGetContentLength(req, &clen64);
            int clen = (clen64 > 0 && clen64 <= 1024 * 1024)
                ? (int)clen64 : 512 * 1024;   /* assume up to 512 KB */

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
    sceHttpDeleteTemplate(tpl_id);
    return result;
}

/* Join g_nas_base + g_cwd + name into out, collapsing duplicate slashes so a
   URL never contains "//" except the "scheme://" separator. Returns 0, or -1
   on overflow/truncation. */
static int build_url(const char *name, char *out, size_t outlen) {
    char joined[MAX_PATH_LEN * 2];
    int n = snprintf(joined, sizeof(joined), "%s/%s/%s", g_nas_base,
                     g_cwd, name);
    size_t i, o = 0;
    if (n < 0 || (size_t)n >= sizeof(joined) || outlen == 0) return -1;
    for (i = 0; joined[i] != '\0' && o + 1 < outlen; i++) {
        /* keep a '/' unless it duplicates the previous one, but never
           collapse the "//" that follows a ':' (scheme://) */
        if (joined[i] != '/' || o == 0 || out[o - 1] != '/' ||
            (o >= 2 && out[o - 2] == ':')) out[o++] = joined[i];
    }
    if (joined[i] != '\0') return -1;   /* out filled before joined drained */
    out[o] = '\0';
    return 0;
}

static int add_file(const char *href, long long size, const char *category,
                    const char *platform) {
    /* Skip parent links and server-absolute links */
    if (href[0] == '\0') return 0;
    if (strstr(href, "../") == href) return 0;
    if (href[0] == '/') return 0;
    if (strchr(href, '/') != NULL) {          /* nested path: only keep it
                                                 if it is a dir itself      */
        size_t l = strlen(href);
        if (!(l > 0 && href[l - 1] == '/')) return 0;
    }

    size_t hl = strlen(href);
    int is_dir = (hl > 0 && href[hl - 1] == '/');

    /* Display name: the raw name (directories carry no brackets - the
       Cover Flow model adds the trailing '/' to folder titles). */
    char display[MAX_PATH_LEN];
    url_decode(display, href, sizeof(display));
    if (is_dir && hl > 0) display[hl - 1] = '\0';

    if (g_file_count >= MAX_FILES) return 0;

    FileEntry *e = &g_files[g_file_count];
    if (build_url(href, e->url, MAX_PATH_LEN) != 0) return 0;
    snprintf(e->name, MAX_PATH_LEN, "%s", display);
    e->is_dir = is_dir;
    e->size = size;
    snprintf(e->category, sizeof(e->category), "%s", category ? category : "");
    snprintf(e->platform, sizeof(e->platform), "%s", platform ? platform : "");
    g_file_count++;
    return 1;
}

/* Read key="value" from a directory-listing anchor's tail (the text after
   href's closing quote, within that tag only). Returns 1 when found. */
static int attr_at(const char *start, const char *end, const char *key,
                   char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = start; p + kl + 3 <= end; p++) {
        if (p[0] != ' ' || memcmp(p + 1, key, kl) != 0) continue;
        const char *v = p + 1 + kl;
        if (v[0] != '=' || v[1] != '"') continue;
        v += 2;
        const char *q = (const char *)memchr(v, '"', (size_t)(end - v));
        if (!q) return 0;
        size_t n = (size_t)(q - v);
        if (n >= cap) n = cap - 1;
        memcpy(out, v, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

static int parse_listing(const char *html) {
    g_file_count = 0;
    const char *p = html;
    while (g_file_count < MAX_FILES && (p = strstr(p, "href=\"")) != NULL) {
        p += 6;                              /* skip href=" */
        const char *end = strchr(p, '"');
        if (!end) break;
        int len = end - p;
        if (len > 0 && len < MAX_PATH_LEN) {
            char href[MAX_PATH_LEN];
            memcpy(href, p, len);
            href[len] = '\0';
            /* The rest of this anchor may carry data-size / data-cat /
               data-platform; the search stops at the next tag so no
               neighbouring entry leaks in, and a share without the
               attributes parses as before. */
            const char *tag_end = strchr(end, '<');
            if (!tag_end) tag_end = end;
            char sizebuf[24], cat[64], plat[16];
            long long size = -1;
            cat[0] = plat[0] = '\0';
            if (attr_at(end + 1, tag_end, "data-size", sizebuf, sizeof sizebuf))
                size = strtoll(sizebuf, NULL, 10);
            attr_at(end + 1, tag_end, "data-cat", cat, sizeof cat);
            attr_at(end + 1, tag_end, "data-platform", plat, sizeof plat);
            add_file(href, size, cat, plat);
        }
        p = end + 1;
    }
    return g_file_count;
}

/* ------------------------------------------------------------------ */
/* Directory navigation                                                */
/* ------------------------------------------------------------------ */

/* Append dir_name (no trailing slash) to g_cwd. Returns 0 on success. */
static int cwd_enter(const char *dir_name) {
    size_t cl = strlen(g_cwd);
    size_t nl = strlen(dir_name);
    if (cl + nl + 2 > sizeof(g_cwd)) return -1;   /* + slash + NUL */
    memcpy(g_cwd + cl, dir_name, nl);
    g_cwd[cl + nl] = '/';
    g_cwd[cl + nl + 1] = '\0';
    return 0;
}

/* Remove the last path component from g_cwd. */
static void cwd_up(void) {
    char *slash = strrchr(g_cwd, '/');
    if (!slash) return;
    if (slash == g_cwd) {
        g_cwd[0] = '\0';               /* was "/x/" -> root */
        return;
    }
    *slash = '\0';                     /* drop the trailing slash */
    char *parent = strrchr(g_cwd, '/');
    if (parent)
        parent[1] = '\0';              /* keep the parent's slash */
    else
        g_cwd[0] = '\0';               /* one component -> root */
}

/* Fetch the listing for g_cwd. Returns 0 or <0. */
static int fetch_listing(void) {
    char url[MAX_PATH_LEN + 8];
    if (build_url("", url, sizeof(url)) != 0) return -1;
    char *html = fetch_to_memory(url, NULL);
    if (!html) return -2;
    parse_listing(html);
    free(html);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Download with the Store's pixel progress frame                      */
/* resume.c sends Range headers and continues an interrupted transfer  */
/* from the partial file on disk (kept as <name>.part; the Downloads   */
/* list shows it as a paused row under the file's own name).           */
/* ------------------------------------------------------------------ */

static char g_dl_name[MAX_PATH_LEN];
static char g_dl_path[MAX_PATH_LEN + 32];
static int g_dl_last_pct = -999;
static long g_dl_last_total = 0;

static void download_progress(int pct, long total) {
    if (pct == g_dl_last_pct &&
        !(pct < 0 && total - g_dl_last_total >= 1024 * 1024))
        return;                          /* the pixel present is a full copy */
    g_dl_last_pct = pct;
    g_dl_last_total = total;
    store_gui_progress(g_dl_name, g_dl_path, pct, total);
}

static int download_file(const char *url, const char *dest_path,
                         const char *shown_name) {
    snprintf(g_dl_name, sizeof(g_dl_name), "%s", shown_name);
    snprintf(g_dl_path, sizeof(g_dl_path), "%s", dest_path);
    g_dl_last_pct = -999;
    g_dl_last_total = 0;
    store_gui_progress(g_dl_name, g_dl_path, -1, 0);
    return psp_resume_download(url, dest_path, download_progress);
}

/* ------------------------------------------------------------------ */
/* Pad helpers                                                         */
/* ------------------------------------------------------------------ */

static void wait_button(void) {
    SceCtrlData pad;
    do { sceCtrlReadBufferPositive(&pad, 1); sceKernelDelayThread(16000); }
    while (pad.Buttons == 0);
}

/* Edge-detected button read; fills *pressed, returns 0 on new press. */
static int get_pressed(unsigned *pressed, unsigned *prev_out) {
    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);
    *pressed = pad.Buttons & ~*prev_out;
    *prev_out = pad.Buttons;
    return (*pressed != 0) ? 0 : -1;
}

/* After a nested screen (Settings, Store) returns: wait until the pad
   is clear, so the button that closed it is not seen as a fresh press
   here (it would hit the "up one level" branch and quit at root). */
static void pad_clear(unsigned *prev_out) {
    SceCtrlData pad;
    do {
        sceCtrlReadBufferPositive(&pad, 1);
        sceKernelDelayThread(16000);
    } while (pad.Buttons);
    *prev_out = 0;
}

/* Splash screen shown while connecting/fetching, pixel chrome. */
static void draw_splash(const char *msg) {
    store_gui_status("PS Linker", msg, "Starting...");
}

/* ------------------------------------------------------------------ */
/* Settings view: edit the NAS URL with an on-screen character picker  */
/* (the one remaining debug-screen UI - see the header comment)        */
/* ------------------------------------------------------------------ */

/* Single-row character strip, Media Go style. */
static const char SETTINGS_CHARS[] =
    "abcdefghijklmnopqrstuvwxyz0123456789-.:/_";
#define SETTINGS_NCHARS ((int)(sizeof(SETTINGS_CHARS) - 1))
/* Special slots after the chars: 0=delete, 1=save & exit */
#define SP_DEL  SETTINGS_NCHARS
#define SP_OK   (SETTINGS_NCHARS + 1)
#define SETTINGS_NSLOTS (SETTINGS_NCHARS + 2)

static int settings_view(void) {
    char url[MAX_URL_LEN];
    int pick = 0, view = 0;
    unsigned prev = 0;

    /* Consume the entry key: seed `prev` with the buttons still held so
       the SELECT that opened this view cannot re-fire as a cancel. */
    { SceCtrlData p0; sceCtrlReadBufferPositive(&p0, 1); prev = p0.Buttons; }

    /* Visible window of the strip: pick stays inside [view, view+W) */
    const int strip_w = SCREEN_COLS - 16;      /* list panel width */
    const int picker_row = SCREEN_ROWS - 4;

    while (1) {
        pspDebugScreenClear();
        draw_header("PS Linker", "Settings");

        snprintf(url, sizeof(url), "%s", g_nas_base);
        Style lbl = { COL_BG, COL_TEXT_DIM };
        field(14, 4, strip_w, &lbl, " NAS SERVER URL");
        Style val = { COL_BG, COL_TEXT };
        /* truncate URL to fit the panel */
        char shown[MAX_URL_LEN];
        snprintf(shown, sizeof(shown), "%s", url);
        if ((int)strlen(shown) > strip_w - 2) shown[strip_w - 2] = '\0';
        field(15, 6, strip_w, &val, shown);

        Style hint = { COL_BG, COL_TEXT_DIM };
        field(14, 8, strip_w, &hint,
              " <>:pick  X:insert  O:delete  Start:save  Sel:cancel"
              "  Tri:util");

        /* Character picker strip */
        for (int i = 0; i < strip_w / 2 && view + i < SETTINGS_NSLOTS; i++) {
            int slot = view + i;
            char ch[2];
            const Style *st = (slot == pick) ? &STYLE_SEL : &STYLE_ROW;
            if (slot < SETTINGS_NCHARS) {
                ch[0] = SETTINGS_CHARS[slot];
                ch[1] = '\0';
            } else if (slot == SP_DEL) {
                ch[0] = '<'; ch[1] = '\0';
            } else {
                field(14 + i * 2, picker_row, 2, st, "OK");
                continue;
            }
            field(14 + i * 2, picker_row, 2, st, ch);
        }

        unsigned pressed;
        do { sceKernelDelayThread(16000); }
        while (get_pressed(&pressed, &prev) != 0);

        if (pressed & PSP_CTRL_RIGHT) {
            if (pick < SETTINGS_NSLOTS - 1) pick++;
            if (pick >= view + strip_w / 2) view = pick - strip_w / 2 + 1;
        } else if (pressed & PSP_CTRL_LEFT) {
            if (pick > 0) pick--;
            if (pick < view) view = pick;
        } else if (pressed & PSP_CTRL_CROSS) {
            if (pick < SETTINGS_NCHARS) {
                size_t l = strlen(g_nas_base);
                if (l + 1 < MAX_URL_LEN) {
                    g_nas_base[l] = SETTINGS_CHARS[pick];
                    g_nas_base[l + 1] = '\0';
                }
            } else if (pick == SP_DEL) {
                size_t l = strlen(g_nas_base);
                if (l > 0) g_nas_base[l - 1] = '\0';
            } else {                          /* SP_OK: save & exit */
                if (save_settings() == 0) return 1;
            }
        } else if (pressed & PSP_CTRL_CIRCLE) {
            size_t l = strlen(g_nas_base);    /* delete last char */
            if (l > 0) g_nas_base[l - 1] = '\0';
        } else if (pressed & PSP_CTRL_START) {
            save_settings();
            return 1;
        } else if (pressed & PSP_CTRL_SELECT) {
            return 0;                         /* cancel */
        } else if (pressed & PSP_CTRL_TRIANGLE) {
            return 2;                         /* open the Utilities page */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Browser (Cover Flow)                                                */
/* ------------------------------------------------------------------ */

/* The browser IS the Store frame: main.c drives the shared
   ui/coverflow.c state through ui/browser_model.c (listing -> covers +
   tab rail), and store_gui_browse draws it. Tabs: All | share
   categories | Downloads - the Downloads tab is a row list (name +
   progress bar) of what landed in DEST_DIR, .part resumes included as
   paused rows. A PSP browses and fetches PSP files only: Licenses and
   PS3 categories are skipped outright, and the data-platform chrome
   drops any remaining foreign-console file from covers and tabs. */

#define MAX_TABS COVERFLOW_MAX_DYNAMIC_CATEGORIES

/* Categories useless on a PSP: they are skipped as covers AND as tabs. */
static const char *const PSP_SKIP_CATEGORIES[] = { "Licenses", "PS3" };
#define PSP_SKIP_COUNT ((int)(sizeof(PSP_SKIP_CATEGORIES) / sizeof(PSP_SKIP_CATEGORIES[0])))

typedef struct {
    char name[MAX_PATH_LEN];
    long long size;
    int is_part;               /* an interrupted <name>.part, shown too */
} DlEntry;

static DlEntry g_dl[MAX_FILES];
static int g_dl_count = 0;

static coverflow_state_t g_cf;
static coverflow_item_t g_cf_items[MAX_FILES];
static int g_cf_map[MAX_FILES];      /* item slot -> g_files / g_dl index */
static char g_tabs[MAX_TABS][COVERFLOW_DYNAMIC_CATEGORY_LEN];
static int g_tab_count = 0;
static int g_downloads_tab = -1;
static int g_downloads_active = 0;

static void scan_downloads(void) {
    g_dl_count = 0;
    SceUID dir = sceIoDopen(DEST_DIR);
    if (dir < 0) return;
    SceIoDirent ent;
    while (g_dl_count < MAX_FILES && sceIoDread(dir, &ent) > 0) {
        if (FIO_SO_ISDIR(ent.d_stat.st_attr)) continue;
        const char *n = ent.d_name;
        if (n[0] == '.') continue;                    /* hidden          */
        /* DEST_DIR is the app's own folder: the app and its config are
           not downloads (FAT may upper-case the names, so compare
           case-insensitively). */
        if (strcasecmp(n, "settings.txt") == 0 ||
            strcasecmp(n, "EBOOT.PBP") == 0 ||
            strcasecmp(n, "store_state.txt") == 0)
            continue;
        size_t nl = strlen(n);
        const int part = (nl >= 5 && strcmp(n + nl - 5, ".part") == 0);
        /* a resume partial shows as its intended file, paused */
        if (part) nl -= 5;
        snprintf(g_dl[g_dl_count].name, sizeof(g_dl[g_dl_count].name),
                 "%.*s", (int)nl, n);
        g_dl[g_dl_count].size = (long long)(u32)ent.d_stat.st_size;
        g_dl[g_dl_count].is_part = part;
        g_dl_count++;
    }
    sceIoDclose(dir);
}

/* Rebuild the cover items for the active source. reset_tab (a new
   listing) also rebuilds the tab rail and lands on the All tab; a tab
   switch keeps the rail and only swaps the items under it. */
static void browser_fill(int reset_tab) {
    static bm_entry_t sh[MAX_FILES];
    static bm_entry_t dl[MAX_FILES];
    /* gclite display copies: CAT_ folders and CAT_ categories show as
       plain names (navigation still uses the raw e->name / e->url). */
    static char sh_name[MAX_FILES][MAX_PATH_LEN];
    static char sh_cat[MAX_FILES][COVERFLOW_DYNAMIC_CATEGORY_LEN];

    if (reset_tab) g_downloads_active = 0;

    for (int i = 0; i < g_file_count; i++) {
        /* gclite display copies (Utilities switch ON): CAT_ folders and
           CAT_ categories show as plain names; OFF, everything stays raw.
           Navigation always uses the raw e->name / e->url. */
        if (g_gclite_on && g_files[i].is_dir)
            store_cat_name(g_files[i].name, sh_name[i], sizeof(sh_name[i]));
        else
            snprintf(sh_name[i], sizeof(sh_name[i]), "%.*s",
                     (int)sizeof(sh_name[i]) - 1, g_files[i].name);
        if (g_gclite_on)
            store_cat_name(g_files[i].category, sh_cat[i], sizeof(sh_cat[i]));
        else
            snprintf(sh_cat[i], sizeof(sh_cat[i]), "%.*s",
                     (int)sizeof(sh_cat[i]) - 1, g_files[i].category);
        sh[i].name = sh_name[i];
        sh[i].category = sh_cat[i];
        sh[i].platform = g_files[i].platform;
        sh[i].size = g_files[i].size;
        sh[i].is_dir = g_files[i].is_dir;
    }
    for (int i = 0; i < g_dl_count; i++) {
        dl[i].name = g_dl[i].name;
        dl[i].category = "";
        dl[i].platform = "";
        dl[i].size = g_dl[i].size;
        dl[i].is_dir = 0;
    }

    int n = bm_build(g_cf_items, MAX_FILES, sh, g_file_count,
                     dl, g_dl_count,
                     PSP_SKIP_CATEGORIES, PSP_SKIP_COUNT,
                     NULL, 0, "PSP", g_downloads_active,
                     g_tabs, &g_tab_count, g_cf_map);

    g_downloads_tab = (g_tab_count > 0 &&
                       strcmp(g_tabs[g_tab_count - 1], BM_DOWNLOADS_LABEL) == 0)
                      ? g_tab_count - 1 : -1;

    if (coverflow_set_items(&g_cf, g_cf_items, (size_t)n) != 0) return;
    if (reset_tab) {
        const char *tabs[MAX_TABS];
        for (int i = 0; i < g_tab_count; i++) tabs[i] = g_tabs[i];
        coverflow_set_categories(&g_cf, tabs, (size_t)g_tab_count);
    }
}

/* Row progress for the Downloads list: finished files are full bars; a
   .part row is 0..99 when the current listing knows the file's total
   size (browser downloads keep the share name), else -1 for a dim
   "paused" bar. */
static int dl_progress(const DlEntry *d) {
    if (!d->is_part) return 100;
    for (int f = 0; f < g_file_count; f++) {
        if (g_files[f].is_dir || g_files[f].size <= 0) continue;
        if (strcmp(g_files[f].name, d->name) != 0) continue;
        long long pct = d->size * 100 / g_files[f].size;
        return pct > 99 ? 99 : (int)(pct < 0 ? 0 : pct);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* gclite categories popup (PSP only)                                  */
/*                                                                     */
/* game-categories-lite is a CFW VSH plugin that turns memory-stick    */
/* folders named CAT_ / CAT_05... into XMB categories; it cannot be    */
/* linked into a homebrew app, so the integration honors its naming    */
/* convention (browser_fill strips the prefix for display) and, when   */
/* vsh.txt registers the plugin, mirrors the XMB category list as a    */
/* popup the browser opens with START.                                 */
/* ------------------------------------------------------------------ */

static int g_gclite = 0;                  /* category_lite.prx seen    */
static int g_menu_open = 0;
static int g_menu_sel = 0;

/* ms0:/seplugins/vsh.txt lists one plugin path per line; scan it for
   "category_lite" (case-insensitive, the gclite prx name). The 12-byte
   carry between read chunks keeps a match that straddles a boundary. */
static int gclite_installed(void) {
    static const char pat[] = "category_lite";
    const size_t plen = sizeof(pat) - 1;
    SceUID f = sceIoOpen("ms0:/seplugins/vsh.txt", PSP_O_RDONLY, 0777);
    if (f < 0) return 0;
    char buf[256];
    size_t fill = 0;
    int hit = 0;
    while (!hit) {
        int n = (int)sceIoRead(f, buf + fill, sizeof(buf) - fill);
        if (n <= 0) break;
        size_t len = fill + (size_t)n;
        for (size_t i = 0; i + plen <= len; i++)
            if (strncasecmp(buf + i, pat, plen) == 0) { hit = 1; break; }
        if ((size_t)n < sizeof(buf) - fill) break;   /* EOF reached */
        fill = plen - 1;
        memmove(buf, buf + len - fill, fill);
    }
    sceIoClose(f);
    return hit;
}

/* The pixel Utilities page (Settings -> Triangle): the switch home for
   options that do not fit the debug-screen Settings editor. Currently one
   switch - the whole gclite feature (CAT_ name stripping + START
   categories popup). Detection runs on entry, so installing the plugin
   and reopening the page picks it up. Returns 2 to fall back into the
   debug Settings screen, or 0/1 (changed?) to leave for the browser. */
static int utilities_view(void) {
    unsigned prev = 0;
    int changed = 0;
    const int installed = gclite_installed();

    /* Same entry-key consumption as settings_view: the Triangle that
       walked in must not read as "back to Settings". */
    { SceCtrlData p0; sceCtrlReadBufferPositive(&p0, 1); prev = p0.Buttons; }
    for (;;) {
        store_gui_utilities(g_gclite_on, installed);

        unsigned pressed;
        do { sceKernelDelayThread(16000); }
        while (get_pressed(&pressed, &prev) != 0);

        if (pressed & PSP_CTRL_CROSS) {
            g_gclite_on = !g_gclite_on;
            changed = 1;
        } else if (pressed & PSP_CTRL_SELECT) {
            return 2;                          /* back to Settings */
        } else if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) {
            if (changed) save_settings();      /* O and START both apply */
            return changed;                    /* and leave for the flow */
        }
    }
}

/* Would this share entry appear under a tab at all? Same rules the model
   applies: never hidden names, never the other console's files, never a
   skipped category (dirs are covers too, so they count). */
static int menu_shown(const FileEntry *e) {
    if (!e->name[0] || e->name[0] == '.') return 0;
    if (!e->is_dir && !store_platform_ok(e->name, e->platform, "PSP")) return 0;
    if (e->category[0])
        for (int s = 0; s < PSP_SKIP_COUNT; s++)
            if (strcasecmp(e->category, PSP_SKIP_CATEGORIES[s]) == 0) return 0;
    return 1;
}

/* Items behind one tab: cat NULL/"" counts the All tab, otherwise the
   gclite-stripped category must match (as the tab rail shows it). */
static int menu_share_count(const char *cat) {
    int c = 0;
    for (int i = 0; i < g_file_count; i++) {
        const FileEntry *e = &g_files[i];
        if (!menu_shown(e)) continue;
        if (cat && cat[0]) {
            char nm[COVERFLOW_DYNAMIC_CATEGORY_LEN];
            store_cat_name(e->category, nm, sizeof(nm));
            if (!e->category[0] || strcasecmp(nm, cat) != 0) continue;
        }
        c++;
    }
    return c;
}

/* Fill the popup from the tab rail (browser_fill built it stripped):
   row i mirrors tab i, so All first, categories in rail order, and the
   Downloads row when the rail has one. Mock-tile ids are the labels, so
   each category keeps its own placeholder hue across frames. */
static void browser_menu_build(store_menu_t *m, store_menu_row_t *rows,
                               int cap) {
    int n = 0;
    snprintf(rows[n].name, sizeof(rows[n].name), "All");
    snprintf(rows[n].id, sizeof(rows[n].id), "All");
    rows[n].count = menu_share_count(NULL);
    n++;
    for (int i = 1; i < g_tab_count && n < cap; i++) {
        const char *t = g_tabs[i];
        if (!t[0]) continue;                      /* only All carries "" */
        snprintf(rows[n].name, sizeof(rows[n].name), "%s", t);
        snprintf(rows[n].id, sizeof(rows[n].id), "%s", t);
        rows[n].count = (strcmp(t, BM_DOWNLOADS_LABEL) == 0)
                            ? g_dl_count : menu_share_count(t);
        n++;
    }
    if (g_menu_sel >= n) g_menu_sel = n - 1;
    if (g_menu_sel < 0) g_menu_sel = 0;
    m->rows = rows;
    m->n = n;
    m->sel = g_menu_sel;
}

/* Jump the flow to tab `row` (row indexes g_tabs 1:1), swapping the
   item source the same way the tab rail's Cross/L1/R1 handling does. */
static void menu_jump(int row) {
    const int want_dl = (g_downloads_tab >= 0 && row == g_downloads_tab);
    if (want_dl != g_downloads_active) {
        g_downloads_active = want_dl;
        if (want_dl) scan_downloads();
    }
    g_cf.category = (coverflow_category_t)row;
    browser_fill(0);
}

static coverflow_key_t browser_key(unsigned pressed) {
    if (pressed & PSP_CTRL_LEFT)      return COVERFLOW_KEY_LEFT;
    if (pressed & PSP_CTRL_RIGHT)     return COVERFLOW_KEY_RIGHT;
    if (pressed & PSP_CTRL_UP)        return COVERFLOW_KEY_UP;
    if (pressed & PSP_CTRL_DOWN)      return COVERFLOW_KEY_DOWN;
    if (pressed & PSP_CTRL_LTRIGGER)  return COVERFLOW_KEY_SHOULDER_L;
    if (pressed & PSP_CTRL_RTRIGGER)  return COVERFLOW_KEY_SHOULDER_R;
    if (pressed & PSP_CTRL_SQUARE)    return COVERFLOW_KEY_TAB;   /* flow/grid */
    if (pressed & PSP_CTRL_CROSS)     return COVERFLOW_KEY_ENTER;
    if (pressed & PSP_CTRL_CIRCLE)    return COVERFLOW_KEY_BACK;
    return COVERFLOW_KEY_NONE;
}

static int browser(void) {
    draw_splash("Fetching listing from NAS...");

    if (fetch_listing() != 0 || g_file_count == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Could not reach %s", g_nas_base);
        store_gui_status("PS Linker", msg, "Press any button to retry");
        wait_button();
        return -1;
    }

    if (coverflow_init(&g_cf, g_cf_items, 0, SCREEN_COLS, SCREEN_ROWS, 0) != 0) {
        store_gui_status("PS Linker", "Out of memory.", "Exiting...");
        sceKernelDelayThread(3000000);
        return 0;
    }
    scan_downloads();
    browser_fill(1);
    g_gclite = gclite_installed();   /* START's popup is an install-gated extra */

    int rc = 0;
    unsigned prev = 0;
    while (1) {
        const char *ids[MAX_FILES];
        const char *descs[MAX_FILES];
        int pct[MAX_FILES];
        for (size_t i = 0; i < g_cf.item_count; i++) {
            ids[i]   = g_downloads_active ? g_dl[g_cf_map[i]].name
                                          : g_files[g_cf_map[i]].url;
            descs[i] = "";
            pct[i]   = g_downloads_active ? dl_progress(&g_dl[g_cf_map[i]])
                                          : -1;
        }
        store_menu_t menu = { 0 };
        store_menu_row_t menu_rows[MAX_TABS];
        store_menu_t *pmenu = NULL;
        if (g_menu_open) {
            browser_menu_build(&menu, menu_rows, MAX_TABS);
            pmenu = &menu;
        }
        store_gui_browse(&g_cf, ids, descs, g_cwd,
                         g_downloads_active ? pct : NULL, pmenu);

        unsigned pressed;
        do { sceKernelDelayThread(16000); }
        while (get_pressed(&pressed, &prev) != 0);

        /* Screens that own the pad (and partly the display) themselves. */
        if (pressed & PSP_CTRL_TRIANGLE) {
            g_menu_open = 0;
            gui_to_debug();          /* store_run brings the GU back up */
            store_run(g_nas_base);
            gui_init();
            pad_clear(&prev);
            continue;                /* the listing may predate an install */
        }
        if (pressed & PSP_CTRL_SELECT) {
            g_menu_open = 0;
            int go = 1;                      /* 1 Settings, 2 Utilities, 0 out */
            int u_changed = 0;
            while (go) {
                if (go == 1) {               /* debug-screen URL editor */
                    gui_to_debug();
                    go = settings_view();
                    gui_init();              /* GU back up for pixel views */
                } else {
                    const int ur = utilities_view();  /* pixel switch page */
                    if (ur == 2) go = 1;              /* fall back to Settings */
                    else { if (ur) u_changed = 1; go = 0; }
                }
            }
            pad_clear(&prev);
            if (u_changed) browser_fill(1);  /* folder names just changed */
            continue;                /* URL may have changed: redraw */
        }

        /* The gclite popup owns the pad while it is open. */
        if (g_menu_open && menu.n > 0) {
            if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START))
                g_menu_open = 0;
            else if (pressed & PSP_CTRL_UP)
                g_menu_sel = (g_menu_sel + menu.n - 1) % menu.n;
            else if (pressed & PSP_CTRL_DOWN)
                g_menu_sel = (g_menu_sel + 1) % menu.n;
            else if (pressed & PSP_CTRL_CROSS) {
                menu_jump(g_menu_sel);   /* jumps to that tab, like the rail */
                g_menu_open = 0;
            }
            continue;
        }
        if (pressed & PSP_CTRL_START) {
            if (g_gclite && g_gclite_on) {     /* mirror the XMB's category bar */
                g_menu_open = 1;
                g_menu_sel = (int)g_cf.category;   /* light up the current tab */
            }
            continue;
        }

        coverflow_key_t key = browser_key(pressed);
        if (key == COVERFLOW_KEY_NONE) continue;

        const int tab_before = (int)g_cf.category;
        coverflow_action_t action;
        coverflow_handle_key(&g_cf, key, &action);

        /* Tab rail crossed into/out of Downloads: swap the item source. */
        if ((int)g_cf.category != tab_before) {
            const int want_dl = (g_downloads_tab >= 0 &&
                                 (int)g_cf.category == g_downloads_tab);
            if (want_dl != g_downloads_active) {
                g_downloads_active = want_dl;
                if (want_dl) scan_downloads();
                browser_fill(0);
            }
        }

        if (action == COVERFLOW_ACTION_SELECT) {
            const coverflow_item_t *cur = coverflow_current(&g_cf);
            if (!cur) continue;
            const int slot = (int)(cur - g_cf_items);

            if (g_downloads_active) {
                char msg[96];
                const DlEntry *d = &g_dl[g_cf_map[slot]];
                if (d->is_part)
                    snprintf(msg, sizeof(msg), "%s stopped early - download it again to resume", d->name);
                else
                    snprintf(msg, sizeof(msg), "%s is already on this card", d->name);
                store_gui_status("PS Linker", msg, "Press any button to continue");
                wait_button();
                continue;
            }

            FileEntry *e = &g_files[g_cf_map[slot]];
            if (e->is_dir) {
                draw_splash("Loading directory...");
                char saved[MAX_PATH_LEN];
                snprintf(saved, sizeof(saved), "%s", g_cwd);
                if (cwd_enter(e->name) != 0 || fetch_listing() != 0 ||
                    g_file_count == 0) {
                    snprintf(g_cwd, sizeof(g_cwd), "%s", saved);  /* rollback */
                    store_gui_status("PS Linker", "Could not open folder.",
                                     "Press any button to continue");
                    wait_button();
                    continue;
                }
                browser_fill(1);
                continue;
            }

            char dest[MAX_PATH_LEN + 32];
            snprintf(dest, sizeof(dest), DEST_DIR "%s", e->name);
            int drc = download_file(e->url, dest, e->name);
            char msg[128];
            if (drc == 0) snprintf(msg, sizeof(msg), "Done: %s", e->name);
            else          snprintf(msg, sizeof(msg), "Failed (%d): %s", drc, e->name);
            store_gui_status("PS Linker", msg,
                             drc == 0 ? "Download complete" : "Download failed");
            wait_button();
            scan_downloads();        /* the new file belongs in the tab */
            if (g_downloads_active) browser_fill(0);
        } else if (action == COVERFLOW_ACTION_BACK) {
            if (g_downloads_active) {          /* leave the Downloads view */
                g_cf.category = COVERFLOW_CAT_ALL;
                browser_fill(1);
                continue;
            }
            if (g_cwd[0]) {                    /* up one level */
                draw_splash("Going up...");
                cwd_up();
                if (fetch_listing() != 0) {
                    store_gui_status("PS Linker",
                                     "Could not reload parent directory.",
                                     "Press any button to continue");
                    wait_button();
                }
                browser_fill(1);
                continue;
            }
            break;                             /* at root: quit (rc stays 0) */
        }
    }

    coverflow_shutdown(&g_cf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Network init                                                        */
/* ------------------------------------------------------------------ */

static int wait_for_ip(unsigned int timeout_us) {
    unsigned int start = sceKernelGetSystemTimeWide();
    SceNetApctlInfo info;
    do {
        if (sceNetApctlGetInfo(1, &info) == 0) return 0;
        sceKernelDelayThread(200 * 1000);
    } while (sceKernelGetSystemTimeWide() - start < timeout_us);
    return -2;
}

static int net_init(void) {
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) != 0) return -1;
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_INET)   != 0) return -2;
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_HTTP)   != 0) return -3;
    if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) != 0) return -4;
    if (sceNetInetInit()    != 0) return -5;
    if (sceNetApctlInit(0x10000, 48) != 0) return -6;

    draw_splash("Connecting to Wi-Fi (profile 1)...");
    if (sceNetApctlConnect(1) != 0) return -7;   /* Wi-Fi profile 1 */
    if (wait_for_ip(20 * 1000 * 1000) != 0) return -8;

    /* libhttp needs its own memory pool before any template is created. */
    if (sceHttpInit(20000) != 0) return -9;

    SceNetApctlInfo info;
    if (sceNetApctlGetInfo(1, &info) == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Connected: %s (%s)", info.ssid, info.ip);
        store_gui_status("PS Linker", msg, NULL);
        sceKernelDelayThread(800 * 1000);
    }
    return 0;
}

int main(void) {
    pspDebugScreenInit();
    load_settings();
    sceIoMkdir(DEST_DIR, 0777);   /* also holds settings.txt */

    if (gui_init() != 0) {
        pspDebugScreenClear();
        set_style(&(Style){ COL_BG, COL_ERR });
        field(0, 4, SCREEN_COLS, &(Style){ COL_BG, COL_ERR },
              "GUI init failed - cannot start the pixel UI.");
        sceKernelDelayThread(5 * 1000 * 1000);
        sceKernelExitGame();
        return 1;
    }
    draw_splash("Starting...");

    int rc = net_init();
    if (rc != 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Network init failed (%d). Check Wi-Fi.", rc);
        store_gui_status("PS Linker", msg, "Exiting...");
        sceKernelDelayThread(5 * 1000 * 1000);
        gui_term();
        sceKernelExitGame();
        return 1;
    }

    while (browser() != 0) {
        /* browser() returned an error: retry after any button press */
        wait_button();
    }

    draw_splash("Goodbye!");
    sceKernelDelayThread(2 * 1000 * 1000);
    gui_term();
    sceKernelExitGame();
    return 0;
}
