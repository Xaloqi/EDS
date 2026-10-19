// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit/test_service_0x10.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x10.c
 *                    SID 0x10 — DiagnosticSessionControl
 *
 * PURPOSE:
 *   Verify every branch of the 0x10 handler: session transitions,
 *   positive-response encoding, NRC conditions and length guards.
 *
 * TEST CASES:
 *   TC-0x10-001  Default session request         → positive response 0x50 0x01
 *   TC-0x10-002  Programming session request     → positive response 0x50 0x02
 *   TC-0x10-003  Extended session request        → positive response 0x50 0x03
 *   TC-0x10-004  SafetySystem session request    → positive response 0x50 0x04
 *   TC-0x10-005  Invalid sub-function 0xAA       → ERR_SESSION_INVALID (NRC 0x12)
 *   TC-0x10-006  Request length == 1 (no subFn)  → ERR_INVALID_PARAM  (NRC 0x13)
 *   TC-0x10-007  Request length == 0             → ERR_INVALID_PARAM  (NRC 0x13)
 *   TC-0x10-008  Positive response has 6 bytes with P2/P2* timing
 *   TC-0x10-009  P2server bytes = 0x00 0x19 (25 ms default)
 *   TC-0x10-010  P2*server bytes = 0x01 0xF4 (5000 ms default)
 *   TC-0x10-011  Session actually transitions in ctx after handler
 *   TC-0x10-012  Transition to same session (DEFAULT→DEFAULT) → OK
 *   TC-0x10-013  NULL req → ERR_NULL_PTR from validate_length
 *   TC-0x10-014  NULL resp → ERR_NULL_PTR from write_pos_sid
 *
 * FRAMEWORK: Zephyr Ztest
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

/* Module under test */
#include "services.h"

/* Dependencies pulled in by the handler */
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_types.h"
#include "uds_comm_control.h"
#include "uds_transfer_ctx.h"

/* =========================================================================
 * Minimal security stubs
 * ========================================================================= */

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l;
    for (uint8_t i = 0; i < n && i < 4U; i++) { b[i] = (uint8_t)(0x10U + i); }
    *o = (n < 4U) ? n : 4U;
    return UDS_STATUS_OK;
}

static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl,
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
 * Shared context objects
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

static void setup(void)
{
    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_sec,  0, sizeof(g_sec));
    memset(&g_srv,  0, sizeof(g_srv));

    zassert_equal(uds_session_init(&g_sess, 5000U), UDS_STATUS_OK, "sess init");

    static const uds_security_cfg_t sc = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,
        .key_validate_cb  = t_key,
        .seed_generate_cb = t_seed,
    };
    zassert_equal(uds_security_init(&g_sec, &sc), UDS_STATUS_OK, "sec init");

    static const uds_server_cfg_t svc = {
        .p2_server_max_ms      = 25U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_uds_service_table,
        .service_table_count   = (uint8_t)UDS_SERVICE_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(&g_srv, &svc), UDS_STATUS_OK, "srv init");
}

/** Build a 2-byte SessionControl request. */
static uds_msg_buf_t make_req(uint8_t sub)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x10U;
    r.data[1] = sub;
    r.length  = 2U;
    return r;
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_service_0x10, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------- */

/**
 * TC-0x10-001: Default session (0x01) → positive response 0x50, echo 0x01.
 */
ZTEST(test_service_0x10, tc001_default_session)
{
    setup();
    uds_msg_buf_t req  = make_req(0x01U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "handler must return OK");
    zassert_equal(resp.data[0], 0x50U, "response SID must be 0x50");
    zassert_equal(resp.data[1], 0x01U, "echo sub-function must be 0x01");
    zassert_equal(resp.length,  6U,    "response must be 6 bytes");
}

/**
 * TC-0x10-002: Programming session (0x02) → 0x50, echo 0x02.
 */
ZTEST(test_service_0x10, tc002_programming_session)
{
    setup();
    uds_msg_buf_t req  = make_req(0x02U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "handler must return OK");
    zassert_equal(resp.data[0], 0x50U, "response SID must be 0x50");
    zassert_equal(resp.data[1], 0x02U, "echo sub-function must be 0x02");
    zassert_equal(resp.length,  6U,    "response must be 6 bytes");
}

/**
 * TC-0x10-003: Extended diagnostic session (0x03) → 0x50, echo 0x03.
 */
ZTEST(test_service_0x10, tc003_extended_session)
{
    setup();
    uds_msg_buf_t req  = make_req(0x03U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "handler must return OK");
    zassert_equal(resp.data[0], 0x50U, "response SID must be 0x50");
    zassert_equal(resp.data[1], 0x03U, "echo sub-function must be 0x03");
    zassert_equal(resp.length,  6U,    "response must be 6 bytes");
}

/**
 * TC-0x10-004: Safety system session (0x04), once enabled → 0x50, echo 0x04.
 */
ZTEST(test_service_0x10, tc004_safety_system_session)
{
    setup();
    zassert_equal(uds_session_set_safety_system_enabled(&g_sess, true), UDS_STATUS_OK,
                  "enable 0x04 [EDS#494]");
    uds_msg_buf_t req  = make_req(0x04U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "handler must return OK");
    zassert_equal(resp.data[1], 0x04U, "echo sub-function must be 0x04");
}

/**
 * TC-0x10-004b: 0x04 not enabled (the default) → SUBFUNCTION_NOT_SUP (NRC
 * 0x12), session unchanged [EDS#494].
 */
ZTEST(test_service_0x10, tc004b_safety_system_session_disabled_by_default)
{
    setup();
    uds_msg_buf_t req  = make_req(0x04U);
    uds_msg_buf_t resp = {0};

    zassert_equal(uds_service_0x10_handler(&g_srv, &req, &resp),
                  UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  "0x04 must be unsupported until enabled (NRC 0x12, EDS#494)");
    uds_session_type_t active;
    (void)uds_session_get_active(&g_sess, &active);
    zassert_equal(active, UDS_SESSION_DEFAULT, "session must stay DEFAULT");
}

/**
 * TC-0x10-005: Invalid sub-function 0xAA → ERR_SESSION_INVALID.
 * Dispatcher maps this to NRC 0x12 (SubFunctionNotSupported).
 */
ZTEST(test_service_0x10, tc005_invalid_subfunction)
{
    setup();
    uds_msg_buf_t req  = make_req(0xAAU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  "unknown session type must return ERR_SUBFUNCTION_NOT_SUP (NRC 0x12)");
}

/**
 * TC-0x10-006: Request length == 1 (missing sub-function) → ERR_INVALID_PARAM.
 * ISO 14229-1: NRC 0x13 — incorrectMessageLengthOrInvalidFormat.
 */
ZTEST(test_service_0x10, tc006_short_request_len1)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x10U;
    req.length  = 1U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "length 1 must fail with ERR_INVALID_PARAM");
}

/**
 * TC-0x10-007: Request length == 0 → ERR_INVALID_PARAM.
 */
ZTEST(test_service_0x10, tc007_zero_length_request)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.length = 0U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "zero length must fail");
}

/**
 * TC-0x10-008: Positive response carries exactly 6 bytes.
 * Format: [0x50, subFn, P2_hi, P2_lo, P2star_hi, P2star_lo]
 */
ZTEST(test_service_0x10, tc008_response_is_6_bytes)
{
    setup();
    uds_msg_buf_t req  = make_req(0x03U);
    uds_msg_buf_t resp = {0};

    uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 6U, "response must always be 6 bytes");
}

/**
 * TC-0x10-009: P2server bytes = 0x00, 0x19 (25 ms as big-endian uint16).
 */
ZTEST(test_service_0x10, tc009_p2server_timing_bytes)
{
    setup();
    uds_msg_buf_t req  = make_req(0x01U);
    uds_msg_buf_t resp = {0};

    uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[2], 0x00U, "P2 high byte must be 0x00");
    zassert_equal(resp.data[3], 0x19U, "P2 low  byte must be 0x19 (25 ms)");
}

/**
 * TC-0x10-010: P2*server bytes = 0x01, 0xF4 (5000 ms as big-endian uint16).
 */
ZTEST(test_service_0x10, tc010_p2star_timing_bytes)
{
    setup();
    uds_msg_buf_t req  = make_req(0x01U);
    uds_msg_buf_t resp = {0};

    uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[4], 0x01U, "P2* high byte must be 0x01");
    zassert_equal(resp.data[5], 0xF4U, "P2* low  byte must be 0xF4 (5000 ms)");
}

/**
 * TC-0x10-011: Session actually transitions inside ctx after handler.
 * Request EXTENDED → verify session_ctx reflects EXTENDED.
 */
ZTEST(test_service_0x10, tc011_session_transitions_in_ctx)
{
    setup();
    uds_msg_buf_t req  = make_req(0x03U); /* EXTENDED */
    uds_msg_buf_t resp = {0};

    uds_service_0x10_handler(&g_srv, &req, &resp);

    uds_session_type_t active;
    uds_session_get_active(&g_sess, &active);
    zassert_equal(active, UDS_SESSION_EXTENDED,
                  "session_ctx must reflect EXTENDED after handler");
}

/**
 * TC-0x10-012: DEFAULT → DEFAULT transition is valid and returns OK.
 */
ZTEST(test_service_0x10, tc012_default_to_default_ok)
{
    setup();
    uds_msg_buf_t req  = make_req(0x01U); /* DEFAULT again */
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "DEFAULT→DEFAULT must be OK");
}

/**
 * TC-0x10-013: NULL req → ERR_NULL_PTR from validate_length guard.
 */
ZTEST(test_service_0x10, tc013_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x10_handler(&g_srv, NULL, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL req must return ERR_NULL_PTR");
}

/**
 * TC-0x10-014: NULL resp → ERR_NULL_PTR from write_pos_sid guard.
 */
ZTEST(test_service_0x10, tc014_null_resp)
{
    setup();
    uds_msg_buf_t req = make_req(0x01U);

    uds_status_t rc = uds_service_0x10_handler(&g_srv, &req, NULL);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL resp must return ERR_NULL_PTR");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x10__tc001_default_session(void);
extern void test_service_0x10__tc002_programming_session(void);
extern void test_service_0x10__tc003_extended_session(void);
extern void test_service_0x10__tc004_safety_system_session(void);
extern void test_service_0x10__tc005_invalid_subfunction(void);
extern void test_service_0x10__tc006_short_request_len1(void);
extern void test_service_0x10__tc007_zero_length_request(void);
extern void test_service_0x10__tc008_response_is_6_bytes(void);
extern void test_service_0x10__tc009_p2server_timing_bytes(void);
extern void test_service_0x10__tc010_p2star_timing_bytes(void);
extern void test_service_0x10__tc011_session_transitions_in_ctx(void);
extern void test_service_0x10__tc012_default_to_default_ok(void);
extern void test_service_0x10__tc013_null_req(void);
extern void test_service_0x10__tc014_null_resp(void);

/* =========================================================================
 * [SEC-DSC-RELOCK-01] / [SEC-SESS-COMMRESTORE-01]
 * ISO 14229-1:2026 §9.2.1 c) and d), §9.2.4 Table 24.
 * ========================================================================= */

static bool is_unlocked_l1(void)
{
    bool u = false;
    (void)uds_security_is_unlocked(&g_sec, 1U, &u);
    return u;
}

static void enter(uint8_t sub)
{
    uds_msg_buf_t req = make_req(sub);
    uds_msg_buf_t resp;
    memset(&resp, 0, sizeof(resp));
    zassert_equal(uds_service_0x10_handler(&g_srv, &req, &resp), UDS_STATUS_OK, "DSC 0x%02X", sub);
}

static void comm_reset_init(void)
{
    uds_comm_control_test_reset();
    zassert_equal(uds_comm_control_init(NULL), UDS_STATUS_OK, "comm init");
}

/* Unlocked in extended, then 10 02: programming must start locked. */
ZTEST(test_service_0x10, tc015_relock_extended_to_programming)
{
    setup();
    enter(0x03U);
    g_sec.active_level = 1U;
    enter(0x02U);
    zassert_false(is_unlocked_l1(),
        "unlock carried from extended into programming (SEC-DSC-RELOCK-01)");
}

/* Refreshing the active non-default session re-locks too (case c). */
ZTEST(test_service_0x10, tc016_relock_on_session_refresh)
{
    setup();
    enter(0x03U);
    g_sec.active_level = 1U;
    enter(0x03U);
    zassert_false(is_unlocked_l1(),
        "unlock survived a 10 03 refresh of the extended session (SEC-DSC-RELOCK-01)");
}

/* A rejected transition keeps the active session and its unlock (Table 24). */
ZTEST(test_service_0x10, tc017_rejected_transition_keeps_unlock)
{
    uds_msg_buf_t req, resp;
    setup();
    enter(0x02U);
    g_sec.active_level = 1U;
    req = make_req(0x03U);            /* programming -> extended is refused */
    memset(&resp, 0, sizeof(resp));
    zassert_not_equal(uds_service_0x10_handler(&g_srv, &req, &resp), UDS_STATUS_OK, "");
    zassert_true(is_unlocked_l1(), "a refused DSC must not re-lock the active session");
}

/* 10 01 restores CommunicationControl and ControlDTCSetting (case d). */
ZTEST(test_service_0x10, tc018_default_restores_comm_and_dtc_setting)
{
    uds_comm_mode_t m; uds_dtc_setting_mode_t d;
    setup(); comm_reset_init();
    enter(0x03U);
    zassert_equal(uds_comm_control_apply(0x03U, 0x03U), UDS_STATUS_OK, "");
    zassert_equal(uds_comm_control_set_dtc_setting(0x02U), UDS_STATUS_OK, "");
    enter(0x01U);
    (void)uds_comm_control_get_mode(&m); (void)uds_comm_control_get_dtc_setting(&d);
    zassert_equal(UDS_COMM_MODE_ENABLE_RX_TX, m,
        "communication left disabled after 10 01 (SEC-SESS-COMMRESTORE-01)");
    zassert_equal(UDS_DTC_SETTING_ON, d,
        "DTC setting left off after 10 01 (SEC-SESS-COMMRESTORE-01)");
}

/* An S3 timeout is a return to default too. */
ZTEST(test_service_0x10, tc019_s3_timeout_restores_comm_and_dtc_setting)
{
    uds_comm_mode_t m; uds_dtc_setting_mode_t d;
    setup(); comm_reset_init();
    enter(0x03U);
    (void)uds_comm_control_apply(0x03U, 0x03U);
    (void)uds_comm_control_set_dtc_setting(0x02U);
    for (uint32_t i = 0U; i < 5001U; i++) { (void)uds_server_tick_1ms(&g_srv); }
    (void)uds_comm_control_get_mode(&m); (void)uds_comm_control_get_dtc_setting(&d);
    zassert_equal(UDS_COMM_MODE_ENABLE_RX_TX, m,
        "communication left disabled after S3 timeout (SEC-SESS-COMMRESTORE-01)");
    zassert_equal(UDS_DTC_SETTING_ON, d,
        "DTC setting left off after S3 timeout (SEC-SESS-COMMRESTORE-01)");
}

/* Non-default -> non-default keeps both states (case c). */
ZTEST(test_service_0x10, tc020_nondefault_transition_keeps_comm_state)
{
    uds_comm_mode_t m;
    setup(); comm_reset_init();
    enter(0x03U);
    (void)uds_comm_control_apply(0x03U, 0x03U);
    enter(0x02U);
    (void)uds_comm_control_get_mode(&m);
    zassert_equal(UDS_COMM_MODE_DISABLE_RX_TX, m,
        "CommunicationControl state must be kept across extended -> programming");
}

/* [EDS#442] A session change aborts any transfer, so refusing a 0x34 during
 * a download (Table 444) can't let an abandoned one block the next. */
ZTEST(test_service_0x10, tc021_session_change_aborts_transfer)
{
    setup();
    enter(0x02U);
    uds_transfer_ctx_get()->state = UDS_TRANSFER_ACTIVE;
    enter(0x02U);
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
        "a DSC transition must abort the active transfer (EDS#442)");
}

ZTEST(test_service_0x10, tc022_s3_timeout_aborts_transfer)
{
    setup();
    enter(0x02U);
    uds_transfer_ctx_get()->state = UDS_TRANSFER_ACTIVE;
    for (uint32_t i = 0U; i < 5001U; i++) { (void)uds_server_tick_1ms(&g_srv); }
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
        "an S3 timeout must abort the active transfer (EDS#442)");
}

void run_all_tests(void)
{
    RUN_TEST(test_service_0x10__tc001_default_session);
    RUN_TEST(test_service_0x10__tc002_programming_session);
    RUN_TEST(test_service_0x10__tc003_extended_session);
    RUN_TEST(test_service_0x10__tc004_safety_system_session);
    RUN_TEST(test_service_0x10__tc005_invalid_subfunction);
    RUN_TEST(test_service_0x10__tc006_short_request_len1);
    RUN_TEST(test_service_0x10__tc007_zero_length_request);
    RUN_TEST(test_service_0x10__tc008_response_is_6_bytes);
    RUN_TEST(test_service_0x10__tc009_p2server_timing_bytes);
    RUN_TEST(test_service_0x10__tc010_p2star_timing_bytes);
    RUN_TEST(test_service_0x10__tc011_session_transitions_in_ctx);
    RUN_TEST(test_service_0x10__tc012_default_to_default_ok);
    RUN_TEST(test_service_0x10__tc013_null_req);
    RUN_TEST(test_service_0x10__tc014_null_resp);
    RUN_TEST(test_service_0x10__tc015_relock_extended_to_programming);
    RUN_TEST(test_service_0x10__tc016_relock_on_session_refresh);
    RUN_TEST(test_service_0x10__tc017_rejected_transition_keeps_unlock);
    RUN_TEST(test_service_0x10__tc018_default_restores_comm_and_dtc_setting);
    RUN_TEST(test_service_0x10__tc019_s3_timeout_restores_comm_and_dtc_setting);
    RUN_TEST(test_service_0x10__tc020_nondefault_transition_keeps_comm_state);
    RUN_TEST(test_service_0x10__tc021_session_change_aborts_transfer);
    RUN_TEST(test_service_0x10__tc022_s3_timeout_aborts_transfer);
    RUN_TEST(test_service_0x10__tc004b_safety_system_session_disabled_by_default);
}
