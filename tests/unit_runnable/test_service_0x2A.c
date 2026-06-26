// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_service_0x2A.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x2A.c
 *                    SID 0x2A — ReadDataByPeriodicIdentifier handler
 *
 * PURPOSE:
 *   Verify all branches of the 0x2A handler: transmissionMode validation,
 *   DID lookup, access-flag checking, subscription registration, stop-sending,
 *   and positive-response encoding.
 *
 *   Two test DIDs are registered:
 *     0xF2AB — readable (DID_ACCESS_READ)  — periodic identifier 0xAB
 *     0xF2CD — write-only (DID_ACCESS_WRITE) — used to test "no read flag" path
 *
 * TEST CASES:
 *   TC-0x2A-001  NULL ctx  → ERR_NULL_PTR
 *   TC-0x2A-002  NULL req  → ERR_NULL_PTR (from uds_service_validate_length)
 *   TC-0x2A-003  Request too short (len 2)  → ERR_INVALID_PARAM
 *   TC-0x2A-004  transmissionMode 0x00 (below SLOW) → ERR_SUBFUNCTION_NOT_SUP
 *   TC-0x2A-005  transmissionMode 0x05 (above STOP)  → ERR_SUBFUNCTION_NOT_SUP
 *   TC-0x2A-006  Subscribe 0xAB with SLOW (0x01)   → OK; response SID = 0x6A
 *   TC-0x2A-007  Subscribe 0xAB with MEDIUM (0x02) → OK
 *   TC-0x2A-008  Subscribe 0xAB with FAST (0x03)   → OK
 *   TC-0x2A-009  stopSending (0x04) for 0xAB → OK (unsubscribes; no error)
 *   TC-0x2A-010  Subscribe DID not in database (0xF2FF) → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x2A-011  Subscribe write-only DID 0xF2CD (no DID_ACCESS_READ) → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x2A-012  Positive response: data[0] = 0x6A, length = 1
 *
 * FRAMEWORK: Zephyr Ztest (compiled as host Unity via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_periodic.h"
#include "uds_types.h"
#include "did_database.h"

/* =========================================================================
 * Stub callbacks for UDS security seed/key (required by uds_security_init)
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
        if (k[i] != (uint8_t)(s[i] ^ 0xAAU)) { return false; }
    }
    return true;
}

/* =========================================================================
 * Test DID read callbacks
 * ========================================================================= */

static uds_status_t t_read_f2ab(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    (void)buf_len;
    buf[0]   = 0x01U;
    *out_len = (uint16_t)1U;
    return UDS_STATUS_OK;
}

/* =========================================================================
 * Stack context (initialised once per binary)
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;
static bool               g_stack_ready = false;

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

        /* Initialise DID database and register test DIDs */
        (void)did_database_init();

        static const did_entry_t s_did_readable = {
            .did_id             = (uint16_t)0xF2ABU,
            .access_flags       = (uint8_t)DID_ACCESS_READ,
            .min_session        = (uint8_t)2U,   /* EXTENDED */
            .read_access_level  = (uint8_t)0U,
            .write_access_level = (uint8_t)0U,
            .data_length        = (uint16_t)1U,
            .read_cb            = t_read_f2ab,
            .write_cb           = NULL,
            .description        = "Test readable periodic DID",
        };
        (void)did_database_register(&s_did_readable);

        static const did_entry_t s_did_writeonly = {
            .did_id             = (uint16_t)0xF2CDU,
            .access_flags       = (uint8_t)DID_ACCESS_WRITE,
            .min_session        = (uint8_t)2U,
            .read_access_level  = (uint8_t)0U,
            .write_access_level = (uint8_t)0U,
            .data_length        = (uint16_t)1U,
            .read_cb            = NULL,
            .write_cb           = NULL,
            .description        = "Test write-only DID",
        };
        (void)did_database_register(&s_did_writeonly);

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

    /* Reset session to EXTENDED before each test (0x2A is non-default) */
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    (void)uds_periodic_init();
}

/** Build a 0x2A request: [0x2A, mode, id1 {, id2 ...}] */
static uds_msg_buf_t make_req(uint8_t mode, uint8_t periodic_id)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = (uint8_t)0x2AU;
    r.data[1] = mode;
    r.data[2] = periodic_id;
    r.length  = (uint16_t)3U;
    return r;
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_service_0x2A, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------- */

/**
 * TC-0x2A-001: NULL ctx → ERR_NULL_PTR.
 */
ZTEST(test_service_0x2A, tc001_null_ctx)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_SLOW, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(NULL, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must return ERR_NULL_PTR");
}

/**
 * TC-0x2A-002: NULL req → ERR_NULL_PTR (from validate_length).
 */
ZTEST(test_service_0x2A, tc002_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, NULL, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL req must return ERR_NULL_PTR");
}

/**
 * TC-0x2A-003: Request too short (length 2, missing periodic ID) → ERR_INVALID_PARAM.
 */
ZTEST(test_service_0x2A, tc003_too_short)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = (uint8_t)0x2AU;
    req.data[1] = UDS_PERIODIC_MODE_SLOW;
    req.length  = (uint16_t)2U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "request shorter than 3 bytes must be rejected");
}

/**
 * TC-0x2A-004: transmissionMode 0x00 (below SLOW) → ERR_SUBFUNCTION_NOT_SUP.
 */
ZTEST(test_service_0x2A, tc004_invalid_mode_zero)
{
    setup();
    uds_msg_buf_t req  = make_req((uint8_t)0x00U, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  "mode 0x00 must return ERR_SUBFUNCTION_NOT_SUP");
}

/**
 * TC-0x2A-005: transmissionMode 0x05 (above STOP=0x04) → ERR_SUBFUNCTION_NOT_SUP.
 */
ZTEST(test_service_0x2A, tc005_invalid_mode_high)
{
    setup();
    uds_msg_buf_t req  = make_req((uint8_t)0x05U, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  "mode 0x05 must return ERR_SUBFUNCTION_NOT_SUP");
}

/**
 * TC-0x2A-006: Subscribe 0xAB with SLOW (0x01) → OK.
 */
ZTEST(test_service_0x2A, tc006_subscribe_slow)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_SLOW, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "SLOW subscribe for 0xAB must succeed");
}

/**
 * TC-0x2A-007: Subscribe 0xAB with MEDIUM (0x02) → OK.
 */
ZTEST(test_service_0x2A, tc007_subscribe_medium)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_MEDIUM, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "MEDIUM subscribe for 0xAB must succeed");
}

/**
 * TC-0x2A-008: Subscribe 0xAB with FAST (0x03) → OK.
 */
ZTEST(test_service_0x2A, tc008_subscribe_fast)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_FAST, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "FAST subscribe for 0xAB must succeed");
}

/**
 * TC-0x2A-009: stopSending (0x04) for 0xAB → OK.
 * Also verifies stopSending is a no-op when the ID is not subscribed.
 */
ZTEST(test_service_0x2A, tc009_stop_sending)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_STOP, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    /* Stop on an ID that was never subscribed must still return OK. */
    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "stopSending must return OK");
}

/**
 * TC-0x2A-010: Subscribe a DID not in the database (0xF2FF) → ERR_REQUEST_OUT_OF_RANGE.
 * Periodic identifier 0xFF → DID 0xF2FF, which is not registered.
 */
ZTEST(test_service_0x2A, tc010_unknown_did)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_SLOW, (uint8_t)0xFFU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "DID not in database must return ERR_REQUEST_OUT_OF_RANGE");
}

/**
 * TC-0x2A-011: Subscribe a write-only DID (0xF2CD, no DID_ACCESS_READ flag) →
 *              ERR_REQUEST_OUT_OF_RANGE.
 */
ZTEST(test_service_0x2A, tc011_write_only_did)
{
    setup();
    /* 0xCD → DID 0xF2CD, registered as write-only */
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_SLOW, (uint8_t)0xCDU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "write-only DID must return ERR_REQUEST_OUT_OF_RANGE");
}

/**
 * TC-0x2A-012: Positive response format: resp.data[0] = 0x6A, resp.length = 1.
 */
ZTEST(test_service_0x2A, tc012_positive_response_format)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_SLOW, (uint8_t)0xABU);
    uds_msg_buf_t resp;
    memset(&resp, 0, sizeof(resp));

    (void)uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], (uint8_t)0x6AU,
                  "positive response SID must be 0x6A");
    zassert_equal(resp.length, (uint16_t)1U,
                  "positive response for 0x2A must be exactly 1 byte");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x2A__tc001_null_ctx(void);
extern void test_service_0x2A__tc002_null_req(void);
extern void test_service_0x2A__tc003_too_short(void);
extern void test_service_0x2A__tc004_invalid_mode_zero(void);
extern void test_service_0x2A__tc005_invalid_mode_high(void);
extern void test_service_0x2A__tc006_subscribe_slow(void);
extern void test_service_0x2A__tc007_subscribe_medium(void);
extern void test_service_0x2A__tc008_subscribe_fast(void);
extern void test_service_0x2A__tc009_stop_sending(void);
extern void test_service_0x2A__tc010_unknown_did(void);
extern void test_service_0x2A__tc011_write_only_did(void);
extern void test_service_0x2A__tc012_positive_response_format(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x2A__tc001_null_ctx);
    RUN_TEST(test_service_0x2A__tc002_null_req);
    RUN_TEST(test_service_0x2A__tc003_too_short);
    RUN_TEST(test_service_0x2A__tc004_invalid_mode_zero);
    RUN_TEST(test_service_0x2A__tc005_invalid_mode_high);
    RUN_TEST(test_service_0x2A__tc006_subscribe_slow);
    RUN_TEST(test_service_0x2A__tc007_subscribe_medium);
    RUN_TEST(test_service_0x2A__tc008_subscribe_fast);
    RUN_TEST(test_service_0x2A__tc009_stop_sending);
    RUN_TEST(test_service_0x2A__tc010_unknown_did);
    RUN_TEST(test_service_0x2A__tc011_write_only_did);
    RUN_TEST(test_service_0x2A__tc012_positive_response_format);
}
