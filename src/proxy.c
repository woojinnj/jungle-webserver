#include "proxy.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONNECT_MS 2000
#define UPSTREAM_MS 5000

static void route_destroy(Route *route)
{
    for (size_t i = 0; i < route->count; ++i) {
        free(route->backends[i].authority);
        freeaddrinfo(route->backends[i].addresses);
    }
    free(route->prefix);
    route->prefix = NULL;
    route->count = 0;
}

void routes_destroy(Routes *routes)
{
    for (size_t i = 0; i < routes->count; ++i)
        route_destroy(&routes->routes[i]);
    routes->count = 0;
}

static int add_backend(Route *route, char *authority)
{
    if (route->count == BACKEND_LIMIT)
        return -1;
    char *port = strrchr(authority, ':');
    if (port == NULL || port == authority || port[1] == '\0')
        return -1;
    if (strspn(port + 1, "0123456789") != strlen(port + 1) ||
        strtoul(port + 1, NULL, 10) == 0 || strtoul(port + 1, NULL, 10) > 65535)
        return -1;

    char *copy = strdup(authority);
    if (copy == NULL)
        return -1;
    *port++ = '\0';
    const char *host = authority;
    size_t length = strlen(host);
    if (*host == '[' && length > 2 && host[length - 1] == ']') {
        authority[length - 1] = '\0';
        host++;
    } else if (strchr(host, ':') != NULL) {
        free(copy);
        return -1;
    }
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_NUMERICSERV
    };
    struct addrinfo *addresses;
    if (strpbrk(host, " \t\r\n") != NULL ||
        getaddrinfo(host, port, &hints, &addresses) != 0) {
        free(copy);
        return -1;
    }
    route->backends[route->count++] = (Backend){copy, addresses};
    return 0;
}

int routes_add(Routes *routes, const char *rule)
{
    if (routes->count == ROUTE_LIMIT)
        return -1;
    const char *equals = strchr(rule, '=');
    if (equals == NULL || *rule != '/')
        return -1;
    size_t length = (size_t)(equals - rule);
    if (length > HTTP_TARGET_LIMIT ||
        strspn(rule, "/-._~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") != length)
        return -1;
    while (length > 1 && rule[length - 1] == '/')
        --length;
    for (size_t i = 0; i < routes->count; ++i) {
        if (routes->routes[i].prefix_length == length &&
            strncmp(routes->routes[i].prefix, rule, length) == 0)
            return -1;
    }

    Route *route = &routes->routes[routes->count];
    route->prefix = strndup(rule, length);
    route->prefix_length = length;
    char *list = strdup(equals + 1);
    if (route->prefix == NULL || list == NULL) {
        free(list);
        route_destroy(route);
        return -1;
    }
    char *backend = list;
    for (;;) {
        char *comma = strchr(backend, ',');
        if (comma != NULL)
            *comma = '\0';
        if (add_backend(route, backend) < 0) {
            free(list);
            route_destroy(route);
            return -1;
        }
        if (comma == NULL)
            break;
        backend = comma + 1;
    }

    free(list);
    atomic_init(&route->next, 0);
    ++routes->count;
    return 0;
}

Route *routes_match(Routes *routes, const char *path)
{
    Route *best = NULL;
    for (size_t i = 0; i < routes->count; ++i) {
        Route *route = &routes->routes[i];
        size_t length = route->prefix_length;
        if ((best == NULL || length > best->prefix_length) &&
            strncmp(path, route->prefix, length) == 0 &&
            (length == 1 || path[length] == '/' || path[length] == '\0'))
            best = route;
    }
    return best;
}

const Backend *route_pick(Route *route)
{
    size_t index = atomic_fetch_add_explicit(&route->next, 1, memory_order_relaxed);
    return &route->backends[index % route->count];
}

static int connect_backend(const Backend *backend, Deadline deadline)
{
    int failure = ECONNREFUSED;
    for (const struct addrinfo *addr = backend->addresses; addr != NULL; addr = addr->ai_next) {
        int fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0) {
            failure = errno;
            continue;
        }
        if (set_nonblocking(fd) < 0) {
            failure = errno;
            close(fd);
            continue;
        }
        if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0)
            return fd;
        int error = errno;
        if (error == EINPROGRESS || error == EINTR) {
            socklen_t size = sizeof error;
            if (await_socket(fd, POLLOUT, deadline) < 0 ||
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
                error = errno;
            if (error == 0)
                return fd;
        }
        close(fd);
        failure = error;
        if (error == ETIMEDOUT)
            break;
    }
    errno = failure;
    return -1;
}

static int forward_fields(int fd, const HttpFields *fields, bool request, Deadline deadline)
{
    for (size_t i = 0; i < fields->count; ++i) {
        HttpField field = fields->fields[i];
        if (http_skip_field(fields, field.name) ||
            (request && (strcasecmp(field.name, "Host") == 0 ||
                         strcasecmp(field.name, "Expect") == 0)))
            continue;
        if (http_send_field(fd, field.name, field.value, deadline) < 0)
            return -1;
    }
    return 0;
}

static int forward_request(int fd, const Backend *backend, const HttpRequest *request,
                           Deadline deadline)
{
    char line[HTTP_TARGET_LIMIT + 32];
    int length = snprintf(line, sizeof line, "%s %s HTTP/1.1\r\n",
                          request->method, request->target);
    if (send_all(fd, line, (size_t)length, deadline) < 0 ||
        http_send_field(fd, "Host", backend->authority, deadline) < 0 ||
        http_send_field(fd, "Connection", "close", deadline) < 0 ||
        http_send_field(fd, "Via", request->http11 ? "1.1 jungle-webserver" : "1.0 jungle-webserver",
                        deadline) < 0 ||
        forward_fields(fd, &request->headers, true, deadline) < 0)
        return -1;
    if ((request->headers.has_length || strcmp(request->method, "POST") == 0) &&
        http_send_length(fd, request->headers.length, deadline) < 0)
        return -1;
    if (send_all(fd, "\r\n", 2, deadline) < 0)
        return -1;
    return send_all(fd, request->body, request->headers.length, deadline);
}

static int relay_body(int client, Stream *upstream, bool fixed, size_t remaining,
                      Deadline read_by, Deadline write_by)
{
    unsigned char buffer[8192];
    while (!fixed || remaining > 0) {
        size_t size = fixed && remaining < sizeof buffer ? remaining : sizeof buffer;
        ssize_t count = stream_read(upstream, buffer, size, read_by);
        if (count == 0)
            return fixed ? -1 : 0;
        if (count < 0 || send_all(client, buffer, (size_t)count, write_by) < 0)
            return -1;
        if (fixed)
            remaining -= (size_t)count;
    }
    return 0;
}

static bool chunk_length(const char *line, size_t *length)
{
    static const char digits[] = "0123456789abcdef";
    static const char upper[] = "ABCDEF";
    size_t size = 0;
    const char *p = line;
    for (; *p != '\0'; ++p) {
        const char *digit = strchr(digits, *p);
        const char *uppercase = strchr(upper, *p);
        if (digit == NULL && uppercase == NULL)
            break;
        size_t value = digit != NULL ? (size_t)(digit - digits) :
                                       (size_t)(uppercase - upper) + 10;
        if (size > (SIZE_MAX - value) / 16)
            return false;
        size = size * 16 + value;
    }
    if (p == line)
        return false;
    p += strspn(p, " \t");
    if (*p != '\0' && *p != ';')
        return false;
    *length = size;
    return true;
}

static int relay_chunks(int client, Stream *upstream, bool use_chunks,
                        Deadline read_by, Deadline write_by)
{
    char line[1024];
    for (;;) {
        ssize_t count = stream_line(upstream, line, sizeof line, read_by);
        if (count < 2)
            return -1;
        line[count - 2] = '\0';
        size_t length;
        if (!chunk_length(line, &length))
            return -1;
        if (length == 0) {
            char trailers[HTTP_HEAD_LIMIT + 1];
            if (stream_head(upstream, trailers, sizeof trailers, read_by) != 1)
                return -1;
            return use_chunks ? send_all(client, "0\r\n\r\n", 5, write_by) : 0;
        }

        if (use_chunks) {
            int size = snprintf(line, sizeof line, "%zx\r\n", length);
            if (send_all(client, line, (size_t)size, write_by) < 0)
                return -1;
        }
        if (relay_body(client, upstream, true, length, read_by, write_by) < 0 ||
            stream_line(upstream, line, 3, read_by) != 2)
            return -1;
        if (use_chunks && send_all(client, "\r\n", 2, write_by) < 0)
            return -1;
    }
}

/* A gateway status is returned only while no downstream response has started. */
static int forward_response(int client, int upstream_fd, bool http11,
                            Deadline read_by, Deadline write_by)
{
    Stream upstream = {.fd = upstream_fd};
    char head[HTTP_HEAD_LIMIT + 1];
    HttpResponse response;
    for (int i = 0;; ++i) {
        int result = stream_head(&upstream, head, sizeof head, read_by);
        if (result != 1)
            return result < 0 && errno == ETIMEDOUT ? 504 : 502;
        if (http_parse_response(head, &response) != 0 || response.status == 101 || i == 16)
            return 502;
        if (response.status >= 200)
            break;
    }
    HttpFields *fields = &response.headers;
    bool has_transfer = fields->transfer_encoding != NULL;
    if (has_transfer && strcasecmp(fields->transfer_encoding, "chunked") != 0)
        return 502;
    bool has_body = response.status != 204 && response.status != 205 && response.status != 304;
    bool use_chunks = http11 && has_transfer && has_body;

    char line[HTTP_HEAD_LIMIT + 1];
    int length = snprintf(line, sizeof line, "HTTP/1.%d %d %s\r\n",
                          http11 ? 1 : 0, response.status, head + 13);
    if (length < 0 || (size_t)length >= sizeof line)
        return 502;
    if (send_all(client, line, (size_t)length, write_by) < 0 ||
        forward_fields(client, fields, false, write_by) < 0 ||
        http_send_field(client, "Connection", "close", write_by) < 0)
        return 0;
    if (use_chunks && http_send_field(client, "Transfer-Encoding", "chunked", write_by) < 0)
        return 0;
    if (((fields->has_length && (has_body || response.status == 304)) || response.status == 205) &&
        http_send_length(client, has_body || response.status == 304 ? fields->length : 0,
                         write_by) < 0)
        return 0;
    if (send_all(client, "\r\n", 2, write_by) < 0 || !has_body)
        return 0;
    if (has_transfer)
        relay_chunks(client, &upstream, use_chunks, read_by, write_by);
    else
        relay_body(client, &upstream, fields->has_length, fields->length, read_by, write_by);
    return 0;
}

void proxy_serve(int client, const Backend *backend, const HttpRequest *request,
                 Deadline deadline)
{
    int fd = connect_backend(backend, deadline_after(CONNECT_MS));
    int status;
    if (fd < 0) {
        status = errno == ETIMEDOUT ? 504 : 502;
    } else {
        Deadline read_by = deadline_after(UPSTREAM_MS);
        if (forward_request(fd, backend, request, read_by) < 0)
            status = errno == ETIMEDOUT ? 504 : 502;
        else
            status = forward_response(client, fd, request->http11, read_by, deadline);
        close(fd);
    }
    if (status != 0)
        http_send_error(client, status, NULL, deadline);
}
