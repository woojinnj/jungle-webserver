#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

/* 사용법을 출력한다.
 * 실행 인자: 포트, 정적 루트, CGI 디렉토리, 스레드 수, 라우팅 규칙, Forward Proxy. */
static void print_usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-p port] [-r root] [-c cgi_dir] [-t threads] [-R /prefix=host:port,...]... [-F]\n"
            "  -p  listen port (default %d)\n"
            "  -r  static file root (default ./www)\n"
            "  -c  CGI program directory (default ./cgi-bin)\n"
            "  -t  worker threads (default %d)\n"
            "  -R  reverse proxy route, repeatable (e.g. -R /api=127.0.0.1:8001,127.0.0.1:8002)\n"
            "  -F  enable forward proxy (absolute-URI requests)\n",
            prog, DEFAULT_PORT, DEFAULT_THREADS);
}

/* SIGINT/SIGTERM 핸들러: 종료 플래그만 세운다 (async-signal-safe).
 * CSAPP 8.5.5: 핸들러는 가능한 단순하게, volatile sig_atomic_t 플래그만. */
static void handle_stop_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* 종료 시그널 핸들러를 달고 SIGPIPE 를 무시한다. 0 성공.
 * SA_RESTART 를 빼야 accept 가 EINTR 로 깨어나 종료 플래그를 확인한다.
 * SIGPIPE 무시: 끊긴 소켓에 쓰면 프로세스가 죽는 대신 send 가 -1(EPIPE). */
static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    sa.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &sa, NULL);
}

/* 디렉토리 경로를 절대 경로로 정규화해 dst 에 저장한다. 0 성공.
 * 루트 밖 접근 검사(static_is_inside_root)를 위해 절대 경로가 필요하다. */
static int normalize_dir(const char *src, char *dst)
{
    char buf[PATH_MAX];

    if (realpath(src, buf) == NULL || strlen(buf) >= MAX_PATH_LEN) {
        perror(src);
        return -1;
    }
    strcpy(dst, buf);
    return 0;
}

/* 라우팅 규칙 하나를 설정에 추가한다. 0 성공.
 * -R 옵션은 여러 번 줄 수 있다. */
static int add_route(server_config *cfg, const char *arg)
{
    if (cfg->proxy.route_count >= MAX_ROUTES ||
        proxy_parse_route_arg(arg, &cfg->proxy.routes[cfg->proxy.route_count]) < 0) {
        fprintf(stderr, "invalid route: %s\n", arg);
        return -1;
    }
    cfg->proxy.route_count++;
    return 0;
}

/* CLI 인자를 읽어 설정을 채운다. 0 성공, -1 잘못된 인자.
 * getopt 로 옵션을 하나씩 처리한다. */
static int parse_args(int argc, char **argv, server_config *cfg)
{
    const char *root = "./www", *cgi = "./cgi-bin";
    int opt;

    cfg->port = DEFAULT_PORT;
    cfg->threads = DEFAULT_THREADS;
    while ((opt = getopt(argc, argv, "p:r:c:t:R:F")) != -1) {
        switch (opt) {
        case 'p': cfg->port = atoi(optarg); break;
        case 'r': root = optarg; break;
        case 'c': cgi = optarg; break;
        case 't': cfg->threads = atoi(optarg); break;
        case 'R': if (add_route(cfg, optarg) < 0) return -1; break;
        case 'F': cfg->forward_proxy = 1; break;
        default: return -1;
        }
    }
    if (cfg->port <= 0 || cfg->port > 65535 || cfg->threads <= 0)
        return -1;
    if (normalize_dir(root, cfg->root) < 0 || normalize_dir(cgi, cfg->cgi_dir) < 0)
        return -1;
    return 0;
}

/* 프로그램 시작점: 인자 해석 → 시그널 설정 → 서버 실행.
 * 설정 구조체는 스레드들이 공유하므로 static 으로 둔다. */
int main(int argc, char **argv)
{
    static server_config cfg;
    int rc;

    if (parse_args(argc, argv, &cfg) < 0) {
        print_usage(argv[0]);
        return 1;
    }
    if (install_signal_handlers() < 0) {
        perror("sigaction");
        return 1;
    }
    if (pthread_mutex_init(&cfg.proxy.lock, NULL) != 0)
        return 1;
    rc = server_run(&cfg);
    pthread_mutex_destroy(&cfg.proxy.lock);
    return rc < 0 ? 1 : 0;
}
