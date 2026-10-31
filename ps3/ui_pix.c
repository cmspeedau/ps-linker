/*
 * PS Linker - PS3 pixel (RSX) UI backend. See ui.h.
 *
 * All screens on the gfx layer at the virtual 1280x720 grid (gfx.c maps
 * it to the panel). The browser frame itself is drawn by store_gui.c
 * (ui_browse is the pass-through); the splash/settings/download frames
 * here keep the Media Go chrome (orange header, sidebar rail) so they
 * read as one app with the Store.
 */

#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "ui.h"
#include "store_gui.h"

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

static void hint_bar(const char *hint)
{
    gfx_text(18, STATUS_Y + 12, hint, COL_TEXT, 2);
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

/* ---- browser ---------------------------------------------------------- */

/* The browser is the Cover Flow frame; ui_browse is a thin pass-through
 * to the Store's pixel card code so both views share one renderer. The
 * row-list browser and its chrome (and the ui_text.c ANSI twin) were
 * removed with the Cover Flow redesign. */

void ui_browse(const coverflow_state_t *cf, const char *const *ids,
               const char *const *descs, const char *path)
{
    store_gui_browse(cf, ids, descs, path);
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
