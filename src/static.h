#ifndef STATIC_H
#define STATIC_H

#include <stddef.h>

#include "http.h"

/* Maps a request URI to root + decoded path (query stripped, %XX decoded).
 * Returns 0, or an HTTP status: 400 bad encoding / not absolute,
 * 403 path traversal ("..") , 414 result too long. The result is NOT yet
 * checked against symlinks; static_serve does that with realpath(). */
int static_resolve(const char *root, const char *uri, size_t urilen, char *out, size_t cap);

/* Serves GET/HEAD from root (a realpath()'d directory). Returns the status
 * sent; *bytes = body bytes written. */
int static_serve(int fd, const char *root, const http_request *req, int keep_alive, size_t *bytes);

#endif
