#ifndef TEST_H
#define TEST_H

/* 아주 작은 테스트 도구: CHECK 가 실패하면 위치를 출력하고 실패 수를 센다. */
#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failures++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_INT(a, b) do { \
    long _a = (long)(a), _b = (long)(b); g_checks++; \
    if (_a != _b) { g_failures++; fprintf(stderr, "  FAIL %s:%d: %s == %ld, expected %ld\n", __FILE__, __LINE__, #a, _a, _b); } \
} while (0)

#define CHECK_STR(a, b) do { \
    const char *_a = (a), *_b = (b); g_checks++; \
    if (_a == NULL || strcmp(_a, _b) != 0) { g_failures++; fprintf(stderr, "  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, _a ? _a : "(null)", _b); } \
} while (0)

#define TEST_REPORT(name) \
    (printf("[%s] %d checks, %d failed -> %s\n", name, g_checks, g_failures, g_failures ? "FAIL" : "PASS"), \
     g_failures ? 1 : 0)

#endif
