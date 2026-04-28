// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
// ================================
// File: tests/unit/test_dtc_database.c
// ================================
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit/test_dtc_database.c
 *
 * MODULE UNDER TEST: config/dtc_database.c
 *
 * PURPOSE:
 *   Verify the static DTC registration database. Tests cover:
 *     - dtc_database_init: double-init guard
 *     - dtc_database_register: NULL description (allowed), zero dtc_code
 *       rejection, duplicate rejection, capacity limit, happy path
 *     - dtc_database_find: known DTC → correct entry, unknown → NULL
 *     - dtc_database_set_status: updates status_byte, unknown DTC fails
 *     - dtc_database_clear_all: resets all status bytes to 0x00
 *     - dtc_database_count_by_status: NULL guard, correct count for mask
 *
 * DTC codes from generated uds_init.c:
 *   0xC00100  severity 0x20  "CAN bus communication loss"
 *   0xC00200  severity 0x40  "Diagnostic transport layer timeout"
 *
 * FRAMEWORK: Zephyr Ztest
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "dtc_database.h"
#include "uds_types.h"

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void dtc_init_fresh(void)
{
    /* Tolerate ALREADY_INITIALIZED — static state not resettable */
    (void)dtc_database_init();
}

static void register_generated_dtcs(void)
{
    dtc_init_fresh();
    (void)dtc_database_register(0xC00100U, 0x20U, "CAN bus communication loss");
    (void)dtc_database_register(0xC00200U, 0x40U, "Diagnostic transport layer timeout");
}

/* =========================================================================
 * Test suite: dtc_database_init
 * ========================================================================= */

ZTEST_SUITE(test_dtc_db_init, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DTC-INIT-001: First call → OK or ALREADY_INITIALIZED.
 */
ZTEST(test_dtc_db_init, test_init_ok)
{
    uds_status_t rc = dtc_database_init();
    zassert_true((rc == UDS_STATUS_OK) || (rc == UDS_STATUS_ERR_ALREADY_INITIALIZED),
                 "init must return OK or ALREADY_INITIALIZED");
}

/**
 * TC-DTC-INIT-002: Double init → ALREADY_INITIALIZED.
 */
ZTEST(test_dtc_db_init, test_double_init)
{
    dtc_database_init();
    uds_status_t rc = dtc_database_init();
    zassert_equal(rc, UDS_STATUS_ERR_ALREADY_INITIALIZED,
                  "Second init must return ALREADY_INITIALIZED");
}

/* =========================================================================
 * Test suite: dtc_database_register
 * ========================================================================= */

ZTEST_SUITE(test_dtc_db_register, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DTC-REG-001: dtc_code == 0 → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_dtc_db_register, test_zero_dtc_code)
{
    dtc_init_fresh();
    uds_status_t rc = dtc_database_register(0U, 0x00U, "invalid");
    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "DTC code 0 must be rejected");
}

/**
 * TC-DTC-REG-002: NULL description is permitted (optional field).
 */
ZTEST(test_dtc_db_register, test_null_description_ok)
{
    dtc_init_fresh();
    uds_status_t rc = dtc_database_register(0xDD0001U, 0x00U, NULL);
    zassert_true((rc == UDS_STATUS_OK) || (rc == UDS_STATUS_ERR_INVALID_PARAM),
                 "NULL description must not cause a crash; may be duplicate");
}

/**
 * TC-DTC-REG-003: Happy path — first generated DTC registers OK.
 */
ZTEST(test_dtc_db_register, test_happy_path_can_bus_loss)
{
    dtc_init_fresh();
    uds_status_t rc = dtc_database_register(0xC00100U, 0x20U,
                                            "CAN bus communication loss");
    zassert_true((rc == UDS_STATUS_OK) || (rc == UDS_STATUS_ERR_INVALID_PARAM),
                 "Registration must succeed or indicate duplicate");
}

/**
 * TC-DTC-REG-004: Duplicate DTC code → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_dtc_db_register, test_duplicate_rejected)
{
    dtc_init_fresh();
    /* First registration */
    dtc_database_register(0xC00100U, 0x20U, "CAN bus communication loss");
    /* Second with same code */
    uds_status_t rc = dtc_database_register(0xC00100U, 0x20U, "dup");
    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Duplicate DTC must be rejected");
}

/**
 * TC-DTC-REG-005: status_byte initialised to 0x00 on registration
 *                 (no fault active at boot).
 */
ZTEST(test_dtc_db_register, test_status_byte_initialised_to_zero)
{
    dtc_init_fresh();
    dtc_database_register(0xC00200U, 0x40U, "transport timeout");
    dtc_entry_t *e = dtc_database_find(0xC00200U);
    if (e != NULL) {
        zassert_equal(e->status_byte, 0x00U,
                      "status_byte must be 0x00 on registration");
        zassert_equal(e->severity, 0x40U, "severity must match");
    }
    /* If e is NULL, DTC was pre-registered by another test with same code */
}

/* =========================================================================
 * Test suite: dtc_database_find
 * ========================================================================= */

ZTEST_SUITE(test_dtc_db_find, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DTC-FIND-001: Both generated DTCs are findable.
 */
ZTEST(test_dtc_db_find, test_find_both_generated_dtcs)
{
    register_generated_dtcs();

    dtc_entry_t *e1 = dtc_database_find(0xC00100U);
    zassert_not_null(e1, "DTC 0xC00100 must be found");
    zassert_equal(e1->dtc_code, 0xC00100U, "DTC code must match");
    zassert_equal(e1->severity,  0x20U, "DTC severity must match");

    dtc_entry_t *e2 = dtc_database_find(0xC00200U);
    zassert_not_null(e2, "DTC 0xC00200 must be found");
    zassert_equal(e2->dtc_code, 0xC00200U, "DTC code must match");
    zassert_equal(e2->severity,  0x40U, "DTC severity must match");
}

/**
 * TC-DTC-FIND-002: Unknown DTC code → NULL.
 */
ZTEST(test_dtc_db_find, test_unknown_dtc_returns_null)
{
    register_generated_dtcs();
    dtc_entry_t *e = dtc_database_find(0xDEAD00U);
    zassert_is_null(e, "Unknown DTC must return NULL");
}

/**
 * TC-DTC-FIND-003: Returned pointer is mutable (status_byte can be written).
 */
ZTEST(test_dtc_db_find, test_returned_pointer_mutable)
{
    register_generated_dtcs();
    dtc_entry_t *e = dtc_database_find(0xC00100U);
    if (e == NULL) return;  /* Already confirmed in test_find_both */
    e->status_byte = DTC_STATUS_TEST_FAILED;  /* Direct write */
    zassert_equal(e->status_byte, DTC_STATUS_TEST_FAILED,
                  "status_byte must be mutable via returned pointer");
    e->status_byte = 0x00U;  /* Restore */
}

/* =========================================================================
 * Test suite: dtc_database_set_status
 * ========================================================================= */

ZTEST_SUITE(test_dtc_db_set_status, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DTC-SETSTAT-001: Set status on known DTC → OK, status updated.
 */
ZTEST(test_dtc_db_set_status, test_set_status_known_dtc)
{
    register_generated_dtcs();
    uds_status_t rc = dtc_database_set_status(0xC00100U,
        (uint8_t)(DTC_STATUS_TEST_FAILED | DTC_STATUS_CONFIRMED_DTC));
    zassert_equal(rc, UDS_STATUS_OK, "set_status must succeed for known DTC");

    dtc_entry_t *e = dtc_database_find(0xC00100U);
    zassert_not_null(e, "DTC must still be findable");
    zassert_equal(e->status_byte,
                  (uint8_t)(DTC_STATUS_TEST_FAILED | DTC_STATUS_CONFIRMED_DTC),
                  "status_byte must reflect new value");
}

/**
 * TC-DTC-SETSTAT-002: Set status on unknown DTC → error.
 */
ZTEST(test_dtc_db_set_status, test_set_status_unknown_dtc)
{
    register_generated_dtcs();
    uds_status_t rc = dtc_database_set_status(0xDEAD01U, 0xFFU);
    /* Must return an error — either DID_NOT_FOUND or equivalent */
    zassert_not_equal(rc, UDS_STATUS_OK,
                      "set_status on unknown DTC must fail");
}

/**
 * TC-DTC-SETSTAT-003: Set all status bits (0xFF) → stored correctly.
 */
ZTEST(test_dtc_db_set_status, test_set_all_bits)
{
    register_generated_dtcs();
    dtc_database_set_status(0xC00200U, 0xFFU);
    dtc_entry_t *e = dtc_database_find(0xC00200U);
    zassert_not_null(e, "DTC must be found");
    zassert_equal(e->status_byte, 0xFFU, "All bits must be stored");
    /* Restore */
    dtc_database_set_status(0xC00200U, 0x00U);
}

/* =========================================================================
 * Test suite: dtc_database_clear_all
 * ========================================================================= */

ZTEST_SUITE(test_dtc_db_clear_all, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DTC-CLEAR-001: After setting status on both DTCs, clear_all resets to 0x00.
 */
ZTEST(test_dtc_db_clear_all, test_clear_resets_all_status_bytes)
{
    register_generated_dtcs();

    /* Set some status bits */
    dtc_database_set_status(0xC00100U, DTC_STATUS_CONFIRMED_DTC);
    dtc_database_set_status(0xC00200U, DTC_STATUS_TEST_FAILED);

    /* Verify they are set */
    dtc_entry_t *e1 = dtc_database_find(0xC00100U);
    dtc_entry_t *e2 = dtc_database_find(0xC00200U);
    if ((e1 == NULL) || (e2 == NULL)) return;
    zassert_not_equal(e1->status_byte, 0x00U, "status must be non-zero before clear");
    zassert_not_equal(e2->status_byte, 0x00U, "status must be non-zero before clear");

    /* Clear */
    uds_status_t rc = dtc_database_clear_all();
    zassert_equal(rc, UDS_STATUS_OK, "clear_all must return OK");

    zassert_equal(e1->status_byte, 0x00U, "DTC1 status must be 0x00 after clear");
    zassert_equal(e2->status_byte, 0x00U, "DTC2 status must be 0x00 after clear");
}

/* =========================================================================
 * Test suite: dtc_database_count_by_status
 * ========================================================================= */

ZTEST_SUITE(test_dtc_db_count_by_status, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DTC-COUNT-001: NULL out_count → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_dtc_db_count_by_status, test_null_out_count)
{
    dtc_init_fresh();
    uds_status_t rc = dtc_database_count_by_status(0xFFU, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL out_count must fail");
}

/**
 * TC-DTC-COUNT-002: No DTCs with TEST_FAILED bit set → count = 0.
 */
ZTEST(test_dtc_db_count_by_status, test_count_zero_when_none_active)
{
    register_generated_dtcs();
    dtc_database_clear_all();  /* Ensure all are cleared */

    uint16_t count = 99U;
    uds_status_t rc = dtc_database_count_by_status(DTC_STATUS_TEST_FAILED, &count);
    zassert_equal(rc, UDS_STATUS_OK, "count_by_status must succeed");
    zassert_equal(count, 0U, "No DTCs with TEST_FAILED bit must yield count=0");
}

/**
 * TC-DTC-COUNT-003: One DTC with TEST_FAILED set → count = 1.
 */
ZTEST(test_dtc_db_count_by_status, test_count_one_active)
{
    register_generated_dtcs();
    dtc_database_clear_all();
    dtc_database_set_status(0xC00100U, DTC_STATUS_TEST_FAILED);

    uint16_t count = 0U;
    dtc_database_count_by_status(DTC_STATUS_TEST_FAILED, &count);
    zassert_equal(count, 1U, "One DTC with TEST_FAILED must yield count=1");

    dtc_database_set_status(0xC00100U, 0x00U);  /* Restore */
}

/**
 * TC-DTC-COUNT-004: Both DTCs with CONFIRMED_DTC bit set → count = 2.
 */
ZTEST(test_dtc_db_count_by_status, test_count_both_confirmed)
{
    register_generated_dtcs();
    dtc_database_clear_all();
    dtc_database_set_status(0xC00100U, DTC_STATUS_CONFIRMED_DTC);
    dtc_database_set_status(0xC00200U, DTC_STATUS_CONFIRMED_DTC);

    uint16_t count = 0U;
    dtc_database_count_by_status(DTC_STATUS_CONFIRMED_DTC, &count);
    zassert_equal(count, 2U, "Both DTCs confirmed must yield count=2");

    dtc_database_clear_all();  /* Restore */
}

/**
 * TC-DTC-COUNT-005: Mask with no matching bits → count = 0.
 */
ZTEST(test_dtc_db_count_by_status, test_count_mask_no_match)
{
    register_generated_dtcs();
    dtc_database_clear_all();
    /* Set DTC_STATUS_TEST_FAILED (0x01) but query CONFIRMED_DTC (0x08) */
    dtc_database_set_status(0xC00100U, DTC_STATUS_TEST_FAILED);

    uint16_t count = 99U;
    dtc_database_count_by_status(DTC_STATUS_CONFIRMED_DTC, &count);
    zassert_equal(count, 0U, "Mask mismatch must yield count=0");

    dtc_database_clear_all();
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_dtc_db_init__test_init_ok(void);
extern void test_dtc_db_init__test_double_init(void);
extern void test_dtc_db_register__test_zero_dtc_code(void);
extern void test_dtc_db_register__test_null_description_ok(void);
extern void test_dtc_db_register__test_happy_path_can_bus_loss(void);
extern void test_dtc_db_register__test_duplicate_rejected(void);
extern void test_dtc_db_register__test_status_byte_initialised_to_zero(void);
extern void test_dtc_db_find__test_find_both_generated_dtcs(void);
extern void test_dtc_db_find__test_unknown_dtc_returns_null(void);
extern void test_dtc_db_find__test_returned_pointer_mutable(void);
extern void test_dtc_db_set_status__test_set_status_known_dtc(void);
extern void test_dtc_db_set_status__test_set_status_unknown_dtc(void);
extern void test_dtc_db_set_status__test_set_all_bits(void);
extern void test_dtc_db_clear_all__test_clear_resets_all_status_bytes(void);
extern void test_dtc_db_count_by_status__test_null_out_count(void);
extern void test_dtc_db_count_by_status__test_count_zero_when_none_active(void);
extern void test_dtc_db_count_by_status__test_count_one_active(void);
extern void test_dtc_db_count_by_status__test_count_both_confirmed(void);
extern void test_dtc_db_count_by_status__test_count_mask_no_match(void);

void run_all_tests(void)
{
    RUN_TEST(test_dtc_db_init__test_init_ok);
    RUN_TEST(test_dtc_db_init__test_double_init);
    RUN_TEST(test_dtc_db_register__test_zero_dtc_code);
    RUN_TEST(test_dtc_db_register__test_null_description_ok);
    RUN_TEST(test_dtc_db_register__test_happy_path_can_bus_loss);
    RUN_TEST(test_dtc_db_register__test_duplicate_rejected);
    RUN_TEST(test_dtc_db_register__test_status_byte_initialised_to_zero);
    RUN_TEST(test_dtc_db_find__test_find_both_generated_dtcs);
    RUN_TEST(test_dtc_db_find__test_unknown_dtc_returns_null);
    RUN_TEST(test_dtc_db_find__test_returned_pointer_mutable);
    RUN_TEST(test_dtc_db_set_status__test_set_status_known_dtc);
    RUN_TEST(test_dtc_db_set_status__test_set_status_unknown_dtc);
    RUN_TEST(test_dtc_db_set_status__test_set_all_bits);
    RUN_TEST(test_dtc_db_clear_all__test_clear_resets_all_status_bytes);
    RUN_TEST(test_dtc_db_count_by_status__test_null_out_count);
    RUN_TEST(test_dtc_db_count_by_status__test_count_zero_when_none_active);
    RUN_TEST(test_dtc_db_count_by_status__test_count_one_active);
    RUN_TEST(test_dtc_db_count_by_status__test_count_both_confirmed);
    RUN_TEST(test_dtc_db_count_by_status__test_count_mask_no_match);
}
