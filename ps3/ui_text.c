/*
 * PS Linker - PS3 ANSI (TTY/netdebug) UI backend. See ui.h.
 *
 * The Media Go-style console screens the PS3 app started with, lifted
 * out of main.c so the pixel backend can replace them without the
 * navigation logic noticing. Drawing is printf with ANSI 256-color
 * escapes over the 80x33 netdebug console (RPCS3 shows this in its
 * PPUsdk log; a real netcat terminal renders it).
 */

#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "../ui/store_logic.h"

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
#define ANSI_BG_ERR  "\x1b[48;5;52m"    /* dark red error panel */
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
static const Style STYLE_ROW    = { ANSI_BG_BG,   ANSI_FG_TXT   };
static const Style STYLE_SEL    = { ANSI_BG_SEL,  ANSI_FG_SEL   };
static const Style STYLE_STATUS = { ANSI_BG_ST,   ANSI_FG_DIM   };

/* ------------------------------------------------------------------ */
/* Console drawing helpers                                             */
/* ------------------------------------------------------------------ */

static void ui_begin(void)
{
    /* Hide cursor, clear screen, home cursor */
    printf("\x1b[?25l\x1b[2J\x1b[1;1H");
}

/* Print text left-aligned inside a width-w field on row (0-based). */
static void field(int row, int width, const Style *s, const char *text)
{
    printf("%s%s\x1b[%d;1H%s", s->bg, s->fg, row + 1, text);
    int len = (int)strlen(text);
    for (int i = len; i < width; i++) putchar(' ');
    printf(ANSI_RESET);
    fflush(stdout);
}

/* Full-width header bar, like Media Go's title strip. */
static void draw_header(const char *title, const char *subtitle)
{
    char buf[SCREEN_COLS + 1];
    snprintf(buf, sizeof(buf), "  %s  %s", title, subtitle);
    field(0, SCREEN_COLS, &STYLE_HEADER, buf);
}

/* Left sidebar, Media Go style. active = currently highlighted item. */
static const char *SIDEBAR[] = { "Games", "Downloads", "Settings" };

static void draw_sidebar(int row_from, int row_to, int active)
{
    char line[32];
    for (int r = row_from; r <= row_to; r++) {
        int idx = r - row_from - 2;
        if (idx >= 0 && idx < (int)(sizeof(SIDEBAR) / sizeof(SIDEBAR[0])))
            snprintf(line, sizeof(line), "   %s%s",
                     idx == active ? "> " : "  ", SIDEBAR[idx]);
        else
            line[0] = '\0';
        printf("%s%s\x1b[%d;1H%-14s%s",
               "\x1b[48;5;236m", ANSI_FG_DIM, r + 1, line, ANSI_RESET);
    }
    fflush(stdout);
}

/* Bottom status bar with hint text and an optional status message. */
static void draw_status(const char *msg)
{
    char buf[SCREEN_COLS + 1];
    snprintf(buf, sizeof(buf),
             " Up/Down:Move  X:Download/Enter  O:Parent  Select:Settings | %s",
             msg ? msg : "");
    field(SCREEN_ROWS - 1, SCREEN_COLS, &STYLE_STATUS, buf);
}

/* ------------------------------------------------------------------ */
/* ui.h backend contract                                               */
/* ------------------------------------------------------------------ */

int ui_init(void) { return 0; }          /* text build: always ANSI      */
void ui_exit(void) { }
int ui_pixel(void) { return 0; }

void ui_splash(const char *msg)
{
    ui_begin();
    draw_header("PS Linker", "v1.3 PS3");
    draw_sidebar(1, SCREEN_ROWS - 2, 0);

    Style big = { ANSI_BG_BG, ANSI_FG_TXT };
    field(12, 50, &big, "P S   L I N K E R");
    field(14, 50, &big, msg);
    draw_status("Starting...");
}

void ui_error(const char *line1, const char *line2)
{
    ui_begin();
    draw_header("PS Linker", "error");
    draw_sidebar(1, SCREEN_ROWS - 2, 0);
    Style err = { ANSI_BG_ERR, ANSI_FG_ERR };
    field(17, SCREEN_COLS - 14, &err, line1);
    if (line2) field(18, SCREEN_COLS - 14, &err, line2);
    draw_status("Error");
}

int ui_browser_rows(void) { return SCREEN_ROWS - 7 - 2; }

void ui_browser(const char *path, const FileEntry *files, int count,
                int sel, int top, const char *status)
{
    const int list_top = 2;
    const int list_rows = ui_browser_rows();

    ui_begin();

    /* Panel header shows current path, truncated to fit. */
    Style head = { ANSI_BG_BG, ANSI_FG_DIM };
    char hdr[SCREEN_COLS - 14 + 1];
    snprintf(hdr, sizeof(hdr), " NAS LIBRARY: /%s", path);
    field(1, SCREEN_COLS - 14, &head, hdr);

    int w = SCREEN_COLS - 16;
    char line[64 + 32];
    for (int i = top; i < count && i < top + list_rows; i++) {
        snprintf(line, sizeof(line), "%s%s",
                 files[i].is_dir ? "[DIR]  " : "       ",
                 files[i].name);
        /* Same store-card meta as the pixel backend: size, plus category
           where the path header does not already state it. */
        char meta[96];
        meta[0] = '\0';
        if (files[i].size >= 0) {
            char sz[24];
            store_format_size((uint64_t)files[i].size, sz, sizeof sz);
            snprintf(meta, sizeof meta, "%s", sz);
        }
        if (!files[i].is_dir && files[i].category[0]) {
            size_t cl = strlen(files[i].category);
            if (!(strncmp(path, files[i].category, cl) == 0 &&
                  (path[cl] == '\0' || path[cl] == '/'))) {
                strncat(meta, meta[0] ? "  " : "", sizeof meta - strlen(meta) - 1);
                strncat(meta, files[i].category, sizeof meta - strlen(meta) - 1);
            }
        }
        int ml = (int)strlen(meta);
        if (ml && (int)strlen(line) + ml <= w - 1) {
            char padded[160];
            snprintf(padded, sizeof padded, "%-*s%s", w - ml, line, meta);
            snprintf(line, sizeof line, "%s", padded);
        } else if ((int)strlen(line) > w) {
            line[w] = '\0';
        }
        printf("%s%s\x1b[%d;15H %-*s %s",
               i == sel ? STYLE_SEL.bg : STYLE_ROW.bg,
               i == sel ? STYLE_SEL.fg : STYLE_ROW.fg,
               list_top + (i - top) + 1, w, line, ANSI_RESET);
    }

    draw_sidebar(1, SCREEN_ROWS - 2, 0);
    draw_status(status);
    fflush(stdout);
}

/* The strip is 20 slots wide and wraps to two rows just above the
   status bar; the pixel backend uses the same slot order as a grid. */
#define STRIP_COLS 20

int ui_settings_cols(void) { return STRIP_COLS; }

void ui_settings(const char *url, int pick, int nslots)
{
    const int strip_w = SCREEN_COLS - 16;      /* list panel width      */
    const int picker_top = SCREEN_ROWS - 8;

    ui_begin();
    draw_header("PS Linker", "Settings");
    draw_sidebar(1, SCREEN_ROWS - 2, 2);   /* highlight "Settings" */

    Style lbl = { ANSI_BG_BG, ANSI_FG_DIM };
    field(4, strip_w, &lbl, " NAS SERVER URL");
    Style val = { ANSI_BG_BG, ANSI_FG_TXT };
    char shown[256];
    snprintf(shown, sizeof(shown), "%s", url);
    if ((int)strlen(shown) > strip_w - 2) shown[strip_w - 2] = '\0';
    field(6, strip_w, &val, shown);

    Style hint = { ANSI_BG_BG, ANSI_FG_DIM };
    field(8, strip_w, &hint,
          " <>:pick ^v:jump  X:insert  O:delete  Start:save  Select:cancel");

    char cell[4];
    for (int slot = 0; slot < nslots; slot++) {
        const Style *st = (slot == pick) ? &STYLE_SEL : &STYLE_ROW;
        if (slot < SETTINGS_NCHARS) {
            cell[0] = SETTINGS_CHARS[slot];
            cell[1] = '\0';
        } else if (slot == nslots - 2) {
            snprintf(cell, sizeof cell, "<");
        } else {
            snprintf(cell, sizeof cell, "OK");
        }
        printf("%s%s\x1b[%d;%dH %-2s%s",
               st->bg, st->fg,
               picker_top + 1 + slot / STRIP_COLS,
               15 + 2 * (slot % STRIP_COLS), cell, ANSI_RESET);
    }
    fflush(stdout);

    draw_status("Settings");
}

void ui_download(const char *name, const char *path, int pct, long total)
{
    /* The whole panel repaints when the file changes; the progress line
       is then patched in place so a busy TCP console never flickers. */
    static char last[256] = "";
    char info[96];
    if (pct >= 0)
        snprintf(info, sizeof(info), "%d%%  (%ld KB)", pct, total / 1024);
    else
        snprintf(info, sizeof(info), "(%ld KB)", total / 1024);
    (void)path;
    Style st = { ANSI_BG_BG, ANSI_FG_DIM };
    if (strcmp(last, name) != 0) {
        snprintf(last, sizeof last, "%s", name);
        ui_begin();
        draw_header("PS Linker", "Downloading...");
        draw_sidebar(1, SCREEN_ROWS - 2, 1);
        field(12, 60, &st, name);
        draw_status("Downloading");
    }
    field(14, 60, &st, info);
}

void ui_result(int ok, const char *msg)
{
    Style res = { ANSI_BG_BG, ok ? ANSI_FG_OK : ANSI_FG_ERR };
    field(SCREEN_ROWS - 3, SCREEN_COLS - 14, &res, msg);
    draw_status(ok ? "Download complete" : "Download failed");
}
