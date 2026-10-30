#include "store_logic.h"
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
    size_t nfile = 0;
    if (base_len == 0) base = "EBOOT.PBP", base_len = strlen(base);
    for (size_t i = 0; i < base_len && nfile < sizeof(file) - 1; i++) {
        unsigned char c = (unsigned char)base[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            file[nfile++] = (char)c;
        else file[nfile++] = '_';
    }
    if (nfile == 0) return -1;
    file[nfile] = '\0';
    if (strcmp(file, "EBOOT.PBP") == 0 || strcmp(file, "eboot.pbp") == 0) {
        int n = snprintf(out, outlen, "ms0:/PSP/GAME/%s/EBOOT.PBP", folder);
        return n >= 0 && (size_t)n < outlen ? 0 : -2;
    }
    int n = snprintf(out, outlen, "ms0:/PSP/GAME/%s/%s", folder, file);
    return n >= 0 && (size_t)n < outlen ? 0 : -2;
}
