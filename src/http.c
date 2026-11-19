#include "http.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool digit(unsigned char byte)
{
    return byte >= '0' && byte <= '9';
}

static bool alpha(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

static bool token_char(unsigned char byte)
{
    return alpha(byte) || digit(byte) ||
           (byte != 0 && strchr("!#$%&'*+-.^`_|~", byte) != NULL);
}

static bool field_text(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p != 0; ++p) {
        if ((*p < 32 && *p != '\t') || *p == 127)
            return false;
    }
    return true;
}

static char *end_line(char *line)
{
    char *end = strstr(line, "\r\n");
    if (end == NULL)
        return NULL;
    *end = '\0';
    return end + 2;
}

static char *trim_ows(char *value)
{
    value += strspn(value, " \t");
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return value;
}

static bool connection_options(const char *p, const char *name, bool *found)
{
    for (;;) {
        p += strspn(p, " \t");
        const char *start = p;
        while (token_char((unsigned char)*p))
            ++p;
        size_t length = (size_t)(p - start);
        if (length == 0)
            return false;
        if (name != NULL && strlen(name) == length &&
            strncasecmp(name, start, length) == 0)
            *found = true;
        p += strspn(p, " \t");
        if (*p == '\0')
            return true;
        if (*p++ != ',')
            return false;
    }
}

static bool parse_length(const char *value, HttpFields *fields)
{
    size_t result = 0;
    if (*value == '\0')
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p != 0; ++p) {
        if (!digit(*p))
            return false;
        size_t number = *p - '0';
        if (result > (SIZE_MAX - number) / 10) {
            fields->length_overflow = true;
        } else if (!fields->length_overflow) {
            result = result * 10 + number;
        }
    }
    fields->length = result;
    return true;
}

static int parse_fields(char *line, HttpFields *fields)
{
    for (;;) {
        char *next = end_line(line);
        if (next == NULL)
            return 400;
        if (*line == '\0')
            break;
        if (fields->count == HTTP_FIELD_LIMIT)
            return 431;
        char *colon = line;
        while (token_char((unsigned char)*colon))
            ++colon;
        if (colon == line || *colon != ':')
            return 400;
        *colon = '\0';
        char *value = trim_ows(colon + 1);
        if (!field_text(value))
            return 400;
        fields->fields[fields->count++] = (HttpField){line, value};
        if (strcasecmp(line, "Content-Length") == 0) {
            if (fields->has_length || !parse_length(value, fields))
                return 400;
            fields->has_length = true;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            if (fields->transfer_encoding != NULL || *value == '\0')
                return 400;
            fields->transfer_encoding = value;
        } else if (strcasecmp(line, "Connection") == 0) {
            if (!connection_options(value, NULL, NULL))
                return 400;
        }
        line = next;
    }
    return fields->has_length && fields->transfer_encoding != NULL ? 400 : 0;
}

static int hex(unsigned char byte)
{
    if (digit(byte))
        return byte - '0';
    if (byte >= 'a' && byte <= 'f')
        return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F')
        return byte - 'A' + 10;
    return -1;
}

static int decode_path(const char *target, char *path)
{
    if (strlen(target) > HTTP_TARGET_LIMIT)
        return 414;
    if (*target != '/')
        return 400;
    bool query = false;
    char *start = path;
    for (const unsigned char *p = (const unsigned char *)target; *p != 0; ++p) {
        int byte = *p;
        if (byte == '?') {
            query = true;
            continue;
        }
        if (byte == '%') {
            int high = hex(*++p);
            if (high < 0)
                return 400;
            int low = hex(*++p);
            if (low < 0)
                return 400;
            byte = high * 16 + low;
            if (!query && (byte < 32 || byte == 127 || byte == '/' || byte == '\\'))
                return 400;
        } else if (!alpha(*p) && !digit(*p) && strchr("-._~!$&'()*+,;=:@/", byte) == NULL) {
            return 400;
        }
        if (!query)
            *path++ = (char)byte;
    }
    *path = '\0';
    for (const char *part = start + strspn(start, "/"); *part != '\0';) {
        size_t length = strcspn(part, "/");
        if ((length == 1 && part[0] == '.') ||
            (length == 2 && part[0] == '.' && part[1] == '.'))
            return 403;
        part += length;
        part += strspn(part, "/");
    }
    return 0;
}

static bool valid_host(const char *host)
{
    if (*host == '\0')
        return false;
    const char *port;
    if (*host == '[') {
        const char *end = strchr(++host, ']');
        char address[INET6_ADDRSTRLEN];
        if (end == NULL || (size_t)(end - host) >= sizeof address)
            return false;
        size_t length = (size_t)(end - host);
        memcpy(address, host, length);
        address[length] = '\0';
        struct in6_addr parsed;
        if (inet_pton(AF_INET6, address, &parsed) != 1)
            return false;
        port = end + 1;
    } else {
        port = host;
        while (*port != '\0' && *port != ':') {
            unsigned char byte = (unsigned char)*port++;
            if (byte == '%') {
                if (hex((unsigned char)port[0]) < 0 || hex((unsigned char)port[1]) < 0)
                    return false;
                port += 2;
            } else if (!alpha(byte) && !digit(byte) && strchr("-._~!$&'()*+;=", byte) == NULL) {
                return false;
            }
        }
        if (port == host)
            return false;
    }
    if (*port == '\0')
        return true;
    return *port == ':' && port[1] != '\0' && port[strspn(port + 1, "0123456789") + 1] == '\0';
}

int http_parse_request(char *head, HttpRequest *request)
{
    *request = (HttpRequest){0};
    char *fields = end_line(head);
    char *target = strchr(head, ' ');
    if (fields == NULL || target == NULL || target == head)
        return 400;
    *target++ = '\0';
    for (const unsigned char *p = (const unsigned char *)head; *p != 0; ++p) {
        if (!token_char(*p))
            return 400;
    }
    char *version = strchr(target, ' ');
    if (version == NULL || version == target)
        return 400;
    *version++ = '\0';
    if (strlen(version) != 8 || strncmp(version, "HTTP/", 5) != 0 ||
        !digit((unsigned char)version[5]) || version[6] != '.' ||
        !digit((unsigned char)version[7]))
        return 400;
    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0)
        return 505;
    request->http11 = version[7] == '1';
    request->method = head;
    request->target = target;
    int error = decode_path(target, request->path);
    if (error != 0)
        return error;
    error = parse_fields(fields, &request->headers);
    if (error != 0)
        return error;

    bool host_seen = false;
    for (size_t i = 0; i < request->headers.count; ++i) {
        HttpField field = request->headers.fields[i];
        if (strcasecmp(field.name, "Host") == 0) {
            if (host_seen || !valid_host(field.value))
                return 400;
            host_seen = true;
        } else if (strcasecmp(field.name, "Expect") == 0) {
            if (!request->http11 || request->expect_continue ||
                strcasecmp(field.value, "100-continue") != 0)
                return 417;
            request->expect_continue = true;
        }
    }
    if (request->http11 && !host_seen)
        return 400;
    if (request->headers.transfer_encoding != NULL)
        return 501;
    if (request->headers.length_overflow || request->headers.length > HTTP_BODY_LIMIT)
        return 413;
    if (strcmp(head, "GET") != 0 && strcmp(head, "POST") != 0)
        return 405;
    return 0;
}

int http_parse_response(char *head, HttpResponse *response)
{
    *response = (HttpResponse){0};
    char *fields = end_line(head);
    if (fields == NULL || strlen(head) < 13 ||
        (strncmp(head, "HTTP/1.1 ", 9) != 0 && strncmp(head, "HTTP/1.0 ", 9) != 0) ||
        !digit((unsigned char)head[9]) || !digit((unsigned char)head[10]) ||
        !digit((unsigned char)head[11]) || head[12] != ' ' || !field_text(head + 13))
        return 400;
    response->status = (head[9] - '0') * 100 + (head[10] - '0') * 10 + head[11] - '0';
    if (response->status < 100 || response->status > 599)
        return 400;
    int error = parse_fields(fields, &response->headers);
    return response->headers.length_overflow ? 400 : error;
}

bool http_skip_field(const HttpFields *fields, const char *name)
{
    static const char *const skip[] = {
        "Connection", "Keep-Alive", "Proxy-Connection", "Transfer-Encoding", "TE",
        "Trailer", "Upgrade", "Proxy-Authenticate", "Proxy-Authorization", "Content-Length"
    };
    for (size_t i = 0; i < sizeof skip / sizeof *skip; ++i) {
        if (strcasecmp(name, skip[i]) == 0)
            return true;
    }
    bool found = false;
    for (size_t i = 0; i < fields->count && !found; ++i) {
        if (strcasecmp(fields->fields[i].name, "Connection") == 0)
            connection_options(fields->fields[i].value, name, &found);
    }
    return found;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "";
    }
}

int http_send_field(int fd, const char *name, const char *value, Deadline deadline)
{
    if (send_all(fd, name, strlen(name), deadline) < 0 ||
        send_all(fd, ": ", 2, deadline) < 0 ||
        send_all(fd, value, strlen(value), deadline) < 0)
        return -1;
    return send_all(fd, "\r\n", 2, deadline);
}

int http_send_length(int fd, size_t length, Deadline deadline)
{
    char value[3 * sizeof length + 1];
    snprintf(value, sizeof value, "%zu", length);
    return http_send_field(fd, "Content-Length", value, deadline);
}

static int send_status(int fd, int status, Deadline deadline)
{
    char line[96];
    int length = snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, http_reason(status));
    return send_all(fd, line, (size_t)length, deadline);
}

static int send_head_fields(int fd, int status, const char *type, size_t length,
                            Deadline deadline)
{
    if (send_status(fd, status, deadline) < 0 ||
        http_send_field(fd, "Content-Type", type, deadline) < 0 ||
        http_send_length(fd, length, deadline) < 0)
        return -1;
    return http_send_field(fd, "Connection", "close", deadline);
}

int http_send_head(int fd, int status, const char *type, size_t length, Deadline deadline)
{
    if (send_head_fields(fd, status, type, length, deadline) < 0)
        return -1;
    return send_all(fd, "\r\n", 2, deadline);
}

void http_send_error(int fd, int status, const char *allow, Deadline deadline)
{
    char body[96];
    int length = snprintf(body, sizeof body, "%d %s\n", status, http_reason(status));
    if (send_head_fields(fd, status, "text/plain; charset=utf-8", (size_t)length, deadline) < 0)
        return;
    if (allow != NULL && http_send_field(fd, "Allow", allow, deadline) < 0)
        return;
    if (send_all(fd, "\r\n", 2, deadline) == 0)
        send_all(fd, body, (size_t)length, deadline);
}
