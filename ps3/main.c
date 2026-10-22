/*
 * PS Linker - PS3 NAS Downloader (PSL1GHT port of the PSP version)
 *
 * Same Media Go-inspired workflow as the PSP build: initialize the
 * network, fetch the NAS HTTP share's directory listing, pick a file
 * with the D-pad, and download it to the HDD.
 *
 * Files land in /dev_hdd0/NASDL/<filename>.
 *
 * Build with the PSL1GHT toolchain: make
 * Requires CFW/HEN to install and run the generated package.
 *
 * Controls:  Up/Down = move, Cross = download, Circle = quit
 *
 * Differences from the PSP version:
 *  - Networking uses BSD sockets (PSL1GHT), not sceHttp.
 *  - Output is ANSI-colored terminal text (netdebug/TV console);
 *    wire in an RSX renderer later for true on-screen graphics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <net/net.h>
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
#define DL_BUF_SIZE   (64 * 1024)   /* 64 KB chunks */

/* Console "screen" metrics for the Media Go-style layout */
#define SCREEN_ROWS 33
#define SCREEN_COLS 80

/* ------------------------------------------------------------------ */
/* Media Go-style palette (ANSI 256-color approximations)              */
/* ------------------------------------------------------------------ */

#define ANSI_RESET   "\x1b[0m"
#define ANSI_BG_BG   "\x1b[48;5;234m"   /* charcoal background  */
#define ANSI_BG_PANE "\x1b[48;5;236m"   /* sidebar/panel        */
#define ANSI_BG_SEL  "\x1b[48;5;25m"    /* blue selection bar   */
#define ANSI_BG_HDR  "\x1b[48;5;25m"    /* header bar           */
#define ANSI_BG_ST   "\x1b[48;5;237m"   /* status bar           */
#define ANSI_FG_TXT  "\x1b[38;5;250m"
#define ANSI_FG_DIM  "\x1b[38;5;243m"
#define ANSI_FG_SEL  "\x1b[97m"         /* bright white on blue */
#define ANSI_FG_OK   "\x1b[38;5;114m"
#define ANSI_FG_ERR  "\x1b[38;5;203m"

typedef struct {
    const char *bg;
    const char *fg;
} Style;

static const Style STYLE_HEADER = { ANSI_BG_HDR,  ANSI_FG_SEL   };
static const Style STYLE_SIDE   = { ANSI_BG_PANE, ANSI_FG_DIM   };
static const Style STYLE_ROW    = { ANSI_BG_BG,   ANSI_FG_TXT   };
static const Style STYLE_SEL    = { ANSI_BG_SEL,  ANSI_FG_SEL   };
static const Style STYLE_STATUS = { ANSI_BG_ST,   ANSI_FG_DIM   };

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
    /* Hide cursor, clear screen, home cursor */
    printf("\x1b[?25l\x1b[2J\x1b[1;1H");
}

/* Print text left-aligned inside a width-w field on row (0-based). */
static void field(int row, int width, const Style *s, const char *text) {
    printf("%s%s\x1b[%d;1H%s", s->bg, s->fg, row + 1, text);
    int len = (int)strlen(text);
    for (int i = len; i < width; i++) putchar(' ');
    printf(ANSI_RESET);
    fflush(stdout);
}

/* Full-width header bar, like Media Go's title strip. */
static void draw_header(const char *title, const char *subtitle) {
    char buf[SCREEN_COLS + 1];
    snprintf(buf, sizeof(buf), "  %s  %s", title, subtitle);
    field(0, SCREEN_COLS, &STYLE_HEADER, buf);
}

/* Left sidebar, Media Go style. active = currently highlighted item. */
static const char *SIDEBAR[] = { "Games", "Downloads", "Settings" };

static void draw_sidebar(int row_from, int row_to, int active) {
    char line[32];
    for (int r = row_from; r <= row_to; r++) {
        int idx = r - row_from - 2;
        if (idx >= 0 && idx < (int)(sizeof(SIDEBAR) / sizeof(SIDEBAR[0])))
            snprintf(line, sizeof(line), "   %s%s",
                     idx == active ? "> " : "  ", SIDEBAR[idx]);
        else
            line[0] = '\0';
        printf("%s%s\x1b[%d;1H%-14s%s",
               STYLE_SIDE.bg, STYLE_SIDE.fg, r + 1, line, ANSI_RESET);
    }
    fflush(stdout);
}

/* Bottom status bar with hint text and an optional status message. */
static void draw_status(const char *msg) {
    char buf[SCREEN_COLS + 1];
    snprintf(buf, sizeof(buf), " X:Download   O:Back   Up/Down:Move   | %s",
             msg ? msg : "");
    field(SCREEN_ROWS - 1, SCREEN_COLS, &STYLE_STATUS, buf);
}

/* One-line message inside the list panel area. */
static void show_msg(int row, const Style *s, const char *msg) {
    printf("%s%s\x1b[%d;15H %s %s", s->bg, s->fg, row + 1, msg, ANSI_RESET);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* HTTP over BSD sockets (PSL1GHT)                                     */
/* ------------------------------------------------------------------ */

/* Strip scheme+host; returns path portion of url ("/" at minimum). */
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
    if (n == 0 || n >= len) return -1;
    memcpy(hostbuf, p, n);
    hostbuf[n] = '\0';
    return 0;
}

/* Resolve host (with optional :port), connect, return socket or <0. */
static int http_connect(const char *url) {
    char host[128];
    if (url_host(url, host, sizeof(host)) != 0) return -1;

    int port = 80;
    char *colon = strchr(host, ':');
    if (colon) { port = atoi(colon + 1); *colon = '\0'; }

    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list[0]) return -2;

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
    return s;
}

/*
 * Send a GET request. On success the socket is left positioned at
 * the START of the response (status line included), so callers can
 * parse headers and read the body themselves.
 */
static int http_send_get(const char *url) {
    const char *path = url_path(url);
    if (!path) return -1;

    char host[128];
    if (url_host(url, host, sizeof(host)) != 0) return -1;

    int s = http_connect(url);
    if (s < 0) return s;

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\n"
                      "Host: %s\r\n"
                      "User-Agent: PSLinker/1.0\r\n"
                      "Connection: close\r\n\r\n", path, host);
    if (send(s, req, rl, 0) != rl) { close(s); return -5; }
    return s;
}

/* Skip the response headers on socket s. Returns content length
   (-1 if unknown) or <0 on error. */
static int http_skip_headers(int s) {
    char hdr[4096];
    int hdr_total = 0, n;
    while (hdr_total < (int)sizeof(hdr) - 1) {
        n = recv(s, hdr + hdr_total, 1, 0);
        if (n <= 0) { close(s); return -7; }
        hdr_total++;
        if (hdr_total > 4 &&
            memcmp(hdr + hdr_total - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    hdr[hdr_total] = '\0';

    int clen = -1;
    char *cl = strcasestr(hdr, "Content-Length:");
    if (cl) clen = atoi(cl + 15);
    return clen;
}

/* ------------------------------------------------------------------ */
/* Directory-listing parser (same logic as the PSP version)            */
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

static int add_file(const char *href) {
    /* Skip parent links and anything that isn't a file in this dir */
    if (href[0] == '\0') return 0;
    if (strncmp(href, "../", 3) == 0) return 0;
    if (strchr(href, '/') != NULL) return 0;   /* nested dir or absolute */
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

/* GET a URL and pass the body to a callback. Returns 0 or <0. */
static int http_get_body(const char *url,
                         int (*cb)(const char *body, void *user),
                         void *user) {
    int s = http_send_get(url);
    if (s < 0) return s;

    if (http_skip_headers(s) < 0) return -7;

    char buf[8192];
    char *acc = NULL;
    size_t total = 0, cap = 0;
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        if (total + n + 1 > cap) {
            cap = (total + n) * 2 + 1024;
            char *tmp = (char *)realloc(acc, cap);
            if (!tmp) { free(acc); close(s); return -6; }
            acc = tmp;
        }
        memcpy(acc + total, buf, n);
        total += n;
    }
    close(s);
    if (!acc) return -6;
    acc[total] = '\0';

    int rc = cb(acc, user);
    free(acc);
    return rc;
}

static int listing_cb(const char *body, void *user) {
    (void)user;
    parse_listing(body);
    return 0;
}

static int fetch_listing(void) {
    char url[MAX_PATH_LEN];
    if (NAS_BASE_URL[strlen(NAS_BASE_URL) - 1] == '/')
        snprintf(url, sizeof(url), "%s", NAS_BASE_URL);
    else
        snprintf(url, sizeof(url), "%s/", NAS_BASE_URL);
    return http_get_body(url, listing_cb, NULL);
}

/* ------------------------------------------------------------------ */
/* Download one file with a Media Go-style progress bar                */
/* ------------------------------------------------------------------ */

static int download_file(const char *url, const char *dest_path) {
    int s = http_send_get(url);
    if (s < 0) return s;

    int content_len = http_skip_headers(s);
    if (content_len < -1) return content_len;   /* header read error */

    FILE *f = fopen(dest_path, "wb");
    if (!f) { close(s); return -3; }

    static char buf[DL_BUF_SIZE];
    long total = 0;
    int ok = 1, n;

    while (ok && (n = recv(s, buf, sizeof(buf), 0)) > 0) {
        if (fwrite(buf, 1, n, f) != (size_t)n) { ok = 0; break; }
        total += n;

        int pct = content_len > 0 ? (int)((total * 100) / content_len) : 0;
        if (pct > 100) pct = 100;
        char info[64];
        snprintf(info, sizeof(info), "%d%%  (%ld KB)", pct, total / 1024);
        Style st = { ANSI_BG_BG, ANSI_FG_DIM };
        field(SCREEN_ROWS - 5, 30, &st, info);
    }

    fclose(f);
    close(s);
    return ok ? 0 : -2;
}

/* ------------------------------------------------------------------ */
/* Pad input                                                           */
/* ------------------------------------------------------------------ */

static void wait_button(void) {
    padInfo pad_inf;
    padData pd;
    unsigned prev = 0;
    while (1) {
        if (ioPadGetInfo(&pad_inf) == 0 && pad_inf.status[0]) {
            if (ioPadGetData(0, &pd) == 0) {
                unsigned pressed = pd.BTN & ~prev;
                prev = pd.BTN;
                if (pressed) return;
            }
        }
        usleep(16000);
    }
}

/* Edge-detected button read; returns 0 and fills *pressed on change. */
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
/* Splash / browser screens (Media Go layout, same as the PSP build)   */
/* ------------------------------------------------------------------ */

static void draw_splash(const char *msg) {
    ui_begin();
    draw_header("PS Linker", "v1.0 PS3");
    draw_sidebar(1, SCREEN_ROWS - 2, 0);

    Style big = { ANSI_BG_BG, ANSI_FG_TXT };
    field(12, 50, &big, "P S   L I N K E R");
    field(14, 50, &big, msg);
    draw_status("Starting...");
}

static int browser(void) {
    draw_splash("Fetching listing from NAS...");

    if (fetch_listing() != 0 || g_file_count == 0) {
        Style err = { ANSI_BG_ERR, ANSI_FG_ERR };
        char msg[96];
        snprintf(msg, sizeof(msg), "Could not reach %s", NAS_BASE_URL);
        show_msg(17, &err, msg);
        show_msg(18, &err, "Press any button to retry.");
        wait_button();
        return -1;
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
            printf("%s%s\x1b[%d;15H %-*s %s",
                   i == sel ? STYLE_SEL.bg : STYLE_ROW.bg,
                   i == sel ? STYLE_SEL.fg : STYLE_ROW.fg,
                   list_top + (i - top) + 1, w, line, ANSI_RESET);
        }

        draw_sidebar(1, SCREEN_ROWS - 2, 0);

        char status[64];
        snprintf(status, sizeof(status), "%d files - %d selected",
                 g_file_count, sel + 1);
        draw_status(status);
        fflush(stdout);

        unsigned pressed;
        do { usleep(16000); }
        while (get_pressed(&pressed) != 0 || pressed == 0);

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
    ioPadInit(1);

    draw_splash("Starting...");

    if (netInitialize() < 0) {   /* uses XMB network settings */
        Style err = { ANSI_BG_ERR, ANSI_FG_ERR };
        show_msg(17, &err, "Network init failed. Check XMB network settings.");
        sleep(5);
        ioPadEnd();
        return 1;
    }

    mkdir(DEST_DIR, 0777);

    while (browser() != 0)
        wait_button();   /* error: wait, then retry the listing */

    draw_splash("Goodbye!");
    sleep(2);

    netDeinitialize();
    ioPadEnd();
    return 0;
}
