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
 * TC-0x10-004: Safety system session (0x04) → 0x50, echo 0x04.
 */
ZTEST(test_service_0x10, tc004_safety_system_session)
{
    setup();
    uds_msg_buf_t req  = make_req(0x04U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x10_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "handler must return OK");
    zassert_equal(resp.data[1], 0x04U, "echo sub-function must be 0x04");
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
}
