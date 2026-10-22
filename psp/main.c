/*
 * PS Linker - PSP NAS Downloader with interactive browser
 *
 * Connects to Wi-Fi, browses the NAS HTTP share's directory listing,
 * lets you pick a file with the D-pad, and downloads it to the
 * Memory Stick. Files land in ms0:/PSP/GAME/NASDL/<filename>.
 *
 * Build with pspdev toolchain: make
 * Requires CFW (ARK-4 / PRO) to run.
 *
 * Controls in browser:  Up/Down = move, X = select, O = quit
 */

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspwlan.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psphttp.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdlib.h>

#define printf pspDebugScreenPrintf
#define SCREEN_ROWS 33

PSP_MODULE_INFO("PSLinker", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

/* Base URL of the NAS share, e.g. "http://192.168.1.50:8000/files" */
#define NAS_BASE_URL "http://192.168.1.50:8000"

/* Destination directory on the Memory Stick */
#define DEST_DIR "ms0:/PSP/GAME/NASDL/"

#define MAX_FILES     128
#define MAX_PATH_LEN  256
#define DL_BUF_SIZE   64 * 1024   /* 64 KB chunks */

typedef struct {
    char name[MAX_PATH_LEN];
    char url[MAX_PATH_LEN];   /* full URL to download */
} FileEntry;

static FileEntry g_files[MAX_FILES];
static int g_file_count = 0;

/* ------------------------------------------------------------------ */
/* Simple parser: extract href entries from an HTML directory listing  */
/* Works with python3 -m http.server, nginx autoindex, Synology shares */
/* ------------------------------------------------------------------ */

static void url_decode(char *dst, const char *src, size_t dstlen) {
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < dstlen; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/* Fetch a URL fully into a heap buffer. Returns NULL on failure. */
static char *fetch_to_memory(const char *url, int *out_len) {
    int tpl_id = sceHttpCreateTemplate("PSLinker", 1, 1);
    if (tpl_id < 0) return NULL;

    char *result = NULL;
    int conn = sceHttpCreateConnectionWithURL(tpl_id, url, 0);
    if (conn >= 0) {
        int req = sceHttpCreateRequestWithURL(conn, PSP_HTTP_METHOD_GET, url, 0);
        if (req >= 0 && sceHttpSendRequest(req, NULL, 0) >= 0) {
            int clen = 0;
            sceHttpGetContentLength(req, &clen);
            if (clen <= 0) clen = 512 * 1024;   /* assume up to 512 KB */

            char *buf = (char *)malloc(clen + 1);
            if (buf) {
                int total = 0, n;
                while ((n = sceHttpReadData(req, buf + total, clen - total)) > 0)
                    total += n;
                buf[total] = '\0';
                result = buf;
                if (out_len) *out_len = total;
            }
        }
        if (req >= 0) sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
    }
    sceHttpDeleteTemplate(tpl_id);
    return result;
}

static int add_file(const char *href) {
    /* Skip parent links and anything that isn't a file in this dir */
    if (href[0] == '\0') return 0;
    if (strstr(href, "../") == href) return 0;
    if (strchr(href, '/') != NULL)    return 0;   /* nested dir or absolute */
    size_t hl = strlen(href);
    if (hl > 0 && href[hl - 1] == '/') return 0;

    size_t used = strlen(NAS_BASE_URL) + 1 + strlen(href);
    if (used >= MAX_PATH_LEN || g_file_count >= MAX_FILES) return 0;

    FileEntry *e = &g_files[g_file_count];
    url_decode(e->name, href, MAX_PATH_LEN);
    if (NAS_BASE_URL[strlen(NAS_BASE_URL) - 1] == '/')
        snprintf(e->url, MAX_PATH_LEN, "%s%s", NAS_BASE_URL, href);
    else
        snprintf(e->url, MAX_PATH_LEN, "%s/%s", NAS_BASE_URL, href);
    g_file_count++;
    return 1;
}

static int parse_listing(const char *html) {
    g_file_count = 0;
    const char *p = html;
    while (g_file_count < MAX_FILES && (p = strstr(p, "href=\"")) != NULL) {
        p += 6;                              /* skip href=" */
        const char *end = strchr(p, '"');
        if (!end) break;
        int len = end - p;
        if (len > 0 && len < MAX_PATH_LEN) {
            char href[MAX_PATH_LEN];
            memcpy(href, p, len);
            href[len] = '\0';
            add_file(href);
        }
        p = end + 1;
    }
    return g_file_count;
}

/* ------------------------------------------------------------------ */
/* Download with progress bar                                          */
/* ------------------------------------------------------------------ */

static int download_file(const char *url, const char *dest_path) {
    int tpl = sceHttpCreateTemplate("PSLinker", 1, 1);
    if (tpl < 0) return -1;

    int status = -1;
    int conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    if (conn >= 0) {
        int req = sceHttpCreateRequestWithURL(conn, PSP_HTTP_METHOD_GET, url, 0);
        if (req >= 0 && sceHttpSendRequest(req, NULL, 0) >= 0) {
            int clen = 0;
            sceHttpGetContentLength(req, &clen);

            SceUID fd = sceIoOpen(dest_path,
                                  PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (fd >= 0) {
                static char buf[DL_BUF_SIZE];
                int n, total = 0, ok = 1;
                while ((n = sceHttpReadData(req, buf, sizeof(buf))) > 0) {
                    if (sceIoWrite(fd, buf, n) != n) { ok = 0; break; }
                    total += n;
                    if (clen > 0) {
                        int bar = (total * 30) / clen;
                        printf("[");
                        for (int i = 0; i < 30; i++) printf(i < bar ? "=" : " ");
                        printf("] %d%% (%d KB)\r", (total * 100) / clen, total / 1024);
                    } else {
                        printf("Downloaded %d KB...\r", total / 1024);
                    }
                }
                printf("\n");
                sceIoClose(fd);
                status = ok ? 0 : -2;
            } else {
                status = -3;   /* could not open destination file */
            }
        }
        if (req >= 0) sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
    }
    sceHttpDeleteTemplate(tpl);
    return status;
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void wait_button(void) {
    SceCtrlData pad;
    do { sceCtrlReadBufferPositive(&pad, 1); sceKernelDelayThread(16000); }
    while (pad.Buttons == 0);
}

static int browser(void) {
    pspDebugScreenClear();
    printf("Fetching listing from NAS...\n");

    char *html = fetch_to_memory(NAS_BASE_URL "/", NULL);
    if (!html) {
        printf("Could not reach %s\n", NAS_BASE_URL);
        printf("Press any button to retry.\n", NAS_BASE_URL);
        wait_button();
        return -1;
    }
    parse_listing(html);
    free(html);

    if (g_file_count == 0) {
        printf("No files found at %s\nPress any button.\n", NAS_BASE_URL);
        wait_button();
        return -2;
    }

    int sel = 0, top = 0;
    unsigned prev = 0;
    while (1) {
        pspDebugScreenClear();
        printf("== PS Linker: %d files on NAS ==\n\n", g_file_count);

        const int visible = SCREEN_ROWS - 6;
        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;

        for (int i = top; i < g_file_count && i < top + visible; i++)
            printf("%s %s\n", i == sel ? ">" : " ", g_files[i].name);

        printf("\nX=download  O=quit\n");

        /* Simple edge-detect: poll until a new button press appears */
        SceCtrlData pad;
        unsigned pressed = 0;
        do {
            sceCtrlReadBufferPositive(&pad, 1);
            pressed = pad.Buttons & ~prev;
            prev = pad.Buttons;
            if (!pressed) sceKernelDelayThread(16000);
        } while (!pressed);

        if (pressed & PSP_CTRL_DOWN) {
            sel = (sel + 1) % g_file_count;
        } else if (pressed & PSP_CTRL_UP) {
            sel = (sel - 1 + g_file_count) % g_file_count;
        } else if (pressed & PSP_CTRL_CROSS) {
            FileEntry *e = &g_files[sel];
            char dest[MAX_PATH_LEN + 32];
            snprintf(dest, sizeof(dest), DEST_DIR "%s", e->name);
            pspDebugScreenClear();
            printf("Downloading %s\n-> %s\n\n", e->name, dest);
            int rc = download_file(e->url, dest);
            if (rc == 0) printf("Done!\n");
            else         printf("Failed (error %d).\n", rc);
            printf("\nPress any button.\n");
            wait_button();
        } else if (pressed & PSP_CTRL_CIRCLE) {
            return 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Network init                                                        */
/* ------------------------------------------------------------------ */

static int wait_for_ip(unsigned int timeout_us) {
    unsigned int start = sceKernelGetSystemTimeWide();
    SceNetApctlInfo info;
    do {
        if (sceNetApctlGetInfo(1, &info) == 0) {
            printf("Connected: %s (%s)\n", info.ssid, info.ip);
            return 0;
        }
        sceKernelDelayThread(200 * 1000);
    } while (sceKernelGetSystemTimeWide() - start < timeout_us);
    return -2;
}

static int net_init(void) {
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) != 0) return -1;
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_INET)   != 0) return -2;
    if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) != 0) return -3;
    if (sceNetInetInit()    != 0) return -4;
    if (sceNetApctlInit(0x10000, 48) != 0) return -5;

    printf("Connecting to Wi-Fi (profile 1)...\n");
    if (sceNetApctlConnect(1) != 0) return -6;   /* Wi-Fi profile 1 */
    if (wait_for_ip(20 * 1000 * 1000) != 0) return -7;
    return 0;
}

int main(void) {
    pspDebugScreenInit();
    printf("PS Linker v1.1 - PSP NAS Downloader\n\n");

    int rc = net_init();
    if (rc != 0) {
        printf("Network init failed (%d). Check Wi-Fi settings.\n", rc);
        sceKernelDelayThread(5 * 1000 * 1000);
        sceKernelExitGame();
        return 1;
    }

    sceIoMkdir(DEST_DIR, 0777);

    while (browser() != 0) {
        /* browser() returned an error: retry after any button press */
        wait_button();
    }

    printf("Goodbye!\n");
    sceKernelDelayThread(2 * 1000 * 1000);
    sceKernelExitGame();
    return 0;
}
