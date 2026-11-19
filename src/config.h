#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <stddef.h>

#include "router.h"

typedef struct {
    int port;
    char root[PATH_MAX];
    int threads;
    int queue;               /* pending-connection queue length */
    int upstream_timeout_ms; /* backend read timeout */
    int quiet;               /* no access log */
    router rt;
} config;

void config_init(config *c);
void config_free(config *c);

/* One config-file line: "port N", "root DIR", "threads N", "queue N",
 * "timeout SECONDS", "route /prefix host:port [host:port ...]", '#' comments.
 * 0 ok (blank/comment too), -1 + err text. */
int config_apply_line(config *c, const char *line, char *err, size_t errcap);

int config_load_file(config *c, const char *path, char *err, size_t errcap);

#endif
