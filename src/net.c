#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static Deadline monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (Deadline)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

Deadline deadline_after(int milliseconds)
{
    return monotonic_ms() + milliseconds;
}

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int remaining_ms(Deadline deadline)
{
    Deadline remaining = deadline - monotonic_ms();
    if (remaining <= 0) {
        errno = ETIMEDOUT;
        return 0;
    }
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

int await_socket(int fd, short events, Deadline deadline)
{
    struct pollfd pfd = {.fd = fd, .events = events};
    int timeout;
    while ((timeout = remaining_ms(deadline)) > 0) {
        int ready = poll(&pfd, 1, timeout);
        if (ready > 0)
            return 0;
        if (ready < 0 && errno != EINTR)
            break;
    }
    return -1;
}

int send_all(int fd, const void *data, size_t length, Deadline deadline)
{
    const unsigned char *bytes = data;
    while (length > 0 && remaining_ms(deadline) > 0) {
        ssize_t count = send(fd, bytes, length, 0);
        if (count > 0) {
            bytes += count;
            length -= (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (await_socket(fd, POLLOUT, deadline) < 0)
                return -1;
        } else {
            return -1;
        }
    }
    return length == 0 ? 0 : -1;
}

static ssize_t fill_stream(Stream *stream, Deadline deadline)
{
    while (remaining_ms(deadline) > 0) {
        ssize_t count = recv(stream->fd, stream->buffer, sizeof stream->buffer, 0);
        if (count > 0) {
            stream->start = 0;
            stream->end = (size_t)count;
            return count;
        }
        if (count == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        if (await_socket(stream->fd, POLLIN, deadline) < 0)
            return -1;
    }
    return -1;
}

ssize_t stream_read(Stream *stream, void *data, size_t length, Deadline deadline)
{
    if (length == 0)
        return 0;
    if (stream->start == stream->end) {
        ssize_t count = fill_stream(stream, deadline);
        if (count <= 0)
            return count;
    }
    size_t available = stream->end - stream->start;
    size_t count = length < available ? length : available;
    memcpy(data, stream->buffer + stream->start, count);
    stream->start += count;
    return (ssize_t)count;
}

/* Returns the line length including CRLF, zero for clean EOF, or -1. */
ssize_t stream_line(Stream *stream, char *line, size_t capacity, Deadline deadline)
{
    size_t used = 0;
    for (;;) {
        if (stream->start == stream->end) {
            ssize_t count = fill_stream(stream, deadline);
            if (count < 0)
                return -1;
            if (count == 0) {
                if (used == 0)
                    return 0;
                errno = EPROTO;
                return -1;
            }
        }
        const unsigned char *start = stream->buffer + stream->start;
        size_t available = stream->end - stream->start;
        const unsigned char *newline = memchr(start, '\n', available);
        size_t count = newline == NULL ? available : (size_t)(newline - start) + 1;
        if (count >= capacity - used) {
            errno = EMSGSIZE;
            return -1;
        }
        if (memchr(start, '\0', count) != NULL) {
            errno = EPROTO;
            return -1;
        }
        memcpy(line + used, start, count);
        stream->start += count;
        used += count;
        if (newline != NULL) {
            if (used < 2 || line[used - 2] != '\r') {
                errno = EPROTO;
                return -1;
            }
            line[used] = '\0';
            return (ssize_t)used;
        }
    }
}

int stream_head(Stream *stream, char *head, size_t capacity, Deadline deadline)
{
    size_t used = 0;
    for (;;) {
        ssize_t count = stream_line(stream, head + used, capacity - used, deadline);
        if (count < 0)
            return -1;
        if (count == 0) {
            if (used == 0)
                return 0;
            errno = EPROTO;
            return -1;
        }
        used += (size_t)count;
        if (count == 2)
            return 1;
    }
}
