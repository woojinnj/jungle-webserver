#ifndef NET_H
#define NET_H

#include <signal.h>
#include <stddef.h>
#include <sys/socket.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SIGPIPE is ignored globally + SO_NOSIGPIPE */
#endif

long long net_now_ms(void);

/* Per-socket options: TCP_NODELAY, send timeout, SO_NOSIGPIPE where present. */
void net_tune(int fd, int send_timeout_ms);

/* Writes all bytes. 0 ok, -1 on error/timeout/peer gone. */
int net_send_all(int fd, const void *buf, size_t len);

/* Waits for readable. 1 readable (or HUP/ERR: recv will tell), 0 timeout,
 * -1 error, -2 aborted because *stop became set (stop may be NULL). */
int net_wait_readable(int fd, int timeout_ms, volatile sig_atomic_t *stop);

/* Connects with a timeout. Returns a blocking socket or -1; *timed_out set
 * when failure was a timeout. */
int net_connect(const char *host, const char *port, int timeout_ms, int *timed_out);

#endif
