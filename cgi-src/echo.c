/* CGI 예제 (POST): 받은 본문을 그대로 돌려준다.
 * POST 본문은 표준입력으로, 길이는 CONTENT_LENGTH 환경변수로 받는다. */
#include <stdio.h>
#include <stdlib.h>

#define ECHO_CHUNK 4096

int main(void)
{
    const char *len_str = getenv("CONTENT_LENGTH");
    long remaining = len_str ? strtol(len_str, NULL, 10) : 0;
    char buf[ECHO_CHUNK];
    size_t n;

    printf("Content-Type: text/plain; charset=utf-8\r\n\r\n");
    printf("method=%s length=%ld\n", getenv("REQUEST_METHOD"), remaining);
    while (remaining > 0 && (n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
        fwrite(buf, 1, n, stdout);
        remaining -= (long)n;
    }
    return 0;
}
