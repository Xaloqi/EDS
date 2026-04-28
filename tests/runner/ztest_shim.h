// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: tests/runner/ztest_shim.h
 *
 * PURPOSE: Zephyr Ztest-to-Unity compatibility shim for host-side unit test
 *          compilation. Maps ZTEST(), ZTEST_SUITE(), and zassert_*() macros
 *          to their Unity equivalents, allowing the existing test files that
 *          were written for Zephyr's ztest framework to compile and run on a
 *          development host or CI runner without the Zephyr SDK.
 *
 *          Architecture:
 *            tests/unit/test_*.c     (use ZTEST / zassert macros)
 *                      |
 *            tests/runner/ztest_shim.h    (this file — remaps macros)
 *                      |
 *            project/unity/unity.h        (host Unity implementation)
 *
 *          This design allows the same test source files to be compiled
 *          for both Zephyr on-target execution (include <zephyr/ztest.h>)
 *          and host-native CI execution (include this shim instead).
 *
 *          The CMakeLists.txt selects the shim by adding -include directives
 *          that replace <zephyr/ztest.h> with this shim file automatically,
 *          so test source files do not need to be modified.
 *
 * USAGE:
 *   Compile with: -DZTEST_HOST_SHIM -include tests/runner/ztest_shim.h
 *   The -include causes this file to be injected before any #include in
 *   the test source, so ztest.h is never actually included.
 *
 * SAFETY  : Test infrastructure only — not safety-assessed.
 *           Do NOT include in production firmware build.
 * STANDARD: MISRA C:2012 alignment intended (test code, advisory only).
 * =============================================================================
 */

#ifndef ZTEST_SHIM_H
#define ZTEST_SHIM_H

/* Include the Unity host harness. */
#include "unity.h"

/*
 * Prevent the real Zephyr ztest.h from being included.
 * Since CMake injects this via -include before any #include in the test
 * source, the include guard here fires when the test file tries to include
 * <zephyr/ztest.h>, harmlessly suppressing the real include.
 */
#ifndef ZEPHYR_INCLUDE_ZTEST_H_
#define ZEPHYR_INCLUDE_ZTEST_H_
#endif

/* --------------------------------------------------------------------------
 * ZTEST_SUITE — no-op under host compilation
 *
 * Zephyr uses ZTEST_SUITE to register test suites with the Ztest runner.
 * Under Unity we register tests individually via RUN_TEST() in run_all_tests(),
 * so ZTEST_SUITE is a no-op.
 *
 * Signature: ZTEST_SUITE(suite_name, NULL, setup, before, after, teardown)
 * -------------------------------------------------------------------------- */
#define ZTEST_SUITE(name, ...)  /* no-op under host compilation */

/* --------------------------------------------------------------------------
 * ZTEST — map to a plain C function definition
 *
 * Zephyr: ZTEST(suite, fn) { body }
 * Unity:  void suite##__##fn(void) { body }
 *
 * The generated function name uses double-underscore to avoid conflicts
 * between test functions with the same local name in different suites.
 * -------------------------------------------------------------------------- */
#define ZTEST(suite, fn)  static void suite##__##fn(void)

/* --------------------------------------------------------------------------
 * zassert — map to Unity TEST_ASSERT_* equivalents
 * -------------------------------------------------------------------------- */

#define zassert_equal(a, b, ...)             TEST_ASSERT_EQUAL((a), (b))
#define zassert_not_equal(a, b, ...)         TEST_ASSERT_NOT_EQUAL((a), (b))
#define zassert_true(cond, ...)              TEST_ASSERT_TRUE(cond)
#define zassert_false(cond, ...)             TEST_ASSERT_FALSE(cond)
#define zassert_is_null(ptr, ...)            TEST_ASSERT_NULL(ptr)
#define zassert_not_null(ptr, ...)           TEST_ASSERT_NOT_NULL(ptr)
#define zassert_mem_equal(exp, act, len, ...) TEST_ASSERT_EQUAL_MEMORY((exp), (act), (len))
#define zassert_str_equal(exp, act, ...)     TEST_ASSERT_EQUAL_STRING((exp), (act))

/*
 * zassert_ok — Zephyr-specific: asserts that a return code equals 0.
 * Maps to TEST_ASSERT_EQUAL(0, expr).
 */
#define zassert_ok(expr, ...)                TEST_ASSERT_EQUAL(0, (expr))

/*
 * zassert_between_inclusive — range check.
 * Maps to two sequential assertions.
 */
#define zassert_between_inclusive(val, lo, hi, ...) \
    do { \
        TEST_ASSERT_GREATER_OR_EQUAL((lo), (val)); \
        TEST_ASSERT_LESS_THAN((hi) + 1, (val) + 1); \
    } while (0)

/* --------------------------------------------------------------------------
 * Zephyr FAIL macro — map to Unity equivalent
 * -------------------------------------------------------------------------- */
#define ztest_test_fail()   TEST_FAIL_MESSAGE("explicit test failure")

/* --------------------------------------------------------------------------
 * Zephyr attribute stubs — no-op on host
 * -------------------------------------------------------------------------- */
#ifndef __maybe_unused
#  define __maybe_unused   __attribute__((unused))
#endif

#ifndef __unused
#  define __unused         __attribute__((unused))
#endif

#endif /* ZTEST_SHIM_H */
