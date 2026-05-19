#ifndef UNITY_H
#define UNITY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── counters ─────────────────────────────────────────────────────────────── */
extern unsigned unity_tests_run;
extern unsigned unity_tests_passed;
extern unsigned unity_tests_failed;
extern const char *unity_current_test;

/* ── internal helpers ─────────────────────────────────────────────────────── */
void unity_fail(const char *file, int line, const char *msg);

/* Forward declarations — test files define these; weak defaults in test_main.c */
void setUp(void);
void tearDown(void);

#define UNITY_BEGIN()  do { unity_tests_run=0; unity_tests_passed=0; unity_tests_failed=0; } while(0)

#define UNITY_END()    unity_print_summary()
void unity_print_summary(void);

#define RUN_TEST(fn)  do {                                        \
    unity_current_test = #fn;                                     \
    unity_tests_run++;                                            \
    unsigned _f = unity_tests_failed;                            \
    setUp();                                                      \
    fn();                                                         \
    tearDown();                                                   \
    if (unity_tests_failed == _f) {                              \
        printf("  [PASS] %s\n", #fn);                           \
        unity_tests_passed++;                                     \
    }                                                             \
} while(0)

/* ── assertion macros ─────────────────────────────────────────────────────── */
#define TEST_FAIL_MESSAGE(msg) \
    unity_fail(__FILE__, __LINE__, msg)

#define TEST_ASSERT_TRUE(cond) do { \
    if (!(cond)) { unity_fail(__FILE__,__LINE__, "Expected TRUE: " #cond); return; } \
} while(0)

#define TEST_ASSERT_FALSE(cond) do { \
    if ((cond)) { unity_fail(__FILE__,__LINE__, "Expected FALSE: " #cond); return; } \
} while(0)

#define TEST_ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { unity_fail(__FILE__,__LINE__, "Expected NULL: " #ptr); return; } \
} while(0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { unity_fail(__FILE__,__LINE__, "Expected NOT NULL: " #ptr); return; } \
} while(0)

#define TEST_ASSERT_EQUAL(expected, actual) do { \
    if ((long long)(expected) != (long long)(actual)) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), "Expected %lld but got %lld (" #actual ")", \
                 (long long)(expected), (long long)(actual)); \
        unity_fail(__FILE__,__LINE__,_buf); return; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_INT    TEST_ASSERT_EQUAL
#define TEST_ASSERT_EQUAL_INT8   TEST_ASSERT_EQUAL
#define TEST_ASSERT_EQUAL_INT16  TEST_ASSERT_EQUAL
#define TEST_ASSERT_EQUAL_INT32  TEST_ASSERT_EQUAL
#define TEST_ASSERT_EQUAL_UINT   TEST_ASSERT_EQUAL
#define TEST_ASSERT_EQUAL_UINT8  TEST_ASSERT_EQUAL
#define TEST_ASSERT_EQUAL_UINT16 TEST_ASSERT_EQUAL
#define TEST_ASSERT_EQUAL_UINT32 TEST_ASSERT_EQUAL
#define TEST_ASSERT_EQUAL_HEX    TEST_ASSERT_EQUAL
#define TEST_ASSERT_EQUAL_HEX8   TEST_ASSERT_EQUAL
#define TEST_ASSERT_EQUAL_HEX16  TEST_ASSERT_EQUAL

#define TEST_ASSERT_NOT_EQUAL(a, b) do { \
    if ((long long)(a) == (long long)(b)) { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), "Expected NOT EQUAL to %lld", (long long)(a)); \
        unity_fail(__FILE__,__LINE__,_buf); return; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_MEMORY(expected, actual, len) do { \
    if (memcmp((expected), (actual), (len)) != 0) { \
        unity_fail(__FILE__,__LINE__, "Memory mismatch: " #actual); return; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) do { \
    if (strcmp((expected), (actual)) != 0) { \
        unity_fail(__FILE__,__LINE__, "String mismatch"); return; \
    } \
} while(0)

#define TEST_ASSERT_GREATER_THAN(threshold, actual) do { \
    if (!((long long)(actual) > (long long)(threshold))) { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), "%lld not > %lld", (long long)(actual), (long long)(threshold)); \
        unity_fail(__FILE__,__LINE__,_buf); return; \
    } \
} while(0)

#define TEST_ASSERT_LESS_THAN(threshold, actual) do { \
    if (!((long long)(actual) < (long long)(threshold))) { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), "%lld not < %lld", (long long)(actual), (long long)(threshold)); \
        unity_fail(__FILE__,__LINE__,_buf); return; \
    } \
} while(0)

#define TEST_ASSERT_GREATER_OR_EQUAL(threshold, actual) do { \
    if (!((long long)(actual) >= (long long)(threshold))) { \
        unity_fail(__FILE__,__LINE__, #actual " not >= " #threshold); return; \
    } \
} while(0)

#define TEST_ASSERT_BITS(mask, expected, actual) do { \
    if (((expected) & (mask)) != ((actual) & (mask))) { \
        unity_fail(__FILE__,__LINE__, "Bits mismatch: " #actual); return; \
    } \
} while(0)

#endif /* UNITY_H */
