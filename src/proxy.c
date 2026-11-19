#include "proxy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "net.h"
#include "response.h"

#define DEFAULT_HTTP_PORT "80"

/* "host:port" 문자열을 백엔드 주소로 나눈다. 0 성공, -1 형식 오류.
 * 규칙 6: host/port 길이를 버퍼 크기 안으로 제한한다. */
int proxy_parse_hostport(const char *s, proxy_backend *out)
{
    const char *colon = strrchr(s, ':');
    size_t host_len;

    if (colon == NULL) {
        if (*s == '\0' || strlen(s) >= sizeof(out->host))
            return -1;
        strcpy(out->host, s);
        strcpy(out->port, DEFAULT_HTTP_PORT);
        return 0;
    }
    host_len = (size_t)(colon - s);
    if (host_len == 0 || host_len >= sizeof(out->host) ||
        colon[1] == '\0' || strlen(colon + 1) >= sizeof(out->port))
        return -1;
    for (const char *p = colon + 1; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
    }
    memcpy(out->host, s, host_len);
    out->host[host_len] = '\0';
    strcpy(out->port, colon + 1);
    return 0;
}

/* 라우팅 규칙 "/api=127.0.0.1:8001,127.0.0.1:8002" 를 파싱한다. 0 성공, -1 오류.
 * Reverse Proxy: 경로 접두사 → 백엔드 목록. */
int proxy_parse_route_arg(const char *arg, proxy_route *out)
{
    const char *eq = strchr(arg, '=');
    char list[MAX_BACKENDS * (MAX_HOST_LEN + PORT_LEN)];
    char *item, *save = NULL;
    size_t prefix_len;

    if (eq == NULL || arg[0] != '/')
        return -1;
    prefix_len = (size_t)(eq - arg);
    if (prefix_len >= sizeof(out->prefix) || strlen(eq + 1) >= sizeof(list))
        return -1;
    memcpy(out->prefix, arg, prefix_len);
    out->prefix[prefix_len] = '\0';
    strcpy(list, eq + 1);
    out->backend_count = 0;
    out->next = 0;
    for (item = strtok_r(list, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        if (out->backend_count >= MAX_BACKENDS ||
            proxy_parse_hostport(item, &out->backends[out->backend_count]) < 0)
            return -1;
        out->backend_count++;
    }
    return out->backend_count > 0 ? 0 : -1;
}

/* path 가 prefix 로 시작하는지 (경로 구간 단위로) 검사한다.
 * "/api" 는 "/api", "/api/x" 와 맞고 "/apix" 와는 맞지 않는다. */
static int prefix_matches(const char *prefix, const char *path)
{
    size_t n = strlen(prefix);

    if (strncmp(prefix, path, n) != 0)
        return 0;
    return prefix[n - 1] == '/' || path[n] == '\0' || path[n] == '/';
}

/* 경로에 맞는 라우팅 규칙을 찾는다 (가장 긴 접두사 우선). 없으면 NULL.
 * Reverse Proxy: 경로 기반 라우팅. */
proxy_route *proxy_match_route(proxy_table *table, const char *path)
{
    proxy_route *best = NULL;

    for (int i = 0; i < table->route_count; i++) {
        proxy_route *r = &table->routes[i];
        if (prefix_matches(r->prefix, path) &&
            (best == NULL || strlen(r->prefix) > strlen(best->prefix)))
            best = r;
    }
    return best;
}

/* 규칙의 백엔드를 차례대로(Round-Robin) 하나 고른다.
 * 여러 스레드가 동시에 next 를 바꾸므로 mutex 로 보호한다 (CSAPP 12.5). */
const proxy_backend *proxy_pick_backend(proxy_table *table, proxy_route *route)
{
    unsigned int idx;

    pthread_mutex_lock(&table->lock);
    idx = route->next % (unsigned int)route->backend_count;
    route->next = idx + 1;
    pthread_mutex_unlock(&table->lock);
    return &route->backends[idx];
}

/* "http://host:port/path?q" 를 대상 주소와 경로로 나눈다. 0 성공, -1 오류.
 * Forward Proxy: 프록시로 오는 요청 라인에는 절대 URI 가 들어 있다 (RFC 7230 5.3.2). */
int proxy_parse_absolute_uri(const char *uri, proxy_backend *target, char *path, size_t path_size)
{
    const char *host_start, *slash;
    char hostport[MAX_HOST_LEN + PORT_LEN];
    size_t hp_len;

    if (strncasecmp(uri, "http://", 7) != 0)
        return -1;
    host_start = uri + 7;
    slash = strchr(host_start, '/');
    hp_len = slash ? (size_t)(slash - host_start) : strlen(host_start);
    if (hp_len == 0 || hp_len >= sizeof(hostport))
        return -1;
    memcpy(hostport, host_start, hp_len);
    hostport[hp_len] = '\0';
    if (proxy_parse_hostport(hostport, target) < 0)
        return -1;
    if (snprintf(path, path_size, "%s", slash ? slash : "/") >= (int)path_size)
        return -1;
    return 0;
}

/* 백엔드로 넘기지 않는 hop-by-hop 헤더인지 검사한다.
 * RFC 7230 6.1: Connection 계열 헤더는 연결 구간마다 따로 정한다. */
static int is_hop_by_hop(const char *name)
{
    return strcasecmp(name, "Connection") == 0 || strcasecmp(name, "Proxy-Connection") == 0 ||
           strcasecmp(name, "Keep-Alive") == 0 || strcasecmp(name, "Host") == 0;
}

/* 버퍼 뒤에 문자열을 붙인다. 넘치면 -1.
 * 규칙 6: 요청을 다시 만들 때 버퍼 크기를 넘지 않게. */
static int append_str(char *buf, size_t size, size_t *len, const char *fmt_a, const char *b, const char *c)
{
    int n = snprintf(buf + *len, size - *len, fmt_a, b, c);

    if (n < 0 || (size_t)n >= size - *len)
        return -1;
    *len += (size_t)n;
    return 0;
}

/* 백엔드에 보낼 요청 헤더를 새로 만든다 (Connection: close 로 한 번만 쓰고 닫음). 길이 또는 -1.
 * 프록시는 요청 라인의 경로와 Host 를 바꾸고 나머지 헤더는 그대로 전달한다. */
int proxy_build_request(const http_request *req, const char *path, const char *host_header,
                        char *buf, size_t size)
{
    size_t len = 0;

    if (append_str(buf, size, &len, "%s %s HTTP/1.1\r\n", req->method, path) < 0 ||
        append_str(buf, size, &len, "%s%s\r\n", "Host: ", host_header) < 0)
        return -1;
    for (int i = 0; i < req->header_count; i++) {
        if (is_hop_by_hop(req->headers[i].name))
            continue;
        if (append_str(buf, size, &len, "%s: %s\r\n", req->headers[i].name, req->headers[i].value) < 0)
            return -1;
    }
    if (append_str(buf, size, &len, "%s%s", "Connection: close\r\n\r\n", "") < 0)
        return -1;
    return (int)len;
}

/* 첫 응답 조각에서 상태코드를 읽는다 (로그용). 못 읽으면 200 으로 본다.
 * 응답 상태 라인 "HTTP/1.x NNN ..." */
static int peek_status_code(const char *buf, size_t len)
{
    int code;

    if (len >= 12 && strncmp(buf, "HTTP/1.", 7) == 0 && sscanf(buf + 9, "%3d", &code) == 1)
        return code;
    return 200;
}

/* 백엔드 응답을 EOF 까지 그대로 클라이언트에 중계한다.
 * 반환: 응답 상태코드, 0(받은 게 없음: 끊김/타임아웃), -1(클라이언트 전송 실패). */
static int relay_response(int backend_fd, int client_fd, int *timed_out)
{
    char buf[IO_CHUNK_SIZE];
    size_t total = 0;
    int status = 0;
    ssize_t n;

    *timed_out = 0;
    while ((n = recv(backend_fd, buf, sizeof(buf), 0)) != 0) {
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            *timed_out = (errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
        if (total == 0)
            status = peek_status_code(buf, (size_t)n);
        if (send_all(client_fd, buf, (size_t)n) < 0)
            return -1;
        total += (size_t)n;
    }
    return status;
}

/* 요청을 target 으로 전달하고 응답을 중계한다. 응답 상태코드 (-1: 클라이언트 전송 실패).
 * 실패 처리: 연결 실패 → 502, 응답 없음(타임아웃) → 504. */
static int forward_to(int client_fd, const proxy_backend *target, const char *path,
                      const char *host_header, const http_request *req)
{
    char head[MAX_HEAD_SIZE];
    int backend_fd, len, status, timed_out;

    len = proxy_build_request(req, path, host_header, head, sizeof(head));
    if (len < 0)
        return send_error(client_fd, 431, 0) < 0 ? -1 : 431;
    backend_fd = net_connect_timeout(target->host, target->port, BACKEND_TIMEOUT_SEC);
    if (backend_fd < 0)
        return send_error(client_fd, 502, 0) < 0 ? -1 : 502;
    if (send_all(backend_fd, head, (size_t)len) < 0 ||
        (req->body_len > 0 && send_all(backend_fd, req->body, req->body_len) < 0)) {
        close(backend_fd);
        return send_error(client_fd, 502, 0) < 0 ? -1 : 502;
    }
    status = relay_response(backend_fd, client_fd, &timed_out);
    close(backend_fd);
    if (status == 0)
        return send_error(client_fd, timed_out ? 504 : 502, 0) < 0 ? -1 : (timed_out ? 504 : 502);
    return status;
}

/* Reverse Proxy: 규칙에 맞는 백엔드 하나를 골라 요청을 전달한다.
 * 클라이언트 → Reverse Proxy → 백엔드(1..N), Host 는 클라이언트 값을 유지. */
int proxy_reverse(int client_fd, proxy_table *table, proxy_route *route, const http_request *req)
{
    const proxy_backend *b = proxy_pick_backend(table, route);
    const char *host = http_get_header(req, "Host");

    return forward_to(client_fd, b, req->uri, host ? host : b->host, req);
}

/* Forward Proxy: 절대 URI 의 호스트로 새 TCP 연결을 만들어 요청을 대신 전달한다.
 * 클라이언트 → Proxy → 원 서버. */
int proxy_forward(int client_fd, const http_request *req)
{
    proxy_backend target;
    char path[MAX_URI_LEN];
    char host_header[MAX_HOST_LEN + PORT_LEN];

    if (proxy_parse_absolute_uri(req->uri, &target, path, sizeof(path)) < 0)
        return send_error(client_fd, 400, 0) < 0 ? -1 : 400;
    if (strcmp(target.port, DEFAULT_HTTP_PORT) == 0)
        snprintf(host_header, sizeof(host_header), "%s", target.host);
    else
        snprintf(host_header, sizeof(host_header), "%s:%s", target.host, target.port);
    return forward_to(client_fd, &target, path, host_header, req);
}
