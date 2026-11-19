#include "cgi.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "net.h"
#include "response.h"

#define CGI_DEFAULT_TYPE "text/plain; charset=utf-8"

/* 요청 경로가 CGI 프로그램(/cgi-bin/...) 요청인지 확인한다.
 * CSAPP 11.6: Tiny 는 "cgi-bin" 이 들어간 URI 를 동적 콘텐츠로 본다. */
int cgi_is_request(const char *path)
{
    return strncmp(path, CGI_PREFIX, strlen(CGI_PREFIX)) == 0;
}

/* /cgi-bin/이름 을 cgi_dir/이름 실행 파일 경로로 바꾼다. 0 성공, 그 외 상태코드.
 * 보안: 이름에 '/' 나 ".." 이 있으면 디렉토리 밖 실행이므로 403. */
int cgi_resolve_program(const char *cgi_dir, const char *req_path, char *out, size_t out_size)
{
    const char *name = req_path + strlen(CGI_PREFIX);
    int n;

    if (*name == '\0' || strchr(name, '/') != NULL || strcmp(name, "..") == 0 || name[0] == '.')
        return 403;
    n = snprintf(out, out_size, "%s/%s", cgi_dir, name);
    if (n < 0 || (size_t)n >= out_size)
        return 414;
    if (access(out, F_OK) < 0)
        return 404;
    if (access(out, X_OK) < 0)
        return 403;
    return 0;
}

/* "NAME=value" 문자열 하나를 env 저장소에 만든다. 너무 길면 -1.
 * CSAPP 11.5.4 (CGI): 인자는 환경변수로 자식에게 전달된다. */
static int make_env(char *slot, const char *name, const char *value)
{
    int n = snprintf(slot, MAX_ENV_LEN, "%s=%s", name, value);

    return (n < 0 || n >= MAX_ENV_LEN) ? -1 : 0;
}

/* CGI 환경변수 목록(envp)을 만든다. fork 전에 만들어 둔다 (자식은 exec 만 하도록).
 * CSAPP 11.5.4: QUERY_STRING, REQUEST_METHOD, CONTENT_LENGTH 등. */
static int build_cgi_env(const http_request *req, char store[][MAX_ENV_LEN], char *envp[])
{
    const char *type = http_get_header(req, "Content-Type");
    char len_str[32];
    int i = 0;

    snprintf(len_str, sizeof(len_str), "%zu", req->body_len);
    if (make_env(store[i++], "QUERY_STRING", req->query) < 0 ||
        make_env(store[i++], "REQUEST_METHOD", req->method) < 0 ||
        make_env(store[i++], "CONTENT_LENGTH", len_str) < 0 ||
        make_env(store[i++], "CONTENT_TYPE", type ? type : "") < 0 ||
        make_env(store[i++], "SERVER_PROTOCOL", req->version) < 0 ||
        make_env(store[i++], "GATEWAY_INTERFACE", "CGI/1.1") < 0)
        return -1;
    for (int k = 0; k < i; k++)
        envp[k] = store[k];
    envp[i] = NULL;
    return 0;
}

/* 요청 본문을 임시 파일에 써서, 자식의 표준입력으로 쓸 fd 를 돌려준다.
 * 파이프 대신 파일을 쓰면 부모·자식이 서로 기다리는 교착(deadlock)이 없다. */
static int make_stdin_file(const http_request *req)
{
    FILE *tmp = tmpfile();
    int fd;

    if (tmp == NULL)
        return -1;
    if ((req->body_len > 0 && fwrite(req->body, 1, req->body_len, tmp) != req->body_len) ||
        fflush(tmp) != 0 || (fd = dup(fileno(tmp))) < 0) {
        fclose(tmp);
        return -1;
    }
    fclose(tmp);
    if (lseek(fd, 0, SEEK_SET) < 0 || net_set_cloexec(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 자식 프로세스: 표준입력·출력을 바꾸고 CGI 프로그램 을 실행한다 (돌아오지 않는다).
 * CSAPP 11.6 (serve_dynamic): dup2 로 표준출력을 바꾼 뒤 execve. */
static void exec_cgi_child(const char *program, char *envp[], int in_fd, int out_fd)
{
    char *argv[] = { (char *)program, NULL };

    if (dup2(in_fd, STDIN_FILENO) < 0 || dup2(out_fd, STDOUT_FILENO) < 0)
        _exit(127);
    execve(program, argv, envp);
    _exit(127);
}

/* 파이프에서 EOF 까지 읽어 malloc 버퍼에 담는다. 넘치면 -1.
 * CGI 출력 크기를 MAX_CGI_OUTPUT 으로 제한한다. */
static long read_all_from_pipe(int fd, char **out)
{
    size_t cap = IO_CHUNK_SIZE, len = 0;
    char *buf = malloc(cap), *tmp;
    ssize_t n;

    if (buf == NULL)
        return -1;
    while ((n = read(fd, buf + len, cap - len)) != 0) {
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            goto fail;
        len += (size_t)n;
        if (len == cap) {
            if (cap >= MAX_CGI_OUTPUT || (tmp = realloc(buf, cap * 2)) == NULL)
                goto fail;
            buf = tmp;
            cap *= 2;
        }
    }
    *out = buf;
    return (long)len;
fail:
    free(buf);
    return -1;
}

/* 자식이 끝날 때까지 기다리고 정상 종료(0)인지 확인한다.
 * CSAPP 8.4.3 (waitpid): 자식을 회수해야 좀비 프로세스가 남지 않는다. */
static int wait_child_ok(pid_t pid)
{
    int st;

    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR)
            return 0;
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* CGI 프로그램을 실행하고 표준출력 전체를 받아온다. 출력 길이 또는 -1.
 * CSAPP 11.6 (serve_dynamic): fork → (자식) exec, (부모) 출력 수집 + wait. */
static long run_cgi(const char *program, char *envp[], int in_fd, char **out)
{
    int pipefd[2];
    pid_t pid;
    long len;

    if (pipe(pipefd) < 0)
        return -1;
    /* 양쪽 다 CLOEXEC: 다른 스레드의 fork 로 파이프가 새면 EOF 가 늦어진다 (dup2 한 0/1 번은 유지) */
    if (net_set_cloexec(pipefd[0]) < 0 || net_set_cloexec(pipefd[1]) < 0 || (pid = fork()) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0)
        exec_cgi_child(program, envp, in_fd, pipefd[1]);
    close(pipefd[1]);
    len = read_all_from_pipe(pipefd[0], out);
    close(pipefd[0]);
    if (!wait_child_ok(pid) && len >= 0) {
        free(*out);
        return -1;
    }
    return len;
}

/* CGI 출력 앞부분의 헤더(Content-Type)와 본문을 나눈다. 0 성공, -1 형식 오류.
 * CGI 규약: 프로그램은 헤더 + 빈 줄 + 본문을 출력한다. */
int cgi_split_output(const char *out, size_t len, cgi_output *result)
{
    const char *p = out, *end = out + len;

    snprintf(result->content_type, sizeof(result->content_type), "%s", CGI_DEFAULT_TYPE);
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len;
        if (nl == NULL)
            return -1;
        line_len = (size_t)(nl - p);
        if (line_len > 0 && p[line_len - 1] == '\r')
            line_len--;
        if (line_len == 0) {
            result->body = nl + 1;
            result->body_len = (size_t)(end - (nl + 1));
            return 0;
        }
        if (line_len > 13 && strncasecmp(p, "Content-Type:", 13) == 0) {
            const char *v = p + 13;
            while (*v == ' ')
                v++;
            snprintf(result->content_type, sizeof(result->content_type), "%.*s",
                     (int)(line_len - (size_t)(v - p)), v);
        }
        p = nl + 1;
    }
    return -1;
}

/* CGI 출력을 HTTP 응답으로 보낸다. 응답한 상태코드 (-1: 전송 실패).
 * 서버가 Content-Length 를 계산해 붙이므로 keep-alive 를 유지할 수 있다. */
static int send_cgi_response(int fd, const char *out, size_t len, int keep_alive)
{
    cgi_output parsed;

    if (cgi_split_output(out, len, &parsed) < 0)
        return send_error(fd, 500, keep_alive) < 0 ? -1 : 500;
    if (send_response_head(fd, 200, parsed.content_type, parsed.body_len, keep_alive) < 0 ||
        send_all(fd, parsed.body, parsed.body_len) < 0)
        return -1;
    return 200;
}

/* CGI 요청을 처리한다: 프로그램 확인 → 실행 → 응답. 응답한 상태코드 (-1: 전송 실패).
 * CSAPP 11.6 (serve_dynamic). */
int cgi_serve(int fd, const char *cgi_dir, const http_request *req)
{
    char program[MAX_PATH_LEN];
    char env_store[MAX_CGI_ENV][MAX_ENV_LEN];
    char *envp[MAX_CGI_ENV + 1];
    char *out = NULL;
    int status, in_fd;
    long len;

    status = cgi_resolve_program(cgi_dir, req->path, program, sizeof(program));
    if (status != 0)
        return send_error(fd, status, req->keep_alive) < 0 ? -1 : status;
    if (build_cgi_env(req, env_store, envp) < 0)
        return send_error(fd, 414, req->keep_alive) < 0 ? -1 : 414;
    if ((in_fd = make_stdin_file(req)) < 0)
        return send_error(fd, 500, req->keep_alive) < 0 ? -1 : 500;
    len = run_cgi(program, envp, in_fd, &out);
    close(in_fd);
    if (len < 0)
        return send_error(fd, 500, req->keep_alive) < 0 ? -1 : 500;
    status = send_cgi_response(fd, out, (size_t)len, req->keep_alive);
    free(out);
    return status;
}
