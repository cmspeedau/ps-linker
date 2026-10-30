#ifndef STORE_LOGIC_H
#define STORE_LOGIC_H
#include <stddef.h>
#include <stdint.h>

#define STORE_CATEGORY_LIMIT 8
#define STORE_CATEGORY_LEN 32

/* Add a unique, non-empty catalog category, preserving first-seen order.
   Returns 1 if added, 0 if empty/duplicate/full or arguments are invalid. */
int store_category_add(char categories[STORE_CATEGORY_LIMIT][STORE_CATEGORY_LEN],
                       int *count, const char *category);

void store_format_size(uint64_t bytes, char *out, size_t outlen);
/* Derive the ms0: destination for a catalog item.
   Returns 0 on success, -1 if the type is not installable, -2 if the path
   does not fit in `out`. */
int store_derive_path(const char *url, const char *title,
                      char *out, size_t outlen);

/* ---- id-hashed placeholder cover tiles ---------------------------------
 * The web GUI draws an icon-less item's cover as a gradient keyed to a
 * hash of the item id (server/store/ui/state.js `placeholderBG`). The
 * device must reproduce that hash exactly so one item keeps the same
 * look on every surface. */

/* Hue in degrees for an item id, hashing the way state.js does:
   h = 0; per byte: h = (h*31 + byte) & 0x3fffffff; hue = h % 360. */
int store_cover_hue(const char *id);

/* HSL -> 8-bit RGB. h is degrees (any int), s and l percent 0..100
   (clamped). Rounds half-up like CSS colour conversion. */
void store_hsl_to_rgb(int h, int s, int l, unsigned char out[3]);

/* Fill w*h pixels of the item's placeholder cover tile, row-major, one
   0xFFRRGGBB word per pixel (the app's debug-palette convention): a
   vertical blend from hsl(hue,42,26) to hsl(hue+40,52,12) — the web
   gradient's two stops. */
void store_cover_pixels(uint32_t *buf, int w, int h, const char *id);

#endif
