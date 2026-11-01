// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_uds_server_pending.c
 *
 * MODULE UNDER TEST: core/uds_server.c — [EDS#282] response-pending jobs
 *                    (uds_server_defer / uds_server_poll /
 *                    uds_server_frame_sent / uds_server_pending_abort)
 *
 * Coverage:
 *   TC-PEND-001  defer -> 7F sid 78 as the first response, even with SPRMIB
 *                set (ISO 14229-1 never suppresses a negative response).
 *   TC-PEND-002  poll with nothing pending -> IDLE, out_frame untouched.
 *   TC-PEND-003  fast steps, no time passing -> no frame until DONE; the
 *                final positive response goes out despite SPRMIB.
 *   TC-PEND-004  keepalive at P2*server_max / 2 of ticks, not before.
 *   TC-PEND-005  a step whose step_max_ms exceeds the keepalive gets a 0x78
 *                after every step (ticks lost while it blocked are credited).
 *   TC-PEND-006  the owed frame is returned again until frame_sent(), and
 *                no step runs meanwhile (ISO-TP busy).
 *   TC-PEND-007  FAILED -> 7F sid nrc, abort NOT called (the job ended).
 *   TC-PEND-008  cap -> 7F sid 10 and exactly one abort.
 *   TC-PEND-009  second request while pending: physical -> 0x21 (0x10 too,
 *                session unchanged), functional -> dropped, 3E 80 ->
 *                accepted silently, 3E 00 physical -> 0x21.
 *   TC-PEND-010  functional deferral refused (NRC 0x22), nothing pending.
 *   TC-PEND-011  S3 restarted per frame; with nothing polling, S3 runs out
 *                and the job is aborted once and the slot freed.
 *   TC-PEND-012  re-entrancy: a step calling poll() or process_request()
 *                fails its job with 7F sid 10, abort called.
 *   TC-PEND-013  step_max_ms >= P2*server_max refused at defer.
 *   TC-PEND-014  keepalive clamped to S3 / 2 when S3 is the shorter.
 *   TC-PEND-015  pending_abort(): abort once while running; not at all once
 *                the final frame is owed.
 *   TC-PEND-016  DONE with an empty response -> 7F sid 10.
 *   TC-PEND-017  the server is free again after the final frame is sent.
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h for host compilation)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stdbool.h>

#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_types.h"
#include "uds_access_table.h"

/* ==========================================================================
 * Mock job
 * ========================================================================== */

static uint32_t          g_steps_needed;   /* DONE on this call; 0 = never */
static uint32_t          g_step_calls;
static uint32_t          g_abort_calls;
static uint32_t          g_fail_on_call;   /* 0 = never */
static uds_nrc_t         g_fail_nrc;
static bool              g_empty_done;
static int               g_reenter;        /* 0 none, 1 poll, 2 process */
static uds_server_ctx_t  g_srv;

static uds_step_result_t mock_step(uds_msg_buf_t *final_resp, uds_nrc_t *nrc)
{
    g_step_calls++;

    if (g_reenter == 1) {
        uds_msg_buf_t scratch;
        (void)uds_server_poll(&g_srv, &scratch);
    } else if (g_reenter == 2) {
        uds_msg_buf_t req;
        uds_msg_buf_t resp;
        memset(&req, 0, sizeof(req));
        req.data[0] = 0x3EU;
        req.data[1] = 0x00U;
        req.length  = 2U;
        (void)uds_server_process_request(&g_srv, &req, &resp);
    } else {
        /* no re-entry */
    }

    if (g_step_calls == g_fail_on_call) {
        *nrc = g_fail_nrc;
        return UDS_STEP_FAILED;
    }
    if ((g_steps_needed != 0U) && (g_step_calls >= g_steps_needed)) {
        if (g_empty_done) {
            final_resp->length = 0U;
        } else {
            final_resp->data[0] = 0x71U;
            final_resp->data[1] = 0x01U;
            final_resp->data[2] = 0xF0U;
            final_resp->data[3] = 0x00U;
            final_resp->length  = 4U;
        }
        return UDS_STEP_DONE;
    }
    return UDS_STEP_IN_PROGRESS;
}

static void mock_abort(void)
{
    g_abort_calls++;
}

static uint32_t g_step_max_ms;

/* SID 0x31 stub: defers whatever it gets. */
static uds_status_t stub_deferring_handler(uds_server_ctx_t    *ctx,
                                           const uds_msg_buf_t *req,
                                           uds_msg_buf_t       *resp)
{
    uds_pending_job_t job;

    (void)req;
    job.sid         = 0x31U;
    job.step_max_ms = g_step_max_ms;
    job.step        = mock_step;
    job.abort       = mock_abort;
    return uds_server_defer(ctx, &job, resp);
}

static uds_status_t stub_ok_handler(uds_server_ctx_t    *ctx,
                                    const uds_msg_buf_t *req,
                                    uds_msg_buf_t       *resp)
{
    (void)ctx;
    resp->data[0] = (uint8_t)(req->data[0] + 0x40U);
    resp->data[1] = req->data[1];
    resp->length  = 2U;
    return UDS_STATUS_OK;
}

static uds_status_t stub_seed_gen(uint8_t lvl, uint8_t *buf, uint8_t len, uint8_t *out)
{
    (void)lvl; (void)buf; (void)len;
    *out = 0U;
    return UDS_STATUS_OK;
}

static bool stub_key_validate(uint8_t lvl, const uint8_t *seed, uint8_t sl,
                              const uint8_t *key, uint8_t kl)
{
    (void)lvl; (void)seed; (void)sl; (void)key; (void)kl;
    return false;
}

static const uds_service_entry_t g_table[] = {
    { 0x31U, stub_deferring_handler, true },
    { 0x22U, stub_ok_handler,        false },
    { 0x3EU, stub_ok_handler,        true },
};

static const uds_access_entry_t g_acl[] = {
    { 0x31U, UDS_ACL_SESSION_ALL, 0U, false },
    { 0x22U, UDS_ACL_SESSION_ALL, 0U, false },
    { 0x3EU, UDS_ACL_SESSION_ALL, 0U, false },
};

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_msg_buf_t      g_req;
static uds_msg_buf_t      g_resp;
static uds_msg_buf_t      g_frame;

static void server_up(uint32_t s3_ms)
{
    static const uds_security_cfg_t sec_cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,
        .key_validate_cb  = stub_key_validate,
        .seed_generate_cb = stub_seed_gen,
    };
    uds_server_cfg_t cfg;

    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_sec,  0, sizeof(g_sec));
    memset(&g_srv,  0, sizeof(g_srv));
    (void)uds_session_init(&g_sess, s3_ms);
    (void)uds_security_init(&g_sec, &sec_cfg);

    memset(&cfg, 0, sizeof(cfg));
    cfg.p2_server_max_ms      = 50U;
    cfg.p2_star_server_max_ms = 5000U;
    cfg.session_ctx           = &g_sess;
    cfg.security_ctx          = &g_sec;
    cfg.service_table         = g_table;
    cfg.service_table_count   = (uint8_t)3U;
    cfg.access_table          = g_acl;
    cfg.access_table_count    = (uint8_t)3U;
    zassert_equal(UDS_STATUS_OK, uds_server_init(&g_srv, &cfg), "server init");
}

void setUp(void)
{
    g_steps_needed = 0U;
    g_step_calls   = 0U;
    g_abort_calls  = 0U;
    g_fail_on_call = 0U;
    g_fail_nrc     = UDS_NRC_CONDITIONS_NOT_CORRECT;
    g_empty_done   = false;
    g_reenter      = 0;
    g_step_max_ms  = 1U;
    memset(&g_req,   0, sizeof(g_req));
    memset(&g_resp,  0, sizeof(g_resp));
    memset(&g_frame, 0, sizeof(g_frame));
    server_up(5000U);
}

void tearDown(void) {}

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static uds_status_t send2(uint8_t sid, uint8_t sub, uds_addressing_t addr)
{
    memset(&g_req, 0, sizeof(g_req));
    memset(&g_resp, 0, sizeof(g_resp));
    g_req.data[0] = sid;
    g_req.data[1] = sub;
    g_req.length  = 2U;
    return uds_server_process_request_ex(&g_srv, &g_req, &g_resp, addr);
}

/* Start a routine (31 01 F0 00, or 31 81 F0 00 with SPRMIB). */
static void start_job(bool sprmib)
{
    memset(&g_req, 0, sizeof(g_req));
    memset(&g_resp, 0, sizeof(g_resp));
    g_req.data[0] = 0x31U;
    g_req.data[1] = sprmib ? 0x81U : 0x01U;
    g_req.data[2] = 0xF0U;
    g_req.data[3] = 0x00U;
    g_req.length  = 4U;
    zassert_equal(UDS_STATUS_OK,
                  uds_server_process_request(&g_srv, &g_req, &g_resp), "");
}

static void tick_ms(uint32_t ms)
{
    uint32_t i;
    for (i = 0U; i < ms; i++) {
        (void)uds_server_tick_1ms(&g_srv);
    }
}

static bool is_nrc(const uds_msg_buf_t *m, uint8_t sid, uint8_t nrc)
{
    return (m->length == 3U) && (m->data[0] == 0x7FU) &&
           (m->data[1] == sid) && (m->data[2] == nrc);
}

/* ==========================================================================
 * Tests
 * ========================================================================== */

ZTEST_SUITE(uds_server_pending, NULL, NULL, NULL, NULL, NULL);

/* TC-PEND-001 */
ZTEST(uds_server_pending, test_defer_answers_0x78_despite_sprmib)
{
    start_job(true);
    zassert_true(is_nrc(&g_resp, 0x31U, 0x78U),
                 "first response must be 7F 31 78 even with SPRMIB (the §1 trap)");
    zassert_true(uds_server_pending_active(&g_srv), "");
    zassert_equal(0U, g_step_calls, "defer must not run a step itself");
}

/* TC-PEND-002 */
ZTEST(uds_server_pending, test_poll_idle_when_nothing_pending)
{
    memset(&g_frame, 0xAA, sizeof(g_frame));
    zassert_equal((int)UDS_POLL_IDLE, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_equal(0xAAU, g_frame.data[0], "out_frame must be untouched");
}

/* TC-PEND-003 */
ZTEST(uds_server_pending, test_fast_steps_then_final_despite_sprmib)
{
    g_steps_needed = 5U;
    start_job(true);

    for (int i = 0; i < 4; i++) {
        zassert_equal((int)UDS_POLL_NOTHING_NOW, (int)uds_server_poll(&g_srv, &g_frame),
                      "no frame while steps are fast and no keepalive is due");
    }
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_equal(4U, g_frame.length, "");
    zassert_equal(0x71U, g_frame.data[0],
                  "final positive response must be sent after a 0x78, SPRMIB or not");
    uds_server_frame_sent(&g_srv);
    zassert_false(uds_server_pending_active(&g_srv), "");
    zassert_equal(0U, g_abort_calls, "a job that completes is not aborted");
}

/* TC-PEND-004 */
ZTEST(uds_server_pending, test_keepalive_at_half_p2_star)
{
    start_job(false);   /* never finishes */

    tick_ms(2498U);   /* + 1 ms step credit = 2499 */
    zassert_equal((int)UDS_POLL_NOTHING_NOW, (int)uds_server_poll(&g_srv, &g_frame),
                  "keepalive before P2*/2");
    tick_ms(1U);
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_true(is_nrc(&g_frame, 0x31U, 0x78U), "keepalive must be 7F 31 78");
    uds_server_frame_sent(&g_srv);

    /* The interval restarts from the frame. Step credit (1 ms per poll)
     * counts too, so tick less than the interval minus one step. */
    tick_ms(2400U);
    zassert_equal((int)UDS_POLL_NOTHING_NOW, (int)uds_server_poll(&g_srv, &g_frame), "");
    tick_ms(100U);
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_true(is_nrc(&g_frame, 0x31U, 0x78U), "");
    uds_server_pending_abort(&g_srv);
}

/* TC-PEND-005 */
ZTEST(uds_server_pending, test_long_step_gets_0x78_after_every_step)
{
    g_step_max_ms  = 4000U;   /* the 0x34 STM32H7 sector-erase assumption */
    g_steps_needed = 3U;
    start_job(false);

    for (uint32_t step = 1U; step <= 2U; step++) {
        zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
        zassert_true(is_nrc(&g_frame, 0x31U, 0x78U),
                     "a 4 s step with no ticks must still be followed by a 0x78");
        zassert_equal(step, g_step_calls, "");
        uds_server_frame_sent(&g_srv);
    }
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_equal(0x71U, g_frame.data[0], "");
    uds_server_frame_sent(&g_srv);
    zassert_false(uds_server_pending_active(&g_srv), "");
}

/* TC-PEND-006 */
ZTEST(uds_server_pending, test_owed_frame_retained_until_sent)
{
    g_steps_needed = 1U;
    start_job(false);

    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_equal(0x71U, g_frame.data[0], "");

    /* Transport busy: no frame_sent(). */
    memset(&g_frame, 0, sizeof(g_frame));
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_equal(0x71U, g_frame.data[0], "the same frame must come back");
    zassert_equal(4U, g_frame.length, "");
    zassert_equal(1U, g_step_calls, "no step may run while a frame is owed");
    zassert_true(uds_server_pending_active(&g_srv), "slot held until the final frame is sent");

    uds_server_frame_sent(&g_srv);
    zassert_equal((int)UDS_POLL_IDLE, (int)uds_server_poll(&g_srv, &g_frame), "");
}

/* TC-PEND-007 */
ZTEST(uds_server_pending, test_failed_step_sends_its_nrc)
{
    g_fail_on_call = 2U;
    g_fail_nrc     = UDS_NRC_GENERAL_PROG_FAILURE;
    start_job(false);

    zassert_equal((int)UDS_POLL_NOTHING_NOW, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_true(is_nrc(&g_frame, 0x31U, 0x72U), "step's NRC must be sent");
    uds_server_frame_sent(&g_srv);
    zassert_false(uds_server_pending_active(&g_srv), "");
    zassert_equal(0U, g_abort_calls, "a job that failed by itself is not aborted");
}

/* TC-PEND-008 */
ZTEST(uds_server_pending, test_cap_sends_general_reject_and_aborts_once)
{
    uint32_t frames_0x78 = 0U;
    bool     got_reject  = false;

    start_job(false);   /* never finishes */

    /* Drive it like a loop: 10 ms per iteration until the cap. */
    for (uint32_t it = 0U; it < 13000U; it++) {
        uds_poll_result_t r;
        tick_ms(10U);
        r = uds_server_poll(&g_srv, &g_frame);
        if (r == UDS_POLL_FRAME) {
            if (is_nrc(&g_frame, 0x31U, 0x78U)) {
                frames_0x78++;
            } else {
                zassert_true(is_nrc(&g_frame, 0x31U, 0x10U), "cap must answer 7F 31 10");
                got_reject = true;
            }
            uds_server_frame_sent(&g_srv);
            if (got_reject) {
                break;
            }
        }
    }
    zassert_true(got_reject, "cap never fired");
    zassert_equal(1U, g_abort_calls, "abort exactly once at the cap");
    zassert_true(frames_0x78 >= 40U, "keepalives every ~2.5 s over 120 s");
    zassert_false(uds_server_pending_active(&g_srv), "");
    zassert_equal(1U, g_abort_calls, "");
}

/* TC-PEND-009 */
ZTEST(uds_server_pending, test_second_request_matrix)
{
    start_job(false);

    /* Physical 22 F1 90 -> 7F 22 21. */
    memset(&g_req, 0, sizeof(g_req));
    g_req.data[0] = 0x22U; g_req.data[1] = 0xF1U; g_req.data[2] = 0x90U;
    g_req.length = 3U;
    zassert_equal(UDS_STATUS_OK,
                  uds_server_process_request(&g_srv, &g_req, &g_resp), "");
    zassert_true(is_nrc(&g_resp, 0x22U, 0x21U), "physical request while pending -> 0x21");

    /* Physical 10 03 -> 0x21, session unchanged. */
    zassert_equal(UDS_STATUS_OK, send2(0x10U, 0x03U, UDS_ADDRESSING_PHYSICAL), "");
    zassert_true(is_nrc(&g_resp, 0x10U, 0x21U), "");
    zassert_equal((int)UDS_SESSION_DEFAULT, (int)g_sess.active_session,
                  "a session change must not happen under a running job");

    /* Physical 11 01 -> 0x21, no reset recorded. */
    zassert_equal(UDS_STATUS_OK, send2(0x11U, 0x01U, UDS_ADDRESSING_PHYSICAL), "");
    zassert_true(is_nrc(&g_resp, 0x11U, 0x21U), "");
    zassert_equal(0U, g_srv.pending_reset_type, "");

    /* Functional 22 -> dropped. */
    zassert_equal(UDS_STATUS_OK, send2(0x22U, 0xF1U, UDS_ADDRESSING_FUNCTIONAL), "");
    zassert_equal(0U, g_resp.length, "functional request while pending is dropped");

    /* 3E 80, functional and physical -> accepted silently, S3 restarted. */
    tick_ms(1000U);
    zassert_equal(UDS_STATUS_OK, send2(0x3EU, 0x80U, UDS_ADDRESSING_FUNCTIONAL), "");
    zassert_equal(0U, g_resp.length, "");
    zassert_equal(5000U, g_sess.s3_timer_ms, "3E 80 must restart S3");
    zassert_equal(UDS_STATUS_OK, send2(0x3EU, 0x80U, UDS_ADDRESSING_PHYSICAL), "");
    zassert_equal(0U, g_resp.length, "");

    /* 3E 00 physical -> 0x21. */
    zassert_equal(UDS_STATUS_OK, send2(0x3EU, 0x00U, UDS_ADDRESSING_PHYSICAL), "");
    zassert_true(is_nrc(&g_resp, 0x3EU, 0x21U), "");

    zassert_equal(0U, g_step_calls, "no request may drive the job");
    zassert_true(uds_server_pending_active(&g_srv), "the job is unaffected");
    uds_server_pending_abort(&g_srv);
}

/* TC-PEND-010 */
ZTEST(uds_server_pending, test_functional_deferral_refused)
{
    memset(&g_req, 0, sizeof(g_req));
    g_req.data[0] = 0x31U; g_req.data[1] = 0x01U; g_req.data[2] = 0xF0U; g_req.data[3] = 0x00U;
    g_req.length = 4U;
    zassert_equal(UDS_STATUS_OK,
                  uds_server_process_request_ex(&g_srv, &g_req, &g_resp,
                                                UDS_ADDRESSING_FUNCTIONAL), "");
    zassert_true(is_nrc(&g_resp, 0x31U, 0x22U), "no functional deferral in v1 -> 0x22");
    zassert_false(uds_server_pending_active(&g_srv), "");
}

/* TC-PEND-011 */
ZTEST(uds_server_pending, test_s3_restarted_per_frame_and_runs_out_unpolled)
{
    /* S3 only runs outside the default session. */
    (void)uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);

    start_job(false);
    tick_ms(2500U);
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_equal(5000U, g_sess.s3_timer_ms, "a keepalive must restart S3");
    uds_server_frame_sent(&g_srv);

    /* Nothing polls any more: S3 runs out, the job is dropped. */
    tick_ms(5001U);
    zassert_equal((int)UDS_SESSION_DEFAULT, (int)g_sess.active_session,
                  "S3 must not be held when nothing polls");
    zassert_equal(1U, g_abort_calls, "S3 expiry aborts the job once");
    zassert_false(uds_server_pending_active(&g_srv), "slot freed on S3 expiry");
}

/* TC-PEND-012 */
ZTEST(uds_server_pending, test_reentrant_step_fails_job)
{
    for (int mode = 1; mode <= 2; mode++) {
        setUp();
        g_reenter = mode;
        start_job(false);
        zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
        zassert_true(is_nrc(&g_frame, 0x31U, 0x10U), "re-entry must fail the job with 0x10");
        zassert_equal(1U, g_abort_calls, "");
        uds_server_frame_sent(&g_srv);
        zassert_false(uds_server_pending_active(&g_srv), "");
    }
}

/* TC-PEND-013 */
ZTEST(uds_server_pending, test_step_max_not_below_p2_star_refused)
{
    g_step_max_ms = 5000U;
    start_job(false);
    zassert_true(is_nrc(&g_resp, 0x31U, 0x22U), "step_max >= P2* -> 0x22");
    zassert_false(uds_server_pending_active(&g_srv), "");
}

/* TC-PEND-014 */
ZTEST(uds_server_pending, test_keepalive_clamped_to_half_s3)
{
    server_up(1000U);
    start_job(false);
    tick_ms(498U);
    zassert_equal((int)UDS_POLL_NOTHING_NOW, (int)uds_server_poll(&g_srv, &g_frame), "");
    tick_ms(1U);
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame),
                  "keepalive must beat a 1 s S3 (S3/2 = 500 ms)");
    uds_server_pending_abort(&g_srv);
}

/* TC-PEND-015 */
ZTEST(uds_server_pending, test_pending_abort)
{
    start_job(false);
    uds_server_pending_abort(&g_srv);
    zassert_equal(1U, g_abort_calls, "");
    zassert_false(uds_server_pending_active(&g_srv), "");
    uds_server_pending_abort(&g_srv);
    zassert_equal(1U, g_abort_calls, "idempotent");

    g_steps_needed = 1U;
    g_step_calls   = 0U;
    start_job(false);
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
    uds_server_pending_abort(&g_srv);
    zassert_equal(1U, g_abort_calls, "a finished job (final frame owed) is not aborted");
    zassert_false(uds_server_pending_active(&g_srv), "");
}

/* TC-PEND-016 */
ZTEST(uds_server_pending, test_done_with_empty_response_rejected)
{
    g_steps_needed = 1U;
    g_empty_done   = true;
    start_job(false);
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");
    zassert_true(is_nrc(&g_frame, 0x31U, 0x10U), "an empty final response is a job fault");
    uds_server_frame_sent(&g_srv);
}

/* TC-PEND-017 */
ZTEST(uds_server_pending, test_server_free_after_final_sent)
{
    g_steps_needed = 1U;
    start_job(false);
    zassert_equal((int)UDS_POLL_FRAME, (int)uds_server_poll(&g_srv, &g_frame), "");

    /* Final owed, not sent: still busy. */
    zassert_equal(UDS_STATUS_OK, send2(0x3EU, 0x00U, UDS_ADDRESSING_PHYSICAL), "");
    zassert_true(is_nrc(&g_resp, 0x3EU, 0x21U), "busy until the final frame is sent");

    uds_server_frame_sent(&g_srv);
    zassert_equal(UDS_STATUS_OK, send2(0x3EU, 0x00U, UDS_ADDRESSING_PHYSICAL), "");
    zassert_equal(0x7EU, g_resp.data[0], "served normally once the job is over");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(uds_server_pending__test_defer_answers_0x78_despite_sprmib);
    RUN_TEST(uds_server_pending__test_poll_idle_when_nothing_pending);
    RUN_TEST(uds_server_pending__test_fast_steps_then_final_despite_sprmib);
    RUN_TEST(uds_server_pending__test_keepalive_at_half_p2_star);
    RUN_TEST(uds_server_pending__test_long_step_gets_0x78_after_every_step);
    RUN_TEST(uds_server_pending__test_owed_frame_retained_until_sent);
    RUN_TEST(uds_server_pending__test_failed_step_sends_its_nrc);
    RUN_TEST(uds_server_pending__test_cap_sends_general_reject_and_aborts_once);
    RUN_TEST(uds_server_pending__test_second_request_matrix);
    RUN_TEST(uds_server_pending__test_functional_deferral_refused);
    RUN_TEST(uds_server_pending__test_s3_restarted_per_frame_and_runs_out_unpolled);
    RUN_TEST(uds_server_pending__test_reentrant_step_fails_job);
    RUN_TEST(uds_server_pending__test_step_max_not_below_p2_star_refused);
    RUN_TEST(uds_server_pending__test_keepalive_clamped_to_half_s3);
    RUN_TEST(uds_server_pending__test_pending_abort);
    RUN_TEST(uds_server_pending__test_done_with_empty_response_rejected);
    RUN_TEST(uds_server_pending__test_server_free_after_final_sent);
}
