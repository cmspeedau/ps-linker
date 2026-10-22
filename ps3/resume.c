/*
 * PS Linker - resumable HTTP download (PS3 / PSL1GHT)
 *
 * Strategy: if dest_path exists and is non-empty, send
 * "Range: bytes=<size>-". A 206 response means we append ("ab"); a
 * 200 response means the server ignored the header, so we restart
 * from scratch ("wb"). Anything else is an error.
 */

#include "resume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <net/net.h>

#define DL_BUF_SIZE (64 * 1024)

/* Strip scheme+host; returns path portion of url ("/" at minimum). */
static const char *url_path(const char *url) {
    const char *p = strstr(url, "://");
    if (!p) return NULL;
    p += 3;
    p = strchr(p, '/');
    return p ? p : "/";
}

/* Extract host[:port] from "http://host[:port]/..." into hostbuf. */
static int url_host(const char *url, char *hostbuf, size_t len) {
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n == 0 || n >= len) return -1;
    memcpy(hostbuf, p, n);
    hostbuf[n] = '\0';
    return 0;
}

/* Resolve host (with optional :port), connect, return socket or <0. */
static int http_connect(const char *url) {
    char host[128];
    if (url_host(url, host, sizeof(host)) != 0) return -1;

    int port = 80;
    char *colon = strchr(host, ':');
    if (colon) { port = atoi(colon + 1); *colon = '\0'; }

    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list[0]) return -2;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -3;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(s);
        return -4;
    }
    return s;
}

/*
 * Send a GET (with optional Range header), then read and parse the
 * response headers. Fills *status_code and *content_len (-1 if absent).
 * Returns the socket positioned at the body, or <0 on error.
 */
static int http_get_with_range(const char *url, long range_from,
                               int *status_code, long *content_len) {
    const char *path = url_path(url);
    if (!path) return -1;

    char host[128];
    if (url_host(url, host, sizeof(host)) != 0) return -1;

    int s = http_connect(url);
    if (s < 0) return s;

    char req[512];
    int rl;
    if (range_from > 0)
        rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s\r\n"
                      "Range: bytes=%ld-\r\nConnection: close\r\n\r\n",
                      path, host, range_from);
    else
        rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s\r\n"
                      "Connection: close\r\n\r\n",
                      path, host);
    if (rl <= 0 || send(s, req, (size_t)rl, 0) != rl) {
        close(s);
        return -5;
    }

    /* Read the header block into a small buffer. */
    char hdr[4096];
    size_t used = 0;
    while (used < sizeof(hdr) - 1) {
        char c;
        ssize_t n = recv(s, &c, 1, 0);
        if (n <= 0) { close(s); return -8; }
        hdr[used++] = c;
        if (used >= 4 && memcmp(hdr + used - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    hdr[used] = '\0';

    if (sscanf(hdr, "HTTP/%*d.%*d %d", status_code) != 1) {
        close(s);
        return -8;
    }

    *content_len = -1;
    char *cl = strcasestr(hdr, "content-length:");
    if (cl) *content_len = atol(cl + strlen("content-length:"));

    return s;
}

int ps3_resume_download(const char *url, const char *dest_path,
                        resume_progress_cb progress)
{
    long resume_from = 0;

    /* How much of the file do we already have? */
    struct stat st;
    if (stat(dest_path, &st) == 0 && st.st_size > 0)
        resume_from = (long)st.st_size;

    int code = 0;
    long remaining = -1;
    int s = http_get_with_range(url, resume_from, &code, &remaining);
    if (s < 0) return s;

    int appending;
    if (code == 206 && resume_from > 0) {
        appending = 1;                       /* confirmed resume */
    } else if (code == 200) {
        resume_from = 0;                     /* server ignored Range */
        appending = 0;
    } else {
        close(s);
        return -7;
    }

    long full_size = resume_from + (remaining > 0 ? remaining : 0);

    FILE *f = fopen(dest_path, appending ? "ab" : "wb");
    if (!f) { close(s); return -6; }

    static char buf[DL_BUF_SIZE];
    long total = resume_from;
    int ok = 1;
    ssize_t n;

    while (ok && (n = recv(s, buf, sizeof(buf), 0)) > 0) {
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ok = 0; break; }
        total += n;
        if (progress) {
            int pct = full_size > 0 ? (int)((total * 100) / full_size) : -1;
            if (pct > 100) pct = 100;
            progress(pct, total);
        }
    }
    if (n < 0) ok = 0;                       /* recv error */

    fclose(f);
    close(s);
    return ok ? 0 : -9;
}
