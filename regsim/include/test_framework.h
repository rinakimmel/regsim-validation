/*
 * test_framework.h — Minimal test framework for the regsim test suite.
 *
 * Features:
 *   - Named test cases with PASS / FAIL results
 *   - Descriptive failure messages with file/line
 *   - Summary table printed at end
 *   - Exit code: 0 if all tests pass, 1 if any fail
 *
 * Usage example:
 *
 *   #include "test_framework.h"
 *
 *   static void test_addition(test_suite_t *s) {
 *       TEST_ASSERT(s, 1 + 1 == 2);
 *       TEST_ASSERT_EQ(s, (int)1+1, 2);
 *   }
 *
 *   int main(void) {
 *       test_suite_t *s = test_suite_new("Math Tests");
 *       RUN_TEST(s, test_addition);
 *       return test_suite_finish(s);
 *   }
 */

#ifndef REGSIM_TEST_FRAMEWORK_H
#define REGSIM_TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

#define TF_MAX_TESTS 256

typedef struct {
    const char *suite_name;
    int         total;
    int         passed;
    int         failed;

    /* Per-test tracking */
    const char *current_test;
    int         current_failed;

    /* Results table */
    struct {
        const char *name;
        int         passed;
        char        msg[256];
    } results[TF_MAX_TESTS];
} test_suite_t;

/* -------------------------------------------------------------------------
 * Suite lifecycle
 * ---------------------------------------------------------------------- */

static inline test_suite_t *test_suite_new(const char *name) {
    test_suite_t *s = (test_suite_t *)calloc(1, sizeof(test_suite_t));
    if (!s) { perror("calloc"); exit(1); }
    s->suite_name = name;
    return s;
}

/* Print results table and return 0 (all pass) or 1 (any fail). */
static inline int test_suite_finish(test_suite_t *s) {
    printf("\n");
    printf("=================================================================\n");
    printf("  Test Suite: %s\n", s->suite_name);
    printf("=================================================================\n");
    printf("  %-42s  %s\n", "Test Name", "Result");
    printf("  %-42s  ------\n", "------------------------------------------");
    for (int i = 0; i < s->total; i++) {
        printf("  %-42s  %s",
               s->results[i].name,
               s->results[i].passed ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m");
        if (!s->results[i].passed && s->results[i].msg[0]) {
            printf("  (%s)", s->results[i].msg);
        }
        printf("\n");
    }
    printf("-----------------------------------------------------------------\n");
    printf("  Total: %d   Passed: \033[32m%d\033[0m   Failed: \033[31m%d\033[0m\n",
           s->total, s->passed, s->failed);
    printf("=================================================================\n");
    int rc = (s->failed > 0) ? 1 : 0;
    free(s);
    return rc;
}

/* -------------------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------------- */

typedef void (*test_fn_t)(test_suite_t *);

static inline void _tf_run(test_suite_t *s, test_fn_t fn, const char *name) {
    if (s->total >= TF_MAX_TESTS) {
        fprintf(stderr, "test_framework: too many tests (max %d)\n", TF_MAX_TESTS);
        exit(1);
    }
    s->current_test   = name;
    s->current_failed = 0;
    fn(s);
    int idx = s->total++;
    s->results[idx].name   = name;
    s->results[idx].passed = (s->current_failed == 0);
    if (s->current_failed == 0) {
        s->passed++;
    } else {
        s->failed++;
    }
}

#define RUN_TEST(suite, fn)  _tf_run((suite), (fn), #fn)

/* -------------------------------------------------------------------------
 * Assertion macros
 * ---------------------------------------------------------------------- */

/* Generic boolean assertion */
#define TEST_ASSERT(s, expr)                                                \
    do {                                                                    \
        if (!(expr)) {                                                      \
            if ((s)->current_failed == 0) {                                 \
                snprintf((s)->results[(s)->total].msg,                      \
                         sizeof((s)->results[(s)->total].msg),              \
                         "%s:%d: assertion failed: %s",                     \
                         __FILE__, __LINE__, #expr);                        \
            }                                                               \
            (s)->current_failed++;                                          \
        }                                                                   \
    } while (0)

/* Integer equality */
#define TEST_ASSERT_EQ(s, a, b)                                             \
    do {                                                                    \
        long long _a = (long long)(a);                                      \
        long long _b = (long long)(b);                                      \
        if (_a != _b) {                                                     \
            if ((s)->current_failed == 0) {                                 \
                snprintf((s)->results[(s)->total].msg,                      \
                         sizeof((s)->results[(s)->total].msg),              \
                         "%s:%d: %s == %s  got %lld != %lld",              \
                         __FILE__, __LINE__, #a, #b, _a, _b);              \
            }                                                               \
            (s)->current_failed++;                                          \
        }                                                                   \
    } while (0)

/* Unsigned hex equality */
#define TEST_ASSERT_HEX(s, a, b)                                            \
    do {                                                                    \
        unsigned long long _a = (unsigned long long)(a);                    \
        unsigned long long _b = (unsigned long long)(b);                    \
        if (_a != _b) {                                                     \
            if ((s)->current_failed == 0) {                                 \
                snprintf((s)->results[(s)->total].msg,                      \
                         sizeof((s)->results[(s)->total].msg),              \
                         "%s:%d: %s == %s  got 0x%llX != 0x%llX",         \
                         __FILE__, __LINE__, #a, #b, _a, _b);              \
            }                                                               \
            (s)->current_failed++;                                          \
        }                                                                   \
    } while (0)

/* Not-equal assertion */
#define TEST_ASSERT_NEQ(s, a, b)                                            \
    do {                                                                    \
        long long _a = (long long)(a);                                      \
        long long _b = (long long)(b);                                      \
        if (_a == _b) {                                                     \
            if ((s)->current_failed == 0) {                                 \
                snprintf((s)->results[(s)->total].msg,                      \
                         sizeof((s)->results[(s)->total].msg),              \
                         "%s:%d: expected %s != %s, both = %lld",          \
                         __FILE__, __LINE__, #a, #b, _a);                   \
            }                                                               \
            (s)->current_failed++;                                          \
        }                                                                   \
    } while (0)

/* Explicit fail with message */
#define TEST_FAIL(s, msg_str)                                               \
    do {                                                                    \
        if ((s)->current_failed == 0) {                                     \
            snprintf((s)->results[(s)->total].msg,                          \
                     sizeof((s)->results[(s)->total].msg),                  \
                     "%s:%d: %s", __FILE__, __LINE__, (msg_str));           \
        }                                                                   \
        (s)->current_failed++;                                              \
    } while (0)

#endif /* REGSIM_TEST_FRAMEWORK_H */
