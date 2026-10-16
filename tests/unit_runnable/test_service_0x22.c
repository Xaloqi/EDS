// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit/test_service_0x22.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x22.c
 *                    SID 0x22 — ReadDataByIdentifier
 *
 * PURPOSE:
 *   Verify all branches of the 0x22 handler: DID lookup, positive-response
 *   encoding, unknown-DID rejection, malformed request rejection.
 *
 * TEST CASES:
 *   TC-0x22-001  Valid DID 0x0C00 (Engine Speed)  → 0x62 + DID echo + data
 *   TC-0x22-002  Valid DID 0x0500 (Coolant Temp)  → positive response
 *   TC-0x22-003  Valid DID 0xF190 (VIN)           → positive response
 *   TC-0x22-004  Valid DID 0xF18C (ECU Serial)    → positive response
 *   TC-0x22-005  Valid DID 0xF187 (Spare Part)    → positive response
 *   TC-0x22-006  Unknown DID 0xDEAD               → ERR_DID_NOT_FOUND
 *   TC-0x22-007  Request length == 2 (no DID)     → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x22-008  Request length == 1 (SID only)   → ERR_INVALID_PARAM
 *   TC-0x22-009  Odd payload length (1 DID byte)  → ERR_INVALID_PARAM
 *   TC-0x22-010  Response SID byte == 0x62
 *   TC-0x22-011  DID echoed in response bytes [1],[2]
 *   TC-0x22-012  NULL req  → ERR_NULL_PTR
 *   TC-0x22-013  NULL resp → ERR_NULL_PTR (from write_pos_sid)
 *
 * FRAMEWORK: Zephyr Ztest
 *
 * NOTE: The current Phase-1 implementation of service_0x22.c uses a stub
 *       read callback (zero-fill). These tests validate the dispatching
 *       logic and DID database lookups, not actual sensor values.
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

/* [EDS#465] Test DIDs for the Figure 21 cases. */
#define TEST_DID_EXT_ONLY  (0xA100U)  /**< Readable in EXTENDED only. */
#define TEST_DID_SECURED   (0xA101U)  /**< Read needs security level 1. */
#define TEST_DID_COND_FAIL (0xA102U)  /**< read_cb reports a condition failure. */
#define TEST_DID_WRITE_ONLY (0xA103U) /**< No DID_ACCESS_READ. */

static uds_status_t t_read_ok(uint8_t *buf, uint16_t len, uint16_t *out)
{
    (void)len;
    buf[0] = 0x5AU;
    *out   = 1U;
    return UDS_STATUS_OK;
}
static uds_status_t t_read_cond_fail(uint8_t *buf, uint16_t len, uint16_t *out)
{
    (void)buf; (void)len; (void)out;
    return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
}

static void register_test_dids(void)
{
    static const struct { uint16_t id; uint8_t flags; uint8_t sess; uint8_t lvl;
                          did_read_cb_fn cb; } k[] = {
        { TEST_DID_EXT_ONLY,   (uint8_t)DID_ACCESS_READ,  (uint8_t)UDS_SESSION_EXTENDED, 0U, t_read_ok },
        { TEST_DID_SECURED,    (uint8_t)DID_ACCESS_READ,  (uint8_t)UDS_SESSION_DEFAULT,  1U, t_read_ok },
        { TEST_DID_COND_FAIL,  (uint8_t)DID_ACCESS_READ,  (uint8_t)UDS_SESSION_DEFAULT,  0U, t_read_cond_fail },
        { TEST_DID_WRITE_ONLY, (uint8_t)DID_ACCESS_WRITE, (uint8_t)UDS_SESSION_DEFAULT,  0U, t_read_ok },
    };
    for (uint8_t i = 0U; i < 4U; i++) {
        did_entry_t e;
        memset(&e, 0, sizeof(e));
        e.did_id            = k[i].id;
        e.access_flags      = k[i].flags;
        e.min_session       = k[i].sess;
        e.read_access_level = k[i].lvl;
        e.data_length       = 1U;
        e.read_cb           = k[i].cb;
        e.description       = "test";
        (void)did_database_register(&e);
    }
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
    /* Initialise only once — DID database cannot be re-inited */
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

        /* Register the 5 generated DIDs */
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

    /* Reset session to DEFAULT before each test */
    uds_session_transition(&g_sess, UDS_SESSION_DEFAULT);
}

/** Build a ReadDataByIdentifier request for a single DID. */
static uds_msg_buf_t make_req(uint16_t did)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x22U;
    r.data[1] = (uint8_t)((did >> 8U) & 0xFFU);
    r.data[2] = (uint8_t)(did & 0xFFU);
    r.length  = 3U;
    return r;
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_service_0x22, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------- */

/**
 * TC-0x22-001: Valid DID 0x0C00 (Engine Speed) → positive response.
 * Response: [0x62, 0x0C, 0x00, <data>]
 */
ZTEST(test_service_0x22, tc001_engine_speed_ok)
{
    setup();
    uds_msg_buf_t req  = make_req(0x0C00U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x22_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,   "ReadDBI for 0x0C00 must succeed");
    zassert_equal(resp.data[0], 0x62U, "Response SID must be 0x62");
    zassert_equal(resp.data[1], 0x0CU, "DID high byte echoed");
    zassert_equal(resp.data[2], 0x00U, "DID low  byte echoed");
    zassert_true(resp.length >= 3U,    "Response must have at least 3 bytes");
}

/**
 * TC-0x22-002: Valid DID 0x0500 (Coolant Temperature) → positive response.
 */
ZTEST(test_service_0x22, tc002_coolant_temp_ok)
{
    setup();
    uds_msg_buf_t req  = make_req(0x0500U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x22_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,   "ReadDBI for 0x0500 must succeed");
    zassert_equal(resp.data[0], 0x62U, "Response SID must be 0x62");
    zassert_equal(resp.data[1], 0x05U, "DID high byte echoed");
    zassert_equal(resp.data[2], 0x00U, "DID low  byte echoed");
}

/**
 * TC-0x22-003: Valid DID 0xF190 (VIN) → positive response with DID echoed.
 */
ZTEST(test_service_0x22, tc003_vin_ok)
{
    setup();
    uds_msg_buf_t req  = make_req(0xF190U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x22_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,   "ReadDBI for 0xF190 must succeed");
    zassert_equal(resp.data[1], 0xF1U, "DID high byte echoed");
    zassert_equal(resp.data[2], 0x90U, "DID low  byte echoed");
}

/**
 * TC-0x22-004: Valid DID 0xF18C (ECU Serial Number) → positive response.
 */
ZTEST(test_service_0x22, tc004_ecu_serial_ok)
{
    setup();
    uds_msg_buf_t req  = make_req(0xF18CU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x22_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,   "ReadDBI for 0xF18C must succeed");
    zassert_equal(resp.data[1], 0xF1U, "DID high byte echoed");
    zassert_equal(resp.data[2], 0x8CU, "DID low  byte echoed");
}

/**
 * TC-0x22-005: Valid DID 0xF187 (Spare Part Number) → positive response.
 * Note: In Phase-1 the handler uses stub data; session/security checks are
 * Phase-2. The lookup itself must succeed.
 */
ZTEST(test_service_0x22, tc005_spare_part_ok)
{
    setup();
    /* DID 0xF187 requires EXTENDED session */
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    uds_msg_buf_t req  = make_req(0xF187U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x22_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,   "ReadDBI for 0xF187 must succeed");
    zassert_equal(resp.data[1], 0xF1U, "DID high byte echoed");
    zassert_equal(resp.data[2], 0x87U, "DID low  byte echoed");
}

/**
 * TC-0x22-006: Unknown DID 0xDEAD → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31).
 */
ZTEST(test_service_0x22, tc006_unknown_did)
{
    setup();
    uds_msg_buf_t req  = make_req(0xDEADU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x22_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "Unknown DID must return NRC 0x31 (EDS#465)");
}

/**
 * TC-0x22-007: Request length == 2 ([0x22, hi] — incomplete DID pair)
 *              → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x22, tc007_truncated_request_len2)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x22U;
    req.data[1] = 0x0CU;
    req.length  = 2U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x22_handler(&g_srv, &req, &resp);

    /* Length 2 = SID + 1 byte → odd DID payload → ERR_INVALID_PARAM */
    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Incomplete DID pair must be rejected");
}

/**
 * TC-0x22-008: Request length == 1 (SID only) → ERR_INVALID_PARAM.
 */
ZTEST(test_service_0x22, tc008_sid_only_len1)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x22U;
    req.length  = 1U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x22_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "SID-only request must fail");
}

/**
 * TC-0x22-009: Odd payload length after SID → ERR_INVALID_PARAM.
 * Request = [0x22, 0x0C, 0x00, 0xF1] → 4 bytes total, 3 payload bytes (odd).
 */
ZTEST(test_service_0x22, tc009_odd_payload_rejected)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x22U;
    req.data[1] = 0x0CU;
    req.data[2] = 0x00U;
    req.data[3] = 0xF1U; /* Extra lone byte — odd DID count */
    req.length  = 4U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x22_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Odd payload length must be rejected (incomplete DID pair)");
}

/**
 * TC-0x22-010: Response SID byte is 0x62 for all valid requests.
 */
ZTEST(test_service_0x22, tc010_response_sid_0x62)
{
    setup();
    uint16_t dids[] = { 0x0C00U, 0x0500U, 0xF190U, 0xF18CU, 0xF187U };

    for (size_t i = 0; i < 5U; i++) {
        uds_msg_buf_t req  = make_req(dids[i]);
        uds_msg_buf_t resp = {0};
        uds_service_0x22_handler(&g_srv, &req, &resp);
        zassert_equal(resp.data[0], 0x62U,
                      "Response SID must be 0x62 for DID 0x%04X", dids[i]);
    }
}

/**
 * TC-0x22-011: DID identifier echoed verbatim in response bytes [1] and [2].
 */
ZTEST(test_service_0x22, tc011_did_echoed_in_response)
{
    setup();
    uds_msg_buf_t req  = make_req(0xF190U);
    uds_msg_buf_t resp = {0};

    uds_service_0x22_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[1], 0xF1U, "DID high byte must be echoed");
    zassert_equal(resp.data[2], 0x90U, "DID low  byte must be echoed");
}

/**
 * TC-0x22-012: NULL req → ERR_NULL_PTR.
 */
ZTEST(test_service_0x22, tc012_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x22_handler(&g_srv, NULL, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL req must fail");
}

/**
 * TC-0x22-013: NULL resp → ERR_NULL_PTR from write_pos_sid.
 */
ZTEST(test_service_0x22, tc013_null_resp)
{
    setup();
    uds_msg_buf_t req = make_req(0x0C00U);

    uds_status_t rc = uds_service_0x22_handler(&g_srv, &req, NULL);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL resp must fail");
}

/* ---- [EDS#465] ISO 14229-1:2026 §10.2.4, Figure 21 -------------------- */

static uds_msg_buf_t make_req_n(const uint16_t *dids, uint16_t n)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x22U;
    for (uint16_t i = 0U; i < n; i++) {
        r.data[1U + (2U * i)] = (uint8_t)(dids[i] >> 8U);
        r.data[2U + (2U * i)] = (uint8_t)(dids[i] & 0xFFU);
    }
    r.length = (uint16_t)(1U + (2U * n));
    return r;
}

/** TC-0x22-014: an unsupported DID among supported ones is skipped; the
 *  response carries the supported DIDs only. */
ZTEST(test_service_0x22, tc014_unsupported_did_skipped)
{
    setup();
    static const uint16_t dids[] = { 0xDEADU, TEST_DID_EXT_ONLY, TEST_DID_WRITE_ONLY, 0xF190U };
    uds_msg_buf_t req  = make_req_n(dids, 4U);
    static uds_msg_buf_t resp;
    memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_service_0x22_handler(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "supported DIDs must still be answered (EDS#465)");
    zassert_equal(resp.data[1], 0xF1U, "only the VIN is supported in DEFAULT");
    zassert_equal(resp.data[2], 0x90U, "VIN echoed");
}

/** TC-0x22-015: none supported in the active session → NRC 0x31; the same
 *  DID becomes readable in its session. */
ZTEST(test_service_0x22, tc015_none_supported_in_session)
{
    setup();
    static const uint16_t dids[] = { TEST_DID_EXT_ONLY, 0xDEADU };
    uds_msg_buf_t req  = make_req_n(dids, 2U);
    static uds_msg_buf_t resp;
    zassert_equal(uds_service_0x22_handler(&g_srv, &req, &resp),
                  UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE, "none supported → 0x31");

    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    zassert_equal(uds_service_0x22_handler(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "EXTENDED-only DID readable in EXTENDED");
    zassert_equal(resp.length, 4U, "one DID: 62 A1 00 5A");
}

/** TC-0x22-016: a secured DID while locked → NRC 0x33, even after a
 *  skipped unsupported DID. */
ZTEST(test_service_0x22, tc016_secured_did_denied)
{
    setup();
    static const uint16_t dids[] = { 0xDEADU, TEST_DID_SECURED };
    uds_msg_buf_t req  = make_req_n(dids, 2U);
    static uds_msg_buf_t resp;
    zassert_equal(uds_service_0x22_handler(&g_srv, &req, &resp),
                  UDS_STATUS_ERR_SEC_NOT_UNLOCKED, "secured DID → 0x33");
}

/** TC-0x22-017: a DID condition failure → NRC 0x22, not 0x31. */
ZTEST(test_service_0x22, tc017_condition_failure_is_0x22)
{
    setup();
    uds_msg_buf_t req  = make_req(TEST_DID_COND_FAIL);
    static uds_msg_buf_t resp;
    zassert_equal(uds_service_0x22_handler(&g_srv, &req, &resp),
                  UDS_STATUS_ERR_CONDITIONS_NOT_MET, "condition failure → 0x22 (EDS#465)");
}

/** TC-0x22-018: total response over the transport limit → NRC 0x14. */
ZTEST(test_service_0x22, tc018_response_too_long)
{
    setup();
    static uint16_t dids[300];
    for (uint16_t i = 0U; i < 300U; i++) { dids[i] = 0xF190U; }  /* 300 x (2 + 17) */
    static uds_msg_buf_t req;
    static uds_msg_buf_t resp;
    req = make_req_n(dids, 300U);
    zassert_equal(uds_service_0x22_handler(&g_srv, &req, &resp),
                  UDS_STATUS_ERR_RESPONSE_TOO_LONG, "response > 4095 → 0x14 (EDS#465)");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x22__tc001_engine_speed_ok(void);
extern void test_service_0x22__tc002_coolant_temp_ok(void);
extern void test_service_0x22__tc003_vin_ok(void);
extern void test_service_0x22__tc004_ecu_serial_ok(void);
extern void test_service_0x22__tc005_spare_part_ok(void);
extern void test_service_0x22__tc006_unknown_did(void);
extern void test_service_0x22__tc007_truncated_request_len2(void);
extern void test_service_0x22__tc008_sid_only_len1(void);
extern void test_service_0x22__tc009_odd_payload_rejected(void);
extern void test_service_0x22__tc010_response_sid_0x62(void);
extern void test_service_0x22__tc011_did_echoed_in_response(void);
extern void test_service_0x22__tc012_null_req(void);
extern void test_service_0x22__tc013_null_resp(void);
extern void test_service_0x22__tc014_unsupported_did_skipped(void);
extern void test_service_0x22__tc015_none_supported_in_session(void);
extern void test_service_0x22__tc016_secured_did_denied(void);
extern void test_service_0x22__tc017_condition_failure_is_0x22(void);
extern void test_service_0x22__tc018_response_too_long(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x22__tc001_engine_speed_ok);
    RUN_TEST(test_service_0x22__tc002_coolant_temp_ok);
    RUN_TEST(test_service_0x22__tc003_vin_ok);
    RUN_TEST(test_service_0x22__tc004_ecu_serial_ok);
    RUN_TEST(test_service_0x22__tc005_spare_part_ok);
    RUN_TEST(test_service_0x22__tc006_unknown_did);
    RUN_TEST(test_service_0x22__tc007_truncated_request_len2);
    RUN_TEST(test_service_0x22__tc008_sid_only_len1);
    RUN_TEST(test_service_0x22__tc009_odd_payload_rejected);
    RUN_TEST(test_service_0x22__tc010_response_sid_0x62);
    RUN_TEST(test_service_0x22__tc011_did_echoed_in_response);
    RUN_TEST(test_service_0x22__tc012_null_req);
    RUN_TEST(test_service_0x22__tc013_null_resp);
    RUN_TEST(test_service_0x22__tc014_unsupported_did_skipped);
    RUN_TEST(test_service_0x22__tc015_none_supported_in_session);
    RUN_TEST(test_service_0x22__tc016_secured_did_denied);
    RUN_TEST(test_service_0x22__tc017_condition_failure_is_0x22);
    RUN_TEST(test_service_0x22__tc018_response_too_long);
}
