#include "http.h"
#include "proxy.h"
#include "static.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_request(const char *wire, int status)
{
    char head[HTTP_HEAD_LIMIT + 1];
    assert(strlen(wire) < sizeof head);
    strcpy(head, wire);
    HttpRequest request;
    assert(http_parse_request(head, &request) == status);
}

static void test_request(void)
{
    char message[] = "POST /dir/some%20file.txt?q=x%2By HTTP/1.1\r\n"
                     "hOsT: localhost:8080\r\ncontent-length: \t42 \t\r\n"
                     "Connection: Keep-Alive, X-Internal\r\n"
                     "X-Internal: value\r\nContent-Type: application/octet-stream\r\n\r\n";
    HttpRequest request;
    assert(http_parse_request(message, &request) == 0);
    assert(strcmp(request.method, "POST") == 0);
    assert(strcmp(request.target, "/dir/some%20file.txt?q=x%2By") == 0);
    assert(strcmp(request.path, "/dir/some file.txt") == 0);
    assert(request.http11 && request.headers.has_length && request.headers.length == 42);
    assert(http_skip_field(&request.headers, "x-internal"));
    assert(http_skip_field(&request.headers, "Content-Length"));
    assert(!http_skip_field(&request.headers, "Content-Type"));

    expect_request("GET / HTTP/1.0\r\n\r\n", 0);
    expect_request("GET / HTTP/1.1\r\n\r\n", 400);
    expect_request("GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n", 400);
    expect_request("GET / HTTP/1.1\r\nHost: [::1]:8080\r\n\r\n", 0);
    expect_request("GET / HTTP/1.1\r\nHost: [not-ipv6]\r\n\r\n", 400);
    expect_request("GET / HTTP/1.1\r\nHost: example.com:abc\r\n\r\n", 400);
    expect_request("GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400);
    expect_request("GET / HTTP/1.1\r\nHost: x\r\n", 400);
    expect_request("G@T / HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    expect_request("PUT / HTTP/1.1\r\nHost: x\r\n\r\n", 405);
    expect_request("GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505);
    expect_request("GET / HTTP/1.01\r\nHost: x\r\n\r\n", 400);
    expect_request("GET /bad% HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    expect_request("GET /bad%0 HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    expect_request("GET /a%00b HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    expect_request("GET /one/%2e%2e/two HTTP/1.1\r\nHost: x\r\n\r\n", 403);
    expect_request("GET / HTTP/1.1\r\nHost: x\r\nConnection: close,\r\n\r\n", 400);
    expect_request("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +1\r\n\r\n", 400);
    expect_request("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n"
                   "Content-Length: 1\r\n\r\n", 400);
    expect_request("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n"
                   "Transfer-Encoding: chunked\r\n\r\n", 400);
    expect_request("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
    expect_request("POST / HTTP/1.1\r\nHost: x\r\n"
                   "Content-Length: 9999999999999999999999999999\r\n\r\n", 413);
    expect_request("POST / HTTP/1.1\r\nHost: x\r\nExpect: something\r\n\r\n", 417);

    char long_path[HTTP_TARGET_LIMIT + 2];
    memset(long_path, 'a', sizeof long_path - 1);
    long_path[0] = '/';
    long_path[sizeof long_path - 1] = '\0';
    char wire[sizeof long_path + 64];
    snprintf(wire, sizeof wire, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", long_path);
    expect_request(wire, 414);
    long_path[HTTP_TARGET_LIMIT] = '\0';
    snprintf(wire, sizeof wire, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", long_path);
    expect_request(wire, 0);
}

static void test_response(void)
{
    HttpResponse response;
    char chunked[] = "HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n";
    assert(http_parse_response(chunked, &response) == 0);
    assert(response.status == 201 && !response.headers.has_length);
    assert(strcmp(response.headers.transfer_encoding, "chunked") == 0);
    char bad[] = "HTTP/1.1 999 Invalid\r\n\r\n";
    assert(http_parse_response(bad, &response) != 0);
}

static void test_mime(void)
{
    assert(strcmp(static_mime_type("index.html"), "text/html; charset=utf-8") == 0);
    assert(strcmp(static_mime_type("main.css"), "text/css; charset=utf-8") == 0);
    assert(strcmp(static_mime_type("main.js"), "text/javascript; charset=utf-8") == 0);
    assert(strcmp(static_mime_type("picture.PNG"), "image/png") == 0);
    assert(strcmp(static_mime_type("picture.jpeg"), "image/jpeg") == 0);
    assert(strcmp(static_mime_type("unknown"), "application/octet-stream") == 0);
}

static void test_routes(void)
{
    Routes routes = {0};
    assert(routes_add(&routes, "/api=127.0.0.1:8001,127.0.0.1:8002") == 0);
    assert(routes_add(&routes, "/api/v2/=127.0.0.1:8003") == 0);
    assert(routes_add(&routes, "/api=127.0.0.1:9000") != 0);
    assert(routes_add(&routes, "/invalid=127.0.0.1:8001,") != 0);
    assert(routes_add(&routes, "/invalid=127.0.0.1:99999") != 0);
    assert(routes_match(&routes, "/apix") == NULL);
    assert(routes_match(&routes, "/api/v2x") == routes_match(&routes, "/api"));
    Route *api = routes_match(&routes, "/api/get");
    Route *v2 = routes_match(&routes, "/api/v2");
    assert(api != NULL && v2 != NULL && api != v2);
    assert(strcmp(route_pick(api)->authority, "127.0.0.1:8001") == 0);
    assert(strcmp(route_pick(api)->authority, "127.0.0.1:8002") == 0);
    assert(strcmp(route_pick(api)->authority, "127.0.0.1:8001") == 0);
    assert(strcmp(route_pick(v2)->authority, "127.0.0.1:8003") == 0);
    routes_destroy(&routes);
}

int main(void)
{
    test_request();
    test_response();
    test_mime();
    test_routes();
    puts("Unit tests passed");
    return 0;
}
