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
 * Controls in browser:  Up/Down = move, X = download, O = quit
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
#include <string.h>
#include <stdlib.h>

#define printf pspDebugScreenPrintf
#define SCREEN_ROWS 33
#define SCREEN_COLS 68

PSP_MODULE_INFO("PSLinker", 0, 1, 2);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

/* Base URL of the NAS share, e.g. "http://192.168.1.50:8000/files" */
#define NAS_BASE_URL "http://192.168.1.50:8000"

/* Destination directory on the Memory Stick */
#define DEST_DIR "ms0:/PSP/GAME/NASDL/"

#define MAX_FILES     128
#define MAX_PATH_LEN  256
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
static const Style STYLE_SIDE_H = { COL_PANEL,    0xFFFFFFFF };
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
    snprintf(buf, sizeof(buf), " X:Download   O:Back   Up/Down:Move   | %s",
             msg ? msg : "");
    field(0, SCREEN_ROWS - 1, SCREEN_COLS, &STYLE_STATUS, buf);
}

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
        int req = sceHttpCreateRequestWithURL(conn, PSP_HTTP_METHOD_GET, url, 0);
        if (req >= 0 && sceHttpSendRequest(req, NULL, 0) >= 0) {
            int clen = 0;
            sceHttpGetContentLength(req, &clen);
            if (clen <= 0) clen = 512 * 1024;   /* assume up to 512 KB */

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

static int add_file(const char *href) {
    /* Skip parent links and anything that isn't a file in this dir */
    if (href[0] == '\0') return 0;
    if (strstr(href, "../") == href) return 0;
    if (strchr(href, '/') != NULL)    return 0;   /* nested dir or absolute */
    size_t hl = strlen(href);
    if (hl > 0 && href[hl - 1] == '/') return 0;

    size_t used = strlen(NAS_BASE_URL) + 1 + strlen(href);
    if (used >= MAX_PATH_LEN || g_file_count >= MAX_FILES) return 0;

    FileEntry *e = &g_files[g_file_count];
    url_decode(e->name, href, MAX_PATH_LEN);
    if (NAS_BASE_URL[strlen(NAS_BASE_URL) - 1] == '/')
        snprintf(e->url, MAX_PATH_LEN, "%s%s", NAS_BASE_URL, href);
    else
        snprintf(e->url, MAX_PATH_LEN, "%s/%s", NAS_BASE_URL, href);
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
/* Download with Media Go-style progress bar                           */
/* ------------------------------------------------------------------ */

static int download_file(const char *url, const char *dest_path) {
    int tpl = sceHttpCreateTemplate("PSLinker", 1, 1);
    if (tpl < 0) return -1;

    int status = -1;
    int conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    if (conn >= 0) {
        int req = sceHttpCreateRequestWithURL(conn, PSP_HTTP_METHOD_GET, url, 0);
        if (req >= 0 && sceHttpSendRequest(req, NULL, 0) >= 0) {
            int clen = 0;
            sceHttpGetContentLength(req, &clen);

            SceUID fd = sceIoOpen(dest_path,
                                  PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (fd >= 0) {
                static char buf[DL_BUF_SIZE];
                int n, total = 0, ok = 1;
                char label[64];
                snprintf(label, sizeof(label), "Saving %s", dest_path);
                draw_header("PS Linker", "Downloading...");
                while ((n = sceHttpReadData(req, buf, sizeof(buf))) > 0) {
                    if (sceIoWrite(fd, buf, n) != n) { ok = 0; break; }
                    total += n;

                    /* Progress bar: filled track on a fixed row */
                    int pct = clen > 0 ? (total * 100) / clen : 0;
                    if (pct > 100) pct = 100;
                    int bar_w = SCREEN_COLS - 16;
                    int fill = clen > 0 ? (total * bar_w) / clen : 0;
                    char info[48];
                    snprintf(info, sizeof(info), "%d%%  (%d KB)", pct, total / 1024);

                    pspDebugScreenSetXY(0, SCREEN_ROWS - 6);
                    Style st = { COL_BG, COL_TEXT };
                    field(0, SCREEN_ROWS - 6, SCREEN_COLS, &st, label);

                    pspDebugScreenSetXY(0, SCREEN_ROWS - 5);
                    set_style(&STYLE_ROW);
                    pad(bar_w);                 /* empty track */
                    pspDebugScreenSetXY(0, SCREEN_ROWS - 5);
                    Style sp = { COL_PROGRESS, COL_PROGRESS };
                    field(0, SCREEN_ROWS - 5, fill, &sp, "");

                    Style si = { COL_BG, COL_TEXT_DIM };
                    field(bar_w + 1, SCREEN_ROWS - 5, 15, &si, info);
                }
                sceIoClose(fd);
                status = ok ? 0 : -2;
            } else {
                status = -3;   /* could not open destination file */
            }
        }
        if (req >= 0) sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
    }
    sceHttpDeleteTemplate(tpl);
    return status;
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void wait_button(void) {
    SceCtrlData pad;
    do { sceCtrlReadBufferPositive(&pad, 1); sceKernelDelayThread(16000); }
    while (pad.Buttons == 0);
}

/* Splash screen shown while connecting, Media Go style. */
static void draw_splash(const char *msg) {
    set_style(&(Style){ COL_BG, COL_TEXT });
    pspDebugScreenClear();
    draw_header("PS Linker", "v1.2");
    draw_sidebar(1, SCREEN_ROWS - 2, 0);
    Style big = { COL_BG, COL_TEXT };
    field(16, 12, 50, &big, "P S   L I N K E R");
    field(16, 14, 50, &big, msg);
    draw_status("Starting...");
}

static int browser(void) {
    draw_splash("Fetching listing from NAS...");

    char *html = fetch_to_memory(NAS_BASE_URL "/", NULL);
    if (!html) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Could not reach %s", NAS_BASE_URL);
        Style err = { COL_BG, COL_ERR };
        field(16, 16, 50, &err, msg);
        field(16, 17, 50, &err, "Press any button to retry.");
        wait_button();
        return -1;
    }
    parse_listing(html);
    free(html);

    if (g_file_count == 0) {
        Style err = { COL_BG, COL_ERR };
        char msg[96];
        snprintf(msg, sizeof(msg), "No files found at %s", NAS_BASE_URL);
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

        /* List panel background + header row */
        Style head = { COL_BG, COL_TEXT_DIM };
        field(14, 1, SCREEN_COLS - 14, &head, " NAS LIBRARY");

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
        snprintf(status, sizeof(status), "%d files - %d selected",
                 g_file_count, sel + 1);
        draw_status(status);

        /* Simple edge-detect: poll until a new button press appears */
        SceCtrlData pad;
        unsigned pressed = 0;
        do {
            sceCtrlReadBufferPositive(&pad, 1);
            pressed = pad.Buttons & ~prev;
            prev = pad.Buttons;
            if (!pressed) sceKernelDelayThread(16000);
        } while (!pressed);

        if (pressed & PSP_CTRL_DOWN) {
            sel = (sel + 1) % g_file_count;
        } else if (pressed & PSP_CTRL_UP) {
            sel = (sel - 1 + g_file_count) % g_file_count;
        } else if (pressed & PSP_CTRL_CROSS) {
            FileEntry *e = &g_files[sel];
            char dest[MAX_PATH_LEN + 32];
            snprintf(dest, sizeof(dest), DEST_DIR "%s", e->name);
            int rc = download_file(e->url, dest);

            Style res = { COL_BG, rc == 0 ? COL_OK : COL_ERR };
            char msg[96];
            snprintf(msg, sizeof(msg), rc == 0 ? "Done: %s" : "Failed (%d): %s",
                     rc == 0 ? 0 : rc, e->name);
            field(14, SCREEN_ROWS - 3, SCREEN_COLS - 14, &res, msg);
            draw_status(rc == 0 ? "Download complete" : "Download failed");
            wait_button();
        } else if (pressed & PSP_CTRL_CIRCLE) {
            return 0;
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
    if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) != 0) return -3;
    if (sceNetInetInit()    != 0) return -4;
    if (sceNetApctlInit(0x10000, 48) != 0) return -5;

    draw_splash("Connecting to Wi-Fi (profile 1)...");
    if (sceNetApctlConnect(1) != 0) return -6;   /* Wi-Fi profile 1 */
    if (wait_for_ip(20 * 1000 * 1000) != 0) return -7;

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

    sceIoMkdir(DEST_DIR, 0777);

    while (browser() != 0) {
        /* browser() returned an error: retry after any button press */
        wait_button();
    }

    draw_splash("Goodbye!");
    sceKernelDelayThread(2 * 1000 * 1000);
    sceKernelExitGame();
    return 0;
}
