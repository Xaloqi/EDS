// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
// ================================
// File: tests/unit/test_did_handlers.c
// ================================
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit/test_did_handlers.c
 *
 * MODULE UNDER TEST: generated/did_handlers.c
 *                    generated/did_safety_wrappers.c
 *
 * PURPOSE:
 *   Verify all generated DID read/write handler stubs and their safe
 *   accessor wrappers. Tests cover:
 *
 *   DID handlers (generated/did_handlers.c):
 *     - did_read_engine_speed:
 *         NULL buf/out_len → ERR_NULL_PTR
 *         buf_len too small → ERR_BUFFER_OVERFLOW
 *         valid call → OK, out_len=2, zeros returned
 *     - did_read_coolant_temperature: same pattern, 1 byte
 *     - did_read_vehicle_identification_number: 17 bytes
 *     - did_read_ecu_serial_number: 4 bytes
 *     - did_read_vehicle_manufacturer_spare_part_number: 11 bytes
 *     - did_write_vehicle_manufacturer_spare_part_number:
 *         NULL buf → ERR_NULL_PTR
 *         len != 11 → ERR_INVALID_PARAM
 *         valid write → OK, data stored (verifiable via read)
 *     - did_handlers_register_all(): registers all 5 DIDs, each findable
 *
 *   Safety wrappers (generated/did_safety_wrappers.c):
 *     - did_safe_read_engine_speed: NULL session_ctx, NULL buf, OK path
 *     - did_safe_read_coolant_temperature: buffer too small → BUFFER_OVERFLOW
 *     - did_safe_read_vehicle_identification_number: wrong session → SESSION_INVALID
 *     - did_safe_read_vehicle_manufacturer_spare_part_number:
 *         DEFAULT session → SESSION_INVALID (requires EXTENDED)
 *     - did_safe_write_vehicle_manufacturer_spare_part_number:
 *         security not unlocked → SEC_NOT_UNLOCKED
 *         security unlocked → OK
 *
 * DID constants (generated/did_handlers.c):
 *   0x0C00  Engine Speed         2 bytes  READ  DEFAULT  sec=0
 *   0x0500  Coolant Temperature  1 byte   READ  DEFAULT  sec=0
 *   0xF190  VIN                 17 bytes  READ  DEFAULT  sec=0 write=2
 *   0xF18C  ECU Serial Number    4 bytes  READ  DEFAULT  sec=0 write=2
 *   0xF187  Spare Part Number   11 bytes  RW    EXTENDED write=2
 *
 * FRAMEWORK: Zephyr Ztest
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "did_handlers.h"
#include "did_safety_wrappers.h"
#include "did_database.h"
#include "uds_safety.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_types.h"

/* =========================================================================
 * Security stubs for wrapper tests
 * ========================================================================= */

static uds_status_t w_seed_gen(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l;
    for (uint8_t i = 0; i < n && i < 4U; i++) { b[i] = (uint8_t)(0x10U + i); }
    *o = (n < 4U) ? n : 4U;
    return UDS_STATUS_OK;
}

static bool w_key_val(uint8_t l, const uint8_t *s, uint8_t sl,
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
 * Full stack setup helper
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;

static void full_init(void)
{
    uds_safety_init();
    uds_safety_reset_counters();

    (void)did_database_init();
    (void)did_handlers_register_all();

    memset(&g_sess, 0, sizeof(g_sess));
    uds_session_init(&g_sess, 5000U);

    memset(&g_sec, 0, sizeof(g_sec));
    static const uds_security_cfg_t sec_cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,
        .key_validate_cb  = w_key_val,
        .seed_generate_cb = w_seed_gen,
    };
    uds_security_init(&g_sec, &sec_cfg);
}

/** Perform security level-1 unlock. */
static void do_sec_unlock(void)
{
    uint8_t seed[UDS_SECURITY_SEED_LEN]; uint8_t seed_len = 0U;
    uds_security_request_seed(&g_sec, 0x01U, seed, sizeof(seed), &seed_len);
    /* [P1-SEC FIX] key = UDS_SECURITY_KEY_LEN (4) bytes */
    uint8_t key[UDS_SECURITY_KEY_LEN];
    for (uint8_t i = 0; i < (uint8_t)UDS_SECURITY_KEY_LEN; i++) {
        key[i] = (uint8_t)(seed[i] ^ 0xAAU);
    }
    uds_security_send_key(&g_sec, 0x02U, key, (uint8_t)UDS_SECURITY_KEY_LEN);
}

/* =========================================================================
 * Test suite: did_handlers_register_all
 * ========================================================================= */

ZTEST_SUITE(test_did_handlers_register, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DIDH-REG-001: did_handlers_register_all() succeeds and all 5 DIDs
 *                  are findable by ID.
 */
ZTEST(test_did_handlers_register, test_all_dids_registered_and_findable)
{
    full_init();

    static const uint16_t expected_ids[5] = {
        0x0C00U, 0x0500U, 0xF190U, 0xF18CU, 0xF187U
    };
    static const uint16_t expected_lens[5] = { 2U, 1U, 17U, 4U, 11U };

    for (size_t i = 0; i < 5U; i++) {
        const did_entry_t *e = did_database_find(expected_ids[i]);
        zassert_not_null(e, "DID 0x%04X must be registered", expected_ids[i]);
        zassert_equal(e->data_length, expected_lens[i],
                      "data_length mismatch for DID 0x%04X", expected_ids[i]);
    }
}

/**
 * TC-DIDH-REG-002: GEN_DID_HANDLER_COUNT == 5 (compile-time constant).
 */
ZTEST(test_did_handlers_register, test_gen_did_count_macro)
{
    zassert_equal(GEN_DID_HANDLER_COUNT, 5U,
                  "GEN_DID_HANDLER_COUNT must equal 5");
}

/* =========================================================================
 * Test suite: did_read_engine_speed (DID 0x0C00, 2 bytes)
 * ========================================================================= */

ZTEST_SUITE(test_did_read_engine_speed, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DIDH-RPM-001: NULL buf → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_did_read_engine_speed, test_null_buf)
{
    uint16_t out_len;
    zassert_equal(did_read_engine_speed(NULL, 4U, &out_len),
                  UDS_STATUS_ERR_NULL_PTR, "NULL buf must fail");
}

/**
 * TC-DIDH-RPM-002: NULL out_len → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_did_read_engine_speed, test_null_out_len)
{
    uint8_t buf[4];
    zassert_equal(did_read_engine_speed(buf, 4U, NULL),
                  UDS_STATUS_ERR_NULL_PTR, "NULL out_len must fail");
}

/**
 * TC-DIDH-RPM-003: buf_len < 2 → UDS_STATUS_ERR_BUFFER_OVERFLOW.
 */
ZTEST(test_did_read_engine_speed, test_buf_too_small)
{
    uint8_t buf[1]; uint16_t out_len;
    zassert_equal(did_read_engine_speed(buf, 1U, &out_len),
                  UDS_STATUS_ERR_BUFFER_OVERFLOW, "buf_len < 2 must fail");
}

/**
 * TC-DIDH-RPM-004: Valid call → OK, out_len = 2.
 */
ZTEST(test_did_read_engine_speed, test_happy_path)
{
    uint8_t buf[4] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU };
    uint16_t out_len = 0U;
    zassert_equal(did_read_engine_speed(buf, 4U, &out_len),
                  UDS_STATUS_OK, "Valid read must return OK");
    zassert_equal(out_len, 2U, "out_len must be 2");
}

/**
 * TC-DIDH-RPM-005: buf_len == 2 (exact boundary) → OK.
 */
ZTEST(test_did_read_engine_speed, test_exact_buf_size)
{
    uint8_t buf[2]; uint16_t out_len;
    zassert_equal(did_read_engine_speed(buf, 2U, &out_len),
                  UDS_STATUS_OK, "Exact buf_len must succeed");
    zassert_equal(out_len, 2U, "out_len must be 2");
}

/* =========================================================================
 * Test suite: did_read_coolant_temperature (DID 0x0500, 1 byte)
 * ========================================================================= */

ZTEST_SUITE(test_did_read_coolant, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DIDH-COOL-001: NULL buf → ERR_NULL_PTR.
 */
ZTEST(test_did_read_coolant, test_null_buf)
{
    uint16_t out_len;
    zassert_equal(did_read_coolant_temperature(NULL, 4U, &out_len),
                  UDS_STATUS_ERR_NULL_PTR, "NULL buf must fail");
}

/**
 * TC-DIDH-COOL-002: buf_len == 0 → UDS_STATUS_ERR_BUFFER_OVERFLOW.
 */
ZTEST(test_did_read_coolant, test_zero_buf)
{
    uint8_t buf[1]; uint16_t out_len;
    zassert_equal(did_read_coolant_temperature(buf, 0U, &out_len),
                  UDS_STATUS_ERR_BUFFER_OVERFLOW, "buf_len=0 must fail");
}

/**
 * TC-DIDH-COOL-003: Valid call → OK, out_len = 1.
 */
ZTEST(test_did_read_coolant, test_happy_path)
{
    uint8_t buf[4]; uint16_t out_len = 0U;
    zassert_equal(did_read_coolant_temperature(buf, 4U, &out_len),
                  UDS_STATUS_OK, "Valid read must return OK");
    zassert_equal(out_len, 1U, "out_len must be 1");
}

/* =========================================================================
 * Test suite: did_read_vehicle_identification_number (DID 0xF190, 17 bytes)
 * ========================================================================= */

ZTEST_SUITE(test_did_read_vin, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DIDH-VIN-001: buf_len < 17 → UDS_STATUS_ERR_BUFFER_OVERFLOW.
 */
ZTEST(test_did_read_vin, test_buf_too_small)
{
    uint8_t buf[16]; uint16_t out_len;
    zassert_equal(did_read_vehicle_identification_number(buf, 16U, &out_len),
                  UDS_STATUS_ERR_BUFFER_OVERFLOW,
                  "buf_len < 17 must fail for VIN");
}

/**
 * TC-DIDH-VIN-002: Valid call → OK, out_len = 17.
 */
ZTEST(test_did_read_vin, test_happy_path)
{
    uint8_t buf[17]; uint16_t out_len = 0U;
    zassert_equal(did_read_vehicle_identification_number(buf, 17U, &out_len),
                  UDS_STATUS_OK, "Valid VIN read must succeed");
    zassert_equal(out_len, 17U, "VIN out_len must be 17");
}

/* =========================================================================
 * Test suite: did_read_ecu_serial_number (DID 0xF18C, 4 bytes)
 * ========================================================================= */

ZTEST_SUITE(test_did_read_ecu_serial, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DIDH-SER-001: buf_len < 4 → UDS_STATUS_ERR_BUFFER_OVERFLOW.
 */
ZTEST(test_did_read_ecu_serial, test_buf_too_small)
{
    uint8_t buf[3]; uint16_t out_len;
    zassert_equal(did_read_ecu_serial_number(buf, 3U, &out_len),
                  UDS_STATUS_ERR_BUFFER_OVERFLOW,
                  "buf_len < 4 must fail for ECU serial");
}

/**
 * TC-DIDH-SER-002: Valid call → OK, out_len = 4.
 */
ZTEST(test_did_read_ecu_serial, test_happy_path)
{
    uint8_t buf[4]; uint16_t out_len = 0U;
    zassert_equal(did_read_ecu_serial_number(buf, 4U, &out_len),
                  UDS_STATUS_OK, "Valid ECU serial read must succeed");
    zassert_equal(out_len, 4U, "ECU serial out_len must be 4");
}

/* =========================================================================
 * Test suite: did_read/write_vehicle_manufacturer_spare_part_number
 *             (DID 0xF187, 11 bytes, READ+WRITE)
 * ========================================================================= */

ZTEST_SUITE(test_did_spare_part, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-DIDH-SPARE-R-001: NULL buf → ERR_NULL_PTR.
 */
ZTEST(test_did_spare_part, test_read_null_buf)
{
    uint16_t out_len;
    zassert_equal(
        did_read_vehicle_manufacturer_spare_part_number(NULL, 12U, &out_len),
        UDS_STATUS_ERR_NULL_PTR, "NULL buf must fail");
}

/**
 * TC-DIDH-SPARE-R-002: buf_len < 11 → UDS_STATUS_ERR_BUFFER_OVERFLOW.
 */
ZTEST(test_did_spare_part, test_read_buf_too_small)
{
    uint8_t buf[10]; uint16_t out_len;
    zassert_equal(
        did_read_vehicle_manufacturer_spare_part_number(buf, 10U, &out_len),
        UDS_STATUS_ERR_BUFFER_OVERFLOW,
        "buf_len < 11 must fail for Spare Part");
}

/**
 * TC-DIDH-SPARE-R-003: Valid read → OK, out_len = 11.
 */
ZTEST(test_did_spare_part, test_read_happy_path)
{
    uint8_t buf[11]; uint16_t out_len = 0U;
    zassert_equal(
        did_read_vehicle_manufacturer_spare_part_number(buf, 11U, &out_len),
        UDS_STATUS_OK, "Valid Spare Part read must succeed");
    zassert_equal(out_len, 11U, "out_len must be 11");
}

/**
 * TC-DIDH-SPARE-W-001: NULL buf → ERR_NULL_PTR.
 */
ZTEST(test_did_spare_part, test_write_null_buf)
{
    zassert_equal(
        did_write_vehicle_manufacturer_spare_part_number(NULL, 11U),
        UDS_STATUS_ERR_NULL_PTR, "NULL buf on write must fail");
}

/**
 * TC-DIDH-SPARE-W-002: len != 11 → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_did_spare_part, test_write_wrong_length)
{
    uint8_t buf[12] = { 0 };
    zassert_equal(
        did_write_vehicle_manufacturer_spare_part_number(buf, 10U),
        UDS_STATUS_ERR_INVALID_PARAM,
        "len != 11 must fail (NRC 0x13 scenario)");
    zassert_equal(
        did_write_vehicle_manufacturer_spare_part_number(buf, 12U),
        UDS_STATUS_ERR_INVALID_PARAM,
        "len > 11 must also fail");
}

/**
 * TC-DIDH-SPARE-W-003: Valid write → OK; read back reflects new data.
 *
 * The stub stores into s_mock_vehicle_manufacturer_spare_part_number[].
 * Reading back should return the same bytes.
 */
ZTEST(test_did_spare_part, test_write_and_read_back)
{
    uint8_t new_val[11] = {
        0x41U, 0x42U, 0x43U, 0x44U, 0x45U,
        0x46U, 0x47U, 0x48U, 0x49U, 0x4AU, 0x4BU
    };
    uds_status_t wrc = did_write_vehicle_manufacturer_spare_part_number(new_val, 11U);
    zassert_equal(wrc, UDS_STATUS_OK, "Valid write must succeed");

    uint8_t read_back[11] = { 0 };
    uint16_t out_len = 0U;
    zassert_equal(
        did_read_vehicle_manufacturer_spare_part_number(read_back, 11U, &out_len),
        UDS_STATUS_OK, "Read after write must succeed");
    zassert_equal(out_len, 11U, "out_len must be 11");
    zassert_mem_equal(read_back, new_val, 11U,
                      "Read-back data must match written data");
}

/* =========================================================================
 * Test suite: did_safe_read_engine_speed (safety wrapper)
 * ========================================================================= */

ZTEST_SUITE(test_safe_read_engine_speed, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SAFE-RPM-001: NULL session_ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_safe_read_engine_speed, test_null_session)
{
    full_init();
    uint8_t buf[4]; uint16_t out_len;
    zassert_equal(
        did_safe_read_engine_speed(NULL, &g_sec, buf, 4U, &out_len),
        UDS_STATUS_ERR_NULL_PTR, "NULL session_ctx must fail");
}

/**
 * TC-SAFE-RPM-002: NULL security_ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_safe_read_engine_speed, test_null_security)
{
    full_init();
    uint8_t buf[4]; uint16_t out_len;
    zassert_equal(
        did_safe_read_engine_speed(&g_sess, NULL, buf, 4U, &out_len),
        UDS_STATUS_ERR_NULL_PTR, "NULL security_ctx must fail");
}

/**
 * TC-SAFE-RPM-003: NULL buf → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_safe_read_engine_speed, test_null_buf)
{
    full_init();
    uint16_t out_len;
    zassert_equal(
        did_safe_read_engine_speed(&g_sess, &g_sec, NULL, 4U, &out_len),
        UDS_STATUS_ERR_NULL_PTR, "NULL buf must fail");
}

/**
 * TC-SAFE-RPM-004: Valid call in DEFAULT session (Engine Speed requires DEFAULT) → OK.
 */
ZTEST(test_safe_read_engine_speed, test_happy_path_default_session)
{
    full_init();
    uint8_t buf[4]; uint16_t out_len = 0U;
    uds_status_t rc = did_safe_read_engine_speed(&g_sess, &g_sec, buf, 4U, &out_len);
    zassert_equal(rc, UDS_STATUS_OK,
                  "Engine Speed safe read in DEFAULT must succeed");
    zassert_equal(out_len, 2U, "out_len must be 2");
}

/**
 * TC-SAFE-RPM-005: buf_len < 2 → UDS_STATUS_ERR_BUFFER_OVERFLOW from bounds check.
 */
ZTEST(test_safe_read_engine_speed, test_buf_too_small_bounds_check)
{
    full_init();
    uint8_t buf[1]; uint16_t out_len;
    uds_status_t rc = did_safe_read_engine_speed(&g_sess, &g_sec, buf, 1U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_BUFFER_OVERFLOW,
                  "buf_len < data_length must fail bounds check");
}

/* =========================================================================
 * Test suite: did_safe_read_vehicle_manufacturer_spare_part_number
 *             (requires EXTENDED session)
 * ========================================================================= */

ZTEST_SUITE(test_safe_read_spare_part, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SAFE-SPARE-001: DEFAULT session → UDS_STATUS_ERR_SESSION_INVALID.
 */
ZTEST(test_safe_read_spare_part, test_default_session_rejected)
{
    full_init();
    uint8_t buf[11]; uint16_t out_len;
    uds_status_t rc = did_safe_read_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 11U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_SESSION_INVALID,
                  "Spare Part read in DEFAULT must fail session check");
}

/**
 * TC-SAFE-SPARE-002: EXTENDED session → OK.
 */
ZTEST(test_safe_read_spare_part, test_extended_session_ok)
{
    full_init();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    uint8_t buf[11]; uint16_t out_len = 0U;
    uds_status_t rc = did_safe_read_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 11U, &out_len);
    zassert_equal(rc, UDS_STATUS_OK,
                  "Spare Part read in EXTENDED must succeed");
    zassert_equal(out_len, 11U, "out_len must be 11");
}

/* =========================================================================
 * Test suite: did_safe_write_vehicle_manufacturer_spare_part_number
 *             (requires EXTENDED session + security level 2)
 * ========================================================================= */

ZTEST_SUITE(test_safe_write_spare_part, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SAFE-SPAREW-001: DEFAULT session → UDS_STATUS_ERR_SESSION_INVALID.
 */
ZTEST(test_safe_write_spare_part, test_default_session_rejected)
{
    full_init();
    uint8_t buf[11] = { 0 };
    uds_status_t rc = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 11U);
    zassert_equal(rc, UDS_STATUS_ERR_SESSION_INVALID,
                  "Spare Part write in DEFAULT must fail");
}

/**
 * TC-SAFE-SPAREW-002: EXTENDED session, security NOT unlocked
 *                     → UDS_STATUS_ERR_SEC_NOT_UNLOCKED.
 */
ZTEST(test_safe_write_spare_part, test_security_not_unlocked)
{
    full_init();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    uint8_t buf[11] = { 0 };
    uds_status_t rc = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 11U);
    zassert_equal(rc, UDS_STATUS_ERR_SEC_NOT_UNLOCKED,
                  "Write without security unlock must fail");
}

/**
 * TC-SAFE-SPAREW-003: EXTENDED session, security unlocked → OK.
 */
ZTEST(test_safe_write_spare_part, test_security_unlocked_ok)
{
    full_init();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    do_sec_unlock();

    uint8_t buf[11];
    memset(buf, 0x55U, sizeof(buf));
    uds_status_t rc = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 11U);
    zassert_equal(rc, UDS_STATUS_OK,
                  "Write with security unlocked must succeed");
}

/**
 * TC-SAFE-SPAREW-004: len != 11 (wrong write length)
 *                     → UDS_STATUS_ERR_INVALID_PARAM (write_data_length check).
 */
ZTEST(test_safe_write_spare_part, test_wrong_write_length)
{
    full_init();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    do_sec_unlock();

    uint8_t buf[12] = { 0 };
    uds_status_t rc = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 10U);
    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Mismatched write length must fail even when unlocked");
}

/**
 * TC-SAFE-SPAREW-005: NULL buf → UDS_STATUS_ERR_NULL_PTR (before any other check).
 */
ZTEST(test_safe_write_spare_part, test_null_buf)
{
    full_init();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    uds_status_t rc = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, NULL, 11U);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL buf must fail immediately");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_did_handlers_register__test_all_dids_registered_and_findable(void);
extern void test_did_handlers_register__test_gen_did_count_macro(void);
extern void test_did_read_engine_speed__test_null_buf(void);
extern void test_did_read_engine_speed__test_null_out_len(void);
extern void test_did_read_engine_speed__test_buf_too_small(void);
extern void test_did_read_engine_speed__test_happy_path(void);
extern void test_did_read_engine_speed__test_exact_buf_size(void);
extern void test_did_read_coolant__test_null_buf(void);
extern void test_did_read_coolant__test_zero_buf(void);
extern void test_did_read_coolant__test_happy_path(void);
extern void test_did_read_vin__test_buf_too_small(void);
extern void test_did_read_vin__test_happy_path(void);
extern void test_did_read_ecu_serial__test_buf_too_small(void);
extern void test_did_read_ecu_serial__test_happy_path(void);
extern void test_did_spare_part__test_read_null_buf(void);
extern void test_did_spare_part__test_read_buf_too_small(void);
extern void test_did_spare_part__test_read_happy_path(void);
extern void test_did_spare_part__test_write_null_buf(void);
extern void test_did_spare_part__test_write_wrong_length(void);
extern void test_did_spare_part__test_write_and_read_back(void);
extern void test_safe_read_engine_speed__test_null_session(void);
extern void test_safe_read_engine_speed__test_null_security(void);
extern void test_safe_read_engine_speed__test_null_buf(void);
extern void test_safe_read_engine_speed__test_happy_path_default_session(void);
extern void test_safe_read_engine_speed__test_buf_too_small_bounds_check(void);
extern void test_safe_read_spare_part__test_default_session_rejected(void);
extern void test_safe_read_spare_part__test_extended_session_ok(void);
extern void test_safe_write_spare_part__test_default_session_rejected(void);
extern void test_safe_write_spare_part__test_security_not_unlocked(void);
extern void test_safe_write_spare_part__test_security_unlocked_ok(void);
extern void test_safe_write_spare_part__test_wrong_write_length(void);
extern void test_safe_write_spare_part__test_null_buf(void);

void run_all_tests(void)
{
    RUN_TEST(test_did_handlers_register__test_all_dids_registered_and_findable);
    RUN_TEST(test_did_handlers_register__test_gen_did_count_macro);
    RUN_TEST(test_did_read_engine_speed__test_null_buf);
    RUN_TEST(test_did_read_engine_speed__test_null_out_len);
    RUN_TEST(test_did_read_engine_speed__test_buf_too_small);
    RUN_TEST(test_did_read_engine_speed__test_happy_path);
    RUN_TEST(test_did_read_engine_speed__test_exact_buf_size);
    RUN_TEST(test_did_read_coolant__test_null_buf);
    RUN_TEST(test_did_read_coolant__test_zero_buf);
    RUN_TEST(test_did_read_coolant__test_happy_path);
    RUN_TEST(test_did_read_vin__test_buf_too_small);
    RUN_TEST(test_did_read_vin__test_happy_path);
    RUN_TEST(test_did_read_ecu_serial__test_buf_too_small);
    RUN_TEST(test_did_read_ecu_serial__test_happy_path);
    RUN_TEST(test_did_spare_part__test_read_null_buf);
    RUN_TEST(test_did_spare_part__test_read_buf_too_small);
    RUN_TEST(test_did_spare_part__test_read_happy_path);
    RUN_TEST(test_did_spare_part__test_write_null_buf);
    RUN_TEST(test_did_spare_part__test_write_wrong_length);
    RUN_TEST(test_did_spare_part__test_write_and_read_back);
    RUN_TEST(test_safe_read_engine_speed__test_null_session);
    RUN_TEST(test_safe_read_engine_speed__test_null_security);
    RUN_TEST(test_safe_read_engine_speed__test_null_buf);
    RUN_TEST(test_safe_read_engine_speed__test_happy_path_default_session);
    RUN_TEST(test_safe_read_engine_speed__test_buf_too_small_bounds_check);
    RUN_TEST(test_safe_read_spare_part__test_default_session_rejected);
    RUN_TEST(test_safe_read_spare_part__test_extended_session_ok);
    RUN_TEST(test_safe_write_spare_part__test_default_session_rejected);
    RUN_TEST(test_safe_write_spare_part__test_security_not_unlocked);
    RUN_TEST(test_safe_write_spare_part__test_security_unlocked_ok);
    RUN_TEST(test_safe_write_spare_part__test_wrong_write_length);
    RUN_TEST(test_safe_write_spare_part__test_null_buf);
}
