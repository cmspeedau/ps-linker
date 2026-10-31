/*
 * PS Linker - PS3 2D renderer core (RSX, CPU-drawn). See gfx.h.
 *
 * Frames are drawn by the CPU into a main-RAM back buffer (fast cached
 * memory, where the per-pixel blending the Store needs is cheap), then
 * one sequential write-combined copy hands the frame to the RSX display
 * buffer and a vsync flip presents it. The RSX is never asked to draw:
 * it only scans the buffer out, which is exactly the arrangement
 * RPCS3's emulated GPU and CFW hardware both agree on.
 */

#include <malloc.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <rsx/mm.h>
#include <sysutil/video.h>

#include "gfx.h"
#include "../ui/font8x8.h"

/* Virtual design grid; every call maps into the panel's real mode. */
#define VW 1280
#define VH 720

/* rsxInit command buffer and IO heap (host-mapped, 1 MB aligned). */
#define CMD_BUFFER_SIZE (4u * 1024u * 1024u)
#define IO_HEAP_SIZE    (8u * 1024u * 1024u)

static gcmContextData *g_ctx;
static void *g_host;
static uint32_t *g_fb[2];            /* RSX-side display buffers       */
static uint32_t  g_fb_size;          /* bytes per buffer                */
static uint32_t *g_back;             /* main-RAM back buffer            */
static int g_pw, g_ph;               /* physical pixels                 */
static uint32_t g_pitch;             /* byte pitch (= pw * 4)           */
static int g_cur = 0;                /* buffer the next present writes  */
static int g_ready = 0;

/* ---- geometry mapping (virtual -> physical) ---------------------------
 * Edges map, spans subtract, so adjacent rects always tile the physical
 * screen with no gaps whatever the ratio (1:1 at 720p, 1.5x at 1080i,
 * 0.56x at 480i). */

static int mx(int x)
{
    long v = (long)x * g_pw / VW;
    if (v < 0) return 0;
    if (v > g_pw) return g_pw;
    return (int)v;
}

static int my(int y)
{
    long v = (long)y * g_ph / VH;
    if (v < 0) return 0;
    if (v > g_ph) return g_ph;
    return (int)v;
}

/* ---- primitives on the back buffer ------------------------------------- */

static void blend_px(uint32_t *dst, uint32_t c)
{
    const unsigned a = c >> 24;
    if (a >= 255) { *dst = c | 0xFF000000u; return; }
    if (a == 0) return;
    const uint32_t d = *dst;
    const unsigned ia = 255u - a;
    const unsigned r = (((c >> 16 & 255) * a) + ((d >> 16 & 255) * ia)) / 255u;
    const unsigned g = (((c >>  8 & 255) * a) + ((d >>  8 & 255) * ia)) / 255u;
    const unsigned b = (((c & 255) * a) + ((d & 255) * ia)) / 255u;
    *dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Fill a virtual rect with one color (alpha blends over the frame). */
static void paint(int x, int y, int w, int h, uint32_t c)
{
    const int x0 = mx(x), x1 = mx(x + w);
    const int y0 = my(y), y1 = my(y + h);
    if (x1 <= x0 || y1 <= y0 || !g_back) return;
    const unsigned a = c >> 24;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = g_back + (size_t)yy * g_pw;
        if (a >= 255) {
            for (int xx = x0; xx < x1; xx++) row[xx] = c | 0xFF000000u;
        } else if (a > 0) {
            for (int xx = x0; xx < x1; xx++) blend_px(&row[xx], c);
        }
    }
}

void gfx_rect(int x, int y, int w, int h, unsigned int argb)
{
    if (!g_ready) return;
    paint(x, y, w, h, (uint32_t)argb);
}

/* ---- text --------------------------------------------------------------
 * One font bit = one virtual `scale`-px square; the square's corners go
 * through the same mapping as every other rect, so glyphs stay aligned
 * with the chrome at any panel mode. */

int gfx_text_width(const char *s, int scale)
{
    int n = 0;
    for (const char *p = s; *p; p++) n++;
    return n * 8 * scale;
}

void gfx_text(int x, int y, const char *s, unsigned int argb, int scale)
{
    if (!g_ready || scale < 1) return;
    int cx = x;
    for (const char *p = s; *p; p++, cx += 8 * scale) {
        unsigned ch = (unsigned char)*p;
        if (ch < 0x20 || ch > 0x7E) continue;   /* skipped, still advances */
        for (int row = 0; row < 8; row++) {
            const unsigned char bits = (unsigned char)font8x8_basic[ch][row];
            for (int col = 0; col < 8; col++)
                if (bits & (1u << col))
                    paint(cx + col * scale, y + row * scale, scale, scale,
                          (uint32_t)argb);
        }
    }
}

void gfx_text_center(int cx, int y, const char *s, unsigned int argb, int scale)
{
    gfx_text(cx - gfx_text_width(s, scale) / 2, y, s, argb, scale);
}

/* ---- image blit --------------------------------------------------------
 * Nearest sampling, one source texel at a time: its destination virtual
 * square maps to a physical column span that is filled (tinted, alpha
 * blended). `linear` is accepted for psp/gui.h parity and ignored. */

void gfx_tex_rect(int x, int y, int w, int h, const void *img,
                  int tex_w, int tex_h, int src_w, int src_h,
                  unsigned int modulate, int linear)
{
    (void)linear;
    if (!g_ready || !img || w <= 0 || h <= 0 || src_w <= 0 || src_h <= 0) return;
    const uint32_t *src = (const uint32_t *)img;
    if (src_w > tex_w) src_w = tex_w;
    if (src_h > tex_h) src_h = tex_h;

    const unsigned ma = (modulate >> 24) & 255u;
    const unsigned mr = (modulate >> 16) & 255u;
    const unsigned mg = (modulate >>  8) & 255u;
    const unsigned mb = modulate & 255u;
    const int plain = (modulate == 0xFFFFFFFFu);

    for (int sy = 0; sy < src_h; sy++) {
        const int vy = y + sy * h / src_h;
        const int vy2 = y + (sy + 1 == src_h ? h : (sy + 1) * h / src_h);
        const int py0 = my(vy), py1 = my(vy2);
        if (py1 <= py0) continue;
        const uint32_t *srow = src + (size_t)sy * (size_t)tex_w;
        for (int sx = 0; sx < src_w; sx++) {
            const int vx = x + sx * w / src_w;
            const int vx2 = x + (sx + 1 == src_w ? w : (sx + 1) * w / src_w);
            const int px0 = mx(vx), px1 = mx(vx2);
            if (px1 <= px0) continue;
            uint32_t c = srow[sx];
            if (!plain) {
                const unsigned r = ((c >> 16 & 255u) * mr + 127u) / 255u;
                const unsigned g = ((c >>  8 & 255u) * mg + 127u) / 255u;
                const unsigned b = ((c & 255u) * mb + 127u) / 255u;
                c = (ma << 24) | (r << 16) | (g << 8) | b;
            }
            for (int yy = py0; yy < py1; yy++) {
                uint32_t *row = g_back + (size_t)yy * g_pw;
                for (int xx = px0; xx < px1; xx++) blend_px(&row[xx], c);
            }
        }
    }
}

/* ---- flip / present ---------------------------------------------------- */

static void wait_flip(void)
{
    while (gcmGetFlipStatus() != 0) usleep(200);
    gcmResetFlipStatus();
}

void gfx_begin(unsigned int bg)
{
    if (!g_ready) return;
    uint32_t *p = g_back, *end = g_back + (size_t)(g_pitch / 4) * g_ph;
    const uint32_t c = (uint32_t)bg | 0xFF000000u;
    while (p < end) *p++ = c;
}

void gfx_end(void)
{
    if (!g_ready) return;
    /* Sequential write-combined stores are the only fast way into RSX
       memory from the PPU; the sync orders them before the flip. */
    volatile uint32_t *dst = g_fb[g_cur];
    const uint32_t *src = g_back;
    const size_t words = (size_t)(g_pitch / 4) * g_ph;
    for (size_t i = 0; i < words; i++) dst[i] = src[i];
    __sync_synchronize();

    gcmSetFlip(g_ctx, (u32)g_cur);
    rsxFlushBuffer(g_ctx);
    wait_flip();
    g_cur ^= 1;
}

/* ---- lifecycle --------------------------------------------------------- */

static void gfx_fail(void)
{
    for (int i = 0; i < 2; i++)
        if (g_fb[i]) { rsxFree(g_fb[i]); g_fb[i] = NULL; }
    free(g_back);   g_back = NULL;
    free(g_host);   g_host = NULL;
    g_ready = 0;
}

int gfx_init(void)
{
    if (g_ready) return 0;

    g_host = memalign(1024 * 1024, IO_HEAP_SIZE);
    if (!g_host) return -1;

    g_ctx = rsxInit(CMD_BUFFER_SIZE, IO_HEAP_SIZE, g_host);
    if (!g_ctx) { free(g_host); g_host = NULL; return -1; }

    videoState state;
    videoResolution res;
    if (videoGetState(0, 0, &state) != 0 ||
        videoGetResolution(state.displayMode.resolution, &res) != 0) {
        gfx_fail(); return -1;
    }
    g_pw = res.width;
    g_ph = res.height;
    g_pitch = (uint32_t)g_pw * 4u;

    videoConfiguration vconfig;
    memset(&vconfig, 0, sizeof(vconfig));
    vconfig.resolution = state.displayMode.resolution;
    vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.aspect = state.displayMode.aspect;
    vconfig.pitch = g_pitch;
    if (videoConfigure(0, &vconfig, NULL, 0) != 0) { gfx_fail(); return -1; }

    gcmSetFlipMode(GCM_FLIP_VSYNC);

    g_fb_size = g_pitch * (uint32_t)g_ph;
    for (int i = 0; i < 2; i++) {
        g_fb[i] = (uint32_t *)rsxMemalign(64, g_fb_size);
        uint32_t offset;
        if (!g_fb[i] || rsxAddressToOffset(g_fb[i], &offset) != 0 ||
            gcmSetDisplayBuffer((u32)i, offset, g_pitch,
                                (u32)g_pw, (u32)g_ph) != 0) {
            gfx_fail(); return -1;
        }
    }

    g_back = (uint32_t *)malloc(g_fb_size);
    if (!g_back) { gfx_fail(); return -1; }

    /* Black both buffers, present the first and wait for the flip to
       land, so the caller's first real frame never races the scanout. */
    for (int i = 0; i < 2; i++) {
        volatile uint32_t *d = g_fb[i];
        for (size_t w = 0; w < g_fb_size / 4; w++) d[w] = 0xFF000000u;
    }
    __sync_synchronize();
    gcmResetFlipStatus();
    gcmSetFlip(g_ctx, 0);
    rsxFlushBuffer(g_ctx);
    wait_flip();
    g_cur = 1;

    g_ready = 1;
    return 0;
}

void gfx_term(void)
{
    if (!g_ready) return;
    g_ready = 0;
    wait_flip();
    rsxFinish(g_ctx, 1);
    for (int i = 0; i < 2; i++) { if (g_fb[i]) rsxFree(g_fb[i]); g_fb[i] = NULL; }
    free(g_back);  g_back = NULL;
    free(g_host);  g_host = NULL;
    g_ctx = NULL;
}

int gfx_ready(void) { return g_ready; }
