#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"

#define PORT_STR_LEN 8

/* fd 에 FD_CLOEXEC 를 켠다: CGI 로 fork/exec 할 때 다른 연결 fd 가 자식에게 새지 않게.
 * CSAPP 10.2 (파일 디스크립터) / 8.4 (fork 시 fd 상속). */
int net_set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);

    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* 소켓의 송수신 타임아웃을 설정한다 (블로킹 recv/send 가 영원히 멈추지 않게).
 * 블로킹 I/O + 타임아웃: keep-alive 대기, 응답 없는 백엔드 대응. */
int net_set_timeout(int fd, int timeout_sec)
{
    struct timeval tv;

    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return -1;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* 주소 하나로 socket → setsockopt(SO_REUSEADDR) → bind → listen 을 시도한다.
 * CSAPP 11.4.8 (open_listenfd). */
static int try_listen_addr(const struct addrinfo *ai)
{
    int fd, optval = 1;

    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0 ||
        bind(fd, ai->ai_addr, ai->ai_addrlen) < 0 ||
        listen(fd, LISTEN_BACKLOG) < 0 ||
        net_set_cloexec(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 포트에서 연결을 기다리는 듣기 소켓을 연다. 실패하면 -1.
 * CSAPP 11.4.8: getaddrinfo(AI_PASSIVE) 로 얻은 주소를 차례로 시도. */
int net_open_listenfd(int port)
{
    struct addrinfo hints, *list, *p;
    char port_str[PORT_STR_LEN];
    int fd = -1, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    snprintf(port_str, sizeof(port_str), "%d", port);
    if ((rc = getaddrinfo(NULL, port_str, &hints, &list)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }
    for (p = list; p != NULL && fd < 0; p = p->ai_next)
        fd = try_listen_addr(p);
    freeaddrinfo(list);
    return fd;
}

/* fd 의 O_NONBLOCK 플래그를 켜거나 끈다.
 * 블로킹 / 논블로킹 소켓의 차이: 논블로킹 connect 는 바로 돌아온다. */
static int set_nonblocking(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags < 0)
        return -1;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags);
}

/* 논블로킹 connect 가 끝날 때까지 poll 로 최대 timeout_sec 기다린다. 성공 0.
 * 3-way handshake 가 끝나면 소켓이 쓰기 가능 상태가 된다. */
static int wait_connect(int fd, int timeout_sec)
{
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int err = 0, rc;
    socklen_t len = sizeof(err);

    do {
        rc = poll(&pfd, 1, timeout_sec * 1000);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0)
        return -1;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0)
        return -1;
    return 0;
}

/* 주소 하나로 타임아웃 있는 connect 를 시도한다. 성공하면 블로킹 소켓 fd.
 * CSAPP 11.4.7 (open_clientfd) + 타임아웃. */
static int try_connect_addr(const struct addrinfo *ai, int timeout_sec)
{
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

    if (fd < 0)
        return -1;
    if (net_set_cloexec(fd) < 0 || set_nonblocking(fd, 1) < 0)
        goto fail;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 &&
        (errno != EINPROGRESS || wait_connect(fd, timeout_sec) < 0))
        goto fail;
    if (set_nonblocking(fd, 0) < 0 || net_set_timeout(fd, timeout_sec) < 0)
        goto fail;
    return fd;
fail:
    close(fd);
    return -1;
}

/* host:port 로 TCP 연결을 연다 (DNS 조회 포함). 실패하면 -1.
 * CSAPP 11.4.7 / 11.3.2 (DNS): getaddrinfo 로 이름을 주소로 바꾼다. */
int net_connect_timeout(const char *host, const char *port, int timeout_sec)
{
    struct addrinfo hints, *list, *p;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(host, port, &hints, &list) != 0)
        return -1;
    for (p = list; p != NULL && fd < 0; p = p->ai_next)
        fd = try_connect_addr(p, timeout_sec);
    freeaddrinfo(list);
    return fd;
}
