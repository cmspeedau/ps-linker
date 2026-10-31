/*
 * PS Linker - Store pixel view (PSP)
 *
 * Draws the Store screens from the shared Cover Flow state using the GU
 * layer in gui.c. The module (ui/coverflow.h) owns selection, filtering
 * and view switching; this file only draws what it exposes, which is the
 * same contract the ANSI renderer works under — one state, two backends.
 *
 * Layout mirrors the web GUI's store (docs/UI-REFERENCE.md): orange
 * header bar, tab rail, centred Cover Flow card with dimmed neighbours,
 * or the card grid when the module reports COVERFLOW_VIEW_GRID.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"
#include "store_gui.h"
#include "catalog.h"
#include "../ui/coverflow.h"

/* Palette: store.c's debug-screen colours verbatim, plus the two shades
 * the web GUI uses for card surfaces. */
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

#define SCR_W  480
#define SCR_H  272

/* One placeholder card, refilled per visible item (flow shows at most
 * three, the grid page is 8 — cheap to regenerate every frame). Rows are
 * 64-wide (a GE-friendly texture width) x 128 tall; the tile proper is
 * 64 x 85 and the texcoords never leave that region. */
#define COVER_W    64
#define COVER_H    85
#define COVER_TEX_H 128
static uint32_t g_cover[COVER_W * COVER_TEX_H] __attribute__((aligned(16)));

static void cover_tile(const char *id)
{
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

/* Truncate to whole glyphs (8px each, 474px usable). */
static void clip(char *dst, size_t cap, const char *src, int max_chars)
{
    size_t n = 0;
    while (n + 1 < cap && n < (size_t)max_chars && src[n]) {
        dst[n] = src[n];
        n++;
    }
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
    gui_rect(0, 0, SCR_W, 16, COL_HEADER);
    gui_text(6, 4, title, COL_BLACK, 1);
    if (right && right[0])
        gui_text(SCR_W - 6 - gui_text_width(right, 1), 4, right, COL_BLACK, 1);
}

static void draw_tabs(const coverflow_state_t *cf)
{
    gui_rect(0, 16, SCR_W, 17, COL_FIELD);
    gui_rect(0, 33, SCR_W, 1, COL_LINE);

    const int n = (int)cf->dynamic_category_count;
    if (!n) return;

    int w[COVERFLOW_MAX_DYNAMIC_CATEGORIES];
    for (int i = 0; i < n; i++)
        /* Empty name = wildcard tab; the browser passes "" and the label
           convention (ui/coverflow.c) is to render it "All". */
        w[i] = gui_text_width(cf->dynamic_categories[i][0]
                                  ? cf->dynamic_categories[i] : "All", 1) + 20;

    /* Window the rail around the active tab (same job the ANSI menu_row
       windows at 60 columns). An active tab is never wider than a screen,
       so sliding `start` right always terminates the search. */
    int start = 0;
    while (start < (int)cf->category) {
        int x = 6, vis = 0;
        for (int i = start; i < n && x + w[i] <= SCR_W - 6; i++) {
            if (i == (int)cf->category) vis = 1;
            x += w[i];
        }
        if (vis) break;
        start++;
    }

    int x = 6;
    for (int i = start; i < n && x + w[i] <= SCR_W - 6; i++) {
        const int active = (i == (int)cf->category);
        if (active) {
            gui_rect(x, 16, w[i], 17, COL_CARD);
            gui_rect(x, 31, w[i], 2, COL_HEADER);
        }
        gui_text(x + 10, 21,
                 cf->dynamic_categories[i][0] ? cf->dynamic_categories[i]
                                              : "All",
                 active ? COL_TEXT : COL_DIM, 1);
        x += w[i];
    }
}

static void draw_flow(const coverflow_state_t *cf,
                      const char *const *ids, const char *const *descs)
{
    const coverflow_item_t *it = coverflow_current(cf);
    if (!it) {
        gui_text_center(SCR_W / 2, 150, "(no items in this tab)", COL_DIM, 1);
        return;
    }
    const size_t idx = cf->index;
    const size_t slot = cf->filtered[idx];

    /* Side cards: same quad, 52x70, faded via the modulate alpha. */
    if (idx > 0) {
        cover_tile(ids[cf->filtered[idx - 1]]);
        gui_tex_rect(116, 70, 52, 70, g_cover, COVER_W, COVER_TEX_H,
                     COVER_W, COVER_H, 0x59FFFFFF, 1);
    }
    if (idx + 1 < cf->filtered_len) {
        cover_tile(ids[cf->filtered[idx + 1]]);
        gui_tex_rect(312, 70, 52, 70, g_cover, COVER_W, COVER_TEX_H,
                     COVER_W, COVER_H, 0x59FFFFFF, 1);
    }

    /* Centre card: orange frame, 92x123, initials over the tile. */
    cover_tile(ids[slot]);
    gui_rect(192, 42, 96, 127, COL_HEADER);
    gui_tex_rect(194, 44, 92, 123, g_cover, COVER_W, COVER_TEX_H,
                 COVER_W, COVER_H, COL_INK, 1);
    char ini[3];
    initials(it->title, ini);
    if (ini[0])
        gui_text_center(SCR_W / 2, 86, ini, 0xAAFFFFFF, 2);

    char buf[64], meta[128];
    clip(buf, sizeof buf, it->title, 56);
    gui_text_center(SCR_W / 2, 175, buf, COL_TEXT, 1);

    meta[0] = '\0';
    int off = 0;
    if (it->version[0]) off = app(meta, sizeof meta, off, "%s  ", it->version);
    if (it->size_human[0]) off = app(meta, sizeof meta, off, "%s  ", it->size_human);
    off = app(meta, sizeof meta, off, "%s", it->category);
    if (it->is_favorite) off = app(meta, sizeof meta, off, "  * fav");
    gui_text_center(SCR_W / 2, 187, meta, COL_DIM, 1);

    clip(buf, sizeof buf, descs[slot], 57);
    if (buf[0]) gui_text_center(SCR_W / 2, 203, buf, COL_DIM, 1);

    snprintf(meta, sizeof meta, "[ %lu / %lu ]",
             (unsigned long)(idx + 1), (unsigned long)cf->filtered_len);
    gui_text_center(SCR_W / 2, 219, meta, COL_FAINT, 1);
}

static void draw_grid(const coverflow_state_t *cf, const char *const *ids)
{
    /* 4x2 page. The ANSI grid never pages a <=64-item catalog (its page
       is cols*(rows=23)=69 cells), so the pixel layout defines paging:
       same rule shape, page = index / per_page. */
    const int cols = 4, rows = 2, per = cols * rows;
    const size_t first = (cf->index / (size_t)per) * (size_t)per;
    size_t last = first + (size_t)per;
    if (last > cf->filtered_len) last = cf->filtered_len;

    size_t i = first;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols && i < last; c++, i++) {
            const size_t slot = cf->filtered[i];
            const coverflow_item_t *it = &cf->items[slot];
            const int x = 8 + c * 118, y = 40 + r * 106;

            if (i == cf->index) gui_rect(x - 2, y - 2, 116, 104, COL_HEADER);
            gui_rect(x, y, 112, 100, COL_CARD);
            cover_tile(ids[slot]);
            gui_tex_rect(x + 2, y + 2, 108, 72, g_cover, COVER_W, COVER_TEX_H,
                         COVER_W, COVER_H, COL_INK, 1);

            char buf[16];
            clip(buf, sizeof buf, it->title, 13);
            gui_text(x + 6, y + 78, buf, COL_TEXT, 1);
            clip(buf, sizeof buf, it->size_human, 13);
            gui_text(x + 6, y + 89, buf, COL_DIM, 1);
            if (it->is_favorite) gui_text(x + 98, y + 4, "*", COL_FAV, 1);
        }
    }

    const size_t pages = (cf->filtered_len + (size_t)per - 1) / (size_t)per;
    char buf[32];
    snprintf(buf, sizeof buf, "[ page %lu / %lu ]",
             (unsigned long)(first / (size_t)per + 1), (unsigned long)pages);
    gui_text_center(SCR_W / 2, 246, buf, COL_FAINT, 1);
}

static void draw_hints(int grid)
{
    gui_rect(0, 256, SCR_W, 16, COL_LINE);
    gui_text(6, 260,
             grid ? "Arrows move  L1R1 tab  Sel flow  Sq fav  X go  O back"
                  : "L/R cover  UpDn jump  L1R1 tab  Sel grid  Sq fav  X O",
             COL_TEXT, 1);
}

void store_gui_draw(const coverflow_state_t *cf,
                    const char *const *ids, const char *const *descs)
{
    gui_begin(COL_BG);
    draw_header("PS Linker", "PSP");
    draw_tabs(cf);
    if (cf->view == COVERFLOW_VIEW_GRID) draw_grid(cf, ids);
    else                                draw_flow(cf, ids, descs);
    draw_hints(cf->view == COVERFLOW_VIEW_GRID);
    gui_end();
}

/* The browser's Cover Flow frame (main.c browser()): same layout as the
 * Store's, browser keys in the hint bar, and the cwd in the header where
 * the store shows the platform tag. The 480px bar fits 57 glyphs, so the
 * browser hints are terser than the PS3 twin's. */
void store_gui_browse(const coverflow_state_t *cf,
                      const char *const *ids, const char *const *descs,
                      const char *path)
{
    gui_begin(COL_BG);
    char head[64];
    snprintf(head, sizeof head, "NAS /%.40s", path ? path : "");
    draw_header("PS Linker", head);
    draw_tabs(cf);
    if (cf->view == COVERFLOW_VIEW_GRID) draw_grid(cf, ids);
    else                                draw_flow(cf, ids, descs);

    gui_rect(0, 256, SCR_W, 16, COL_LINE);
    gui_text(6, 260,
             cf->view == COVERFLOW_VIEW_GRID
                 ? "Arrows L1R1 tab Sq flow X open O up SEL:set TRI:sto"
                 : "L/R cover L1R1 tab Sq grid X open O up SEL:set TRI:sto",
             COL_TEXT, 1);
    gui_end();
}

void store_gui_status(const char *header, const char *body, const char *hint)
{
    gui_begin(COL_BG);
    draw_header(header ? header : "PS Linker", "");
    if (body) {
        char buf[60];
        clip(buf, sizeof buf, body, 58);
        gui_text(10, 40, buf, COL_TEXT, 1);
    }
    if (hint) {
        gui_rect(0, 256, SCR_W, 16, COL_LINE);
        gui_text(6, 260, hint, COL_TEXT, 1);
    }
    gui_end();
}

void store_gui_progress(const char *title, const char *path, int pct, long total)
{
    gui_begin(COL_BG);
    draw_header("PS Linker", "installing");

    char buf[64], titlebuf[60];
    clip(titlebuf, sizeof titlebuf, title ? title : "", 56);
    gui_text_center(SCR_W / 2, 64, titlebuf, COL_TEXT, 1);

    gui_rect(110, 96, 260, 12, COL_LINE);
    gui_rect(112, 98, 256, 8, 0xFF111111);
    if (pct > 0) {
        if (pct > 100) pct = 100;
        gui_rect(112, 98, 256 * pct / 100, 8, COL_HEADER);
    }

    if (pct >= 0) snprintf(buf, sizeof buf, "%d%%   %ld KB", pct, total / 1024);
    else          snprintf(buf, sizeof buf, "%ld KB", total / 1024);
    gui_text_center(SCR_W / 2, 116, buf, COL_TEXT, 1);

    if (path && path[0]) {
        clip(buf, sizeof buf, path, 56);
        gui_text_center(SCR_W / 2, 132, buf, COL_DIM, 1);
    }

    gui_rect(0, 256, SCR_W, 16, COL_LINE);
    gui_text(6, 260, "O aborts - partial file kept, resume-ready", COL_TEXT, 1);
    gui_end();
}
