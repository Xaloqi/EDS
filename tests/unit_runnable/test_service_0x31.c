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
 *   Session too low                → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31, EDS#468)
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
    routine_database_reset_state(); /* [EDS#484] every RID starts never-started */

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

/* [EDS#484] Start RID_FULL so stop/results are in sequence. */
static void start_full(void)
{
    build_req(0x01U, RID_FULL);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "start must succeed");
    memset(&s_resp, 0, sizeof(s_resp));
}

/* ==========================================================================
 * [EDS#282] Deferred startRoutine (routine_database_set_deferral)
 * ========================================================================== */
#define RID_DEFER   (0xF0F0U)

static uint32_t  s_defer_steps_left;
static uint32_t  s_defer_step_calls;
static uint32_t  s_defer_abort_calls;
static bool      s_defer_step_fail;

static uds_step_result_t cb_defer_step(uint8_t *res, uint8_t res_max,
                                       uint8_t *res_out, uds_nrc_t *nrc)
{
    s_defer_step_calls++;
    if (s_defer_step_fail) {
        *nrc = UDS_NRC_GENERAL_PROG_FAILURE;
        return UDS_STEP_FAILED;
    }
    if (s_defer_steps_left > 0U) {
        s_defer_steps_left--;
        return UDS_STEP_IN_PROGRESS;
    }
    if (res_max >= 1U) {
        res[0] = 0xAAU;
        *res_out = 1U;
    }
    return UDS_STEP_DONE;
}

static void cb_defer_abort(void)
{
    s_defer_abort_calls++;
}

static const routine_deferral_t k_deferral = {
    .step_cb = cb_defer_step, .abort_cb = cb_defer_abort, .step_max_ms = 3000U,
};

/* Register RID_DEFER (once: the database is never re-initialised between
 * tests) and opt it in; clear the job counters. */
static void defer_setup(void)
{
    routine_entry_t e;
    memset(&e, 0, sizeof(e));
    e.rid           = RID_DEFER;
    e.support_flags = (uint8_t)ROUTINE_SUPPORT_START;
    e.min_session   = (uint8_t)UDS_SESSION_EXTENDED;
    e.start_cb      = cb_start;
    e.description   = "defer";
    (void)routine_database_register(&e);
    zassert_equal(UDS_STATUS_OK, routine_database_set_deferral(RID_DEFER, &k_deferral), "");
    s_defer_steps_left  = 0U;
    s_defer_step_calls  = 0U;
    s_defer_abort_calls = 0U;
    s_defer_step_fail   = false;
}

static void defer_teardown(void)
{
    uds_server_pending_abort(&s_srv);
    (void)routine_database_set_deferral(RID_DEFER, NULL);
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
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
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
    start_full();
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
    start_full();
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
    start_full();
    build_req(0x03U, RID_FULL);
    s_results_fail = true;
    zassert_equal(UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
}

/* [EDS#484] ISO 14229-1:2026 Table 430: results for a routine that was
 * never started → NRC 0x24, and the callback is not invoked. */
ZTEST(svc_0x31, test_results_before_start_is_sequence_error)
{
    build_req(0x03U, RID_FULL);
    s_results_fail = true; /* would be 0x22 if the callback were reached */
    zassert_equal(UDS_STATUS_ERR_REQUEST_SEQUENCE,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp),
                  "results before start must be NRC 0x24 (EDS#484)");
}

/* [EDS#484] Table 430: stop for a routine that is not active → 0x24. */
ZTEST(svc_0x31, test_stop_before_start_is_sequence_error)
{
    build_req(0x02U, RID_FULL);
    zassert_equal(UDS_STATUS_ERR_REQUEST_SEQUENCE,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp),
                  "stop before start must be NRC 0x24 (EDS#484)");
}

/* [EDS#484] A stopped routine is no longer active, but its results remain. */
ZTEST(svc_0x31, test_after_stop_results_ok_second_stop_sequence_error)
{
    start_full();
    build_req(0x02U, RID_FULL);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "first stop");
    build_req(0x02U, RID_FULL);
    zassert_equal(UDS_STATUS_ERR_REQUEST_SEQUENCE,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp),
                  "a second stop must be NRC 0x24");
    build_req(0x03U, RID_FULL);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp),
                  "results must stay available after a stop (method A)");
}

/* [EDS#484] A failed start records nothing. */
ZTEST(svc_0x31, test_failed_start_does_not_count_as_started)
{
    build_req(0x01U, RID_FULL);
    s_start_fail = true;
    zassert_equal(UDS_STATUS_ERR_ROUTINE_FAILED,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "start fails");
    build_req(0x03U, RID_FULL);
    zassert_equal(UDS_STATUS_ERR_REQUEST_SEQUENCE,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp),
                  "results after a failed start must be NRC 0x24");
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

/* [EDS#282] TC-0x31-DEF-001: 7F 31 78, a 0x78 after each 3 s step, then
 * 71 01 F0 F0 AA, and the routine counts as started. */
ZTEST(svc_0x31, test_deferred_start_completes)
{
    uds_msg_buf_t frame;
    uint8_t       state = 0U;

    defer_setup();
    s_defer_steps_left = 2U;
    build_req(0x01U, RID_DEFER);
    zassert_equal(UDS_STATUS_OK, uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(0x7FU, s_resp.data[0], "");
    zassert_equal(0x78U, s_resp.data[2], "deferred start must answer 7F 31 78");
    zassert_equal(0U, s_defer_step_calls, "");

    for (int i = 0; i < 2; i++) {
        zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&s_srv, &frame), "");
        zassert_equal(0x78U, frame.data[2], "0x78 after each long step");
        uds_server_frame_sent(&s_srv);
    }
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&s_srv, &frame), "");
    zassert_equal(5U, frame.length, "");
    zassert_equal(0x71U, frame.data[0], "");
    zassert_equal(0x01U, frame.data[1], "");
    zassert_equal(0xF0U, frame.data[2], "");
    zassert_equal(0xF0U, frame.data[3], "");
    zassert_equal(0xAAU, frame.data[4], "routineStatusRecord from step_cb");
    uds_server_frame_sent(&s_srv);

    (void)routine_database_get_state(RID_DEFER, &state);
    zassert_true((state & ROUTINE_STATE_STARTED) != 0U, "a completed deferred start is started");
    zassert_equal(0U, s_defer_abort_calls, "");
    defer_teardown();
}

/* [EDS#282] TC-0x31-DEF-002: start_cb decides every NRC but 0x78. */
ZTEST(svc_0x31, test_deferred_start_cb_failure_not_deferred)
{
    defer_setup();
    s_start_fail = true;
    build_req(0x01U, RID_DEFER);
    zassert_equal(UDS_STATUS_ERR_ROUTINE_FAILED,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
    zassert_false(uds_server_pending_active(&s_srv), "a refused start is not deferred");
    zassert_equal(0U, s_defer_abort_calls, "");
    defer_teardown();
}

/* [EDS#282] TC-0x31-DEF-003: a failing step answers its NRC, routine not started. */
ZTEST(svc_0x31, test_deferred_step_failure)
{
    uds_msg_buf_t frame;
    uint8_t       state = 0xFFU;

    defer_setup();
    s_defer_step_fail = true;
    build_req(0x01U, RID_DEFER);
    zassert_equal(UDS_STATUS_OK, uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&s_srv, &frame), "");
    zassert_equal(0x7FU, frame.data[0], "");
    zassert_equal(0x72U, frame.data[2], "step_cb's NRC");
    uds_server_frame_sent(&s_srv);
    (void)routine_database_get_state(RID_DEFER, &state);
    zassert_equal(0U, state, "a failed deferred start is not started");
    zassert_equal(0U, s_defer_abort_calls, "");
    defer_teardown();
}

/* [EDS#282] TC-0x31-DEF-004: functional -> NRC 0x22 (no functional
 * deferral in v1), and the work start_cb began is aborted. */
ZTEST(svc_0x31, test_deferred_functional_refused_and_aborted)
{
    defer_setup();
    s_srv.rx_addressing = UDS_ADDRESSING_FUNCTIONAL;
    build_req(0x01U, RID_DEFER);
    zassert_equal(UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(1U, s_defer_abort_calls, "start_cb's work must be stopped");
    zassert_false(uds_server_pending_active(&s_srv), "");
    s_srv.rx_addressing = UDS_ADDRESSING_PHYSICAL;
    defer_teardown();
}

/* [EDS#282] TC-0x31-DEF-005: cap / S3 / disconnect reach abort_cb. */
ZTEST(svc_0x31, test_deferred_server_abort_calls_abort_cb)
{
    defer_setup();
    s_defer_steps_left = 100U;
    build_req(0x01U, RID_DEFER);
    zassert_equal(UDS_STATUS_OK, uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
    uds_server_pending_abort(&s_srv);
    zassert_equal(1U, s_defer_abort_calls, "");
    defer_teardown();
}

/* [EDS#282] TC-0x31-DEF-006: clearing the deferral makes the start synchronous. */
ZTEST(svc_0x31, test_deferral_cleared_is_synchronous)
{
    defer_setup();
    zassert_equal(UDS_STATUS_OK, routine_database_set_deferral(RID_DEFER, NULL), "");
    zassert_true(routine_database_get_deferral(RID_DEFER) == NULL, "");
    build_req(0x01U, RID_DEFER);
    zassert_equal(UDS_STATUS_OK, uds_service_0x31_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(0x71U, s_resp.data[0], "synchronous positive response");
    zassert_equal(UDS_STATUS_ERR_ROUTINE_NOT_FOUND,
                  routine_database_set_deferral(0x1234U, &k_deferral), "unknown RID");
    defer_teardown();
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
    RUN_TEST(svc_0x31__test_results_before_start_is_sequence_error);
    RUN_TEST(svc_0x31__test_stop_before_start_is_sequence_error);
    RUN_TEST(svc_0x31__test_after_stop_results_ok_second_stop_sequence_error);
    RUN_TEST(svc_0x31__test_failed_start_does_not_count_as_started);
    RUN_TEST(svc_0x31__test_option_record_no_crash);
    RUN_TEST(svc_0x31__test_deferred_start_completes);
    RUN_TEST(svc_0x31__test_deferred_start_cb_failure_not_deferred);
    RUN_TEST(svc_0x31__test_deferred_step_failure);
    RUN_TEST(svc_0x31__test_deferred_functional_refused_and_aborted);
    RUN_TEST(svc_0x31__test_deferred_server_abort_calls_abort_cb);
    RUN_TEST(svc_0x31__test_deferral_cleared_is_synchronous);
}
