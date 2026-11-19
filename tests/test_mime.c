#include "mime.h"
#include "testutil.h"

int main(void) {
    CHECK_STR(mime_type("/www/index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type("a.CSS"), "text/css; charset=utf-8");
    CHECK_STR(mime_type("app.js"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_type("a.PNG"), "image/png");
    CHECK_STR(mime_type("a.jpg"), "image/jpeg");
    CHECK_STR(mime_type("a.jpeg"), "image/jpeg");
    CHECK_STR(mime_type("logo.svg"), "image/svg+xml");
    CHECK_STR(mime_type("data.json"), "application/json");
    CHECK_STR(mime_type("archive.tar.gz"), "application/octet-stream");
    CHECK_STR(mime_type("README"), "application/octet-stream");
    CHECK_STR(mime_type("/dir.html/file"), "application/octet-stream"); /* dot in a directory name */
    CHECK_STR(mime_type("trailingdot."), "application/octet-stream");
    CHECK_STR(mime_type(".html"), "text/html; charset=utf-8");
    TEST_DONE();
}
