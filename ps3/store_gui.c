/*
 * PS Linker - Store pixel view (PS3)
 *
 * Draws the Store screens from the shared Cover Flow state using the
 * RSX renderer in gfx.c. The module (ui/coverflow.c) owns selection,
 * filtering and view switching; this file only draws what it exposes —
 * the same contract the ANSI renderer and psp/store_gui.c work under.
 *
 * Layout mirrors psp/store_gui.c on the virtual 1280x720 grid: orange
 * header, tab rail with active underline, centred Cover Flow card with
 * two dimmed neighbours per side, or the 4x2 card grid, plus the
 * install screen with a live progress bar.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "store_gui.h"
#include "../ui/store_logic.h"
#include "../ui/coverflow.h"

/* Palette: identical to psp/store_gui.c. */
#define COL_BG      0xFF1D1D1D
#define COL_HEADER  0xFFCC7A00
#define COL_FIELD   0xFF161616
#define COL_LINE    0xFF333333
#define COL_CARD    0xFF232323
#define COL_TEXT    0xFFCCCCCC
#define COL_DIM     0xFF808080
#define COL_FAINT   0xFF666666
#define COL_BLACK   0xFF000000
#define COL_FAV     0xFFE3B341
#define COL_INK     0xFFFFFFFF

#define SCR_W  1280
#define SCR_H  720
#define HEADER_H   44
#define TABS_Y     44
#define TABS_H     46
#define STATUS_Y   676

/* One placeholder card, refilled per visible item. Same 64x85 tile on
 * a 64x128 buffer as on the PSP; gfx_tex_rect stretches it. */
#define COVER_W    64
#define COVER_H    85
#define COVER_TEX_H 128
static uint32_t g_cover[COVER_W * COVER_TEX_H] __attribute__((aligned(16)));

static void cover_tile(const char *id)
{
    memset(g_cover, 0, sizeof(g_cover));
    store_cover_pixels(g_cover, COVER_W, COVER_H, id);
}

/* snprintf-append that can never run `off` past the buffer. */
static int app(char *dst, size_t cap, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off >= cap) return (int)cap - 1;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(dst + off, cap - (size_t)off, fmt, ap);
    va_end(ap);
    if (n < 0) return off;
    return ((size_t)n >= cap - (size_t)off) ? (int)cap - 1 : off + n;
}

/* Truncate to whole glyphs at the given scale, fitting `px` pixels. */
static void clip(char *dst, size_t cap, const char *src, int px, int scale)
{
    const int max = px / (8 * scale);
    size_t n = 0;
    while (n + 1 < cap && n < (size_t)max && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

/* First letters of the first two words, upper case — the label baked
 * into the web GUI's placeholder covers. */
static void initials(const char *title, char out[3])
{
    out[0] = out[1] = out[2] = '\0';
    int n = 0, word_start = 1;
    for (const char *p = title; *p && n < 2; p++) {
        unsigned char c = (unsigned char)*p;
        const int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9');
        if (!alnum) { word_start = 1; continue; }
        if (n == 0 || word_start) {
            if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
            out[n++] = (char)c;
        }
        word_start = 0;
    }
}

static void draw_header(const char *title, const char *right)
{
    gfx_rect(0, 0, SCR_W, HEADER_H, COL_HEADER);
    gfx_text(18, 12, title, COL_BLACK, 2);
    if (right && right[0])
        gfx_text(SCR_W - 18 - gfx_text_width(right, 2), 12, right, COL_BLACK, 2);
}

static void draw_tabs(const coverflow_state_t *cf)
{
    gfx_rect(0, TABS_Y, SCR_W, TABS_H, COL_FIELD);
    gfx_rect(0, TABS_Y + TABS_H, SCR_W, 2, COL_LINE);

    const int n = (int)cf->dynamic_category_count;
    if (!n) return;

    int w[COVERFLOW_MAX_DYNAMIC_CATEGORIES];
    for (int i = 0; i < n; i++)
        w[i] = gfx_text_width(cf->dynamic_categories[i], 2) + 40;

    /* Window the rail around the active tab; an active tab is never
       wider than the screen, so sliding `start` right terminates. */
    int start = 0;
    while (start < (int)cf->category) {
        int x = 18, vis = 0;
        for (int i = start; i < n && x + w[i] <= SCR_W - 18; i++) {
            if (i == (int)cf->category) vis = 1;
            x += w[i];
        }
        if (vis) break;
        start++;
    }

    int x = 18;
    for (int i = start; i < n && x + w[i] <= SCR_W - 18; i++) {
        const int active = (i == (int)cf->category);
        if (active) {
            gfx_rect(x, TABS_Y, w[i], TABS_H, COL_CARD);
            gfx_rect(x, TABS_Y + TABS_H - 5, w[i], 5, COL_HEADER);
        }
        gfx_text(x + 20, TABS_Y + 14, cf->dynamic_categories[i],
                 active ? COL_TEXT : COL_DIM, 2);
        x += w[i];
    }
}

static void draw_flow(const coverflow_state_t *cf,
                      const char *const *ids, const char *const *descs)
{
    const coverflow_item_t *it = coverflow_current(cf);
    if (!it) {
        gfx_text_center(SCR_W / 2, 360, "(no items in this tab)", COL_DIM, 2);
        return;
    }
    const size_t idx = cf->index;
    const size_t slot = cf->filtered[idx];

    /* Side cards: two per side at 720p, faded via the modulate alpha;
       the same quad as the centre, just smaller. */
    if (idx > 0) {
        cover_tile(ids[cf->filtered[idx - 1]]);
        gfx_tex_rect(300, 175, 170, 226, g_cover, COVER_W, COVER_TEX_H,
                     COVER_W, COVER_H, 0x80FFFFFF, 1);
    }
    if (idx > 1) {
        cover_tile(ids[cf->filtered[idx - 2]]);
        gfx_tex_rect(110, 210, 110, 146, g_cover, COVER_W, COVER_TEX_H,
                     COVER_W, COVER_H, 0x40FFFFFF, 1);
    }
    if (idx + 1 < cf->filtered_len) {
        cover_tile(ids[cf->filtered[idx + 1]]);
        gfx_tex_rect(810, 175, 170, 226, g_cover, COVER_W, COVER_TEX_H,
                     COVER_W, COVER_H, 0x80FFFFFF, 1);
    }
    if (idx + 2 < cf->filtered_len) {
        cover_tile(ids[cf->filtered[idx + 2]]);
        gfx_tex_rect(1060, 210, 110, 146, g_cover, COVER_W, COVER_TEX_H,
                     COVER_W, COVER_H, 0x40FFFFFF, 1);
    }

    /* Centre card: orange frame, 260x348 cover, initials over it. */
    cover_tile(ids[slot]);
    gfx_rect(505, 115, 270, 358, COL_HEADER);
    gfx_tex_rect(510, 120, 260, 348, g_cover, COVER_W, COVER_TEX_H,
                 COVER_W, COVER_H, COL_INK, 1);
    char ini[3];
    initials(it->title, ini);
    if (ini[0])
        gfx_text_center(SCR_W / 2, 265, ini, 0xAAFFFFFF, 6);

    char buf[300], meta[160];
    clip(buf, sizeof buf, it->title, SCR_W - 80, 3);
    gfx_text_center(SCR_W / 2, 492, buf, COL_TEXT, 3);

    meta[0] = '\0';
    int off = 0;
    if (it->version[0]) off = app(meta, sizeof meta, off, "%s  ", it->version);
    if (it->size_human[0]) off = app(meta, sizeof meta, off, "%s  ", it->size_human);
    off = app(meta, sizeof meta, off, "%s", it->category);
    if (it->is_favorite) off = app(meta, sizeof meta, off, "  * fav");
    gfx_text_center(SCR_W / 2, 536, meta, COL_DIM, 2);

    clip(buf, sizeof buf, descs[slot], SCR_W - 80, 2);
    if (buf[0]) gfx_text_center(SCR_W / 2, 572, buf, COL_DIM, 2);

    snprintf(meta, sizeof meta, "[ %lu / %lu ]",
             (unsigned long)(idx + 1), (unsigned long)cf->filtered_len);
    gfx_text_center(SCR_W / 2, 616, meta, COL_FAINT, 2);
}

static void draw_grid(const coverflow_state_t *cf,
                      const char *const *ids, const char *const *descs)
{
    (void)descs;
    /* 4x2 page. Same rule shape as psp/store_gui.c: the pixel layout
       owns paging, page = index / per_page. */
    const int cols = 4, rows = 2, per = cols * rows;
    const size_t first = (cf->index / (size_t)per) * (size_t)per;
    size_t last = first + (size_t)per;
    if (last > cf->filtered_len) last = cf->filtered_len;

    size_t i = first;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols && i < last; c++, i++) {
            const size_t slot = cf->filtered[i];
            const coverflow_item_t *it = &cf->items[slot];
            const int x = 24 + c * 312, y = 112 + r * 278;

            if (i == cf->index) gfx_rect(x - 4, y - 4, 298, 266, COL_HEADER);
            gfx_rect(x, y, 290, 258, COL_CARD);
            cover_tile(ids[slot]);
            gfx_tex_rect(x + 4, y + 4, 282, 180, g_cover, COVER_W, COVER_TEX_H,
                         COVER_W, COVER_H, COL_INK, 1);
            char ini[3];
            initials(it->title, ini);
            if (ini[0])
                gfx_text_center(x + 145, y + 80, ini, 0x99FFFFFF, 4);

            char buf[64];
            clip(buf, sizeof buf, it->title, 260, 2);
            gfx_text(x + 14, y + 200, buf, COL_TEXT, 2);
            clip(buf, sizeof buf, it->size_human, 260, 2);
            gfx_text(x + 14, y + 230, buf, COL_DIM, 2);
            if (it->is_favorite) gfx_text(x + 268, y + 12, "*", COL_FAV, 2);
        }
    }

    const size_t pages = (cf->filtered_len + (size_t)per - 1) / (size_t)per;
    char buf[64];
    snprintf(buf, sizeof buf, "[ page %lu / %lu ]",
             (unsigned long)(first / (size_t)per + 1), (unsigned long)pages);
    gfx_text_center(SCR_W / 2, 654, buf, COL_FAINT, 2);
}

static void draw_hints(int grid)
{
    gfx_rect(0, STATUS_Y, SCR_W, SCR_H - STATUS_Y, COL_LINE);
    gfx_text(18, STATUS_Y + 12,
             grid ? "Arrows move  L1R1 tab  Sel flow  Sq fav  X go  O back"
                  : "L/R cover  UpDn jump  L1R1 tab  Sel grid  Sq fav  X O",
             COL_TEXT, 2);
}

void store_gui_draw(const coverflow_state_t *cf,
                    const char *const *ids, const char *const *descs)
{
    gfx_begin(COL_BG);
    draw_header("PS Linker", "PS3");
    draw_tabs(cf);
    if (cf->view == COVERFLOW_VIEW_GRID) draw_grid(cf, ids, descs);
    else                                 draw_flow(cf, ids, descs);
    draw_hints(cf->view == COVERFLOW_VIEW_GRID);
    gfx_end();
}

void store_gui_status(const char *header, const char *body, const char *hint)
{
    gfx_begin(COL_BG);
    draw_header(header ? header : "PS Linker", "");
    if (body) {
        char buf[300];
        clip(buf, sizeof buf, body, SCR_W - 80, 3);
        gfx_text(40, 200, buf, COL_TEXT, 3);
    }
    if (hint) {
        gfx_rect(0, STATUS_Y, SCR_W, SCR_H - STATUS_Y, COL_LINE);
        gfx_text(18, STATUS_Y + 12, hint, COL_TEXT, 2);
    }
    gfx_end();
}

void store_gui_progress(const char *title, const char *path, int pct, long total)
{
    gfx_begin(COL_BG);
    draw_header("PS Linker", "installing");

    char buf[300], titlebuf[300];
    clip(titlebuf, sizeof titlebuf, title ? title : "", SCR_W - 80, 3);
    gfx_text_center(SCR_W / 2, 240, titlebuf, COL_TEXT, 3);

    gfx_rect(290, 400, 700, 30, COL_LINE);
    gfx_rect(296, 406, 688, 18, 0xFF111111);
    if (pct > 0) {
        if (pct > 100) pct = 100;
        gfx_rect(296, 406, 688 * pct / 100, 18, COL_HEADER);
    }

    if (pct >= 0) snprintf(buf, sizeof buf, "%d%%   %ld KB", pct, total / 1024);
    else          snprintf(buf, sizeof buf, "%ld KB", total / 1024);
    gfx_text_center(SCR_W / 2, 450, buf, COL_TEXT, 2);

    if (path && path[0]) {
        clip(buf, sizeof buf, path, SCR_W - 80, 2);
        gfx_text_center(SCR_W / 2, 490, buf, COL_DIM, 2);
    }

    /* A pkg only becomes a game once Package Manager has picked it up. */
    const int is_pkg = path && strncmp(path, "/dev_hdd0/packages/", 21) == 0;

    gfx_rect(0, STATUS_Y, SCR_W, SCR_H - STATUS_Y, COL_LINE);
    gfx_text(18, STATUS_Y + 12,
             is_pkg ? "Then install it from XMB > Game > Package Manager"
                    : "Interrupted files stay resume-ready",
             COL_TEXT, 2);
    gfx_end();
}
