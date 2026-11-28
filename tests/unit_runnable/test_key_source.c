// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_key_source.c
 *
 * MODULE UNDER TEST: platform/eds_key_source.h — eds_keys_install(), the key
 *                    installation behind CONFIG_EDS_KEYS_APP / _OTP (EDS#56).
 *
 * PURPOSE:
 *   A mocked key source stands in for OTP and for the application's function.
 *   The test checks that a good key reaches the SecurityAccess algorithm (the
 *   response to a seed is AES-CMAC under THAT key, not the placeholder), that
 *   an all-zero, all-0xFF or unreadable key is refused, that a refusal locks
 *   every level (a refused level must not fall back to the public placeholder
 *   key) and that the caller's buffer is wiped.
 *
 * FRAMEWORK: Zephyr Ztest (shim used for host builds — see tests/runner/)
 * BUILT BY : build_tests.sh, tests/CMakeLists.txt
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <stdint.h>
#include <string.h>

#include "eds_key_source.h"
#include "uds_aes_cmac.h"

static uint8_t g_otp_l1[EDS_KEY_LEN];
static uint8_t g_otp_l3[EDS_KEY_LEN];
static int     g_fail_level;     /* level for which read() fails, 0 = none */
static const uint8_t *g_scratch_seen_during_read;

static const uint8_t SEED[UDS_ALGO_SEED_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};

static int mock_read(uint8_t level, uint8_t *key, void *ctx)
{
    (void)ctx;
    g_scratch_seen_during_read = key;
    if ((int)level == g_fail_level) {
        return -5;
    }
    memcpy(key, (level == 0x01U) ? g_otp_l1 : g_otp_l3, EDS_KEY_LEN);
    return 0;
}

static void arm(void)
{
    uint8_t i;
    uds_security_algo_reset();
    g_fail_level = 0;
    for (i = 0U; i < EDS_KEY_LEN; i++) {
        g_otp_l1[i] = (uint8_t)(0xC0U + i);
        g_otp_l3[i] = (uint8_t)(0x30U + (3U * i));
    }
}

/* The 4-byte key the algorithm expects for `seed` at `level`. */
static void expected_key(uint8_t level, uint8_t out[UDS_ALGO_KEY_LEN])
{
    TEST_ASSERT_EQUAL_INT(UDS_STATUS_OK, uds_security_algo_derive_key(level, SEED, out));
}

static void cmac_of(const uint8_t key[EDS_KEY_LEN], uint8_t out[UDS_ALGO_KEY_LEN])
{
    uint8_t mac[UDS_CMAC_TAG_LEN];
    TEST_ASSERT_EQUAL_INT(0, uds_aes_cmac(key, SEED, UDS_ALGO_SEED_LEN, mac));
    memcpy(out, mac, UDS_ALGO_KEY_LEN);
}

ZTEST(key_source_suite, test_good_keys_reach_the_algorithm)
{
    eds_key_scratch_t w;
    eds_key_state_t st[EDS_KEY_LEVELS];
    uint8_t got[UDS_ALGO_KEY_LEN], want[UDS_ALGO_KEY_LEN];

    arm();
    TEST_ASSERT_TRUE(eds_keys_install(mock_read, NULL, &w, st));
    TEST_ASSERT_EQUAL_INT(EDS_KEY_INSTALLED, st[0]);
    TEST_ASSERT_EQUAL_INT(EDS_KEY_INSTALLED, st[1]);
    TEST_ASSERT_FALSE(uds_security_algo_keys_are_placeholder());

    expected_key(0x01U, got);
    cmac_of(g_otp_l1, want);
    TEST_ASSERT_EQUAL_INT(0, memcmp(got, want, UDS_ALGO_KEY_LEN));
    expected_key(0x03U, got);
    cmac_of(g_otp_l3, want);
    TEST_ASSERT_EQUAL_INT(0, memcmp(got, want, UDS_ALGO_KEY_LEN));
}

ZTEST(key_source_suite, test_scratch_is_wiped)
{
    eds_key_scratch_t w;
    eds_key_state_t st[EDS_KEY_LEVELS];
    uint8_t zero[EDS_KEY_LEN] = {0};

    arm();
    memset(&w, 0x77, sizeof(w));
    (void)eds_keys_install(mock_read, NULL, &w, st);
    TEST_ASSERT_TRUE(g_scratch_seen_during_read == w.key);
    TEST_ASSERT_EQUAL_INT(0, memcmp(w.key, zero, EDS_KEY_LEN));
}

ZTEST(key_source_suite, test_scratch_is_wiped_after_a_refusal_too)
{
    eds_key_scratch_t w;
    eds_key_state_t st[EDS_KEY_LEVELS];
    uint8_t zero[EDS_KEY_LEN] = {0};

    arm();
    memset(g_otp_l3, 0xFF, EDS_KEY_LEN);
    TEST_ASSERT_FALSE(eds_keys_install(mock_read, NULL, &w, st));
    TEST_ASSERT_EQUAL_INT(0, memcmp(w.key, zero, EDS_KEY_LEN));
}

ZTEST(key_source_suite, test_all_zero_key_is_refused_and_locks_every_level)
{
    eds_key_scratch_t w;
    eds_key_state_t st[EDS_KEY_LEVELS];
    uint8_t got[UDS_ALGO_KEY_LEN];

    arm();
    memset(g_otp_l3, 0x00, EDS_KEY_LEN);
    TEST_ASSERT_FALSE(eds_keys_install(mock_read, NULL, &w, st));
    TEST_ASSERT_EQUAL_INT(EDS_KEY_INSTALLED, st[0]);
    TEST_ASSERT_EQUAL_INT(EDS_KEY_ALL_ZERO, st[1]);
    /* Neither level derives a key, so no tester key can match. */
    TEST_ASSERT_NOT_EQUAL(UDS_STATUS_OK, uds_security_algo_derive_key(0x01U, SEED, got));
    TEST_ASSERT_NOT_EQUAL(UDS_STATUS_OK, uds_security_algo_derive_key(0x03U, SEED, got));
    /* The placeholder keys remain flagged, so a production init refuses to start. */
    TEST_ASSERT_TRUE(uds_security_algo_keys_are_placeholder());
}

ZTEST(key_source_suite, test_all_ff_key_is_refused)
{
    eds_key_scratch_t w;
    eds_key_state_t st[EDS_KEY_LEVELS];

    arm();
    memset(g_otp_l1, 0xFF, EDS_KEY_LEN);
    TEST_ASSERT_FALSE(eds_keys_install(mock_read, NULL, &w, st));
    TEST_ASSERT_EQUAL_INT(EDS_KEY_ALL_FF, st[0]);
    TEST_ASSERT_EQUAL_INT(EDS_KEY_INSTALLED, st[1]);
}

ZTEST(key_source_suite, test_read_error_is_refused)
{
    eds_key_scratch_t w;
    eds_key_state_t st[EDS_KEY_LEVELS];
    uint8_t got[UDS_ALGO_KEY_LEN];

    arm();
    g_fail_level = 0x03;
    TEST_ASSERT_FALSE(eds_keys_install(mock_read, NULL, &w, st));
    TEST_ASSERT_EQUAL_INT(EDS_KEY_READ_FAILED, st[1]);
    TEST_ASSERT_NOT_EQUAL(UDS_STATUS_OK, uds_security_algo_derive_key(0x01U, SEED, got));
}

/* One byte different from all-zero / all-0xFF is a usable key. */
ZTEST(key_source_suite, test_almost_all_zero_key_is_accepted)
{
    eds_key_scratch_t w;
    eds_key_state_t st[EDS_KEY_LEVELS];

    arm();
    memset(g_otp_l1, 0x00, EDS_KEY_LEN);
    g_otp_l1[15] = 0x01U;
    memset(g_otp_l3, 0xFF, EDS_KEY_LEN);
    g_otp_l3[0] = 0xFEU;
    TEST_ASSERT_TRUE(eds_keys_install(mock_read, NULL, &w, st));
}

ZTEST(key_source_suite, test_null_arguments)
{
    eds_key_scratch_t w;
    eds_key_state_t st[EDS_KEY_LEVELS];

    arm();
    TEST_ASSERT_FALSE(eds_keys_install(NULL, NULL, &w, st));
    TEST_ASSERT_FALSE(eds_keys_install(mock_read, NULL, NULL, st));
    TEST_ASSERT_FALSE(eds_keys_install(mock_read, NULL, &w, NULL));
}

/* ============================================================================
 * Unity runner infrastructure
 * ========================================================================= */

void setUp(void)    { }
void tearDown(void) { }

void run_all_tests(void)
{
    RUN_TEST(key_source_suite__test_good_keys_reach_the_algorithm);
    RUN_TEST(key_source_suite__test_scratch_is_wiped);
    RUN_TEST(key_source_suite__test_scratch_is_wiped_after_a_refusal_too);
    RUN_TEST(key_source_suite__test_all_zero_key_is_refused_and_locks_every_level);
    RUN_TEST(key_source_suite__test_all_ff_key_is_refused);
    RUN_TEST(key_source_suite__test_read_error_is_refused);
    RUN_TEST(key_source_suite__test_almost_all_zero_key_is_accepted);
    RUN_TEST(key_source_suite__test_null_arguments);
}
