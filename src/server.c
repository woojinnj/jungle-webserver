#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cgi.h"
#include "http.h"
#include "net.h"
#include "response.h"
#include "static_file.h"
#include "threadpool.h"

volatile sig_atomic_t g_stop = 0;

/* 요청 한 건을 한 줄로 기록한다: 메서드 URI 상태코드.
 * fprintf 는 호출 단위로 스레드 안전하다. */
static void log_request(const http_request *req, int status)
{
    fprintf(stderr, "[%lu] %s %s -> %d\n", (unsigned long)pthread_self(),
            req->method, req->uri, status);
}

/* 요청 종류에 따라 처리기를 고른다: Forward Proxy / Reverse Proxy / CGI / 정적 파일.
 * 반환: 응답한 상태코드 (-1: 전송 실패). */
int server_dispatch(int fd, server_config *cfg, const http_request *req)
{
    proxy_route *route;

    if (req->uri[0] != '/')
        return cfg->forward_proxy ? proxy_forward(fd, req)
                                  : (send_error(fd, 403, 0) < 0 ? -1 : 403);
    route = proxy_match_route(&cfg->proxy, req->path);
    if (route != NULL)
        return proxy_reverse(fd, &cfg->proxy, route, req);
    if (cgi_is_request(req->path))
        return cgi_serve(fd, cfg->cgi_dir, req);
    return static_serve(fd, cfg->root, req);
}

/* 이 응답 뒤에 연결을 유지할지 정한다.
 * 프록시 응답은 백엔드가 닫을 때까지 중계하므로 닫는다. 큐에 기다리는 연결이 있으면
 * 스레드를 양보하려고 닫는다 (스레드 수보다 keep-alive 연결이 많을 때 굶주림 방지). */
static int should_keep_alive(const http_request *req, server_config *cfg)
{
    if (req->uri[0] != '/')
        return 0;
    if (proxy_match_route(&cfg->proxy, req->path) != NULL)
        return 0;
    if (cfg->pool != NULL && threadpool_has_waiting(cfg->pool))
        return 0;
    return req->keep_alive;
}

/* 연결 하나에서 요청을 반복해서 처리한다 (keep-alive). 끝나면 소켓을 닫는다.
 * CSAPP 11.6 (doit) + RFC 7230 6.3 (persistent connection). */
void server_handle_connection(int fd, void *arg)
{
    server_config *cfg = arg;
    http_conn conn;
    http_request req;
    int status, keep = 1;

    if (net_set_timeout(fd, KEEPALIVE_TIMEOUT_SEC) < 0) {
        close(fd);
        return;
    }
    http_conn_init(&conn, fd);
    while (keep && !g_stop) {
        status = http_read_request(&conn, &req);
        if (status == 0) {
            req.keep_alive = should_keep_alive(&req, cfg);
            status = server_dispatch(fd, cfg, &req);
            log_request(&req, status);
            keep = (status >= 0) && req.keep_alive;
        } else {
            if (status > 0) {
                send_error(fd, status, 0);
                fprintf(stderr, "[%lu] (bad request) -> %d\n", (unsigned long)pthread_self(), status);
            }
            keep = 0;
        }
        http_request_free(&req);
    }
    close(fd);
}

/* 연결 하나를 accept 한다. 시그널로 깨어나면 -1 (종료 확인용).
 * CSAPP 11.4.6 (accept): 연결 요청이 올 때까지 블로킹된다. */
static int accept_client(int listenfd)
{
    int fd = accept(listenfd, NULL, NULL);

    if (fd < 0) {
        if (errno != EINTR)
            perror("accept");
        return -1;
    }
    if (net_set_cloexec(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 듣기 소켓 + 스레드 풀을 만들고, 종료 시그널이 올 때까지 연결을 받아 넘긴다.
 * CSAPP 12.5.5: 메인 스레드는 accept 만, 작업 스레드가 요청 처리. */
int server_run(server_config *cfg)
{
    threadpool pool;
    int listenfd, fd;

    listenfd = net_open_listenfd(cfg->port);
    if (listenfd < 0) {
        fprintf(stderr, "listen on port %d failed: %s\n", cfg->port, strerror(errno));
        return -1;
    }
    if (threadpool_init(&pool, cfg->threads, server_handle_connection, cfg) < 0) {
        close(listenfd);
        return -1;
    }
    cfg->pool = &pool;
    fprintf(stderr, "listening on port %d (threads=%d)\n", cfg->port, cfg->threads);
    while (!g_stop) {
        fd = accept_client(listenfd);
        if (fd >= 0 && threadpool_submit(&pool, fd) < 0)
            close(fd);
    }
    fprintf(stderr, "shutting down...\n");
    close(listenfd);
    threadpool_destroy(&pool);
    cfg->pool = NULL;
    return 0;
}
