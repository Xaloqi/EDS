// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_eds_elapsed.c
 *
 * MODULE UNDER TEST: core/eds_elapsed.h (EDS#411, EDS#412)
 *
 * PURPOSE:
 *   Elapsed time in place of counted stand-ins. The decisions on EDS#411
 *   this file pins down:
 *     - a stall is carried and drained in bounded steps, never dropped
 *       (dropping runs every timer long: the permissive direction for S3);
 *     - the carry saturates at EDS_ELAPSED_MAX_CARRY_MS, so a debugger halt
 *       is not replayed as a burst of ticks on resume;
 *     - everything is wrap-safe across the 32-bit clock.
 *
 * FRAMEWORK: Zephyr Ztest (shim used for host builds — see tests/runner/)
 * BUILT BY : build_tests.sh
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <stdint.h>
#include <stddef.h>

#include "eds_elapsed.h"

/* Upper bound on drain iterations, so a broken carry fails instead of
 * looping forever. */
#define DRAIN_GUARD (1000000U)

/* Call consume at a fixed now_ms until it hands out nothing; return the
 * total handed out. *calls receives the number of non-zero calls. */
static uint32_t drain(eds_elapsed_t *e, uint32_t now_ms, uint32_t cap_ms, uint32_t *calls)
{
    uint32_t total = 0U;
    uint32_t n = 0U;

    for (uint32_t i = 0U; i < DRAIN_GUARD; i++) {
        uint32_t got = eds_elapsed_consume(e, now_ms, cap_ms);
        if (got == 0U) {
            break;
        }
        if (got > cap_ms) {
            break; /* asserted by the caller through the totals */
        }
        total += got;
        n++;
    }
    *calls = n;
    return total;
}

/* --- eds_elapsed_since (EDS#412 deadline use) --------------------------- */

ZTEST(eds_elapsed_suite, test_since_measures_from_init_across_wrap)
{
    eds_elapsed_t e;

    eds_elapsed_init(&e, 0xFFFFFF00U);
    TEST_ASSERT_EQUAL_UINT32(0U,    eds_elapsed_since(&e, 0xFFFFFF00U));
    TEST_ASSERT_EQUAL_UINT32(0x200U, eds_elapsed_since(&e, 0x00000100U));
}

/* --- eds_elapsed_consume (EDS#411 tick-loop use) ------------------------ */

/* A loop that keeps up: each call hands out exactly the time since the
 * previous call. */
ZTEST(eds_elapsed_suite, test_consume_hands_out_elapsed_when_under_cap)
{
    eds_elapsed_t e;

    eds_elapsed_init(&e, 1000U);
    TEST_ASSERT_EQUAL_UINT32(1U, eds_elapsed_consume(&e, 1001U, 10U));
    TEST_ASSERT_EQUAL_UINT32(2U, eds_elapsed_consume(&e, 1003U, 10U));
    TEST_ASSERT_EQUAL_UINT32(0U, eds_elapsed_consume(&e, 1003U, 10U));
}

/* D2: a 1.5 s stall is handed out in cap-sized steps and none of it is
 * lost. An implementation that drops the excess hands out 10 ms here and
 * S3 runs 1.49 s long. */
ZTEST(eds_elapsed_suite, test_consume_carries_stall_without_losing_time)
{
    eds_elapsed_t e;
    uint32_t calls = 0U;

    eds_elapsed_init(&e, 0U);
    uint32_t total = drain(&e, 1500U, 10U, &calls);

    TEST_ASSERT_EQUAL_UINT32(1500U, total);   /* no time dropped (D2) */
    TEST_ASSERT_EQUAL_UINT32(150U, calls);    /* every step bounded by cap */
}

/* D5: a 5 min debugger halt is not replayed. At most
 * EDS_ELAPSED_MAX_CARRY_MS is handed out, then the clock runs normally. */
ZTEST(eds_elapsed_suite, test_consume_saturates_after_debugger_halt)
{
    eds_elapsed_t e;
    uint32_t calls = 0U;

    eds_elapsed_init(&e, 0U);
    uint32_t total = drain(&e, 300000U, 10U, &calls);

    TEST_ASSERT_EQUAL_UINT32(EDS_ELAPSED_MAX_CARRY_MS, total);
    TEST_ASSERT_EQUAL_UINT32(1U, eds_elapsed_consume(&e, 300001U, 10U));
}

/* Owed time accumulating across calls also saturates: a loop handing out
 * 1 ms per call against a clock running 2 ms per call cannot build an
 * unbounded backlog. */
ZTEST(eds_elapsed_suite, test_consume_backlog_is_bounded)
{
    eds_elapsed_t e;
    uint32_t now = 0U;

    eds_elapsed_init(&e, now);
    for (uint32_t i = 0U; i < 20000U; i++) {
        now += 2U;
        (void)eds_elapsed_consume(&e, now, 1U);
    }
    uint32_t calls = 0U;
    uint32_t backlog = drain(&e, now, 1U, &calls);

    TEST_ASSERT_TRUE(backlog <= EDS_ELAPSED_MAX_CARRY_MS);
}

ZTEST(eds_elapsed_suite, test_consume_across_clock_wrap)
{
    eds_elapsed_t e;

    eds_elapsed_init(&e, 0xFFFFFFF0U);
    TEST_ASSERT_EQUAL_UINT32(0x20U, eds_elapsed_consume(&e, 0x00000010U, 100U));
}

ZTEST(eds_elapsed_suite, test_null_is_safe)
{
    eds_elapsed_init(NULL, 5U);
    TEST_ASSERT_EQUAL_UINT32(0U, eds_elapsed_since(NULL, 5U));
    TEST_ASSERT_EQUAL_UINT32(0U, eds_elapsed_consume(NULL, 5U, 10U));
}

ZTEST_SUITE(eds_elapsed_suite, NULL, NULL, NULL, NULL, NULL);

/* =========================================================================
 * Unity runner infrastructure
 * ========================================================================= */

void setUp(void)    { }
void tearDown(void) { }

void run_all_tests(void)
{
    RUN_TEST(eds_elapsed_suite__test_since_measures_from_init_across_wrap);
    RUN_TEST(eds_elapsed_suite__test_consume_hands_out_elapsed_when_under_cap);
    RUN_TEST(eds_elapsed_suite__test_consume_carries_stall_without_losing_time);
    RUN_TEST(eds_elapsed_suite__test_consume_saturates_after_debugger_halt);
    RUN_TEST(eds_elapsed_suite__test_consume_backlog_is_bounded);
    RUN_TEST(eds_elapsed_suite__test_consume_across_clock_wrap);
    RUN_TEST(eds_elapsed_suite__test_null_is_safe);
}
