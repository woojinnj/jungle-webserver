#ifndef ROUTER_H
#define ROUTER_H

#include <stdatomic.h>
#include <stddef.h>

#define ROUTER_MAX_BACKENDS 16

typedef struct {
    char host[256];
    char port[8];
} backend;

typedef struct {
    char *prefix;
    size_t plen;
    backend be[ROUTER_MAX_BACKENDS];
    int nbe;
    atomic_uint rr; /* round-robin cursor */
} route;

typedef struct {
    route **routes;
    int n;
} router;

/* Adds "prefix -> backends". spec is "host:port" items separated by spaces or
 * commas ("[::1]:80" and "http://host:port" accepted). 0 ok, -1 + err text. */
int router_add(router *rt, const char *prefix, const char *spec, char *err, size_t errcap);

/* Longest matching prefix wins. "/api" matches "/api" and "/api/x" but not
 * "/apix"; a prefix ending in '/' is a plain startswith. */
route *router_match(const router *rt, const char *path, size_t plen);

/* Next round-robin start index (monotonic; caller takes it modulo nbe). */
unsigned route_pick(route *r);

void router_free(router *rt);

#endif
