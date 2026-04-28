// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * TEST SUITE: test_phase3_nvm_integration.c
 *
 * PURPOSE: End-to-end NVM flush/restore cycle — exercises nvm_store_mock,
 *          dtc_mirror, uds_security_nvm, and uds_session_stats together
 *          through the simulated "flush before reset → power-cycle → reload"
 *          lifecycle.
 *
 * COVERS:
 *   uds_session_stats: init, on_change, record_security, flush, get
 *   Session stats survive power-cycle
 *   total_resets increments each init
 *   programming_session_count and extended_session_count increment correctly
 *   security_unlock_count and security_lockout_count increment correctly
 *   Dirty flag: flush writes only when changed
 *   Full pre-reset flush: DTC + security + session stats all written atomically
 *   NVM schema version record written and preserved across deinit/reinit
 *
 * TEST COUNT: 13
 * =============================================================================
 */

#include "unity.h"
#include "nvm_store.h"
#include "uds_session_stats.h"
#include "uds_security_nvm.h"
#include "dtc_mirror.h"
#include "dtc_database.h"
#include "uds_types.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

extern void nvm_mock_reset(void);
extern void nvm_mock_deinit(void);
extern void dtc_database_test_reset(void);
extern void dtc_mirror_test_reset(void);
extern void uds_session_stats_test_reset(void);

#define DTC_X  (0xAB0001UL)
#define DTC_Y  (0xAB0002UL)

/* --------------------------------------------------------------------------
 * setUp / tearDown
 * -------------------------------------------------------------------------- */

void setUp(void)
{
    nvm_mock_reset();
    nvm_store_init(NULL);
    dtc_database_test_reset();
    dtc_mirror_test_reset();
    uds_session_stats_test_reset();
}

void tearDown(void) {}

/* --------------------------------------------------------------------------
 * Helper: full simulated stack init (can be called multiple times in one test)
 * -------------------------------------------------------------------------- */
static void stack_init(void)
{
    dtc_database_test_reset();
    dtc_mirror_test_reset();
    uds_session_stats_test_reset();
    dtc_database_init();
    dtc_mirror_init();
    uds_session_stats_init();
}

/* --------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------- */

/* 1. uds_session_stats_init increments total_resets from 0 to 1 */
void test_stats_init_increments_reset_counter(void)
{
    uds_session_stats_init();

    nvm_session_stats_t stats;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_session_stats_get(&stats));
    TEST_ASSERT_EQUAL_UINT32(1U, stats.total_resets);
}

/* 2. total_resets increments each power-cycle */
void test_total_resets_accumulates(void)
{
    /* Boot 1: setUp already called nvm_mock_reset + nvm_store_init */
    uds_session_stats_init();
    uds_session_stats_flush();

    /* Boot 2: power-cycle preserving NVM data, re-init session stats */
    nvm_mock_deinit();
    nvm_store_init(NULL);
    uds_session_stats_test_reset();
    uds_session_stats_init();

    nvm_session_stats_t stats;
    uds_session_stats_get(&stats);
    TEST_ASSERT_EQUAL_UINT32(2U, stats.total_resets);

    /* Boot 3 */
    uds_session_stats_flush();
    nvm_mock_deinit();
    nvm_store_init(NULL);
    uds_session_stats_test_reset();
    uds_session_stats_init();
    uds_session_stats_get(&stats);
    TEST_ASSERT_EQUAL_UINT32(3U, stats.total_resets);
}

/* 3. programming_session_count increments on PROGRAMMING transition */
void test_programming_session_count(void)
{
    uds_session_stats_init();

    uds_session_stats_on_change(UDS_SESSION_DEFAULT, UDS_SESSION_PROGRAMMING);
    uds_session_stats_on_change(UDS_SESSION_PROGRAMMING, UDS_SESSION_DEFAULT);
    uds_session_stats_on_change(UDS_SESSION_DEFAULT, UDS_SESSION_PROGRAMMING);

    nvm_session_stats_t stats;
    uds_session_stats_get(&stats);
    TEST_ASSERT_EQUAL_UINT32(2U, stats.programming_session_count);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.extended_session_count);
}

/* 4. extended_session_count increments on EXTENDED transition */
void test_extended_session_count(void)
{
    uds_session_stats_init();

    uds_session_stats_on_change(UDS_SESSION_DEFAULT, UDS_SESSION_EXTENDED);

    nvm_session_stats_t stats;
    uds_session_stats_get(&stats);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.programming_session_count);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.extended_session_count);
}

/* 5. security_unlock_count increments on successful unlock */
void test_security_unlock_count(void)
{
    uds_session_stats_init();

    uds_session_stats_record_security(true);
    uds_session_stats_record_security(true);

    nvm_session_stats_t stats;
    uds_session_stats_get(&stats);
    TEST_ASSERT_EQUAL_UINT32(2U, stats.security_unlock_count);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.security_lockout_count);
}

/* 6. security_lockout_count increments on lockout */
void test_security_lockout_count(void)
{
    uds_session_stats_init();

    uds_session_stats_record_security(false);

    nvm_session_stats_t stats;
    uds_session_stats_get(&stats);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.security_unlock_count);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.security_lockout_count);
}

/* 7. Session stats survive power-cycle via flush + reload */
void test_session_stats_survive_power_cycle(void)
{
    uds_session_stats_init();
    uds_session_stats_on_change(UDS_SESSION_DEFAULT, UDS_SESSION_PROGRAMMING);
    uds_session_stats_on_change(UDS_SESSION_DEFAULT, UDS_SESSION_EXTENDED);
    uds_session_stats_record_security(true);
    uds_session_stats_flush();

    /* Power-cycle: preserve NVM */
    nvm_mock_deinit();
    nvm_store_init(NULL);
    uds_session_stats_test_reset();
    uds_session_stats_init(); /* second boot — total_resets becomes 2 */

    nvm_session_stats_t stats;
    uds_session_stats_get(&stats);
    TEST_ASSERT_EQUAL_UINT32(2U,  stats.total_resets);
    TEST_ASSERT_EQUAL_UINT32(1U,  stats.programming_session_count);
    TEST_ASSERT_EQUAL_UINT32(1U,  stats.extended_session_count);
    TEST_ASSERT_EQUAL_UINT32(1U,  stats.security_unlock_count);
    TEST_ASSERT_EQUAL_UINT32(0U,  stats.security_lockout_count);
}

/* 8. Flush when not dirty is a no-op (returns OK without writing) */
void test_flush_noop_when_not_dirty(void)
{
    uds_session_stats_init();
    /* First flush marks as clean */
    uds_session_stats_flush();

    /* No changes made — second flush should return OK without NVM write */
    /* We can't easily detect "no write happened" without instrumentation,
     * so we just verify it returns OK and doesn't corrupt the record. */
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_session_stats_flush());

    nvm_session_stats_t stats;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_session_stats_get(&stats));
    TEST_ASSERT_EQUAL_UINT32(1U, stats.total_resets);
}

/* 9. uds_session_stats_get NULL pointer guard */
void test_stats_get_null_guard(void)
{
    uds_session_stats_init();
    TEST_ASSERT_EQUAL(UDS_STATUS_ERR_NULL_PTR, uds_session_stats_get(NULL));
}

/* 10. uds_session_stats_init returns ERR_ALREADY_INITIALIZED on second call */
void test_stats_init_idempotent(void)
{
    uds_session_stats_init();
    TEST_ASSERT_EQUAL(UDS_STATUS_ERR_ALREADY_INITIALIZED, uds_session_stats_init());
}

/* 11. NVM schema version is written on first init */
void test_schema_version_written_on_init(void)
{
    /* nvm_store_init already called in setUp (which writes schema version) */
    uint16_t version = 0U;
    size_t   len     = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK,
        nvm_store_read(NVM_KEY_SCHEMA_VERSION, &version, sizeof(version), &len));
    TEST_ASSERT_EQUAL_UINT16(NVM_SCHEMA_VERSION_CURRENT, version);
    TEST_ASSERT_EQUAL_INT((int)sizeof(uint16_t), (int)len);
}

/* 12. Schema version persists across deinit/reinit */
void test_schema_version_survives_reinit(void)
{
    nvm_mock_deinit();
    nvm_store_init(NULL);

    uint16_t version = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK,
        nvm_store_read(NVM_KEY_SCHEMA_VERSION, &version, sizeof(version), NULL));
    TEST_ASSERT_EQUAL_UINT16(NVM_SCHEMA_VERSION_CURRENT, version);
}

/* 13. Full pre-reset flush: DTC + security + stats all written in sequence */
void test_full_pre_reset_flush(void)
{
    /* Stack init */
    stack_init();

    /* Register DTCs and set some faults */
    dtc_database_register(DTC_X, 0x00U, "DTC_X");
    dtc_database_register(DTC_Y, 0x00U, "DTC_Y");
    dtc_database_set_status(DTC_X, 0x09U);
    dtc_database_set_status(DTC_Y, 0x04U);

    /* Security: persist 1 failed attempt */
    uint8_t  attempts = 1U;
    uint32_t lockout  = 0U;
    uds_security_nvm_save(attempts, lockout);

    /* Session events */
    uds_session_stats_on_change(UDS_SESSION_DEFAULT, UDS_SESSION_PROGRAMMING);

    /* Simulate pre-reset flush (what zephyr_port_nvm_flush does) */
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_flush_all());
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_session_stats_flush());
    /* Security is already current in NVM from eager write above */

    /* Power-cycle */
    nvm_mock_deinit();
    nvm_store_init(NULL);
    stack_init();

    /* Re-register and reload DTC mirror */
    dtc_database_register(DTC_X, 0x00U, "DTC_X");
    dtc_database_register(DTC_Y, 0x00U, "DTC_Y");
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_load());

    /* Verify DTC status bytes restored */
    TEST_ASSERT_EQUAL_UINT8(0x09U, dtc_database_find(DTC_X)->status_byte);
    TEST_ASSERT_EQUAL_UINT8(0x04U, dtc_database_find(DTC_Y)->status_byte);

    /* Verify security counter restored */
    uint8_t  out_a = 0U;
    uint32_t out_l = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_security_nvm_load(&out_a, &out_l));
    TEST_ASSERT_EQUAL_UINT8(1U, out_a);
    TEST_ASSERT_EQUAL_UINT32(0U, out_l);

    /* Verify session stats: total_resets = 2 (1 per init call), prog count = 1 */
    nvm_session_stats_t stats;
    uds_session_stats_get(&stats);
    TEST_ASSERT_EQUAL_UINT32(2U, stats.total_resets);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.programming_session_count);
}

/* --------------------------------------------------------------------------
 * Test runner
 * -------------------------------------------------------------------------- */
void run_all_tests(void)
{
    RUN_TEST(test_stats_init_increments_reset_counter);
    RUN_TEST(test_total_resets_accumulates);
    RUN_TEST(test_programming_session_count);
    RUN_TEST(test_extended_session_count);
    RUN_TEST(test_security_unlock_count);
    RUN_TEST(test_security_lockout_count);
    RUN_TEST(test_session_stats_survive_power_cycle);
    RUN_TEST(test_flush_noop_when_not_dirty);
    RUN_TEST(test_stats_get_null_guard);
    RUN_TEST(test_stats_init_idempotent);
    RUN_TEST(test_schema_version_written_on_init);
    RUN_TEST(test_schema_version_survives_reinit);
    RUN_TEST(test_full_pre_reset_flush);
}
