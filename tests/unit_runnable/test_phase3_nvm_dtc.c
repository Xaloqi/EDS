// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * TEST SUITE: test_phase3_nvm_dtc.c
 *
 * PURPOSE: Verify that DTC status bytes persist across simulated power-cycles
 *          via the dtc_mirror module.
 *
 * COVERS:
 *   dtc_mirror_init / dtc_mirror_load / dtc_mirror_flush_all
 *   dtc_mirror_save_one (triggers full flush)
 *   dtc_mirror_clear_all
 *   dtc_database_get_by_index
 *   dtc_database_get_count
 *   Status bytes restored correctly on re-init
 *   Entries absent from database (firmware update removes DTC) skipped
 *   DTC with status 0x00 NOT lost on round-trip
 *
 * Also covers issue #123 (DTC mirror could exceed NVM_MAX_RECORD_BYTES
 * before UDS_MAX_DTC_COUNT was reached):
 *   Filling to DTC_MIRROR_MAX_PERSISTED_DTCS (today's persisted-DTC cap)
 *     round-trips every entry through flush_all()/load().
 *   Filling dtc_database to its full UDS_MAX_DTC_COUNT (128) capacity still
 *     flushes OK (this is the regression check: on unmodified pre-#123
 *     source, this flush_all() call returns ERR_INVALID_PARAM instead of
 *     OK because the serialized record is 521 bytes against the 512-byte
 *     NVM cap); entries beyond the persisted-DTC cap are documented as not
 *     persisted, not corrupted.
 *
 * TEST COUNT: 15
 * =============================================================================
 */

#include "unity.h"
#include "dtc_mirror.h"
#include "dtc_database.h"
#include "nvm_store.h"
#include "uds_types.h"

#include <string.h>
#include <stdint.h>

extern void nvm_mock_reset(void);
extern void nvm_mock_deinit(void);
extern void dtc_database_test_reset(void);
extern void dtc_mirror_test_reset(void);

#define DTC_A  (0xC00100UL)
#define DTC_B  (0xC00200UL)
#define DTC_C  (0xC00300UL)

/* --------------------------------------------------------------------------
 * setUp / tearDown
 * -------------------------------------------------------------------------- */

void setUp(void)
{
    nvm_mock_reset();
    nvm_store_init(NULL);
    dtc_database_test_reset();
    dtc_database_init();
    dtc_mirror_test_reset();
    dtc_mirror_init();
}

void tearDown(void) {}

/* --------------------------------------------------------------------------
 * Helper: register the three test DTCs
 * -------------------------------------------------------------------------- */
static void register_three_dtcs(void)
{
    dtc_database_register(DTC_A, 0x00U, "DTC_A");
    dtc_database_register(DTC_B, 0x00U, "DTC_B");
    dtc_database_register(DTC_C, 0x00U, "DTC_C");
}

/* --------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------- */

/* 1. dtc_mirror_load on first boot (no NVM record) returns OK, status stays 0 */
void test_load_first_boot_no_change(void)
{
    register_three_dtcs();
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_load());

    /* All status bytes must remain 0 */
    dtc_entry_t *e = dtc_database_find(DTC_A);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(0x00U, e->status_byte);
}

/* 2. dtc_database_get_by_index returns entries in registration order */
void test_get_by_index_order(void)
{
    register_three_dtcs();

    uint32_t code; uint8_t status;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_database_get_by_index(0U, &code, &status));
    TEST_ASSERT_EQUAL_UINT32(DTC_A, code);

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_database_get_by_index(1U, &code, &status));
    TEST_ASSERT_EQUAL_UINT32(DTC_B, code);

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_database_get_by_index(2U, &code, &status));
    TEST_ASSERT_EQUAL_UINT32(DTC_C, code);

    /* Index 3 is past the end */
    TEST_ASSERT_EQUAL(UDS_STATUS_ERR_DID_NOT_FOUND,
        dtc_database_get_by_index(3U, &code, &status));
}

/* 3. dtc_database_get_count returns registered count */
void test_get_count(void)
{
    uint16_t count = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_database_get_count(&count));
    TEST_ASSERT_EQUAL_UINT16(0U, count);

    register_three_dtcs();
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_database_get_count(&count));
    TEST_ASSERT_EQUAL_UINT16(3U, count);
}

/* 4. dtc_mirror_flush_all serializes status bytes and write to NVM */
void test_flush_all_writes_nvm(void)
{
    register_three_dtcs();
    dtc_database_set_status(DTC_A, 0x09U); /* CONFIRMED | TEST_FAILED */
    dtc_database_set_status(DTC_B, 0x04U); /* PENDING */

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_flush_all());

    /* NVM record must exist now */
    uint8_t buf[32];
    size_t  len = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK,
        nvm_store_read(NVM_KEY_DTC_MIRROR, buf, sizeof(buf), &len));
    /* At minimum: 2-byte header + 3×4 = 14 bytes */
    TEST_ASSERT_GREATER_OR_EQUAL(14U, len);
}

/* 5. Status bytes survive power-cycle via flush + load */
void test_status_survives_power_cycle(void)
{
    /* Boot #1 */
    register_three_dtcs();
    dtc_database_set_status(DTC_A, 0x09U);
    dtc_database_set_status(DTC_B, 0x04U);
    dtc_database_set_status(DTC_C, 0x00U);
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_flush_all());

    /* Power-cycle: preserve NVM data */
    nvm_mock_deinit();
    nvm_store_init(NULL);
    dtc_database_test_reset();
    dtc_database_init();
    dtc_mirror_test_reset();
    dtc_mirror_init();

    register_three_dtcs(); /* re-register (done at stack init) */
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_load());

    TEST_ASSERT_EQUAL_UINT8(0x09U, dtc_database_find(DTC_A)->status_byte);
    TEST_ASSERT_EQUAL_UINT8(0x04U, dtc_database_find(DTC_B)->status_byte);
    TEST_ASSERT_EQUAL_UINT8(0x00U, dtc_database_find(DTC_C)->status_byte);
}

/* 6. dtc_mirror_save_one triggers a full flush */
void test_save_one_triggers_full_flush(void)
{
    register_three_dtcs();
    dtc_database_set_status(DTC_A, 0x0FU);

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_save_one(DTC_A, 0x0FU));

    /* Verify NVM was updated */
    uint8_t buf[64];
    size_t  len = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK,
        nvm_store_read(NVM_KEY_DTC_MIRROR, buf, sizeof(buf), &len));
    TEST_ASSERT_GREATER_THAN(0U, len);
}

/* 7. dtc_mirror_clear_all writes all-zero status bytes to NVM */
void test_clear_all_writes_zeros(void)
{
    register_three_dtcs();
    dtc_database_set_status(DTC_A, 0xFFU);
    dtc_database_set_status(DTC_B, 0xFFU);
    dtc_mirror_flush_all();

    /* Clear */
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_clear_all());

    /* Simulate power-cycle and reload */
    nvm_mock_deinit();
    nvm_store_init(NULL);
    dtc_database_test_reset();
    dtc_database_init();
    dtc_mirror_test_reset();
    dtc_mirror_init();
    register_three_dtcs();
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_load());

    TEST_ASSERT_EQUAL_UINT8(0x00U, dtc_database_find(DTC_A)->status_byte);
    TEST_ASSERT_EQUAL_UINT8(0x00U, dtc_database_find(DTC_B)->status_byte);
}

/* 8. DTC in NVM but not registered in database is silently skipped */
void test_load_skips_unregistered_dtc(void)
{
    /* Boot #1: register 3, set status, flush */
    register_three_dtcs();
    dtc_database_set_status(DTC_A, 0x09U);
    dtc_database_set_status(DTC_B, 0x04U);
    dtc_database_set_status(DTC_C, 0x08U);
    dtc_mirror_flush_all();

    /* Power-cycle: preserve NVM data, re-init all modules cleanly */
    nvm_mock_deinit();
    nvm_store_init(NULL);
    dtc_database_test_reset();
    dtc_database_init();
    dtc_mirror_test_reset();
    dtc_mirror_init();

    /* Boot #2: firmware update removed DTC_C — only register A and B */
    dtc_database_register(DTC_A, 0x00U, "DTC_A");
    dtc_database_register(DTC_B, 0x00U, "DTC_B");
    /* DTC_C not registered */

    /* Load must succeed without error */
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_load());

    /* A and B restored; DTC_C is simply absent — no crash */
    TEST_ASSERT_EQUAL_UINT8(0x09U, dtc_database_find(DTC_A)->status_byte);
    TEST_ASSERT_EQUAL_UINT8(0x04U, dtc_database_find(DTC_B)->status_byte);
    TEST_ASSERT_NULL(dtc_database_find(DTC_C));
}

/* 9. Calling flush_all with empty DTC table writes valid (zero-count) record */
void test_flush_empty_table(void)
{
    /* No DTCs registered */
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_flush_all());

    uint8_t buf[16];
    size_t  len = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK,
        nvm_store_read(NVM_KEY_DTC_MIRROR, buf, sizeof(buf), &len));
    /* Header(5) + no entries + crc(4) = 9 bytes, count = 0 */
    TEST_ASSERT_EQUAL_INT((int)9U, (int)len);
    TEST_ASSERT_EQUAL_UINT8(DTC_MIRROR_MAGIC_0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(DTC_MIRROR_MAGIC_1, buf[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)DTC_MIRROR_FORMAT_VERSION, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x00U, buf[3]); /* count high */
    TEST_ASSERT_EQUAL_UINT8(0x00U, buf[4]); /* count low  */
}

/* 10. dtc_mirror_init returns ERR_ALREADY_INITIALIZED on second call within same setUp */
void test_init_idempotent(void)
{
    /* setUp already called dtc_mirror_init() once — calling again must return error */
    TEST_ASSERT_EQUAL(UDS_STATUS_ERR_ALREADY_INITIALIZED, dtc_mirror_init());
}

/* 11. DTC status 0x00 round-trips correctly (not pruned during serialization) */
void test_zero_status_round_trips(void)
{
    register_three_dtcs();
    /* Leave all at 0x00 */
    dtc_mirror_flush_all();

    nvm_mock_deinit();
    nvm_store_init(NULL);
    dtc_database_test_reset();
    dtc_database_init();
    dtc_mirror_test_reset();
    dtc_mirror_init();
    register_three_dtcs();
    /* Set non-zero so we can confirm they were overwritten by load */
    dtc_database_set_status(DTC_A, 0xFFU);
    dtc_database_set_status(DTC_B, 0xFFU);
    dtc_database_set_status(DTC_C, 0xFFU);

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_load());

    TEST_ASSERT_EQUAL_UINT8(0x00U, dtc_database_find(DTC_A)->status_byte);
    TEST_ASSERT_EQUAL_UINT8(0x00U, dtc_database_find(DTC_B)->status_byte);
    TEST_ASSERT_EQUAL_UINT8(0x00U, dtc_database_find(DTC_C)->status_byte);
}

/* 12. dtc_database_get_by_index NULL pointer guard */
void test_get_by_index_null_guard(void)
{
    register_three_dtcs();
    uint32_t code;
    uint8_t  status;
    TEST_ASSERT_EQUAL(UDS_STATUS_ERR_NULL_PTR,
        dtc_database_get_by_index(0U, NULL, &status));
    TEST_ASSERT_EQUAL(UDS_STATUS_ERR_NULL_PTR,
        dtc_database_get_by_index(0U, &code, NULL));
}

/* 13. dtc_database_get_count NULL pointer guard */
void test_get_count_null_guard(void)
{
    TEST_ASSERT_EQUAL(UDS_STATUS_ERR_NULL_PTR, dtc_database_get_count(NULL));
}

/* --------------------------------------------------------------------------
 * Issue #123 — DTC mirror sizing vs. NVM_MAX_RECORD_BYTES
 * -------------------------------------------------------------------------- */

/*
 * Deliberately a LITERAL, not config/dtc_mirror.h's DTC_MIRROR_MAX_PERSISTED_DTCS:
 * these two tests must still compile against the unmodified pre-#123 source
 * (which does not define that constant) so the regression check below fails
 * at its runtime assertion — proving the actual behavioral bug — rather than
 * failing to build and masking what's really being tested. TC-MIRROR-028 in
 * test_dtc_mirror.c cross-checks this literal against the real
 * DTC_MIRROR_MAX_PERSISTED_DTCS constant, so drift between the two is caught.
 */
#define TEST_EXPECTED_MIRROR_PERSISTED_CAP ((uint16_t)125U)

/* Register `count` unique, nonzero DTC codes (0x010000 + i) with distinct
 * status bytes ((i % 0xFF) + 1, always nonzero so "not persisted" is
 * distinguishable from a genuine round-tripped 0x00 status). */
static void register_n_dtcs_with_status(uint16_t count)
{
    uint16_t i;

    for (i = 0U; i < count; i++) {
        uint32_t dtc_code = 0x010000UL + (uint32_t)i;
        uint8_t  status    = (uint8_t)((i % 0xFFU) + 1U);

        TEST_ASSERT_EQUAL(UDS_STATUS_OK,
            dtc_database_register(dtc_code, 0x00U, NULL));
        TEST_ASSERT_EQUAL(UDS_STATUS_OK,
            dtc_database_set_status(dtc_code, status));
    }
}

/* 14. Filling to exactly TEST_EXPECTED_MIRROR_PERSISTED_CAP (today's cap,
 * 125) must flush OK and every entry must round-trip through a
 * power-cycle. */
void test_flush_and_load_at_persisted_cap_round_trips_all(void)
{
    uint16_t i;

    register_n_dtcs_with_status(TEST_EXPECTED_MIRROR_PERSISTED_CAP);

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_flush_all());

    /* Power-cycle: preserve NVM data, re-init all modules cleanly. */
    nvm_mock_deinit();
    nvm_store_init(NULL);
    dtc_database_test_reset();
    dtc_database_init();
    dtc_mirror_test_reset();
    dtc_mirror_init();

    /* Re-register (as stack init would), then load. */
    for (i = 0U; i < TEST_EXPECTED_MIRROR_PERSISTED_CAP; i++) {
        uint32_t dtc_code = 0x010000UL + (uint32_t)i;
        TEST_ASSERT_EQUAL(UDS_STATUS_OK,
            dtc_database_register(dtc_code, 0x00U, NULL));
    }

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_load());

    for (i = 0U; i < TEST_EXPECTED_MIRROR_PERSISTED_CAP; i++) {
        uint32_t     dtc_code = 0x010000UL + (uint32_t)i;
        uint8_t      expected = (uint8_t)((i % 0xFFU) + 1U);
        dtc_entry_t *e        = dtc_database_find(dtc_code);

        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_UINT8(expected, e->status_byte);
    }
}

/* 15. THE REGRESSION TEST (issue #123): filling dtc_database to its full
 * UDS_MAX_DTC_COUNT (128) capacity — three more than the mirror's
 * persisted cap — must still flush OK.
 *
 * On unmodified pre-#123 source this assertion FAILS: mirror_serialize()
 * walks all 128 registered entries, producing a 521-byte record
 * (5 + 128*4 + 4), and nvm_store_write() rejects anything over
 * NVM_MAX_RECORD_BYTES (512) — dtc_mirror_flush_all() returns a non-OK
 * status instead of UDS_STATUS_OK. This is the exact issue #123 failure
 * mode: silent persistence loss at worst-case fault load.
 *
 * After the fix, the mirror only ever serializes the first
 * TEST_EXPECTED_MIRROR_PERSISTED_CAP entries (509 bytes) and the write
 * succeeds. Entries beyond the cap are a documented limitation, not a
 * crash or a truncated/corrupt write: after a power-cycle and reload,
 * DTCs within the cap round-trip exactly, and DTCs beyond it come back at
 * their power-on default (0x00) rather than the (nonzero) status set
 * before the flush — proving they were simply never written, not
 * partially/incorrectly written. */
void test_flush_at_full_dtc_database_capacity_still_succeeds(void)
{
    uint16_t i;

    TEST_ASSERT_EQUAL((uint16_t)128U, (uint16_t)UDS_MAX_DTC_COUNT);
    TEST_ASSERT_TRUE((uint16_t)UDS_MAX_DTC_COUNT > TEST_EXPECTED_MIRROR_PERSISTED_CAP);

    register_n_dtcs_with_status((uint16_t)UDS_MAX_DTC_COUNT);

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_flush_all());

    /* Power-cycle and reload. */
    nvm_mock_deinit();
    nvm_store_init(NULL);
    dtc_database_test_reset();
    dtc_database_init();
    dtc_mirror_test_reset();
    dtc_mirror_init();

    for (i = 0U; i < (uint16_t)UDS_MAX_DTC_COUNT; i++) {
        uint32_t dtc_code = 0x010000UL + (uint32_t)i;
        TEST_ASSERT_EQUAL(UDS_STATUS_OK,
            dtc_database_register(dtc_code, 0x00U, NULL));
    }

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, dtc_mirror_load());

    /* Within the persisted cap: exact round-trip. */
    for (i = 0U; i < TEST_EXPECTED_MIRROR_PERSISTED_CAP; i++) {
        uint32_t     dtc_code = 0x010000UL + (uint32_t)i;
        uint8_t      expected = (uint8_t)((i % 0xFFU) + 1U);
        dtc_entry_t *e        = dtc_database_find(dtc_code);

        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_UINT8(expected, e->status_byte);
    }

    /* Beyond the persisted cap: documented as not persisted -> default 0x00,
     * NOT the nonzero status that was set (and never written) before flush. */
    for (i = TEST_EXPECTED_MIRROR_PERSISTED_CAP; i < (uint16_t)UDS_MAX_DTC_COUNT; i++) {
        uint32_t     dtc_code = 0x010000UL + (uint32_t)i;
        dtc_entry_t *e        = dtc_database_find(dtc_code);

        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_UINT8(0x00U, e->status_byte);
    }
}

/* --------------------------------------------------------------------------
 * Test runner
 * -------------------------------------------------------------------------- */
void run_all_tests(void)
{
    RUN_TEST(test_load_first_boot_no_change);
    RUN_TEST(test_get_by_index_order);
    RUN_TEST(test_get_count);
    RUN_TEST(test_flush_all_writes_nvm);
    RUN_TEST(test_status_survives_power_cycle);
    RUN_TEST(test_save_one_triggers_full_flush);
    RUN_TEST(test_clear_all_writes_zeros);
    RUN_TEST(test_load_skips_unregistered_dtc);
    RUN_TEST(test_flush_empty_table);
    RUN_TEST(test_init_idempotent);
    RUN_TEST(test_zero_status_round_trips);
    RUN_TEST(test_get_by_index_null_guard);
    RUN_TEST(test_get_count_null_guard);
    RUN_TEST(test_flush_and_load_at_persisted_cap_round_trips_all);
    RUN_TEST(test_flush_at_full_dtc_database_capacity_still_succeeds);
}
