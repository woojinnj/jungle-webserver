#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_backend(char *tok, backend *b, char *err, size_t ec) {
    if (strncmp(tok, "http://", 7) == 0) tok += 7;
    size_t l = strlen(tok);
    while (l > 0 && tok[l - 1] == '/') tok[--l] = '\0';

    const char *host = tok, *port = "80";
    char *colon;
    if (tok[0] == '[') { /* [v6]:port */
        char *rb = strchr(tok, ']');
        if (!rb) goto bad;
        *rb = '\0';
        host = tok + 1;
        if (rb[1] == ':') port = rb + 2;
        else if (rb[1] != '\0') goto bad;
    } else if ((colon = strrchr(tok, ':')) != NULL) {
        *colon = '\0';
        port = colon + 1;
    }
    size_t pl = strlen(port);
    if (*host == '\0' || strlen(host) >= sizeof b->host || pl == 0 || pl >= sizeof b->port) goto bad;
    long pv = 0;
    for (size_t i = 0; i < pl; i++) {
        if (port[i] < '0' || port[i] > '9') goto bad;
        pv = pv * 10 + (port[i] - '0');
    }
    if (pv < 1 || pv > 65535) goto bad;
    strcpy(b->host, host);
    strcpy(b->port, port);
    return 0;
bad:
    snprintf(err, ec, "invalid backend '%s' (want host:port)", tok);
    return -1;
}

int router_add(router *rt, const char *prefix, const char *spec, char *err, size_t errcap) {
    if (prefix[0] != '/') {
        snprintf(err, errcap, "route prefix '%s' must start with '/'", prefix);
        return -1;
    }
    char *copy = strdup(spec);
    route *r = calloc(1, sizeof *r);
    if (!copy || !r) {
        free(copy);
        free(r);
        snprintf(err, errcap, "out of memory");
        return -1;
    }
    char *save = NULL;
    for (char *t = strtok_r(copy, " ,\t", &save); t; t = strtok_r(NULL, " ,\t", &save)) {
        if (r->nbe >= ROUTER_MAX_BACKENDS) {
            snprintf(err, errcap, "too many backends for '%s' (max %d)", prefix, ROUTER_MAX_BACKENDS);
            goto fail;
        }
        if (parse_backend(t, &r->be[r->nbe], err, errcap) != 0) goto fail;
        r->nbe++;
    }
    if (r->nbe == 0) {
        snprintf(err, errcap, "route '%s' has no backend", prefix);
        goto fail;
    }
    r->prefix = strdup(prefix);
    route **grown = realloc(rt->routes, sizeof *grown * (size_t)(rt->n + 1));
    if (!r->prefix || !grown) {
        snprintf(err, errcap, "out of memory");
        if (grown) rt->routes = grown;
        goto fail;
    }
    r->plen = strlen(prefix);
    atomic_init(&r->rr, 0);
    rt->routes = grown;
    rt->routes[rt->n++] = r;
    free(copy);
    return 0;
fail:
    free(copy);
    free(r->prefix);
    free(r);
    return -1;
}

route *router_match(const router *rt, const char *path, size_t plen) {
    route *best = NULL;
    for (int i = 0; i < rt->n; i++) {
        route *r = rt->routes[i];
        if (r->plen > plen || memcmp(path, r->prefix, r->plen) != 0) continue;
        if (r->prefix[r->plen - 1] != '/' && plen > r->plen && path[r->plen] != '/') continue;
        if (!best || r->plen > best->plen) best = r;
    }
    return best;
}

unsigned route_pick(route *r) { return atomic_fetch_add(&r->rr, 1u); }

void router_free(router *rt) {
    for (int i = 0; i < rt->n; i++) {
        free(rt->routes[i]->prefix);
        free(rt->routes[i]);
    }
    free(rt->routes);
    rt->routes = NULL;
    rt->n = 0;
}
