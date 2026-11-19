#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "static.h"
#include "testutil.h"

static int resolve(const char *uri, char *out) {
    return static_resolve("/srv/www", uri, strlen(uri), out, 4096);
}

static void test_resolve(void) {
    char o[4096];
    CHECK(resolve("/index.html", o) == 0 && strcmp(o, "/srv/www/index.html") == 0);
    CHECK(resolve("/a/b.css?v=1&x=%2e%2e", o) == 0 && strcmp(o, "/srv/www/a/b.css") == 0);
    CHECK(resolve("/", o) == 0 && strcmp(o, "/srv/www/") == 0);
    CHECK(resolve("/a%20b.txt", o) == 0 && strcmp(o, "/srv/www/a b.txt") == 0);
    CHECK(resolve("/..foo/bar..", o) == 0);           /* ".." only as a whole segment */
    /* traversal, plain and encoded */
    CHECK(resolve("/../etc/passwd", o) == 403);
    CHECK(resolve("/a/../../etc/passwd", o) == 403);
    CHECK(resolve("/%2e%2e/etc/passwd", o) == 403);
    CHECK(resolve("/%2E%2e/etc/passwd", o) == 403);
    CHECK(resolve("/a/..", o) == 403);
    /* bad encodings */
    CHECK(resolve("/%zz", o) == 400);
    CHECK(resolve("/%4", o) == 400);
    CHECK(resolve("/%", o) == 400);
    CHECK(resolve("/a%00b", o) == 400);
    CHECK(resolve("noslash", o) == 400);
    /* too long for the output buffer */
    char small[16];
    CHECK(static_resolve("/srv/www", "/aaaaaaaaaaaaaaaaaaaa", 21, small, sizeof small) == 414);
}

static void test_serve(void) {
    /* build a throwaway root:  index.html, sub/index.html, sub2/ (no index), link -> outside */
    char tmpl[] = "/tmp/cwebd_test_XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    char root[64], p[128], cmd[512];
    snprintf(root, sizeof root, "%s", tmpl);
    snprintf(p, sizeof p, "%s/index.html", root);
    FILE *f = fopen(p, "w");
    fputs("<h1>hi</h1>", f);
    fclose(f);
    snprintf(cmd, sizeof cmd, "mkdir %s/sub %s/sub2 && echo x > %s/sub/index.html && ln -s /etc/passwd %s/link", root, root, root, root);
    CHECK(system(cmd) == 0);

    struct {
        const char *uri;
        int status;
    } cases[] = {{"/", 200},        {"/index.html", 200}, {"/sub/", 200}, {"/sub", 301},
                 {"/sub2/", 403},   {"/missing", 404},    {"/link", 403}, {"/../x", 403}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        char req[256];
        snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: t\r\n\r\n", cases[i].uri);
        http_request r;
        CHECK(http_parse_request(req, strlen(req), &r) == 0);
        size_t bytes;
        int st = static_serve(sv[0], root, &r, 0, &bytes);
        if (st != cases[i].status) fprintf(stderr, "uri %s: got %d want %d\n", cases[i].uri, st, cases[i].status);
        CHECK(st == cases[i].status);
        close(sv[0]);
        char resp[1024];
        ssize_t n = read(sv[1], resp, sizeof resp - 1);
        CHECK(n > 0);
        if (n > 0) {
            resp[n] = '\0';
            char want[32];
            snprintf(want, sizeof want, "HTTP/1.1 %d ", cases[i].status);
            CHECK(strncmp(resp, want, strlen(want)) == 0);
        }
        close(sv[1]);
    }
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    CHECK(system(cmd) == 0);
}

int main(void) {
    test_resolve();
    test_serve();
    TEST_DONE();
}
