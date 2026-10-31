/*
 * PS Linker - PS3 socket HTTP. See http.h.
 *
 * Lifted out of main.c unchanged (same requests, same tolerance for
 * hand-rolled share servers); the status code and http_get_memory are
 * what the Store catalog fetch needs on top.
 */

#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"

#ifdef NASDL_NETDEBUG
#define NETDEBUG(...) do { fprintf(stderr, "[NETDEBUG] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#else
#define NETDEBUG(...) do { } while (0)
#endif

/* Strip scheme+host; returns path portion of url ("/" at minimum). */
const char *url_path(const char *url)
{
    const char *p = strstr(url, "://");
    if (!p) return NULL;
    p += 3;
    p = strchr(p, '/');
    return p ? p : "/";
}

/* Extract host[:port] from "http://host[:port]/..." into hostbuf. */
int url_host(const char *url, char *hostbuf, size_t len)
{
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
int http_connect(const char *url)
{
    char host[128];
    NETDEBUG("connect URL: %s", url);
    if (url_host(url, host, sizeof(host)) != 0) { NETDEBUG("invalid URL host"); return -1; }

    int port = 80;
    char *colon = strchr(host, ':');
    if (colon) { port = atoi(colon + 1); *colon = '\0'; }

    NETDEBUG("DNS host=%s port=%d", host, port);
    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list[0]) { NETDEBUG("DNS failed for %s", host); return -2; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { NETDEBUG("socket failed"); return -3; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        NETDEBUG("connect failed host=%s port=%d", host, port);
        close(s);
        return -4;
    }
    NETDEBUG("connected socket=%d", s);
    return s;
}

/*
 * Send a GET request. On success the socket is left positioned at
 * the START of the response (status line included), so callers can
 * parse headers and read the body themselves.
 */
int http_send_get(const char *url)
{
    const char *path = url_path(url);
    NETDEBUG("GET URL: %s", url);
    if (!path) { NETDEBUG("URL has no path"); return -1; }

    char host[128];
    if (url_host(url, host, sizeof(host)) != 0) return -1;

    int s = http_connect(url);
    if (s < 0) return s;

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\n"
                      "Host: %s\r\n"
                      "User-Agent: PSLinker/1.0\r\n"
                      "Connection: close\r\n\r\n", path, host);
    int sent = send(s, req, rl, 0);
    NETDEBUG("send GET result=%d expected=%d", sent, rl);
    if (sent != rl) { close(s); return -5; }
    return s;
}

/* Skip the response headers on socket s. Fills *status and returns
   content length (-1 if unknown) or <0 on error (socket closed). */
int http_skip_headers(int s, int *status)
{
    char hdr[4096];
    int hdr_total = 0, n;
    if (status) *status = 0;
    while (hdr_total < (int)sizeof(hdr) - 1) {
        n = recv(s, hdr + hdr_total, 1, 0);
        if (n <= 0) { NETDEBUG("receive headers failed socket=%d result=%d", s, n); close(s); return -7; }
        hdr_total++;
        if (hdr_total > 4 &&
            memcmp(hdr + hdr_total - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    hdr[hdr_total] = '\0';
    NETDEBUG("HTTP status: %.*s", (int)(strchr(hdr, '\r') ? strchr(hdr, '\r') - hdr : 0), hdr);

    int code = 0;
    if (sscanf(hdr, "HTTP/%*d.%*d %d", &code) == 1 && status) *status = code;

    int clen = -1;
    char *cl = strcasestr(hdr, "Content-Length:");
    if (cl) clen = atoi(cl + 15);
    return clen;
}

/* GET a URL and pass the body to a callback. Returns 0 or <0. */
static int http_get_body_cap(const char *url,
                             int (*cb)(const char *body, void *user),
                             void *user, size_t cap_max)
{
    int s = http_send_get(url);
    if (s < 0) { NETDEBUG("listing request failed rc=%d", s); return s; }

    int status = 0;
    if (http_skip_headers(s, &status) < 0) { NETDEBUG("listing header read failed"); return -7; }
    if (status < 200 || status > 299) {
        NETDEBUG("HTTP status %d for %s", status, url);
        close(s);
        return -7;
    }

    char buf[8192];
    char *acc = NULL;
    size_t total = 0, cap = 0;
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        if (cap_max && total + n > cap_max) {   /* oversized body: abort */
            NETDEBUG("body over cap %zu for %s", cap_max, url);
            free(acc); close(s); return -6;
        }
        if (total + n + 1 > cap) {
            cap = (total + n) * 2 + 1024;
            char *tmp = (char *)realloc(acc, cap);
            if (!tmp) { free(acc); close(s); return -6; }
            acc = tmp;
        }
        memcpy(acc + total, buf, n);
        total += n;
    }
    close(s);
    if (n < 0) NETDEBUG("listing body receive failed result=%d", n);
    if (!acc) return -6;
    acc[total] = '\0';

    int rc = cb(acc, user);
    free(acc);
    return rc;
}

int http_get_body(const char *url,
                  int (*cb)(const char *body, void *user),
                  void *user)
{
    return http_get_body_cap(url, cb, user, 0);
}

/* ---- memory variant -------------------------------------------------- */

static int mem_cb(const char *body, void *user)
{
    char **out = (char **)user;
    *out = strdup(body);
    return *out ? 0 : -6;
}

int http_get_memory(const char *url, char **out)
{
    if (!out) return -1;
    *out = NULL;
    return http_get_body(url, mem_cb, out);
}
