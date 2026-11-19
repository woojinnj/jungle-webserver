#ifndef TESTUTIL_H
#define TESTUTIL_H

#include <stdio.h>
#include <string.h>

static int t_run, t_fail;

#define CHECK(c)                                                                   \
    do {                                                                           \
        t_run++;                                                                   \
        if (!(c)) {                                                                \
            t_fail++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
        }                                                                          \
    } while (0)

#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)

#define TEST_DONE()                                                                \
    do {                                                                           \
        printf("%-28s %3d checks, %d failed\n", __FILE__, t_run, t_fail);          \
        return t_fail ? 1 : 0;                                                     \
    } while (0)

#endif
