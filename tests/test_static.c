/* 정적 파일 경로 해석 단위 테스트: .. 차단, 루트 밖 차단, index.html, 404 */
#include <limits.h>
#include <stdlib.h>
#include "static_file.h"
#include "test.h"

int main(void)
{
    char root[PATH_MAX], out[PATH_MAX];

    CHECK(static_has_dotdot("/../etc/passwd"));
    CHECK(static_has_dotdot("/a/.."));
    CHECK(!static_has_dotdot("/a..b/c"));          /* 이름 안의 .. 는 허용 */
    CHECK(!static_has_dotdot("/index.html"));

    CHECK(static_is_inside_root("/srv/www", "/srv/www/a.html"));
    CHECK(static_is_inside_root("/srv/www", "/srv/www"));
    CHECK(!static_is_inside_root("/srv/www", "/srv/www2/a.html"));   /* 접두사만 같은 경우 */
    CHECK(!static_is_inside_root("/srv/www", "/etc/passwd"));

    if (realpath("www", root) == NULL) {
        fprintf(stderr, "run from repository root\n");
        return 1;
    }
    CHECK_INT(static_resolve_path(root, "/", out, sizeof(out)), 0);
    CHECK(strstr(out, "/www/index.html") != NULL);                  /* 디렉토리 → index.html */
    CHECK_INT(static_resolve_path(root, "/css/style.css", out, sizeof(out)), 0);
    CHECK_INT(static_resolve_path(root, "/nope.html", out, sizeof(out)), 404);
    CHECK_INT(static_resolve_path(root, "/../Makefile", out, sizeof(out)), 403);
    CHECK_INT(static_resolve_path(root, "noslash", out, sizeof(out)), 403);
    return TEST_REPORT("static");
}
