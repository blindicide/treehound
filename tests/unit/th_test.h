/* SPDX-License-Identifier: MIT */
/* Minimal assertion helpers for the unit tests. */
#ifndef TH_TEST_H
#define TH_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int th_test_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            th_test_failures++;                                                  \
        }                                                                        \
    } while (0)

#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        const char *a_ = (a), *b_ = (b);                                         \
        if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                 \
            fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__,    \
                    __LINE__, b_ ? b_ : "(null)", a_ ? a_ : "(null)");           \
            th_test_failures++;                                                  \
        }                                                                        \
    } while (0)

#define CHECK_INT(a, b)                                                          \
    do {                                                                         \
        long long a_ = (long long)(a), b_ = (long long)(b);                      \
        if (a_ != b_) {                                                          \
            fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__,      \
                    __LINE__, #a, a_, b_);                                       \
            th_test_failures++;                                                  \
        }                                                                        \
    } while (0)

#define REQUIRE(cond)                                                            \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

#define RUN(fn)                                                                  \
    do {                                                                         \
        int before_ = th_test_failures;                                          \
        fn();                                                                    \
        fprintf(stderr, "%-40s %s\n", #fn, before_ == th_test_failures ? "ok" : "FAILED"); \
    } while (0)

#define TEST_EXIT() (th_test_failures ? (fprintf(stderr, "%d failure(s)\n", th_test_failures), 1) : 0)

#endif
