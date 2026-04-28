// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: tests/runner/test_main_template.c
 *
 * PURPOSE: Host-side test runner entry point for the Unity unit test harness.
 *          Provides setUp/tearDown lifecycle hooks and the main() function
 *          that invokes each test function registered via RUN_TEST().
 *
 *          Architecture:
 *            tests/CMakeLists.txt compiles one test_*.c + unity.c + this file
 *            into a host-native executable per module under test. CTest then
 *            invokes each executable and checks its exit code.
 *
 *            test_<module>.c  ->  test_main.c  ->  unity.h macros
 *                                      |
 *                              production source files (compiled without Zephyr)
 *
 *          This design allows all 15 unit test files to run on a development
 *          host (CI runner) without a Zephyr SDK or hardware target present.
 *
 * USAGE:
 *   Each test_*.c file must:
 *     1. Include "unity.h".
 *     2. Define void setUp(void) and void tearDown(void).
 *     3. Define test functions with signature void test_<name>(void).
 *     4. Define void run_all_tests(void) which calls RUN_TEST() for each test.
 *
 *   This file provides main() which calls:
 *     UNITY_BEGIN() -> run_all_tests() -> UNITY_END() -> return exit code.
 *
 * SAFETY  : Test infrastructure only — not safety-assessed.
 *           Do NOT include in production firmware build.
 * STANDARD: MISRA C:2012 alignment intended (test code, advisory only).
 * =============================================================================
 */

#include "unity.h"
#include <stdio.h>
#include <stdlib.h>

/* --------------------------------------------------------------------------
 * Weak-linked lifecycle hooks
 *
 * These default implementations are no-ops. Each test_*.c file overrides
 * them (strong link) when module-specific setup/teardown is needed.
 * -------------------------------------------------------------------------- */

/**
 * @brief Called before each individual test function.
 *
 * Override in test_*.c to reset module state, clear mocks, or
 * (re-)initialize the module under test.
 *
 * Default: no-op.
 */
__attribute__((weak)) void setUp(void)
{
    /* No-op default. Override in individual test file. */
}

/**
 * @brief Called after each individual test function.
 *
 * Override in test_*.c to release resources, restore mocked state,
 * or verify no residual errors remain.
 *
 * Default: no-op.
 */
__attribute__((weak)) void tearDown(void)
{
    /* No-op default. Override in individual test file. */
}

/* --------------------------------------------------------------------------
 * Forward declaration of the test registration function
 *
 * Must be defined in the corresponding test_*.c file.
 * -------------------------------------------------------------------------- */

/**
 * @brief Called by main() to register and execute all tests in the module.
 *
 * Example implementation in test_uds_session.c:
 *
 *   void run_all_tests(void) {
 *       RUN_TEST(test_session_init_null_ctx);
 *       RUN_TEST(test_session_init_happy_path);
 *       RUN_TEST(test_session_transition_valid);
 *       ...
 *   }
 */
extern void run_all_tests(void);

/* --------------------------------------------------------------------------
 * Main entry point
 * -------------------------------------------------------------------------- */

/**
 * @brief Test executable entry point.
 *
 * Orchestrates the Unity test lifecycle:
 *   1. UNITY_BEGIN()     — reset counters
 *   2. run_all_tests()   — call RUN_TEST() for each test function
 *   3. UNITY_END()       — print summary to stdout
 *   4. return exit code  — 0 on all pass, 1 on any failure (CTest convention)
 *
 * @return EXIT_SUCCESS (0) if all tests pass.
 * @return EXIT_FAILURE (1) if any test fails.
 */
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    UNITY_BEGIN();

    run_all_tests();

    UNITY_END();

    return (unity_tests_failed == 0U) ? EXIT_SUCCESS : EXIT_FAILURE;
}
