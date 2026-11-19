#ifndef SERVER_H
#define SERVER_H

#include <signal.h>
#include "config.h"
#include "proxy.h"

struct threadpool;

/* 실행 설정: CLI 인자로 채운다. */
typedef struct {
    int port;
    int threads;
    char root[MAX_PATH_LEN];       /* 정적 파일 루트 (realpath 로 정규화) */
    char cgi_dir[MAX_PATH_LEN];    /* CGI 프로그램 디렉토리 */
    int forward_proxy;             /* 1: Forward Proxy 허용 */
    proxy_table proxy;             /* Reverse Proxy 라우팅 규칙 */
    struct threadpool *pool;       /* 대기 연결 확인용 (server_run 이 채움) */
} server_config;

extern volatile sig_atomic_t g_stop;

int server_dispatch(int fd, server_config *cfg, const http_request *req);
void server_handle_connection(int fd, void *arg);
int server_run(server_config *cfg);

#endif
