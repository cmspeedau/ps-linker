/*
 * Host unit test for the URL / current-working-dir path logic shared by the
 * PSP and PS3 builds (build_url + cwd_enter). Run on the host with:
 *
 *     cc -Wall -Werror -O1 url_cwd_test.c -o /tmp/url_cwd_test && /tmp/url_cwd_test
 *
 * Guards the double-slash bug seen on the PS3 build ("/Homebrew//...") and the
 * leading-slash handling in cwd_enter. The build_url below must be kept in sync
 * with the device copies in psp/main.c and ps3/main.c.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH_LEN 256
static char g_nas_base[MAX_PATH_LEN];
static char g_cwd[MAX_PATH_LEN];

static int build_url(const char *name, char *out, size_t outlen) {
    char joined[MAX_PATH_LEN * 2];
    int n = snprintf(joined, sizeof(joined), "%s/%s/%s", g_nas_base, g_cwd, name);
    size_t i, o = 0;
    if (n < 0 || (size_t)n >= sizeof(joined) || outlen == 0) return -1;
    for (i = 0; joined[i] != '\0' && o + 1 < outlen; i++) {
        if (joined[i] != '/' || o == 0 || out[o - 1] != '/' ||
            (o >= 2 && out[o - 2] == ':')) out[o++] = joined[i];
    }
    if (joined[i] != '\0') return -1;
    out[o] = '\0';
    return 0;
}

static int cwd_enter(const char *dir_name) {
    size_t cl = strlen(g_cwd);
    while (*dir_name == '/') dir_name++;
    size_t nl = strlen(dir_name);
    if (cl + nl + 2 > sizeof(g_cwd)) return -1;
    memcpy(g_cwd + cl, dir_name, nl);
    g_cwd[cl + nl] = '/';
    g_cwd[cl + nl + 1] = '\0';
    return 0;
}

static void check_url(const char *cwd, const char *name, const char *want) {
    char out[MAX_PATH_LEN];
    snprintf(g_cwd, sizeof(g_cwd), "%s", cwd);
    assert(build_url(name, out, sizeof(out)) == 0);
    assert(strcmp(out, want) == 0);
    assert(strstr(out + 7, "//") == NULL); /* ignore http:// */
}

int main(void) {
    snprintf(g_nas_base, sizeof(g_nas_base), "http://host");
    check_url("", "file", "http://host/file");
    check_url("Homebrew", "file", "http://host/Homebrew/file");
    check_url("/Homebrew", "/file", "http://host/Homebrew/file");
    check_url("Homebrew/", "file", "http://host/Homebrew/file");
    check_url("Homebrew//", "/file", "http://host/Homebrew/file");

    g_cwd[0] = '\0';
    assert(cwd_enter("/Homebrew") == 0);
    assert(strcmp(g_cwd, "Homebrew/") == 0);
    assert(cwd_enter("//nested") == 0);
    assert(strcmp(g_cwd, "Homebrew/nested/") == 0);
    puts("all URL/cwd assertions passed");
    return 0;
}
