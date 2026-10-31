/*
 * PS Linker - PS3 pixel (RSX) UI backend. See ui.h.
 *
 * All screens on the gfx layer at the virtual 1280x720 grid (gfx.c maps
 * it to the panel). The browser frame is store_gui_browse and the URL
 * editor is store_gui_url_edit; the splash/error/download/result frames
 * here use the same chrome as the Store - orange header, status hint bar
 * - so one look runs through the whole app. The old Media Go sidebar
 * chrome and its character-grid Settings screen were removed with the
 * pixel Utilities page (SELECT): nothing on screen is a foreign style.
 */

#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "ui.h"
#include "store_gui.h"

/* Shared palette (see psp/store_gui.c for the PSP twin). */
#define COL_BG      0xFF1D1D1D
#define COL_HEADER  0xFFCC7A00
#define COL_LINE    0xFF333333
#define COL_TEXT    0xFFCCCCCC
#define COL_DIM     0xFF808080
#define COL_BLACK   0xFF000000
#define COL_OK      0xFF7AC943
#define COL_ERRTX   0xFFE57373

#define SCR_W  1280
#define SCR_H  720
#define HEADER_H   44
#define STATUS_Y   676

/* ---- chrome --------------------------------------------------------- */

/* Store frame: orange header bar with the app name and a right tag. */
static void frame(const char *right)
{
    gfx_begin(COL_BG);
    gfx_rect(0, 0, SCR_W, HEADER_H, COL_HEADER);
    gfx_text(18, 12, "PS Linker", COL_BLACK, 2);
    if (right && right[0])
        gfx_text(SCR_W - 18 - gfx_text_width(right, 2), 12, right,
                 COL_BLACK, 2);
}

/* The frame's bottom hint bar (same bar the browser draws). */
static void hint_bar(const char *hint)
{
    gfx_rect(0, STATUS_Y, SCR_W, SCR_H - STATUS_Y, COL_LINE);
    gfx_text(18, STATUS_Y + 12, hint, COL_TEXT, 2);
}

/* Truncate to what fits x pixels at the given scale (whole glyphs). */
static void clip(char *dst, size_t cap, const char *src, int px, int scale)
{
    const int max = px / (8 * scale);
    size_t n = 0;
    while (n + 1 < cap && n < (size_t)max && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
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
    frame("v1.3 PS3");
    gfx_text_center(SCR_W / 2, 260, "P S   L I N K E R", COL_TEXT, 4);
    if (msg && msg[0]) gfx_text_center(SCR_W / 2, 340, msg, COL_DIM, 2);
    hint_bar("Starting...");
    gfx_end();
}

void ui_error(const char *line1, const char *line2)
{
    frame("error");
    char buf[300];
    clip(buf, sizeof buf, line1 ? line1 : "", SCR_W - 80, 3);
    gfx_text_center(SCR_W / 2, 320, buf, COL_ERRTX, 3);
    /* The second line is an instruction, so it goes in the frame's hint
     * bar - the place the Store and the PSP twin put key hints. */
    hint_bar(line2 && line2[0] ? line2 : "Press any button to retry");
    gfx_end();
}

/* ---- browser ---------------------------------------------------------- */

/* The browser is the Cover Flow frame; ui_browse is a thin pass-through
 * to the Store's pixel card code so both views share one renderer. The
 * row-list browser and its sidebar chrome were removed with the pixel
 * Utilities page. */

void ui_browse(const coverflow_state_t *cf, const char *const *ids,
               const char *const *descs, const char *path,
               const int *progress)
{
    store_gui_browse(cf, ids, descs, path, progress);
}

void ui_download(const char *name, const char *path, int pct, long total)
{
    frame("downloading");

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

    hint_bar("Interrupted files stay resume-ready");
    gfx_end();
}

void ui_result(int ok, const char *msg)
{
    frame(ok ? "done" : "failed");
    char buf[300];
    clip(buf, sizeof buf, msg ? msg : "", SCR_W - 80, 3);
    gfx_text_center(SCR_W / 2, 320, buf, ok ? COL_OK : COL_ERRTX, 3);
    hint_bar("Press any button to continue");
    gfx_end();
}
