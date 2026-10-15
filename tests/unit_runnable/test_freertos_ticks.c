// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_freertos_ticks.c
 *
 * MODULE UNDER TEST: platform/freertos/freertos_ticks.h —
 *                    eds_freertos_ticks_to_ms(), the conversion behind the
 *                    FreeRTOS eds_platform_uptime_ms() (EDS#418).
 *
 * PURPOSE:
 *   The clock must be exact at any configTICK_RATE_HZ. The old
 *   ticks * portTICK_PERIOD_MS form was exact only when 1000 is a multiple
 *   of the rate: at 300 Hz it under-reported by ~10%, and above 1 kHz it
 *   returned 0 forever.
 *
 * FRAMEWORK: Zephyr Ztest (shim used for host builds — see tests/runner/)
 * BUILT BY : build_tests.sh
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <stdint.h>

#include "freertos/freertos_ticks.h"

/* 1 kHz, the shipped rate: one tick is one ms, unchanged by EDS#418. */
ZTEST(freertos_ticks_suite, test_ticks_to_ms_1000hz_unchanged)
{
    TEST_ASSERT_EQUAL_UINT32(0U,     eds_freertos_ticks_to_ms(0U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(5000U,  eds_freertos_ticks_to_ms(5000U, 1000U));
    /* Wraps with the tick counter, continuously. */
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFU, eds_freertos_ticks_to_ms(0xFFFFFFFFU, 1000U));
}

/* 100 Hz: 1000 is a multiple of the rate, so the old form was exact too. */
ZTEST(freertos_ticks_suite, test_ticks_to_ms_100hz)
{
    TEST_ASSERT_EQUAL_UINT32(5000U, eds_freertos_ticks_to_ms(500U, 100U));
}

/* 300 Hz: 1500 ticks are 5 s. The old form reported 4.5 s, so a 5 s S3
 * timeout ran ~5.6 s. */
ZTEST(freertos_ticks_suite, test_ticks_to_ms_300hz_not_truncated)
{
    TEST_ASSERT_EQUAL_UINT32(5000U, eds_freertos_ticks_to_ms(1500U, 300U));
    /* 1 tick = 3.33 ms, truncated down, never rounded up. */
    TEST_ASSERT_EQUAL_UINT32(3U, eds_freertos_ticks_to_ms(1U, 300U));
}

/* 2 kHz: the old portTICK_PERIOD_MS was 0, so the clock never advanced and
 * no UDS timer ever expired. */
ZTEST(freertos_ticks_suite, test_ticks_to_ms_2000hz_advances)
{
    TEST_ASSERT_EQUAL_UINT32(5000U, eds_freertos_ticks_to_ms(10000U, 2000U));
    TEST_ASSERT_EQUAL_UINT32(1U,    eds_freertos_ticks_to_ms(2U, 2000U));
}

/* No 32-bit overflow in the intermediate product: at 300 Hz, 2^32 - 1
 * ticks x 1000 needs 42 bits. */
ZTEST(freertos_ticks_suite, test_ticks_to_ms_no_intermediate_overflow)
{
    /* (2^32 - 1) * 1000 / 300 = 14316557650, mod 2^32 = 1431655762. */
    TEST_ASSERT_EQUAL_UINT32(1431655762U, eds_freertos_ticks_to_ms(0xFFFFFFFFU, 300U));
}

/* A zero rate is a misconfiguration; never divide by it. */
ZTEST(freertos_ticks_suite, test_ticks_to_ms_zero_rate_is_safe)
{
    TEST_ASSERT_EQUAL_UINT32(0U, eds_freertos_ticks_to_ms(1234U, 0U));
}

ZTEST_SUITE(freertos_ticks_suite, NULL, NULL, NULL, NULL, NULL);

/* =========================================================================
 * Unity runner infrastructure
 * ========================================================================= */

void setUp(void)    { }
void tearDown(void) { }

void run_all_tests(void)
{
    RUN_TEST(freertos_ticks_suite__test_ticks_to_ms_1000hz_unchanged);
    RUN_TEST(freertos_ticks_suite__test_ticks_to_ms_100hz);
    RUN_TEST(freertos_ticks_suite__test_ticks_to_ms_300hz_not_truncated);
    RUN_TEST(freertos_ticks_suite__test_ticks_to_ms_2000hz_advances);
    RUN_TEST(freertos_ticks_suite__test_ticks_to_ms_no_intermediate_overflow);
    RUN_TEST(freertos_ticks_suite__test_ticks_to_ms_zero_rate_is_safe);
}
