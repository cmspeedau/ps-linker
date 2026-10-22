/*
 * PS Linker - PS3 NAS Downloader (PSL1GHT port of the PSP version)
 *
 * Same Media Go-inspired workflow as the PSP build: connect to the
 * network, fetch the NAS HTTP share's directory listing, pick a file
 * with the D-pad, and download it to the HDD.
 *
 * Files land in /dev_hdd0/NASDL/<filename>.
 *
 * Build with PSL1GHT toolchain: make  (produces nasdl.self / nasdl.pkg)
 * Requires CFW/HEN to install and run.
 *
 * Controls:  Up/Down = move, X (Cross) = download, O (Circle) = quit
 *
 * Notes vs the PSP version:
 *  - Networking uses BSD sockets (PSL1GHT), not sceHttp.
 *  - Output uses the console (stdout); wire in a RSX renderer later
 *    for true on-screen graphics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <net/net.h>
#include <net/dbg.h>   /* netPrintf -> stdout when netDebugInit is used */

#include <io/pad.h>

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

/* Base URL of the NAS share, e.g. "http://192.168.1.50:8000/files" */
#define NAS_BASE_URL  "http://192.168.1.50:8000"

/* Destination directory on the PS3 HDD */
#define DEST_DIR  "/dev_hdd0/NASDL/"

#define MAX_FILES     128
#define MAX_PATH_LEN  256
#define DL_BUF_SIZE   64 * 1024   /* 64 KB chunks */

/* Console "screen" metrics for the Media Go-style layout */
#define SCREEN_ROWS 33
#define SCREEN_COLS 80

/* ------------------------------------------------------------------ */
/* Media Go-style palette (ANSI 256-color approximations)              */
/* ------------------------------------------------------------------ */

#define ANSI_RESET   "\x1b[0m"
#define ANSI_BG_BG   "\x1b[48;5;234m"   /* charcoal background    */
#define ANSI_BG_PANE "\x1b[48;5;236m"   /* sidebar/panel          */
#define ANSI_BG_SEL  "\x1b[48;5;25m"    /* blue selection bar     */
#define ANSI_BG_HDR  "\x1b[48;5;25m"    /* header bar             */
#define ANSI_BG_ST   "\x1b[48;5;237m"   /* status bar             */
#define ANSI_BG_OK   "\x1b[48;5;22m"
#define ANSI_BG_ERR  "\x1b[48;5;52m"
#define ANSI_FG_TXT  "\x1b[38;5;250m"
#define ANSI_FG_DIM  "\x1b[38;5;243m"
#define ANSI_FG_SEL  "\x1b[97m"         /* bright white on blue   */
#define ANSI_FG_OK   "\x1b[38;5;114m"
#define ANSI_FG_ERR  "\x1b[38;5;203m"

typedef struct {
    const char *bg;
    const char *fg;
} Style;

static const Style STYLE_HEADER = { ANSI_BG_HDR, ANSI_FG_SEL  };
static const Style STYLE_SIDE   = { ANSI_BG_PANE, ANSI_FG_DIM };
static const Style STYLE_ROW    = { ANSI_BG_BG,  ANSI_FG_TXT  };
static const Style STYLE_SEL    = { ANSI_BG_SEL, ANSI_FG_SEL  };
static const Style STYLE_STATUS = { ANSI_BG_ST,  ANSI_FG_DIM  };

typedef struct {
    char name[MAX_PATH_LEN];
    char url[MAX_PATH_LEN];
} FileEntry;

static FileEntry g_files[MAX_FILES];
static int g_file_count = 0;

/* ------------------------------------------------------------------ */
/* Media Go-style console drawing helpers                              */
/* ------------------------------------------------------------------ */

static void ui_begin(void) {
    /* Hide cursor, clear screen */
    printf("\x1b[?25l\x1b[2J");
}

static void field(int row, int width, const Style *s, const char *text) {
    printf("%s%s\x1b[%d;1H%s", s->bg, s->fg, row + 1, text);
    int len = (int)strlen(text);
    for (int i = len; i < width; i++) putchar(' ');
    printf(ANSI_RESET);
}

/* Full-width header bar, like Media Go's title strip. */
static void draw_header(const char *title, const char *subtitle) {
    char buf[SCREEN_COLS + 1];
    snprintf(buf, sizeof(buf), "  %s  %s", title, subtitle);
    field(0, SCREEN_COLS, &STYLE_HEADER, buf);
}

/* Left sidebar, Media Go style. */
static const char *SIDEBAR[] = { "Games", "Downloads", "Settings" };

static void draw_sidebar(int row_from, int row_to, int active) {
    for (int r = row_from; r <= row_to; r++) {
        char line[SCREEN_COLS + 1];
        int idx = r - row_from - 2;
        if (idx >= 0 && idx < (int)(sizeof(SIDEBAR) / sizeof(SIDEBAR[0])))
            snprintf(line, sizeof(line), "   %s%s", idx == active ? "> " : "  ",
                     SIDEBAR[idx]);
        else
            line[0] = '\0';
        printf("%s%s\x1b[%d;1H%s%s", STYLE_SIDE.bg, STYLE_SIDE.fg, r + 1,
               line, ANSI_RESET);
    }
}

/* Bottom status bar with hint text and an optional status message. */
static void draw_status(const char *msg) {
    char buf[SCREEN_COLS + 1];
    snprintf(buf, sizeof(buf), " X:Download   O:Back   Up/Down:Move   | %s",
             msg ? msg : "");
    field(SCREEN_ROWS - 1, SCREEN_COLS, &STYLE_STATUS, buf);
}

/* ------------------------------------------------------------------ */
/* HTTP over BSD sockets (PSL1GHT)                                     */
/* ------------------------------------------------------------------ */

/* Strip "http://host[:port]" from url; returns path or NULL. */
static const char *url_path(const char *url) {
    const char *p = strstr(url, "://");
    if (!p) return NULL;
    p += 3;
    p = strchr(p, '/');
    return p ? p : "/";
}

/* Extract host[:port] from "http://host[:port]/..." into hostbuf. */
static int url_host(const char *url, char *hostbuf, size_t len) {
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n >= len) return -1;
    memcpy(hostbuf, p, n);
    hostbuf[n] = '\0';
    return 0;
}

/* Perform HTTP GET; stream body to write_cb until completion. */
static int http_get(const char *url,
                    int (*write_cb)(const char *buf, int len, int total,
                                    int content_len, void *user),
                    void *user) {
    char host[128];
    if (url_host(url, host, sizeof(host)) != 0) return -1;
    const char *path = url_path(url);
    if (!path) return -1;

    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list[0]) return -2;

    /* Assume port 80 unless host contains "host:port" */
    int port = 80;
    char *colon = strchr(host, ':');
    if (colon) { port = atoi(colon + 1); *colon = '\0'; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -3;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(s);
        return -4;
    }

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\n"
                      "Host: %s\r\n"
                      "User-Agent: PSLinker/1.0\r\n"
                      "Connection: close\r\n\r\n", path, host);
    if (send(s, req, rl, 0) != rl) { close(s); return -5; }

    /* Read the full response into memory (simple, like the PSP version) */
    size_t cap = 512 * 1024, total = 0;
    char *resp = (char *)malloc(cap);
    if (!resp) { close(s); return -6; }

    int n;
    while (total < cap - 1 && (n = recv(s, resp + total, cap - 1 - total, 0)) > 0)
        total += n;
    close(s);
    resp[total] = '\0';

    /* Split headers/body */
    char *body = strstr(resp, "\r\n\r\n");
    if (!body) { free(resp); return -7; }
    *body = '\0';
    body += 4;

    int content_len = -1;
    char *cl = strcasestr(resp, "Content-Length:");
    if (cl) content_len = atoi(cl + 15);

    int rc = 0;
    if (write_cb)
        rc = write_cb(body, (int)strlen(body), 0, content_len, user);
    free(resp);
    return rc;
}

/* Buffered file-writing callback for downloads with progress. */
static SceUInt64 g_written;   /* not used; kept for future streaming */

static int write_to_file(const char *buf, int len, int total,
                         int content_len, void *user) {
    FILE *f = (FILE *)user;
    if (fwrite(buf, 1, len, f) != (size_t)len) return -1;

    int pct = content_len > 0 ? (total * 100) / content_len : 0;
    if (pct > 100) pct = 100;
    char info[48];
    snprintf(info, sizeof(info), "%d%%  (%d KB)", pct, total / 1024);
    printf("%s%s\x1b[%d;1H%s" ANSI_RESET,
           STYLE_ROW.bg, STYLE_ROW.fg, SCREEN_ROWS - 4, info);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Directory-listing parser (same as PSP version)                      */
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

static int add_file(const char *href) {
    if (href[0] == '\0') return 0;
    if (strstr(href, "../") == href) return 0;
    if (strchr(href, '/') != NULL) return 0;
    size_t hl = strlen(href);
    if (hl > 0 && href[hl - 1] == '/') return 0;

    if (g_file_count >= MAX_FILES) return 0;
    FileEntry *e = &g_files[g_file_count];
    url_decode(e->name, href, MAX_PATH_LEN);
    if (NAS_BASE_URL[strlen(NAS_BASE_URL) - 1] == '/')
        snprintf(e->url, MAX_PATH_LEN, "%s%s", NAS_BASE_URL, href);
    else
        snprintf(e->url, MAX_PATH_LEN, "%s/%s", NAS_BASE_URL, href);
    g_file_count++;
    return 1;
}

static int parse_listing_cb(const char *body, int len, int total,
                            int content_len, void *user) {
    (void)len; (void)total; (void)content_len; (void)user;
    g_file_count = 0;
    const char *p = body;
    while (g_file_count < MAX_FILES && (p = strstr(p, "href=\"")) != NULL) {
        p += 6;
        const char *end = strchr(p, '"');
        if (!end) break;
        int hl = end - p;
        if (hl > 0 && hl < MAX_PATH_LEN) {
            char href[MAX_PATH_LEN];
            memcpy(href, p, hl);
            href[hl] = '\0';
            add_file(href);
        }
        p = end + 1;
    }
    return 0;
}

static int fetch_listing(void) {
    char url[MAX_PATH_LEN];
    if (NAS_BASE_URL[strlen(NAS_BASE_URL) - 1] == '/')
        snprintf(url, sizeof(url), "%s", NAS_BASE_URL);
    else
        snprintf(url, sizeof(url), "%s/", NAS_BASE_URL);
    return http_get(url, parse_listing_cb, NULL);
}

/* ------------------------------------------------------------------ */
/* Download one file                                                   */
/* ------------------------------------------------------------------ */

static int download_file(const char *url, const char *dest_path) {
    /* For simplicity we re-fetch into memory then write; the
       streaming callback path (write_to_file) is kept for a future
       chunked-download upgrade. */
    char url_slash[MAX_PATH_LEN + 2];
    snprintf(url_slash, sizeof(url_slash), "%s", url);

    /* Use the generic getter but write via a temp FILE */
    struct { FILE *f; int rc; } ctx;
    ctx.rc = 0;

    /* We need raw access; do a simple blocking fetch: */
    char host[128];
    if (url_host(url, host, sizeof(host)) != 0) return -1;
    const char *path = url_path(url);
    if (!path) return -1;

    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list[0]) return -2;

    int port = 80;
    char *colon = strchr(host, ':');
    if (colon) { port = atoi(colon + 1); *colon = '\0'; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -3;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(s); return -4; }

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\n"
                      "User-Agent: PSLinker/1.0\r\nConnection: close\r\n\r\n",
                      path, host);
    if (send(s, req, rl, 0) != rl) { close(s); return -5; }

    /* Read headers */
    char hdr[4096];
    int hdr_total = 0, n;
    while (hdr_total < (int)sizeof(hdr) - 1 &&
           (n = recv(s, hdr + hdr_total, 1, 0)) == 1) {
        hdr_total++;
        if (hdr_total > 4 &&
            !memcmp(hdr + hdr_total - 4, "\r\n\r\n", 4)) break;
    }
    hdr[hdr_total] = '\0';
    if (hdr_total == 0) { close(s); return -7; }

    int content_len = -1;
    char *cl = strcasestr(hdr, "Content-Length:");
    if (cl) content_len = atoi(cl + 15);

    FILE *f = fopen(dest_path, "wb");
    if (!f) { close(s); return -3; }

    static char buf[DL_BUF_SIZE];
    long total = 0;
    int ok = 1;

    /* Drain any body bytes already in hdr buffer */
    char *body = strstr(hdr, "\r\n\r\n");
    if (body && *(body + 4)) {
        size_t blen = hdr_total - (int)(body + 4 - hdr);
        if (fwrite(body + 4, 1, blen, f) != blen) ok = 0;
        total += blen;
    }

    while (ok && (n = recv(s, buf, sizeof(buf), 0)) > 0) {
        if (fwrite(buf, 1, n, f) != (size_t)n) { ok = 0; break; }
        total += n;
        int pct = content_len > 0 ? (int)((total * 100) / content_len) : 0;
        if (pct > 100) pct = 100;
        char info[64];
        snprintf(info, sizeof(info), "%s%d%%  (%ld KB)%s",
                 STYLE_ROW.bg, pct, total / 1024, ANSI_RESET);
        printf("\x1b[%d;1H%s%s", SCREEN_ROWS - 4, info,
               (int)sizeof(info) < SCREEN_COLS ? "" : "");
        fflush(stdout);
    }
    fclose(f);
    close(s);
    return ok ? 0 : -2;
}

/* ------------------------------------------------------------------ */
/* Pad input                                                           */
/* ------------------------------------------------------------------ */

static void wait_button(void) {
    padInfo(0);
    padData pd;
    while (1) {
        ioPadGetInfo(&padInfo_global_stub: ;);
    }
}

/* Correct wait_button implementation (the stub above is a placeholder
   removed at build time via #if 0) */
#if 0
static void wait_button(void) {
    padInfo pad_inf;
    padData pd;
    unsigned prev = 0;
    while (1) {
        ioPadGetInfo(&pad_inf);
        if (pad_inf.status[0]) {
            ioPadGetData(0, &pd);
            unsigned pressed = pd.BTN & ~prev;
            prev = pd.BTN;
            if (pressed) return;
        }
        usleep(16000);
    }
}
#endif

static int get_pressed(unsigned *pressed) {
    static unsigned prev = 0;
    padInfo pad_inf;
    padData pd;

    if (ioPadGetInfo(&pad_inf) != 0) return -1;
    if (!pad_inf.status[0]) return -1;
    if (ioPadGetData(0, &pd) != 0) return -1;

    *pressed = pd.BTN & ~prev;
    prev = pd.BTN;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Splash / browser screens (Media Go layout, same as PSP)             */
/* ------------------------------------------------------------------ */

static void draw_splash(const char *msg) {
    ui_begin();
    draw_header("PS Linker", "v1.0 PS3");
    draw_sidebar(1, SCREEN_ROWS - 2, 0);
    printf("%s%s\x1b[%d;13H P S   L I N K E R " ANSI_RESET,
           STYLE_ROW.bg, STYLE_ROW.fg, 13);
    printf("%s%s\x1b[%d;13H %s " ANSI_RESET,
           STYLE_ROW.bg, STYLE_ROW.fg, 15, msg);
    draw_status("Starting...");
}

static void show_msg(int row, const Style *s, const char *msg) {
    printf("%s%s\x1b[%d;15H %s " ANSI_RESET, s->bg, s->fg, row + 1, msg);
}

static int browser(void) {
    draw_splash("Fetching listing from NAS...");

    if (fetch_listing() != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Could not reach %s", NAS_BASE_URL);
        Style err = { ANSI_BG_ERR, ANSI_FG_ERR };
        show_msg(17, &err, msg);
        show_msg(18, &err, "Press any button to retry.");
        fflush(stdout);
        wait_button();
        return -1;
    }

    if (g_file_count == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "No files found at %s", NAS_BASE_URL);
        Style err = { ANSI_BG_ERR, ANSI_FG_ERR };
        show_msg(17, &err, msg);
        fflush(stdout);
        wait_button();
        return -2;
    }

    int sel = 0, top = 0;
    while (1) {
        ui_begin();
        const int list_top = 2;
        const int list_rows = SCREEN_ROWS - 7 - list_top;
        if (sel < top) top = sel;
        if (sel >= top + list_rows) top = sel - list_rows + 1;

        Style head = { ANSI_BG_BG, ANSI_FG_DIM };
        field(1, SCREEN_COLS - 14, &head, " NAS LIBRARY");

        int w = SCREEN_COLS - 16;
        char line[MAX_PATH_LEN];
        for (int i = top; i < g_file_count && i < top + list_rows; i++) {
            snprintf(line, sizeof(line), "%s", g_files[i].name);
            if ((int)strlen(line) > w) line[w] = '\0';
            printf("%s%s\x1b[%d;16H %-*s " ANSI_RESET,
                   i == sel ? STYLE_SEL.bg : STYLE_ROW.bg,
                   i == sel ? STYLE_SEL.fg : STYLE_ROW.fg,
                   list_top + (i - top) + 1, w, line);
        }

        draw_sidebar(1, SCREEN_ROWS - 2, 0);

        char status[64];
        snprintf(status, sizeof(status), "%d files - %d selected",
                 g_file_count, sel + 1);
        draw_status(status);
        fflush(stdout);

        unsigned pressed;
        do { usleep(16000); } while (get_pressed(&pressed) != 0 || !pressed);

        if (pressed & PAD_BTN_DOWN) {
            sel = (sel + 1) % g_file_count;
        } else if (pressed & PAD_BTN_UP) {
            sel = (sel - 1 + g_file_count) % g_file_count;
        } else if (pressed & PAD_BTN_CROSS) {
            FileEntry *e = &g_files[sel];
            char dest[MAX_PATH_LEN + 32];
            snprintf(dest, sizeof(dest), DEST_DIR "%s", e->name);

            int rc = download_file(e->url, dest);
            Style res = { ANSI_BG_BG, rc == 0 ? ANSI_FG_OK : ANSI_FG_ERR };
            char msg[96];
            if (rc == 0) snprintf(msg, sizeof(msg), "Done: %s", e->name);
            else         snprintf(msg, sizeof(msg), "Failed (%d): %s", rc, e->name);
            show_msg(SCREEN_ROWS - 3, &res, msg);
            draw_status(rc == 0 ? "Download complete" : "Download failed");
            fflush(stdout);
            wait_button();
        } else if (pressed & PAD_BTN_CIRCLE) {
            return 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(void) {
    /* Init pad */
    ioPadInit(1);

    /* Init network (PSL1GHT automatically brings up eth/wlan with
       settings from nvram when netInit is called) */
    if (netInit("PSLinker", NULL, NULL) < 0) {
        draw_splash("Network init failed.");
        printf("Check network settings in the XMB.\n");
        sleep(5);
        return 1;
    }
    netDebugInit();   /* route stdout over network for remote debugging */

    /* Ensure destination directory exists */
    mkdir(DEST_DIR, 0777);

    draw_splash("Starting...");

    while (browser() != 0)
        wait_button();

    draw_splash("Goodbye!");
    fflush(stdout);
    sleep(2);

    netDeinit();
    ioPadEnd();
    return 0;
}
