#include "proxy.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

#define CONNECT_TIMEOUT_MS 3000
#define BUF_CAP 24576 /* request head <= 16K plus the headers we add */

typedef struct {
    char *p;
    size_t len, cap;
    int bad;
} sbuf;

static void sb_add(sbuf *b, const char *s, size_t n) {
    if (b->len + n > b->cap) {
        b->bad = 1;
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void sb_fmt(sbuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) b->bad = 1;
    else b->len += (size_t)n;
}

/* Hop-by-hop headers (RFC 7230 6.1) are never forwarded. */
static int is_hop(const char *n, size_t l) {
    static const char *const h[] = {"connection", "keep-alive", "proxy-authenticate",
                                    "proxy-authorization", "proxy-connection", "te", "trailer",
                                    "transfer-encoding", "upgrade", NULL};
    for (int i = 0; h[i]; i++)
        if (http_ieq(n, l, h[i])) return 1;
    return 0;
}

static int build_request(sbuf *b, const http_request *req, const backend *be, size_t blen,
                         const char *ip) {
    const char *br = strchr(be->host, ':') ? "[" : "", *bl = strchr(be->host, ':') ? "]" : "";
    sb_fmt(b, "%.*s %.*s HTTP/1.1\r\nHost: %s%s%s:%s\r\n", (int)req->method_len, req->method_str,
           (int)req->uri_len, req->uri, br, be->host, bl, be->port);
    for (size_t i = 0; i < req->nheaders; i++) {
        const http_header *h = &req->headers[i];
        if (is_hop(h->name, h->nlen) || http_ieq(h->name, h->nlen, "host") ||
            http_ieq(h->name, h->nlen, "content-length") || http_ieq(h->name, h->nlen, "expect") ||
            http_ieq(h->name, h->nlen, "x-forwarded-for") ||
            http_ieq(h->name, h->nlen, "x-forwarded-host"))
            continue;
        sb_fmt(b, "%.*s: %.*s\r\n", (int)h->nlen, h->name, (int)h->vlen, h->val);
    }
    size_t xl = 0, hl = 0;
    const char *xff = http_header_get(req, "x-forwarded-for", &xl);
    if (xff) sb_fmt(b, "X-Forwarded-For: %.*s, %s\r\n", (int)xl, xff, ip);
    else sb_fmt(b, "X-Forwarded-For: %s\r\n", ip);
    const char *host = http_header_get(req, "host", &hl);
    if (host) sb_fmt(b, "X-Forwarded-Host: %.*s\r\n", (int)hl, host);
    if (blen > 0 || (req->method != M_GET && req->method != M_HEAD))
        sb_fmt(b, "Content-Length: %zu\r\n", blen);
    sb_fmt(b, "Connection: close\r\n\r\n");
    return b->bad ? -1 : 0;
}

static const char *crlf(const char *p, const char *end) {
    for (; p + 1 < end; p++)
        if (p[0] == '\r' && p[1] == '\n') return p;
    return NULL;
}

/* recv with a timeout. >0 bytes, 0 EOF, -1 error, -2 timeout. */
static ssize_t recv_to(int fd, void *buf, size_t n, int timeout_ms) {
    for (;;) {
        int w = net_wait_readable(fd, timeout_ms, NULL);
        if (w == 0) return -2;
        if (w < 0) return -1;
        ssize_t r = recv(fd, buf, n, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

int proxy_handle(int cfd, route *rt, const http_request *req, const char *body, size_t blen,
                 const char *ip, int read_timeout_ms, int *keep, size_t *bytes) {
    *bytes = 0;
    int head_only = req->method == M_HEAD;
    int status = 502, ufd = -1;

    /* 1. connect: round-robin start, fail over to the next backend */
    unsigned start = route_pick(rt);
    const backend *be = NULL;
    for (int i = 0; i < rt->nbe && ufd < 0; i++) {
        be = &rt->be[(start + (unsigned)i) % (unsigned)rt->nbe];
        int timed_out = 0;
        ufd = net_connect(be->host, be->port, CONNECT_TIMEOUT_MS, &timed_out);
        if (ufd < 0) status = timed_out ? 504 : 502;
    }
    if (ufd < 0) goto fail;

    /* 2. forward the request */
    char reqbuf[BUF_CAP];
    sbuf rb = {reqbuf, 0, sizeof reqbuf, 0};
    if (build_request(&rb, req, be, blen, ip) != 0) {
        status = 500;
        goto fail;
    }
    if (net_send_all(ufd, reqbuf, rb.len) != 0 || (blen > 0 && net_send_all(ufd, body, blen) != 0))
        goto fail;

    /* 3. read the reply head */
    char hb[HTTP_MAX_HEADER_BYTES];
    size_t hl = 0;
    const char *he;
    while (!(he = http_find_head_end(hb, hl))) {
        if (hl == sizeof hb) goto fail;
        ssize_t n = recv_to(ufd, hb + hl, sizeof hb - hl, read_timeout_ms);
        if (n == -2) {
            status = 504;
            goto fail;
        }
        if (n <= 0) goto fail;
        hl += (size_t)n;
    }

    /* 4. parse the reply head and rebuild it for the client */
    const char *end = hb + hl, *eol = crlf(hb, end);
    if (!eol || eol - hb < 12 || strncmp(hb, "HTTP/1.", 7) != 0 || hb[8] != ' ' ||
        hb[9] < '1' || hb[9] > '5' || hb[10] < '0' || hb[10] > '9' || hb[11] < '0' || hb[11] > '9')
        goto fail;
    int ustatus = (hb[9] - '0') * 100 + (hb[10] - '0') * 10 + (hb[11] - '0');
    const char *reason = hb + 12;
    if (*reason == ' ') reason++;

    char outbuf[BUF_CAP];
    sbuf ob = {outbuf, 0, sizeof outbuf, 0};
    sb_fmt(&ob, "HTTP/1.1 %d %.*s\r\n", ustatus, (int)(eol - reason), reason);

    int has_cl = 0, chunked = 0;
    size_t clen = 0;
    for (const char *cur = eol + 2; cur < he - 2;) {
        const char *le = crlf(cur, he);
        const char *colon = memchr(cur, ':', (size_t)(le - cur));
        if (!colon) goto fail;
        const char *v = colon + 1, *ve = le;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t nl = (size_t)(colon - cur), vl = (size_t)(ve - v);
        if (http_ieq(cur, nl, "content-length")) {
            size_t x;
            if (http_parse_uint(v, vl, &x) != 0 || (has_cl && x != clen)) goto fail;
            clen = x;
            has_cl = 1;
        } else if (http_ieq(cur, nl, "transfer-encoding")) {
            chunked = http_token_list_has(v, vl, "chunked");
        }
        /* We relay chunked bodies raw, so Transfer-Encoding must stay. */
        if (http_ieq(cur, nl, "transfer-encoding") || !is_hop(cur, nl))
            sb_add(&ob, cur, (size_t)(le - cur) + 2);
        cur = le + 2;
    }

    int no_body = head_only || ustatus < 200 || ustatus == 204 || ustatus == 304;
    enum { F_NONE, F_LEN, F_EOF } framing = no_body ? F_NONE : chunked ? F_EOF : has_cl ? F_LEN : F_EOF;
    /* close-delimited or chunked replies end the client connection too */
    if (framing == F_EOF) *keep = 0;
    sb_fmt(&ob, "Connection: %s\r\n\r\n", *keep ? "keep-alive" : "close");
    if (ob.bad) goto fail;

    /* 5. send head, relay body */
    if (net_send_all(cfd, outbuf, ob.len) != 0) {
        *keep = 0;
        close(ufd);
        return ustatus;
    }
    if (framing != F_NONE) {
        char buf[16384];
        const char *bp = he; /* body bytes that arrived together with the head */
        size_t have = hl - (size_t)(he - hb), sent = 0;
        for (;;) {
            if (have > 0) {
                size_t n = (framing == F_LEN && have > clen - sent) ? clen - sent : have;
                if (n > 0 && net_send_all(cfd, bp, n) != 0) {
                    *keep = 0;
                    break;
                }
                sent += n;
            }
            if (framing == F_LEN && sent >= clen) break;
            ssize_t r = recv_to(ufd, buf, sizeof buf, read_timeout_ms);
            if (r <= 0) { /* EOF ends a close-delimited body; else the body was cut short */
                if (framing == F_LEN) *keep = 0;
                break;
            }
            bp = buf;
            have = (size_t)r;
        }
        *bytes = sent;
    }
    close(ufd);
    return ustatus;

fail:
    if (ufd >= 0) close(ufd);
    http_send_error(cfd, status, *keep, head_only, NULL);
    return status;
}
