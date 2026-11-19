#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef int64_t Deadline;

typedef struct {
    int fd;
    size_t start;
    size_t end;
    unsigned char buffer[8192];
} Stream;

Deadline deadline_after(int milliseconds);
int set_nonblocking(int fd);
int await_socket(int fd, short events, Deadline deadline);
int send_all(int fd, const void *data, size_t length, Deadline deadline);
ssize_t stream_read(Stream *stream, void *data, size_t length, Deadline deadline);
ssize_t stream_line(Stream *stream, char *line, size_t capacity, Deadline deadline);
int stream_head(Stream *stream, char *head, size_t capacity, Deadline deadline);

#endif
