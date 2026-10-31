/*
 * PS Linker - PS3 pixel (RSX) UI backend. See ui.h.
 *
 * The same Media Go layout the ANSI backend draws, on the gfx layer at
 * the virtual 1280x720 grid (gfx.c maps it to the panel). Colours and
 * metrics match the Store's pixel screens (store_gui.c) so the browser,
 * Settings and the Store read as one app.
 */

#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "ui.h"
#include "../ui/store_logic.h"

/* Shared palette (see psp/store_gui.c for the PSP twin). */
#define COL_BG      0xFF1D1D1D
#define COL_HEADER  0xFFCC7A00
#define COL_FIELD   0xFF161616
#define COL_LINE    0xFF333333
#define COL_PANE    0xFF262626
#define COL_SEL     0xFF1F5FA8
#define COL_SELTX   0xFFFFFFFF
#define COL_TEXT    0xFFCCCCCC
#define COL_DIM     0xFF808080
#define COL_FAINT   0xFF666666
#define COL_BLACK   0xFF000000
#define COL_OK      0xFF7AC943
#define COL_ERRTX   0xFFE57373
#define COL_ERRBG   0xFF521A1A

#define SCR_W  1280
#define SCR_H  720
#define HEADER_H   44
#define STATUS_Y   676
#define SIDEBAR_W  240
#define PANEL_X    256
#define PANEL_R    1264

/* ---- chrome --------------------------------------------------------- */

static void chrome(const char *subtitle, int sidebar_active)
{
    gfx_begin(COL_BG);
    gfx_rect(0, 0, SCR_W, HEADER_H, COL_HEADER);
    gfx_text(18, 12, "PS Linker", COL_BLACK, 2);
    if (subtitle && subtitle[0])
        gfx_text(18 + gfx_text_width("PS Linker", 2) + 20, 12, subtitle,
                 COL_BLACK, 2);
    gfx_rect(0, HEADER_H, SIDEBAR_W, STATUS_Y - HEADER_H, COL_PANE);
    static const char *items[] = { "Games", "Downloads", "Settings" };
    for (int i = 0; i < 3; i++) {
        const int y = 70 + i * 54;
        if (i == sidebar_active)
            gfx_rect(0, y, SIDEBAR_W, 48, COL_SEL);
        gfx_text(28, y + 14, items[i], i == sidebar_active ? COL_SELTX : COL_DIM, 2);
    }
    gfx_rect(0, STATUS_Y, SCR_W, SCR_H - STATUS_Y, COL_LINE);
}

/* Truncate to what fits x pixels at the given scale (whole glyphs). */
static void clip(char *dst, size_t cap, const char *src, int px, int scale)
{
    const int max = px / (8 * scale);
    size_t n = 0;
    while (n + 1 < cap && n < (size_t)max && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

static void status_bar(const char *msg)
{
    if (!msg || !msg[0]) return;
    const int w = gfx_text_width(msg, 2);
    gfx_text(PANEL_R - w, STATUS_Y + 12, msg, COL_TEXT, 2);
}

static void hint_bar(const char *hint)
{
    gfx_text(18, STATUS_Y + 12, hint, COL_TEXT, 2);
}

/* Hint on the left, status right-aligned, in one bar: the hint is
 * clipped to the space the status leaves so the two never collide. */
static void hint_status_bar(const char *hint, const char *status)
{
    int room = SCR_W - 36;
    if (status && status[0]) {
        status_bar(status);
        room -= gfx_text_width(status, 2) + 24;
    }
    char buf[160];
    clip(buf, sizeof buf, hint, room, 2);
    hint_bar(buf);
}

/* ---- ui.h contract --------------------------------------------------- */

static int g_pixel = 0;

int ui_init(void)
{
    g_pixel = (gfx_init() == 0);
    return g_pixel;
}

void ui_exit(void)
{
    if (g_pixel) gfx_term();
    g_pixel = 0;
}

int ui_pixel(void) { return g_pixel; }

void ui_splash(const char *msg)
{
    chrome("v1.3 PS3", 0);
    gfx_text_center(SCR_W / 2, 260, "P S   L I N K E R", COL_TEXT, 4);
    if (msg && msg[0]) gfx_text_center(SCR_W / 2, 340, msg, COL_DIM, 2);
    hint_bar("Starting...");
    gfx_end();
}

void ui_error(const char *line1, const char *line2)
{
    chrome("error", 0);
    gfx_rect(PANEL_X, 280, PANEL_R - PANEL_X, 140, COL_ERRBG);
    char buf[128];
    clip(buf, sizeof buf, line1 ? line1 : "", PANEL_R - PANEL_X - 60, 2);
    gfx_text(PANEL_X + 30, 310, buf, COL_ERRTX, 2);
    if (line2) {
        clip(buf, sizeof buf, line2, PANEL_R - PANEL_X - 60, 2);
        gfx_text(PANEL_X + 30, 360, buf, COL_ERRTX, 2);
    }
    hint_bar("Press any button to retry");
    gfx_end();
}

/* List rows: y=104 pitch 48, down to the status bar. */
#define ROWS_TOP   104
#define ROW_PITCH  48
#define ROW_TEXT_X 366

int ui_browser_rows(void) { return (STATUS_Y - 8 - ROWS_TOP) / ROW_PITCH; }

void ui_browser(const char *path, const FileEntry *files, int count,
                int sel, int top, const char *status)
{
    chrome(NULL, 0);

    char hdr[300];
    snprintf(hdr, sizeof hdr, " NAS LIBRARY: /%s", path);
    gfx_text(PANEL_X + 14, 58, hdr, COL_DIM, 2);

    const int rows = ui_browser_rows();
    for (int i = top; i < count && i < top + rows; i++) {
        const int y = ROWS_TOP + (i - top) * ROW_PITCH;
        if (i == sel) gfx_rect(PANEL_X - 8, y - 4, PANEL_R - PANEL_X + 16, ROW_PITCH - 4, COL_SEL);
        if (files[i].is_dir)
            gfx_text(PANEL_X + 10, y + 10, "[DIR]", i == sel ? COL_SELTX : COL_DIM, 2);

        /* Store-card chrome: the size, plus the category where the path
           header does not already state it (the path only shows its own
           top folder, and a folder row's category is its own name).
           Shares without the attributes report size -1/category "", so
           these rows come out exactly as they used to. */
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
        const int mw = meta[0] ? gfx_text_width(meta, 2) : 0;

        char name[300];
        clip(name, sizeof name, files[i].name,
             PANEL_R - ROW_TEXT_X - 30 - (mw ? mw + 12 : 0), 2);
        gfx_text(ROW_TEXT_X, y + 10, name,
                 i == sel ? COL_SELTX : COL_TEXT, 2);
        if (mw)
            gfx_text(PANEL_R - 14 - mw, y + 10, meta,
                     i == sel ? COL_SELTX : COL_DIM, 2);
    }

    hint_status_bar("Up/Down move   X open   O up   SELECT settings   Triangle store",
                    status);
    gfx_end();
}

/* 13 picker columns; DEL/OK are the last two slots. */
#define PICK_COLS 13

int ui_settings_cols(void) { return PICK_COLS; }

void ui_settings(const char *url, int pick, int nslots)
{
    chrome("Settings", 2);

    gfx_text(PANEL_X + 14, 72, "NAS SERVER URL", COL_DIM, 2);
    gfx_rect(PANEL_X + 14, 112, PANEL_R - PANEL_X - 28, 56, COL_FIELD);
    gfx_rect(PANEL_X + 14, 170, PANEL_R - PANEL_X - 28, 3, COL_HEADER);
    char shown[256];
    clip(shown, sizeof shown, url, PANEL_R - PANEL_X - 80, 3);
    gfx_text(PANEL_X + 30, 124, shown, COL_TEXT, 3);
    /* caret right after the text, like a real field */
    gfx_text(PANEL_X + 30 + gfx_text_width(shown, 3) + 4, 124, "_", COL_HEADER, 3);

    gfx_text(PANEL_X + 14, 196,
             "D-pad pick  X insert  O delete  START save  SELECT cancel",
             COL_FAINT, 2);

    const int cell_w = 70, cell_h = 50, gap_x = 6, gap_y = 12;
    const int x0 = PANEL_X + 14, y0 = 250;
    for (int slot = 0; slot < nslots; slot++) {
        const int col = slot % PICK_COLS, row = slot / PICK_COLS;
        const int x = x0 + col * (cell_w + gap_x);
        const int y = y0 + row * (cell_h + gap_y);
        const int active = (slot == pick);
        gfx_rect(x, y, cell_w, cell_h, active ? COL_SEL : COL_FIELD);
        char label[4] = { 0 };
        if (slot < SETTINGS_NCHARS) { label[0] = SETTINGS_CHARS[slot]; }
        else if (slot == nslots - 2) { snprintf(label, sizeof label, "<"); }
        else                         { snprintf(label, sizeof label, "OK"); }
        gfx_text_center(x + cell_w / 2, y + (cell_h - 16) / 2, label,
                        active ? COL_SELTX : COL_TEXT, 2);
    }

    hint_bar("Settings");
    gfx_end();
}

void ui_download(const char *name, const char *path, int pct, long total)
{
    chrome("Downloading...", 1);

    char buf[96], nm[300];
    clip(nm, sizeof nm, name, SCR_W - 100, 3);
    gfx_text_center(SCR_W / 2, 240, nm, COL_TEXT, 3);

    gfx_rect(290, 400, 700, 30, COL_LINE);
    gfx_rect(296, 406, 688, 18, 0xFF111111);
    if (pct > 0)
        gfx_rect(296, 406, 688 * (pct > 100 ? 100 : pct) / 100, 18, COL_HEADER);

    if (pct >= 0) snprintf(buf, sizeof buf, "%d%%   %ld KB", pct, total / 1024);
    else          snprintf(buf, sizeof buf, "%ld KB", total / 1024);
    gfx_text_center(SCR_W / 2, 450, buf, COL_TEXT, 2);

    if (path && path[0]) {
        clip(buf, sizeof buf, path, SCR_W - 100, 2);
        gfx_text_center(SCR_W / 2, 490, buf, COL_DIM, 2);
    }

    hint_bar("O aborts where supported - partial file stays resume-ready");
    gfx_end();
}

void ui_result(int ok, const char *msg)
{
    chrome(ok ? "done" : "failed", 1);
    gfx_text_center(SCR_W / 2, 320, msg, ok ? COL_OK : COL_ERRTX, 3);
    hint_bar("Press any button to continue");
    gfx_end();
}
