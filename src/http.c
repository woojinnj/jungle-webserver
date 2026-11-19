#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>

/* 요청 헤더의 끝(빈 줄 "\r\n\r\n")을 찾아 헤더 전체 길이를 돌려준다. 없으면 -1.
 * CSAPP 11.5.3: 요청은 요청 라인 + 헤더 + 빈 줄로 끝난다 (본문과의 경계). */
long http_find_head_end(const char *buf, size_t len)
{
    size_t i;

    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return (long)(i + 4);
    }
    return -1;
}

/* 메서드 문자열이 대문자 토큰인지 검사한다.
 * CSAPP 11.5.3: 메서드는 GET, POST 같은 토큰이다. */
static int is_method_token(const char *s)
{
    if (*s == '\0')
        return 0;
    for (; *s; s++) {
        if (!isupper((unsigned char)*s))
            return 0;
    }
    return 1;
}

/* 버전 문자열을 검사해 0(지원), 400(형식 오류), 505(미지원 버전)를 돌려준다.
 * CSAPP 11.5.3: 버전은 HTTP/1.0 또는 HTTP/1.1 이다. */
static int check_version(const char *version)
{
    if (strcmp(version, "HTTP/1.1") == 0 || strcmp(version, "HTTP/1.0") == 0)
        return 0;
    if (strncmp(version, "HTTP/", 5) == 0)
        return 505;
    return 400;
}

/* 토큰 하나를 dst 에 복사한다. 길이를 넘으면 -1.
 * 규칙 6: 버퍼 크기를 넘지 않게 복사한다. */
static int copy_token(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);

    if (n >= dst_size)
        return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

/* 요청 라인 "METHOD URI VERSION" 을 파싱한다.
 * CSAPP 11.5.3 (HTTP Requests): 요청 라인의 세 부분. */
int http_parse_request_line(char *line, http_request *req)
{
    char *method, *uri, *version, *extra, *save = NULL;
    int status;

    method = strtok_r(line, " ", &save);
    uri = strtok_r(NULL, " ", &save);
    version = strtok_r(NULL, " ", &save);
    extra = strtok_r(NULL, " ", &save);
    if (method == NULL || uri == NULL || version == NULL || extra != NULL)
        return 400;
    if (!is_method_token(method) || copy_token(req->method, sizeof(req->method), method) < 0)
        return 400;
    if (copy_token(req->uri, sizeof(req->uri), uri) < 0)
        return 414;
    if (copy_token(req->version, sizeof(req->version), version) < 0)
        return 400;
    if ((status = check_version(req->version)) != 0)
        return status;
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "POST") != 0)
        return 501;
    if (req->uri[0] != '/' && strncmp(req->uri, "http://", 7) != 0)
        return 400;
    return 0;
}

/* 헤더 한 줄 "Name: value" 를 파싱해 req->headers 에 추가한다.
 * CSAPP 11.5.3: 요청 헤더는 header-name: header-data 형식. */
int http_parse_header_line(char *line, http_request *req)
{
    char *colon = strchr(line, ':');
    char *value, *end;

    if (colon == NULL || colon == line)
        return 400;
    for (char *p = line; p < colon; p++) {
        if (isspace((unsigned char)*p))
            return 400;
    }
    if (req->header_count >= MAX_HEADERS)
        return 431;
    *colon = '\0';
    value = colon + 1;
    while (*value == ' ' || *value == '\t')
        value++;
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    req->headers[req->header_count].name = line;
    req->headers[req->header_count].value = value;
    req->header_count++;
    return 0;
}

/* 이름으로 헤더 값을 찾는다 (대소문자 무시). 없으면 NULL.
 * RFC 7230 3.2: 헤더 이름은 대소문자를 구분하지 않는다. */
const char *http_get_header(const http_request *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

/* Content-Length 값을 숫자로 바꾼다. 형식 오류 400, 너무 크면 413.
 * RFC 7230 3.3.2: 본문 길이는 Content-Length 로 알 수 있다. */
static int parse_content_length(const char *value, size_t *out)
{
    size_t n = 0;

    if (*value == '\0')
        return 400;
    for (; *value; value++) {
        if (!isdigit((unsigned char)*value))
            return 400;
        n = n * 10 + (size_t)(*value - '0');
        if (n > MAX_BODY_SIZE)
            return 413;
    }
    *out = n;
    return 0;
}

/* Connection 헤더와 버전으로 keep-alive 여부를 정한다.
 * RFC 7230 6.3: HTTP/1.1 은 기본 유지, HTTP/1.0 은 기본 종료. */
static int decide_keep_alive(const http_request *req)
{
    const char *conn = http_get_header(req, "Connection");
    int keep = (strcmp(req->version, "HTTP/1.1") == 0);

    if (conn != NULL && strcasecmp(conn, "close") == 0)
        keep = 0;
    else if (conn != NULL && strcasecmp(conn, "keep-alive") == 0)
        keep = 1;
    return keep;
}

/* 파싱된 헤더에서 Host, Content-Length, Connection 등을 해석한다.
 * RFC 7230 5.4: HTTP/1.1 요청에는 Host 헤더가 꼭 있어야 한다. */
int http_apply_headers(http_request *req)
{
    const char *cl = http_get_header(req, "Content-Length");
    int status;

    if (strcmp(req->version, "HTTP/1.1") == 0 && http_get_header(req, "Host") == NULL)
        return 400;
    if (http_get_header(req, "Transfer-Encoding") != NULL)
        return 501;
    req->content_length = 0;
    if (cl != NULL && (status = parse_content_length(cl, &req->content_length)) != 0)
        return status;
    req->keep_alive = decide_keep_alive(req);
    return 0;
}

/* URI 를 경로(path)와 쿼리(query)로 나눈다.
 * CSAPP 11.6 (parse_uri): ? 뒤는 CGI 인자. */
void http_split_uri(http_request *req)
{
    char *q = strchr(req->uri, '?');
    size_t path_len = q ? (size_t)(q - req->uri) : strlen(req->uri);

    memcpy(req->path, req->uri, path_len);
    req->path[path_len] = '\0';
    if (q != NULL)
        strcpy(req->query, q + 1);     /* uri 와 같은 크기라 넘치지 않는다 */
    else
        req->query[0] = '\0';
}

/* 헤더 원문(요청 라인 + 헤더 + 빈 줄)을 파싱해 req 를 채운다.
 * CSAPP 11.5.3: 줄 단위(\r\n)로 요청 라인과 헤더를 나눈다. */
int http_parse_head(const char *head, size_t len, http_request *req)
{
    char *line, *next;
    int status;

    if (len > MAX_HEAD_SIZE)
        return 431;
    memcpy(req->head, head, len);
    req->head[len] = '\0';
    req->header_count = 0;
    line = req->head;
    next = strstr(line, "\r\n");
    if (next == NULL)
        return 400;
    *next = '\0';
    if ((status = http_parse_request_line(line, req)) != 0)
        return status;
    for (line = next + 2; (next = strstr(line, "\r\n")) != NULL && next != line; line = next + 2) {
        *next = '\0';
        if ((status = http_parse_header_line(line, req)) != 0)
            return status;
    }
    http_split_uri(req);
    return http_apply_headers(req);
}

/* 연결 수신 버퍼를 초기화한다.
 * CSAPP 10.5 (RIO): 소켓 읽기는 내부 버퍼를 두고 나눠 읽는다. */
void http_conn_init(http_conn *conn, int fd)
{
    conn->fd = fd;
    conn->len = 0;
}

/* 소켓에서 읽어 conn->buf 뒤에 붙인다. 읽은 바이트 수, 0(끊김), -1(오류/타임아웃).
 * CSAPP 10.4: read 는 요청보다 적게 읽을 수 있다 (short count). */
static long recv_into_conn(http_conn *conn)
{
    ssize_t n;

    do {
        n = recv(conn->fd, conn->buf + conn->len, MAX_HEAD_SIZE - conn->len, 0);
    } while (n < 0 && errno == EINTR);
    if (n > 0)
        conn->len += (size_t)n;
    return (long)n;
}

/* 버퍼가 꽉 찼는데 헤더 끝이 없을 때 보낼 오류 코드를 고른다.
 * 첫 줄도 끝나지 않았으면 URI 가 너무 긴 것(414), 아니면 헤더가 너무 큰 것(431). */
static int head_overflow_status(const http_conn *conn)
{
    for (size_t i = 0; i + 1 < conn->len; i++) {
        if (conn->buf[i] == '\r' && conn->buf[i + 1] == '\n')
            return 431;
    }
    return 414;
}

/* 헤더 끝을 찾을 때까지 읽는다. 헤더 길이, -1(끊김) 또는 -(오류코드) 반환.
 * CSAPP 11.5.3: 빈 줄이 나와야 헤더가 끝난다. */
static long read_until_head_end(http_conn *conn)
{
    long head_len;

    while ((head_len = http_find_head_end(conn->buf, conn->len)) < 0) {
        if (conn->len >= MAX_HEAD_SIZE)
            return -head_overflow_status(conn);
        if (recv_into_conn(conn) <= 0)
            return -1;
    }
    return head_len;
}

/* conn->buf 앞에서 n 바이트를 버리고 남은 바이트를 앞으로 당긴다.
 * keep-alive: 다음 요청의 앞부분이 버퍼에 남아 있을 수 있다. */
static void consume_conn(http_conn *conn, size_t n)
{
    memmove(conn->buf, conn->buf + n, conn->len - n);
    conn->len -= n;
}

/* Content-Length 만큼 본문을 읽어 req->body 에 담는다.
 * RFC 7230 3.3.3: 본문 길이는 Content-Length 로 정해진다. */
static int read_body(http_conn *conn, http_request *req)
{
    size_t have, need = req->content_length;
    ssize_t n;

    req->body = malloc(need + 1);
    if (req->body == NULL)
        return 500;
    have = conn->len < need ? conn->len : need;
    memcpy(req->body, conn->buf, have);
    consume_conn(conn, have);
    while (have < need) {
        n = recv(conn->fd, req->body + have, need - have, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        have += (size_t)n;
    }
    req->body[need] = '\0';
    req->body_len = need;
    return 0;
}

/* 연결에서 요청 하나(헤더 + 본문)를 읽어 파싱한다.
 * 반환: 0 성공, -1 연결 종료/타임아웃, 그 외 HTTP 오류 상태코드. */
int http_read_request(http_conn *conn, http_request *req)
{
    long head_len;
    int status;

    req->body = NULL;
    req->body_len = 0;
    req->keep_alive = 0;
    head_len = read_until_head_end(conn);
    if (head_len == -1)
        return -1;
    if (head_len < 0)
        return (int)-head_len;
    status = http_parse_head(conn->buf, (size_t)head_len, req);
    consume_conn(conn, (size_t)head_len);
    if (status != 0)
        return status;
    if (req->content_length > 0)
        return read_body(conn, req);
    return 0;
}

/* 요청이 잡은 메모리(본문)를 해제한다.
 * malloc / free 짝 맞추기. */
void http_request_free(http_request *req)
{
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}
