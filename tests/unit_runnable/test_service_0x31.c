// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_service_0x31.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x31.c
 *
 * Coverage:
 *   Request too short              → ERR_INVALID_PARAM
 *   Sub-fn 0x00 / 0x04 invalid    → ERR_SUBFUNCTION_NOT_SUP
 *   RID not in database            → ERR_ROUTINE_NOT_FOUND
 *   Session too low                → ERR_ROUTINE_NOT_SUPPORTED_IN_SESSION
 *   startRoutine — empty result    → positive response [0x71,0x01,RID_hi,RID_lo]
 *   startRoutine — 4-byte result   → response extended by 4 bytes
 *   Callback failure               → ERR_ROUTINE_FAILED
 *   stopRoutine on start-only RID  → ERR_SUBFUNCTION_NOT_SUP
 *   stopRoutine on full RID        → positive response [0x71,0x02,...]
 *   requestResults on start-only   → ERR_SUBFUNCTION_NOT_SUP
 *   requestResults with data       → positive response + payload
 *   Results callback cond-not-met  → ERR_CONDITIONS_NOT_MET
 *   Option record forwarded        → no crash
 *   NULL ctx                       → ERR_NULL_PTR
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h for host compilation)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_safety.h"
#include "routine_database.h"

/* ==========================================================================
 * Test state
 * ========================================================================== */

static uds_session_ctx_t  s_sess;
static uds_security_ctx_t s_sec;
static uds_server_ctx_t   s_srv;
static uds_msg_buf_t      s_req;
static uds_msg_buf_t      s_resp;

static bool    s_start_fail    = false;
static bool    s_stop_fail     = false;
static bool    s_results_fail  = false;
static uint8_t s_result_data[4] = { 0xAAU, 0xBBU, 0xCCU, 0xDDU };
static uint8_t s_result_len     = 0U;

/* ==========================================================================
 * Stub callbacks
 * ========================================================================== */

static uds_status_t cb_start(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *res, uint8_t res_max, uint8_t *res_out)
{
    (void)opt; (void)opt_len;
    if (s_start_fail) { return UDS_STATUS_ERR_ROUTINE_FAILED; }
    if (s_result_len > 0U && res_max >= s_result_len) {
        memcpy(res, s_result_data, s_result_len);
    }
    *res_out = s_result_len;
    return UDS_STATUS_OK;
}

static uds_status_t cb_stop(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *res, uint8_t res_max, uint8_t *res_out)
{
    (void)opt; (void)opt_len; (void)res; (void)res_max;
    if (s_stop_fail) { return UDS_STATUS_ERR_ROUTINE_FAILED; }
    *res_out = 0U;
    return UDS_STATUS_OK;
}

static uds_status_t cb_results(uint8_t *res, uint8_t res_max, uint8_t *res_out)
{
    if (s_results_fail) { return UDS_STATUS_ERR_CONDITIONS_NOT_MET; }
    if (s_result_len > 0U && res_max >= s_result_len) {
        memcpy(res, s_result_data, s_result_len);
    }
    *res_out = s_result_len;
    return UDS_STATUS_OK;
}

/* ==========================================================================
 * Test RIDs
 * ========================================================================== */
#define RID_FULL    (0xFF00U)   /* start + stop + results */
#define RID_SIMPLE  (0xFF01U)   /* start only              */

static void register_test_routines(void)
{
    routine_entry_t e;
    memset(&e, 0, sizeof(e));

    e.rid           = RID_FULL;
    e.support_flags = (uint8_t)(ROUTINE_SUPPORT_START |
                                ROUTINE_SUPPORT_STOP  |
                                ROUTINE_SUPPORT_RESULTS);
    e.min_session   = (uint8_t)UDS_SESSION_EXTENDED;
    e.security_level = 0U;
    e.start_cb      = cb_start;
    e.stop_cb       = cb_stop;
    e.results_cb    = cb_results;
    e.description   = "full";
    (void)routine_database_register(&e);

    memset(&e, 0, sizeof(e));
    e.rid           = RID_SIMPLE;
    e.support_flags = (uint8_t)ROUTINE_SUPPORT_START;
    e.min_session   = (uint8_t)UDS_SESSION_EXTENDED;
    e.security_level = 0U;
    e.start_cb      = cb_start;
    e.description   = "simple";
    (void)routine_database_register(&e);
}

/* ==========================================================================
 * setUp / tearDown
 * ========================================================================== */

void setUp(void)
{
    memset(&s_sess, 0, sizeof(s_sess));
    memset(&s_sec,  0, sizeof(s_sec));
    memset(&s_srv,  0, sizeof(s_srv));
    memset(&s_req,  0, sizeof(s_req));
    memset(&s_resp, 0, sizeof(s_resp));

    s_start_fail   = false;
    s_stop_fail    = false;
    s_results_fail = false;
    s_result_len   = 0U;

    (void)uds_safety_init();
    (void)routine_database_init();

    s_sess.initialized    = true;
    s_sess.active_session = UDS_SESSION_EXTENDED;
    s_sec.initialized     = true;
    s_sec.active_level    = 0U;
    s_srv.cfg.session_ctx  = &s_sess;
    s_srv.cfg.security_ctx = &s_sec;

    register_test_routines();
}

void tearDown(void) {}

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static void build_req(uint8_t subfn, uint16_t rid)
{
    s_req.data[0] = 0x31U;
    s_req.data[1] = subfn;
    s_req.data[2] = (uint8_t)(rid >> 8U);
    s_req.data[3] = (uint8_t)(rid & 0xFFU);
    s_req.length  = 4U;
}

/* ==========================================================================
 * Suite
 * ========================================================================== */

ZTEST_SUITE(svc_0x31, NULL, NULL, NULL, NULL, NULL);

ZTEST(svc_0x31, test_null_ctx)
{
    build_req(0x01U, RID_FULL);
    zassert_equal(UDS_STATUS_ERR_NULL_PTR,
                  uds_service_0x31_handler(NULL, &s_req, &s_resp), "");
}

ZTEST(svc_0x31, test_request_too_short)
{
    s_req.data[0] = 0x31U;
    s_req.data[1] = 0x01U;
    s_req.data[2] = 0xFFU;
    s_req.length  = 3U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

ZTEST(svc_0x31, test_subfn_zero_invalid)
{
    build_req(0x00U, RID_FULL);
    zassert_equal(UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

ZTEST(svc_0x31, test_subfn_four_invalid)
{
    build_req(0x04U, RID_FULL);
    zassert_equal(UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

ZTEST(svc_0x31, test_rid_not_found)
{
    build_req(0x01U, 0xDEADU);
    zassert_equal(UDS_STATUS_ERR_ROUTINE_NOT_FOUND,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

ZTEST(svc_0x31, test_session_too_low)
{
    s_sess.active_session = UDS_SESSION_DEFAULT;
    build_req(0x01U, RID_FULL);
    zassert_equal(UDS_STATUS_ERR_ROUTINE_NOT_SUPPORTED_IN_SESSION,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

ZTEST(svc_0x31, test_start_success_empty_result)
{
    build_req(0x01U, RID_FULL);
    s_result_len = 0U;
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");

    zassert_equal(4U, s_resp.length, "response length");
    zassert_equal(0x71U, s_resp.data[0], "RSID");
    zassert_equal(0x01U, s_resp.data[1], "subfn echo");
    zassert_equal(0xFFU, s_resp.data[2], "RID hi");
    zassert_equal(0x00U, s_resp.data[3], "RID lo");
}

ZTEST(svc_0x31, test_start_success_with_result_record)
{
    build_req(0x01U, RID_FULL);
    s_result_len = 4U;
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(8U, s_resp.length, "extended length");
    zassert_equal(0xAAU, s_resp.data[4], "result[0]");
    zassert_equal(0xBBU, s_resp.data[5], "result[1]");
    zassert_equal(0xCCU, s_resp.data[6], "result[2]");
    zassert_equal(0xDDU, s_resp.data[7], "result[3]");
}

ZTEST(svc_0x31, test_start_callback_failure)
{
    build_req(0x01U, RID_FULL);
    s_start_fail = true;
    zassert_equal(UDS_STATUS_ERR_ROUTINE_FAILED,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

ZTEST(svc_0x31, test_stop_not_supported_on_simple_rid)
{
    build_req(0x02U, RID_SIMPLE);
    zassert_equal(UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

ZTEST(svc_0x31, test_stop_success_on_full_rid)
{
    build_req(0x02U, RID_FULL);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(0x71U, s_resp.data[0], "RSID");
    zassert_equal(0x02U, s_resp.data[1], "subfn");
}

ZTEST(svc_0x31, test_results_not_supported_on_simple_rid)
{
    build_req(0x03U, RID_SIMPLE);
    zassert_equal(UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

ZTEST(svc_0x31, test_results_success_with_payload)
{
    build_req(0x03U, RID_FULL);
    s_result_len     = 2U;
    s_result_data[0] = 0x01U;
    s_result_data[1] = 0x00U;
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(6U,    s_resp.length,  "length with 2-byte result");
    zassert_equal(0x71U, s_resp.data[0], "RSID");
    zassert_equal(0x03U, s_resp.data[1], "subfn");
    zassert_equal(0x01U, s_resp.data[4], "result byte 0");
}

ZTEST(svc_0x31, test_results_callback_cond_not_met)
{
    build_req(0x03U, RID_FULL);
    s_results_fail = true;
    zassert_equal(UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

ZTEST(svc_0x31, test_option_record_no_crash)
{
    build_req(0x01U, RID_FULL);
    s_req.data[4] = 0x12U;
    s_req.data[5] = 0x34U;
    s_req.length  = 6U;
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(svc_0x31__test_null_ctx);
    RUN_TEST(svc_0x31__test_request_too_short);
    RUN_TEST(svc_0x31__test_subfn_zero_invalid);
    RUN_TEST(svc_0x31__test_subfn_four_invalid);
    RUN_TEST(svc_0x31__test_rid_not_found);
    RUN_TEST(svc_0x31__test_session_too_low);
    RUN_TEST(svc_0x31__test_start_success_empty_result);
    RUN_TEST(svc_0x31__test_start_success_with_result_record);
    RUN_TEST(svc_0x31__test_start_callback_failure);
    RUN_TEST(svc_0x31__test_stop_not_supported_on_simple_rid);
    RUN_TEST(svc_0x31__test_stop_success_on_full_rid);
    RUN_TEST(svc_0x31__test_results_not_supported_on_simple_rid);
    RUN_TEST(svc_0x31__test_results_success_with_payload);
    RUN_TEST(svc_0x31__test_results_callback_cond_not_met);
    RUN_TEST(svc_0x31__test_option_record_no_crash);
}
