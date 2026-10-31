/*
 * PS Linker - PS3 2D renderer core (RSX, CPU-drawn)
 *
 * The pixel layer under the whole PS3 app: two RSX display buffers are
 * registered for scanout, every frame is drawn into a main-RAM back
 * buffer by the CPU, copied to the current buffer and flipped on vsync.
 * No RSX draws, shaders or surface setup are involved - the GPU only
 * scans the memory out - which is what makes this reliable under RPCS3
 * and on CFW alike. The API mirrors psp/gui.h so the Store's drawing
 * code ports across nearly 1:1.
 *
 * Coordinates are VIRTUAL 1280x720 pixels: every call maps through the
 * panel's real resolution (720p maps 1:1, 1080i 1.5x, 480i/576i downs),
 * so the layouts are written once for the HD grid and stay right on any
 * TV the XMB reports.
 *
 * Colours are 0xAARRGGBB - the convention shared with psp/gui.h and the
 * debug-palette screens - and an alpha below FF blends over what is
 * already in the frame.
 */

#ifndef PS3_GFX_H
#define PS3_GFX_H

/* Open the RSX + video pipeline at the resolution the XMB reports and
 * allocate the back buffer. Returns 0 on success; after a failure the
 * caller keeps the ANSI UI and every other call here is inert. */
int  gfx_init(void);

/* Tear everything down and release the display (the TTY owns the screen
 * again; on real hardware the XMB takes the display back at exit). */
void gfx_term(void);

/* 1 once gfx_init() succeeded. */
int  gfx_ready(void);

/* One frame: gfx_begin() clears the back buffer to `bg` (0xAARRGGBB),
 * draw calls follow, gfx_end() copies to the RSX buffer, flips and
 * waits for vsync. */
void gfx_begin(unsigned int bg);
void gfx_end(void);

/* Axis-aligned fill. */
void gfx_rect(int x, int y, int w, int h, unsigned int argb);

/* Text width in virtual pixels for the glyph count in `s` (all chars
 * advance one 8-virtual-px cell, scaled). */
int  gfx_text_width(const char *s, int scale);

/* Draw `s` at (x,y) top-left, `scale` virtual px per font pixel (1 =
 * 8px glyphs, 3 = the size the 720p layouts use). Chars outside
 * 0x20..0x7E are skipped but still advance. */
void gfx_text(int x, int y, const char *s, unsigned int argb, int scale);

/* Same, centred horizontally on cx. */
void gfx_text_center(int cx, int y, const char *s, unsigned int argb, int scale);

/* Nearest-neighbour image blit: img is a CPU-side 0xFFRRGGBB array of
 * tex_w x tex_h words, of which the top-left src_w x src_h texels map
 * onto the destination rect x,y,w,h. `modulate` tints and (by its
 * alpha) fades. `linear` is accepted and ignored - the CPU sampler is
 * nearest. */
void gfx_tex_rect(int x, int y, int w, int h, const void *img,
                  int tex_w, int tex_h, int src_w, int src_h,
                  unsigned int modulate, int linear);

#endif /* PS3_GFX_H */
