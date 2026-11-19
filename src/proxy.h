#ifndef PROXY_H
#define PROXY_H

#include "http.h"

#include <netdb.h>
#include <stdatomic.h>

#define ROUTE_LIMIT 32
#define BACKEND_LIMIT 16

typedef struct {
    char *authority;
    struct addrinfo *addresses;
} Backend;

typedef struct {
    char *prefix;
    size_t prefix_length;
    Backend backends[BACKEND_LIMIT];
    size_t count;
    atomic_size_t next;
} Route;

typedef struct {
    Route routes[ROUTE_LIMIT];
    size_t count;
} Routes;

int routes_add(Routes *routes, const char *rule);
void routes_destroy(Routes *routes);
Route *routes_match(Routes *routes, const char *path);
const Backend *route_pick(Route *route);
void proxy_serve(int client, const Backend *backend, const HttpRequest *request,
                 Deadline deadline);

#endif
