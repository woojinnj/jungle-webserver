#include "static.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mime.h"
#include "net.h"

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int static_resolve(const char *root, const char *uri, size_t urilen, char *out, size_t cap) {
    char dec[HTTP_MAX_URI + 1];
    size_t n = 0;
    for (size_t i = 0; i < urilen && uri[i] != '?'; i++) {
        int c = (unsigned char)uri[i];
        if (c == '%') {
            int h, l;
            if (i + 2 >= urilen || (h = hexval(uri[i + 1])) < 0 || (l = hexval(uri[i + 2])) < 0)
                return 400;
            c = h * 16 + l;
            i += 2;
            if (c == 0) return 400; /* NUL would truncate the path */
        }
        if (n >= sizeof dec - 1) return 414;
        dec[n++] = (char)c;
    }
    dec[n] = '\0';
    if (dec[0] != '/') return 400;

    /* Reject any ".." segment (checked after decoding, so %2e%2e is caught). */
    for (size_t i = 0; i < n;) {
        size_t s = i;
        while (i < n && dec[i] != '/') i++;
        if (i - s == 2 && dec[s] == '.' && dec[s + 1] == '.') return 403;
        if (i < n) i++;
    }
    int w = snprintf(out, cap, "%s%s", root, dec);
    if (w < 0 || (size_t)w >= cap) return 414;
    return 0;
}

static int errno_status(int e) {
    if (e == ENOENT || e == ENOTDIR) return 404;
    if (e == EACCES) return 403;
    if (e == ENAMETOOLONG) return 414;
    return 500;
}

int static_serve(int fd, const char *root, const http_request *req, int ka, size_t *bytes) {
    *bytes = 0;
    int head_only = req->method == M_HEAD;
    char path[PATH_MAX], real[PATH_MAX];
    int st = static_resolve(root, req->uri, req->uri_len, path, sizeof path);
    if (st) goto error;

    const char *q = memchr(req->uri, '?', req->uri_len);
    size_t plen = q ? (size_t)(q - req->uri) : req->uri_len;
    size_t rtl = strlen(root);
    struct stat sb;

    for (int pass = 0;; pass++) {
        if (!realpath(path, real)) {
            /* directory without index.html: forbidden, not "missing" */
            st = (pass == 1 && errno == ENOENT) ? 403 : errno_status(errno);
            goto error;
        }
        /* symlinks must not lead out of the root */
        if (strncmp(real, root, rtl) != 0 || (rtl > 1 && real[rtl] != '/' && real[rtl] != '\0')) {
            st = 403;
            goto error;
        }
        if (stat(real, &sb) < 0) {
            st = errno_status(errno);
            goto error;
        }
        if (!S_ISDIR(sb.st_mode)) break;
        if (pass == 1) { /* index.html is itself a directory */
            st = 403;
            goto error;
        }
        if (req->uri[plen - 1] != '/') {
            char loc[HTTP_MAX_URI + 64];
            snprintf(loc, sizeof loc, "Location: %.*s/%.*s\r\n", (int)plen, req->uri,
                     (int)(req->uri_len - plen), req->uri + plen);
            http_send_error(fd, 301, ka, head_only, loc);
            return 301;
        }
        if (snprintf(path, sizeof path, "%s/index.html", real) >= (int)sizeof path) {
            st = 414;
            goto error;
        }
    }
    if (!S_ISREG(sb.st_mode)) { /* FIFO, device, socket: never open these */
        st = 403;
        goto error;
    }

    int ffd = open(real, O_RDONLY);
    if (ffd < 0) {
        st = errno_status(errno);
        goto error;
    }

    char buf[32768];
    size_t hl = http_format_head(buf, sizeof buf, 200, mime_type(real), (size_t)sb.st_size, ka, NULL);
    if (hl == 0 || net_send_all(fd, buf, hl) != 0) {
        close(ffd);
        return 200;
    }
    if (!head_only) {
        size_t left = (size_t)sb.st_size;
        while (left > 0) {
            ssize_t n = read(ffd, buf, left < sizeof buf ? left : sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break; /* file shrank or I/O error: client sees a short body */
            if (net_send_all(fd, buf, (size_t)n) != 0) break;
            *bytes += (size_t)n;
            left -= (size_t)n;
        }
    }
    close(ffd);
    return 200;

error:
    http_send_error(fd, st, ka, head_only, NULL);
    return st;
}
