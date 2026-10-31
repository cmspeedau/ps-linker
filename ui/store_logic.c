#include "store_logic.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int store_category_add(char categories[STORE_CATEGORY_LIMIT][STORE_CATEGORY_LEN],
                       int *count, const char *category)
{
    if (!categories || !count || !category || !category[0] ||
        *count < 0 || *count > STORE_CATEGORY_LIMIT) return 0;
    char normalized[STORE_CATEGORY_LEN];
    snprintf(normalized, sizeof normalized, "%s", category);
    for (int i = 0; i < *count; i++)
        if (strcmp(categories[i], normalized) == 0) return 0;
    if (*count == STORE_CATEGORY_LIMIT) return 0;
    snprintf(categories[*count], STORE_CATEGORY_LEN, "%s", normalized);
    (*count)++;
    return 1;
}

void store_format_size(uint64_t bytes, char *out, size_t outlen)
{
    if (!out || outlen == 0) return;
    if (bytes < 1024ULL * 1024ULL) {
        snprintf(out, outlen, "%llu KB", (unsigned long long)((bytes + 512ULL) / 1024ULL));
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, outlen, "%llu MB", (unsigned long long)((bytes + 524288ULL) / 1048576ULL));
    } else {
        snprintf(out, outlen, "%llu GB", (unsigned long long)((bytes + 536870912ULL) / 1073741824ULL));
    }
}

static const char *basename_of(const char *url)
{
    const char *end = strchr(url, '?');
    size_t len = end ? (size_t)(end - url) : strlen(url);
    const char *base = url;
    for (size_t i = 0; i < len; i++)
        if (url[i] == '/') base = url + i + 1;
    return base;
}

/* Case-insensitive suffix test over exactly `len` bytes; the basename may
   carry a query string, so it is not NUL-terminated at base_len. Both sides
   are folded, so callers may pass the suffix in any case. */
static int ends_with_ci(const char *s, size_t len, const char *ext)
{
    size_t el = strlen(ext);
    if (len < el) return 0;
    for (size_t i = 0; i < el; i++) {
        unsigned char a = (unsigned char)s[len - el + i];
        unsigned char b = (unsigned char)ext[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Sanitise a path segment: PSP folders are safest as [A-Za-z0-9_-], runs of
   anything else collapse to a single '_', leading/trailing separators drop.
   Returns the stored length (0 = nothing usable). */
static size_t fold_name(char *dst, size_t cap, const char *src, size_t srclen)
{
    size_t n = 0;
    for (size_t i = 0; i < srclen && n < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            dst[n++] = (char)c;
        else if (n && dst[n - 1] != '_')
            dst[n++] = '_';
    }
    while (n && (dst[n - 1] == '_' || dst[n - 1] == '-'))
        n--;
    dst[n] = '\0';
    return n;
}

/* The directory above the file: "/Games/Doom/EBOOT.PBP" -> "Doom".
   Returns the stored length, 0 when the URL has no usable folder above it
   (root file, or the host part of an absolute URL). */
static size_t parent_dir_of(const char *url, char *out, size_t outlen)
{
    const char *end = strchr(url, '?');
    size_t plen = end ? (size_t)(end - url) : strlen(url);
    const char *slash = NULL, *prev = NULL;
    for (const char *p = url; p < url + plen; p++)
        if (*p == '/') { prev = slash; slash = p; }
    if (!slash || !prev || slash == url) return 0;
    if (prev > url && *(prev - 1) == '/') return 0;   /* "//host" is not a folder */
    return fold_name(out, outlen, prev + 1, (size_t)(slash - prev - 1));
}

/* Sanitise a file name: [A-Za-z0-9._-] kept, anything else becomes '_'.
   Returns the stored length (0 = nothing usable). */
static size_t sanitize_file(char *dst, size_t cap, const char *src, size_t srclen)
{
    size_t n = 0;
    for (size_t i = 0; i < srclen && n < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            dst[n++] = (char)c;
        else dst[n++] = '_';
    }
    dst[n] = '\0';
    return n;
}

int store_derive_path(const char *url, const char *title,
                      char *out, size_t outlen)
{
    if (!url || !title || !out || outlen == 0) return -1;
    const char *base = basename_of(url);
    size_t base_len = strcspn(base, "?");
    int iso = ends_with_ci(base, base_len, ".iso");
    int cso = ends_with_ci(base, base_len, ".cso");
    if (iso || cso) {
        int n = snprintf(out, outlen, "ms0:/ISO/%.*s", (int)base_len, base);
        return n >= 0 && (size_t)n < outlen ? 0 : -2;
    }

    if (!ends_with_ci(base, base_len, ".pbp")) return -1;

    /* An EBOOT.PBP has to live in its own game folder, and a generated catalog
       titles every one of them "EBOOT" (the file stem), so the folder comes
       from the URL's parent directory. Taking it from the title put every
       homebrew in ms0:/PSP/GAME/EBOOT/, where the second install resumed
       against the first file and could never complete. */
    char folder[64];
    size_t nfolder = 0;
    if (base_len == 9 && ends_with_ci(base, base_len, "EBOOT.PBP"))
        nfolder = parent_dir_of(url, folder, sizeof(folder));
    if (!nfolder)
        nfolder = fold_name(folder, sizeof(folder), title, strlen(title));
    if (nfolder == 0) { memcpy(folder, "Homebrew", 8); nfolder = 8; }
    folder[nfolder] = '\0';

    char file[64];
    if (base_len == 0) base = "EBOOT.PBP", base_len = strlen(base);
    size_t nfile = sanitize_file(file, sizeof(file), base, base_len);
    if (nfile == 0) return -1;
    if (strcmp(file, "EBOOT.PBP") == 0 || strcmp(file, "eboot.pbp") == 0) {
        int n = snprintf(out, outlen, "ms0:/PSP/GAME/%s/EBOOT.PBP", folder);
        return n >= 0 && (size_t)n < outlen ? 0 : -2;
    }
    int n = snprintf(out, outlen, "ms0:/PSP/GAME/%s/%s", folder, file);
    return n >= 0 && (size_t)n < outlen ? 0 : -2;
}

int store_derive_path_ps3(const char *url, const char *title,
                          const char *platform, char *out, size_t outlen)
{
    if (!url || !out || outlen == 0) return -1;
    const char *base = basename_of(url);
    size_t base_len = strcspn(base, "?");
    char file[96];
    if (base_len == 0) {                      /* catalogue with a bare name */
        if (!title || !title[0]) return -1;
        base_len = strlen(title);
        base = title;
    }
    if (sanitize_file(file, sizeof(file), base, base_len) == 0) return -1;

    const int pkg  = ends_with_ci(file, strlen(file), ".pkg");
    const int iso  = ends_with_ci(file, strlen(file), ".iso");
    const int cso  = ends_with_ci(file, strlen(file), ".cso");
    const int rap  = ends_with_ci(file, strlen(file), ".rap");
    if (!pkg && !iso && !cso && !rap) return -1;   /* .pbp and the rest: no */

    /* A PSP disc image mounted by a PS3 game manager lives in PSPISO;
       the sidecar's platform field is the only signal the catalog has. */
    int psp_disc = 0;
    if (iso || cso) {
        const char *p = platform;
        psp_disc = p && ((p[0] == 'p' || p[0] == 'P') &&
                         (p[1] == 's' || p[1] == 'S') &&
                         (p[2] == 'p' || p[2] == 'P'));
    }

    const char *dir = rap ? "/dev_hdd0/exdata/"
                    : pkg ? "/dev_hdd0/packages/"
                    : psp_disc ? "/dev_hdd0/PSPISO/"
                    : "/dev_hdd0/PS3ISO/";
    int n = snprintf(out, outlen, "%s%s", dir, file);
    return n >= 0 && (size_t)n < outlen ? 0 : -2;
}

int store_license_url(const char *item_url, const char *rap,
                      char *out, size_t outlen)
{
    if (!item_url || !item_url[0] || !rap || !rap[0] ||
        !out || outlen == 0) return -1;

    char dir[512];
    int n = snprintf(dir, sizeof dir, "%s", item_url);
    if (n < 0 || (size_t)n >= sizeof dir) return -1;

    size_t l = strlen(dir);
    if (l && dir[l - 1] == '/') {
        /* folder-style URL: the license rides inside the folder */
    } else {
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = '\0';         /* parent folder, slash kept   */
        else dir[0] = '\0';                 /* bare name: share root       */
    }
    int m = snprintf(out, outlen, "%s%s", dir, rap);
    return (m >= 0 && (size_t)m < outlen) ? 0 : -1;
}

/* ---- id-hashed placeholder cover tiles ---------------------------------
 * Port of the web GUI's placeholderBG(): one item, one hue, on the web
 * page and on the PSP alike. Pure CPU so psp/tests can pin the parity. */

int store_cover_hue(const char *id)
{
    uint32_t h = 0;
    if (id)
        for (; *id; id++)
            h = (h * 31u + (unsigned char)*id) & 0x3fffffffu;
    return (int)(h % 360u);
}

void store_hsl_to_rgb(int h, int s, int l, unsigned char out[3])
{
    h %= 360;
    if (h < 0) h += 360;
    if (s < 0)
        s = 0;
    if (s > 100)
        s = 100;
    if (l < 0)
        l = 0;
    if (l > 100)
        l = 100;

    const float sf = s / 100.f, lf = l / 100.f;
    const float c = (1.f - fabsf(2.f * lf - 1.f)) * sf;
    const float x = c * (1.f - fabsf(fmodf((float)h / 60.f, 2.f) - 1.f));
    const float m = lf - c / 2.f;
    float r, g, b;
    switch (h / 60) {
        case 0:  r = c; g = x; b = 0; break;
        case 1:  r = x; g = c; b = 0; break;
        case 2:  r = 0; g = c; b = x; break;
        case 3:  r = 0; g = x; b = c; break;
        case 4:  r = x; g = 0; b = c; break;
        default: r = c; g = 0; b = x; break;
    }
    out[0] = (unsigned char)((r + m) * 255.f + 0.5f);
    out[1] = (unsigned char)((g + m) * 255.f + 0.5f);
    out[2] = (unsigned char)((b + m) * 255.f + 0.5f);
}

void store_cover_pixels(uint32_t *buf, int w, int h, const char *id)
{
    if (!buf || w <= 0 || h <= 0) return;
    const int hue = store_cover_hue(id);
    unsigned char top[3], bot[3];
    store_hsl_to_rgb(hue, 42, 26, top);
    store_hsl_to_rgb((hue + 40) % 360, 52, 12, bot);

    for (int y = 0; y < h; y++) {
        const int t = h > 1 ? y * 255 / (h - 1) : 0;   /* 0 = top, 255 = bottom */
        int r = top[0] + ((int)bot[0] - (int)top[0]) * t / 255;
        int g = top[1] + ((int)bot[1] - (int)top[1]) * t / 255;
        int b = top[2] + ((int)bot[2] - (int)top[2]) * t / 255;
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        const uint32_t px = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        for (int i = 0; i < w; i++) buf[(size_t)y * (size_t)w + (size_t)i] = px;
    }
}
