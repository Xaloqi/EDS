// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit/test_did_safety_wrappers.c
 *
 * MODULE UNDER TEST: generated/did_safety_wrappers.c
 *
 * PURPOSE:
 *   Focused ASIL-B validation of every per-DID safe accessor function.
 *   Complements test_did_handlers.c (raw callbacks) and test_uds_safety.c
 *   (generic safety engine) by testing the GENERATED wrapper glue specifically.
 *
 * TEST CASES — did_safe_read_engine_speed (0x0C00, 2 bytes, DEFAULT):
 *   TC-WRAP-001  NULL session_ctx  → ERR_NULL_PTR
 *   TC-WRAP-002  NULL security_ctx → ERR_NULL_PTR
 *   TC-WRAP-003  NULL buf          → ERR_NULL_PTR
 *   TC-WRAP-004  NULL out_len      → ERR_NULL_PTR
 *   TC-WRAP-005  DEFAULT session, no lock → OK, out_len=2
 *   TC-WRAP-006  buf_len < 2       → ERR_BUFFER_OVERFLOW (bounds gate)
 *
 * TEST CASES — did_safe_read_coolant_temperature (0x0500, 1 byte, DEFAULT):
 *   TC-WRAP-007  DEFAULT session → OK, out_len=1
 *   TC-WRAP-008  buf_len == 0    → ERR_BUFFER_OVERFLOW
 *
 * TEST CASES — did_safe_read_vehicle_identification_number (0xF190, 17 bytes, DEFAULT):
 *   TC-WRAP-009  DEFAULT session, buf=17 → OK, out_len=17
 *   TC-WRAP-010  buf_len < 17           → ERR_BUFFER_OVERFLOW
 *
 * TEST CASES — did_safe_read_ecu_serial_number (0xF18C, 4 bytes, DEFAULT):
 *   TC-WRAP-011  DEFAULT session, buf=4 → OK
 *   TC-WRAP-012  buf_len < 4           → ERR_BUFFER_OVERFLOW
 *
 * TEST CASES — did_safe_read_vehicle_manufacturer_spare_part_number
 *              (0xF187, 11 bytes, EXTENDED, read_level=0):
 *   TC-WRAP-013  DEFAULT session       → ERR_SESSION_INVALID
 *   TC-WRAP-014  EXTENDED session      → OK, out_len=11
 *   TC-WRAP-015  buf_len < 11 in EXTENDED → ERR_BUFFER_OVERFLOW
 *
 * TEST CASES — did_safe_write_vehicle_manufacturer_spare_part_number
 *              (0xF187, 11 bytes, EXTENDED, write_level=2):
 *   TC-WRAP-016  NULL session_ctx  → ERR_NULL_PTR
 *   TC-WRAP-017  NULL buf          → ERR_NULL_PTR
 *   TC-WRAP-018  DEFAULT session   → ERR_SESSION_INVALID
 *   TC-WRAP-019  EXTENDED, sec NOT unlocked → ERR_SEC_NOT_UNLOCKED
 *   TC-WRAP-020  EXTENDED, sec unlocked → OK
 *   TC-WRAP-021  EXTENDED, unlocked, len != 11 → ERR_INVALID_PARAM
 *   TC-WRAP-022  Write then read confirms data stored
 *
 * FRAMEWORK: Zephyr Ztest
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "did_safety_wrappers.h"
#include "did_handlers.h"
#include "did_database.h"
#include "uds_safety.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_types.h"

/* =========================================================================
 * Security stubs
 * ========================================================================= */

static uds_status_t w_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l;
    for (uint8_t i = 0; i < n && i < 4U; i++) { b[i] = (uint8_t)(0x10U + i); }
    *o = (n < 4U) ? n : 4U;
    return UDS_STATUS_OK;
}
static bool w_key(uint8_t l, const uint8_t *s, uint8_t sl,
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
 * Shared state
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static bool               g_ready = false;

static void stack_init(void)
{
    if (!g_ready) {
        uds_safety_init();
        uds_safety_reset_counters();
        (void)did_database_init();
        (void)did_handlers_register_all();
        g_ready = true;
    }
    uds_safety_reset_counters();

    memset(&g_sess, 0, sizeof(g_sess));
    uds_session_init(&g_sess, 5000U);

    memset(&g_sec, 0, sizeof(g_sec));
    static const uds_security_cfg_t sc = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,
        .key_validate_cb  = w_key,
        .seed_generate_cb = w_seed,
    };
    uds_security_init(&g_sec, &sc);
}

static void go_extended(void)
{
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
}

static void do_unlock(void)
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
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_did_safety_wrappers, NULL, NULL, NULL, NULL, NULL);

/* --- Engine Speed (0x0C00) ------------------------------------------------ */

/**
 * TC-WRAP-001: NULL session_ctx → ERR_NULL_PTR (first NULL guard).
 */
ZTEST(test_did_safety_wrappers, tc001_engine_speed_null_session)
{
    stack_init();
    uint8_t buf[4]; uint16_t out_len;
    uds_status_t rc = did_safe_read_engine_speed(NULL, &g_sec, buf, 4U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL session_ctx must fail");
}

/**
 * TC-WRAP-002: NULL security_ctx → ERR_NULL_PTR.
 */
ZTEST(test_did_safety_wrappers, tc002_engine_speed_null_security)
{
    stack_init();
    uint8_t buf[4]; uint16_t out_len;
    uds_status_t rc = did_safe_read_engine_speed(&g_sess, NULL, buf, 4U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL security_ctx must fail");
}

/**
 * TC-WRAP-003: NULL buf → ERR_NULL_PTR.
 */
ZTEST(test_did_safety_wrappers, tc003_engine_speed_null_buf)
{
    stack_init();
    uint16_t out_len;
    uds_status_t rc = did_safe_read_engine_speed(&g_sess, &g_sec, NULL, 4U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL buf must fail");
}

/**
 * TC-WRAP-004: NULL out_len → ERR_NULL_PTR.
 */
ZTEST(test_did_safety_wrappers, tc004_engine_speed_null_out_len)
{
    stack_init();
    uint8_t buf[4];
    uds_status_t rc = did_safe_read_engine_speed(&g_sess, &g_sec, buf, 4U, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL out_len must fail");
}

/**
 * TC-WRAP-005: DEFAULT session, no security required, buf_len >= 2 → OK, out_len=2.
 */
ZTEST(test_did_safety_wrappers, tc005_engine_speed_ok)
{
    stack_init();
    uint8_t buf[4]; uint16_t out_len = 0U;
    uds_status_t rc = did_safe_read_engine_speed(&g_sess, &g_sec, buf, 4U, &out_len);
    zassert_equal(rc, UDS_STATUS_OK, "Engine Speed safe read must succeed");
    zassert_equal(out_len, 2U, "out_len must be 2");
}

/**
 * TC-WRAP-006: buf_len < 2 → ERR_BUFFER_OVERFLOW (bounds check gate).
 */
ZTEST(test_did_safety_wrappers, tc006_engine_speed_buf_too_small)
{
    stack_init();
    uint8_t buf[1]; uint16_t out_len;
    uds_status_t rc = did_safe_read_engine_speed(&g_sess, &g_sec, buf, 1U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_BUFFER_OVERFLOW,
                  "buf_len < 2 must fail bounds check");
}

/* --- Coolant Temperature (0x0500) ---------------------------------------- */

/**
 * TC-WRAP-007: DEFAULT session, buf_len=4 → OK, out_len=1.
 */
ZTEST(test_did_safety_wrappers, tc007_coolant_temp_ok)
{
    stack_init();
    uint8_t buf[4]; uint16_t out_len = 0U;
    uds_status_t rc = did_safe_read_coolant_temperature(&g_sess, &g_sec, buf, 4U, &out_len);
    zassert_equal(rc, UDS_STATUS_OK, "Coolant Temp safe read must succeed");
    zassert_equal(out_len, 1U, "out_len must be 1");
}

/**
 * TC-WRAP-008: buf_len == 0 → ERR_BUFFER_OVERFLOW.
 */
ZTEST(test_did_safety_wrappers, tc008_coolant_temp_zero_buf)
{
    stack_init();
    uint8_t buf[1]; uint16_t out_len;
    uds_status_t rc = did_safe_read_coolant_temperature(&g_sess, &g_sec, buf, 0U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_BUFFER_OVERFLOW,
                  "buf_len=0 must fail");
}

/* --- VIN (0xF190) --------------------------------------------------------- */

/**
 * TC-WRAP-009: DEFAULT session, buf_len=17 → OK, out_len=17.
 */
ZTEST(test_did_safety_wrappers, tc009_vin_ok)
{
    stack_init();
    uint8_t buf[17]; uint16_t out_len = 0U;
    uds_status_t rc = did_safe_read_vehicle_identification_number(
        &g_sess, &g_sec, buf, 17U, &out_len);
    zassert_equal(rc, UDS_STATUS_OK, "VIN safe read must succeed");
    zassert_equal(out_len, 17U, "out_len must be 17");
}

/**
 * TC-WRAP-010: buf_len < 17 → ERR_BUFFER_OVERFLOW.
 */
ZTEST(test_did_safety_wrappers, tc010_vin_buf_too_small)
{
    stack_init();
    uint8_t buf[16]; uint16_t out_len;
    uds_status_t rc = did_safe_read_vehicle_identification_number(
        &g_sess, &g_sec, buf, 16U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_BUFFER_OVERFLOW,
                  "buf_len < 17 must fail bounds check");
}

/* --- ECU Serial Number (0xF18C) ------------------------------------------ */

/**
 * TC-WRAP-011: DEFAULT session, buf_len=4 → OK.
 */
ZTEST(test_did_safety_wrappers, tc011_ecu_serial_ok)
{
    stack_init();
    uint8_t buf[4]; uint16_t out_len = 0U;
    uds_status_t rc = did_safe_read_ecu_serial_number(&g_sess, &g_sec, buf, 4U, &out_len);
    zassert_equal(rc, UDS_STATUS_OK, "ECU Serial safe read must succeed");
    zassert_equal(out_len, 4U, "out_len must be 4");
}

/**
 * TC-WRAP-012: buf_len < 4 → ERR_BUFFER_OVERFLOW.
 */
ZTEST(test_did_safety_wrappers, tc012_ecu_serial_buf_too_small)
{
    stack_init();
    uint8_t buf[3]; uint16_t out_len;
    uds_status_t rc = did_safe_read_ecu_serial_number(&g_sess, &g_sec, buf, 3U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_BUFFER_OVERFLOW,
                  "buf_len < 4 must fail");
}

/* --- Spare Part Number (0xF187) — READ ------------------------------------- */

/**
 * TC-WRAP-013: DEFAULT session → ERR_SESSION_INVALID (requires EXTENDED).
 */
ZTEST(test_did_safety_wrappers, tc013_spare_part_read_default_fails)
{
    stack_init(); /* session = DEFAULT */
    uint8_t buf[11]; uint16_t out_len;
    uds_status_t rc = did_safe_read_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 11U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_SESSION_INVALID,
                  "Spare Part read in DEFAULT must fail session check");
}

/**
 * TC-WRAP-014: EXTENDED session, no security required for read → OK, out_len=11.
 */
ZTEST(test_did_safety_wrappers, tc014_spare_part_read_extended_ok)
{
    stack_init();
    go_extended();
    uint8_t buf[11]; uint16_t out_len = 0U;
    uds_status_t rc = did_safe_read_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 11U, &out_len);
    zassert_equal(rc, UDS_STATUS_OK,
                  "Spare Part read in EXTENDED (read_level=0) must succeed");
    zassert_equal(out_len, 11U, "out_len must be 11");
}

/**
 * TC-WRAP-015: EXTENDED session, buf_len < 11 → ERR_BUFFER_OVERFLOW.
 */
ZTEST(test_did_safety_wrappers, tc015_spare_part_read_buf_small)
{
    stack_init();
    go_extended();
    uint8_t buf[10]; uint16_t out_len;
    uds_status_t rc = did_safe_read_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 10U, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_BUFFER_OVERFLOW,
                  "buf_len < 11 must fail bounds check");
}

/* --- Spare Part Number (0xF187) — WRITE ------------------------------------ */

/**
 * TC-WRAP-016: NULL session_ctx on write → ERR_NULL_PTR.
 */
ZTEST(test_did_safety_wrappers, tc016_spare_write_null_session)
{
    stack_init();
    uint8_t buf[11] = { 0 };
    uds_status_t rc = did_safe_write_vehicle_manufacturer_spare_part_number(
        NULL, &g_sec, buf, 11U);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL session_ctx must fail");
}

/**
 * TC-WRAP-017: NULL buf on write → ERR_NULL_PTR.
 */
ZTEST(test_did_safety_wrappers, tc017_spare_write_null_buf)
{
    stack_init();
    go_extended();
    uds_status_t rc = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, NULL, 11U);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL buf must fail");
}

/**
 * TC-WRAP-018: DEFAULT session → ERR_SESSION_INVALID.
 */
ZTEST(test_did_safety_wrappers, tc018_spare_write_default_session_fails)
{
    stack_init(); /* DEFAULT */
    uint8_t buf[11] = { 0 };
    uds_status_t rc = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 11U);
    zassert_equal(rc, UDS_STATUS_ERR_SESSION_INVALID,
                  "Write in DEFAULT must fail session check");
}

/**
 * TC-WRAP-019: EXTENDED, security NOT unlocked → ERR_SEC_NOT_UNLOCKED.
 */
ZTEST(test_did_safety_wrappers, tc019_spare_write_not_unlocked)
{
    stack_init();
    go_extended();
    uint8_t buf[11] = { 0 };
    uds_status_t rc = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 11U);
    zassert_equal(rc, UDS_STATUS_ERR_SEC_NOT_UNLOCKED,
                  "Write without security unlock must fail");
}

/**
 * TC-WRAP-020: EXTENDED, security unlocked → OK.
 */
ZTEST(test_did_safety_wrappers, tc020_spare_write_ok)
{
    stack_init();
    go_extended();
    do_unlock();

    uint8_t buf[11];
    memset(buf, 0x5AU, 11U);
    uds_status_t rc = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 11U);
    zassert_equal(rc, UDS_STATUS_OK,
                  "Write with EXTENDED+unlocked must succeed");
}

/**
 * TC-WRAP-021: EXTENDED, unlocked, len != 11 → ERR_INVALID_PARAM (write_data_length).
 */
ZTEST(test_did_safety_wrappers, tc021_spare_write_wrong_len)
{
    stack_init();
    go_extended();
    do_unlock();

    uint8_t buf[12] = { 0 };
    /* Try len=10 and len=12 — both must fail */
    uds_status_t rc10 = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 10U);
    uds_status_t rc12 = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, buf, 12U);

    zassert_equal(rc10, UDS_STATUS_ERR_INVALID_PARAM,
                  "len=10 must fail write_data_length check");
    zassert_equal(rc12, UDS_STATUS_ERR_INVALID_PARAM,
                  "len=12 must fail write_data_length check");
}

/**
 * TC-WRAP-022: Write 11 bytes → read back confirms same data stored.
 */
ZTEST(test_did_safety_wrappers, tc022_write_then_read_back)
{
    stack_init();
    go_extended();
    do_unlock();

    uint8_t written[11] = {
        0x31U, 0x32U, 0x33U, 0x34U, 0x35U,
        0x36U, 0x37U, 0x38U, 0x39U, 0x41U, 0x42U
    };
    uds_status_t wrc = did_safe_write_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, written, 11U);
    zassert_equal(wrc, UDS_STATUS_OK, "Write must succeed");

    uint8_t read_buf[11] = { 0 };
    uint16_t out_len = 0U;
    uds_status_t rrc = did_safe_read_vehicle_manufacturer_spare_part_number(
        &g_sess, &g_sec, read_buf, 11U, &out_len);
    zassert_equal(rrc, UDS_STATUS_OK, "Read-back must succeed");
    zassert_equal(out_len, 11U, "out_len must be 11");
    zassert_mem_equal(read_buf, written, 11U,
                      "Read-back data must exactly match written data");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_did_safety_wrappers__tc001_engine_speed_null_session(void);
extern void test_did_safety_wrappers__tc002_engine_speed_null_security(void);
extern void test_did_safety_wrappers__tc003_engine_speed_null_buf(void);
extern void test_did_safety_wrappers__tc004_engine_speed_null_out_len(void);
extern void test_did_safety_wrappers__tc005_engine_speed_ok(void);
extern void test_did_safety_wrappers__tc006_engine_speed_buf_too_small(void);
extern void test_did_safety_wrappers__tc007_coolant_temp_ok(void);
extern void test_did_safety_wrappers__tc008_coolant_temp_zero_buf(void);
extern void test_did_safety_wrappers__tc009_vin_ok(void);
extern void test_did_safety_wrappers__tc010_vin_buf_too_small(void);
extern void test_did_safety_wrappers__tc011_ecu_serial_ok(void);
extern void test_did_safety_wrappers__tc012_ecu_serial_buf_too_small(void);
extern void test_did_safety_wrappers__tc013_spare_part_read_default_fails(void);
extern void test_did_safety_wrappers__tc014_spare_part_read_extended_ok(void);
extern void test_did_safety_wrappers__tc015_spare_part_read_buf_small(void);
extern void test_did_safety_wrappers__tc016_spare_write_null_session(void);
extern void test_did_safety_wrappers__tc017_spare_write_null_buf(void);
extern void test_did_safety_wrappers__tc018_spare_write_default_session_fails(void);
extern void test_did_safety_wrappers__tc019_spare_write_not_unlocked(void);
extern void test_did_safety_wrappers__tc020_spare_write_ok(void);
extern void test_did_safety_wrappers__tc021_spare_write_wrong_len(void);
extern void test_did_safety_wrappers__tc022_write_then_read_back(void);

void run_all_tests(void)
{
    RUN_TEST(test_did_safety_wrappers__tc001_engine_speed_null_session);
    RUN_TEST(test_did_safety_wrappers__tc002_engine_speed_null_security);
    RUN_TEST(test_did_safety_wrappers__tc003_engine_speed_null_buf);
    RUN_TEST(test_did_safety_wrappers__tc004_engine_speed_null_out_len);
    RUN_TEST(test_did_safety_wrappers__tc005_engine_speed_ok);
    RUN_TEST(test_did_safety_wrappers__tc006_engine_speed_buf_too_small);
    RUN_TEST(test_did_safety_wrappers__tc007_coolant_temp_ok);
    RUN_TEST(test_did_safety_wrappers__tc008_coolant_temp_zero_buf);
    RUN_TEST(test_did_safety_wrappers__tc009_vin_ok);
    RUN_TEST(test_did_safety_wrappers__tc010_vin_buf_too_small);
    RUN_TEST(test_did_safety_wrappers__tc011_ecu_serial_ok);
    RUN_TEST(test_did_safety_wrappers__tc012_ecu_serial_buf_too_small);
    RUN_TEST(test_did_safety_wrappers__tc013_spare_part_read_default_fails);
    RUN_TEST(test_did_safety_wrappers__tc014_spare_part_read_extended_ok);
    RUN_TEST(test_did_safety_wrappers__tc015_spare_part_read_buf_small);
    RUN_TEST(test_did_safety_wrappers__tc016_spare_write_null_session);
    RUN_TEST(test_did_safety_wrappers__tc017_spare_write_null_buf);
    RUN_TEST(test_did_safety_wrappers__tc018_spare_write_default_session_fails);
    RUN_TEST(test_did_safety_wrappers__tc019_spare_write_not_unlocked);
    RUN_TEST(test_did_safety_wrappers__tc020_spare_write_ok);
    RUN_TEST(test_did_safety_wrappers__tc021_spare_write_wrong_len);
    RUN_TEST(test_did_safety_wrappers__tc022_write_then_read_back);
}
