#include <stdlib.h>

#include "http.h"
#include "testutil.h"

static int parse(const char *s, http_request *r) { return http_parse_request(s, strlen(s), r); }

static void test_valid(void) {
    http_request r;
    const char *req = "GET /a/b.html?x=1 HTTP/1.1\r\nHost: example.com\r\nUser-Agent: t\r\n\r\n";
    CHECK(parse(req, &r) == 0);
    CHECK(r.method == M_GET);
    CHECK(r.uri_len == 13 && memcmp(r.uri, "/a/b.html?x=1", 13) == 0);
    CHECK(r.minor == 1 && r.keep_alive == 1);
    CHECK(r.nheaders == 2);
    CHECK(r.content_length == 0);
    CHECK(r.head_len == strlen(req));
    size_t vl;
    const char *h = http_header_get(&r, "HOST", &vl);
    CHECK(h && vl == 11 && memcmp(h, "example.com", 11) == 0);
    CHECK(http_header_get(&r, "x-none", &vl) == NULL);
}

static void test_post_and_pipelined(void) {
    http_request r;
    const char *req = "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhelloGET";
    CHECK(parse(req, &r) == 0);
    CHECK(r.method == M_POST && r.content_length == 5);
    CHECK(r.head_len == strlen(req) - 8); /* body and next request not consumed */
}

static void test_incomplete(void) {
    http_request r;
    CHECK(parse("", &r) == HTTP_PARSE_INCOMPLETE);
    CHECK(parse("GET / HT", &r) == HTTP_PARSE_INCOMPLETE);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\n", &r) == HTTP_PARSE_INCOMPLETE);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\n\r", &r) == HTTP_PARSE_INCOMPLETE);
}

static void test_bad_requests(void) {
    http_request r;
    CHECK(parse("GET\r\n\r\n", &r) == 400);
    CHECK(parse("GET /\r\n\r\n", &r) == 400);
    CHECK(parse("get / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 400);
    CHECK(parse("GET / HTTP/2.0\r\nHost: a\r\n\r\n", &r) == 505);
    CHECK(parse("GET / HTTP/1.9\r\nHost: a\r\n\r\n", &r) == 505);
    CHECK(parse("GET / FTP/1.1\r\nHost: a\r\n\r\n", &r) == 400);
    CHECK(parse("GET index.html HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a b HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 400);
    CHECK(parse("GET / HTTP/1.1\r\n\r\n", &r) == 400); /* no Host */
    CHECK(parse("GET / HTTP/1.1\r\nHost:\r\n\r\n", &r) == 400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nBadHeader\r\n\r\n", &r) == 400);
    CHECK(parse("GET / HTTP/1.1\r\nHost : a\r\n\r\n", &r) == 400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", &r) == 400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\nX: y\r\n\r\n", &r) == 400); /* bare LF */
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: abc\r\n\r\n", &r) == 400);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", &r) == 400);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", &r) == 400);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n", &r) == 400);
}

static void test_semantics(void) {
    http_request r;
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", &r) == 501);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\n", &r) == 0);
    CHECK(r.content_length == 3);
    /* huge Content-Length saturates instead of overflowing */
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999999\r\n\r\n", &r) == 0);
    CHECK(r.content_length > HTTP_MAX_BODY);
    CHECK(parse("BREW / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 0 && r.method == M_OTHER);
    CHECK(parse("HEAD / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == 0 && r.method == M_HEAD);
    /* keep-alive rules */
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", &r) == 0 && r.keep_alive == 0);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: Foo, CLOSE\r\n\r\n", &r) == 0 && r.keep_alive == 0);
    CHECK(parse("GET / HTTP/1.0\r\n\r\n", &r) == 0 && r.keep_alive == 0); /* 1.0: no Host needed */
    CHECK(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &r) == 0 && r.keep_alive == 1);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n", &r) == 0 && r.expect_continue);
}

static void test_limits(void) {
    http_request r;
    /* long URI -> 414, both while incomplete and once complete */
    size_t n = HTTP_MAX_URI + 100;
    char *s = malloc(n + 64);
    if (!s) abort();
    strcpy(s, "GET /");
    memset(s + 5, 'a', n);
    strcpy(s + 5 + n, " HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK(http_parse_request(s, strlen(s), &r) == 414);
    CHECK(http_parse_request(s, 5 + n, &r) == 414);
    free(s);

    /* too many headers -> 431 */
    char *h = malloc(HTTP_MAX_HEADER_BYTES * 2);
    if (!h) abort();
    strcpy(h, "GET / HTTP/1.1\r\nHost: a\r\n");
    for (int i = 0; i < HTTP_MAX_HEADERS + 5; i++) strcat(h, "X: y\r\n");
    strcat(h, "\r\n");
    CHECK(http_parse_request(h, strlen(h), &r) == 431);

    /* head larger than the buffer limit and still unterminated -> 431 */
    strcpy(h, "GET / HTTP/1.1\r\nHost: a\r\n");
    while (strlen(h) < HTTP_MAX_HEADER_BYTES + 10) strcat(h, "X-Pad: 0123456789012345678901234567890123456789\r\n");
    CHECK(http_parse_request(h, HTTP_MAX_HEADER_BYTES, &r) == 431);

    /* one overlong header line -> 431 */
    strcpy(h, "GET / HTTP/1.1\r\nHost: a\r\nX: ");
    size_t l = strlen(h);
    memset(h + l, 'v', HTTP_MAX_LINE + 10);
    strcpy(h + l + HTTP_MAX_LINE + 10, "\r\n\r\n");
    CHECK(http_parse_request(h, strlen(h), &r) == 431);
    free(h);
}

static void test_response(void) {
    char buf[512];
    size_t n = http_format_head(buf, sizeof buf, 404, "text/html", 12, 0, "Allow: GET\r\n");
    CHECK(n > 0 && n == strlen(buf));
    CHECK(strncmp(buf, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(strstr(buf, "Content-Type: text/html\r\n") != NULL);
    CHECK(strstr(buf, "Content-Length: 12\r\n") != NULL);
    CHECK(strstr(buf, "Connection: close\r\n") != NULL);
    CHECK(strstr(buf, "Allow: GET\r\n") != NULL);
    CHECK(n >= 4 && strcmp(buf + n - 4, "\r\n\r\n") == 0);
    CHECK(http_format_head(buf, 20, 200, NULL, 0, 1, NULL) == 0); /* does not fit */
    CHECK_STR(http_status_text(502), "Bad Gateway");
}

static void test_helpers(void) {
    CHECK(http_token_list_has("keep-alive, Upgrade", 19, "upgrade"));
    CHECK(!http_token_list_has("closed", 6, "close"));
    size_t v;
    CHECK(http_parse_uint("0123", 4, &v) == 0 && v == 123);
    CHECK(http_parse_uint("", 0, &v) == -1);
    CHECK(http_parse_uint("12a", 3, &v) == -1);
    const char *e = http_find_head_end("a\r\n\r\nrest", 9);
    CHECK(e && strcmp(e, "rest") == 0);
    CHECK(http_find_head_end("a\r\n\r", 4) == NULL);
}

int main(void) {
    test_valid();
    test_post_and_pipelined();
    test_incomplete();
    test_bad_requests();
    test_semantics();
    test_limits();
    test_response();
    test_helpers();
    TEST_DONE();
}
