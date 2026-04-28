// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
// ================================
// File: tests/unit/test_uds_server.c
// ================================
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit/test_uds_server.c
 *
 * MODULE UNDER TEST: core/uds_server.c
 *
 * PURPOSE:
 *   Verify the UDS server dispatcher. Tests cover:
 *     - uds_server_init: NULL guards, invalid-param guards,
 *       happy path, double-init
 *     - uds_server_process_request: NULL guards, not-initialised,
 *       service dispatch, unknown SID → NRC 0x11, handler error → NRC 0x22,
 *       request counter and negative-response counter
 *     - uds_server_build_negative_response: NULL guard, NRC encoding,
 *       all standard NRC values
 *     - uds_server_get_counters: NULL guard, correct counter values
 *     - uds_server_tick_1ms: NULL guard, delegation to session/security tick
 *
 * Each test uses a minimal real session + security context (no mocks
 * needed — those modules are already unit-tested separately).
 *
 * FRAMEWORK: Zephyr Ztest
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_types.h"

/* =========================================================================
 * Shared stubs for session / security / service table
 * ========================================================================= */

static uds_status_t stub_seed_gen(uint8_t lvl, uint8_t *buf, uint8_t len, uint8_t *out)
{
    (void)lvl;
    for (uint8_t i = 0; i < len && i < 4U; i++) { buf[i] = (uint8_t)(0x10U + i); }
    *out = (len < 4U) ? len : 4U;
    return UDS_STATUS_OK;
}

static bool stub_key_validate(uint8_t lvl, const uint8_t *seed, uint8_t sl,
                               const uint8_t *key, uint8_t kl)
{
    (void)lvl;
    if (sl != kl) return false;
    for (uint8_t i = 0; i < sl; i++) {
        if (key[i] != (uint8_t)(seed[i] ^ 0xAAU)) return false;
    }
    return true;
}

/** Minimal stub handler — returns OK with a 2-byte response. */
static uds_status_t stub_handler_ok(uds_server_ctx_t *ctx,
                                     const uds_msg_buf_t *req,
                                     uds_msg_buf_t *resp)
{
    (void)ctx; (void)req;
    resp->data[0] = 0x7EU;
    resp->data[1] = 0x00U;
    resp->length  = 2U;
    return UDS_STATUS_OK;
}

/** Stub handler that returns an error (triggers NRC). */
static uds_status_t stub_handler_err(uds_server_ctx_t *ctx,
                                      const uds_msg_buf_t *req,
                                      uds_msg_buf_t *resp)
{
    (void)ctx; (void)req; (void)resp;
    return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
}

/** Service table for tests — two entries. */
static const uds_service_entry_t g_test_service_table[] = {
    { 0x3EU, stub_handler_ok,  true  },   /* TesterPresent (stub) */
    { 0xFFU, stub_handler_err, false },   /* Artificial SID 0xFF — errors */
};
#define TEST_SVC_TABLE_COUNT  (2U)

/* =========================================================================
 * Helpers
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

static void ctx_init_all(void)
{
    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_sec,  0, sizeof(g_sec));
    memset(&g_srv,  0, sizeof(g_srv));

    uds_session_init(&g_sess, 5000U);

    static const uds_security_cfg_t sec_cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,
        .key_validate_cb  = stub_key_validate,
        .seed_generate_cb = stub_seed_gen,
    };
    uds_security_init(&g_sec, &sec_cfg);
}

static uds_status_t srv_init_with_table(void)
{
    static const uds_server_cfg_t cfg = {
        .p2_server_max_ms      = 25U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_test_service_table,
        .service_table_count   = (uint8_t)TEST_SVC_TABLE_COUNT,
    };
    return uds_server_init(&g_srv, &cfg);
}

/** Build a minimal request buffer. */
static uds_msg_buf_t make_req(uint8_t sid, uint8_t sub)
{
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = sid;
    req.data[1] = sub;
    req.length  = 2U;
    return req;
}

/* =========================================================================
 * Test suite: uds_server_init
 * ========================================================================= */

ZTEST_SUITE(test_uds_server_init, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SRV-INIT-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_server_init, test_null_ctx)
{
    ctx_init_all();
    static const uds_server_cfg_t cfg = {
        .p2_server_max_ms    = 25U,
        .session_ctx         = &g_sess,
        .security_ctx        = &g_sec,
        .service_table       = g_test_service_table,
        .service_table_count = (uint8_t)TEST_SVC_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(NULL, &cfg), UDS_STATUS_ERR_NULL_PTR,
                  "NULL ctx must fail");
}

/**
 * TC-SRV-INIT-002: NULL cfg → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_server_init, test_null_cfg)
{
    ctx_init_all();
    zassert_equal(uds_server_init(&g_srv, NULL), UDS_STATUS_ERR_NULL_PTR,
                  "NULL cfg must fail");
}

/**
 * TC-SRV-INIT-003: NULL session_ctx → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_uds_server_init, test_null_session_ctx)
{
    ctx_init_all();
    static const uds_server_cfg_t cfg = {
        .p2_server_max_ms    = 25U,
        .session_ctx         = NULL,
        .security_ctx        = &g_sec,
        .service_table       = g_test_service_table,
        .service_table_count = (uint8_t)TEST_SVC_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(&g_srv, &cfg), UDS_STATUS_ERR_INVALID_PARAM,
                  "NULL session_ctx must fail");
}

/**
 * TC-SRV-INIT-004: NULL security_ctx → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_uds_server_init, test_null_security_ctx)
{
    ctx_init_all();
    static const uds_server_cfg_t cfg = {
        .p2_server_max_ms    = 25U,
        .session_ctx         = &g_sess,
        .security_ctx        = NULL,
        .service_table       = g_test_service_table,
        .service_table_count = (uint8_t)TEST_SVC_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(&g_srv, &cfg), UDS_STATUS_ERR_INVALID_PARAM,
                  "NULL security_ctx must fail");
}

/**
 * TC-SRV-INIT-005: service_table_count > UDS_SERVER_MAX_HANDLERS
 *                  → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_uds_server_init, test_too_many_handlers)
{
    ctx_init_all();
    static const uds_server_cfg_t cfg = {
        .p2_server_max_ms    = 25U,
        .session_ctx         = &g_sess,
        .security_ctx        = &g_sec,
        .service_table       = g_test_service_table,
        .service_table_count = (uint8_t)(UDS_SERVER_MAX_HANDLERS + 1U),
    };
    zassert_equal(uds_server_init(&g_srv, &cfg), UDS_STATUS_ERR_INVALID_PARAM,
                  "Too many handlers must be rejected");
}

/**
 * TC-SRV-INIT-006: Happy path → OK, initialized = true.
 */
ZTEST(test_uds_server_init, test_happy_path)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "Init must succeed");
    zassert_true(g_srv.initialized, "initialized must be set");
    zassert_equal(g_srv.request_count, 0U, "request_count must start at 0");
    zassert_equal(g_srv.negative_response_count, 0U,
                  "negative_response_count must start at 0");
}

/**
 * TC-SRV-INIT-007: Double init → UDS_STATUS_ERR_ALREADY_INITIALIZED.
 */
ZTEST(test_uds_server_init, test_double_init)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "First init must succeed");
    zassert_equal(srv_init_with_table(), UDS_STATUS_ERR_ALREADY_INITIALIZED,
                  "Double init must fail");
}

/* =========================================================================
 * Test suite: uds_server_process_request
 * ========================================================================= */

ZTEST_SUITE(test_uds_server_process, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SRV-PROC-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_server_process, test_null_ctx)
{
    uds_msg_buf_t req = make_req(0x3EU, 0x00U);
    uds_msg_buf_t resp = {0};
    zassert_equal(uds_server_process_request(NULL, &req, &resp),
                  UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-SRV-PROC-002: NULL request → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_server_process, test_null_req)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "init failed");
    uds_msg_buf_t resp = {0};
    zassert_equal(uds_server_process_request(&g_srv, NULL, &resp),
                  UDS_STATUS_ERR_NULL_PTR, "NULL req must fail");
}

/**
 * TC-SRV-PROC-003: NULL response → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_server_process, test_null_resp)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "init failed");
    uds_msg_buf_t req = make_req(0x3EU, 0x00U);
    zassert_equal(uds_server_process_request(&g_srv, &req, NULL),
                  UDS_STATUS_ERR_NULL_PTR, "NULL resp must fail");
}

/**
 * TC-SRV-PROC-004: Not initialised → UDS_STATUS_ERR_NOT_INITIALIZED.
 */
ZTEST(test_uds_server_process, test_not_initialised)
{
    uds_server_ctx_t srv;
    memset(&srv, 0, sizeof(srv));  /* initialized = false */
    uds_msg_buf_t req = make_req(0x3EU, 0x00U);
    uds_msg_buf_t resp = {0};
    zassert_equal(uds_server_process_request(&srv, &req, &resp),
                  UDS_STATUS_ERR_NOT_INITIALIZED, "Not-init must fail");
}

/**
 * TC-SRV-PROC-005: Zero-length request → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_uds_server_process, test_zero_length_request)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "init failed");
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.length = 0U;
    uds_msg_buf_t resp = {0};
    zassert_equal(uds_server_process_request(&g_srv, &req, &resp),
                  UDS_STATUS_ERR_INVALID_PARAM, "Zero-length request must fail");
}

/**
 * TC-SRV-PROC-006: Unknown SID → negative response with NRC 0x11 (ServiceNotSupported).
 */
ZTEST(test_uds_server_process, test_unknown_sid_returns_nrc)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "init failed");
    uds_msg_buf_t req = make_req(0xAAU, 0x00U);  /* SID 0xAA — not registered */
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_server_process_request(&g_srv, &req, &resp);
    zassert_equal(rc, UDS_STATUS_OK, "process_request must return OK even for NRC");

    /* Negative response: [0x7F, SID, NRC] */
    zassert_equal(resp.length, 3U, "NR response must be 3 bytes");
    zassert_equal(resp.data[0], (uint8_t)UDS_SID_NEGATIVE_RESPONSE,
                  "Byte 0 must be 0x7F");
    zassert_equal(resp.data[1], 0xAAU, "Byte 1 must be echo of SID");
    zassert_equal(resp.data[2], (uint8_t)UDS_NRC_SERVICE_NOT_SUPPORTED,
                  "NRC must be 0x11 (ServiceNotSupported)");
}

/**
 * TC-SRV-PROC-007: Handler returning error → NRC 0x22 (ConditionsNotCorrect).
 */
ZTEST(test_uds_server_process, test_handler_error_returns_nrc_22)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "init failed");
    uds_msg_buf_t req = make_req(0xFFU, 0x00U);  /* SID 0xFF — error stub */
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_server_process_request(&g_srv, &req, &resp);
    zassert_equal(rc, UDS_STATUS_OK, "process_request must return OK");
    zassert_equal(resp.data[0], (uint8_t)UDS_SID_NEGATIVE_RESPONSE,
                  "Must be NR PDU");
    zassert_equal(resp.data[2], (uint8_t)UDS_NRC_CONDITIONS_NOT_CORRECT,
                  "NRC must be 0x22");
}

/**
 * TC-SRV-PROC-008: Successful dispatch → positive response bytes from handler.
 */
ZTEST(test_uds_server_process, test_successful_dispatch)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "init failed");
    uds_msg_buf_t req = make_req(0x3EU, 0x00U);  /* TesterPresent stub */
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_server_process_request(&g_srv, &req, &resp);
    zassert_equal(rc, UDS_STATUS_OK, "Dispatch must succeed");
    /* stub_handler_ok writes 0x7E, 0x00 */
    zassert_equal(resp.data[0], 0x7EU, "Positive SID mismatch");
    zassert_equal(resp.length, 2U, "Response length mismatch");
}

/**
 * TC-SRV-PROC-009: request_count increments on each processed request.
 */
ZTEST(test_uds_server_process, test_request_counter)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "init failed");

    uds_msg_buf_t req = make_req(0x3EU, 0x00U);
    uds_msg_buf_t resp = {0};

    for (int i = 1; i <= 5; i++) {
        uds_server_process_request(&g_srv, &req, &resp);
        uint32_t count, neg;
        uds_server_get_counters(&g_srv, &count, &neg);
        zassert_equal(count, (uint32_t)i, "request_count must be %d", i);
    }
}

/**
 * TC-SRV-PROC-010: negative_response_count increments for each NRC.
 */
ZTEST(test_uds_server_process, test_negative_response_counter)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "init failed");

    uds_msg_buf_t req_bad = make_req(0xBBU, 0x00U);  /* Unknown SID */
    uds_msg_buf_t resp = {0};

    for (int i = 1; i <= 3; i++) {
        uds_server_process_request(&g_srv, &req_bad, &resp);
        uint32_t total, neg;
        uds_server_get_counters(&g_srv, &total, &neg);
        zassert_equal(neg, (uint32_t)i, "negative_response_count must be %d", i);
    }
}

/* =========================================================================
 * Test suite: uds_server_build_negative_response
 * ========================================================================= */

ZTEST_SUITE(test_uds_server_build_nrc, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SRV-NRC-001: NULL resp → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_server_build_nrc, test_null_resp)
{
    zassert_equal(uds_server_build_negative_response(0x22U, UDS_NRC_GENERAL_REJECT, NULL),
                  UDS_STATUS_ERR_NULL_PTR, "NULL resp must fail");
}

/**
 * TC-SRV-NRC-002: Verify complete NRC PDU encoding for NRC 0x10 (GeneralReject).
 *
 * Expected: [0x7F, SID, NRC], length = 3.
 */
ZTEST(test_uds_server_build_nrc, test_nrc_general_reject)
{
    uds_msg_buf_t resp = {0};
    zassert_equal(uds_server_build_negative_response(0x10U, UDS_NRC_GENERAL_REJECT, &resp),
                  UDS_STATUS_OK, "build_negative_response must succeed");
    zassert_equal(resp.length, 3U, "NR length must be 3");
    zassert_equal(resp.data[0], 0x7FU, "Byte 0 must be 0x7F");
    zassert_equal(resp.data[1], 0x10U, "Byte 1 must be SID 0x10");
    zassert_equal(resp.data[2], (uint8_t)UDS_NRC_GENERAL_REJECT,
                  "Byte 2 must be NRC 0x10");
}

/**
 * TC-SRV-NRC-003: Verify NRC 0x35 (InvalidKey) for SID 0x27.
 */
ZTEST(test_uds_server_build_nrc, test_nrc_invalid_key)
{
    uds_msg_buf_t resp = {0};
    uds_server_build_negative_response(0x27U, UDS_NRC_INVALID_KEY, &resp);
    zassert_equal(resp.data[1], 0x27U, "SID must be 0x27");
    zassert_equal(resp.data[2], (uint8_t)UDS_NRC_INVALID_KEY, "NRC must be 0x35");
}

/**
 * TC-SRV-NRC-004: Verify NRC 0x36 (ExceededNumberOfAttempts) for SID 0x27.
 */
ZTEST(test_uds_server_build_nrc, test_nrc_exceeded_attempts)
{
    uds_msg_buf_t resp = {0};
    uds_server_build_negative_response(0x27U, UDS_NRC_EXCEEDED_NUM_OF_ATTEMPTS, &resp);
    zassert_equal(resp.data[2], (uint8_t)UDS_NRC_EXCEEDED_NUM_OF_ATTEMPTS,
                  "NRC must be 0x36");
}

/**
 * TC-SRV-NRC-005: Verify NRC 0x13 (IncorrectMsgLenOrFormat) for SID 0x22.
 */
ZTEST(test_uds_server_build_nrc, test_nrc_incorrect_msg_len)
{
    uds_msg_buf_t resp = {0};
    uds_server_build_negative_response(0x22U, UDS_NRC_INCORRECT_MSG_LEN_OR_FORMAT, &resp);
    zassert_equal(resp.data[2], (uint8_t)UDS_NRC_INCORRECT_MSG_LEN_OR_FORMAT,
                  "NRC must be 0x13");
}

/* =========================================================================
 * Test suite: uds_server_get_counters
 * ========================================================================= */

ZTEST_SUITE(test_uds_server_get_counters, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SRV-CTR-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_server_get_counters, test_null_ctx)
{
    uint32_t a, b;
    zassert_equal(uds_server_get_counters(NULL, &a, &b),
                  UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-SRV-CTR-002: NULL out_request_count → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_server_get_counters, test_null_count)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "init failed");
    uint32_t neg;
    zassert_equal(uds_server_get_counters(&g_srv, NULL, &neg),
                  UDS_STATUS_ERR_NULL_PTR, "NULL count must fail");
}

/**
 * TC-SRV-CTR-003: Fresh server has both counters at 0.
 */
ZTEST(test_uds_server_get_counters, test_fresh_counters_zero)
{
    ctx_init_all();
    zassert_equal(srv_init_with_table(), UDS_STATUS_OK, "init failed");
    uint32_t req_count, neg_count;
    zassert_equal(uds_server_get_counters(&g_srv, &req_count, &neg_count),
                  UDS_STATUS_OK, "get_counters must succeed");
    zassert_equal(req_count, 0U, "request_count must start at 0");
    zassert_equal(neg_count, 0U, "negative_response_count must start at 0");
}

/* =========================================================================
 * Test suite: uds_server_tick_1ms
 * ========================================================================= */

ZTEST_SUITE(test_uds_server_tick, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SRV-TICK-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_server_tick, test_null_ctx)
{
    zassert_equal(uds_server_tick_1ms(NULL), UDS_STATUS_ERR_NULL_PTR,
                  "NULL ctx must fail");
}

/**
 * TC-SRV-TICK-002: Not initialized → UDS_STATUS_ERR_NOT_INITIALIZED.
 */
ZTEST(test_uds_server_tick, test_not_initialized)
{
    uds_server_ctx_t srv;
    memset(&srv, 0, sizeof(srv));
    zassert_equal(uds_server_tick_1ms(&srv), UDS_STATUS_ERR_NOT_INITIALIZED,
                  "Tick before init must fail");
}

/**
 * TC-SRV-TICK-003: S3 timeout propagated through server tick.
 *
 * Uses 5-ms S3 timeout so the test runs quickly.
 */
ZTEST(test_uds_server_tick, test_s3_timeout_propagated)
{
    uds_session_ctx_t sess;
    uds_security_ctx_t sec;
    uds_server_ctx_t srv;
    memset(&sess, 0, sizeof(sess));
    memset(&sec,  0, sizeof(sec));
    memset(&srv,  0, sizeof(srv));

    /* 5-ms S3 timeout */
    uds_session_init(&sess, 5U);
    static const uds_security_cfg_t sec_cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,
        .key_validate_cb  = stub_key_validate,
        .seed_generate_cb = stub_seed_gen,
    };
    uds_security_init(&sec, &sec_cfg);

    uds_server_cfg_t cfg = {
        .p2_server_max_ms      = 25U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &sess,
        .security_ctx          = &sec,
        .service_table         = g_test_service_table,
        .service_table_count   = (uint8_t)TEST_SVC_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(&srv, &cfg), UDS_STATUS_OK, "init failed");

    /* Transition to EXTENDED to arm S3 timer */
    uds_session_transition(&sess, UDS_SESSION_EXTENDED);

    /* Tick until S3 fires */
    uds_status_t tick_rc = UDS_STATUS_OK;
    for (int i = 0; i <= 7; i++) {
        tick_rc = uds_server_tick_1ms(&srv);
        if (tick_rc != UDS_STATUS_OK) break;
    }
    zassert_equal(tick_rc, UDS_STATUS_ERR_SESSION_TIMEOUT,
                  "S3 timeout must propagate through server tick");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */


/* =========================================================================
 * Test suite: uds_server_timing (REQ-TIMING-001, REQ-TIMING-002)
 *
 * Verifies that:
 *   - p2_server_max_ms and p2_star_server_max_ms are stored and accessible
 *     from the server config after init (REQ-TIMING-001)
 *   - uds_server_build_negative_response correctly produces NRC 0x78
 *     (requestCorrectlyReceivedResponsePending) for the response-pending
 *     mechanism (REQ-TIMING-002)
 *   - Zero p2_server_max_ms is accepted (caller responsible for timing)
 *   - p2_star_server_max_ms >= p2_server_max_ms is the expected config
 *
 * Design note: ISO 14229-1 S7.2 assigns P2Server_max enforcement to the
 * integration layer, not the server dispatcher. uds_server.c stores the
 * configured values in cfg for the integration layer to consume. These
 * tests verify storage and NRC 0x78 mechanism correctness.
 *
 * TRACEABILITY: REQ-TIMING-001, REQ-TIMING-002
 * ========================================================================= */

ZTEST_SUITE(test_uds_server_timing, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SRV-TIMING-001: p2_server_max_ms stored correctly after init.
 *
 * REQ-TIMING-001: p2_server_max_ms shall be configurable at init time
 * and readable from the server context by the integration layer.
 */
ZTEST(test_uds_server_timing, test_p2_server_max_stored)
{
    ctx_init_all();

    static const uds_server_cfg_t cfg = {
        .p2_server_max_ms      = 50U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_test_service_table,
        .service_table_count   = (uint8_t)TEST_SVC_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(&g_srv, &cfg), UDS_STATUS_OK,
                  "Init must succeed");
    zassert_equal(g_srv.cfg.p2_server_max_ms, 50U,
                  "p2_server_max_ms must be stored correctly");
}

/**
 * TC-SRV-TIMING-002: p2_star_server_max_ms stored correctly after init.
 *
 * REQ-TIMING-001: p2_star_server_max_ms shall be configurable at init
 * time and readable from the server context by the integration layer.
 */
ZTEST(test_uds_server_timing, test_p2_star_server_max_stored)
{
    ctx_init_all();

    static const uds_server_cfg_t cfg = {
        .p2_server_max_ms      = 25U,
        .p2_star_server_max_ms = 2000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_test_service_table,
        .service_table_count   = (uint8_t)TEST_SVC_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(&g_srv, &cfg), UDS_STATUS_OK,
                  "Init must succeed");
    zassert_equal(g_srv.cfg.p2_star_server_max_ms, 2000U,
                  "p2_star_server_max_ms must be stored correctly");
}

/**
 * TC-SRV-TIMING-003: Both timing values survive init with ISO 14229-1
 * Table 4 default values. Verify p2_star >= p2 invariant holds.
 *
 * REQ-TIMING-001: ISO defaults are p2=50ms, p2_star=5000ms.
 */
ZTEST(test_uds_server_timing, test_p2_iso_default_values_stored)
{
    ctx_init_all();

    static const uds_server_cfg_t cfg = {
        .p2_server_max_ms      = 50U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_test_service_table,
        .service_table_count   = (uint8_t)TEST_SVC_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(&g_srv, &cfg), UDS_STATUS_OK,
                  "Init with ISO defaults must succeed");
    zassert_equal(g_srv.cfg.p2_server_max_ms, 50U,
                  "p2_server_max_ms ISO default must be stored");
    zassert_equal(g_srv.cfg.p2_star_server_max_ms, 5000U,
                  "p2_star_server_max_ms ISO default must be stored");
    zassert_true(g_srv.cfg.p2_star_server_max_ms >= g_srv.cfg.p2_server_max_ms,
                 "p2_star_server_max_ms must be >= p2_server_max_ms");
}

/**
 * TC-SRV-TIMING-004: build_negative_response produces correct NRC 0x78 PDU.
 *
 * REQ-TIMING-002: When a service cannot respond within P2Server_max, the
 * integration layer transmits NRC 0x78
 * (requestCorrectlyReceivedResponsePending). Verify the PDU is correctly
 * encoded: 0x7F <SID> 0x78.
 */
ZTEST(test_uds_server_timing, test_nrc_0x78_response_pending_encoding)
{
    uds_msg_buf_t resp;
    memset(&resp, 0, sizeof(resp));

    zassert_equal(
        uds_server_build_negative_response(
            0x22U,
            UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING,
            &resp),
        UDS_STATUS_OK,
        "build_negative_response for NRC 0x78 must succeed");
    zassert_equal(resp.length, 3U,   "NRC 0x78 PDU must be 3 bytes");
    zassert_equal(resp.data[0], 0x7FU, "Byte 0: negative response SID");
    zassert_equal(resp.data[1], 0x22U, "Byte 1: request SID (0x22)");
    zassert_equal(resp.data[2], 0x78U, "Byte 2: NRC 0x78");
}

/**
 * TC-SRV-TIMING-005: NRC 0x78 encoding for WriteDataByIdentifier (0x2E).
 *
 * REQ-TIMING-002: Write services may need response-pending when DID
 * callbacks perform slow NVM writes.
 */
ZTEST(test_uds_server_timing, test_nrc_0x78_for_write_service)
{
    uds_msg_buf_t resp;
    memset(&resp, 0, sizeof(resp));

    zassert_equal(
        uds_server_build_negative_response(
            0x2EU,
            UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING,
            &resp),
        UDS_STATUS_OK,
        "build_negative_response for 0x2E NRC 0x78 must succeed");
    zassert_equal(resp.data[0], 0x7FU, "Must be NRC frame");
    zassert_equal(resp.data[1], 0x2EU, "SID must be 0x2E");
    zassert_equal(resp.data[2], 0x78U, "NRC must be 0x78");
}

/**
 * TC-SRV-TIMING-006: NRC 0x78 encoding for RequestDownload (0x34).
 *
 * REQ-TIMING-002: DFU flash erase operations commonly exceed P2Server_max.
 * Verify NRC 0x78 is correctly encoded for SID 0x34.
 */
ZTEST(test_uds_server_timing, test_nrc_0x78_for_download_service)
{
    uds_msg_buf_t resp;
    memset(&resp, 0, sizeof(resp));

    zassert_equal(
        uds_server_build_negative_response(
            0x34U,
            UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING,
            &resp),
        UDS_STATUS_OK,
        "build_negative_response for 0x34 NRC 0x78 must succeed");
    zassert_equal(resp.data[0], 0x7FU, "Must be NRC frame");
    zassert_equal(resp.data[1], 0x34U, "SID must be 0x34");
    zassert_equal(resp.data[2], 0x78U, "NRC must be 0x78");
}

/**
 * TC-SRV-TIMING-007: p2_server_max_ms = 0 is accepted.
 *
 * REQ-TIMING-001: The server does not enforce P2Server_max internally.
 * Zero is a valid value for CI/simulator builds where timing is not
 * enforced by the integration layer.
 */
ZTEST(test_uds_server_timing, test_p2_zero_accepted)
{
    ctx_init_all();

    static const uds_server_cfg_t cfg = {
        .p2_server_max_ms      = 0U,
        .p2_star_server_max_ms = 0U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_test_service_table,
        .service_table_count   = (uint8_t)TEST_SVC_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(&g_srv, &cfg), UDS_STATUS_OK,
                  "Zero timing values must be accepted");
    zassert_equal(g_srv.cfg.p2_server_max_ms, 0U,
                  "Zero p2_server_max_ms must be stored");
}

/**
 * TC-SRV-TIMING-008: Normal dispatch succeeds when timing config is set.
 *
 * REQ-TIMING-001: Timing fields are informational for the integration
 * layer and must not affect normal service dispatch behaviour.
 */
ZTEST(test_uds_server_timing, test_timing_config_does_not_affect_dispatch)
{
    ctx_init_all();

    static const uds_server_cfg_t cfg = {
        .p2_server_max_ms      = 50U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_test_service_table,
        .service_table_count   = (uint8_t)TEST_SVC_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(&g_srv, &cfg), UDS_STATUS_OK,
                  "Init must succeed");

    uds_msg_buf_t req  = make_req(0x3EU, 0x00U);
    uds_msg_buf_t resp = {0};

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp),
                  UDS_STATUS_OK,
                  "Dispatch must succeed with timing config set");
    zassert_equal(resp.data[0], 0x7EU,
                  "TesterPresent stub response byte must be 0x7E");
}

extern void test_uds_server_init__test_null_ctx(void);
extern void test_uds_server_init__test_null_cfg(void);
extern void test_uds_server_init__test_null_session_ctx(void);
extern void test_uds_server_init__test_null_security_ctx(void);
extern void test_uds_server_init__test_too_many_handlers(void);
extern void test_uds_server_init__test_happy_path(void);
extern void test_uds_server_init__test_double_init(void);
extern void test_uds_server_process__test_null_ctx(void);
extern void test_uds_server_process__test_null_req(void);
extern void test_uds_server_process__test_null_resp(void);
extern void test_uds_server_process__test_not_initialised(void);
extern void test_uds_server_process__test_zero_length_request(void);
extern void test_uds_server_process__test_unknown_sid_returns_nrc(void);
extern void test_uds_server_process__test_handler_error_returns_nrc_22(void);
extern void test_uds_server_process__test_successful_dispatch(void);
extern void test_uds_server_process__test_request_counter(void);
extern void test_uds_server_process__test_negative_response_counter(void);
extern void test_uds_server_build_nrc__test_null_resp(void);
extern void test_uds_server_build_nrc__test_nrc_general_reject(void);
extern void test_uds_server_build_nrc__test_nrc_invalid_key(void);
extern void test_uds_server_build_nrc__test_nrc_exceeded_attempts(void);
extern void test_uds_server_build_nrc__test_nrc_incorrect_msg_len(void);
extern void test_uds_server_get_counters__test_null_ctx(void);
extern void test_uds_server_get_counters__test_null_count(void);
extern void test_uds_server_get_counters__test_fresh_counters_zero(void);
extern void test_uds_server_tick__test_null_ctx(void);
extern void test_uds_server_tick__test_not_initialized(void);
extern void test_uds_server_tick__test_s3_timeout_propagated(void);

extern void test_uds_server_timing__test_p2_server_max_stored(void);
extern void test_uds_server_timing__test_p2_star_server_max_stored(void);
extern void test_uds_server_timing__test_p2_iso_default_values_stored(void);
extern void test_uds_server_timing__test_nrc_0x78_response_pending_encoding(void);
extern void test_uds_server_timing__test_nrc_0x78_for_write_service(void);
extern void test_uds_server_timing__test_nrc_0x78_for_download_service(void);
extern void test_uds_server_timing__test_p2_zero_accepted(void);
extern void test_uds_server_timing__test_timing_config_does_not_affect_dispatch(void);

void run_all_tests(void)
{
    RUN_TEST(test_uds_server_init__test_null_ctx);
    RUN_TEST(test_uds_server_init__test_null_cfg);
    RUN_TEST(test_uds_server_init__test_null_session_ctx);
    RUN_TEST(test_uds_server_init__test_null_security_ctx);
    RUN_TEST(test_uds_server_init__test_too_many_handlers);
    RUN_TEST(test_uds_server_init__test_happy_path);
    RUN_TEST(test_uds_server_init__test_double_init);
    RUN_TEST(test_uds_server_process__test_null_ctx);
    RUN_TEST(test_uds_server_process__test_null_req);
    RUN_TEST(test_uds_server_process__test_null_resp);
    RUN_TEST(test_uds_server_process__test_not_initialised);
    RUN_TEST(test_uds_server_process__test_zero_length_request);
    RUN_TEST(test_uds_server_process__test_unknown_sid_returns_nrc);
    RUN_TEST(test_uds_server_process__test_handler_error_returns_nrc_22);
    RUN_TEST(test_uds_server_process__test_successful_dispatch);
    RUN_TEST(test_uds_server_process__test_request_counter);
    RUN_TEST(test_uds_server_process__test_negative_response_counter);
    RUN_TEST(test_uds_server_build_nrc__test_null_resp);
    RUN_TEST(test_uds_server_build_nrc__test_nrc_general_reject);
    RUN_TEST(test_uds_server_build_nrc__test_nrc_invalid_key);
    RUN_TEST(test_uds_server_build_nrc__test_nrc_exceeded_attempts);
    RUN_TEST(test_uds_server_build_nrc__test_nrc_incorrect_msg_len);
    RUN_TEST(test_uds_server_get_counters__test_null_ctx);
    RUN_TEST(test_uds_server_get_counters__test_null_count);
    RUN_TEST(test_uds_server_get_counters__test_fresh_counters_zero);
    RUN_TEST(test_uds_server_tick__test_null_ctx);
    RUN_TEST(test_uds_server_tick__test_not_initialized);
    RUN_TEST(test_uds_server_tick__test_s3_timeout_propagated);
    RUN_TEST(test_uds_server_timing__test_p2_server_max_stored);
    RUN_TEST(test_uds_server_timing__test_p2_star_server_max_stored);
    RUN_TEST(test_uds_server_timing__test_p2_iso_default_values_stored);
    RUN_TEST(test_uds_server_timing__test_nrc_0x78_response_pending_encoding);
    RUN_TEST(test_uds_server_timing__test_nrc_0x78_for_write_service);
    RUN_TEST(test_uds_server_timing__test_nrc_0x78_for_download_service);
    RUN_TEST(test_uds_server_timing__test_p2_zero_accepted);
    RUN_TEST(test_uds_server_timing__test_timing_config_does_not_affect_dispatch);

}
