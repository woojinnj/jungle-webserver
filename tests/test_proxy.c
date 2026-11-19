/* 프록시 단위 테스트: 라우팅 규칙 파싱, 접두사 매칭, Round-Robin, 절대 URI, 요청 재작성 */
#include "http.h"
#include "proxy.h"
#include "test.h"

static proxy_table table;

static void test_parse_route(void)
{
    proxy_route r;

    CHECK_INT(proxy_parse_route_arg("/api=127.0.0.1:8001,localhost:8002", &r), 0);
    CHECK_STR(r.prefix, "/api");
    CHECK_INT(r.backend_count, 2);
    CHECK_STR(r.backends[0].host, "127.0.0.1");
    CHECK_STR(r.backends[0].port, "8001");
    CHECK_STR(r.backends[1].host, "localhost");
    CHECK_INT(proxy_parse_route_arg("api=127.0.0.1:1", &r), -1);     /* / 로 시작 안 함 */
    CHECK_INT(proxy_parse_route_arg("/api", &r), -1);                /* = 없음 */
    CHECK_INT(proxy_parse_route_arg("/api=host:abc", &r), -1);       /* 포트가 숫자 아님 */
    CHECK_INT(proxy_parse_route_arg("/api=", &r), -1);               /* 백엔드 없음 */
}

static void test_match_and_round_robin(void)
{
    proxy_route *r;
    const char *ports[4];

    pthread_mutex_init(&table.lock, NULL);
    proxy_parse_route_arg("/api=h:1,h:2", &table.routes[0]);
    proxy_parse_route_arg("/api/v2=h:3", &table.routes[1]);
    table.route_count = 2;

    CHECK(proxy_match_route(&table, "/api") == &table.routes[0]);
    CHECK(proxy_match_route(&table, "/api/users") == &table.routes[0]);
    CHECK(proxy_match_route(&table, "/api/v2/x") == &table.routes[1]); /* 가장 긴 접두사 */
    CHECK(proxy_match_route(&table, "/apix") == NULL);                 /* 구간 경계 */
    CHECK(proxy_match_route(&table, "/static/a.css") == NULL);

    r = &table.routes[0];
    for (int i = 0; i < 4; i++)
        ports[i] = proxy_pick_backend(&table, r)->port;
    CHECK_STR(ports[0], "1");
    CHECK_STR(ports[1], "2");
    CHECK_STR(ports[2], "1");
    CHECK_STR(ports[3], "2");
    pthread_mutex_destroy(&table.lock);
}

static void test_absolute_uri(void)
{
    proxy_backend t;
    char path[256];

    CHECK_INT(proxy_parse_absolute_uri("http://example.com/a/b?q=1", &t, path, sizeof(path)), 0);
    CHECK_STR(t.host, "example.com");
    CHECK_STR(t.port, "80");
    CHECK_STR(path, "/a/b?q=1");
    CHECK_INT(proxy_parse_absolute_uri("http://127.0.0.1:8001", &t, path, sizeof(path)), 0);
    CHECK_STR(t.port, "8001");
    CHECK_STR(path, "/");
    CHECK_INT(proxy_parse_absolute_uri("https://x.com/", &t, path, sizeof(path)), -1);
    CHECK_INT(proxy_parse_absolute_uri("http:///a", &t, path, sizeof(path)), -1);
}

static void test_build_request(void)
{
    static http_request req;
    const char *head = "GET /api/x HTTP/1.1\r\nHost: front\r\nConnection: keep-alive\r\nX-Test: 1\r\n\r\n";
    char buf[1024];
    int len;

    http_parse_head(head, strlen(head), &req);
    len = proxy_build_request(&req, "/api/x", "front", buf, sizeof(buf));
    CHECK(len > 0);
    const char *expect = "GET /api/x HTTP/1.1\r\nHost: front\r\n";
    CHECK(strncmp(buf, expect, strlen(expect)) == 0);
    CHECK(strstr(buf, "X-Test: 1\r\n") != NULL);                 /* 일반 헤더는 전달 */
    CHECK(strstr(buf, "keep-alive") == NULL);                    /* hop-by-hop 제거 */
    CHECK(strstr(buf, "Connection: close\r\n\r\n") != NULL);
    CHECK_INT(proxy_build_request(&req, "/api/x", "front", buf, 20), -1); /* 버퍼 부족 */
}

int main(void)
{
    test_parse_route();
    test_match_and_round_robin();
    test_absolute_uri();
    test_build_request();
    return TEST_REPORT("proxy");
}
