/* MIME 매핑 단위 테스트 */
#include "mime.h"
#include "test.h"

int main(void)
{
    CHECK_STR(mime_type_for_path("/a/index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for_path("style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type_for_path("app.js"), "application/javascript; charset=utf-8");
    CHECK_STR(mime_type_for_path("logo.PNG"), "image/png");          /* 대소문자 무시 */
    CHECK_STR(mime_type_for_path("photo.jpg"), "image/jpeg");
    CHECK_STR(mime_type_for_path("photo.jpeg"), "image/jpeg");
    CHECK_STR(mime_type_for_path("noext"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("/dir.d/file"), "application/octet-stream"); /* 디렉토리의 점 */
    CHECK_STR(mime_type_for_path("archive.xyz"), "application/octet-stream");
    return TEST_REPORT("mime");
}
