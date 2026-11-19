#ifndef HTTP_H
#define HTTP_H

#include "net.h"

#include <stdbool.h>
#include <stddef.h>

#define HTTP_HEAD_LIMIT 16384
#define HTTP_TARGET_LIMIT 2048
#define HTTP_BODY_LIMIT (1024 * 1024)
#define HTTP_FIELD_LIMIT 100

typedef struct {
    char *name;
    char *value;
} HttpField;

typedef struct {
    HttpField fields[HTTP_FIELD_LIMIT];
    size_t count;
    size_t length;
    bool has_length;
    bool length_overflow;
    const char *transfer_encoding;
} HttpFields;

typedef struct {
    char *method;
    char *target;
    char path[HTTP_TARGET_LIMIT + 1];
    bool http11;
    bool expect_continue;
    HttpFields headers;
    unsigned char *body;
} HttpRequest;

typedef struct {
    int status;
    HttpFields headers;
} HttpResponse;

int http_parse_request(char *head, HttpRequest *request);
int http_parse_response(char *head, HttpResponse *response);
bool http_skip_field(const HttpFields *fields, const char *name);
const char *http_reason(int status);
int http_send_field(int fd, const char *name, const char *value, Deadline deadline);
int http_send_length(int fd, size_t length, Deadline deadline);
int http_send_head(int fd, int status, const char *type, size_t length, Deadline deadline);
void http_send_error(int fd, int status, const char *allow, Deadline deadline);

#endif
