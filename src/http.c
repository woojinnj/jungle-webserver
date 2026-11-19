#include "http.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "net.h"

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int http_ieq(const char *a, size_t alen, const char *b) {
    if (alen != strlen(b)) return 0;
    for (size_t i = 0; i < alen; i++)
        if (lc((unsigned char)a[i]) != lc((unsigned char)b[i])) return 0;
    return 1;
}

int http_token_list_has(const char *v, size_t n, const char *tok) {
    size_t i = 0;
    while (i < n) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < n && v[i] != ',') i++;
        size_t e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        if (e > s && http_ieq(v + s, e - s, tok)) return 1;
    }
    return 0;
}

int http_parse_uint(const char *s, size_t n, size_t *out) {
    if (n == 0) return -1;
    unsigned long long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        if (v < 1000000000000000ULL) v = v * 10 + (unsigned)(s[i] - '0');
    }
    if (v > 1000000000000000ULL) v = 1000000000000000ULL;
    *out = v > (unsigned long long)SIZE_MAX ? SIZE_MAX : (size_t)v;
    return 0;
}

static int is_tchar(int c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static const char *find_crlf(const char *b, size_t n) {
    for (size_t i = 0; i + 1 < n; i++)
        if (b[i] == '\r' && b[i + 1] == '\n') return b + i;
    return NULL;
}

const char *http_find_head_end(const char *b, size_t n) {
    for (size_t i = 0; i + 3 < n; i++)
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n')
            return b + i + 4;
    return NULL;
}

const char *http_header_get(const http_request *r, const char *name, size_t *vlen) {
    for (size_t i = 0; i < r->nheaders; i++) {
        if (http_ieq(r->headers[i].name, r->headers[i].nlen, name)) {
            if (vlen) *vlen = r->headers[i].vlen;
            return r->headers[i].val;
        }
    }
    return NULL;
}

static int parse_request_line(const char *buf, const char *eol, http_request *r) {
    const char *sp1 = memchr(buf, ' ', (size_t)(eol - buf));
    if (!sp1 || sp1 == buf) return 400;
    const char *uri = sp1 + 1;
    const char *sp2 = memchr(uri, ' ', (size_t)(eol - uri));
    if (!sp2 || sp2 == uri) return 400;
    const char *ver = sp2 + 1;
    size_t mlen = (size_t)(sp1 - buf), ulen = (size_t)(sp2 - uri), vlen = (size_t)(eol - ver);

    if (mlen > 16) return 400;
    for (size_t i = 0; i < mlen; i++)
        if (buf[i] < 'A' || buf[i] > 'Z') return 400;
    if (ulen > HTTP_MAX_URI) return 414;
    if (uri[0] != '/') return 400;
    for (size_t i = 0; i < ulen; i++)
        if ((unsigned char)uri[i] <= 0x20 || uri[i] == 0x7f) return 400;

    if (vlen != 8 || memcmp(ver, "HTTP/", 5) != 0 || ver[5] < '0' || ver[5] > '9' ||
        ver[6] != '.' || ver[7] < '0' || ver[7] > '9')
        return 400;
    if (ver[5] != '1' || (ver[7] != '0' && ver[7] != '1')) return 505;

    r->minor = ver[7] - '0';
    r->method_str = buf;
    r->method_len = mlen;
    r->uri = uri;
    r->uri_len = ulen;
    if (mlen == 3 && memcmp(buf, "GET", 3) == 0) r->method = M_GET;
    else if (mlen == 4 && memcmp(buf, "POST", 4) == 0) r->method = M_POST;
    else if (mlen == 4 && memcmp(buf, "HEAD", 4) == 0) r->method = M_HEAD;
    else r->method = M_OTHER;
    return 0;
}

static int parse_header_line(const char *s, const char *e, http_request *r) {
    if (s[0] == ' ' || s[0] == '\t') return 400; /* obs-fold / leading whitespace */
    const char *colon = memchr(s, ':', (size_t)(e - s));
    if (!colon || colon == s) return 400;
    for (const char *p = s; p < colon; p++)
        if (!is_tchar((unsigned char)*p)) return 400; /* also rejects space before ':' */
    const char *v = colon + 1;
    while (v < e && (*v == ' ' || *v == '\t')) v++;
    const char *ve = e;
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
    for (const char *p = v; p < ve; p++)
        if (((unsigned char)*p < 0x20 && *p != '\t') || *p == 0x7f) return 400;
    if (r->nheaders >= HTTP_MAX_HEADERS) return 431;
    http_header *h = &r->headers[r->nheaders++];
    h->name = s;
    h->nlen = (size_t)(colon - s);
    h->val = v;
    h->vlen = (size_t)(ve - v);
    return 0;
}

int http_parse_request(const char *buf, size_t len, http_request *r) {
    memset(r, 0, sizeof *r);
    const char *eol = find_crlf(buf, len);
    if (!eol) return len > HTTP_MAX_URI + 32 ? 414 : HTTP_PARSE_INCOMPLETE;
    int rc = parse_request_line(buf, eol, r);
    if (rc) return rc;

    const char *cur = eol + 2;
    const char *end = buf + len;
    for (;;) {
        const char *le = find_crlf(cur, (size_t)(end - cur));
        if (!le) {
            if ((size_t)(end - cur) > HTTP_MAX_LINE) return 431;
            return len >= HTTP_MAX_HEADER_BYTES ? 431 : HTTP_PARSE_INCOMPLETE;
        }
        if (le == cur) { /* blank line: end of head */
            r->head_len = (size_t)(le + 2 - buf);
            break;
        }
        if ((size_t)(le - cur) > HTTP_MAX_LINE) return 431;
        rc = parse_header_line(cur, le, r);
        if (rc) return rc;
        cur = le + 2;
    }
    if (r->head_len > HTTP_MAX_HEADER_BYTES) return 431;

    /* Framing headers: Content-Length must be unambiguous, chunked unsupported. */
    int have_cl = 0, have_te = 0;
    for (size_t i = 0; i < r->nheaders; i++) {
        const http_header *h = &r->headers[i];
        if (http_ieq(h->name, h->nlen, "content-length")) {
            size_t v;
            if (http_parse_uint(h->val, h->vlen, &v) != 0) return 400;
            if (have_cl && v != r->content_length) return 400;
            r->content_length = v;
            have_cl = 1;
        } else if (http_ieq(h->name, h->nlen, "transfer-encoding")) {
            have_te = 1;
        }
    }
    if (have_te) return have_cl ? 400 : 501;

    size_t vl = 0;
    const char *host = http_header_get(r, "host", &vl);
    if (r->minor == 1 && (!host || vl == 0)) return 400;

    const char *conn = http_header_get(r, "connection", &vl);
    if (r->minor == 1) r->keep_alive = !(conn && http_token_list_has(conn, vl, "close"));
    else r->keep_alive = conn && http_token_list_has(conn, vl, "keep-alive");

    const char *ex = http_header_get(r, "expect", &vl);
    r->expect_continue = ex && http_ieq(ex, vl, "100-continue");
    return HTTP_PARSE_OK;
}

const char *http_status_text(int c) {
    switch (c) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}

size_t http_format_head(char *out, size_t cap, int status, const char *ctype, size_t clen,
                        int keep_alive, const char *extra) {
    char date[40];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\nServer: cwebd\r\nDate: %s\r\n%s%s%sContent-Length: %zu\r\n"
                     "Connection: %s\r\n%s\r\n",
                     status, http_status_text(status), date, ctype ? "Content-Type: " : "",
                     ctype ? ctype : "", ctype ? "\r\n" : "", clen,
                     keep_alive ? "keep-alive" : "close", extra ? extra : "");
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

int http_send_response(int fd, int status, const char *ctype, const void *body, size_t blen,
                       int keep_alive, const char *extra, int head_only) {
    char buf[2048 + 4096];
    size_t hl = http_format_head(buf, 2048, status, ctype, blen, keep_alive, extra);
    if (hl == 0) return -1;
    if (head_only || blen == 0) return net_send_all(fd, buf, hl);
    if (blen <= sizeof buf - hl) { /* one packet for small bodies */
        memcpy(buf + hl, body, blen);
        return net_send_all(fd, buf, hl + blen);
    }
    if (net_send_all(fd, buf, hl) != 0) return -1;
    return net_send_all(fd, body, blen);
}

int http_send_error(int fd, int status, int keep_alive, int head_only, const char *extra) {
    char body[256];
    const char *t = http_status_text(status);
    int n = snprintf(body, sizeof body,
                     "<html><head><title>%d %s</title></head><body><h1>%d %s</h1></body></html>\n",
                     status, t, status, t);
    return http_send_response(fd, status, "text/html; charset=utf-8", body, (size_t)n, keep_alive,
                              extra, head_only);
}
