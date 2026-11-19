/* HTTP 파서 단위 테스트: 요청 라인 / 헤더 / Content-Length / keep-alive / 오류 코드 */
#include <stdlib.h>
#include "http.h"
#include "test.h"

static http_request req;

/* 헤더 문자열 하나를 파싱해 상태코드를 돌려준다. */
static int parse(const char *head)
{
    memset(&req, 0, sizeof(req));
    return http_parse_head(head, strlen(head), &req);
}

static void test_valid_get(void)
{
    CHECK_INT(parse("GET /index.html?x=1 HTTP/1.1\r\nHost: a\r\nUser-Agent: t\r\n\r\n"), 0);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.uri, "/index.html?x=1");
    CHECK_STR(req.path, "/index.html");
    CHECK_STR(req.query, "x=1");
    CHECK_STR(req.version, "HTTP/1.1");
    CHECK_INT(req.header_count, 2);
    CHECK_STR(http_get_header(&req, "user-agent"), "t");   /* 대소문자 무시 */
    CHECK_INT(req.keep_alive, 1);                          /* 1.1 기본 유지 */
}

static void test_post_content_length(void)
{
    CHECK_INT(parse("POST /cgi-bin/echo HTTP/1.1\r\nHost: a\r\nContent-Length: 42\r\n\r\n"), 0);
    CHECK_INT(req.content_length, 42);
    CHECK_INT(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: abc\r\n\r\n"), 400);
    CHECK_INT(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999\r\n\r\n"), 413);
    CHECK_INT(parse("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"), 501);
}

static void test_connection_header(void)
{
    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n"), 0);
    CHECK_INT(req.keep_alive, 0);
    CHECK_INT(parse("GET / HTTP/1.0\r\n\r\n"), 0);                 /* 1.0 은 Host 없어도 됨 */
    CHECK_INT(req.keep_alive, 0);                                  /* 1.0 기본 종료 */
    CHECK_INT(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"), 0);
    CHECK_INT(req.keep_alive, 1);
}

static void test_bad_requests(void)
{
    CHECK_INT(parse("DELETE / HTTP/1.1\r\nHost: a\r\n\r\n"), 501);   /* 미지원 메서드 */
    CHECK_INT(parse("get / HTTP/1.1\r\nHost: a\r\n\r\n"), 400);      /* 소문자 메서드 */
    CHECK_INT(parse("GET / HTTP/2.0\r\nHost: a\r\n\r\n"), 505);      /* 미지원 버전 */
    CHECK_INT(parse("GET / FOO/1.1\r\nHost: a\r\n\r\n"), 400);       /* 이상한 버전 */
    CHECK_INT(parse("GET /\r\nHost: a\r\n\r\n"), 400);               /* 토큰 부족 */
    CHECK_INT(parse("GET / HTTP/1.1 extra\r\nHost: a\r\n\r\n"), 400); /* 토큰 초과 */
    CHECK_INT(parse("GET index.html HTTP/1.1\r\nHost: a\r\n\r\n"), 400); /* / 로 시작 안 함 */
    CHECK_INT(parse("GET / HTTP/1.1\r\n\r\n"), 400);                 /* 1.1 인데 Host 없음 */
    CHECK_INT(parse("GET / HTTP/1.1\r\nHost a\r\n\r\n"), 400);       /* 콜론 없는 헤더 */
    CHECK_INT(parse("GET / HTTP/1.1\r\nBad Name: x\r\n\r\n"), 400);  /* 이름에 공백 */
}

static void test_long_uri(void)
{
    char head[MAX_URI_LEN + 64];
    char uri[MAX_URI_LEN + 2];

    memset(uri, 'a', sizeof(uri) - 1);
    uri[0] = '/';
    uri[sizeof(uri) - 1] = '\0';
    snprintf(head, sizeof(head), "GET %s HTTP/1.1\r\nHost: a\r\n\r\n", uri);
    CHECK_INT(parse(head), 414);
}

static void test_too_many_headers(void)
{
    char head[MAX_HEAD_SIZE];
    size_t len = (size_t)snprintf(head, sizeof(head), "GET / HTTP/1.1\r\nHost: a\r\n");

    for (int i = 0; i < MAX_HEADERS; i++)
        len += (size_t)snprintf(head + len, sizeof(head) - len, "X-%d: v\r\n", i);
    snprintf(head + len, sizeof(head) - len, "\r\n");
    CHECK_INT(parse(head), 431);
}

static void test_find_head_end(void)
{
    const char *s = "GET / HTTP/1.1\r\nHost: a\r\n\r\nBODY";
    CHECK_INT(http_find_head_end(s, strlen(s)), (long)(strlen(s) - 4));
    CHECK_INT(http_find_head_end("GET / HTTP/1.1\r\n", 16), -1);       /* 불완전한 헤더 */
}

static void test_absolute_uri(void)
{
    CHECK_INT(parse("GET http://example.com/a HTTP/1.1\r\nHost: example.com\r\n\r\n"), 0);
    CHECK_STR(req.uri, "http://example.com/a");
}

int main(void)
{
    test_valid_get();
    test_post_content_length();
    test_connection_header();
    test_bad_requests();
    test_long_uri();
    test_too_many_headers();
    test_find_head_end();
    test_absolute_uri();
    return TEST_REPORT("http");
}
