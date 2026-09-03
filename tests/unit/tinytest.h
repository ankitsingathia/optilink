/*
 * tinytest.h - just enough test framework.
 *
 * No registration macros, no fixtures, no dependency to vendor. Each suite is
 * a plain function, main.c calls them in order, and a failure prints file,
 * line, case name and both values. That is the whole framework, and it links
 * into the same freestanding-ish build as the firmware.
 */
#ifndef TINYTEST_H
#define TINYTEST_H

#include <stdio.h>
#include <string.h>

extern unsigned tt_checks;
extern unsigned tt_failures;
extern const char *tt_case;

#define TT_CASE(name) (tt_case = (name))

#define CHECK(cond)                                                           \
    do {                                                                      \
        tt_checks++;                                                          \
        if (!(cond)) {                                                        \
            tt_failures++;                                                    \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, tt_case,       \
                   #cond);                                                    \
        }                                                                     \
    } while (0)

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        long long tt_a = (long long)(actual);                                 \
        long long tt_e = (long long)(expected);                               \
        tt_checks++;                                                          \
        if (tt_a != tt_e) {                                                   \
            tt_failures++;                                                    \
            printf("FAIL %s:%d [%s] %s: got %lld want %lld\n", __FILE__,      \
                   __LINE__, tt_case, #actual, tt_a, tt_e);                   \
        }                                                                     \
    } while (0)

#define CHECK_STR(actual, expected)                                           \
    do {                                                                      \
        tt_checks++;                                                          \
        if (strcmp((actual), (expected)) != 0) {                              \
            tt_failures++;                                                    \
            printf("FAIL %s:%d [%s] got \"%s\" want \"%s\"\n", __FILE__,      \
                   __LINE__, tt_case, (actual), (expected));                  \
        }                                                                     \
    } while (0)

#endif /* TINYTEST_H */
