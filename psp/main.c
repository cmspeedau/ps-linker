/*
 * PS Linker - NAS Downloader (PSP homebrew)
 *
 * Connects to Wi-Fi, then downloads files listed below from the
 * PS Linker server (HTTP share) to the Memory Stick.
 *
 * Build with the pspdev toolchain: make
 * Requires CFW (ARK-4 / PRO) or PPSSPP to run.
 */
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspwlan.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psphttp.h>
#include <pspiofilemgr.h>
#include <string.h>

#define printf pspDebugScreenPrintf

PSP_MODULE_INFO("NASDownloader", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

/* Server address - change to your NAS / Linux box */
#define SERVER "http://192.168.1.50:8000"

typedef struct {
    const char *remote;   /* path on the server, relative to SERVER */
    const char *out;      /* destination on the Memory Stick */
} Item;

static Item items[] = {
    { "/games/homebrew.zip", "ms0:/PSP/GAME/downloads/homebrew.zip" },
};
#define NITEMS (sizeof(items) / sizeof(items[0]))

/* ---------- Network ---------- */
int net_init(void) {
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) != 0) return -1;
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_INET) != 0) return -1;
    if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) != 0) return -1;
    if (sceNetInetInit() != 0) return -1;
    if (sceNetApctlInit(0x10000, 48) != 0) return -1;

    pspDebugScreenClear();
    printf("Connecting to Wi-Fi...\n");
    if (sceNetApctlConnect(1) != 0) return -1; /* connection profile 1 */

    SceNetApctlInfo info;
    for (int i = 0; i < 100; i++) {
        if (sceNetApctlGetInfo(1, &info) == 0) {
            printf("Connected: %s (%s)\n", info.ssid, info.ip);
            return 0;
        }
        sceKernelDelayThread(100 * 1000);
    }
    return -1;
}

/* ---------- HTTP download ---------- */
int download(const char *url, const char *outpath) {
    int tmpl = sceHttpCreateTemplate("PSLinker", 1, 1);
    if (tmpl < 0) return -1;

    int ret = -1;
    int conn = sceHttpCreateConnectionWithURL(tmpl, url, 0);
    if (conn < 0) goto del_template;

    int req = sceHttpCreateRequestWithURL(conn, PSP_HTTP_METHOD_GET, url, 0);
    if (req < 0) goto del_conn;

    if (sceHttpSendRequest(req, NULL, 0) < 0) goto del_req;

    sceIoMkdir(dirname_str(outpath), 0777);

    SceUID fd = sceIoOpen(outpath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) goto del_req;

    char buf[8192];
    int read_total = 0, len;
    while ((len = sceHttpReadData(req, buf, sizeof(buf))) > 0) {
        if (sceIoWrite(fd, buf, len) != len) { len = -1; break; }
        read_total += len;
    }
    sceIoClose(fd);
    if (len == 0) { printf("  saved %d bytes\n", read_total); ret = 0; }

del_req:
    sceHttpDeleteRequest(req);
del_conn:
    sceHttpDeleteConnection(conn);
del_template:
    sceHttpDeleteTemplate(tmpl);
    return ret;
}

/* naive dirname helper (pspsdk has none in libc) */
static char *dirname_str(const char *path) {
    static char buf[256];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *slash = strrchr(buf, '/');
    if (slash) *slash = 0;
    return buf;
}

int main(void) {
    pspDebugScreenInit();

    if (net_init() < 0) {
        printf("Network init failed.\n");
        sceKernelDelayThread(5 * 1000 * 1000);
        sceKernelExitGame();
    }

    for (int i = 0; i < (int)NITEMS; i++) {
        char url[512];
        snprintf(url, sizeof(url), "%s%s", SERVER, items[i].remote);
        printf("[%d/%d] %s\n", i + 1, (int)NITEMS, url);
        if (download(url, items[i].out) < 0)
            printf("  FAILED\n");
    }

    printf("\nDone. Press Home to exit.\n");
    sceKernelSleepThread();
    return 0;
}
