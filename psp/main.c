/*
 * PS Linker - PSP NAS Downloader with interactive browser
 *
 * Connects to Wi-Fi, browses the NAS HTTP share's directory listing,
 * lets you pick a file with the D-pad, and downloads it to the
 * Memory Stick. Files land in ms0:/PSP/GAME/NASDL/<filename>.
 *
 * UI is styled after Sony's Media Go: dark charcoal panels, a left
 * sidebar, blue selection highlight, and an accent-colored header and
 * status bar.
 *
 * Build with pspdev toolchain: make
 * Requires CFW (ARK-4 / PRO) to run.
 *
 * Controls in browser:  Up/Down = move, X = download (or enter dir),
 *                       O = parent dir (quit at root)
 * Controls in settings: Left/Right = pick char, X = insert/OK,
 *                       O = delete char, Start = save & back,
 *                       Select = back without saving
 */

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspwlan.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psphttp.h>
#include <pspiofilemgr.h>
#include <psputility_netmodules.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "resume.h"
#include "store.h"

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

/* Destination directory on the Memory Stick */
#define DEST_DIR "ms0:/PSP/GAME/NASDL/"

/* Settings file (plain text: one line, the base URL) */
#define SETTINGS_PATH "ms0:/PSP/GAME/NASDL/settings.txt"

#define MAX_FILES     128
#define MAX_PATH_LEN  256
#define MAX_URL_LEN   128
#define DL_BUF_SIZE   64 * 1024   /* 64 KB chunks */

/* ------------------------------------------------------------------ */
/* Media Go-style palette                                              */
/* (PSP debug screen colors are 0xAABBGGRR)                            */
/* ------------------------------------------------------------------ */

#define COL_BG        0xFF1D1D1D   /* charcoal background            */
#define COL_PANEL     0xFF262626   /* sidebar / panel background     */
#define COL_HEADER    0xFFCC7A00   /* accent bar (Media Go blue-ish) */
#define COL_SELECT    0xFF714706   /* selection highlight            */
#define COL_TEXT      0xFFCCCCCC   /* normal text                    */
#define COL_TEXT_DIM  0xFF808080   /* muted text                     */
#define COL_TEXT_SEL  0xFFFFFFFF   /* text on selection              */
#define COL_STATUS    0xFF333333   /* status bar background          */
#define COL_OK        0xFF3EC13E   /* success green                  */
#define COL_ERR       0xFF3D3DD5   /* error red                      */
#define COL_PROGRESS  0xFFCC7A00   /* progress fill                  */

typedef struct {
    u32 bg, fg;
} Style;

static const Style STYLE_HEADER = { COL_HEADER,   0xFF000000 };
static const Style STYLE_SIDE   = { COL_PANEL,    COL_TEXT_DIM };
static const Style STYLE_ROW    = { COL_BG,       COL_TEXT };
static const Style STYLE_SEL    = { COL_SELECT,   COL_TEXT_SEL };
static const Style STYLE_STATUS = { COL_STATUS,   COL_TEXT_DIM };

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

/* Left sidebar, Media Go style. active = currently highlighted item. */
static const char *SIDEBAR[] = { "Games", "Downloads", "Settings" };

static void draw_sidebar(int row_from, int row_to, int active) {
    for (int r = row_from; r <= row_to; r++) {
        pspDebugScreenSetXY(0, r);
        set_style(&STYLE_SIDE);
        pad(14);
        int idx = r - row_from - 2;
        if (idx >= 0 && idx < (int)(sizeof(SIDEBAR) / sizeof(SIDEBAR[0]))) {
            pspDebugScreenSetXY(2, r);
            printf("%s%s", idx == active ? "> " : "  ", SIDEBAR[idx]);
        }
    }
}

/* Bottom status bar with hint text and an optional status message. */
static void draw_status(const char *msg) {
    char buf[SCREEN_COLS + 1];
    /* Leave room for the fixed hint prefix; long messages are clipped. */
    snprintf(buf, sizeof(buf), " X:Dl/Enter  O:Up  Select:Settings | %.28s",
             msg ? msg : "");
    field(0, SCREEN_ROWS - 1, SCREEN_COLS, &STYLE_STATUS, buf);
}

/* ------------------------------------------------------------------ */
/* Settings: runtime NAS URL, persisted to the Memory Stick            */
/* ------------------------------------------------------------------ */

/* Base URL of the NAS share, e.g. "http://192.168.1.50:8000".
   Editable in the Settings view; lives in RAM after load. */
static char g_nas_base[MAX_URL_LEN] = DEFAULT_NAS_URL;

/* Load the saved URL. Silently keeps the default if absent/invalid. */
static void load_settings(void) {
    SceUID fd = sceIoOpen(SETTINGS_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    char buf[MAX_URL_LEN];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    /* trim trailing whitespace/newlines */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    if (n > 7 && strncmp(buf, "http://", 7) == 0)
        snprintf(g_nas_base, sizeof(g_nas_base), "%s", buf);
}

static int save_settings(void) {
    SceUID fd = sceIoOpen(SETTINGS_PATH,
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;
    int len = (int)strlen(g_nas_base);
    int w = sceIoWrite(fd, g_nas_base, len);
    sceIoClose(fd);
    return (w == len) ? 0 : -2;
}

/* ------------------------------------------------------------------ */
/* File listing state                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[MAX_PATH_LEN];   /* display name (decoded)            */
    char url[MAX_PATH_LEN];    /* absolute URL for fetch/download   */
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

static int add_file(const char *href) {
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

    /* Display name: strip trailing slash for dirs */
    char display[MAX_PATH_LEN];
    url_decode(display, href, sizeof(display));
    if (is_dir && hl > 0) display[hl - 1] = '\0';

    if (g_file_count >= MAX_FILES) return 0;

    FileEntry *e = &g_files[g_file_count];
    if (build_url(href, e->url, MAX_PATH_LEN) != 0) return 0;
    snprintf(e->name, MAX_PATH_LEN, is_dir ? "[%s]" : "%s", display);
    e->is_dir = is_dir;
    g_file_count++;
    return 1;
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
            add_file(href);
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
/* Download with Media Go-style progress bar                           */
/* Downloading goes through resume.c, which sends Range headers and    */
/* continues an interrupted transfer from the partial file on disk.    */
/* ------------------------------------------------------------------ */

/* Progress callback from resume.c: redraw the bar in place.
   pct is 0-100, or -1 when the total size is unknown. */
static void download_progress(int pct, long total) {
    const int bar_w = SCREEN_COLS - 16;
    int fill = (pct >= 0 && pct <= 100) ? (pct * bar_w) / 100 : 0;
    char info[48];
    if (pct >= 0)
        snprintf(info, sizeof(info), "%d%%  (%ld KB)", pct, total / 1024);
    else
        snprintf(info, sizeof(info), "(%ld KB)", total / 1024);

    Style st = { COL_BG, COL_TEXT };
    field(0, SCREEN_ROWS - 6, SCREEN_COLS, &st, "Downloading...");

    pspDebugScreenSetXY(0, SCREEN_ROWS - 5);
    set_style(&STYLE_ROW);
    pad(bar_w);                             /* empty track */
    pspDebugScreenSetXY(0, SCREEN_ROWS - 5);
    Style sp = { COL_PROGRESS, COL_PROGRESS };
    field(0, SCREEN_ROWS - 5, fill, &sp, "");

    Style si = { COL_BG, COL_TEXT_DIM };
    field(bar_w + 1, SCREEN_ROWS - 5, 15, &si, info);
}

static int download_file(const char *url, const char *dest_path) {
    draw_header("PS Linker", "Downloading...");
    return psp_resume_download(url, dest_path, download_progress);
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
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

/* Splash screen shown while connecting, Media Go style. */
static void draw_splash(const char *msg) {
    set_style(&(Style){ COL_BG, COL_TEXT });
    pspDebugScreenClear();
    draw_header("PS Linker", "v1.3");
    draw_sidebar(1, SCREEN_ROWS - 2, 0);
    Style big = { COL_BG, COL_TEXT };
    field(16, 12, 50, &big, "P S   L I N K E R");
    field(16, 14, 50, &big, msg);
    draw_status("Starting...");
}

/* ------------------------------------------------------------------ */
/* Settings view: edit the NAS URL with an on-screen character picker  */
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

    /* Visible window of the strip: pick stays inside [view, view+W) */
    const int strip_w = SCREEN_COLS - 16;      /* list panel width */
    const int picker_row = SCREEN_ROWS - 4;

    while (1) {
        pspDebugScreenClear();
        draw_header("PS Linker", "Settings");
        draw_sidebar(1, SCREEN_ROWS - 2, 2);   /* highlight "Settings" */

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
              " <>:pick  X:insert  O:delete  Start:save  Select:cancel");

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

        draw_status("Settings");

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
        }
    }
}

/* ------------------------------------------------------------------ */
/* Browser                                                             */
/* ------------------------------------------------------------------ */

static int browser(void) {
    draw_splash("Fetching listing from NAS...");

    if (fetch_listing() != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Could not reach %s", g_nas_base);
        Style err = { COL_BG, COL_ERR };
        field(16, 16, 50, &err, msg);
        field(16, 17, 50, &err, "Press any button to retry.");
        wait_button();
        return -1;
    }

    if (g_file_count == 0) {
        Style err = { COL_BG, COL_ERR };
        char msg[96];
        snprintf(msg, sizeof(msg), "No files found at %s", g_nas_base);
        field(16, 16, 50, &err, msg);
        wait_button();
        return -2;
    }

    int sel = 0, top = 0;
    unsigned prev = 0;
    while (1) {
        pspDebugScreenClear();

        /* Layout: rows 1..30 list area, sidebar cols 0..13 */
        const int list_top = 2;
        const int list_rows = SCREEN_ROWS - 7 - list_top;
        if (sel < top) top = sel;
        if (sel >= top + list_rows) top = sel - list_rows + 1;

        /* List panel background + header row (shows current dir) */
        Style head = { COL_BG, COL_TEXT_DIM };
        char headtxt[MAX_PATH_LEN + 16];
        snprintf(headtxt, sizeof(headtxt), " NAS LIBRARY: %s",
                 g_cwd[0] ? g_cwd : "/");
        int hw = SCREEN_COLS - 14;
        if ((int)strlen(headtxt) > hw) headtxt[hw] = '\0';
        field(14, 1, hw, &head, headtxt);

        for (int i = top; i < g_file_count && i < top + list_rows; i++) {
            char line[MAX_PATH_LEN];
            snprintf(line, sizeof(line), "%s", g_files[i].name);
            /* Truncate to the list panel width */
            int w = SCREEN_COLS - 16;
            if ((int)strlen(line) > w) line[w] = '\0';
            field(15, list_top + (i - top), w,
                  i == sel ? &STYLE_SEL : &STYLE_ROW, line);
        }

        draw_sidebar(1, SCREEN_ROWS - 2, 0);

        char status[64];
        snprintf(status, sizeof(status), "%d entries - %d selected",
                 g_file_count, sel + 1);
        draw_status(status);

        /* Simple edge-detect: poll until a new button press appears */
        unsigned pressed;
        do {
            sceKernelDelayThread(16000);
        } while (get_pressed(&pressed, &prev) != 0);

        if (pressed & PSP_CTRL_TRIANGLE) {
            store_run(g_nas_base);
            /* The Store has its own button handling; wait until the pad is
               clear before reading again. Without this the O that closed the
               Store is still down here, gets seen as a fresh press, and hits
               the "up one level" branch below - which at the share root quits
               the app instead of returning to the browser. */
            SceCtrlData released;
            do {
                sceCtrlReadBufferPositive(&released, 1);
                sceKernelDelayThread(16000);
            } while (released.Buttons);
            prev = 0;
        } else if (pressed & PSP_CTRL_SELECT) {
            settings_view();
        } else if (pressed & PSP_CTRL_DOWN) {
            sel = (sel + 1) % g_file_count;
        } else if (pressed & PSP_CTRL_UP) {
            sel = (sel - 1 + g_file_count) % g_file_count;
        } else if (pressed & PSP_CTRL_CROSS) {
            FileEntry *e = &g_files[sel];

            if (e->is_dir) {
                /* Enter subdirectory: rollback g_cwd if the fetch fails */
                char saved[MAX_PATH_LEN];
                snprintf(saved, sizeof(saved), "%s", g_cwd);
                /* strip display name back to raw href form */
                char raw[MAX_PATH_LEN];
                snprintf(raw, sizeof(raw), "%s", e->name + 1);   /* skip '[' */
                raw[strlen(raw) - 1] = '\0';                     /* drop ']' */
                if (cwd_enter(raw) != 0 || fetch_listing() != 0 ||
                    g_file_count == 0) {
                    snprintf(g_cwd, sizeof(g_cwd), "%s", saved);
                    Style err = { COL_BG, COL_ERR };
                    field(16, SCREEN_ROWS - 4, 50, &err,
                          "Could not open folder.");
                    wait_button();
                }
                sel = 0; top = 0;
            } else {
                char dest[MAX_PATH_LEN + 32];
                snprintf(dest, sizeof(dest), DEST_DIR "%s", e->name);
                int rc = download_file(e->url, dest);

                Style res = { COL_BG, rc == 0 ? COL_OK : COL_ERR };
                char msg[96];
                if (rc == 0)
                    snprintf(msg, sizeof(msg), "Done: %s", e->name);
                else
                    snprintf(msg, sizeof(msg), "Failed (%d): %s", rc, e->name);
                field(14, SCREEN_ROWS - 3, SCREEN_COLS - 14, &res, msg);
                draw_status(rc == 0 ? "Download complete" : "Download failed");
                wait_button();
            }
        } else if (pressed & PSP_CTRL_CIRCLE) {
            if (g_cwd[0]) {                  /* up one level */
                cwd_up();
                if (fetch_listing() != 0 || g_file_count == 0) {
                    /* Nothing above: treat as quit */
                    return 0;
                }
                sel = 0; top = 0;
            } else {
                return 0;
            }
        }
    }
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
        Style ok = { COL_BG, COL_OK };
        char msg[96];
        snprintf(msg, sizeof(msg), "Connected: %s (%s)", info.ssid, info.ip);
        field(16, 16, 50, &ok, msg);
        sceKernelDelayThread(800 * 1000);
    }
    return 0;
}

int main(void) {
    pspDebugScreenInit();
    set_style(&(Style){ COL_BG, COL_TEXT });
    pspDebugScreenClear();
    draw_splash("Starting...");

    load_settings();
    sceIoMkdir(DEST_DIR, 0777);   /* also holds settings.txt */

    int rc = net_init();
    if (rc != 0) {
        Style err = { COL_BG, COL_ERR };
        char msg[80];
        snprintf(msg, sizeof(msg), "Network init failed (%d). Check Wi-Fi.", rc);
        field(16, 17, 50, &err, msg);
        sceKernelDelayThread(5 * 1000 * 1000);
        sceKernelExitGame();
        return 1;
    }

    while (browser() != 0) {
        /* browser() returned an error: retry after any button press */
        wait_button();
    }

    draw_splash("Goodbye!");
    sceKernelDelayThread(2 * 1000 * 1000);
    sceKernelExitGame();
    return 0;
}
