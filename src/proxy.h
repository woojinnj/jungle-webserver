#ifndef PROXY_H
#define PROXY_H

#include <pthread.h>
#include <stddef.h>
#include "config.h"
#include "http.h"

#define PORT_LEN 8

typedef struct {
    char host[MAX_HOST_LEN];
    char port[PORT_LEN];
} proxy_backend;

/* 라우팅 규칙 하나: prefix 로 시작하는 경로를 backends 로 돌아가며(Round-Robin) 보낸다. */
typedef struct {
    char prefix[MAX_PREFIX_LEN];
    proxy_backend backends[MAX_BACKENDS];
    int backend_count;
    unsigned int next;             /* 다음에 고를 백엔드 번호 (lock 으로 보호) */
} proxy_route;

typedef struct {
    proxy_route routes[MAX_ROUTES];
    int route_count;
    pthread_mutex_t lock;
} proxy_table;

int proxy_parse_hostport(const char *s, proxy_backend *out);
int proxy_parse_route_arg(const char *arg, proxy_route *out);
proxy_route *proxy_match_route(proxy_table *table, const char *path);
const proxy_backend *proxy_pick_backend(proxy_table *table, proxy_route *route);
int proxy_parse_absolute_uri(const char *uri, proxy_backend *target, char *path, size_t path_size);
int proxy_build_request(const http_request *req, const char *path, const char *host_header,
                        char *buf, size_t size);
int proxy_reverse(int client_fd, proxy_table *table, proxy_route *route, const http_request *req);
int proxy_forward(int client_fd, const http_request *req);

#endif
