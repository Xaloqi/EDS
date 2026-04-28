// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_service_0x14.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x14.c
 *                    SID 0x14 — ClearDiagnosticInformation
 *
 * PURPOSE:
 *   Verify that SID 0x14 correctly clears DTC status bytes, enforces the
 *   request format, handles empty databases, and persists via dtc_mirror.
 *
 * TEST CASES:
 *   TC-0x14-001  Group 0xFFFFFF with registered DTCs → positive response 0x54
 *   TC-0x14-002  Positive response is exactly 1 byte
 *   TC-0x14-003  All DTC status bytes are zero after successful clear
 *   TC-0x14-004  Group 0xFFFFFE (powertrain) also accepted → clear all
 *   TC-0x14-005  Group 0x000001 (non-zero, non-standard) accepted → clear all
 *   TC-0x14-006  Group 0x000000 → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31)
 *   TC-0x14-007  Request length 3 (missing groupLB) → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x14-008  Request length 5 (too long) → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x14-009  Request length 1 (SID only) → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x14-010  Empty DTC database → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31)
 *   TC-0x14-011  Multiple DTCs all cleared
 *   TC-0x14-012  NULL req  → ERR_NULL_PTR
 *   TC-0x14-013  NULL resp → ERR_NULL_PTR
 *   TC-0x14-014  DTC mirror persists clear (cleared status in NVM after call)
 *   TC-0x14-015  Non-zero status survives across a second clear call (idempotent)
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
#include "dtc_database.h"
#include "dtc_mirror.h"
#include "nvm_store.h"

/* =========================================================================
 * Test-reset helpers (defined in production source files)
 * ========================================================================= */
extern void dtc_database_test_reset(void);
extern void dtc_mirror_test_reset(void);
extern void nvm_mock_reset(void);

/* =========================================================================
 * Minimal security stubs
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
 * Shared context objects and well-known DTC codes
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

#define DTC_P0001  (0xC00100UL)
#define DTC_P0002  (0xC00200UL)
#define DTC_P0003  (0xC00300UL)

static void setup(void)
{
    /* Reset all module state */
    nvm_mock_reset();
    nvm_store_init(NULL);
    dtc_database_test_reset();
    dtc_database_init();
    dtc_mirror_test_reset();
    dtc_mirror_init();

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

/** Register three DTCs with non-zero status bytes. */
static void register_three_faulted_dtcs(void)
{
    dtc_database_register(DTC_P0001, 0x00U, "P0001");
    dtc_database_register(DTC_P0002, 0x00U, "P0002");
    dtc_database_register(DTC_P0003, 0x00U, "P0003");
    dtc_database_set_status(DTC_P0001, 0x09U); /* TEST_FAILED | CONFIRMED */
    dtc_database_set_status(DTC_P0002, 0x04U); /* PENDING */
    dtc_database_set_status(DTC_P0003, 0x08U); /* CONFIRMED */
}

/** Build a ClearDiagnosticInformation request with a 3-byte group. */
static uds_msg_buf_t make_clear_req(uint32_t group_of_dtc)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x14U;
    r.data[1] = (uint8_t)((group_of_dtc >> 16U) & 0xFFU);
    r.data[2] = (uint8_t)((group_of_dtc >>  8U) & 0xFFU);
    r.data[3] = (uint8_t)( group_of_dtc         & 0xFFU);
    r.length  = 4U;
    return r;
}

/* =========================================================================
 * Test cases
 * ========================================================================= */

ZTEST_SUITE(test_service_0x14, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-0x14-001: Standard clear-all with group 0xFFFFFF.
 * Must return UDS_STATUS_OK with positive response SID 0x54.
 */
ZTEST(test_service_0x14, tc001_clear_all_group_ff)
{
    setup();
    register_three_faulted_dtcs();

    uds_msg_buf_t req  = make_clear_req(0xFFFFFFUL);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "handler must return OK");
    zassert_equal(resp.data[0], 0x54U, "response SID must be 0x54");
}

/**
 * TC-0x14-002: Positive response is exactly 1 byte (only the response SID).
 */
ZTEST(test_service_0x14, tc002_response_length_is_one)
{
    setup();
    register_three_faulted_dtcs();

    uds_msg_buf_t req  = make_clear_req(0xFFFFFFUL);
    uds_msg_buf_t resp = {0};

    uds_service_0x14_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 1U,
        "ClearDiagnosticInformation response must be exactly 1 byte");
}

/**
 * TC-0x14-003: All registered DTC status bytes are 0x00 after successful clear.
 */
ZTEST(test_service_0x14, tc003_dtc_status_bytes_zeroed)
{
    setup();
    register_three_faulted_dtcs();

    /* Verify precondition: at least one DTC is faulted */
    dtc_entry_t *e = dtc_database_find(DTC_P0001);
    zassert_not_null(e, "DTC_P0001 must be registered");
    zassert_equal(e->status_byte, 0x09U, "precondition: DTC_P0001 must be faulted");

    uds_msg_buf_t req  = make_clear_req(0xFFFFFFUL);
    uds_msg_buf_t resp = {0};
    uds_service_0x14_handler(&g_srv, &req, &resp);

    zassert_equal(dtc_database_find(DTC_P0001)->status_byte, 0x00U,
        "DTC_P0001 status must be 0x00 after clear");
    zassert_equal(dtc_database_find(DTC_P0002)->status_byte, 0x00U,
        "DTC_P0002 status must be 0x00 after clear");
    zassert_equal(dtc_database_find(DTC_P0003)->status_byte, 0x00U,
        "DTC_P0003 status must be 0x00 after clear");
}

/**
 * TC-0x14-004: Group 0xFFFFFE (SAE powertrain group) is accepted → clear all.
 */
ZTEST(test_service_0x14, tc004_group_fffe_accepted)
{
    setup();
    register_three_faulted_dtcs();

    uds_msg_buf_t req  = make_clear_req(0xFFFFFEUL);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "group 0xFFFFFE must be accepted");
    zassert_equal(resp.data[0], 0x54U, "positive response SID must be 0x54");
}

/**
 * TC-0x14-005: Any non-zero group is accepted — test with 0x000001.
 */
ZTEST(test_service_0x14, tc005_nonzero_group_accepted)
{
    setup();
    register_three_faulted_dtcs();

    uds_msg_buf_t req  = make_clear_req(0x000001UL);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "any non-zero group must be accepted");
}

/**
 * TC-0x14-006: Group 0x000000 is not defined — must return ERR_REQUEST_OUT_OF_RANGE.
 */
ZTEST(test_service_0x14, tc006_group_zero_rejected)
{
    setup();
    register_three_faulted_dtcs();

    uds_msg_buf_t req  = make_clear_req(0x000000UL);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
        "group 0x000000 must return NRC 0x31");
}

/**
 * TC-0x14-007: Request length 3 (missing group LSB) → invalid format.
 */
ZTEST(test_service_0x14, tc007_short_request_rejected)
{
    setup();
    register_three_faulted_dtcs();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x14U;
    req.data[1] = 0xFFU;
    req.data[2] = 0xFFU;
    req.length  = 3U; /* Missing group LSB */

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
        "3-byte request must be rejected");
}

/**
 * TC-0x14-008: Request length 5 (extra byte) → invalid format.
 */
ZTEST(test_service_0x14, tc008_long_request_rejected)
{
    setup();
    register_three_faulted_dtcs();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x14U;
    req.data[1] = 0xFFU;
    req.data[2] = 0xFFU;
    req.data[3] = 0xFFU;
    req.data[4] = 0x00U;
    req.length  = 5U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
        "5-byte request must be rejected");
}

/**
 * TC-0x14-009: Request length 1 (SID only) → invalid format.
 */
ZTEST(test_service_0x14, tc009_length_one_rejected)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x14U;
    req.length  = 1U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
        "length-1 request must be rejected");
}

/**
 * TC-0x14-010: Empty DTC database → ERR_REQUEST_OUT_OF_RANGE.
 * No DTCs registered → nothing to clear → NRC 0x31.
 */
ZTEST(test_service_0x14, tc010_empty_database_rejected)
{
    setup();
    /* Do NOT register any DTCs */

    uds_msg_buf_t req  = make_clear_req(0xFFFFFFUL);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
        "empty database must return NRC 0x31");
}

/**
 * TC-0x14-011: Multiple DTCs, all cleared after one request.
 * Specifically checks each individual entry rather than relying on TC-003.
 */
ZTEST(test_service_0x14, tc011_multiple_dtcs_all_cleared)
{
    setup();
    register_three_faulted_dtcs();

    uint16_t count_before;
    dtc_database_count_by_status(0xFFU, &count_before);
    zassert_equal(count_before, 3U, "precondition: 3 faulted DTCs");

    uds_msg_buf_t req  = make_clear_req(0xFFFFFFUL);
    uds_msg_buf_t resp = {0};
    uds_service_0x14_handler(&g_srv, &req, &resp);

    uint16_t count_after;
    dtc_database_count_by_status(0xFFU, &count_after);
    zassert_equal(count_after, 0U, "all DTCs must be cleared");
}

/**
 * TC-0x14-012: NULL req pointer → ERR_NULL_PTR.
 */
ZTEST(test_service_0x14, tc012_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x14_handler(&g_srv, NULL, &resp);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
        "NULL req must return ERR_NULL_PTR");
}

/**
 * TC-0x14-013: NULL resp pointer → ERR_NULL_PTR.
 */
ZTEST(test_service_0x14, tc013_null_resp)
{
    setup();
    register_three_faulted_dtcs();
    uds_msg_buf_t req = make_clear_req(0xFFFFFFUL);
    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
        "NULL resp must return ERR_NULL_PTR");
}

/**
 * TC-0x14-014: After a successful clear, dtc_mirror_load() on a fresh
 * database confirms all statuses are zero (NVM was persisted).
 */
ZTEST(test_service_0x14, tc014_mirror_persists_cleared_state)
{
    setup();
    register_three_faulted_dtcs();

    /* First flush current (faulted) state so mirror has something to overwrite */
    dtc_mirror_flush_all();

    uds_msg_buf_t req  = make_clear_req(0xFFFFFFUL);
    uds_msg_buf_t resp = {0};
    uds_service_0x14_handler(&g_srv, &req, &resp);

    /* Simulate power-cycle: keep NVM data, re-init everything */
    extern void nvm_mock_deinit(void);
    nvm_mock_deinit();
    nvm_store_init(NULL);
    dtc_database_test_reset();
    dtc_database_init();
    dtc_mirror_test_reset();
    dtc_mirror_init();

    /* Re-register and load mirror */
    dtc_database_register(DTC_P0001, 0x00U, "P0001");
    dtc_database_register(DTC_P0002, 0x00U, "P0002");
    dtc_database_register(DTC_P0003, 0x00U, "P0003");
    /* Set non-zero pre-load so we can confirm load actually ran */
    dtc_database_set_status(DTC_P0001, 0xFFU);
    dtc_mirror_load();

    zassert_equal(dtc_database_find(DTC_P0001)->status_byte, 0x00U,
        "DTC mirror must persist cleared state (status must be 0x00 after reload)");
}

/**
 * TC-0x14-015: Calling clear twice is idempotent — second call still returns OK
 * and status bytes remain zero.
 */
ZTEST(test_service_0x14, tc015_clear_twice_idempotent)
{
    setup();
    register_three_faulted_dtcs();

    uds_msg_buf_t req  = make_clear_req(0xFFFFFFUL);
    uds_msg_buf_t resp = {0};

    uds_status_t rc1 = uds_service_0x14_handler(&g_srv, &req, &resp);
    zassert_equal(rc1, UDS_STATUS_OK, "first clear must succeed");

    /* Second clear — database already at 0x00, but count is still > 0 */
    memset(&resp, 0, sizeof(resp));
    uds_status_t rc2 = uds_service_0x14_handler(&g_srv, &req, &resp);
    zassert_equal(rc2, UDS_STATUS_OK, "second clear must also succeed");
    zassert_equal(dtc_database_find(DTC_P0001)->status_byte, 0x00U,
        "status must remain zero after second clear");
}

/* =========================================================================
 * run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x14__tc001_clear_all_group_ff(void);
extern void test_service_0x14__tc002_response_length_is_one(void);
extern void test_service_0x14__tc003_dtc_status_bytes_zeroed(void);
extern void test_service_0x14__tc004_group_fffe_accepted(void);
extern void test_service_0x14__tc005_nonzero_group_accepted(void);
extern void test_service_0x14__tc006_group_zero_rejected(void);
extern void test_service_0x14__tc007_short_request_rejected(void);
extern void test_service_0x14__tc008_long_request_rejected(void);
extern void test_service_0x14__tc009_length_one_rejected(void);
extern void test_service_0x14__tc010_empty_database_rejected(void);
extern void test_service_0x14__tc011_multiple_dtcs_all_cleared(void);
extern void test_service_0x14__tc012_null_req(void);
extern void test_service_0x14__tc013_null_resp(void);
extern void test_service_0x14__tc014_mirror_persists_cleared_state(void);
extern void test_service_0x14__tc015_clear_twice_idempotent(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x14__tc001_clear_all_group_ff);
    RUN_TEST(test_service_0x14__tc002_response_length_is_one);
    RUN_TEST(test_service_0x14__tc003_dtc_status_bytes_zeroed);
    RUN_TEST(test_service_0x14__tc004_group_fffe_accepted);
    RUN_TEST(test_service_0x14__tc005_nonzero_group_accepted);
    RUN_TEST(test_service_0x14__tc006_group_zero_rejected);
    RUN_TEST(test_service_0x14__tc007_short_request_rejected);
    RUN_TEST(test_service_0x14__tc008_long_request_rejected);
    RUN_TEST(test_service_0x14__tc009_length_one_rejected);
    RUN_TEST(test_service_0x14__tc010_empty_database_rejected);
    RUN_TEST(test_service_0x14__tc011_multiple_dtcs_all_cleared);
    RUN_TEST(test_service_0x14__tc012_null_req);
    RUN_TEST(test_service_0x14__tc013_null_resp);
    RUN_TEST(test_service_0x14__tc014_mirror_persists_cleared_state);
    RUN_TEST(test_service_0x14__tc015_clear_twice_idempotent);
}
