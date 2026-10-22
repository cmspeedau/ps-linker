/*
 * PS Linker - resumable HTTP download (PSP)
 *
 * Strategy: if dest_path exists and is non-empty, send
 * "Range: bytes=<size>-". A 206 response means we append; a 200
 * response means the server ignored the header, so we restart from
 * scratch. Anything else is an error.
 */

#include "resume.h"

#include <pspsdk.h>
#include <pspkernel.h>
#include <psphttp.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>

#define DL_BUF_SIZE (64 * 1024)

int psp_resume_download(const char *url, const char *dest_path,
                       resume_progress_cb progress)
{
    SceOff resume_from = 0;

    /* How much of the file do we already have? */
    SceIoStat st;
    if (sceIoGetstat(dest_path, &st) == 0 && st.st_size > 0)
        resume_from = st.st_size;

    int tpl = sceHttpCreateTemplate("PSLinker", 1, 1);
    if (tpl < 0) return -1;

    int status = -2;
    int conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
    if (conn >= 0) {
        int req = sceHttpCreateRequestWithURL(conn, PSP_HTTP_METHOD_GET,
                                              (char *)url, 0);
        if (req >= 0) {
            char range[48];
            if (resume_from > 0) {
                snprintf(range, sizeof(range), "bytes=%lld-",
                         (long long)resume_from);
                sceHttpAddExtraHeader(req, "Range", range, 0);
            }

            if (sceHttpSendRequest(req, NULL, 0) >= 0) {
                int code = 0;
                sceHttpGetStatusCode(req, &code);

                int appending = 0;
                if (code == 206 && resume_from > 0) {
                    appending = 1;          /* confirmed resume */
                } else if (code == 200) {
                    resume_from = 0;        /* server ignored Range */
                } else {
                    sceHttpDeleteRequest(req);
                    sceHttpDeleteConnection(conn);
                    sceHttpDeleteTemplate(tpl);
                    return -4;
                }

                /* Content-Length from the server is the REMAINING
                   size on a 206, full size on a 200. */
                int clen = 0;
                SceULong64 clen64 = 0;
                sceHttpGetContentLength(req, &clen64);
                if (clen64 > 0) clen = (int)clen64;
                long full_size = resume_from + (clen > 0 ? clen : 0);

                int open_flags = appending
                    ? (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND)
                    : (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC);
                SceUID fd = sceIoOpen(dest_path, open_flags, 0777);
                if (fd < 0) {
                    sceHttpDeleteRequest(req);
                    sceHttpDeleteConnection(conn);
                    sceHttpDeleteTemplate(tpl);
                    return -3;
                }

                static char buf[DL_BUF_SIZE];
                long total = (long)resume_from;
                int ok = 1, n;
                while ((n = sceHttpReadData(req, buf, sizeof(buf))) > 0) {
                    if (sceIoWrite(fd, buf, n) != n) { ok = 0; break; }
                    total += n;
                    if (progress) {
                        int pct = full_size > 0
                            ? (int)((total * 100) / full_size) : -1;
                        if (pct > 100) pct = 100;
                        progress(pct, total);
                    }
                }
                if (n < 0) ok = 0;          /* read error */

                sceIoClose(fd);
                status = ok ? 0 : -5;
            }
            sceHttpDeleteRequest(req);
        }
        sceHttpDeleteConnection(conn);
    }
    sceHttpDeleteTemplate(tpl);
    return status;
}
