#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_init(config *c) {
    memset(c, 0, sizeof *c);
    c->port = 8080;
    strcpy(c->root, "./www");
    c->threads = 32; /* each idle keep-alive client pins a thread for up to 5s */
    c->queue = 1024;
    c->upstream_timeout_ms = 10000;
}

void config_free(config *c) { router_free(&c->rt); }

static int parse_int(const char *s, long lo, long hi, long *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

int config_apply_line(config *c, const char *line, char *err, size_t ec) {
    char buf[4096];
    if (strlen(line) >= sizeof buf) {
        snprintf(err, ec, "line too long");
        return -1;
    }
    strcpy(buf, line);
    char *h = strchr(buf, '#');
    if (h) *h = '\0';

    char *p = buf;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '\0') return 0;
    char *kw = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p) *p++ = '\0';
    while (isspace((unsigned char)*p)) p++;
    char *end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1])) *--end = '\0';
    if (*p == '\0') {
        snprintf(err, ec, "'%s' needs a value", kw);
        return -1;
    }

    long v;
    if (strcmp(kw, "port") == 0) {
        if (parse_int(p, 1, 65535, &v)) goto badval;
        c->port = (int)v;
    } else if (strcmp(kw, "threads") == 0) {
        if (parse_int(p, 1, 512, &v)) goto badval;
        c->threads = (int)v;
    } else if (strcmp(kw, "queue") == 0) {
        if (parse_int(p, 1, 100000, &v)) goto badval;
        c->queue = (int)v;
    } else if (strcmp(kw, "timeout") == 0) {
        if (parse_int(p, 1, 3600, &v)) goto badval;
        c->upstream_timeout_ms = (int)v * 1000;
    } else if (strcmp(kw, "root") == 0) {
        if (strlen(p) >= sizeof c->root) goto badval;
        strcpy(c->root, p);
    } else if (strcmp(kw, "route") == 0) {
        char *sp = p;
        while (*sp && !isspace((unsigned char)*sp)) sp++;
        if (*sp) *sp++ = '\0';
        return router_add(&c->rt, p, sp, err, ec);
    } else {
        snprintf(err, ec, "unknown directive '%s'", kw);
        return -1;
    }
    return 0;
badval:
    snprintf(err, ec, "bad value '%s' for '%s'", p, kw);
    return -1;
}

int config_load_file(config *c, const char *path, char *err, size_t ec) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, ec, "%s: %s", path, strerror(errno));
        return -1;
    }
    char line[4096], msg[256];
    for (int no = 1; fgets(line, sizeof line, f); no++) {
        if (config_apply_line(c, line, msg, sizeof msg) != 0) {
            snprintf(err, ec, "%s:%d: %s", path, no, msg);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}
