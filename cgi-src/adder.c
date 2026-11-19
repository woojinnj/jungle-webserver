/* CGI 예제 (GET): /cgi-bin/adder?a=1&b=2 → 두 수의 합.
 * CSAPP 11.5.4 / 그림 11.27 (adder): 인자는 QUERY_STRING 환경변수로 받는다. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "a=1&b=2" 에서 key 의 값을 정수로 읽는다. 없으면 0.
 * QUERY_STRING 은 key=value 를 & 로 이은 문자열. */
static long query_value(const char *query, const char *key)
{
    size_t klen = strlen(key);
    const char *p = query;

    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
            return strtol(p + klen + 1, NULL, 10);
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return 0;
}

int main(void)
{
    const char *query = getenv("QUERY_STRING");
    long a = query_value(query ? query : "", "a");
    long b = query_value(query ? query : "", "b");

    printf("Content-Type: text/plain; charset=utf-8\r\n\r\n");
    printf("%ld + %ld = %ld\n", a, b, a + b);
    return 0;
}
