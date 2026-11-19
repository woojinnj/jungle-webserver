#ifndef RESPONSE_H
#define RESPONSE_H

#include <stddef.h>

const char *response_reason(int status);
int send_all(int fd, const void *buf, size_t len);
int send_response_head(int fd, int status, const char *content_type,
                       size_t content_length, int keep_alive);
int send_error(int fd, int status, int keep_alive);

#endif
