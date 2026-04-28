// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit/test_service_0x3E.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x3E.c
 *                    SID 0x3E — TesterPresent
 *
 * PURPOSE:
 *   Verify keepalive behaviour, suppress bit handling, invalid sub-function
 *   rejection, length guards and S3 timer reset.
 *
 * TEST CASES:
 *   TC-0x3E-001  subFn 0x00 → 0x7E 0x00, length 2
 *   TC-0x3E-002  subFn 0x80 (suppress bit) → OK (no NRC, timer still reset)
 *   TC-0x3E-003  subFn 0x01 (invalid) → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12)
 *   TC-0x3E-004  subFn 0x7F (invalid) → ERR_SUBFUNCTION_NOT_SUP
 *   TC-0x3E-005  Request length == 1 (SID only) → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x3E-006  Request length == 0            → ERR_INVALID_PARAM
 *   TC-0x3E-007  Response SID byte == 0x7E (0x3E + 0x40)
 *   TC-0x3E-008  Response sub-function byte is 0x00 (suppress cleared)
 *   TC-0x3E-009  Response length == 2
 *   TC-0x3E-010  S3 timer actually resets after handler
 *   TC-0x3E-011  S3 timer reset verifiable by tick count
 *   TC-0x3E-012  NULL req  → ERR_NULL_PTR
 *   TC-0x3E-013  NULL resp → ERR_NULL_PTR
 *   TC-0x3E-014  Keepalive in DEFAULT session also succeeds
 *   TC-0x3E-015  Keepalive in EXTENDED session resets S3 and prevents timeout
 *
 * FRAMEWORK: Zephyr Ztest
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

/* =========================================================================
 * Stubs
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
 * Context
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

static void setup(void)
{
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

    static const uds_server_cfg_t svc = {
        .p2_server_max_ms      = 25U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_uds_service_table,
        .service_table_count   = (uint8_t)UDS_SERVICE_TABLE_COUNT,
    };
    uds_server_init(&g_srv, &svc);
}

static uds_msg_buf_t make_req(uint8_t sub)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x3EU;
    r.data[1] = sub;
    r.length  = 2U;
    return r;
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_service_0x3E, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-0x3E-001: subFn 0x00 → positive response 0x7E 0x00, length 2.
 */
ZTEST(test_service_0x3E, tc001_subfn_zero_ok)
{
    setup();
    uds_msg_buf_t req  = make_req(0x00U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,   "subFn 0x00 must succeed");
    zassert_equal(resp.data[0], 0x7EU, "Response SID must be 0x7E");
    zassert_equal(resp.data[1], 0x00U, "Response subFn must be 0x00");
    zassert_equal(resp.length,  2U,    "Response must be 2 bytes");
}

/**
 * TC-0x3E-002: subFn 0x80 (suppressPosRspMsgIndicationBit set) → OK.
 * The handler itself returns OK; suppression is the server dispatcher's job.
 */
ZTEST(test_service_0x3E, tc002_suppress_bit_ok)
{
    setup();
    uds_msg_buf_t req  = make_req(0x80U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,
                  "subFn 0x80 (suppress bit) must return OK");
}

/**
 * TC-0x3E-003: subFn 0x01 (invalid) → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12).
 */
ZTEST(test_service_0x3E, tc003_invalid_subfn_0x01)
{
    setup();
    uds_msg_buf_t req  = make_req(0x01U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  "subFn 0x01 must return ERR_SUBFUNCTION_NOT_SUP");
}

/**
 * TC-0x3E-004: subFn 0x7F (invalid, just below suppress bit) → ERR_SUBFUNCTION_NOT_SUP.
 */
ZTEST(test_service_0x3E, tc004_invalid_subfn_0x7F)
{
    setup();
    uds_msg_buf_t req  = make_req(0x7FU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  "subFn 0x7F must be rejected");
}

/**
 * TC-0x3E-005: Request length == 1 → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x3E, tc005_length_1_rejected)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x3EU;
    req.length  = 1U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Length 1 must fail with ERR_INVALID_PARAM");
}

/**
 * TC-0x3E-006: Request length == 0 → ERR_INVALID_PARAM.
 */
ZTEST(test_service_0x3E, tc006_length_0_rejected)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.length = 0U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Zero length must fail");
}

/**
 * TC-0x3E-007: Response SID byte is 0x7E (0x3E + 0x40 positive offset).
 */
ZTEST(test_service_0x3E, tc007_response_sid_0x7E)
{
    setup();
    uds_msg_buf_t req  = make_req(0x00U);
    uds_msg_buf_t resp = {0};

    uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], (uint8_t)(0x3EU + 0x40U),
                  "Response SID must be 0x7E");
}

/**
 * TC-0x3E-008: Response sub-function byte is always 0x00 (suppress bit cleared).
 */
ZTEST(test_service_0x3E, tc008_response_subfn_zero)
{
    setup();
    /* Even when suppress bit is set in request, response byte must be 0x00 */
    uds_msg_buf_t req  = make_req(0x80U);
    uds_msg_buf_t resp = {0};

    uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[1], 0x00U,
                  "Response sub-function must always be 0x00");
}

/**
 * TC-0x3E-009: Response length is exactly 2.
 */
ZTEST(test_service_0x3E, tc009_response_length_2)
{
    setup();
    uds_msg_buf_t req  = make_req(0x00U);
    uds_msg_buf_t resp = {0};

    uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 2U, "TesterPresent response must be 2 bytes");
}

/**
 * TC-0x3E-010: S3 timer is reset after handler.
 * After consuming 200 ticks in EXTENDED session, call TesterPresent and
 * verify timer is back to 5000.
 */
ZTEST(test_service_0x3E, tc010_s3_timer_reset)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);

    /* Consume 200 ms of S3 timer */
    for (int i = 0; i < 200; i++) {
        uds_session_tick_1ms(&g_sess);
    }
    zassert_true(g_sess.s3_timer_ms < 5000U, "Timer must have decremented");

    /* TesterPresent keepalive */
    uds_msg_buf_t req  = make_req(0x00U);
    uds_msg_buf_t resp = {0};
    uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(g_sess.s3_timer_ms, 5000U,
                  "S3 timer must be fully reset after TesterPresent");
}

/**
 * TC-0x3E-011: Without keepalive, S3 times out; with keepalive, it doesn't.
 * Uses a short 50-ms S3 timeout.
 */
ZTEST(test_service_0x3E, tc011_keepalive_prevents_s3_timeout)
{
    /* Fresh setup with short S3 */
    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_sec,  0, sizeof(g_sec));
    memset(&g_srv,  0, sizeof(g_srv));

    uds_session_init(&g_sess, 50U);  /* 50-ms S3 */
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
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);

    /* Tick 40 ms — still within timeout window */
    for (int i = 0; i < 40; i++) {
        uds_server_tick_1ms(&g_srv);
    }

    /* Send keepalive — resets timer to 50 ms */
    uds_msg_buf_t req  = make_req(0x00U);
    uds_msg_buf_t resp = {0};
    uds_service_0x3E_handler(&g_srv, &req, &resp);

    /* Tick another 40 ms — if keepalive worked, no timeout yet */
    uds_status_t tick_rc = UDS_STATUS_OK;
    for (int i = 0; i < 40; i++) {
        tick_rc = uds_server_tick_1ms(&g_srv);
        if (tick_rc != UDS_STATUS_OK) break;
    }
    zassert_equal(tick_rc, UDS_STATUS_OK,
                  "TesterPresent keepalive must prevent S3 timeout within 40ms");

    /* Verify session still EXTENDED */
    uds_session_type_t active;
    uds_session_get_active(&g_sess, &active);
    zassert_equal(active, UDS_SESSION_EXTENDED,
                  "Session must remain EXTENDED after keepalive");
}

/**
 * TC-0x3E-012: NULL req → ERR_NULL_PTR.
 */
ZTEST(test_service_0x3E, tc012_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x3E_handler(&g_srv, NULL, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL req must fail");
}

/**
 * TC-0x3E-013: NULL resp → ERR_NULL_PTR from write_pos_sid.
 */
ZTEST(test_service_0x3E, tc013_null_resp)
{
    setup();
    uds_msg_buf_t req = make_req(0x00U);

    uds_status_t rc = uds_service_0x3E_handler(&g_srv, &req, NULL);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL resp must fail");
}

/**
 * TC-0x3E-014: TesterPresent works in DEFAULT session.
 * ISO 14229-1: TesterPresent is allowed in any session.
 */
ZTEST(test_service_0x3E, tc014_works_in_default_session)
{
    setup();
    /* Confirm in DEFAULT */
    uds_session_type_t active;
    uds_session_get_active(&g_sess, &active);
    zassert_equal(active, UDS_SESSION_DEFAULT, "Must be in DEFAULT");

    uds_msg_buf_t req  = make_req(0x00U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x3E_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,
                  "TesterPresent must succeed in DEFAULT session");
}

/**
 * TC-0x3E-015: TesterPresent in EXTENDED session resets S3 and prevents timeout.
 * Mirrors TC-011 with a different observation point.
 */
ZTEST(test_service_0x3E, tc015_extended_session_keepalive)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);

    /* Consume 4000 ms */
    for (int i = 0; i < 4000; i++) {
        uds_session_tick_1ms(&g_sess);
    }
    zassert_true(g_sess.s3_timer_ms < 1100U,
                 "S3 timer must be low after 4000 ticks");

    /* Keepalive */
    uds_msg_buf_t req  = make_req(0x00U);
    uds_msg_buf_t resp = {0};
    uds_service_0x3E_handler(&g_srv, &req, &resp);

    /* Must now have full 5000 ms again */
    zassert_equal(g_sess.s3_timer_ms, 5000U,
                  "Timer must reload to 5000 after keepalive in EXTENDED");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x3E__tc001_subfn_zero_ok(void);
extern void test_service_0x3E__tc002_suppress_bit_ok(void);
extern void test_service_0x3E__tc003_invalid_subfn_0x01(void);
extern void test_service_0x3E__tc004_invalid_subfn_0x7F(void);
extern void test_service_0x3E__tc005_length_1_rejected(void);
extern void test_service_0x3E__tc006_length_0_rejected(void);
extern void test_service_0x3E__tc007_response_sid_0x7E(void);
extern void test_service_0x3E__tc008_response_subfn_zero(void);
extern void test_service_0x3E__tc009_response_length_2(void);
extern void test_service_0x3E__tc010_s3_timer_reset(void);
extern void test_service_0x3E__tc011_keepalive_prevents_s3_timeout(void);
extern void test_service_0x3E__tc012_null_req(void);
extern void test_service_0x3E__tc013_null_resp(void);
extern void test_service_0x3E__tc014_works_in_default_session(void);
extern void test_service_0x3E__tc015_extended_session_keepalive(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x3E__tc001_subfn_zero_ok);
    RUN_TEST(test_service_0x3E__tc002_suppress_bit_ok);
    RUN_TEST(test_service_0x3E__tc003_invalid_subfn_0x01);
    RUN_TEST(test_service_0x3E__tc004_invalid_subfn_0x7F);
    RUN_TEST(test_service_0x3E__tc005_length_1_rejected);
    RUN_TEST(test_service_0x3E__tc006_length_0_rejected);
    RUN_TEST(test_service_0x3E__tc007_response_sid_0x7E);
    RUN_TEST(test_service_0x3E__tc008_response_subfn_zero);
    RUN_TEST(test_service_0x3E__tc009_response_length_2);
    RUN_TEST(test_service_0x3E__tc010_s3_timer_reset);
    RUN_TEST(test_service_0x3E__tc011_keepalive_prevents_s3_timeout);
    RUN_TEST(test_service_0x3E__tc012_null_req);
    RUN_TEST(test_service_0x3E__tc013_null_resp);
    RUN_TEST(test_service_0x3E__tc014_works_in_default_session);
    RUN_TEST(test_service_0x3E__tc015_extended_session_keepalive);
}
