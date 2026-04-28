// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_service_0x28.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x28.c
 *                    SID 0x28 — CommunicationControl
 *
 * PURPOSE:
 *   Verify all four sub-functions, session gating, response format, error
 *   paths, and state restoration on Default session transition.
 *
 * TEST CASES:
 *   TC-0x28-001  0x00 enableRxAndTx in EXTENDED session → 0x68 0x00, len=2
 *   TC-0x28-002  0x01 enableRxAndDisableTx → 0x68 0x01
 *   TC-0x28-003  0x02 disableRxAndEnableTx → 0x68 0x02
 *   TC-0x28-004  0x03 disableRxAndTx → 0x68 0x03
 *   TC-0x28-005  controlType echoed correctly in response byte 1
 *   TC-0x28-006  Response is exactly 2 bytes
 *   TC-0x28-007  DEFAULT session → ERR_SERVICE_NOT_SUPPORTED_IN_SESSION (NRC 0x7E)
 *   TC-0x28-008  PROGRAMMING session is accepted
 *   TC-0x28-009  Unsupported controlType 0x04 → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12)
 *   TC-0x28-010  Unsupported controlType 0xFF → ERR_SUBFUNCTION_NOT_SUP
 *   TC-0x28-011  Request length 2 (missing commType) → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x28-012  Request length 4 (extra byte) → ERR_INVALID_PARAM
 *   TC-0x28-013  suppressPosRspMsgIndicationBit stripped (0x80 | 0x03 → 0x03)
 *   TC-0x28-014  State change persists: get_mode() reflects new mode after request
 *   TC-0x28-015  restore_defaults() called after Default session → mode = ENABLE_RX_TX
 *   TC-0x28-016  NULL req → ERR_NULL_PTR
 *   TC-0x28-017  NULL resp → ERR_NULL_PTR
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h)
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
#include "uds_types.h"
#include "uds_comm_control.h"

/* Test-only reset (defined in uds_comm_control.c, not in public header) */
extern void uds_comm_control_test_reset(void);

/* =========================================================================
 * Security stubs
 * ========================================================================= */

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l;
    for (uint8_t i = 0U; i < n && i < 4U; i++) { b[i] = (uint8_t)(0x28U + i); }
    *o = (n < 4U) ? n : 4U;
    return UDS_STATUS_OK;
}

static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl,
                  const uint8_t *k, uint8_t kl)
{
    (void)l; (void)s; (void)sl; (void)k; (void)kl;
    return true;
}

/* =========================================================================
 * Shared context
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

static void setup(void)
{
    uds_comm_control_test_reset();

    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_sec,  0, sizeof(g_sec));
    memset(&g_srv,  0, sizeof(g_srv));

    uds_session_init(&g_sess, 5000U);
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED); /* 0x28 needs non-default */

    static const uds_security_cfg_t sc = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,
        .key_validate_cb  = t_key,
        .seed_generate_cb = t_seed,
    };
    uds_security_init(&g_sec, &sc);

    static const uds_server_cfg_t svc = {
        .p2_server_max_ms      = 25U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_uds_service_table,
        .service_table_count   = (uint8_t)UDS_SERVICE_TABLE_COUNT,
    };
    uds_server_init(&g_srv, &svc);

    /* Initialize comm_control with no platform callbacks (RAM-only) */
    static const uds_comm_control_cfg_t cc_cfg = {
        .comm_cb = NULL,
        .dtc_cb  = NULL,
    };
    uds_comm_control_init(&cc_cfg);
}

/** Build a 3-byte CommunicationControl request. */
static uds_msg_buf_t make_req(uint8_t control_type, uint8_t comm_type)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x28U;
    r.data[1] = control_type;
    r.data[2] = comm_type;
    r.length  = 3U;
    return r;
}

/* =========================================================================
 * Test cases
 * ========================================================================= */

ZTEST_SUITE(test_service_0x28, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-0x28-001: enableRxAndTx (0x00) in EXTENDED session → positive response 0x68 0x00.
 */
ZTEST(test_service_0x28, tc001_enable_rx_tx)
{
    setup();

    uds_msg_buf_t req  = make_req(0x00U, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "enableRxAndTx must return OK");
    zassert_equal(resp.data[0], 0x68U, "response SID must be 0x68");
    zassert_equal(resp.data[1], 0x00U, "echo controlType must be 0x00");
}

/**
 * TC-0x28-002: enableRxAndDisableTx (0x01) → 0x68 0x01.
 */
ZTEST(test_service_0x28, tc002_enable_rx_disable_tx)
{
    setup();

    uds_msg_buf_t req  = make_req(0x01U, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "enableRxAndDisableTx must return OK");
    zassert_equal(resp.data[1], 0x01U, "echo controlType must be 0x01");
}

/**
 * TC-0x28-003: disableRxAndEnableTx (0x02) → 0x68 0x02.
 */
ZTEST(test_service_0x28, tc003_disable_rx_enable_tx)
{
    setup();

    uds_msg_buf_t req  = make_req(0x02U, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "disableRxAndEnableTx must return OK");
    zassert_equal(resp.data[1], 0x02U, "echo controlType must be 0x02");
}

/**
 * TC-0x28-004: disableRxAndTx (0x03) → 0x68 0x03.
 */
ZTEST(test_service_0x28, tc004_disable_rx_tx)
{
    setup();

    uds_msg_buf_t req  = make_req(0x03U, 0x03U); /* commType = both NM + app */
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "disableRxAndTx must return OK");
    zassert_equal(resp.data[1], 0x03U, "echo controlType must be 0x03");
}

/**
 * TC-0x28-005: controlType is echoed in response byte 1 (without suppress bit).
 */
ZTEST(test_service_0x28, tc005_control_type_echoed)
{
    setup();

    /* Use controlType 0x02 and verify echo */
    uds_msg_buf_t req  = make_req(0x02U, 0x02U);
    uds_msg_buf_t resp = {0};
    uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[1], 0x02U,
        "echoed controlType must match request controlType");
}

/**
 * TC-0x28-006: Positive response is exactly 2 bytes.
 */
ZTEST(test_service_0x28, tc006_response_two_bytes)
{
    setup();

    uds_msg_buf_t req  = make_req(0x00U, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 2U,
        "CommunicationControl response must be exactly 2 bytes");
}

/**
 * TC-0x28-007: In DEFAULT session → ERR_SERVICE_NOT_SUPPORTED_IN_SESSION (NRC 0x7E).
 */
ZTEST(test_service_0x28, tc007_default_session_rejected)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_DEFAULT); /* back to default */

    uds_msg_buf_t req  = make_req(0x03U, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION,
        "DEFAULT session must return NRC 0x7E");
}

/**
 * TC-0x28-008: PROGRAMMING session is accepted.
 */
ZTEST(test_service_0x28, tc008_programming_session_accepted)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_PROGRAMMING);

    uds_msg_buf_t req  = make_req(0x03U, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,
        "PROGRAMMING session must be accepted for 0x28");
}

/**
 * TC-0x28-009: Unsupported controlType 0x04 → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12).
 */
ZTEST(test_service_0x28, tc009_unsupported_control_type_04)
{
    setup();

    uds_msg_buf_t req  = make_req(0x04U, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
        "controlType 0x04 must return NRC 0x12");
}

/**
 * TC-0x28-010: Unsupported controlType 0x7F → ERR_SUBFUNCTION_NOT_SUP.
 */
ZTEST(test_service_0x28, tc010_unsupported_control_type_7f)
{
    setup();

    uds_msg_buf_t req  = make_req(0x7FU, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
        "controlType 0x7F must return NRC 0x12");
}

/**
 * TC-0x28-011: Request length 2 (no commType byte) → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x28, tc011_short_request_rejected)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x28U;
    req.data[1] = 0x03U;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
        "2-byte request must be rejected (NRC 0x13)");
}

/**
 * TC-0x28-012: Request length 4 (extra byte) → ERR_INVALID_PARAM.
 */
ZTEST(test_service_0x28, tc012_long_request_rejected)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x28U;
    req.data[1] = 0x03U;
    req.data[2] = 0x01U;
    req.data[3] = 0x00U;
    req.length  = 4U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
        "4-byte request must be rejected (NRC 0x13)");
}

/**
 * TC-0x28-013: suppressPosRspMsgIndicationBit is stripped.
 * 0x83 = (0x80 | 0x03) → handler treats as controlType 0x03.
 */
ZTEST(test_service_0x28, tc013_suppress_bit_stripped)
{
    setup();

    uds_msg_buf_t req  = make_req(0x83U, 0x01U); /* 0x80 | 0x03 */
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,
        "suppress bit must be stripped; controlType 0x03 must succeed");
    /* Echo must be 0x03, not 0x83 */
    zassert_equal(resp.data[1], 0x03U,
        "echoed controlType must be 0x03 (suppress bit stripped)");
}

/**
 * TC-0x28-014: comm_control state reflects new mode after request.
 */
ZTEST(test_service_0x28, tc014_state_updated_after_request)
{
    setup();

    /* First set to disable-rx-tx */
    uds_msg_buf_t req  = make_req(0x03U, 0x03U);
    uds_msg_buf_t resp = {0};
    uds_service_0x28_handler(&g_srv, &req, &resp);

    uds_comm_mode_t mode;
    uds_comm_control_get_mode(&mode);
    zassert_equal(mode, UDS_COMM_MODE_DISABLE_RX_TX,
        "comm mode must be DISABLE_RX_TX after request");
}

/**
 * TC-0x28-015: After restore_defaults(), mode returns to ENABLE_RX_TX.
 * This models the session_0x10 transition back to Default.
 */
ZTEST(test_service_0x28, tc015_restore_defaults_on_session_transition)
{
    setup();

    /* Apply a non-default comm control mode */
    uds_msg_buf_t req  = make_req(0x01U, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_service_0x28_handler(&g_srv, &req, &resp);

    /* Simulate Default session transition (called by service_0x10) */
    uds_comm_control_restore_defaults();

    uds_comm_mode_t mode;
    uds_comm_control_get_mode(&mode);
    zassert_equal(mode, UDS_COMM_MODE_ENABLE_RX_TX,
        "comm mode must be restored to ENABLE_RX_TX on Default session");
}

/**
 * TC-0x28-016: NULL req → ERR_NULL_PTR.
 */
ZTEST(test_service_0x28, tc016_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x28_handler(&g_srv, NULL, &resp);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
        "NULL req must return ERR_NULL_PTR");
}

/**
 * TC-0x28-017: NULL resp → ERR_NULL_PTR.
 */
ZTEST(test_service_0x28, tc017_null_resp)
{
    setup();
    uds_msg_buf_t req = make_req(0x00U, 0x01U);
    uds_status_t rc = uds_service_0x28_handler(&g_srv, &req, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
        "NULL resp must return ERR_NULL_PTR");
}

/* =========================================================================
 * run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x28__tc001_enable_rx_tx(void);
extern void test_service_0x28__tc002_enable_rx_disable_tx(void);
extern void test_service_0x28__tc003_disable_rx_enable_tx(void);
extern void test_service_0x28__tc004_disable_rx_tx(void);
extern void test_service_0x28__tc005_control_type_echoed(void);
extern void test_service_0x28__tc006_response_two_bytes(void);
extern void test_service_0x28__tc007_default_session_rejected(void);
extern void test_service_0x28__tc008_programming_session_accepted(void);
extern void test_service_0x28__tc009_unsupported_control_type_04(void);
extern void test_service_0x28__tc010_unsupported_control_type_7f(void);
extern void test_service_0x28__tc011_short_request_rejected(void);
extern void test_service_0x28__tc012_long_request_rejected(void);
extern void test_service_0x28__tc013_suppress_bit_stripped(void);
extern void test_service_0x28__tc014_state_updated_after_request(void);
extern void test_service_0x28__tc015_restore_defaults_on_session_transition(void);
extern void test_service_0x28__tc016_null_req(void);
extern void test_service_0x28__tc017_null_resp(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x28__tc001_enable_rx_tx);
    RUN_TEST(test_service_0x28__tc002_enable_rx_disable_tx);
    RUN_TEST(test_service_0x28__tc003_disable_rx_enable_tx);
    RUN_TEST(test_service_0x28__tc004_disable_rx_tx);
    RUN_TEST(test_service_0x28__tc005_control_type_echoed);
    RUN_TEST(test_service_0x28__tc006_response_two_bytes);
    RUN_TEST(test_service_0x28__tc007_default_session_rejected);
    RUN_TEST(test_service_0x28__tc008_programming_session_accepted);
    RUN_TEST(test_service_0x28__tc009_unsupported_control_type_04);
    RUN_TEST(test_service_0x28__tc010_unsupported_control_type_7f);
    RUN_TEST(test_service_0x28__tc011_short_request_rejected);
    RUN_TEST(test_service_0x28__tc012_long_request_rejected);
    RUN_TEST(test_service_0x28__tc013_suppress_bit_stripped);
    RUN_TEST(test_service_0x28__tc014_state_updated_after_request);
    RUN_TEST(test_service_0x28__tc015_restore_defaults_on_session_transition);
    RUN_TEST(test_service_0x28__tc016_null_req);
    RUN_TEST(test_service_0x28__tc017_null_resp);
}
