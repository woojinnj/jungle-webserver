#include "response.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#define RESPONSE_HEAD_SIZE 512   /* 상태 라인 + 헤더 버퍼 크기 */
#define ERROR_BODY_SIZE    256   /* 오류 페이지 본문 버퍼 크기 */

/* 상태코드에 맞는 이유 문구(reason phrase)를 돌려준다.
 * CSAPP 11.5.4 (HTTP Responses): 상태 라인 = 버전 + 상태코드 + 상태 메시지. */
const char *response_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* len 바이트를 모두 보낼 때까지 반복해서 쓴다. 성공 0, 실패 -1.
 * CSAPP 10.5 (rio_writen): write 는 일부만 쓰고 돌아올 수 있다. */
int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    ssize_t n;

    while (len > 0) {
        n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 상태 라인과 표준 헤더(Content-Type, Content-Length, Connection)를 보낸다.
 * CSAPP 11.5.4: 응답 = 상태 라인 + 헤더 + 빈 줄 + 본문. */
int send_response_head(int fd, int status, const char *content_type,
                       size_t content_length, int keep_alive)
{
    char head[RESPONSE_HEAD_SIZE];
    int n;

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Server: jungle-webserver\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: %s\r\n"
                 "\r\n",
                 status, response_reason(status), content_type, content_length,
                 keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= sizeof(head))
        return -1;
    return send_all(fd, head, (size_t)n);
}

/* 간단한 HTML 오류 페이지를 보낸다.
 * CSAPP 11.6 (clienterror): 오류도 정상 응답 형식으로 보낸다. */
int send_error(int fd, int status, int keep_alive)
{
    char body[ERROR_BODY_SIZE];
    int n;

    n = snprintf(body, sizeof(body),
                 "<html><body><h1>%d %s</h1></body></html>\n",
                 status, response_reason(status));
    if (n < 0 || (size_t)n >= sizeof(body))
        return -1;
    if (send_response_head(fd, status, "text/html; charset=utf-8", (size_t)n, keep_alive) < 0)
        return -1;
    return send_all(fd, body, (size_t)n);
}
