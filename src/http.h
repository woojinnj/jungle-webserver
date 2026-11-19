#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include "config.h"

/* 헤더 한 줄: name/value 는 http_request.head 안을 가리킨다. */
typedef struct {
    char *name;
    char *value;
} http_header;

/* 파싱된 요청 하나 */
typedef struct {
    char method[MAX_METHOD_LEN];
    char uri[MAX_URI_LEN];
    char version[MAX_VERSION_LEN];
    char path[MAX_URI_LEN];        /* URI 에서 ? 앞부분 */
    char query[MAX_URI_LEN];       /* URI 에서 ? 뒷부분 */
    char head[MAX_HEAD_SIZE + 1];  /* 헤더 원문 복사본 (헤더 문자열 저장소) */
    http_header headers[MAX_HEADERS];
    int header_count;
    size_t content_length;
    int keep_alive;                /* 1: 응답 후 연결 유지 */
    char *body;                    /* malloc, 없으면 NULL */
    size_t body_len;
} http_request;

/* 연결 하나의 수신 버퍼: keep-alive 에서 다음 요청 바이트가 남을 수 있다. */
typedef struct {
    int fd;
    char buf[MAX_HEAD_SIZE + 1];
    size_t len;
} http_conn;

/* 반환값: 0 = 성공, -1 = 연결 종료/타임아웃, 그 외 = 보낼 HTTP 오류 상태코드 */
int http_parse_head(const char *head, size_t len, http_request *req);
int http_parse_request_line(char *line, http_request *req);
int http_parse_header_line(char *line, http_request *req);
int http_apply_headers(http_request *req);
void http_split_uri(http_request *req);
const char *http_get_header(const http_request *req, const char *name);
long http_find_head_end(const char *buf, size_t len);

void http_conn_init(http_conn *conn, int fd);
int http_read_request(http_conn *conn, http_request *req);
void http_request_free(http_request *req);

#endif
