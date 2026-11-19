#include "config.h"
#include "router.h"
#include "testutil.h"

static route *match(router *rt, const char *p) { return router_match(rt, p, strlen(p)); }

int main(void) {
    router rt = {0};
    char err[128];
    CHECK(router_add(&rt, "/api", "127.0.0.1:8001, 127.0.0.1:8002", err, sizeof err) == 0);
    CHECK(router_add(&rt, "/api/v2/", "http://localhost:9000/", err, sizeof err) == 0);
    CHECK(router_add(&rt, "/static/", "[::1]:8003 example.com", err, sizeof err) == 0);

    route *r = match(&rt, "/api");
    CHECK(r && r->nbe == 2 && strcmp(r->be[1].port, "8002") == 0);
    CHECK(match(&rt, "/api/users") == r);
    CHECK(match(&rt, "/apix") == NULL);    /* prefix must end on a path boundary */
    CHECK(match(&rt, "/ap") == NULL);
    CHECK(match(&rt, "/") == NULL);
    r = match(&rt, "/api/v2/items");
    CHECK(r && strcmp(r->be[0].host, "localhost") == 0 && strcmp(r->be[0].port, "9000") == 0);
    CHECK(match(&rt, "/api/v2") != r);     /* "/api/v2/" needs the trailing slash... */
    CHECK(match(&rt, "/api/v2") == match(&rt, "/api")); /* ...so it falls back to "/api" */
    r = match(&rt, "/static/a.png");
    CHECK(r && r->nbe == 2 && strcmp(r->be[0].host, "::1") == 0);
    CHECK(strcmp(r->be[1].host, "example.com") == 0 && strcmp(r->be[1].port, "80") == 0);
    CHECK(match(&rt, "/static") == NULL);  /* trailing-slash prefix: plain startswith */

    /* round robin cursor advances per pick */
    r = match(&rt, "/api");
    unsigned a = route_pick(r), b = route_pick(r), c = route_pick(r);
    CHECK(b == a + 1 && c == a + 2);
    CHECK((a % 2) != (b % 2) && (a % 2) == (c % 2));

    /* invalid input is rejected without leaking or adding routes */
    int n = rt.n;
    CHECK(router_add(&rt, "api", "h:1", err, sizeof err) == -1);
    CHECK(router_add(&rt, "/x", "", err, sizeof err) == -1);
    CHECK(router_add(&rt, "/x", "host:0", err, sizeof err) == -1);
    CHECK(router_add(&rt, "/x", "host:70000", err, sizeof err) == -1);
    CHECK(router_add(&rt, "/x", "host:12ab", err, sizeof err) == -1);
    CHECK(router_add(&rt, "/x", ":80", err, sizeof err) == -1);
    CHECK(router_add(&rt, "/x", "[::1", err, sizeof err) == -1);
    CHECK(router_add(&rt, "/x", "a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8,i:9,j:10,k:11,l:12,m:13,n:14,o:15,p:16,q:17", err, sizeof err) == -1);
    CHECK(rt.n == n);
    router_free(&rt);

    /* config lines */
    config cfg;
    config_init(&cfg);
    CHECK(config_apply_line(&cfg, "# comment\n", err, sizeof err) == 0);
    CHECK(config_apply_line(&cfg, "   \n", err, sizeof err) == 0);
    CHECK(config_apply_line(&cfg, "port 9090  # trailing comment\n", err, sizeof err) == 0 && cfg.port == 9090);
    CHECK(config_apply_line(&cfg, "threads 4", err, sizeof err) == 0 && cfg.threads == 4);
    CHECK(config_apply_line(&cfg, "timeout 3", err, sizeof err) == 0 && cfg.upstream_timeout_ms == 3000);
    CHECK(config_apply_line(&cfg, "root /srv/www", err, sizeof err) == 0 && strcmp(cfg.root, "/srv/www") == 0);
    CHECK(config_apply_line(&cfg, "route /api 127.0.0.1:1 127.0.0.1:2", err, sizeof err) == 0 && cfg.rt.n == 1);
    CHECK(config_apply_line(&cfg, "port 0", err, sizeof err) == -1);
    CHECK(config_apply_line(&cfg, "port abc", err, sizeof err) == -1);
    CHECK(config_apply_line(&cfg, "port", err, sizeof err) == -1);
    CHECK(config_apply_line(&cfg, "bogus 1", err, sizeof err) == -1 && strstr(err, "bogus"));
    CHECK(config_apply_line(&cfg, "route /only-prefix", err, sizeof err) == -1);
    CHECK(cfg.port == 9090);
    config_free(&cfg);
    TEST_DONE();
}
