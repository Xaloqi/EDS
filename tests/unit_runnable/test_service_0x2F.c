// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_service_0x2F.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x2F.c
 *                    SID 0x2F — InputOutputControlByIdentifier
 *
 * PURPOSE:
 *   Verify all branches of the 0x2F handler: controlParam dispatch, DID
 *   lookup, callback invocation, positive-response encoding, and all NRC
 *   rejection paths.
 *
 * TEST CASES:
 *   TC-0x2F-001  NULL ctx                                → ERR_NULL_PTR
 *   TC-0x2F-002  Request < 4 bytes                       → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x2F-003  controlParam == 0x04 (unknown)          → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12)
 *   TC-0x2F-004  DID not found in database               → ERR_DID_NOT_FOUND (NRC 0x31)
 *   TC-0x2F-005  DID found but io_control_cb == NULL     → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31)
 *   TC-0x2F-006  returnControlToECU (0x00) success       → [0x6F, DID_Hi, DID_Lo, 0x00, status...]
 *   TC-0x2F-007  resetToDefault (0x01) success           → positive response
 *   TC-0x2F-008  freezeCurrentState (0x02) success       → positive response
 *   TC-0x2F-009  shortTermAdjustment (0x03) success      → controlOptionRecord passed to cb
 *   TC-0x2F-010  shortTermAdjustment — request too short → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x2F-011  shortTermAdjustment — wrong data length → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x2F-012  io_control_cb returns ERR_CONDITIONS_NOT_MET → NRC 0x22
 *   TC-0x2F-013  0x00/0x01/0x02 with trailing bytes     → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x2F-014  controlStatusRecord byte-exact in response
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
 * DID 0xA001: fully IO-controllable, data_length = 2.
 * DID 0xA002: DID_ACCESS_IO_CONTROL set but io_control_cb == NULL.
 * ========================================================================= */

#define TEST_DID_CTRL   (0xA001U)  /**< Fully IO-controllable DID. */
#define TEST_DID_NO_CB  (0xA002U)  /**< IO flag set but no callback. */
#define TEST_DATA_LEN   (2U)       /**< data_length for TEST_DID_CTRL. */

/* =========================================================================
 * Mock io_control_cb
 * ========================================================================= */

/* Last call parameters recorded by the mock callback. */
static uint8_t  s_last_ctrl_param;
static uint8_t  s_last_ctrl_record[DID_MAX_DATA_LEN];
static uint16_t s_last_ctrl_len;

/* Status the mock callback should return on next invocation. */
static uds_status_t s_mock_cb_return;

/* Status bytes written to the status_record output by the mock callback. */
static uint8_t  s_mock_status_bytes[DID_MAX_DATA_LEN];
static uint16_t s_mock_status_len;

static uds_status_t mock_io_control_cb(
    uint8_t         control_param,
    const uint8_t  *control_record,
    uint16_t        control_len,
    uint8_t        *status_record,
    uint16_t       *status_len)
{
    uint16_t i;

    s_last_ctrl_param = control_param;
    s_last_ctrl_len   = control_len;

    if ((control_record != NULL) && (control_len > (uint16_t)0U)) {
        for (i = (uint16_t)0U; i < control_len && i < (uint16_t)DID_MAX_DATA_LEN; i++) {
            s_last_ctrl_record[i] = control_record[i];
        }
    }

    if (s_mock_cb_return == UDS_STATUS_OK) {
        for (i = (uint16_t)0U; i < s_mock_status_len; i++) {
            status_record[i] = s_mock_status_bytes[i];
        }
        *status_len = s_mock_status_len;
    }

    return s_mock_cb_return;
}

/* =========================================================================
 * Stub read/write callbacks for the test DID (required by did_entry_t)
 * ========================================================================= */

static uds_status_t stub_read_cb(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    (void)buf; (void)buf_len;
    *out_len = (uint16_t)0U;
    return UDS_STATUS_OK;
}

static uds_status_t stub_write_cb(const uint8_t *buf, uint16_t len)
{
    (void)buf; (void)len;
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

static void setup(void)
{
    if (!g_stack_ready) {
        did_entry_t entry;

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

        /* Register DID 0xA001: IO-controllable, data_length = 2. */
        memset(&entry, 0, sizeof(entry));
        entry.did_id             = TEST_DID_CTRL;
        entry.access_flags       = (uint8_t)(DID_ACCESS_READ |
                                             DID_ACCESS_WRITE |
                                             DID_ACCESS_IO_CONTROL);
        entry.min_session        = (uint8_t)UDS_SESSION_DEFAULT;
        entry.read_access_level  = (uint8_t)0U;
        entry.write_access_level = (uint8_t)0U;
        entry.data_length        = (uint16_t)TEST_DATA_LEN;
        entry.read_cb            = stub_read_cb;
        entry.write_cb           = stub_write_cb;
        entry.io_control_cb      = mock_io_control_cb;
        entry.description        = "Test IO-control DID";
        (void)did_database_register(&entry);

        /* Register DID 0xA002: IO flag set but no callback. */
        memset(&entry, 0, sizeof(entry));
        entry.did_id             = TEST_DID_NO_CB;
        entry.access_flags       = (uint8_t)DID_ACCESS_IO_CONTROL;
        entry.min_session        = (uint8_t)UDS_SESSION_DEFAULT;
        entry.read_access_level  = (uint8_t)0U;
        entry.write_access_level = (uint8_t)0U;
        entry.data_length        = (uint16_t)TEST_DATA_LEN;
        entry.read_cb            = NULL;
        entry.write_cb           = NULL;
        entry.io_control_cb      = NULL;
        entry.description        = "Test DID no cb";
        (void)did_database_register(&entry);

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

    /* Reset session and clear mock state before each test. */
    uds_session_transition(&g_sess, UDS_SESSION_DEFAULT);
    s_last_ctrl_param       = (uint8_t)0xFFU;
    s_last_ctrl_len         = (uint16_t)0U;
    s_mock_cb_return        = UDS_STATUS_OK;
    s_mock_status_bytes[0]  = (uint8_t)0xCAU;
    s_mock_status_bytes[1]  = (uint8_t)0xFEU;
    s_mock_status_len       = (uint16_t)TEST_DATA_LEN;
    memset(s_last_ctrl_record, 0, sizeof(s_last_ctrl_record));
}

/** Build a minimal 0x2F request for a given DID and controlParam. */
static uds_msg_buf_t make_req(uint16_t did, uint8_t ctrl_param)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = (uint8_t)UDS_SID_INPUT_OUTPUT_CONTROL;
    r.data[1] = (uint8_t)((did >> 8U) & (uint8_t)0xFFU);
    r.data[2] = (uint8_t)(did & (uint8_t)0xFFU);
    r.data[3] = ctrl_param;
    r.length  = 4U;
    return r;
}

/** Build a 0x03 shortTermAdjustment request with a 2-byte control record. */
static uds_msg_buf_t make_sta_req(uint16_t did, uint8_t b0, uint8_t b1)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = (uint8_t)UDS_SID_INPUT_OUTPUT_CONTROL;
    r.data[1] = (uint8_t)((did >> 8U) & (uint8_t)0xFFU);
    r.data[2] = (uint8_t)(did & (uint8_t)0xFFU);
    r.data[3] = (uint8_t)0x03U;  /* shortTermAdjustment */
    r.data[4] = b0;
    r.data[5] = b1;
    r.length  = 6U;
    return r;
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_service_0x2F, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------- */

/**
 * TC-0x2F-001: NULL ctx → ERR_NULL_PTR.
 */
ZTEST(test_service_0x2F, tc001_null_ctx)
{
    setup();
    uds_msg_buf_t req  = make_req(TEST_DID_CTRL, 0x00U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(NULL, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must return ERR_NULL_PTR");
}

/**
 * TC-0x2F-002: Request < 4 bytes → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x2F, tc002_request_too_short)
{
    setup();
    uds_msg_buf_t req;
    uds_msg_buf_t resp = {0};
    memset(&req, 0, sizeof(req));
    req.data[0] = (uint8_t)UDS_SID_INPUT_OUTPUT_CONTROL;
    req.data[1] = (uint8_t)0xA0U;
    req.data[2] = (uint8_t)0x01U;
    req.length  = 3U;

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Request < 4 bytes must return ERR_INVALID_PARAM (NRC 0x13)");
}

/**
 * TC-0x2F-003: controlParam == 0x04 (unknown) → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12).
 */
ZTEST(test_service_0x2F, tc003_unknown_control_param)
{
    setup();
    uds_msg_buf_t req  = make_req(TEST_DID_CTRL, 0x04U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  "Unknown controlParam must return ERR_SUBFUNCTION_NOT_SUP (NRC 0x12)");
}

/**
 * TC-0x2F-004: DID not found in database → ERR_DID_NOT_FOUND (NRC 0x31).
 */
ZTEST(test_service_0x2F, tc004_did_not_found)
{
    setup();
    uds_msg_buf_t req  = make_req(0xDEADU, 0x00U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_DID_NOT_FOUND,
                  "Unknown DID must return ERR_DID_NOT_FOUND (→ NRC 0x31)");
}

/**
 * TC-0x2F-005: DID found but io_control_cb == NULL → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31).
 */
ZTEST(test_service_0x2F, tc005_no_io_control_cb)
{
    setup();
    uds_msg_buf_t req  = make_req(TEST_DID_NO_CB, 0x00U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "NULL io_control_cb must return ERR_REQUEST_OUT_OF_RANGE (→ NRC 0x31)");
}

/**
 * TC-0x2F-006: returnControlToECU (0x00) success → [0x6F, DID_Hi, DID_Lo, 0x00, status...].
 */
ZTEST(test_service_0x2F, tc006_return_control_to_ecu_ok)
{
    setup();
    uds_msg_buf_t req  = make_req(TEST_DID_CTRL, 0x00U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,          "returnControlToECU must succeed");
    zassert_equal(resp.data[0], 0x6FU,        "Response SID must be 0x6F");
    zassert_equal(resp.data[1], 0xA0U,        "DID high byte echoed");
    zassert_equal(resp.data[2], 0x01U,        "DID low  byte echoed");
    zassert_equal(resp.data[3], 0x00U,        "controlParam echoed");
    zassert_equal(s_last_ctrl_param, 0x00U,   "Callback received controlParam 0x00");
    zassert_equal(s_last_ctrl_len, (uint16_t)0U, "No control record for 0x00");
}

/**
 * TC-0x2F-007: resetToDefault (0x01) success → positive response.
 */
ZTEST(test_service_0x2F, tc007_reset_to_default_ok)
{
    setup();
    uds_msg_buf_t req  = make_req(TEST_DID_CTRL, 0x01U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,        "resetToDefault must succeed");
    zassert_equal(resp.data[0], 0x6FU,      "Response SID must be 0x6F");
    zassert_equal(resp.data[3], 0x01U,      "controlParam 0x01 echoed");
    zassert_equal(s_last_ctrl_param, 0x01U, "Callback received controlParam 0x01");
}

/**
 * TC-0x2F-008: freezeCurrentState (0x02) success → positive response.
 */
ZTEST(test_service_0x2F, tc008_freeze_current_state_ok)
{
    setup();
    uds_msg_buf_t req  = make_req(TEST_DID_CTRL, 0x02U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,        "freezeCurrentState must succeed");
    zassert_equal(resp.data[0], 0x6FU,      "Response SID must be 0x6F");
    zassert_equal(resp.data[3], 0x02U,      "controlParam 0x02 echoed");
    zassert_equal(s_last_ctrl_param, 0x02U, "Callback received controlParam 0x02");
    zassert_equal(s_last_ctrl_len, (uint16_t)0U, "No control record for 0x02");
}

/**
 * TC-0x2F-009: shortTermAdjustment (0x03) success → controlOptionRecord
 *              bytes passed correctly to callback.
 */
ZTEST(test_service_0x2F, tc009_short_term_adjustment_ok)
{
    setup();
    uds_msg_buf_t req  = make_sta_req(TEST_DID_CTRL, 0xABU, 0xCDU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,          "shortTermAdjustment must succeed");
    zassert_equal(resp.data[0], 0x6FU,        "Response SID must be 0x6F");
    zassert_equal(resp.data[3], 0x03U,        "controlParam 0x03 echoed");
    zassert_equal(s_last_ctrl_param, 0x03U,   "Callback received controlParam 0x03");
    zassert_equal(s_last_ctrl_len, (uint16_t)TEST_DATA_LEN,
                  "Control record length must equal data_length");
    zassert_equal(s_last_ctrl_record[0], 0xABU, "Byte 0 of controlOptionRecord correct");
    zassert_equal(s_last_ctrl_record[1], 0xCDU, "Byte 1 of controlOptionRecord correct");
}

/**
 * TC-0x2F-010: shortTermAdjustment — request is exactly 4 bytes (no
 *              controlOptionRecord present) → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x2F, tc010_sta_no_data_record)
{
    setup();
    /* 4-byte request: [SID, DID_Hi, DID_Lo, 0x03] — missing controlOptionRecord. */
    uds_msg_buf_t req  = make_req(TEST_DID_CTRL, 0x03U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    /* control_len = 0 != entry->data_length (2) → ERR_INVALID_PARAM. */
    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "STA with no controlOptionRecord must return ERR_INVALID_PARAM");
}

/**
 * TC-0x2F-011: shortTermAdjustment — controlOptionRecord has wrong length
 *              (1 byte instead of data_length = 2) → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x2F, tc011_sta_wrong_data_length)
{
    setup();
    uds_msg_buf_t req;
    uds_msg_buf_t resp = {0};
    memset(&req, 0, sizeof(req));
    req.data[0] = (uint8_t)UDS_SID_INPUT_OUTPUT_CONTROL;
    req.data[1] = (uint8_t)((TEST_DID_CTRL >> 8U) & (uint8_t)0xFFU);
    req.data[2] = (uint8_t)(TEST_DID_CTRL & (uint8_t)0xFFU);
    req.data[3] = (uint8_t)0x03U;
    req.data[4] = (uint8_t)0x11U;  /* Only 1 byte; data_length = 2. */
    req.length  = 5U;

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "STA with wrong controlOptionRecord length must return ERR_INVALID_PARAM");
}

/**
 * TC-0x2F-012: io_control_cb returns ERR_CONDITIONS_NOT_MET → NRC 0x22.
 */
ZTEST(test_service_0x2F, tc012_cb_conditions_not_met)
{
    setup();
    s_mock_cb_return = UDS_STATUS_ERR_CONDITIONS_NOT_MET;

    uds_msg_buf_t req  = make_req(TEST_DID_CTRL, 0x00U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  "Callback ERR_CONDITIONS_NOT_MET must propagate (→ NRC 0x22)");
}

/**
 * TC-0x2F-013: returnControlToECU (0x00) with one extra trailing byte →
 *              ERR_INVALID_PARAM (NRC 0x13). Exact length = 4 required.
 */
ZTEST(test_service_0x2F, tc013_trailing_bytes_rejected)
{
    setup();
    uds_msg_buf_t req;
    uds_msg_buf_t resp = {0};
    memset(&req, 0, sizeof(req));
    req.data[0] = (uint8_t)UDS_SID_INPUT_OUTPUT_CONTROL;
    req.data[1] = (uint8_t)((TEST_DID_CTRL >> 8U) & (uint8_t)0xFFU);
    req.data[2] = (uint8_t)(TEST_DID_CTRL & (uint8_t)0xFFU);
    req.data[3] = (uint8_t)0x00U;  /* returnControlToECU */
    req.data[4] = (uint8_t)0xFFU;  /* Unexpected trailing byte. */
    req.length  = 5U;

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Trailing bytes for 0x00/0x01/0x02 must return ERR_INVALID_PARAM (NRC 0x13)");
}

/**
 * TC-0x2F-014: controlStatusRecord bytes written correctly to response.
 *              Verifies byte-exact copy from status_buf to response.
 */
ZTEST(test_service_0x2F, tc014_status_record_in_response)
{
    setup();
    s_mock_status_bytes[0] = (uint8_t)0xBEU;
    s_mock_status_bytes[1] = (uint8_t)0xEFU;
    s_mock_status_len      = (uint16_t)TEST_DATA_LEN;

    uds_msg_buf_t req  = make_req(TEST_DID_CTRL, 0x01U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2F_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "resetToDefault must succeed");
    /* Response: [0x6F, DID_Hi, DID_Lo, controlParam, status[0], status[1]] */
    zassert_equal(resp.length, (uint16_t)(4U + TEST_DATA_LEN),
                  "Response length must be 4 + data_length");
    zassert_equal(resp.data[4], (uint8_t)0xBEU,
                  "controlStatusRecord byte 0 must be 0xBE");
    zassert_equal(resp.data[5], (uint8_t)0xEFU,
                  "controlStatusRecord byte 1 must be 0xEF");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x2F__tc001_null_ctx(void);
extern void test_service_0x2F__tc002_request_too_short(void);
extern void test_service_0x2F__tc003_unknown_control_param(void);
extern void test_service_0x2F__tc004_did_not_found(void);
extern void test_service_0x2F__tc005_no_io_control_cb(void);
extern void test_service_0x2F__tc006_return_control_to_ecu_ok(void);
extern void test_service_0x2F__tc007_reset_to_default_ok(void);
extern void test_service_0x2F__tc008_freeze_current_state_ok(void);
extern void test_service_0x2F__tc009_short_term_adjustment_ok(void);
extern void test_service_0x2F__tc010_sta_no_data_record(void);
extern void test_service_0x2F__tc011_sta_wrong_data_length(void);
extern void test_service_0x2F__tc012_cb_conditions_not_met(void);
extern void test_service_0x2F__tc013_trailing_bytes_rejected(void);
extern void test_service_0x2F__tc014_status_record_in_response(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x2F__tc001_null_ctx);
    RUN_TEST(test_service_0x2F__tc002_request_too_short);
    RUN_TEST(test_service_0x2F__tc003_unknown_control_param);
    RUN_TEST(test_service_0x2F__tc004_did_not_found);
    RUN_TEST(test_service_0x2F__tc005_no_io_control_cb);
    RUN_TEST(test_service_0x2F__tc006_return_control_to_ecu_ok);
    RUN_TEST(test_service_0x2F__tc007_reset_to_default_ok);
    RUN_TEST(test_service_0x2F__tc008_freeze_current_state_ok);
    RUN_TEST(test_service_0x2F__tc009_short_term_adjustment_ok);
    RUN_TEST(test_service_0x2F__tc010_sta_no_data_record);
    RUN_TEST(test_service_0x2F__tc011_sta_wrong_data_length);
    RUN_TEST(test_service_0x2F__tc012_cb_conditions_not_met);
    RUN_TEST(test_service_0x2F__tc013_trailing_bytes_rejected);
    RUN_TEST(test_service_0x2F__tc014_status_record_in_response);
}
