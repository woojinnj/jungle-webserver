#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_MAX_HEADER_BYTES 16384
#define HTTP_MAX_LINE 8192
#define HTTP_MAX_URI 2048
#define HTTP_MAX_HEADERS 64
#define HTTP_MAX_BODY ((size_t)8 * 1024 * 1024)

typedef enum { M_GET, M_POST, M_HEAD, M_OTHER } http_method;

/* All pointers point into the caller's buffer (zero-copy). */
typedef struct {
    const char *name;
    size_t nlen;
    const char *val;
    size_t vlen;
} http_header;

typedef struct {
    http_method method;
    const char *method_str;
    size_t method_len;
    const char *uri;
    size_t uri_len;
    int minor; /* HTTP/1.minor */
    http_header headers[HTTP_MAX_HEADERS];
    size_t nheaders;
    size_t content_length; /* saturates at 1e15; compare with HTTP_MAX_BODY */
    int keep_alive;
    int expect_continue;
    size_t head_len; /* bytes up to and including the blank line */
} http_request;

#define HTTP_PARSE_OK 0
#define HTTP_PARSE_INCOMPLETE (-1)

/* Parses a request head from buf[0..len).
 * 0 = ok, -1 = need more bytes, >0 = HTTP status to reply with
 * (400, 414, 431, 501, 505). */
int http_parse_request(const char *buf, size_t len, http_request *req);

/* Case-insensitive header lookup; NULL if absent. */
const char *http_header_get(const http_request *r, const char *name, size_t *vlen);

/* Pointer just past "\r\n\r\n", or NULL. */
const char *http_find_head_end(const char *buf, size_t len);

/* Digits only, saturating at 1e15. 0 ok, -1 invalid. */
int http_parse_uint(const char *s, size_t n, size_t *out);

int http_ieq(const char *a, size_t alen, const char *b);
int http_token_list_has(const char *v, size_t vlen, const char *tok);

const char *http_status_text(int code);

/* Formats a response head ("extra" = zero or more "Name: v\r\n" lines).
 * Returns length, or 0 if it does not fit. */
size_t http_format_head(char *out, size_t cap, int status, const char *ctype, size_t clen,
                        int keep_alive, const char *extra);

int http_send_response(int fd, int status, const char *ctype, const void *body, size_t blen,
                       int keep_alive, const char *extra, int head_only);

/* Small HTML error page. */
int http_send_error(int fd, int status, int keep_alive, int head_only, const char *extra);

#endif
