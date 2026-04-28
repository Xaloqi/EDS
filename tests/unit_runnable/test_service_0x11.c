// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit/test_service_0x11.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x11.c
 *                    SID 0x11 — ECUReset
 *
 * PURPOSE:
 *   Verify all branches of the 0x11 handler: each supported reset type,
 *   unsupported reset type rejection, length guards.
 *
 * TEST CASES:
 *   TC-0x11-001  Hard reset (0x01)          → 0x51 0x01, length 2
 *   TC-0x11-002  Key off/on reset (0x02)    → 0x51 0x02, length 2
 *   TC-0x11-003  Soft reset (0x03)          → 0x51 0x03, length 2
 *   TC-0x11-004  Unsupported type 0x04      → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12)
 *   TC-0x11-005  Unsupported type 0xFF      → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12)
 *   TC-0x11-006  Request length == 1        → ERR_INVALID_PARAM  (NRC 0x13)
 *   TC-0x11-007  Request length == 0        → ERR_INVALID_PARAM
 *   TC-0x11-008  Response length exactly 2
 *   TC-0x11-009  Response SID byte is 0x51 (0x11 + 0x40 offset)
 *   TC-0x11-010  Reset type echoed in response data[1]
 *   TC-0x11-011  NULL req  → ERR_NULL_PTR
 *   TC-0x11-012  NULL resp → ERR_NULL_PTR
 *   TC-0x11-013  Oversized request (len > 2) is still accepted (extra bytes ignored)
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
 * Context setup
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

static uds_msg_buf_t make_req(uint8_t reset_type)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x11U;
    r.data[1] = reset_type;
    r.length  = 2U;
    return r;
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_service_0x11, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-0x11-001: Hard reset (0x01) → positive response 0x51 0x01, length 2.
 */
ZTEST(test_service_0x11, tc001_hard_reset)
{
    setup();
    uds_msg_buf_t req  = make_req(0x01U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x11_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "Hard reset must return OK");
    zassert_equal(resp.data[0], 0x51U, "Response SID must be 0x51");
    zassert_equal(resp.data[1], 0x01U, "Reset type echoed as 0x01");
    zassert_equal(resp.length,  2U,    "Response must be 2 bytes");
}

/**
 * TC-0x11-002: Key off/on reset (0x02) → 0x51 0x02.
 */
ZTEST(test_service_0x11, tc002_key_off_on_reset)
{
    setup();
    uds_msg_buf_t req  = make_req(0x02U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x11_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "KeyOffOn reset must return OK");
    zassert_equal(resp.data[0], 0x51U, "Response SID must be 0x51");
    zassert_equal(resp.data[1], 0x02U, "Reset type echoed as 0x02");
    zassert_equal(resp.length,  2U,    "Response must be 2 bytes");
}

/**
 * TC-0x11-003: Soft reset (0x03) → 0x51 0x03.
 */
ZTEST(test_service_0x11, tc003_soft_reset)
{
    setup();
    uds_msg_buf_t req  = make_req(0x03U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x11_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "Soft reset must return OK");
    zassert_equal(resp.data[0], 0x51U, "Response SID must be 0x51");
    zassert_equal(resp.data[1], 0x03U, "Reset type echoed as 0x03");
}

/**
 * TC-0x11-004: Unsupported reset type 0x04 → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12).
 */
ZTEST(test_service_0x11, tc004_unsupported_type_0x04)
{
    setup();
    uds_msg_buf_t req  = make_req(0x04U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x11_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  "Reset type 0x04 must be rejected (NRC 0x12)");
}

/**
 * TC-0x11-005: Unsupported reset type 0xFF → ERR_SERVICE_NOT_SUPPORTED.
 */
ZTEST(test_service_0x11, tc005_unsupported_type_0xFF)
{
    setup();
    uds_msg_buf_t req  = make_req(0xFFU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x11_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  "Reset type 0xFF must be rejected (NRC 0x12)");
}

/**
 * TC-0x11-006: Request length == 1 → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x11, tc006_short_request_len1)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x11U;
    req.length  = 1U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x11_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Length 1 must fail with ERR_INVALID_PARAM");
}

/**
 * TC-0x11-007: Request length == 0 → ERR_INVALID_PARAM.
 */
ZTEST(test_service_0x11, tc007_zero_length)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.length = 0U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x11_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Zero length must fail");
}

/**
 * TC-0x11-008: Response length is exactly 2.
 */
ZTEST(test_service_0x11, tc008_response_length_2)
{
    setup();
    uds_msg_buf_t req  = make_req(0x01U);
    uds_msg_buf_t resp = {0};

    uds_service_0x11_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 2U, "ECUReset response must be exactly 2 bytes");
}

/**
 * TC-0x11-009: Response SID byte is 0x51 (positive offset = 0x40).
 */
ZTEST(test_service_0x11, tc009_positive_sid_offset)
{
    setup();
    uds_msg_buf_t req  = make_req(0x02U);
    uds_msg_buf_t resp = {0};

    uds_service_0x11_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], (uint8_t)(0x11U + 0x40U),
                  "Response SID must be 0x51");
}

/**
 * TC-0x11-010: Reset type echoed verbatim in response data[1].
 */
ZTEST(test_service_0x11, tc010_reset_type_echoed)
{
    setup();

    uint8_t types[] = { 0x01U, 0x02U, 0x03U };
    for (size_t i = 0; i < 3U; i++) {
        uds_msg_buf_t req  = make_req(types[i]);
        uds_msg_buf_t resp = {0};
        uds_service_0x11_handler(&g_srv, &req, &resp);
        zassert_equal(resp.data[1], types[i],
                      "Reset type 0x%02X must be echoed", types[i]);
    }
}

/**
 * TC-0x11-011: NULL req → ERR_NULL_PTR.
 */
ZTEST(test_service_0x11, tc011_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x11_handler(&g_srv, NULL, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL req must return ERR_NULL_PTR");
}

/**
 * TC-0x11-012: NULL resp → ERR_NULL_PTR from write_pos_sid.
 */
ZTEST(test_service_0x11, tc012_null_resp)
{
    setup();
    uds_msg_buf_t req = make_req(0x01U);

    uds_status_t rc = uds_service_0x11_handler(&g_srv, &req, NULL);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL resp must return ERR_NULL_PTR");
}

/**
 * TC-0x11-013: Oversized request (3 bytes) — extra bytes ignored, OK.
 * ISO 14229-1 does not require rejection of extra bytes for this service.
 */
ZTEST(test_service_0x11, tc013_oversized_request_ok)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x11U;
    req.data[1] = 0x01U;
    req.data[2] = 0xFFU; /* Extra byte */
    req.length  = 3U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x11_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "Oversized request must still succeed");
    zassert_equal(resp.data[1], 0x01U, "Reset type must still be 0x01");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x11__tc001_hard_reset(void);
extern void test_service_0x11__tc002_key_off_on_reset(void);
extern void test_service_0x11__tc003_soft_reset(void);
extern void test_service_0x11__tc004_unsupported_type_0x04(void);
extern void test_service_0x11__tc005_unsupported_type_0xFF(void);
extern void test_service_0x11__tc006_short_request_len1(void);
extern void test_service_0x11__tc007_zero_length(void);
extern void test_service_0x11__tc008_response_length_2(void);
extern void test_service_0x11__tc009_positive_sid_offset(void);
extern void test_service_0x11__tc010_reset_type_echoed(void);
extern void test_service_0x11__tc011_null_req(void);
extern void test_service_0x11__tc012_null_resp(void);
extern void test_service_0x11__tc013_oversized_request_ok(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x11__tc001_hard_reset);
    RUN_TEST(test_service_0x11__tc002_key_off_on_reset);
    RUN_TEST(test_service_0x11__tc003_soft_reset);
    RUN_TEST(test_service_0x11__tc004_unsupported_type_0x04);
    RUN_TEST(test_service_0x11__tc005_unsupported_type_0xFF);
    RUN_TEST(test_service_0x11__tc006_short_request_len1);
    RUN_TEST(test_service_0x11__tc007_zero_length);
    RUN_TEST(test_service_0x11__tc008_response_length_2);
    RUN_TEST(test_service_0x11__tc009_positive_sid_offset);
    RUN_TEST(test_service_0x11__tc010_reset_type_echoed);
    RUN_TEST(test_service_0x11__tc011_null_req);
    RUN_TEST(test_service_0x11__tc012_null_resp);
    RUN_TEST(test_service_0x11__tc013_oversized_request_ok);
}
