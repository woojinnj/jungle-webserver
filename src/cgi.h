#ifndef CGI_H
#define CGI_H

#include <stddef.h>
#include "http.h"

#define CGI_PREFIX "/cgi-bin/"

/* CGI 프로그램 출력에서 헤더와 본문을 나눈 결과 */
typedef struct {
    char content_type[128];
    const char *body;
    size_t body_len;
} cgi_output;

int cgi_is_request(const char *path);
int cgi_resolve_program(const char *cgi_dir, const char *req_path, char *out, size_t out_size);
int cgi_split_output(const char *out, size_t len, cgi_output *result);
int cgi_serve(int fd, const char *cgi_dir, const http_request *req);

#endif
