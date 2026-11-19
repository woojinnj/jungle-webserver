#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

long long net_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void net_tune(int fd, int send_timeout_ms) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    struct timeval tv;
    tv.tv_sec = send_timeout_ms / 1000;
    tv.tv_usec = (send_timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

int net_send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int net_wait_readable(int fd, int timeout_ms, volatile sig_atomic_t *stop) {
    long long deadline = net_now_ms() + timeout_ms;
    for (;;) {
        if (stop && *stop) return -2;
        long long left = deadline - net_now_ms();
        if (left <= 0) return 0;
        /* 200ms slices so a shutdown flag is noticed promptly */
        int slice = left > 200 ? 200 : (int)left;
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, slice);
        if (r > 0) return 1;
        if (r < 0 && errno != EINTR) return -1;
    }
}

static void set_nonblock(int fd, int on) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

int net_connect(const char *host, const char *port, int timeout_ms, int *timed_out) {
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    *timed_out = 0;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_nonblock(fd, 1);
        int ok = 0;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            ok = 1;
        } else if (errno == EINPROGRESS) {
            struct pollfd p = {fd, POLLOUT, 0};
            int r;
            do {
                r = poll(&p, 1, timeout_ms);
            } while (r < 0 && errno == EINTR);
            if (r == 0) {
                *timed_out = 1;
            } else if (r > 0) {
                int err = 0;
                socklen_t el = sizeof err;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) ok = 1;
            }
        }
        if (ok) {
            *timed_out = 0;
            set_nonblock(fd, 0);
            net_tune(fd, 10000);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}
