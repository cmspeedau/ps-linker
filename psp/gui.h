/*
 * PS Linker - PSP 2D renderer core (sceGu)
 *
 * The pixel layer under the Store's graphics view: a double-buffered
 * 480x272 screen offering filled rects, 8x8 bitmap-font text and one
 * textured-quad primitive. Everything the Store draws is a composition
 * of these three.
 *
 * Colours are 0xAARRGGBB — the same convention the debug-screen palette
 * in main.c/store.c uses — so screens can hand a colour between the
 * text and pixel paths unchanged.
 */

#ifndef PSP_GUI_H
#define PSP_GUI_H

/* Start the GU pipeline and build the font atlas. Returns 0 on success. */
int  gui_init(void);

/* Tear the GU pipeline down; afterwards the debug screen can be re-
 * initialised and owns the framebuffer again. */
void gui_term(void);

/* One frame: gui_begin() clears to `bg` (0xAARRGGBB), draw calls follow,
 * gui_end() finishes the list, waits for vblank and swaps. */
void gui_begin(unsigned int bg);
void gui_end(void);

/* Axis-aligned fill, no texture, usually opaque (alpha FF). */
void gui_rect(int x, int y, int w, int h, unsigned int argb);

/* Text width in pixels for the glyph count in `s` (all chars advance one
 * 8px cell, scaled). */
int  gui_text_width(const char *s, int scale);

/* Draw `s` at (x,y) top-left, `scale` = 1 or 2. Characters outside
 * 0x20..0x7E are skipped but still advance. */
void gui_text(int x, int y, const char *s, unsigned int argb, int scale);

/* Same, centred horizontally on cx. */
void gui_text_center(int cx, int y, const char *s, unsigned int argb, int scale);

/* Textured quad: image is a CPU-side 0xFFRRGGBB array of tex_w x tex_h
 * words (16-byte aligned), of which the top-left src_w x src_h texels are
 * mapped onto the destination rect x,y,w,h. `modulate` tints and dims
 * (its alpha fades the card). linear=1 bilinear-filters the scale. */
void gui_tex_rect(int x, int y, int w, int h, const void *img,
                  int tex_w, int tex_h, int src_w, int src_h,
                  unsigned int modulate, int linear);

#endif /* PSP_GUI_H */
