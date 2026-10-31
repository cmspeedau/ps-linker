/*
 * PS Linker - PS3 socket HTTP (shared by the browser and the Store)
 *
 * Plain BSD-socket HTTP/1.0 GETs over the PSL1GHT network stack. The
 * share only ever needs GET, so there is deliberately no client beyond
 * "connect, send GET, read status + headers, stream the body".
 */

#ifndef PS3_HTTP_H
#define PS3_HTTP_H

#include <stddef.h>

/* Strip scheme+host; returns the path portion of url ("/" at minimum)
   or NULL if url is not absolute. */
const char *url_path(const char *url);

/* Extract host[:port] from "http://host[:port]/..." into hostbuf.
   Returns 0 on success, <0 if the url has no usable host. */
int url_host(const char *url, char *hostbuf, size_t len);

/* Resolve host[:port] and connect. Returns a socket or <0
   (-1 bad URL, -2 DNS, -3 socket, -4 connect). */
int http_connect(const char *url);

/* Connect and send a GET for url. Returns the socket positioned at the
   START of the response (status line included) or <0. */
int http_send_get(const char *url);

/* Read and discard the response headers. Fills *status (0 if
   unparseable) and returns the Content-Length (-1 if absent), or <0 on
   error. The socket is closed on error. */
int http_skip_headers(int s, int *status);

/* GET url and hand the NUL-terminated body to cb. Returns cb's value
   on a 2xx response, <0 on transport failure and -7 on a non-2xx
   status (the body is read but discarded). */
int http_get_body(const char *url,
                  int (*cb)(const char *body, void *user), void *user);

/* GET url into a malloc'd NUL-terminated buffer (freed by the caller).
   Returns the byte count or <0 as for http_get_body. */
int http_get_memory(const char *url, char **out);

#endif /* PS3_HTTP_H */
