#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "static.h"
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *static_mime_type(const char *path)
{
    static const struct {
        const char *extension;
        const char *type;
    } types[] = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".txt", "text/plain; charset=utf-8"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".webp", "image/webp"},
        {".wasm", "application/wasm"},
        {".pdf", "application/pdf"}
    };
    const char *extension = strrchr(path, '.');
    if (extension != NULL) {
        for (size_t i = 0; i < sizeof types / sizeof *types; ++i) {
            if (strcasecmp(extension, types[i].extension) == 0)
                return types[i].type;
        }
    }
    return "application/octet-stream";
}

static int open_child(int parent, const char *name, int *status)
{
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0)
        return fd;
    switch (errno) {
    case EACCES:
    case EPERM:
    case ELOOP:
        *status = 403;
        break;
    case ENOENT:
    case ENOTDIR:
    case ENAMETOOLONG:
        *status = 404;
        break;
    default:
        *status = 500;
        break;
    }
    return -1;
}

static int open_path(int root, char *path, const char **name, struct stat *info, int *status)
{
    int current = dup(root);
    if (current < 0)
        return -1;
    *name = "index.html";
    char *part = path + strspn(path, "/");
    bool directory = true;
    while (*part != '\0') {
        char *next = part + strcspn(part, "/");
        bool followed_by_slash = *next == '/';
        if (followed_by_slash) {
            *next++ = '\0';
            next += strspn(next, "/");
        }
        if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
            close(current);
            *status = 403;
            return -1;
        }

        int child = open_child(current, part, status);
        close(current);
        if (child < 0)
            return -1;
        current = child;
        *name = part;
        if (fstat(current, info) < 0) {
            close(current);
            *status = 500;
            return -1;
        }
        directory = S_ISDIR(info->st_mode);
        if (followed_by_slash && !directory) {
            close(current);
            *status = 404;
            return -1;
        }
        part = next;
    }
    if (directory) {
        *name = "index.html";
        int index = open_child(current, *name, status);
        close(current);
        if (index < 0)
            return -1;
        current = index;
        if (fstat(current, info) < 0) {
            close(current);
            *status = 500;
            return -1;
        }
    }
    if (!S_ISREG(info->st_mode)) {
        close(current);
        *status = 403;
        return -1;
    }
    return current;
}

void static_serve(int client, int root, const char *path, Deadline deadline)
{
    char copy[HTTP_TARGET_LIMIT + 1];
    strcpy(copy, path);
    const char *name;
    struct stat info;
    int status = 500;
    int fd = open_path(root, copy, &name, &info, &status);
    if (fd < 0) {
        http_send_error(client, status, NULL, deadline);
        return;
    }
    if (info.st_size < 0 || (uintmax_t)info.st_size > SIZE_MAX) {
        http_send_error(client, 500, NULL, deadline);
        close(fd);
        return;
    }

    size_t remaining = (size_t)info.st_size;
    if (http_send_head(client, 200, static_mime_type(name), remaining, deadline) == 0) {
        unsigned char buffer[16384];
        while (remaining > 0) {
            size_t length = remaining < sizeof buffer ? remaining : sizeof buffer;
            ssize_t count = read(fd, buffer, length);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0 || send_all(client, buffer, (size_t)count, deadline) < 0)
                break;
            remaining -= (size_t)count;
        }
    }
    close(fd);
}
