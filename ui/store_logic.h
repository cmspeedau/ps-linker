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

/* PS3 twin: the /dev_hdd0 destination for a catalog item.
   - .pkg              -> /dev_hdd0/packages/   (installed from XMB afterwards)
   - PS3 .iso          -> /dev_hdd0/PS3ISO/
   - PSP .iso/.cso     -> /dev_hdd0/PSPISO/     (`platform` starting with
                          "psp", case-insensitive, picks this over PS3ISO)
   - .rap              -> /dev_hdd0/exdata/     (game license)
   Returns 0 on success, -1 if the type is not installable, -2 if the path
   does not fit in `out`. */
int store_derive_path_ps3(const char *url, const char *title,
                          const char *platform, char *out, size_t outlen);

/* May an entry with this file/URL name and platform label be browsed or
   fetched by `console` ("psp" or "ps3", case-insensitive; NULL/empty = no
   filter)? Console-exclusive extensions decide on their own (.pkg/.rap
   only ever help a PS3, .pbp/.cso only a PSP); otherwise a label starting
   with "psp"/"ps3" decides, and anything else — a plain .iso, a text
   file — is fine on either machine. */
int store_platform_ok(const char *name, const char *platform,
                      const char *console);

/* Companion URL for an attached license: the catalog's "rap" field is the
   file name of a .rap stored in the game's own folder (the web GUI records
   it there), so the download URL is the entry URL's parent folder plus that
   name. A folder-style entry URL (trailing slash) takes the folder itself,
   a file URL its parent. Returns 0 on success, -1 for an empty rap name or
   a result that does not fit in `out`. */
int store_license_url(const char *item_url, const char *rap,
                      char *out, size_t outlen);

/* Display name for a game-categories-lite (gclite) category folder.
   The convention on the PSP XMB: a "CAT_" prefix (any case), optionally
   followed by exactly two digits 00-99 that only order the categories.
   The category name is the rest, so CAT_05Shooters and CAT_Shooters both
   read "Shooters". Anything else is returned verbatim, including a bare
   "CAT_" that would strip to nothing — a raw name beats an empty one. */
void store_cat_name(const char *folder, char *out, size_t outlen);

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
