/* CGI 단위 테스트: 요청 판별, 프로그램 경로 보안 검사, 출력 헤더/본문 분리 */
#include <limits.h>
#include "cgi.h"
#include "test.h"

int main(void)
{
    char out[PATH_MAX];
    cgi_output r;
    const char *o1 = "Content-Type: text/html\r\n\r\n<b>hi</b>";
    const char *o2 = "X-Other: 1\n\nplain";

    CHECK(cgi_is_request("/cgi-bin/adder"));
    CHECK(!cgi_is_request("/cgi-binx"));
    CHECK(!cgi_is_request("/index.html"));

    CHECK_INT(cgi_resolve_program("cgi-bin", "/cgi-bin/adder", out, sizeof(out)), 0);
    CHECK_INT(cgi_resolve_program("cgi-bin", "/cgi-bin/nope", out, sizeof(out)), 404);
    CHECK_INT(cgi_resolve_program("cgi-bin", "/cgi-bin/../webserver", out, sizeof(out)), 403);
    CHECK_INT(cgi_resolve_program("cgi-bin", "/cgi-bin/", out, sizeof(out)), 403);

    CHECK_INT(cgi_split_output(o1, strlen(o1), &r), 0);
    CHECK_STR(r.content_type, "text/html");
    CHECK_INT(r.body_len, 9);
    CHECK(strncmp(r.body, "<b>hi</b>", 9) == 0);
    CHECK_INT(cgi_split_output(o2, strlen(o2), &r), 0);          /* \n 만 써도 허용 */
    CHECK_STR(r.content_type, "text/plain; charset=utf-8");      /* 기본값 */
    CHECK_INT(r.body_len, 5);
    CHECK_INT(cgi_split_output("no blank line", 13, &r), -1);    /* 헤더 끝 없음 */
    return TEST_REPORT("cgi");
}
