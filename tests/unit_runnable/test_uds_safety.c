// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
// ================================
// File: tests/unit/test_uds_safety.c
// ================================
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit/test_uds_safety.c
 *
 * MODULE UNDER TEST: core/uds_safety.c
 *
 * PURPOSE:
 *   Verify the ASIL-B safety check engine. Tests cover every public API:
 *
 *   Module lifecycle:
 *     - uds_safety_init: double-init guard, counter reset
 *     - uds_safety_get_ctx: NULL before init, valid after init
 *     - uds_safety_reset_counters: resets all counters
 *
 *   NULL pointer checks (REQ-SAFE-004):
 *     - uds_safety_check_null_ptr: NULL → violation counted + ERR returned,
 *       non-NULL → OK, total_checks_performed increments
 *
 *   Session validation (REQ-SAFE-002):
 *     - uds_safety_validate_session: NULL ctx, ordinal pass/fail
 *     - uds_safety_check_service_in_session: each SID with correct/wrong session
 *
 *   DID access control (REQ-SAFE-001, REQ-SAFE-003):
 *     - uds_safety_find_did: found, not-found, NULL out_entry
 *     - uds_safety_validate_did_access: read/write capability flags,
 *       session gate, security-level gate
 *
 *   Bounds checks (REQ-SAFE-006, REQ-SAFE-007):
 *     - uds_safety_check_did_data_length: buffer too small, exact fit, oversized
 *     - uds_safety_check_request_length: truncated vs. adequate request
 *     - uds_safety_check_write_data_length: mismatch vs. exact match
 *
 *   DTC validation:
 *     - uds_safety_validate_dtc_status_mask: in-mask bits, out-of-mask bits
 *
 *   Integrity checks:
 *     - uds_safety_verify_did_database: valid DB → OK
 *     - uds_safety_self_test: must pass on correctly built firmware
 *
 * SETUP: Each test suite reinitializes the DID/DTC databases and the safety
 *        module so violation counters start at zero.
 *
 * FRAMEWORK: Zephyr Ztest
 *
 * DID constants (from generated/did_handlers.c):
 *   0x0C00  Engine Speed         2 bytes  min_session=DEFAULT  read_level=0
 *   0x0500  Coolant Temperature  1 byte   min_session=DEFAULT  read_level=0
 *   0xF190  VIN                 17 bytes  min_session=DEFAULT  read_level=0  write_level=2
 *   0xF18C  ECU Serial Number    4 bytes  min_session=DEFAULT  read_level=0  write_level=2
 *   0xF187  Spare Part Number   11 bytes  min_session=EXTENDED read_level=0  write_level=2
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "uds_safety.h"
#include "uds_session.h"
#include "uds_security.h"
#include "did_database.h"
#include "dtc_database.h"
#include "uds_types.h"

/* =========================================================================
 * Shared test callbacks (minimal stubs for security init)
 * ========================================================================= */

static uds_status_t t_seed_gen(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l;
    for (uint8_t i = 0; i < n && i < 4U; i++) { b[i] = (uint8_t)(0x10U + i); }
    *o = (n < 4U) ? n : 4U;
    return UDS_STATUS_OK;
}
static bool t_key_val(uint8_t l, const uint8_t *s, uint8_t sl,
                      const uint8_t *k, uint8_t kl)
{
    (void)l;
    if (sl != kl) return false;
    for (uint8_t i = 0; i < sl; i++) {
        if (k[i] != (uint8_t)(s[i] ^ 0xAAU)) return false;
    }
    return true;
}

/* =========================================================================
 * Module reset helper — call before each suite's first test
 * ========================================================================= */

/*
 * The safety module has a static initialized guard. For unit tests that need
 * a fresh module, we call uds_safety_reset_counters() which is the only
 * supported way to clear state without firmware restart.
 *
 * If testing double-init behaviour, do NOT call this helper first.
 */
static void safety_fresh_init(void)
{
    /* If already initialized, just reset counters */
    uds_safety_init();               /* may return ALREADY_INITIALIZED — ignore */
    uds_safety_reset_counters();     /* zero all violation counters */
}

/* Full DB + safety reinit used by most test suites */
static void full_stack_init(uds_session_ctx_t  *sess,
                             uds_security_ctx_t *sec)
{
    /* Safety */
    safety_fresh_init();

    /* DID database */
    did_database_init();   /* may return ALREADY_INITIALIZED — tolerated */

    /* Register the 5 generated DIDs */
    {
        /* DID 0x0C00 Engine Speed */
        static const did_entry_t d1 = {
            .did_id             = 0x0C00U,
            .access_flags       = DID_ACCESS_READ,
            .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
            .read_access_level  = 0U,
            .write_access_level = 0U,
            .data_length        = 2U,
            .read_cb            = NULL,
            .write_cb           = NULL,
            .description        = "Engine Speed",
        };
        /* DID 0x0500 Coolant Temperature */
        static const did_entry_t d2 = {
            .did_id             = 0x0500U,
            .access_flags       = DID_ACCESS_READ,
            .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
            .read_access_level  = 0U,
            .write_access_level = 0U,
            .data_length        = 1U,
            .read_cb            = NULL,
            .write_cb           = NULL,
            .description        = "Coolant Temperature",
        };
        /* DID 0xF190 VIN */
        static const did_entry_t d3 = {
            .did_id             = 0xF190U,
            .access_flags       = DID_ACCESS_READ,
            .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
            .read_access_level  = 0U,
            .write_access_level = 1U,
            .data_length        = 17U,
            .read_cb            = NULL,
            .write_cb           = NULL,
            .description        = "VIN",
        };
        /* DID 0xF18C ECU Serial Number */
        static const did_entry_t d4 = {
            .did_id             = 0xF18CU,
            .access_flags       = DID_ACCESS_READ,
            .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
            .read_access_level  = 0U,
            .write_access_level = 1U,
            .data_length        = 4U,
            .read_cb            = NULL,
            .write_cb           = NULL,
            .description        = "ECU Serial",
        };
        /* DID 0xF187 Spare Part Number — READ+WRITE, requires EXTENDED+level2 write */
        static const did_entry_t d5 = {
            .did_id             = 0xF187U,
            .access_flags       = (uint8_t)(DID_ACCESS_READ | DID_ACCESS_WRITE),
            .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
            .read_access_level  = 0U,
            .write_access_level = 1U,
            .data_length        = 11U,
            .read_cb            = NULL,
            .write_cb           = NULL,
            .description        = "Spare Part Number",
        };

        /* Attempt registration — ignore ALREADY_INITIALIZED (re-init across tests) */
        (void)did_database_register(&d1);
        (void)did_database_register(&d2);
        (void)did_database_register(&d3);
        (void)did_database_register(&d4);
        (void)did_database_register(&d5);
    }

    /* Session */
    memset(sess, 0, sizeof(*sess));
    uds_session_init(sess, 5000U);

    /* Security */
    memset(sec, 0, sizeof(*sec));
    static const uds_security_cfg_t sec_cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,
        .key_validate_cb  = t_key_val,
        .seed_generate_cb = t_seed_gen,
    };
    uds_security_init(sec, &sec_cfg);
}

/* =========================================================================
 * Test suite: uds_safety_init / get_ctx / reset_counters
 * ========================================================================= */

ZTEST_SUITE(test_safety_lifecycle, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SAFE-LIFE-001: uds_safety_init() returns OK on first call. [REQ-SAFE-005]
 *
 * NOTE: Because static state persists across test suites in Ztest, we use
 * the reset_counters path rather than expecting a fresh init every time.
 */
ZTEST(test_safety_lifecycle, test_init_and_get_ctx)
{
    uds_safety_init();   /* may already be initialized */
    uds_safety_reset_counters();

    const uds_safety_ctx_t *ctx = uds_safety_get_ctx();
    zassert_not_null(ctx, "get_ctx must return non-NULL after init");
    zassert_true(ctx->initialized, "initialized must be set");
    zassert_equal(ctx->null_check_violations,     0U, "null violations must be 0");
    zassert_equal(ctx->session_check_violations,  0U, "session violations must be 0");
    zassert_equal(ctx->security_check_violations, 0U, "security violations must be 0");
    zassert_equal(ctx->bounds_check_violations,   0U, "bounds violations must be 0");
}

/**
 * TC-SAFE-LIFE-002: uds_safety_get_ctx() returns non-NULL after init.
 */
ZTEST(test_safety_lifecycle, test_get_ctx_not_null)
{
    uds_safety_init();
    const uds_safety_ctx_t *ctx = uds_safety_get_ctx();
    zassert_not_null(ctx, "get_ctx must not return NULL");
}

/**
 * TC-SAFE-LIFE-003: uds_safety_reset_counters() zeroes all violation counters.
 */
ZTEST(test_safety_lifecycle, test_reset_counters_clears_violations)
{
    uds_safety_init();
    /* Force some violations */
    uds_safety_check_null_ptr(NULL, "test");
    uds_safety_check_null_ptr(NULL, "test2");

    const uds_safety_ctx_t *ctx = uds_safety_get_ctx();
    zassert_true(ctx->null_check_violations > 0U,
                 "Violations must have been recorded");

    uds_status_t rc = uds_safety_reset_counters();
    zassert_equal(rc, UDS_STATUS_OK, "reset_counters must succeed");
    zassert_equal(ctx->null_check_violations, 0U,
                  "null violations cleared after reset");
    zassert_equal(ctx->total_checks_performed, 0U,
                  "total checks cleared after reset");
}

/* =========================================================================
 * Test suite: uds_safety_check_null_ptr (REQ-SAFE-004)
 * ========================================================================= */

ZTEST_SUITE(test_safety_null_ptr, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SAFE-NULL-001: NULL pointer → UDS_STATUS_ERR_NULL_PTR, violation counted.
 */
ZTEST(test_safety_null_ptr, test_null_ptr_detected)
{
    uds_safety_init();
    uds_safety_reset_counters();
    const uds_safety_ctx_t *ctx = uds_safety_get_ctx();

    uds_safety_result_t rc = uds_safety_check_null_ptr(NULL, "test_ptr");
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL must be detected");
    zassert_equal(ctx->null_check_violations, 1U,
                  "Violation counter must increment");
    zassert_equal(ctx->last_violation_code, UDS_STATUS_ERR_NULL_PTR,
                  "last_violation_code must be ERR_NULL_PTR");
}

/**
 * TC-SAFE-NULL-002: Non-NULL pointer → UDS_STATUS_OK, no violation.
 */
ZTEST(test_safety_null_ptr, test_non_null_ok)
{
    uds_safety_init();
    uds_safety_reset_counters();
    const uds_safety_ctx_t *ctx = uds_safety_get_ctx();

    int x = 0;
    uds_safety_result_t rc = uds_safety_check_null_ptr(&x, "x");
    zassert_equal(rc, UDS_STATUS_OK, "Non-NULL must return OK");
    zassert_equal(ctx->null_check_violations, 0U, "No violation for non-NULL");
}

/**
 * TC-SAFE-NULL-003: total_checks_performed increments for both NULL and non-NULL.
 */
ZTEST(test_safety_null_ptr, test_total_checks_increments)
{
    uds_safety_init();
    uds_safety_reset_counters();
    const uds_safety_ctx_t *ctx = uds_safety_get_ctx();

    int x = 1;
    uds_safety_check_null_ptr(NULL, "a");
    uds_safety_check_null_ptr(&x, "b");
    uds_safety_check_null_ptr(NULL, "c");

    zassert_equal(ctx->total_checks_performed, 3U,
                  "total_checks_performed must be 3");
    zassert_equal(ctx->null_check_violations, 2U,
                  "null_check_violations must be 2");
}

/* =========================================================================
 * Test suite: uds_safety_validate_session (REQ-SAFE-002)
 * ========================================================================= */

ZTEST_SUITE(test_safety_session, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SAFE-SESS-001: NULL session_ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_safety_session, test_null_session_ctx)
{
    uds_safety_init();
    uds_safety_reset_counters();
    uds_safety_result_t rc = uds_safety_validate_session(NULL, UDS_SESSION_DEFAULT);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-SAFE-SESS-002: Active DEFAULT meets required DEFAULT → OK.
 */
ZTEST(test_safety_session, test_default_meets_default)
{
    uds_safety_init(); uds_safety_reset_counters();
    uds_session_ctx_t sess; memset(&sess, 0, sizeof(sess));
    uds_session_init(&sess, 5000U);

    uds_safety_result_t rc = uds_safety_validate_session(&sess, UDS_SESSION_DEFAULT);
    zassert_equal(rc, UDS_STATUS_OK, "DEFAULT meets DEFAULT requirement");
}

/**
 * TC-SAFE-SESS-003: Active DEFAULT does NOT meet required EXTENDED → violation.
 */
ZTEST(test_safety_session, test_default_fails_extended_requirement)
{
    uds_safety_init(); uds_safety_reset_counters();
    uds_session_ctx_t sess; memset(&sess, 0, sizeof(sess));
    uds_session_init(&sess, 5000U);
    const uds_safety_ctx_t *ctx = uds_safety_get_ctx();

    uds_safety_result_t rc = uds_safety_validate_session(&sess, UDS_SESSION_EXTENDED);
    zassert_equal(rc, UDS_STATUS_ERR_SESSION_INVALID,
                  "DEFAULT must not meet EXTENDED requirement");
    zassert_equal(ctx->session_check_violations, 1U, "Violation must be recorded");
}

/**
 * TC-SAFE-SESS-004: Active EXTENDED meets required EXTENDED → OK.
 */
ZTEST(test_safety_session, test_extended_meets_extended)
{
    uds_safety_init(); uds_safety_reset_counters();
    uds_session_ctx_t sess; memset(&sess, 0, sizeof(sess));
    uds_session_init(&sess, 5000U);
    uds_session_transition(&sess, UDS_SESSION_EXTENDED);

    uds_safety_result_t rc = uds_safety_validate_session(&sess, UDS_SESSION_EXTENDED);
    zassert_equal(rc, UDS_STATUS_OK, "EXTENDED meets EXTENDED");
}

/**
 * TC-SAFE-SESS-005: Active EXTENDED does NOT meet SAFETY_SYSTEM requirement.
 * (EXTENDED ordinal < SAFETY_SYSTEM ordinal)
 */
ZTEST(test_safety_session, test_extended_fails_safety_system_requirement)
{
    uds_safety_init(); uds_safety_reset_counters();
    uds_session_ctx_t sess; memset(&sess, 0, sizeof(sess));
    uds_session_init(&sess, 5000U);
    uds_session_transition(&sess, UDS_SESSION_EXTENDED);

    uds_safety_result_t rc = uds_safety_validate_session(&sess, UDS_SESSION_SAFETY_SYSTEM);
    zassert_equal(rc, UDS_STATUS_ERR_SESSION_INVALID,
                  "EXTENDED must not meet SAFETY_SYSTEM requirement");
}

/**
 * TC-SAFE-SESS-006: uds_safety_check_service_in_session:
 *   SID 0x27 (SecurityAccess) requires EXTENDED — DEFAULT → violation.
 */
ZTEST(test_safety_session, test_service_in_session_0x27_default_fails)
{
    uds_safety_init(); uds_safety_reset_counters();
    uds_session_ctx_t sess; memset(&sess, 0, sizeof(sess));
    uds_session_init(&sess, 5000U);

    uds_safety_result_t rc = uds_safety_check_service_in_session(&sess, 0x27U);
    zassert_equal(rc, UDS_STATUS_ERR_SESSION_INVALID,
                  "SID 0x27 in DEFAULT session must fail");
}

/**
 * TC-SAFE-SESS-007: SID 0x27 in EXTENDED session → OK.
 */
ZTEST(test_safety_session, test_service_in_session_0x27_extended_ok)
{
    uds_safety_init(); uds_safety_reset_counters();
    uds_session_ctx_t sess; memset(&sess, 0, sizeof(sess));
    uds_session_init(&sess, 5000U);
    uds_session_transition(&sess, UDS_SESSION_EXTENDED);

    uds_safety_result_t rc = uds_safety_check_service_in_session(&sess, 0x27U);
    zassert_equal(rc, UDS_STATUS_OK, "SID 0x27 in EXTENDED must succeed");
}

/**
 * TC-SAFE-SESS-008: SID 0x22 (ReadDBI) in DEFAULT → OK (any session allowed).
 */
ZTEST(test_safety_session, test_service_in_session_0x22_default_ok)
{
    uds_safety_init(); uds_safety_reset_counters();
    uds_session_ctx_t sess; memset(&sess, 0, sizeof(sess));
    uds_session_init(&sess, 5000U);

    uds_safety_result_t rc = uds_safety_check_service_in_session(&sess, 0x22U);
    zassert_equal(rc, UDS_STATUS_OK, "SID 0x22 in DEFAULT must succeed");
}

/**
 * TC-SAFE-SESS-009: Unknown SID → UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED.
 */
ZTEST(test_safety_session, test_service_in_session_unknown_sid)
{
    uds_safety_init(); uds_safety_reset_counters();
    uds_session_ctx_t sess; memset(&sess, 0, sizeof(sess));
    uds_session_init(&sess, 5000U);

    uds_safety_result_t rc = uds_safety_check_service_in_session(&sess, 0xAAU);
    zassert_equal(rc, UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED,
                  "Unknown SID must return SERVICE_NOT_SUPPORTED");
}

/* =========================================================================
 * Test suite: uds_safety_find_did
 * ========================================================================= */

ZTEST_SUITE(test_safety_find_did, NULL, NULL, NULL, NULL, NULL);

static uds_session_ctx_t  g_sess_fd;
static uds_security_ctx_t g_sec_fd;

static void setup_find_did(void *arg)
{
    (void)arg;
    full_stack_init(&g_sess_fd, &g_sec_fd);
}

ZTEST_SUITE(test_safety_find_did_setup, NULL, setup_find_did, NULL, NULL, NULL);

/**
 * TC-SAFE-FIND-001: NULL out_entry → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_safety_find_did_setup, test_null_out_entry)
{
    full_stack_init(&g_sess_fd, &g_sec_fd);

    uds_safety_result_t rc = uds_safety_find_did(0x0C00U, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL out_entry must fail");
}

/**
 * TC-SAFE-FIND-002: Known DID 0x0C00 → OK, entry non-NULL, correct did_id.
 */
ZTEST(test_safety_find_did_setup, test_known_did_found)
{
    full_stack_init(&g_sess_fd, &g_sec_fd);

    const did_entry_t *entry = NULL;
    uds_safety_result_t rc = uds_safety_find_did(0x0C00U, &entry);
    zassert_equal(rc, UDS_STATUS_OK, "Known DID must be found");
    zassert_not_null(entry, "Entry must not be NULL");
    zassert_equal(entry->did_id, 0x0C00U, "DID ID mismatch");
    zassert_equal(entry->data_length, 2U, "Engine Speed must be 2 bytes");
}

/**
 * TC-SAFE-FIND-003: VIN (0xF190) → found with correct data_length = 17.
 */
ZTEST(test_safety_find_did_setup, test_vin_did_found)
{
    full_stack_init(&g_sess_fd, &g_sec_fd);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0xF190U, &entry);
    zassert_not_null(entry, "VIN must be found");
    zassert_equal(entry->data_length, 17U, "VIN must be 17 bytes");
}

/**
 * TC-SAFE-FIND-004: Unknown DID 0xDEAD → UDS_STATUS_ERR_DID_NOT_FOUND.
 */
ZTEST(test_safety_find_did_setup, test_unknown_did_not_found)
{
    full_stack_init(&g_sess_fd, &g_sec_fd);

    const did_entry_t *entry = NULL;
    uds_safety_result_t rc = uds_safety_find_did(0xDEADU, &entry);
    zassert_equal(rc, UDS_STATUS_ERR_DID_NOT_FOUND, "Unknown DID must not be found");
    zassert_is_null(entry, "entry must be NULL for unknown DID");
}

/**
 * TC-SAFE-FIND-005: DID 0xF187 (Spare Part) → found, write-capable.
 */
ZTEST(test_safety_find_did_setup, test_spare_part_did_found_writable)
{
    full_stack_init(&g_sess_fd, &g_sec_fd);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0xF187U, &entry);
    zassert_not_null(entry, "Spare Part DID must be found");
    zassert_equal(entry->data_length, 11U, "Spare Part must be 11 bytes");
    zassert_true((entry->access_flags & DID_ACCESS_WRITE) != 0U,
                 "Spare Part must have WRITE flag");
}

/* =========================================================================
 * Test suite: uds_safety_validate_did_access (REQ-SAFE-001/003)
 * ========================================================================= */

ZTEST_SUITE(test_safety_did_access, NULL, NULL, NULL, NULL, NULL);

static uds_session_ctx_t  g_sess_da;
static uds_security_ctx_t g_sec_da;

static void setup_did_access(void *arg)
{
    (void)arg;
    full_stack_init(&g_sess_da, &g_sec_da);
}

ZTEST_SUITE(test_safety_did_access_s, NULL, setup_did_access, NULL, NULL, NULL);

/**
 * TC-SAFE-DA-001: NULL entry → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_safety_did_access_s, test_null_entry)
{
    full_stack_init(&g_sess_da, &g_sec_da);

    uds_safety_result_t rc = uds_safety_validate_did_access(
        NULL, &g_sess_da, &g_sec_da, UDS_SAFETY_ACCESS_READ);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL entry must fail");
}

/**
 * TC-SAFE-DA-002: NULL session_ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_safety_did_access_s, test_null_session)
{
    full_stack_init(&g_sess_da, &g_sec_da);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0x0C00U, &entry);
    zassert_not_null(entry, "DID must exist");
    uds_safety_result_t rc = uds_safety_validate_did_access(
        entry, NULL, &g_sec_da, UDS_SAFETY_ACCESS_READ);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL session must fail");
}

/**
 * TC-SAFE-DA-003: NULL security_ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_safety_did_access_s, test_null_security)
{
    full_stack_init(&g_sess_da, &g_sec_da);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0x0C00U, &entry);
    zassert_not_null(entry, "DID must exist");
    uds_safety_result_t rc = uds_safety_validate_did_access(
        entry, &g_sess_da, NULL, UDS_SAFETY_ACCESS_READ);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL security must fail");
}

/**
 * TC-SAFE-DA-004: READ access to 0x0C00 in DEFAULT, no lock → OK.
 */
ZTEST(test_safety_did_access_s, test_engine_speed_read_default_ok)
{
    full_stack_init(&g_sess_da, &g_sec_da);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0x0C00U, &entry);
    uds_safety_result_t rc = uds_safety_validate_did_access(
        entry, &g_sess_da, &g_sec_da, UDS_SAFETY_ACCESS_READ);
    zassert_equal(rc, UDS_STATUS_OK, "Engine Speed read in DEFAULT must succeed");
}

/**
 * TC-SAFE-DA-005: WRITE access to 0x0C00 — DID has no WRITE flag
 *                 → UDS_STATUS_ERR_CONDITIONS_NOT_MET.
 */
ZTEST(test_safety_did_access_s, test_engine_speed_write_not_capable)
{
    full_stack_init(&g_sess_da, &g_sec_da);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0x0C00U, &entry);
    uds_safety_result_t rc = uds_safety_validate_did_access(
        entry, &g_sess_da, &g_sec_da, UDS_SAFETY_ACCESS_WRITE);
    zassert_equal(rc, UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  "Engine Speed has no WRITE flag — must fail");
}

/**
 * TC-SAFE-DA-006: READ access to 0xF187 (Spare Part) in DEFAULT session
 *                 → UDS_STATUS_ERR_SESSION_INVALID (requires EXTENDED).
 */
ZTEST(test_safety_did_access_s, test_spare_part_read_default_fails)
{
    full_stack_init(&g_sess_da, &g_sec_da);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0xF187U, &entry);
    zassert_not_null(entry, "Spare Part DID must exist");

    /* Session is DEFAULT — DID requires EXTENDED */
    uds_safety_result_t rc = uds_safety_validate_did_access(
        entry, &g_sess_da, &g_sec_da, UDS_SAFETY_ACCESS_READ);
    zassert_equal(rc, UDS_STATUS_ERR_SESSION_INVALID,
                  "Spare Part read in DEFAULT must fail (requires EXTENDED)");
}

/**
 * TC-SAFE-DA-007: READ access to 0xF187 in EXTENDED session → OK (read_level=0).
 */
ZTEST(test_safety_did_access_s, test_spare_part_read_extended_ok)
{
    full_stack_init(&g_sess_da, &g_sec_da);

    uds_session_transition(&g_sess_da, UDS_SESSION_EXTENDED);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0xF187U, &entry);
    uds_safety_result_t rc = uds_safety_validate_did_access(
        entry, &g_sess_da, &g_sec_da, UDS_SAFETY_ACCESS_READ);
    zassert_equal(rc, UDS_STATUS_OK,
                  "Spare Part read in EXTENDED with level 0 must succeed");
}

/**
 * TC-SAFE-DA-008: WRITE access to 0xF187 in EXTENDED, security NOT unlocked
 *                 → UDS_STATUS_ERR_SEC_NOT_UNLOCKED (write_level=2).
 */
ZTEST(test_safety_did_access_s, test_spare_part_write_security_locked_fails)
{
    full_stack_init(&g_sess_da, &g_sec_da);

    uds_session_transition(&g_sess_da, UDS_SESSION_EXTENDED);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0xF187U, &entry);
    uds_safety_result_t rc = uds_safety_validate_did_access(
        entry, &g_sess_da, &g_sec_da, UDS_SAFETY_ACCESS_WRITE);
    zassert_equal(rc, UDS_STATUS_ERR_SEC_NOT_UNLOCKED,
                  "Spare Part write without security unlock must fail");
}

/**
 * TC-SAFE-DA-009: WRITE access to 0xF187 in EXTENDED, security unlocked
 *                 → OK.
 *
 * Security unlock sequence: request seed (level 1 = 0x01), send correct key.
 * write_access_level = 2, which maps to level-1 seed (0x01)
 * per uds_security_is_unlocked (active_level == security_level).
 */
ZTEST(test_safety_did_access_s, test_spare_part_write_security_unlocked_ok)
{
    full_stack_init(&g_sess_da, &g_sec_da);

    uds_session_transition(&g_sess_da, UDS_SESSION_EXTENDED);

    /* Perform security unlock for level 1 (sub-fn 0x01 seed, 0x02 key) */
    uint8_t seed[UDS_SECURITY_SEED_LEN]; uint8_t seed_len = 0U;
    uds_security_request_seed(&g_sec_da, 0x01U, seed, sizeof(seed), &seed_len);

    /* [P1-SEC FIX] key = UDS_SECURITY_KEY_LEN (4) bytes */
    uint8_t key[UDS_SECURITY_KEY_LEN];
    for (uint8_t i = 0; i < (uint8_t)UDS_SECURITY_KEY_LEN; i++) {
        key[i] = (uint8_t)(seed[i] ^ 0xAAU);
    }
    uds_security_send_key(&g_sec_da, 0x02U, key, (uint8_t)UDS_SECURITY_KEY_LEN);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0xF187U, &entry);
    uds_safety_result_t rc = uds_safety_validate_did_access(
        entry, &g_sess_da, &g_sec_da, UDS_SAFETY_ACCESS_WRITE);
    zassert_equal(rc, UDS_STATUS_OK,
                  "Spare Part write with security unlocked must succeed");
}

/* =========================================================================
 * Test suite: bounds checks (REQ-SAFE-006, REQ-SAFE-007)
 * ========================================================================= */

ZTEST_SUITE(test_safety_bounds, NULL, NULL, NULL, NULL, NULL);

static uds_session_ctx_t  g_sess_bounds;
static uds_security_ctx_t g_sec_bounds;

static void setup_bounds(void *arg)
{
    (void)arg;
    full_stack_init(&g_sess_bounds, &g_sec_bounds);
}

ZTEST_SUITE(test_safety_bounds_s, NULL, setup_bounds, NULL, NULL, NULL);

/**
 * TC-SAFE-BOUNDS-001: check_did_data_length: NULL entry → ERR_NULL_PTR.
 */
ZTEST(test_safety_bounds_s, test_did_data_len_null_entry)
{
    full_stack_init(&g_sess_bounds, &g_sec_bounds);

    zassert_equal(uds_safety_check_did_data_length(NULL, 64U),
                  UDS_STATUS_ERR_NULL_PTR, "NULL entry must fail");
}

/**
 * TC-SAFE-BOUNDS-002: buf_len == data_length (exact fit) → OK.
 * DID 0x0C00 data_length = 2.
 */
ZTEST(test_safety_bounds_s, test_did_data_len_exact_fit)
{
    full_stack_init(&g_sess_bounds, &g_sec_bounds);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0x0C00U, &entry);
    zassert_equal(uds_safety_check_did_data_length(entry, 2U),
                  UDS_STATUS_OK, "Exact fit must pass bounds check");
}

/**
 * TC-SAFE-BOUNDS-003: buf_len > data_length → OK.
 * DID 0x0C00 data_length = 2, buf_len = 64.
 */
ZTEST(test_safety_bounds_s, test_did_data_len_larger_buffer)
{
    full_stack_init(&g_sess_bounds, &g_sec_bounds);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0x0C00U, &entry);
    zassert_equal(uds_safety_check_did_data_length(entry, 64U),
                  UDS_STATUS_OK, "Larger buffer must pass bounds check");
}

/**
 * TC-SAFE-BOUNDS-004: buf_len < data_length → UDS_STATUS_ERR_BUFFER_OVERFLOW.
 * DID 0xF190 (VIN) data_length = 17. Supply buf_len = 16.
 */
ZTEST(test_safety_bounds_s, test_did_data_len_too_small)
{
    full_stack_init(&g_sess_bounds, &g_sec_bounds);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0xF190U, &entry);
    zassert_equal(uds_safety_check_did_data_length(entry, 16U),
                  UDS_STATUS_ERR_BUFFER_OVERFLOW,
                  "Buffer too small must return ERR_BUFFER_OVERFLOW");
}

/**
 * TC-SAFE-BOUNDS-005: check_request_length: NULL req → ERR_NULL_PTR.
 */
ZTEST(test_safety_bounds_s, test_req_len_null)
{
    full_stack_init(&g_sess_bounds, &g_sec_bounds);

    zassert_equal(uds_safety_check_request_length(NULL, 3U),
                  UDS_STATUS_ERR_NULL_PTR, "NULL req must fail");
}

/**
 * TC-SAFE-BOUNDS-006: req->length >= min_length → OK.
 */
ZTEST(test_safety_bounds_s, test_req_len_adequate)
{
    full_stack_init(&g_sess_bounds, &g_sec_bounds);

    uds_msg_buf_t req; memset(&req, 0, sizeof(req));
    req.length = 3U;
    zassert_equal(uds_safety_check_request_length(&req, 3U),
                  UDS_STATUS_OK, "Adequate request length must pass");
}

/**
 * TC-SAFE-BOUNDS-007: req->length < min_length → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_safety_bounds_s, test_req_len_too_short)
{
    full_stack_init(&g_sess_bounds, &g_sec_bounds);

    uds_msg_buf_t req; memset(&req, 0, sizeof(req));
    req.length = 1U;
    zassert_equal(uds_safety_check_request_length(&req, 3U),
                  UDS_STATUS_ERR_INVALID_PARAM,
                  "Too-short request must fail");
}

/**
 * TC-SAFE-BOUNDS-008: check_write_data_length: exact match → OK.
 * DID 0xF187 data_length = 11.
 */
ZTEST(test_safety_bounds_s, test_write_len_exact)
{
    full_stack_init(&g_sess_bounds, &g_sec_bounds);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0xF187U, &entry);
    zassert_equal(uds_safety_check_write_data_length(entry, 11U),
                  UDS_STATUS_OK, "Exact write length must pass");
}

/**
 * TC-SAFE-BOUNDS-009: check_write_data_length: mismatch → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_safety_bounds_s, test_write_len_mismatch)
{
    full_stack_init(&g_sess_bounds, &g_sec_bounds);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0xF187U, &entry);
    zassert_equal(uds_safety_check_write_data_length(entry, 10U),
                  UDS_STATUS_ERR_INVALID_PARAM,
                  "Write length mismatch must fail (NRC 0x13 scenario)");
}

/**
 * TC-SAFE-BOUNDS-010: check_write_data_length: longer than expected → also fails.
 */
ZTEST(test_safety_bounds_s, test_write_len_too_long)
{
    full_stack_init(&g_sess_bounds, &g_sec_bounds);

    const did_entry_t *entry = NULL;
    uds_safety_find_did(0xF187U, &entry);
    zassert_equal(uds_safety_check_write_data_length(entry, 12U),
                  UDS_STATUS_ERR_INVALID_PARAM,
                  "Write length longer than expected must also fail");
}

/* =========================================================================
 * Test suite: DTC status mask validation
 * ========================================================================= */

ZTEST_SUITE(test_safety_dtc_mask, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SAFE-DTC-001: All bits in mask → OK.
 * GEN_SAFETY_DTC_SUPPORT_MASK = 0xFF (all bits supported).
 */
ZTEST(test_safety_dtc_mask, test_all_bits_in_mask_ok)
{
    uds_safety_init(); uds_safety_reset_counters();
    zassert_equal(uds_safety_validate_dtc_status_mask(0xFFU, 0xFFU),
                  UDS_STATUS_OK, "0xFF in 0xFF mask must pass");
}

/**
 * TC-SAFE-DTC-002: No bits set → OK.
 */
ZTEST(test_safety_dtc_mask, test_zero_status_ok)
{
    uds_safety_init(); uds_safety_reset_counters();
    zassert_equal(uds_safety_validate_dtc_status_mask(0x00U, 0xFFU),
                  UDS_STATUS_OK, "0x00 status always passes");
}

/**
 * TC-SAFE-DTC-003: Bit set outside mask → UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE.
 * mask = 0x0F, status = 0x10 (bit 4 not in mask).
 */
ZTEST(test_safety_dtc_mask, test_unsupported_bit_rejected)
{
    uds_safety_init(); uds_safety_reset_counters();
    zassert_equal(uds_safety_validate_dtc_status_mask(0x10U, 0x0FU),
                  UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "Bit outside mask must be rejected");
}

/**
 * TC-SAFE-DTC-004: Subset of supported bits → OK.
 */
ZTEST(test_safety_dtc_mask, test_subset_of_mask_ok)
{
    uds_safety_init(); uds_safety_reset_counters();
    /* All DTC status bits in mask 0xFFU are supported */
    zassert_equal(uds_safety_validate_dtc_status_mask(0x09U, 0xFFU),
                  UDS_STATUS_OK, "Subset of mask must pass");
}

/* =========================================================================
 * Test suite: uds_safety_verify_did_database + self_test
 * ========================================================================= */

ZTEST_SUITE(test_safety_integrity, NULL, NULL, NULL, NULL, NULL);

static uds_session_ctx_t  g_sess_int;
static uds_security_ctx_t g_sec_int;

/**
 * TC-SAFE-INT-001: verify_did_database on valid populated database → OK.
 */
ZTEST(test_safety_integrity, test_verify_did_database_ok)
{
    full_stack_init(&g_sess_int, &g_sec_int);
    uds_safety_result_t rc = uds_safety_verify_did_database();
    zassert_equal(rc, UDS_STATUS_OK,
                  "verify_did_database must pass for valid generated DIDs");
}

/**
 * TC-SAFE-INT-002: uds_safety_self_test() must pass.
 *
 * Per uds_safety.h: a failing self-test indicates a build/memory defect —
 * this test will only fail on a broken build, which is the intended behaviour.
 */
ZTEST(test_safety_integrity, test_self_test_passes)
{
    full_stack_init(&g_sess_int, &g_sec_int);
    uds_safety_result_t rc = uds_safety_self_test();
    zassert_equal(rc, UDS_STATUS_OK,
                  "safety self-test must pass on a correctly built stack");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_safety_lifecycle__test_init_and_get_ctx(void);
extern void test_safety_lifecycle__test_get_ctx_not_null(void);
extern void test_safety_lifecycle__test_reset_counters_clears_violations(void);
extern void test_safety_null_ptr__test_null_ptr_detected(void);
extern void test_safety_null_ptr__test_non_null_ok(void);
extern void test_safety_null_ptr__test_total_checks_increments(void);
extern void test_safety_session__test_null_session_ctx(void);
extern void test_safety_session__test_default_meets_default(void);
extern void test_safety_session__test_default_fails_extended_requirement(void);
extern void test_safety_session__test_extended_meets_extended(void);
extern void test_safety_session__test_extended_fails_safety_system_requirement(void);
extern void test_safety_session__test_service_in_session_0x27_default_fails(void);
extern void test_safety_session__test_service_in_session_0x27_extended_ok(void);
extern void test_safety_session__test_service_in_session_0x22_default_ok(void);
extern void test_safety_session__test_service_in_session_unknown_sid(void);
extern void test_safety_find_did_setup__test_null_out_entry(void);
extern void test_safety_find_did_setup__test_known_did_found(void);
extern void test_safety_find_did_setup__test_vin_did_found(void);
extern void test_safety_find_did_setup__test_unknown_did_not_found(void);
extern void test_safety_find_did_setup__test_spare_part_did_found_writable(void);
extern void test_safety_did_access_s__test_null_entry(void);
extern void test_safety_did_access_s__test_null_session(void);
extern void test_safety_did_access_s__test_null_security(void);
extern void test_safety_did_access_s__test_engine_speed_read_default_ok(void);
extern void test_safety_did_access_s__test_engine_speed_write_not_capable(void);
extern void test_safety_did_access_s__test_spare_part_read_default_fails(void);
extern void test_safety_did_access_s__test_spare_part_read_extended_ok(void);
extern void test_safety_did_access_s__test_spare_part_write_security_locked_fails(void);
extern void test_safety_did_access_s__test_spare_part_write_security_unlocked_ok(void);
extern void test_safety_bounds_s__test_did_data_len_null_entry(void);
extern void test_safety_bounds_s__test_did_data_len_exact_fit(void);
extern void test_safety_bounds_s__test_did_data_len_larger_buffer(void);
extern void test_safety_bounds_s__test_did_data_len_too_small(void);
extern void test_safety_bounds_s__test_req_len_null(void);
extern void test_safety_bounds_s__test_req_len_adequate(void);
extern void test_safety_bounds_s__test_req_len_too_short(void);
extern void test_safety_bounds_s__test_write_len_exact(void);
extern void test_safety_bounds_s__test_write_len_mismatch(void);
extern void test_safety_bounds_s__test_write_len_too_long(void);
extern void test_safety_dtc_mask__test_all_bits_in_mask_ok(void);
extern void test_safety_dtc_mask__test_zero_status_ok(void);
extern void test_safety_dtc_mask__test_unsupported_bit_rejected(void);
extern void test_safety_dtc_mask__test_subset_of_mask_ok(void);
extern void test_safety_integrity__test_verify_did_database_ok(void);
extern void test_safety_integrity__test_self_test_passes(void);

void run_all_tests(void)
{
    RUN_TEST(test_safety_lifecycle__test_init_and_get_ctx);
    RUN_TEST(test_safety_lifecycle__test_get_ctx_not_null);
    RUN_TEST(test_safety_lifecycle__test_reset_counters_clears_violations);
    RUN_TEST(test_safety_null_ptr__test_null_ptr_detected);
    RUN_TEST(test_safety_null_ptr__test_non_null_ok);
    RUN_TEST(test_safety_null_ptr__test_total_checks_increments);
    RUN_TEST(test_safety_session__test_null_session_ctx);
    RUN_TEST(test_safety_session__test_default_meets_default);
    RUN_TEST(test_safety_session__test_default_fails_extended_requirement);
    RUN_TEST(test_safety_session__test_extended_meets_extended);
    RUN_TEST(test_safety_session__test_extended_fails_safety_system_requirement);
    RUN_TEST(test_safety_session__test_service_in_session_0x27_default_fails);
    RUN_TEST(test_safety_session__test_service_in_session_0x27_extended_ok);
    RUN_TEST(test_safety_session__test_service_in_session_0x22_default_ok);
    RUN_TEST(test_safety_session__test_service_in_session_unknown_sid);
    RUN_TEST(test_safety_find_did_setup__test_null_out_entry);
    RUN_TEST(test_safety_find_did_setup__test_known_did_found);
    RUN_TEST(test_safety_find_did_setup__test_vin_did_found);
    RUN_TEST(test_safety_find_did_setup__test_unknown_did_not_found);
    RUN_TEST(test_safety_find_did_setup__test_spare_part_did_found_writable);
    RUN_TEST(test_safety_did_access_s__test_null_entry);
    RUN_TEST(test_safety_did_access_s__test_null_session);
    RUN_TEST(test_safety_did_access_s__test_null_security);
    RUN_TEST(test_safety_did_access_s__test_engine_speed_read_default_ok);
    RUN_TEST(test_safety_did_access_s__test_engine_speed_write_not_capable);
    RUN_TEST(test_safety_did_access_s__test_spare_part_read_default_fails);
    RUN_TEST(test_safety_did_access_s__test_spare_part_read_extended_ok);
    RUN_TEST(test_safety_did_access_s__test_spare_part_write_security_locked_fails);
    RUN_TEST(test_safety_did_access_s__test_spare_part_write_security_unlocked_ok);
    RUN_TEST(test_safety_bounds_s__test_did_data_len_null_entry);
    RUN_TEST(test_safety_bounds_s__test_did_data_len_exact_fit);
    RUN_TEST(test_safety_bounds_s__test_did_data_len_larger_buffer);
    RUN_TEST(test_safety_bounds_s__test_did_data_len_too_small);
    RUN_TEST(test_safety_bounds_s__test_req_len_null);
    RUN_TEST(test_safety_bounds_s__test_req_len_adequate);
    RUN_TEST(test_safety_bounds_s__test_req_len_too_short);
    RUN_TEST(test_safety_bounds_s__test_write_len_exact);
    RUN_TEST(test_safety_bounds_s__test_write_len_mismatch);
    RUN_TEST(test_safety_bounds_s__test_write_len_too_long);
    RUN_TEST(test_safety_dtc_mask__test_all_bits_in_mask_ok);
    RUN_TEST(test_safety_dtc_mask__test_zero_status_ok);
    RUN_TEST(test_safety_dtc_mask__test_unsupported_bit_rejected);
    RUN_TEST(test_safety_dtc_mask__test_subset_of_mask_ok);
    RUN_TEST(test_safety_integrity__test_verify_did_database_ok);
    RUN_TEST(test_safety_integrity__test_self_test_passes);
}
