/*
 * PS Linker - PSP 2D renderer core (sceGu), implementation
 *
 * The init sequence, vertex layout and the synchronous vblank loop mirror
 * the `font` sample shipped with pspsdk (samples/gu/font), because it is
 * the same job: 2D textured sprites at fixed screen coordinates. One
 * simplification over most homebrew: sceGuTexImage() and sceGuDrawArray()
 * accept main-RAM arrays, so textures and the font atlas are plain aligned
 * statics — no VRAM allocator bookkeeping, no per-frame re-upload.
 *
 * Vertex attribute order (texcoord, colour, position) is the order the GE
 * packs for the flag set used below — again exactly as the SDK sample.
 */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>

#include <string.h>

#include "gui.h"
#include "font8x8.h"

#define GUI_SCR_W   480
#define GUI_SCR_H   272
#define GUI_BUF_W   512
#define GUI_FRAME   (GUI_BUF_W * GUI_SCR_H * 4)   /* one 8888 frame at 512 pitch */

#define GUI_ATLAS_W 128
#define GUI_ATLAS_H 128                            /* 16x16 cells of 8x8 */
#define GUI_CELL      8
#define GUI_MAX_GLYPHS 96                          /* longest batched string */

typedef struct {
    float u, v;
    unsigned int color;
    float x, y, z;
} __attribute__((packed)) GuiVertex;

/* Display list + font atlas + text batch: statics, so a failed Store
 * visit never leaks heap and the browser path pays nothing. */
static unsigned int __attribute__((aligned(16))) g_list[16384];
static unsigned int __attribute__((aligned(16))) g_atlas[GUI_ATLAS_W * GUI_ATLAS_H];

/* 16x16 opaque white — a "rect" is a quad of this modulated to the fill
   colour. Flat GU_SPRITES without GU_TEXTURE_32BITF would need the vertex
   data to START with the colour (the GE orders TXTR,CMND,POSITN and packs
   only what the flags enable), which the shared GuiVertex layout does not
   match; reusing the textured path avoids a second vertex struct entirely. */
#define GUI_WHITE_S 16
static unsigned int __attribute__((aligned(16))) g_white[GUI_WHITE_S * GUI_WHITE_S];
static int g_ready;

/* 0xAARRGGBB (app palette) -> 0xAABBGGRR (GE byte order). */
static unsigned int ge_color(unsigned int argb)
{
    const unsigned int a = argb & 0xFF000000u;
    const unsigned int r = (argb >> 16) & 0xFFu;
    const unsigned int g = (argb >> 8) & 0xFFu;
    const unsigned int b = argb & 0xFFu;
    return a | (b << 16) | (g << 8) | r;
}

int gui_init(void)
{
    if (g_ready) return 0;

    /* Glyph atlas: white ink on transparent, so a vertex colour tints the
       text and its alpha fades it (GU_TFX_MODULATE below). */
    memset(g_atlas, 0, sizeof g_atlas);
    for (int ch = ' '; ch <= '~'; ch++) {
        const int cell = ch - ' ';
        const int ox = (cell % 16) * GUI_CELL, oy = (cell / 16) * GUI_CELL;
        for (int row = 0; row < GUI_CELL; row++) {
            /* font8x8: row 0 is the top, bit 0 the leftmost pixel. */
            const unsigned char bits = (unsigned char)font8x8_basic[ch][row];
            for (int col = 0; col < GUI_CELL; col++)
                if (bits & (1u << col))
                    g_atlas[(size_t)(oy + row) * GUI_ATLAS_W + (size_t)(ox + col)] = 0xFFFFFFFFu;
        }
    }
    sceKernelDcacheWritebackAll();
    memset(g_white, 0xFF, sizeof g_white);
    sceKernelDcacheWritebackAll();

    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_8888, NULL, GUI_BUF_W);
    sceGuDispBuffer(GUI_SCR_W, GUI_SCR_H, (void *)(uintptr_t)GUI_FRAME, GUI_BUF_W);
    sceGuDepthBuffer((void *)(uintptr_t)(GUI_FRAME * 2), GUI_BUF_W);
    sceGuOffset(2048 - (GUI_SCR_W / 2), 2048 - (GUI_SCR_H / 2));
    sceGuViewport(2048, 2048, GUI_SCR_W, GUI_SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, GUI_SCR_W, GUI_SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    g_ready = 1;
    return 0;
}

void gui_term(void)
{
    if (!g_ready) return;
    sceGuTerm();
    g_ready = 0;
}

void gui_begin(unsigned int bg)
{
    if (!g_ready) return;
    sceGuStart(GU_DIRECT, g_list);
    sceGuClearColor(ge_color(bg));
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
}

void gui_end(void)
{
    if (!g_ready) return;
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

static void tex_bind(const void *img, int w, int h, int linear)
{
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(linear ? GU_LINEAR : GU_NEAREST,
                   linear ? GU_LINEAR : GU_NEAREST);
    sceGuTexImage(0, w, h, w, img);
}

void gui_rect(int x, int y, int w, int h, unsigned int argb)
{
    if (!g_ready || w <= 0 || h <= 0) return;
    GuiVertex v[2];
    const unsigned int c = ge_color(argb);
    tex_bind(g_white, GUI_WHITE_S, GUI_WHITE_S, 0);
    v[0].u = 0; v[0].v = 0; v[0].color = c;
    v[0].x = (float)x; v[0].y = (float)y; v[0].z = 0;
    v[1].u = GUI_WHITE_S; v[1].v = GUI_WHITE_S; v[1].color = c;
    v[1].x = (float)(x + w); v[1].y = (float)(y + h); v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_COLOR_8888 |
                   GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   2, NULL, v);
}

void gui_text(int x, int y, const char *s, unsigned int argb, int scale)
{
    if (!g_ready || !s) return;
    if (scale < 1) scale = 1;

    GuiVertex batch[GUI_MAX_GLYPHS * 2];
    const unsigned int c = ge_color(argb);
    int n = 0, cx = x;

    tex_bind(g_atlas, GUI_ATLAS_W, GUI_ATLAS_H, 0);

    for (; *s && n < GUI_MAX_GLYPHS; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch >= ' ' && ch <= '~') {
            const int cell = ch - ' ';
            const float u0 = (float)((cell % 16) * GUI_CELL);
            const float v0 = (float)((cell / 16) * GUI_CELL);
            GuiVertex *a = &batch[n * 2], *b = &batch[n * 2 + 1];
            a->u = u0; a->v = v0; a->color = c;
            a->x = (float)cx; a->y = (float)y; a->z = 0;
            b->u = u0 + GUI_CELL; b->v = v0 + GUI_CELL; b->color = c;
            b->x = (float)(cx + GUI_CELL * scale);
            b->y = (float)(y + GUI_CELL * scale); b->z = 0;
            n++;
        }
        cx += GUI_CELL * scale;
    }

    if (n)
        sceGuDrawArray(GU_SPRITES,
                       GU_TEXTURE_32BITF | GU_COLOR_8888 |
                       GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       n * 2, NULL, batch);
}

int gui_text_width(const char *s, int scale)
{
    if (scale < 1) scale = 1;
    return s ? (int)strlen(s) * GUI_CELL * scale : 0;
}

void gui_text_center(int cx, int y, const char *s, unsigned int argb, int scale)
{
    gui_text(cx - gui_text_width(s, scale) / 2, y, s, argb, scale);
}

void gui_tex_rect(int x, int y, int w, int h, const void *img,
                  int tex_w, int tex_h, int src_w, int src_h,
                  unsigned int modulate, int linear)
{
    if (!g_ready || !img || w <= 0 || h <= 0) return;
    if (src_w > tex_w) src_w = tex_w;
    if (src_h > tex_h) src_h = tex_h;

    GuiVertex v[2];
    const unsigned int c = ge_color(modulate);
    tex_bind(img, tex_w, tex_h, linear);
    v[0].u = 0; v[0].v = 0; v[0].color = c;
    v[0].x = (float)x; v[0].y = (float)y; v[0].z = 0;
    v[1].u = (float)src_w; v[1].v = (float)src_h; v[1].color = c;
    v[1].x = (float)(x + w); v[1].y = (float)(y + h); v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_COLOR_8888 |
                   GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   2, NULL, v);
}
