#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "server.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-p port] [-r root] [-w threads] [-t upstream_timeout_s] [-c config]\n"
            "          [-R /prefix=host:port[,host:port...]]... [-q]\n"
            "  -R   reverse-proxy requests under /prefix to the backends (round-robin)\n"
            "  -c   config file (port/root/threads/queue/timeout/route lines)\n"
            "  -q   no access log\n"
            "Options are applied in order, so later ones override -c.\n",
            prog);
}

static void on_signal(int sig) {
    (void)sig;
    server_request_stop();
}

static int fail(const char *what, const char *msg) {
    fprintf(stderr, "%s: %s\n", what, msg);
    return 2;
}

int main(int argc, char **argv) {
    config cfg;
    config_init(&cfg);
    char err[256], line[PATH_MAX + 16];
    int opt, rc = 0;

    while ((opt = getopt(argc, argv, "p:r:w:t:c:R:qh")) != -1) {
        switch (opt) {
        case 'p':
            snprintf(line, sizeof line, "port %s", optarg);
            rc = config_apply_line(&cfg, line, err, sizeof err);
            break;
        case 'r':
            snprintf(line, sizeof line, "root %s", optarg);
            rc = config_apply_line(&cfg, line, err, sizeof err);
            break;
        case 'w':
            snprintf(line, sizeof line, "threads %s", optarg);
            rc = config_apply_line(&cfg, line, err, sizeof err);
            break;
        case 't':
            snprintf(line, sizeof line, "timeout %s", optarg);
            rc = config_apply_line(&cfg, line, err, sizeof err);
            break;
        case 'c':
            rc = config_load_file(&cfg, optarg, err, sizeof err);
            break;
        case 'R': {
            char *eq = strchr(optarg, '=');
            if (!eq) {
                config_free(&cfg);
                return fail("-R", "expected /prefix=host:port[,host:port...]");
            }
            *eq = '\0';
            rc = router_add(&cfg.rt, optarg, eq + 1, err, sizeof err);
            break;
        }
        case 'q':
            cfg.quiet = 1;
            break;
        default:
            usage(argv[0]);
            config_free(&cfg);
            return opt == 'h' ? 0 : 2;
        }
        if (rc != 0) {
            config_free(&cfg);
            return fail("error", err);
        }
    }

    /* The static root is resolved once; static_serve checks every file against it. */
    char real[PATH_MAX];
    struct stat st;
    if (!realpath(cfg.root, real) || stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        config_free(&cfg);
        return fail(cfg.root, "root is not a directory");
    }
    snprintf(cfg.root, sizeof cfg.root, "%s", real);

    signal(SIGPIPE, SIG_IGN); /* a client vanishing mid-write must not kill the server */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    rc = server_run(&cfg);
    config_free(&cfg);
    return rc;
}
