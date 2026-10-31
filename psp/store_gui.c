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

/* Downloads rows: one file per line, name left, progress bar and pct
 * right (the install screen's orange fill). `progress` is the per-slot
 * pct the browser passes: 100 for finished files, 0..99 or -1 for an
 * interrupted .part (unknown total draws a dim "paused" bar). */
#define DL_ROWS 12
static void draw_downloads(const coverflow_state_t *cf,
                           const char *const *ids, const int *progress)
{
    if (cf->filtered_len == 0) {
        gui_text_center(SCR_W / 2, 140, "(no downloads yet)", COL_DIM, 1);
        return;
    }

    /* Window the list so the selected row stays visible (same rule the
       grid paging applies per page). */
    size_t first = 0;
    if (cf->index >= (size_t)DL_ROWS) first = cf->index - (size_t)DL_ROWS + 1;
    size_t last = first + (size_t)DL_ROWS;
    if (last > cf->filtered_len) last = cf->filtered_len;

    for (size_t i = first; i < last; i++) {
        const size_t slot = cf->filtered[i];
        const int sel = (i == cf->index);
        const int y = 40 + (int)(i - first) * 18;

        if (sel) {
            gui_rect(0, y, SCR_W, 18, COL_CARD);
            gui_rect(0, y, 3, 18, COL_HEADER);
        }

        char buf[40];
        clip(buf, sizeof buf, ids[slot], 33);
        gui_text(10, y + 5, buf, sel ? COL_TEXT : COL_DIM, 1);

        const int bx = 286, bw = 104, by = y + 5, bh = 8;
        const int pct = progress[slot];
        gui_rect(bx - 1, by - 1, bw + 2, bh + 2, COL_LINE);
        gui_rect(bx, by, bw, bh, 0xFF111111);
        if (pct > 0) gui_rect(bx, by, bw * (pct > 100 ? 100 : pct) / 100,
                              bh, COL_HEADER);

        if (pct >= 0) {
            snprintf(buf, sizeof buf, "%d%%", pct > 100 ? 100 : pct);
            gui_text(SCR_W - 6 - 4 * 8, y + 5, buf, sel ? COL_TEXT : COL_FAINT, 1);
        } else {
            gui_text(SCR_W - 6 - 6 * 8, y + 5, "paused", sel ? COL_TEXT : COL_FAINT, 1);
        }
    }
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
 * browser hints are terser than the PS3 twin's. `progress` (per-item-slot
 * pct, see draw_downloads) selects the Downloads row list instead of the
 * covers: the browser passes it only while the Downloads tab is open.
 * `menu` (the gclite categories popup, NULL when closed) dims the frame,
 * draws the rows on top and swaps in the popup hint keys. */
/* The browser's gclite categories popup: dim the frame, then one row per
 * category mirroring the tab rail - mock cover tile (the same id-hashed
 * placeholder as a game card), label and item count. The browser owns
 * the keys; this only draws m->rows with m->sel highlighted. */
static void draw_menu(const store_menu_t *m)
{
    gui_rect(0, 0, SCR_W, SCR_H, 0x99000000);

    int rows = m->n;
    if (rows > COVERFLOW_MAX_DYNAMIC_CATEGORIES)
        rows = COVERFLOW_MAX_DYNAMIC_CATEGORIES;
    const int pw = 300, rowh = 26, hh = 18;
    const int ph = hh + rows * rowh + 4;
    const int px = (SCR_W - pw) / 2;
    const int py = 16 + (240 - ph) / 2;

    gui_rect(px - 1, py - 1, pw + 2, ph + 2, COL_LINE);
    gui_rect(px, py, pw, ph, COL_FIELD);
    gui_rect(px, py, pw, hh, COL_HEADER);
    gui_text(px + 8, py + 5, "CATEGORIES", COL_BLACK, 1);

    for (int i = 0; i < rows; i++) {
        const store_menu_row_t *r = &m->rows[i];
        const int sel = (i == m->sel);
        const int y = py + hh + i * rowh;

        if (sel) {
            gui_rect(px + 2, y, pw - 4, rowh, COL_CARD);
            gui_rect(px + 2, y, 3, rowh, COL_HEADER);
        }

        cover_tile(r->id);
        gui_tex_rect(px + 8, y + 1, 20, 26, g_cover, COVER_W, COVER_TEX_H,
                     COVER_W, COVER_H, COL_INK, 1);
        char ini[3];
        initials(r->name, ini);
        if (ini[0]) gui_text_center(px + 18, y + 9, ini, 0xAAFFFFFF, 1);

        char buf[40];
        clip(buf, sizeof buf, r->name, 26);
        gui_text(px + 36, y + 9, buf, sel ? COL_TEXT : COL_DIM, 1);

        snprintf(buf, sizeof buf, "%d", r->count);
        gui_text(px + pw - 8 - gui_text_width(buf, 1), y + 9, buf,
                 sel ? COL_TEXT : COL_FAINT, 1);
    }
}

void store_gui_browse(const coverflow_state_t *cf,
                      const char *const *ids, const char *const *descs,
                      const char *path, const int *progress,
                      const store_menu_t *menu)
{
    gui_begin(COL_BG);
    char head[64];
    snprintf(head, sizeof head, "NAS /%.40s", path ? path : "");
    draw_header("PS Linker", head);
    draw_tabs(cf);
    if (progress)                         draw_downloads(cf, ids, progress);
    else if (cf->view == COVERFLOW_VIEW_GRID) draw_grid(cf, ids);
    else                                     draw_flow(cf, ids, descs);

    if (menu) draw_menu(menu);

    gui_rect(0, 256, SCR_W, 16, COL_LINE);
    gui_text(6, 260,
             menu ? "UpDn pick  X jump  O or Start close"
             : progress ? "L/R row  UpDn jump  L1R1 tab  X info  O leave"
             : cf->view == COVERFLOW_VIEW_GRID
                  ? "Arrows L1R1 tab Sq flow X open O up SEL:util TRI:sto"
                  : "L/R cover L1R1 tab Sq grid X open O up SEL:util TRI:sto",
             COL_TEXT, 1);
    gui_end();
}

/* The Utilities page: SELECT's home, browser-style frame. Two focusable
 * rows - the NAS server URL and one labelled pill switch per option (one
 * switch today, the gclite feature). The focused row gets the Downloads
 * list's highlight (card row + orange bar); the pill shows OFF/ON, knob
 * left/right, dim orange when ON. `installed` is the
 * ms0:/seplugins/vsh.txt detection, so the page also answers "is the
 * plugin even there". */
void store_gui_utilities(int enabled, int installed,
                         const char *url, int focus)
{
    gui_begin(COL_BG);
    draw_header("PS Linker", "utilities");

    gui_rect(0, 16, SCR_W, 1, COL_LINE);

    /* the NAS server URL row (focus 0) */
    gui_text(20, 24, "NAS SERVER URL", COL_HEADER, 2);
    const int uy = 48;
    gui_rect(0, uy, SCR_W, 26, focus == 0 ? COL_CARD : COL_FIELD);
    gui_rect(0, uy, SCR_W, 1, COL_LINE);
    gui_rect(0, uy + 25, SCR_W, 1, COL_LINE);
    if (focus == 0) {
        gui_rect(0, uy, 3, 26, COL_HEADER);
        const char *e = "X edit";
        gui_text(SCR_W - 6 - gui_text_width(e, 1), uy + 8, e, COL_FAINT, 1);
    }
    char shown[64];
    clip(shown, sizeof shown, url, 50);
    if (shown[0]) gui_text(20, uy + 8, shown, COL_TEXT, 1);
    else          gui_text(20, uy + 8, "(not set - X to type one)",
                           COL_FAINT, 1);

    /* the gclite switch row (focus 1) */
    gui_text(20, 88, "GAME CATEGORIES", COL_HEADER, 2);
    gui_text(20, 112, "game-categories-lite: CAT_05Shooters shows as Shooters.",
             COL_DIM, 1);

    const int ry = 132;
    if (focus == 1) {
        gui_rect(0, ry, SCR_W, 34, COL_CARD);
        gui_rect(0, ry, 3, 34, COL_HEADER);
    }
    gui_text(20, ry + 13, "GCLITE CATEGORIES", COL_TEXT, 1);

    const int tx = SCR_W - 6 - 68, ty = ry + 8;      /* pill track */
    gui_rect(tx - 1, ty - 1, 70, 20, COL_LINE);
    gui_rect(tx, ty, 68, 18, COL_FIELD);
    const int knob_x = enabled ? tx + 68 - 32 : tx;
    gui_rect(knob_x, ty, 32, 18, enabled ? COL_HEADER : 0xFF5A5A5A);
    gui_text(knob_x + (enabled ? 8 : 4), ty + 5,
             enabled ? "ON" : "OFF", COL_BLACK, 1);
    /* the inactive side's label stays visible behind the track */
    gui_text(tx + (enabled ? 6 : 40), ty + 5, enabled ? "OFF" : "ON",
             COL_FAINT, 1);

    /* detection line: does this PSP have the plugin? */
    gui_rect(0, ry + 42, SCR_W, 1, COL_LINE);
    if (installed) {
        gui_text(20, ry + 52, "plugin: installed - START opens the menu",
                 COL_TEXT, 1);
    } else {
        gui_text(20, ry + 52,
                 "plugin: not found in ms0:/seplugins -", COL_DIM, 1);
        gui_text(20, ry + 66, "START stays inert whatever this switch says",
                 COL_DIM, 1);
    }
    if (!enabled)
        gui_text(20, ry + 84, "off: folders keep their CAT_ names",
                 COL_FAINT, 1);

    gui_rect(0, 256, SCR_W, 16, COL_LINE);
    gui_text(6, 260, "UpDn pick  X edit or toggle  O or Start save",
             COL_TEXT, 1);
    gui_end();
}

/* The URL editor's key slots, in strip order: a-z on the top row, then
 * digits and URL punctuation, then <del and OK. Each returns the glyph
 * cell's top-left (x,y) and label width; pitch is one 8px glyph plus a
 * 2px gap, and the two rows are centred (row 2 is narrower). */
#define URLK_PITCH  10
#define URLK_ROW0_N 26                          /* a-z */
static void urlk_slot(int slot, int *x, int *y, int *w)
{
    if (slot < URLK_ROW0_N) {                   /* a-z row */
        *x = (SCR_W - URLK_ROW0_N * URLK_PITCH) / 2
             + slot * URLK_PITCH + 1;
        *y = 118;
        *w = 8;
    } else {
        /* the remaining chars (digits, - . : / _) + < + OK (2 cells) */
        const int nchars = (int)strlen(STORE_URL_CHARS);
        const int row1_cells = (nchars - URLK_ROW0_N) + 1 + 2;
        const int x0 = (SCR_W - row1_cells * URLK_PITCH) / 2;
        const int col = slot - URLK_ROW0_N;     /* 0..(row1_cells-1) */
        *x = x0 + col * URLK_PITCH + 1;
        *y = 146;
        *w = (slot == nchars + 1) ? 16 : 8;     /* OK is two cells */
    }
}

void store_gui_url_edit(const char *url, int pick)
{
    gui_begin(COL_BG);
    draw_header("PS Linker", "edit url");
    gui_rect(0, 16, SCR_W, 1, COL_LINE);

    gui_text(20, 26, "NAS SERVER URL", COL_HEADER, 2);

    /* the value: tail-anchored so a long URL always shows its end, and
     * a caret marks where X and O act */
    gui_rect(0, 54, SCR_W, 26, COL_FIELD);
    gui_rect(0, 54, SCR_W, 1, COL_LINE);
    gui_rect(0, 79, SCR_W, 1, COL_LINE);
    const int len = (int)strlen(url);
    const int vis = (SCR_W - 24) / 8;
    const char *tail = len > vis ? url + len - vis : url;
    if (len) gui_text(8, 62, tail, COL_TEXT, 1);
    else     gui_text(8, 62, "(type one)", COL_FAINT, 1);
    gui_rect(10 + (len > vis ? vis : len) * 8, 60, 2, 14, COL_HEADER);

    /* the key strip */
    const char *chars = STORE_URL_CHARS;
    const int nchars = (int)strlen(chars);
    const int nslots = nchars + 2;              /* + < + OK */
    for (int slot = 0; slot < nslots; slot++) {
        int x, y, w;
        urlk_slot(slot, &x, &y, &w);
        const int sel = (slot == pick);
        const char *lab = NULL;
        char chbuf[2] = { 0, 0 };
        if (slot < nchars)      { chbuf[0] = chars[slot]; lab = chbuf; }
        else if (slot == nchars)     lab = "<";              /* delete */
        else                        lab = "OK";
        if (sel) gui_rect(x - 2, y - 3, w + 4, 17, COL_HEADER);
        gui_text(x, y, lab, sel ? COL_BLACK
                                : (slot < nchars ? COL_TEXT : COL_DIM), 1);
    }

    gui_rect(0, 256, SCR_W, 16, COL_LINE);
    gui_text(6, 260, "LR key  X insert  O delete  Start ok  Sel cancel",
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
