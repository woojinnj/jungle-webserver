#include "http.h"
#include "proxy.h"
#include "static.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WORKERS 8
#define QUEUE_SIZE 128
#define REQUEST_MS 5000
#define RESPONSE_MS 15000

typedef struct Pool Pool;

typedef struct {
    pthread_t thread;
    int client;
    Pool *pool;
} Worker;

struct Pool {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    bool stopping;
    size_t first;
    size_t count;
    size_t started;
    int pending[QUEUE_SIZE];
    Worker workers[WORKERS];
    Routes *routes;
    int root;
};

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t wake_fd = -1;

static void stop_server(int signal_number)
{
    (void)signal_number;
    int saved = errno;
    stop_requested = 1;
    if (wake_fd >= 0)
        (void)write((int)wake_fd, "", 1);
    errno = saved;
}

static int install_signals(void)
{
    struct sigaction stop = {.sa_handler = stop_server};
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&stop.sa_mask);
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, NULL) < 0 ||
        sigaction(SIGINT, &stop, NULL) < 0 ||
        sigaction(SIGTERM, &stop, NULL) < 0)
        return -1;
    return 0;
}

static int read_body(Stream *stream, HttpRequest *request, Deadline deadline)
{
    size_t length = request->headers.length;
    if (length == 0)
        return 0;
    request->body = malloc(length);
    if (request->body == NULL)
        return 500;
    static const char continue_head[] = "HTTP/1.1 100 Continue\r\n\r\n";
    if (request->expect_continue &&
        send_all(stream->fd, continue_head, sizeof continue_head - 1, deadline) < 0)
        return 400;
    for (size_t received = 0; received < length;) {
        ssize_t count = stream_read(stream, request->body + received, length - received, deadline);
        if (count <= 0)
            return count < 0 && errno == ETIMEDOUT ? 408 : 400;
        received += (size_t)count;
    }
    return 0;
}

static void serve_client(Pool *pool, int fd)
{
    Stream stream = {.fd = fd};
    char head[HTTP_HEAD_LIMIT + 1];
    Deadline read_by = deadline_after(REQUEST_MS);
    int ready = stream_head(&stream, head, sizeof head, read_by);
    if (ready == 0)
        return;
    if (ready < 0) {
        int status = 400;
        if (errno == EMSGSIZE)
            status = 431;
        else if (errno == ETIMEDOUT)
            status = 408;
        http_send_error(fd, status, NULL, deadline_after(RESPONSE_MS));
        return;
    }

    HttpRequest request;
    int status = http_parse_request(head, &request);
    fprintf(stderr, "[3 parse] status=%d method=%s path=%s\n",
            status, request.method ? request.method : "-", request.path);
    if (status == 0)
        status = read_body(&stream, &request, read_by);
    Deadline write_by = deadline_after(RESPONSE_MS);
    if (status != 0) {
        http_send_error(fd, status, status == 405 ? "GET, POST" : NULL, write_by);
    } else {
        Route *route = routes_match(pool->routes, request.path);
        if (route != NULL) {
            const Backend *backend = route_pick(route);
            fprintf(stderr, "[4 proxy] %s -> %s\n", request.path, backend->authority);
            proxy_serve(fd, backend, &request, write_by);
        } else if (strcmp(request.method, "GET") == 0) {
            if (strcmp(request.path, "/echo") == 0) {
                http_send_error(fd, 405, "POST", write_by);
            } else {
                fprintf(stderr, "[4 static] 정적 파일 서빙: %s\n", request.path);
                static_serve(fd, pool->root, request.path, write_by);
            }
        } else if (strcmp(request.path, "/echo") == 0) {
            fprintf(stderr, "[4 echo] POST 본문 %zu바이트 에코\n", request.headers.length);
            if (http_send_head(fd, 200, "application/octet-stream",
                               request.headers.length, write_by) == 0)
                send_all(fd, request.body, request.headers.length, write_by);
        } else {
            http_send_error(fd, 405, "GET", write_by);
        }
    }
    free(request.body);
}

static void *run_worker(void *arg)
{
    Worker *worker = arg;
    Pool *pool = worker->pool;
    pthread_mutex_lock(&pool->mutex);
    for (;;) {
        while (!pool->stopping && pool->count == 0)
            pthread_cond_wait(&pool->ready, &pool->mutex);
        if (pool->stopping)
            break;
        int fd = pool->pending[pool->first];
        pool->first = (pool->first + 1) % QUEUE_SIZE;
        --pool->count;
        worker->client = fd;
        pthread_mutex_unlock(&pool->mutex);

        fprintf(stderr, "[2 worker #%ld] 큐에서 fd=%d 꺼냄\n",
                (long)(worker - pool->workers), fd);
        serve_client(pool, fd);
        shutdown(fd, SHUT_WR);

        pthread_mutex_lock(&pool->mutex);
        worker->client = -1;
        fprintf(stderr, "[5 worker #%ld] 응답 끝, fd=%d 종료\n",
                (long)(worker - pool->workers), fd);
        close(fd);
    }
    pthread_mutex_unlock(&pool->mutex);
    return NULL;
}

static void pool_stop(Pool *pool);

static int pool_start(Pool *pool)
{
    int error = pthread_mutex_init(&pool->mutex, NULL);
    if (error != 0)
        return error;
    error = pthread_cond_init(&pool->ready, NULL);
    if (error != 0) {
        pthread_mutex_destroy(&pool->mutex);
        return error;
    }

    for (; pool->started < WORKERS; ++pool->started) {
        Worker *worker = &pool->workers[pool->started];
        worker->client = -1;
        worker->pool = pool;
        error = pthread_create(&worker->thread, NULL, run_worker, worker);
        if (error != 0) {
            pool_stop(pool);
            return error;
        }
    }
    return 0;
}

static void pool_stop(Pool *pool)
{
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    for (size_t i = 0; i < pool->count; ++i)
        close(pool->pending[(pool->first + i) % QUEUE_SIZE]);
    for (size_t i = 0; i < pool->started; ++i) {
        if (pool->workers[i].client >= 0)
            shutdown(pool->workers[i].client, SHUT_RDWR);
    }
    pthread_cond_broadcast(&pool->ready);
    pthread_mutex_unlock(&pool->mutex);

    for (size_t i = 0; i < pool->started; ++i)
        pthread_join(pool->workers[i].thread, NULL);
    pthread_cond_destroy(&pool->ready);
    pthread_mutex_destroy(&pool->mutex);
}

static void enqueue(Pool *pool, int fd)
{
    pthread_mutex_lock(&pool->mutex);
    bool full = pool->count == QUEUE_SIZE;
    if (!full) {
        pool->pending[(pool->first + pool->count) % QUEUE_SIZE] = fd;
        ++pool->count;
        pthread_cond_signal(&pool->ready);
    }
    pthread_mutex_unlock(&pool->mutex);
    if (full) {
        static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\n"
                                   "Content-Length: 0\r\nConnection: close\r\n\r\n";
        send(fd, busy, sizeof busy - 1, 0);
        close(fd);
    }
}

static int listen_on(unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int enabled = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof enabled) < 0 ||
        set_nonblocking(fd) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof address) < 0 ||
        listen(fd, SOMAXCONN) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int accept_clients(int listener, int wake, Pool *pool)
{
    struct pollfd events[] = {
        {.fd = listener, .events = POLLIN},
        {.fd = wake, .events = POLLIN}
    };
    while (!stop_requested) {
        if (poll(events, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (stop_requested || events[1].revents != 0)
            break;
        if (events[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return -1;
        }

        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED)
                continue;
            return -1;
        }
        if (set_nonblocking(client) < 0) {
            close(client);
            continue;
        }
        fprintf(stderr, "[1 main] accept! client fd=%d -> 큐에 넣기\n", client);
        enqueue(pool, client);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argv[1][0] == '\0' ||
        strspn(argv[1], "0123456789") != strlen(argv[1])) {
        fprintf(stderr, "Usage: %s PORT DOCUMENT_ROOT [/path=HOST:PORT[,HOST:PORT] ...]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    unsigned long port = strtoul(argv[1], NULL, 10);
    if (port == 0 || port > 65535) {
        fputs("PORT must be between 1 and 65535\n", stderr);
        return EXIT_FAILURE;
    }

    int result = EXIT_FAILURE;
    int root = open(argv[2], O_RDONLY | O_DIRECTORY);
    if (root < 0) {
        perror(argv[2]);
        return result;
    }

    Routes routes = {0};
    for (int i = 3; i < argc; ++i) {
        if (routes_add(&routes, argv[i]) < 0) {
            fprintf(stderr, "Invalid route or unresolved backend: %s\n", argv[i]);
            goto close_root;
        }
    }
    int listener = listen_on((unsigned short)port);
    if (listener < 0) {
        perror("listen");
        goto close_root;
    }
    int wake[2];
    if (pipe(wake) < 0) {
        perror("pipe");
        goto close_listener;
    }
    if (set_nonblocking(wake[1]) < 0) {
        perror("fcntl");
        goto close_pipe;
    }
    wake_fd = wake[1];
    if (install_signals() < 0) {
        perror("sigaction");
        goto close_pipe;
    }

    sigset_t termination;
    sigemptyset(&termination);
    sigaddset(&termination, SIGINT);
    sigaddset(&termination, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &termination, NULL);
    if (error != 0) {
        fprintf(stderr, "Signal mask: %s\n", strerror(error));
        goto close_pipe;
    }
    Pool pool = {.root = root, .routes = &routes};
    error = pool_start(&pool);
    if (error != 0) {
        fprintf(stderr, "Worker pool: %s\n", strerror(error));
    } else {
        error = pthread_sigmask(SIG_UNBLOCK, &termination, NULL);
        if (error != 0) {
            fprintf(stderr, "Signal mask: %s\n", strerror(error));
        } else {
            fprintf(stderr, "Listening on 0.0.0.0:%lu with %d workers and %zu proxy routes\n",
                    port, WORKERS, routes.count);
            if (accept_clients(listener, wake[0], &pool) == 0)
                result = EXIT_SUCCESS;
            else
                perror("accept");
        }
        close(listener);
        listener = -1;
        pool_stop(&pool);
    }
close_pipe:
    wake_fd = -1;
    close(wake[0]);
    close(wake[1]);
close_listener:
    if (listener >= 0)
        close(listener);
close_root:
    routes_destroy(&routes);
    close(root);
    return result;
}
