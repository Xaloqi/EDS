// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_service_0x2E.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x2E.c
 *                    SID 0x2E — WriteDataByIdentifier
 *
 * PURPOSE:
 *   Verify all branches of the 0x2E handler: request-length validation,
 *   the 5-step ASIL-B write chain (NULL/DID/session/security/data-length),
 *   write-callback invocation and failure propagation, and positive-
 *   response encoding.
 *
 *   [EDS#195] Every other DID-access service (0x22, 0x23, 0x27, 0x28,
 *   0x2A, 0x2F) has a dedicated C unit test — 0x2E did not, despite
 *   being the primary write path and the one ASIL_B_REQUIRE_WRITE_SECURITY
 *   exists to protect. This file closes that gap.
 *
 * TEST CASES:
 *   TC-0x2E-001  NULL ctx                                → ERR_NULL_PTR
 *   TC-0x2E-002  Request < 4 bytes (no data byte)         → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x2E-003  DID not registered                       → ERR_DID_NOT_FOUND (NRC 0x31)
 *   TC-0x2E-004  Wrong session (DEFAULT, DID needs EXTENDED) → ERR_SESSION_INVALID (NRC 0x22)
 *   TC-0x2E-005  Wrong security (locked, DID needs level 1) → ERR_SEC_NOT_UNLOCKED (NRC 0x33)
 *   TC-0x2E-006  Wrong data length (short)                → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x2E-007  Wrong data length (long)                 → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x2E-008  write_cb == NULL                          → propagates write_cb's absence safely
 *   TC-0x2E-009  write_cb returns ERR_CONDITIONS_NOT_MET   → propagates (→ NRC 0x22)
 *   TC-0x2E-010  Valid write (level 0, no security needed) → OK, callback invoked with exact bytes
 *   TC-0x2E-011  Valid write (level 1, security unlocked)  → OK
 *   TC-0x2E-012  Boundary: 1-byte data_length write        → OK
 *   TC-0x2E-013  Response format: [0x6E, DID_hi, DID_lo], length == 3
 *   TC-0x2E-014  Two DIDs in sequence don't cross-contaminate write_cb args
 *
 * FRAMEWORK: Zephyr Ztest (host shim in tests/runner/ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "did_database.h"
#include "did_handlers.h"
#include "uds_types.h"

/* =========================================================================
 * Test DIDs
 *
 * DID 0xA010: writable, level 0 (no security needed), data_length = 2.
 * DID 0xA011: writable, EXTENDED session + security level 1 required,
 *             data_length = 2.
 * DID 0xA012: writable, level 0, data_length = 1 (boundary case).
 * DID 0xA013: DID_ACCESS_WRITE set but write_cb == NULL.
 * ========================================================================= */

#define TEST_DID_OPEN     (0xA010U)  /**< No security required. */
#define TEST_DID_GATED    (0xA011U)  /**< EXTENDED + level 1 required. */
#define TEST_DID_ONEBYTE  (0xA012U)  /**< data_length == 1 (boundary). */
#define TEST_DID_NO_CB    (0xA013U)  /**< write_cb == NULL. */
#define TEST_DATA_LEN     (2U)

/* =========================================================================
 * Mock write callback — records the last call's arguments
 * ========================================================================= */

static uint8_t  s_last_write_data[DID_MAX_DATA_LEN];
static uint16_t s_last_write_len;
static uint16_t s_write_call_count;
static uds_status_t s_mock_write_return;

static uds_status_t mock_write_cb(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    s_write_call_count++;
    s_last_write_len = len;
    for (i = (uint16_t)0U; i < len && i < (uint16_t)DID_MAX_DATA_LEN; i++) {
        s_last_write_data[i] = buf[i];
    }
    return s_mock_write_return;
}

/* =========================================================================
 * Stub read callback (required by did_entry_t; write-only DIDs still need
 * a non-NULL read_cb to be found by uds_safety_find_did() in some paths)
 * ========================================================================= */

static uds_status_t stub_read_cb(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    (void)buf; (void)buf_len;
    *out_len = (uint16_t)0U;
    return UDS_STATUS_OK;
}

/* =========================================================================
 * Stubs for security seed/key (required by uds_security_init)
 * ========================================================================= */

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l;
    for (uint8_t i = 0U; i < n && i < 4U; i++) { b[i] = (uint8_t)(0x10U + i); }
    *o = (n < 4U) ? n : 4U;
    return UDS_STATUS_OK;
}

static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl,
                  const uint8_t *k, uint8_t kl)
{
    (void)l;
    if (sl != kl) { return false; }
    for (uint8_t i = 0U; i < sl; i++) {
        if (k[i] != (uint8_t)(s[i] ^ (uint8_t)0xAAU)) { return false; }
    }
    return true;
}

/* =========================================================================
 * Context setup
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;
static bool               g_stack_ready = false;

static void register_test_dids(void)
{
    did_entry_t entry;

    /* DID 0xA010: writable, no security. */
    memset(&entry, 0, sizeof(entry));
    entry.did_id             = TEST_DID_OPEN;
    entry.access_flags       = (uint8_t)(DID_ACCESS_READ | DID_ACCESS_WRITE);
    entry.min_session        = (uint8_t)UDS_SESSION_DEFAULT;
    entry.read_access_level  = (uint8_t)0U;
    entry.write_access_level = (uint8_t)0U;
    entry.data_length        = (uint16_t)TEST_DATA_LEN;
    entry.read_cb            = stub_read_cb;
    entry.write_cb           = mock_write_cb;
    entry.description        = "Test open-write DID";
    (void)did_database_register(&entry);

    /* DID 0xA011: writable, EXTENDED session + security level 1. */
    memset(&entry, 0, sizeof(entry));
    entry.did_id             = TEST_DID_GATED;
    entry.access_flags       = (uint8_t)(DID_ACCESS_READ | DID_ACCESS_WRITE);
    entry.min_session        = (uint8_t)UDS_SESSION_EXTENDED;
    entry.read_access_level  = (uint8_t)0U;
    entry.write_access_level = (uint8_t)1U;
    entry.data_length        = (uint16_t)TEST_DATA_LEN;
    entry.read_cb            = stub_read_cb;
    entry.write_cb           = mock_write_cb;
    entry.description        = "Test gated-write DID";
    (void)did_database_register(&entry);

    /* DID 0xA012: writable, no security, data_length == 1 (boundary). */
    memset(&entry, 0, sizeof(entry));
    entry.did_id             = TEST_DID_ONEBYTE;
    entry.access_flags       = (uint8_t)(DID_ACCESS_READ | DID_ACCESS_WRITE);
    entry.min_session        = (uint8_t)UDS_SESSION_DEFAULT;
    entry.read_access_level  = (uint8_t)0U;
    entry.write_access_level = (uint8_t)0U;
    entry.data_length        = (uint16_t)1U;
    entry.read_cb            = stub_read_cb;
    entry.write_cb           = mock_write_cb;
    entry.description        = "Test 1-byte DID";
    (void)did_database_register(&entry);

    /* DID 0xA013: write flag set but write_cb == NULL. */
    memset(&entry, 0, sizeof(entry));
    entry.did_id             = TEST_DID_NO_CB;
    entry.access_flags       = (uint8_t)DID_ACCESS_WRITE;
    entry.min_session        = (uint8_t)UDS_SESSION_DEFAULT;
    entry.read_access_level  = (uint8_t)0U;
    entry.write_access_level = (uint8_t)0U;
    entry.data_length        = (uint16_t)TEST_DATA_LEN;
    entry.read_cb            = NULL;
    entry.write_cb           = NULL;
    entry.description        = "Test no-write-cb DID";
    (void)did_database_register(&entry);
}

static void setup(void)
{
    if (!g_stack_ready) {
        memset(&g_sess, 0, sizeof(g_sess));
        memset(&g_sec,  0, sizeof(g_sec));
        memset(&g_srv,  0, sizeof(g_srv));

        uds_session_init(&g_sess, 5000U);

        static const uds_security_cfg_t sc = {
            .max_attempts     = 3U,
            .lockout_ms       = 100U,
            .key_validate_cb  = t_key,
            .seed_generate_cb = t_seed,
        };
        uds_security_init(&g_sec, &sc);

        (void)did_database_init();
        (void)did_handlers_register_all();
        register_test_dids();

        static const uds_server_cfg_t svc = {
            .p2_server_max_ms      = 25U,
            .p2_star_server_max_ms = 5000U,
            .session_ctx           = &g_sess,
            .security_ctx          = &g_sec,
            .service_table         = g_uds_service_table,
            .service_table_count   = (uint8_t)UDS_SERVICE_TABLE_COUNT,
        };
        uds_server_init(&g_srv, &svc);
        g_stack_ready = true;
    }

    /* Reset session/security and clear mock state before each test. */
    uds_session_transition(&g_sess, UDS_SESSION_DEFAULT);
    g_sec.active_level     = (uint8_t)0U;
    s_write_call_count     = (uint16_t)0U;
    s_last_write_len       = (uint16_t)0U;
    s_mock_write_return    = UDS_STATUS_OK;
    memset(s_last_write_data, 0, sizeof(s_last_write_data));
}

/** Build a 0x2E request: [0x2E, DID_hi, DID_lo, data...]. */
static uds_msg_buf_t make_req(uint16_t did, const uint8_t *data, uint16_t data_len)
{
    uds_msg_buf_t r;
    uint16_t      i;

    memset(&r, 0, sizeof(r));
    r.data[0] = (uint8_t)UDS_SID_WRITE_DATA_BY_ID;
    r.data[1] = (uint8_t)((did >> 8U) & (uint8_t)0xFFU);
    r.data[2] = (uint8_t)(did & (uint8_t)0xFFU);
    for (i = (uint16_t)0U; i < data_len; i++) {
        r.data[3U + i] = data[i];
    }
    r.length = (uint16_t)3U + data_len;
    return r;
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_service_0x2E, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------- */

/**
 * TC-0x2E-001: NULL ctx → ERR_NULL_PTR.
 */
ZTEST(test_service_0x2E, tc001_null_ctx)
{
    setup();
    uint8_t       data[TEST_DATA_LEN] = { 0x11U, 0x22U };
    uds_msg_buf_t req  = make_req(TEST_DID_OPEN, data, (uint16_t)TEST_DATA_LEN);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(NULL, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must return ERR_NULL_PTR");
}

/**
 * TC-0x2E-002: Request < 4 bytes (SID+DID only, no data byte) → NRC 0x13.
 */
ZTEST(test_service_0x2E, tc002_request_too_short)
{
    setup();
    uds_msg_buf_t req = make_req(TEST_DID_OPEN, NULL, (uint16_t)0U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "request with no data byte must return ERR_INVALID_PARAM (NRC 0x13)");
    zassert_equal(s_write_call_count, (uint16_t)0U,
                  "write_cb must not be invoked when length validation fails");
}

/**
 * TC-0x2E-003: DID not registered → ERR_DID_NOT_FOUND (NRC 0x31).
 */
ZTEST(test_service_0x2E, tc003_did_not_found)
{
    setup();
    uint8_t       data[TEST_DATA_LEN] = { 0x11U, 0x22U };
    uds_msg_buf_t req  = make_req((uint16_t)0xFFFFU, data, (uint16_t)TEST_DATA_LEN);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_DID_NOT_FOUND,
                  "unregistered DID must return ERR_DID_NOT_FOUND (NRC 0x31)");
}

/**
 * TC-0x2E-004: Wrong session — DID needs EXTENDED, session is DEFAULT
 *              → ERR_SESSION_INVALID (NRC 0x22).
 */
ZTEST(test_service_0x2E, tc004_wrong_session)
{
    setup();
    uint8_t       data[TEST_DATA_LEN] = { 0x11U, 0x22U };
    uds_msg_buf_t req  = make_req(TEST_DID_GATED, data, (uint16_t)TEST_DATA_LEN);
    uds_msg_buf_t resp = {0};

    /* session stays DEFAULT (setup() default); TEST_DID_GATED needs EXTENDED */
    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SESSION_INVALID,
                  "wrong session must return ERR_SESSION_INVALID (NRC 0x22)");
    zassert_equal(s_write_call_count, (uint16_t)0U,
                  "write_cb must not be invoked when session check fails");
}

/**
 * TC-0x2E-005: Wrong security — DID needs level 1, security is locked
 *              → ERR_SEC_NOT_UNLOCKED (NRC 0x33).
 */
ZTEST(test_service_0x2E, tc005_wrong_security)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    /* g_sec.active_level stays 0 (locked) — TEST_DID_GATED needs level 1 */

    uint8_t       data[TEST_DATA_LEN] = { 0x11U, 0x22U };
    uds_msg_buf_t req  = make_req(TEST_DID_GATED, data, (uint16_t)TEST_DATA_LEN);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SEC_NOT_UNLOCKED,
                  "locked security must return ERR_SEC_NOT_UNLOCKED (NRC 0x33)");
    zassert_equal(s_write_call_count, (uint16_t)0U,
                  "write_cb must not be invoked when security check fails");
}

/**
 * TC-0x2E-006: Wrong data length (short: 1 byte for a 2-byte DID)
 *              → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x2E, tc006_data_length_short)
{
    setup();
    uint8_t       data[1] = { 0x11U };
    uds_msg_buf_t req  = make_req(TEST_DID_OPEN, data, (uint16_t)1U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "short data record must return ERR_INVALID_PARAM (NRC 0x13)");
    zassert_equal(s_write_call_count, (uint16_t)0U,
                  "write_cb must not be invoked when data-length check fails");
}

/**
 * TC-0x2E-007: Wrong data length (long: 3 bytes for a 2-byte DID)
 *              → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x2E, tc007_data_length_long)
{
    setup();
    uint8_t       data[3] = { 0x11U, 0x22U, 0x33U };
    uds_msg_buf_t req  = make_req(TEST_DID_OPEN, data, (uint16_t)3U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "long data record must return ERR_INVALID_PARAM (NRC 0x13)");
    zassert_equal(s_write_call_count, (uint16_t)0U,
                  "write_cb must not be invoked when data-length check fails");
}

/**
 * TC-0x2E-008: write_cb == NULL — must not crash; must return
 *              ERR_REQUEST_OUT_OF_RANGE (NRC 0x31), mirroring
 *              service_0x2F.c's identical io_control_cb-NULL precedent.
 *
 * This test originally only asserted "must not crash" — running it found
 * a real NULL-pointer dereference in s_did_safe_write() (fixed in the
 * same change as this test; see service_0x2E.c). Now pinned to the exact
 * expected status so a regression back to the unguarded call is caught
 * as a wrong-status failure, not just a crash.
 */
ZTEST(test_service_0x2E, tc008_write_cb_null)
{
    setup();
    uint8_t       data[TEST_DATA_LEN] = { 0x11U, 0x22U };
    uds_msg_buf_t req  = make_req(TEST_DID_NO_CB, data, (uint16_t)TEST_DATA_LEN);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "write_cb == NULL must return ERR_REQUEST_OUT_OF_RANGE (NRC 0x31), "
                  "not crash or silently succeed");
}

/**
 * TC-0x2E-009: write_cb returns ERR_CONDITIONS_NOT_MET → propagates (NRC 0x22).
 */
ZTEST(test_service_0x2E, tc009_write_cb_failure_propagates)
{
    setup();
    s_mock_write_return = UDS_STATUS_ERR_CONDITIONS_NOT_MET;

    uint8_t       data[TEST_DATA_LEN] = { 0x11U, 0x22U };
    uds_msg_buf_t req  = make_req(TEST_DID_OPEN, data, (uint16_t)TEST_DATA_LEN);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  "write_cb's ERR_CONDITIONS_NOT_MET must propagate (→ NRC 0x22)");
    zassert_equal(s_write_call_count, (uint16_t)1U,
                  "write_cb must have been called exactly once");
}

/**
 * TC-0x2E-010: Valid write, no security required → OK, callback sees the
 *              exact bytes sent.
 */
ZTEST(test_service_0x2E, tc010_valid_write_open)
{
    setup();
    uint8_t       data[TEST_DATA_LEN] = { 0xCAU, 0xFEU };
    uds_msg_buf_t req  = make_req(TEST_DID_OPEN, data, (uint16_t)TEST_DATA_LEN);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "valid write must return OK");
    zassert_equal(s_write_call_count, (uint16_t)1U, "write_cb must be called exactly once");
    zassert_equal(s_last_write_len, (uint16_t)TEST_DATA_LEN, "write_cb must see the full data length");
    zassert_equal(s_last_write_data[0], (uint8_t)0xCAU, "write_cb must see byte 0 unmodified");
    zassert_equal(s_last_write_data[1], (uint8_t)0xFEU, "write_cb must see byte 1 unmodified");
}

/**
 * TC-0x2E-011: Valid write, EXTENDED session + security unlocked → OK.
 */
ZTEST(test_service_0x2E, tc011_valid_write_gated)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    g_sec.active_level = (uint8_t)1U;  /* simulate a completed 0x27 unlock */

    uint8_t       data[TEST_DATA_LEN] = { 0x01U, 0x02U };
    uds_msg_buf_t req  = make_req(TEST_DID_GATED, data, (uint16_t)TEST_DATA_LEN);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,
                  "write to a security-gated DID must succeed once unlocked");
    zassert_equal(s_write_call_count, (uint16_t)1U, "write_cb must be called exactly once");
}

/**
 * TC-0x2E-012: Boundary — 1-byte data_length DID, exact-length write → OK.
 */
ZTEST(test_service_0x2E, tc012_boundary_one_byte)
{
    setup();
    uint8_t       data[1] = { 0x7FU };
    uds_msg_buf_t req  = make_req(TEST_DID_ONEBYTE, data, (uint16_t)1U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "exact 1-byte write must return OK");
    zassert_equal(s_last_write_len, (uint16_t)1U, "write_cb must see exactly 1 byte");
    zassert_equal(s_last_write_data[0], (uint8_t)0x7FU, "write_cb must see the correct byte");
}

/**
 * TC-0x2E-013: Response format: [0x6E, DID_hi, DID_lo], length == 3.
 */
ZTEST(test_service_0x2E, tc013_response_format)
{
    setup();
    uint8_t       data[TEST_DATA_LEN] = { 0x11U, 0x22U };
    uds_msg_buf_t req  = make_req(TEST_DID_OPEN, data, (uint16_t)TEST_DATA_LEN);
    uds_msg_buf_t resp = {0};

    (void)uds_service_0x2E_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], (uint8_t)0x6EU,
                  "resp[0] must be 0x6E (positive response for SID 0x2E)");
    zassert_equal(resp.data[1], (uint8_t)((TEST_DID_OPEN >> 8U) & 0xFFU),
                  "resp[1] must be the DID high byte");
    zassert_equal(resp.data[2], (uint8_t)(TEST_DID_OPEN & 0xFFU),
                  "resp[2] must be the DID low byte");
    zassert_equal(resp.length, (uint16_t)3U, "response length must be exactly 3");
}

/**
 * TC-0x2E-014: Two DIDs written in sequence don't cross-contaminate
 *              write_cb's recorded arguments (regression guard against
 *              any stale-buffer/aliasing bug between calls).
 */
ZTEST(test_service_0x2E, tc014_sequential_writes_no_cross_contamination)
{
    setup();
    uint8_t       data1[TEST_DATA_LEN] = { 0xAAU, 0xBBU };
    uds_msg_buf_t req1  = make_req(TEST_DID_OPEN, data1, (uint16_t)TEST_DATA_LEN);
    uds_msg_buf_t resp1 = {0};
    (void)uds_service_0x2E_handler(&g_srv, &req1, &resp1);

    zassert_equal(s_last_write_data[0], (uint8_t)0xAAU, "first write byte 0 mismatch");
    zassert_equal(s_last_write_data[1], (uint8_t)0xBBU, "first write byte 1 mismatch");

    uint8_t       data2[1] = { 0x55U };
    uds_msg_buf_t req2  = make_req(TEST_DID_ONEBYTE, data2, (uint16_t)1U);
    uds_msg_buf_t resp2 = {0};
    (void)uds_service_0x2E_handler(&g_srv, &req2, &resp2);

    zassert_equal(s_write_call_count, (uint16_t)2U, "both writes must have invoked write_cb");
    zassert_equal(s_last_write_len, (uint16_t)1U,
                  "second write's recorded length must be 1, not leftover from the first");
    zassert_equal(s_last_write_data[0], (uint8_t)0x55U,
                  "second write's recorded byte must not be contaminated by the first write");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x2E__tc001_null_ctx(void);
extern void test_service_0x2E__tc002_request_too_short(void);
extern void test_service_0x2E__tc003_did_not_found(void);
extern void test_service_0x2E__tc004_wrong_session(void);
extern void test_service_0x2E__tc005_wrong_security(void);
extern void test_service_0x2E__tc006_data_length_short(void);
extern void test_service_0x2E__tc007_data_length_long(void);
extern void test_service_0x2E__tc008_write_cb_null(void);
extern void test_service_0x2E__tc009_write_cb_failure_propagates(void);
extern void test_service_0x2E__tc010_valid_write_open(void);
extern void test_service_0x2E__tc011_valid_write_gated(void);
extern void test_service_0x2E__tc012_boundary_one_byte(void);
extern void test_service_0x2E__tc013_response_format(void);
extern void test_service_0x2E__tc014_sequential_writes_no_cross_contamination(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x2E__tc001_null_ctx);
    RUN_TEST(test_service_0x2E__tc002_request_too_short);
    RUN_TEST(test_service_0x2E__tc003_did_not_found);
    RUN_TEST(test_service_0x2E__tc004_wrong_session);
    RUN_TEST(test_service_0x2E__tc005_wrong_security);
    RUN_TEST(test_service_0x2E__tc006_data_length_short);
    RUN_TEST(test_service_0x2E__tc007_data_length_long);
    RUN_TEST(test_service_0x2E__tc008_write_cb_null);
    RUN_TEST(test_service_0x2E__tc009_write_cb_failure_propagates);
    RUN_TEST(test_service_0x2E__tc010_valid_write_open);
    RUN_TEST(test_service_0x2E__tc011_valid_write_gated);
    RUN_TEST(test_service_0x2E__tc012_boundary_one_byte);
    RUN_TEST(test_service_0x2E__tc013_response_format);
    RUN_TEST(test_service_0x2E__tc014_sequential_writes_no_cross_contamination);
}
