#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "net.h"
#include "proxy.h"
#include "router.h"
#include "static.h"

#define IDLE_TIMEOUT_MS 5000     /* keep-alive connection with nothing pending */
#define REQUEST_TIMEOUT_MS 15000 /* whole head + body must arrive in this time (slowloris) */
#define SEND_TIMEOUT_MS 10000
#define MAX_REQS_PER_CONN 100

static volatile sig_atomic_t g_stop = 0;
void server_request_stop(void) { g_stop = 1; }

typedef struct {
    int fd;
    char ip[INET6_ADDRSTRLEN];
} job;

/* Bounded FIFO between the acceptor and the workers. */
typedef struct {
    const config *cfg;
    job *q;
    size_t cap, head, count;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int closed;
} pool;

static int pool_push(pool *p, job j) {
    int ok = 0;
    pthread_mutex_lock(&p->mu);
    if (p->count < p->cap) {
        p->q[(p->head + p->count++) % p->cap] = j;
        ok = 1;
        pthread_cond_signal(&p->cv);
    }
    pthread_mutex_unlock(&p->mu);
    return ok;
}

static int pool_pop(pool *p, job *j) {
    pthread_mutex_lock(&p->mu);
    while (p->count == 0 && !p->closed) pthread_cond_wait(&p->cv, &p->mu);
    int ok = p->count > 0;
    if (ok) {
        *j = p->q[p->head];
        p->head = (p->head + 1) % p->cap;
        p->count--;
    }
    pthread_mutex_unlock(&p->mu);
    return ok;
}

/* Connections waiting for a free worker. */
static int pool_waiting(pool *p) {
    pthread_mutex_lock(&p->mu);
    int n = p->count > 0;
    pthread_mutex_unlock(&p->mu);
    return n;
}

static void log_access(const config *cfg, const char *ip, const http_request *r, int status,
                       size_t bytes) {
    if (cfg->quiet) return;
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    if (r)
        fprintf(stderr, "%s %s \"%.*s %.*s\" %d %zu\n", ts, ip, (int)r->method_len, r->method_str,
                (int)r->uri_len, r->uri, status, bytes);
    else
        fprintf(stderr, "%s %s \"-\" %d 0\n", ts, ip, status);
}

static int dispatch(const config *cfg, int fd, const http_request *req, const char *body,
                    size_t blen, const char *ip, int *keep, size_t *bytes) {
    const char *q = memchr(req->uri, '?', req->uri_len);
    size_t plen = q ? (size_t)(q - req->uri) : req->uri_len;

    route *rt = router_match(&cfg->rt, req->uri, plen);
    if (rt) return proxy_handle(fd, rt, req, body, blen, ip, cfg->upstream_timeout_ms, keep, bytes);

    *bytes = 0;
    if (req->method == M_GET || req->method == M_HEAD)
        return static_serve(fd, cfg->root, req, *keep, bytes);
    if (req->method == M_POST && plen == 5 && memcmp(req->uri, "/echo", 5) == 0) {
        http_send_response(fd, 200, "text/plain; charset=utf-8", body, blen, *keep, NULL, 0);
        *bytes = blen;
        return 200;
    }
    http_send_error(fd, 405, *keep, 0, "Allow: GET, HEAD, POST\r\n");
    return 405;
}

/* Reads exactly n body bytes (first those already buffered). 0 ok, -1 failed. */
static int read_body(int fd, char *dst, size_t n, const char *have, size_t havelen,
                     long long deadline) {
    size_t got = havelen < n ? havelen : n;
    memcpy(dst, have, got);
    while (got < n) {
        long long left = deadline - net_now_ms();
        if (left <= 0 || net_wait_readable(fd, (int)left, NULL) <= 0) return -1;
        ssize_t r = recv(fd, dst + got, n - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* After an error reply the client may still be sending. Closing with unread
 * data makes the kernel send RST, which can destroy our reply before the
 * client reads it, so half-close and drain briefly first. */
static void linger_close(int fd) {
    shutdown(fd, SHUT_WR);
    char b[4096];
    size_t total = 0;
    long long end = net_now_ms() + 500;
    while (total < 65536) {
        long long left = end - net_now_ms();
        if (left <= 0 || net_wait_readable(fd, (int)left, NULL) <= 0) break;
        ssize_t n = recv(fd, b, sizeof b, 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
}

/* One client connection: keep-alive loop of parse -> dispatch -> respond. */
static void handle_conn(pool *pl, int fd, const char *ip) {
    const config *cfg = pl->cfg;
    net_tune(fd, SEND_TIMEOUT_MS);
    char *rbuf = malloc(HTTP_MAX_HEADER_BYTES);
    if (!rbuf) {
        close(fd);
        return;
    }
    size_t have = 0;
    int served = 0, linger = 0;

    for (;;) {
        http_request req;
        int pr;
        long long started = have > 0 ? net_now_ms() : 0;

        /* read until a full head is buffered */
        for (;;) {
            if (have > 0) {
                pr = http_parse_request(rbuf, have, &req);
                if (pr != HTTP_PARSE_INCOMPLETE) break;
            }
            int timeout = IDLE_TIMEOUT_MS;
            if (have > 0) {
                long long left = started + REQUEST_TIMEOUT_MS - net_now_ms();
                timeout = left > 0 ? (int)left : 0;
            }
            /* only an idle connection is abandoned on shutdown, never a half-read request */
            int w = net_wait_readable(fd, timeout, have == 0 ? &g_stop : NULL);
            if (w == 0 && have > 0) {
                http_send_error(fd, 408, 0, 0, NULL);
                log_access(cfg, ip, NULL, 408, 0);
                linger = 1;
            }
            if (w <= 0) goto done;
            ssize_t n = recv(fd, rbuf + have, HTTP_MAX_HEADER_BYTES - have, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) goto done; /* client closed or reset: just release resources */
            if (have == 0) started = net_now_ms();
            have += (size_t)n;
        }
        if (pr > 0) { /* malformed request: answer once, then close */
            http_send_error(fd, pr, 0, 0, NULL);
            log_access(cfg, ip, NULL, pr, 0);
            linger = 1;
            goto done;
        }
        if (req.content_length > HTTP_MAX_BODY) {
            http_send_error(fd, 413, 0, 0, NULL);
            log_access(cfg, ip, &req, 413, 0);
            linger = 1;
            goto done;
        }
        if (req.expect_continue && req.content_length > 0)
            net_send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);

        char *body = NULL;
        size_t blen = req.content_length;
        if (blen > 0) {
            body = malloc(blen);
            if (!body || read_body(fd, body, blen, rbuf + req.head_len, have - req.head_len,
                                   started + REQUEST_TIMEOUT_MS) != 0) {
                free(body);
                http_send_error(fd, 408, 0, 0, NULL);
                log_access(cfg, ip, &req, 408, 0);
                linger = 1;
                goto done;
            }
        }

        /* thread-per-connection: when others wait for a worker, do not hold this one
         * for an idle keep-alive client (otherwise > threads clients starve) */
        int keep = req.keep_alive && !g_stop && ++served < MAX_REQS_PER_CONN && !pool_waiting(pl);
        size_t bytes = 0;
        int status = dispatch(cfg, fd, &req, body, blen, ip, &keep, &bytes);
        log_access(cfg, ip, &req, status, bytes);
        free(body);

        /* drop this request, keep any pipelined bytes of the next one */
        size_t used = req.head_len + blen;
        if (used > have) used = have;
        memmove(rbuf, rbuf + used, have - used);
        have -= used;
        if (!keep) break;
    }
done:
    if (linger) linger_close(fd);
    free(rbuf);
    close(fd);
}

static void *worker(void *arg) {
    pool *p = arg;
    job j;
    while (pool_pop(p, &j)) handle_conn(p, j.fd, j.ip);
    return NULL;
}

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1; /* restart right after a stop without waiting out TIME_WAIT */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "bind to port %d failed: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 512) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int server_run(const config *cfg) {
    int lfd = listen_on(cfg->port);
    if (lfd < 0) return 1;

    pool p;
    memset(&p, 0, sizeof p);
    p.cfg = cfg;
    p.cap = (size_t)cfg->queue;
    p.q = calloc(p.cap, sizeof *p.q);
    pthread_t *th = calloc((size_t)cfg->threads, sizeof *th);
    if (!p.q || !th) {
        fprintf(stderr, "out of memory\n");
        free(p.q);
        free(th);
        close(lfd);
        return 1;
    }
    pthread_mutex_init(&p.mu, NULL);
    pthread_cond_init(&p.cv, NULL);

    int started = 0;
    for (int i = 0; i < cfg->threads; i++) {
        if (pthread_create(&th[i], NULL, worker, &p) != 0) break;
        started++;
    }
    if (started == 0) {
        fprintf(stderr, "cannot start worker threads\n");
        g_stop = 1;
    }
    fprintf(stderr, "listening on :%d  root=%s  threads=%d  routes=%d\n", cfg->port, cfg->root,
            started, cfg->rt.n);

    while (!g_stop) {
        struct pollfd pf = {lfd, POLLIN, 0};
        int r = poll(&pf, 1, 200);
        if (r <= 0) continue;
        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        int cfd = accept(lfd, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) {
            if (errno == EMFILE || errno == ENFILE) { /* out of fds: back off instead of spinning */
                struct timespec ts = {0, 50 * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
            continue;
        }
        job j;
        j.fd = cfd;
        inet_ntop(AF_INET, &ca.sin_addr, j.ip, sizeof j.ip);
        if (!pool_push(&p, j)) { /* all workers busy and queue full: shed load */
            net_tune(cfd, 1000);
            http_send_error(cfd, 503, 0, 0, "Retry-After: 1\r\n");
            close(cfd);
        }
    }

    fprintf(stderr, "shutting down...\n");
    close(lfd);
    pthread_mutex_lock(&p.mu);
    p.closed = 1; /* workers drain the queue, finish their request, then exit */
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.mu);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    pthread_mutex_destroy(&p.mu);
    pthread_cond_destroy(&p.cv);
    free(p.q);
    free(th);
    fprintf(stderr, "bye\n");
    return 0;
}
