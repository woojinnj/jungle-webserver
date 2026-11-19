#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <stddef.h>
#include "http.h"

int static_has_dotdot(const char *path);
int static_is_inside_root(const char *root, const char *real_path);
int static_resolve_path(const char *root, const char *req_path, char *out, size_t out_size);
int static_serve(int fd, const char *root, const http_request *req);

#endif
