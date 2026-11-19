#include "static_file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "mime.h"
#include "response.h"

#define INDEX_FILE "index.html"

/* 경로에 ".." 구간이 있는지 검사한다 (디렉토리 탈출 시도 차단).
 * 보안: /../../etc/passwd 같은 경로 조작(path traversal) 방지. */
int static_has_dotdot(const char *path)
{
    const char *p = path;

    while ((p = strstr(p, "..")) != NULL) {
        int starts = (p == path || p[-1] == '/');
        int ends = (p[2] == '\0' || p[2] == '/');
        if (starts && ends)
            return 1;
        p += 2;
    }
    return 0;
}

/* real_path 가 root 디렉토리 안에 있는지 검사한다 (심볼릭 링크 탈출 차단).
 * realpath 로 정규화한 두 경로의 접두사를 비교한다. */
int static_is_inside_root(const char *root, const char *real_path)
{
    size_t n = strlen(root);

    if (strncmp(root, real_path, n) != 0)
        return 0;
    return real_path[n] == '\0' || real_path[n] == '/';
}

/* realpath 실패 원인(errno)을 HTTP 상태코드로 바꾼다.
 * ENOENT → 404, EACCES → 403, 그 밖은 500. */
static int errno_to_status(int err)
{
    if (err == ENOENT || err == ENOTDIR)
        return 404;
    if (err == EACCES)
        return 403;
    return 500;
}

/* 디렉토리 요청이면 index.html 을 붙인다. 경로가 너무 길면 -1.
 * CSAPP 11.6 (parse_uri): "/" 요청은 기본 파일로 바꾼다. */
static int append_index_if_dir(char *path, size_t size)
{
    struct stat st;
    size_t len = strlen(path);

    if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
        return 0;
    if (len + 1 + strlen(INDEX_FILE) + 1 > size)
        return -1;
    snprintf(path + len, size - len, "%s%s", path[len - 1] == '/' ? "" : "/", INDEX_FILE);
    return 0;
}

/* 요청 경로를 root 안의 실제 파일 경로로 바꾼다. 0 성공, 그 외 상태코드.
 * CSAPP 11.6 (parse_uri) + 보안 검사(.., 루트 밖 접근). */
int static_resolve_path(const char *root, const char *req_path, char *out, size_t out_size)
{
    char joined[MAX_PATH_LEN];
    int n;

    if (req_path[0] != '/' || static_has_dotdot(req_path))
        return 403;
    n = snprintf(joined, sizeof(joined), "%s%s", root, req_path);
    if (n < 0 || (size_t)n >= sizeof(joined))
        return 414;
    if (append_index_if_dir(joined, sizeof(joined)) < 0)
        return 414;
    if (out_size < PATH_MAX)
        return 500;
    if (realpath(joined, out) == NULL)
        return errno_to_status(errno);
    if (!static_is_inside_root(root, out))
        return 403;
    return 0;
}

/* 열린 파일을 IO_CHUNK_SIZE 씩 읽어 소켓으로 보낸다. 성공 0.
 * CSAPP 11.6 (serve_static) — mmap 대신 고정 버퍼로 나눠 보낸다. */
static int send_file_body(int sock, int file_fd)
{
    char buf[IO_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(file_fd, buf, sizeof(buf))) != 0) {
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || send_all(sock, buf, (size_t)n) < 0)
            return -1;
    }
    return 0;
}

/* 파일을 열고 일반 파일인지 확인한다. 성공하면 fd, 실패하면 -(상태코드).
 * open 실패 원인에 따라 403 / 404 / 500. */
static int open_regular_file(const char *path, struct stat *st)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -errno_to_status(errno);
    if (fstat(fd, st) < 0) {
        close(fd);
        return -500;
    }
    if (!S_ISREG(st->st_mode)) {
        close(fd);
        return -403;
    }
    return fd;
}

/* 정적 파일 요청을 처리한다. 응답한 상태코드를 돌려준다 (-1: 전송 실패).
 * CSAPP 11.6 (serve_static): 헤더(Content-Type/Length) + 파일 내용. */
int static_serve(int fd, const char *root, const http_request *req)
{
    char path[PATH_MAX];
    struct stat st;
    int status, file_fd, rc;

    if (strcmp(req->method, "GET") != 0)
        return send_error(fd, 405, req->keep_alive) < 0 ? -1 : 405;
    status = static_resolve_path(root, req->path, path, sizeof(path));
    if (status != 0)
        return send_error(fd, status, req->keep_alive) < 0 ? -1 : status;
    file_fd = open_regular_file(path, &st);
    if (file_fd < 0)
        return send_error(fd, -file_fd, req->keep_alive) < 0 ? -1 : -file_fd;
    rc = send_response_head(fd, 200, mime_type_for_path(path), (size_t)st.st_size, req->keep_alive);
    if (rc == 0)
        rc = send_file_body(fd, file_fd);
    close(file_fd);
    return rc < 0 ? -1 : 200;
}
