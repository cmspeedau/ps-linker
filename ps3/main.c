/*
 * PS Linker - PS3 NAS Downloader (PSL1GHT port of the PSP version)
 *
 * Same Media Go-inspired workflow as the PSP build: initialize the
 * network, fetch the NAS HTTP share's directory listing, pick a file
 * with the D-pad, and download it to the HDD. Triangle opens the
 * Store (Cover Flow over catalog.json) like the PSP build does.
 *
 * Files land in /dev_hdd0/NASDL/<filename>.
 *
 * Build with the PSL1GHT toolchain: make
 * Requires CFW/HEN to install and run the generated package.
 *
 * Controls:  Up/Down = move, Cross = download / enter directory,
 *            Circle = parent directory (quit at root),
 *            Select = Settings (edit NAS URL),
 *            Triangle = Store
 * Settings:  Left/Right = pick char, Up/Down = jump a row,
 *            Cross = insert, Circle = delete char,
 *            Start = save & exit, Select = cancel
 *
 * Output goes to the RSX pixel display (ps3/gfx.c: CPU-drawn frames
 * flipped through two RSX buffers, so the TV shows the app). If the
 * display cannot be initialised - or the build passes
 * -DNASDL_TEXT_UI - every screen falls back to the ANSI-colored TTY
 * (netdebug) instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>

#include <net/net.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>

#include "http.h"
#include "resume.h"
#include "store.h"
#include "ui.h"

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

/* Default base URL of the NAS share; editable at runtime via Settings
   and persisted to SETTINGS_PATH. */
#define DEFAULT_NAS_URL "http://192.168.1.50:8000"

/* Destination directory on the PS3 HDD */
#define DEST_DIR  "/dev_hdd0/NASDL/"

/* Settings file (plain text: one line, the base URL) */
#define SETTINGS_PATH DEST_DIR "settings.txt"

#define MAX_FILES     128
#define MAX_PATH_LEN  256
#define MAX_URL_LEN   128

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static FileEntry g_files[MAX_FILES];
static int g_file_count = 0;

/* Current directory, relative to g_nas_base ("" = root).
   Always either empty or terminated with '/'. */
static char g_cwd[MAX_PATH_LEN] = "";

/* Base URL of the NAS share, e.g. "http://192.168.1.50:8000".
   Editable in the Settings view; lives in RAM after load. */
static char g_nas_base[MAX_URL_LEN] = DEFAULT_NAS_URL;

/* sysutil asked us to exit (XMB quit / PS menu). */
static volatile int g_sys_quit = 0;

static void sysutil_cb(u64 status, u64 param, void *usrdata)
{
    (void)param; (void)usrdata;
    if ((status & SYSUTIL_EXIT_GAME))
        g_sys_quit = 1;
}

/* Settings character strip shared with the backends (the header pins
   the length, so the picker slots line up across files). */
const char SETTINGS_CHARS[] =
    "abcdefghijklmnopqrstuvwxyz0123456789-.:/_";
typedef char settings_chars_len_must_match_ui_h
    [(sizeof(SETTINGS_CHARS) - 1 == SETTINGS_NCHARS) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* Settings persistence                                                */
/* ------------------------------------------------------------------ */

/* Load the saved URL. Silently keeps the default if absent/invalid. */
static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char buf[MAX_URL_LEN];
    if (fgets(buf, sizeof(buf), f)) {
        /* trim trailing whitespace/newlines */
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                         buf[n-1] == ' '))
            buf[--n] = '\0';
        if (n > 7 && strncmp(buf, "http://", 7) == 0)
            snprintf(g_nas_base, sizeof(g_nas_base), "%s", buf);
    }
    fclose(f);
}

static int save_settings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return -1;
    int ok = fputs(g_nas_base, f) >= 0;
    fclose(f);
    return ok ? 0 : -2;
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                         */
/* ------------------------------------------------------------------ */

/* Build "BASE[/cwd][/name]". name may be NULL or end in '/'. */
static void build_url(char *dst, size_t len, const char *name)
{
    char joined[MAX_PATH_LEN * 2];
    int n = snprintf(joined, sizeof(joined), "%s/%s/%s", g_nas_base,
                     g_cwd, name ? name : "");
    size_t i, o = 0;
    if (len == 0) return;
    if (n < 0 || (size_t)n >= sizeof(joined)) { dst[0] = '\0'; return; }
    for (i = 0; joined[i] != '\0' && o + 1 < len; i++) {
        if (joined[i] != '/' || o == 0 || dst[o - 1] != '/' ||
            (o >= 2 && dst[o - 2] == ':')) dst[o++] = joined[i];
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Directory-listing parser                                            */
/* Works with python3 -m http.server, nginx autoindex, Synology shares */
/* ------------------------------------------------------------------ */

static void url_decode(char *dst, const char *src, size_t dstlen)
{
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

/* Read key="value" from a directory-listing anchor's tail (the text after
   href's closing quote, within that tag only). Returns 1 when found. */
static int attr_at(const char *start, const char *end, const char *key,
                   char *out, size_t cap)
{
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

static int add_file(const char *href, long long size, const char *category)
{
    if (href[0] == '\0') return 0;
    if (strncmp(href, "../", 3) == 0) return 0;   /* parent link      */
    if (href[0] == '/') return 0;                 /* server-absolute  */

    int is_dir = 0;
    size_t hl = strlen(href);
    if (hl > 0 && href[hl - 1] == '/') {          /* directory entry  */
        is_dir = 1;
        hl--;
    }
    if (hl == 0 || memchr(href, '/', hl) != NULL) return 0; /* nested/abs */

    if (g_file_count >= MAX_FILES) return 0;
    FileEntry *e = &g_files[g_file_count];

    url_decode(e->name, href, sizeof(e->name));
    size_t nl = strlen(e->name);
    if (nl > 0 && e->name[nl - 1] == '/') e->name[--nl] = '\0';
    if (nl == 0) return 0;                        /* nothing left to show */

    /* Full URL: BASE / cwd / href */
    char full[MAX_PATH_LEN];
    build_url(full, sizeof(full), href);
    if (is_dir && full[strlen(full) - 1] != '/')
        strncat(full, "/", sizeof(full) - strlen(full) - 1);
    snprintf(e->url, sizeof(e->url), "%s", full);
    e->is_dir = is_dir;
    e->size = size;
    snprintf(e->category, sizeof(e->category), "%s", category ? category : "");

    g_file_count++;
    return 1;
}

static int parse_listing(const char *html)
{
    g_file_count = 0;
    const char *p = html;
    while (g_file_count < MAX_FILES && (p = strstr(p, "href=\"")) != NULL) {
        p += 6;                              /* skip href=" */
        const char *end = strchr(p, '"');
        if (!end) break;
        int len = end - p;
        if (len > 0 && len < 512) {
            char href[512];
            memcpy(href, p, len);
            href[len] = '\0';
            /* The rest of this anchor may carry data-size / data-cat; the
               search stops at the next tag so no neighbouring entry leaks
               in, and a share without the attributes just parses as before. */
            const char *tag_end = strchr(end, '<');
            if (!tag_end) tag_end = end;
            char sizebuf[24], cat[64];
            long long size = -1;
            cat[0] = '\0';
            if (attr_at(end + 1, tag_end, "data-size", sizebuf,
                        sizeof sizebuf))
                size = strtoll(sizebuf, NULL, 10);
            attr_at(end + 1, tag_end, "data-cat", cat, sizeof cat);
            add_file(href, size, cat);
        }
        p = end + 1;
    }
    return g_file_count;
}

static int listing_cb(const char *body, void *user)
{
    (void)user;
    parse_listing(body);
    return 0;
}

/* Fetch the listing for the current g_cwd. */
static int fetch_listing(void)
{
    char url[MAX_PATH_LEN + 64];
    build_url(url, sizeof(url), NULL);
    return http_get_body(url, listing_cb, NULL);
}

/* ------------------------------------------------------------------ */
/* Download with a live progress frame (ui_download backend decides    */
/* how much redrawing is worth it).                                    */
/* ------------------------------------------------------------------ */

static char g_dl_name[MAX_PATH_LEN];
static char g_dl_path[MAX_PATH_LEN + 32];
static int g_dl_last_pct = -999;
static long g_dl_last_total = 0;

static void download_progress(int pct, long total)
{
    /* Only redraw when the percentage moves (or with unknown size,
       every megabyte) - the pixel present is a full frame copy. */
    if (pct == g_dl_last_pct &&
        !(pct < 0 && total - g_dl_last_total >= 1024 * 1024))
        return;
    g_dl_last_pct = pct;
    g_dl_last_total = total;
    ui_download(g_dl_name, g_dl_path, pct, total);
}

static int download_file(const char *url, const char *dest_path,
                         const char *shown_name)
{
    snprintf(g_dl_name, sizeof(g_dl_name), "%s", shown_name);
    snprintf(g_dl_path, sizeof(g_dl_path), "%s", dest_path);
    g_dl_last_pct = -999;
    g_dl_last_total = 0;
    ui_download(g_dl_name, g_dl_path, -1, 0);
    return ps3_resume_download(url, dest_path, download_progress);
}

/* ------------------------------------------------------------------ */
/* Pad input                                                           */
/* ------------------------------------------------------------------ */

/* PSL1GHT exposes pad buttons as individual bitfields on padData; the
   UI works with a simple edge-detected bitmask instead (bits in store.h
   so the Store's key mapper shares them). */

static unsigned pad_buttons(const padData *pd)
{
    unsigned b = 0;
    if (pd->BTN_LEFT)     b |= PAD_BTN_LEFT;
    if (pd->BTN_DOWN)     b |= PAD_BTN_DOWN;
    if (pd->BTN_RIGHT)    b |= PAD_BTN_RIGHT;
    if (pd->BTN_UP)       b |= PAD_BTN_UP;
    if (pd->BTN_START)    b |= PAD_BTN_START;
    if (pd->BTN_SELECT)   b |= PAD_BTN_SELECT;
    if (pd->BTN_CROSS)    b |= PAD_BTN_CROSS;
    if (pd->BTN_CIRCLE)   b |= PAD_BTN_CIRCLE;
    if (pd->BTN_L1)       b |= PAD_BTN_L1;
    if (pd->BTN_R1)       b |= PAD_BTN_R1;
    if (pd->BTN_TRIANGLE) b |= PAD_BTN_TRIANGLE;
    if (pd->BTN_SQUARE)   b |= PAD_BTN_SQUARE;
    return b;
}

/* Edge-detected button read; returns 0 and fills *pressed on a press
   edge, -1 when nothing new was pressed (including bare releases -
   otherwise the lift of the button that triggered a modal would read
   as "a press" and dismiss it on its own), -2 on a sysutil exit ask. */
static int get_pressed(unsigned *pressed)
{
    static unsigned prev = 0;
    padInfo pad_inf;
    padData pd;

    sysUtilCheckCallback();
    if (g_sys_quit) return -2;

    if (ioPadGetInfo(&pad_inf) != 0) return -1;
    if (!pad_inf.status[0]) return -1;
    if (ioPadGetData(0, &pd) != 0) return -1;

    unsigned buttons = pad_buttons(&pd);
    *pressed = buttons & ~prev;
    prev = buttons;
    return *pressed ? 0 : -1;
}

/* Block until a press (0) or a system exit request (<0). */
static int wait_press(unsigned *pressed)
{
    int r;
    do {
        usleep(16000);
        r = get_pressed(pressed);
    } while (r == -1);
    return r;
}

static int wait_button(void)
{
    unsigned dummy;
    return wait_press(&dummy);
}

/* ------------------------------------------------------------------ */
/* Directory navigation                                                */
/* ------------------------------------------------------------------ */

/* Append "name/" to g_cwd. Returns 0 or <0 if the path would overflow. */
static int cwd_enter(const char *name)
{
    size_t cl = strlen(g_cwd);
    while (*name == '/') name++;
    size_t nl = strlen(name);
    if (cl + nl + 2 > sizeof(g_cwd)) return -1;
    memcpy(g_cwd + cl, name, nl);
    g_cwd[cl + nl] = '/';
    g_cwd[cl + nl + 1] = '\0';
    return 0;
}

/* Remove the last path component from g_cwd ("" if already at root).
   g_cwd always keeps a trailing slash when non-empty, so drop that one
   first, then cut at the parent's slash: "a/b/" -> "a/", "a/" -> "". */
static void cwd_up(void)
{
    size_t l = strlen(g_cwd);
    if (l == 0) return;
    if (g_cwd[l - 1] == '/') g_cwd[--l] = '\0';
    char *slash = strrchr(g_cwd, '/');
    if (slash)
        slash[1] = '\0';              /* keep the parent's slash */
    else
        g_cwd[0] = '\0';              /* one component -> root */
}

/* ------------------------------------------------------------------ */
/* Settings view: edit the NAS URL with an on-screen character picker  */
/* ------------------------------------------------------------------ */

/* Returns 0 to keep browsing, <0 for a system quit. */
static int settings_view(void)
{
    const int nslots = SETTINGS_NCHARS + 2;   /* chars, delete, save-exit */
    const int cols = ui_settings_cols();
    int pick = 0;

    while (1) {
        ui_settings(g_nas_base, pick, nslots);

        unsigned pressed;
        int prc;
        do { usleep(16000); prc = get_pressed(&pressed); } while (prc == -1);
        if (prc < 0) return -1;

        if (pressed & PAD_BTN_RIGHT) {
            if (pick < nslots - 1) pick++;
        } else if (pressed & PAD_BTN_LEFT) {
            if (pick > 0) pick--;
        } else if (pressed & PAD_BTN_DOWN) {
            if (pick + cols < nslots) pick += cols;
            else pick = nslots - 1;
        } else if (pressed & PAD_BTN_UP) {
            if (pick >= cols) pick -= cols;
        } else if (pressed & PAD_BTN_CROSS) {
            if (pick < SETTINGS_NCHARS) {
                size_t l = strlen(g_nas_base);
                if (l + 1 < sizeof(g_nas_base)) {
                    g_nas_base[l] = SETTINGS_CHARS[pick];
                    g_nas_base[l + 1] = '\0';
                }
            } else if (pick == nslots - 2) {     /* delete last char */
                size_t l = strlen(g_nas_base);
                if (l > 0) g_nas_base[l - 1] = '\0';
            } else {                             /* save & exit */
                save_settings();
                return 0;
            }
        } else if (pressed & PAD_BTN_CIRCLE) {
            size_t l = strlen(g_nas_base);    /* delete last char */
            if (l > 0) g_nas_base[l - 1] = '\0';
        } else if (pressed & PAD_BTN_START) {
            save_settings();
            return 0;
        } else if (pressed & PAD_BTN_SELECT) {
            return 0;                           /* cancel */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Browser                                                             */
/* ------------------------------------------------------------------ */

/* Returns:  0 = quit at root, -1 = retry the listing, -2 = system quit */
static int browser(void)
{
    ui_splash("Fetching listing from NAS...");

    if (fetch_listing() != 0 || g_file_count == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Could not reach %s", g_nas_base);
        ui_error(msg, "Check the NAS URL in Settings (Select).");
        if (wait_button() < 0) return -2;
        return -1;
    }

    int sel = 0, top = 0;
    while (1) {
        const int rows = ui_browser_rows();
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;

        char status[64];
        snprintf(status, sizeof(status), "%d entries - %d selected",
                 g_file_count, sel + 1);
        ui_browser(g_cwd, g_files, g_file_count, sel, top, status);

        unsigned pressed;
        int prc;
        do { usleep(16000); prc = get_pressed(&pressed); } while (prc == -1);
        if (prc < 0) return -2;

        if (pressed & PAD_BTN_SELECT) {
            if (settings_view() < 0) return -2;
            continue;   /* URL may have changed: redraw */
        } else if (pressed & PAD_BTN_TRIANGLE) {
            store_run(g_nas_base, get_pressed);
            continue;   /* the listing may predate an install */
        } else if (pressed & PAD_BTN_DOWN) {
            sel = (sel + 1) % g_file_count;
        } else if (pressed & PAD_BTN_UP) {
            sel = (sel - 1 + g_file_count) % g_file_count;
        } else if (pressed & PAD_BTN_CROSS) {
            FileEntry *e = &g_files[sel];

            if (e->is_dir) {
                /* Enter the directory and reload its listing. */
                ui_splash("Loading directory...");
                if (cwd_enter(e->name) != 0 || fetch_listing() != 0) {
                    cwd_up();   /* roll back on failure */
                    ui_error("Could not open directory.", "");
                    if (wait_button() < 0) return -2;
                    continue;
                }
                sel = 0; top = 0;
                continue;
            }

            char dest[MAX_PATH_LEN + 32];
            snprintf(dest, sizeof(dest), DEST_DIR "%s", e->name);

            int rc = download_file(e->url, dest, e->name);
            char msg[256];
            if (rc == 0) snprintf(msg, sizeof(msg), "Done: %s", e->name);
            else         snprintf(msg, sizeof(msg), "Failed (%d): %s", rc, e->name);
            ui_result(rc == 0, msg);
            if (wait_button() < 0) return -2;
        } else if (pressed & PAD_BTN_CIRCLE) {
            if (g_cwd[0] != '\0') {
                /* Go up one level and reload. */
                ui_splash("Going up...");
                cwd_up();
                if (fetch_listing() != 0) {
                    ui_error("Could not reload parent directory.", "");
                    if (wait_button() < 0) return -2;
                }
                sel = 0; top = 0;
                continue;
            }
            return 0;   /* at root: quit */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(void)
{
    ioPadInit(1);
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);

    ui_init();          /* RSX pixel when the display allows, else TTY */
    ui_splash("Starting...");

    if (netInitialize() < 0) {   /* uses XMB network settings */
        ui_error("Network init failed.", "Check XMB network settings.");
        sleep(5);
        ui_exit();
        ioPadEnd();
        return 1;
    }

    mkdir(DEST_DIR, 0777);   /* also holds settings.txt */
    load_settings();

    int rc;
    do { rc = browser(); } while (rc == -1);

    ui_splash("Goodbye!");
    sleep(2);

    netDeinitialize();
    ui_exit();
    ioPadEnd();
    return 0;
}
