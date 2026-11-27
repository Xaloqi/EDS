// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — unit tests
 * FILE: tests/unit_runnable/test_uds_exec.c
 * MODULE UNDER TEST: core/uds_exec.c — the diagnostic executor (EDS#197 J3)
 *
 * PURPOSE: Deterministic interleavings of two producers and the executor, on
 *   one thread. The test is the scheduler: it orders submit, clock tick and
 *   executor pass events, and runs every order of a small event set.
 *   Bindings are fakes: a counted critical section, a test clock, wake /
 *   notify / reset recorders.
 *
 *   Producers: CAN = source 0 (periodic TX), DOIP = source 1 (none, ⬥E).
 *
 *   Every order (permutations):
 *     - test_every_order_session_and_tester_present
 *     - test_every_order_job_and_other_request
 *   A request arriving during a job step (busy_src fast reject):
 *     - test_request_during_job_step_is_rejected_fast
 *   Backpressure, one slot per producer:
 *     - test_backpressure_physical_0x21_functional_dropped
 *   Routing by source:
 *     - test_job_frames_go_to_job_src_and_retry_when_unsent
 *     - test_periodic_frames_go_to_subscriber
 *     - test_0x2a_from_producer_without_periodic_tx_gets_0x22
 *     - test_periodic_for_unknown_src_is_dropped
 *   ECUReset after the response is released:
 *     - test_reset_waits_for_release
 *   Calls, order, counters, configuration:
 *     - test_call_runs_on_executor
 *     - test_round_robin_and_queue_wait
 *     - test_init_rejects_bad_cfg
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "uds_exec.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_periodic.h"
#include "services.h"
#include "did_database.h"
#include "uds_types.h"
#include "eds_race_point.h"

#define SRC_CAN   (0U)
#define SRC_DOIP  (1U)
#define IDX_CAN   (0U)
#define IDX_DOIP  (1U)

/* =========================================================================
 * Fake bindings
 * ========================================================================= */

static uint32_t g_now;
static int32_t  g_depth;          /* critical-section nesting, must be 0/1 */
static int32_t  g_depth_max;
static uint32_t g_wakes;
static uint32_t g_notifies[2];
static uint32_t g_resets;
static uint8_t  g_reset_type;

static uint32_t f_now(void) { return g_now; }
static void f_enter(void)
{
    g_depth++;
    if (g_depth > g_depth_max) {
        g_depth_max = g_depth;
    }
}
static void f_exit(void)                 { g_depth--; }
static void f_wake(void *a)              { (void)a; g_wakes++; }
static void f_notify(void *a)            { g_notifies[(uintptr_t)a]++; }
static void f_reset(uint8_t t)           { g_resets++; g_reset_type = t; }

/* =========================================================================
 * Server: real 0x10 / 0x11 / 0x2A / 0x3E, a deferring 0x31, and 0xBA, which
 * records the source of each dispatch.
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;
static uds_exec_t         g_ex;

static uint32_t g_job_left;      /* steps still IN_PROGRESS */
static uint32_t g_job_steps;
static uint32_t g_job_aborts;
static void   (*g_in_step)(void);  /* runs inside a step, if set */

static uds_step_result_t job_step(uds_msg_buf_t *final_resp, uds_nrc_t *nrc)
{
    (void)nrc;
    g_job_steps++;
    if (g_in_step != NULL) {
        g_in_step();
    }
    if (g_job_left > 0U) {
        g_job_left--;
        return UDS_STEP_IN_PROGRESS;
    }
    final_resp->data[0] = 0x71U;
    final_resp->data[1] = 0x01U;
    final_resp->data[2] = 0xF0U;
    final_resp->data[3] = 0x00U;
    final_resp->length  = 4U;
    return UDS_STEP_DONE;
}

static void job_abort(void) { g_job_aborts++; }

static uds_status_t h_defer_0x31(uds_server_ctx_t *ctx, const uds_msg_buf_t *req,
                                 uds_msg_buf_t *resp)
{
    static const uds_pending_job_t job = {
        .sid = 0x31U, .step_max_ms = 10U, .step = job_step, .abort = job_abort,
    };
    (void)req;
    return uds_server_defer(ctx, &job, resp);
}

static uint8_t g_order[8];
static uint8_t g_order_n;

static uds_status_t h_record_0xba(uds_server_ctx_t *ctx, const uds_msg_buf_t *req,
                                  uds_msg_buf_t *resp)
{
    (void)req;
    if (g_order_n < (uint8_t)sizeof(g_order)) {
        g_order[g_order_n] = uds_server_request_src(ctx);
        g_order_n++;
    }
    resp->data[0] = 0xFAU;
    resp->length  = 1U;
    return UDS_STATUS_OK;
}

static const uds_service_entry_t k_table[] = {
    { 0x10U, uds_service_0x10_handler, true  },
    { 0x11U, uds_service_0x11_handler, true  },
    { 0x2AU, uds_service_0x2A_handler, false },
    { 0x3EU, uds_service_0x3E_handler, true  },
    { 0x31U, h_defer_0x31,             true  },
    { 0xBAU, h_record_0xba,            false },
};
static const uds_access_entry_t k_acl[] = {
    { 0x10U, UDS_ACL_SESSION_ALL,         0U, false },
    { 0x11U, UDS_ACL_SESSION_ALL,         0U, false },
    { 0x2AU, UDS_ACL_SESSION_NON_DEFAULT, 0U, false },
    { 0x3EU, UDS_ACL_SESSION_ALL,         0U, false },
    { 0x31U, UDS_ACL_SESSION_ALL,         0U, false },
    { 0xBAU, UDS_ACL_SESSION_ALL,         0U, false },
};

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l; (void)b; (void)n;
    *o = 0U;
    return UDS_STATUS_OK;
}
static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl, const uint8_t *k, uint8_t kl)
{
    (void)l; (void)s; (void)sl; (void)k; (void)kl;
    return false;
}
static uds_status_t t_read_pdid(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    (void)buf_len;
    buf[0]   = 0x5AU;
    *out_len = 1U;
    return UDS_STATUS_OK;
}

static const uds_exec_producer_cfg_t k_producers[2] = {
    { .src = SRC_CAN,  .periodic_tx = true,  .notify = f_notify, .notify_arg = (void *)(uintptr_t)IDX_CAN },
    { .src = SRC_DOIP, .periodic_tx = false, .notify = f_notify, .notify_arg = (void *)(uintptr_t)IDX_DOIP },
};

static uds_exec_cfg_t exec_cfg(void)
{
    uds_exec_cfg_t c = {
        .srv = &g_srv, .now_ms = f_now, .enter = f_enter, .exit = f_exit,
        .wake = f_wake, .wake_arg = NULL, .reset = f_reset,
        .producers = k_producers, .producer_count = 2U,
    };
    return c;
}

/* Fresh server, DID database, periodic scheduler and executor. */
static void setup(void)
{
    static bool dids_done;
    static const uds_security_cfg_t sc = {
        .max_attempts = 3U, .lockout_ms = 100U,
        .key_validate_cb = t_key, .seed_generate_cb = t_seed,
    };
    static const uds_server_cfg_t svc = {
        .p2_server_max_ms = 50U, .p2_star_server_max_ms = 5000U,
        .session_ctx = &g_sess, .security_ctx = &g_sec,
        .service_table = k_table, .service_table_count = 6U,
        .access_table = k_acl, .access_table_count = 6U,
    };
    uds_exec_cfg_t c = exec_cfg();

    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_sec, 0, sizeof(g_sec));
    memset(&g_srv, 0, sizeof(g_srv));
    (void)uds_session_init(&g_sess, 5000U);
    (void)uds_security_init(&g_sec, &sc);
    (void)uds_server_init(&g_srv, &svc);
    (void)uds_periodic_init();
    uds_periodic_set_tx_can_id(0x7E9U);
    if (!dids_done) {
        static const did_entry_t pdid = {
            .did_id = 0xF2ABU, .access_flags = (uint8_t)DID_ACCESS_READ,
            .min_session = (uint8_t)UDS_SESSION_EXTENDED, .data_length = 1U,
            .read_cb = t_read_pdid, .description = "test pDID",
        };
        (void)did_database_init();
        (void)did_database_register(&pdid);
        dids_done = true;
    }

    g_now = 1000U;
    g_depth = 0; g_depth_max = 0;
    g_wakes = 0U; g_notifies[0] = 0U; g_notifies[1] = 0U;
    g_resets = 0U; g_reset_type = 0U;
    g_job_left = 0U; g_job_steps = 0U; g_job_aborts = 0U; g_in_step = NULL;
    g_order_n = 0U;
    zassert_equal(uds_exec_init(&g_ex, &c), UDS_STATUS_OK, "exec init");
}

/* =========================================================================
 * Producers
 * ========================================================================= */

typedef struct {
    uint8_t       src;
    uds_msg_buf_t req;
    uds_msg_buf_t resp;
    uds_msg_buf_t got[4];   /* responses and pushed frames, in order */
    uint8_t       got_n;
} prod_t;

static prod_t g_p[2];

static void prod_reset(void)
{
    memset(g_p, 0, sizeof(g_p));
    g_p[IDX_CAN].src  = SRC_CAN;
    g_p[IDX_DOIP].src = SRC_DOIP;
}

static void got(prod_t *p, const uds_msg_buf_t *f)
{
    if ((f->length > 0U) && (p->got_n < 4U)) {
        p->got[p->got_n] = *f;
        p->got_n++;
    }
}

static uds_exec_submit_result_t submit(prod_t *p, const uint8_t *b, uint16_t n,
                                       uds_addressing_t a)
{
    memcpy(p->req.data, b, n);
    p->req.length = n;
    return uds_exec_submit(&g_ex, p->src, &p->req, &p->resp, a);
}

/* What a producer does when notified: take its response, send pushed frames. */
static void prod_service(prod_t *p)
{
    const uds_msg_buf_t *f;

    if (uds_exec_done(&g_ex, p->src)) {
        got(p, &p->resp);
        uds_exec_release(&g_ex, p->src);
    }
    f = uds_exec_push_frame(&g_ex, p->src);
    if (f != NULL) {
        got(p, f);
        uds_exec_push_done(&g_ex, p->src, true);
    }
}

static void service_all(void)
{
    prod_service(&g_p[IDX_CAN]);
    prod_service(&g_p[IDX_DOIP]);
}

/* Executor passes, producers serviced after each, until nothing is left. */
static void drain(void)
{
    uint32_t guard = 0U;
    bool     more  = true;

    while (more && (guard < 50U)) {
        more = uds_exec_run_once(&g_ex);
        service_all();
        more = more || (uds_exec_push_frame(&g_ex, SRC_CAN) != NULL) ||
               (uds_exec_push_frame(&g_ex, SRC_DOIP) != NULL);
        guard++;
    }
    zassert_true(guard < 50U, "executor did not settle");
}

static bool frame_is(const uds_msg_buf_t *f, const uint8_t *b, uint16_t n)
{
    return (f->length == n) && (memcmp(f->data, b, n) == 0);
}

/* =========================================================================
 * Every order: the test scheduler
 * ========================================================================= */

enum { EV_A_SUB = 0, EV_B_SUB = 1, EV_TICK = 2, EV_RUN = 3 };

/* Next lexicographic permutation of ev[0..n-1]; false after the last. */
static bool next_perm(uint8_t *ev, uint8_t n)
{
    int i = (int)n - 2;
    int j;
    uint8_t t;

    while ((i >= 0) && (ev[i] >= ev[i + 1])) { i--; }
    if (i < 0) { return false; }
    j = (int)n - 1;
    while (ev[j] <= ev[i]) { j--; }
    t = ev[i]; ev[i] = ev[j]; ev[j] = t;
    for (int a = i + 1, b = (int)n - 1; a < b; a++, b--) {
        t = ev[a]; ev[a] = ev[b]; ev[b] = t;
    }
    return true;
}

static const uint8_t k_10_03[] = { 0x10U, 0x03U };
static const uint8_t k_3e_00[] = { 0x3EU, 0x00U };
static const uint8_t k_31[]    = { 0x31U, 0x01U, 0xF0U, 0x00U };

static void play(const uint8_t *ev, uint8_t n, const uint8_t *a_req, uint16_t a_len)
{
    uint8_t i;

    for (i = 0U; i < n; i++) {
        switch (ev[i]) {
        case EV_A_SUB:
            zassert_equal(submit(&g_p[IDX_CAN], a_req, a_len, UDS_ADDRESSING_PHYSICAL),
                          UDS_EXEC_POSTED, "A posted");
            break;
        case EV_B_SUB:
            zassert_equal(submit(&g_p[IDX_DOIP], k_3e_00, 2U, UDS_ADDRESSING_PHYSICAL),
                          UDS_EXEC_POSTED, "B posted");
            break;
        case EV_TICK:
            g_now++;
            break;
        default:
            (void)uds_exec_run_once(&g_ex);
            service_all();
            break;
        }
    }
    drain();
    zassert_equal(g_depth, 0, "critical section left open");
    zassert_equal(g_depth_max, 1, "critical section nested");
}

/** TC-EXEC-ORD-001: CAN opens 10 03, DoIP sends 3E 00, a clock tick and two
 *  executor passes, in all 60 orders. CAN always gets 50 03 and owns the
 *  session; DoIP gets exactly one answer: 7E 00 if it was dispatched first,
 *  7F 3E 21 (owned by CAN) otherwise; never silence, never both. */
ZTEST(uds_exec_suite, test_every_order_session_and_tester_present)
{
    uint8_t  ev[] = { EV_A_SUB, EV_B_SUB, EV_TICK, EV_RUN, EV_RUN };
    uint32_t orders = 0U;
    uint32_t b_first = 0U;
    uint32_t b_refused = 0U;
    static const uint8_t pos[] = { 0x7EU, 0x00U };
    static const uint8_t busy[] = { 0x7FU, 0x3EU, 0x21U };
    uds_session_type_t s;

    do {
        setup();
        prod_reset();
        play(ev, 5U, k_10_03, 2U);
        orders++;

        zassert_equal(g_p[IDX_CAN].got_n, 1U, "CAN one answer");
        zassert_equal(g_p[IDX_CAN].got[0].data[0], 0x50U, "CAN 50 03");
        zassert_equal(g_p[IDX_DOIP].got_n, 1U, "DoIP exactly one answer");
        if (frame_is(&g_p[IDX_DOIP].got[0], pos, 2U)) {
            b_first++;
        } else {
            zassert_true(frame_is(&g_p[IDX_DOIP].got[0], busy, 3U), "DoIP: 7E 00 or 7F 3E 21");
            b_refused++;
        }
        (void)uds_session_get_active(&g_sess, &s);
        zassert_equal(s, UDS_SESSION_EXTENDED, "CAN's session");
        zassert_equal(g_srv.request_count, 2U, "server counted both");
    } while (next_perm(ev, 5U));

    zassert_equal(orders, 60U, "all orders run");
    zassert_true((b_first > 0U) && (b_refused > 0U), "both outcomes reached");
}

/** TC-EXEC-ORD-002: CAN starts a 2-step job (31 01 F0 00), DoIP sends 3E 00,
 *  four executor passes, all 30 orders. CAN gets 7F 31 78 then 71 01 F0 00,
 *  both routed to CAN; DoIP gets one answer (7E 00, or 7F 3E 21 while the
 *  job is pending) and never one of the job's frames. */
ZTEST(uds_exec_suite, test_every_order_job_and_other_request)
{
    uint8_t  ev[] = { EV_A_SUB, EV_B_SUB, EV_RUN, EV_RUN, EV_RUN, EV_RUN };
    uint32_t orders = 0U;
    static const uint8_t k78[]   = { 0x7FU, 0x31U, 0x78U };
    static const uint8_t final[] = { 0x71U, 0x01U, 0xF0U, 0x00U };

    do {
        setup();
        prod_reset();
        g_job_left = 2U;
        play(ev, 6U, k_31, 4U);
        orders++;

        zassert_equal(g_p[IDX_CAN].got_n, 2U, "CAN: 78 then final");
        zassert_true(frame_is(&g_p[IDX_CAN].got[0], k78, 3U), "CAN 7F 31 78");
        zassert_true(frame_is(&g_p[IDX_CAN].got[1], final, 4U), "CAN final via push");
        zassert_equal(g_p[IDX_DOIP].got_n, 1U, "DoIP one answer");
        zassert_true((g_p[IDX_DOIP].got[0].data[0] == 0x7EU) ||
                     (g_p[IDX_DOIP].got[0].data[2] == 0x21U), "DoIP 7E or 0x21");
        zassert_equal(g_job_aborts, 0U, "job not aborted");
    } while (next_perm(ev, 6U));

    zassert_equal(orders, 30U, "all orders run");
}

/* =========================================================================
 * busy_src fast reject
 * ========================================================================= */

static uds_exec_submit_result_t g_in_step_phys;
static uds_exec_submit_result_t g_in_step_func;
static uds_exec_submit_result_t g_in_step_own;

static void submit_inside_step(void)
{
    static uds_msg_buf_t r2;
    static uds_msg_buf_t s2;

    g_in_step_phys = submit(&g_p[IDX_DOIP], k_3e_00, 2U, UDS_ADDRESSING_PHYSICAL);
    r2.data[0] = 0x3EU; r2.data[1] = 0x80U; r2.length = 2U;
    g_in_step_func = uds_exec_submit(&g_ex, SRC_DOIP, &r2, &s2, UDS_ADDRESSING_FUNCTIONAL);
    g_in_step_own  = submit(&g_p[IDX_CAN], k_3e_00, 2U, UDS_ADDRESSING_PHYSICAL);
    g_in_step = NULL;
}

/** TC-EXEC-BUSY-001: while CAN's job runs a step, a DoIP physical request
 *  is answered 0x21 by the producer (REJECT_BUSY) and a functional one is
 *  dropped, without waiting for the step; the job's own source is not
 *  blocked. After the step, DoIP posts normally. */
ZTEST(uds_exec_suite, test_request_during_job_step_is_rejected_fast)
{
    uds_exec_stats_t st;

    setup();
    prod_reset();
    g_job_left = 3U;
    zassert_equal(submit(&g_p[IDX_CAN], k_31, 4U, UDS_ADDRESSING_PHYSICAL), UDS_EXEC_POSTED, "");
    (void)uds_exec_run_once(&g_ex);              /* dispatch: 7F 31 78 */
    service_all();
    g_in_step = submit_inside_step;
    (void)uds_exec_run_once(&g_ex);              /* first step */

    zassert_equal(g_in_step_phys, UDS_EXEC_REJECT_BUSY, "physical: 0x21 now");
    zassert_equal(g_in_step_func, UDS_EXEC_DROPPED, "functional: dropped");
    zassert_equal(g_in_step_own, UDS_EXEC_POSTED, "the job's source is not fast-rejected");
    zassert_equal(submit(&g_p[IDX_DOIP], k_3e_00, 2U, UDS_ADDRESSING_PHYSICAL),
                  UDS_EXEC_POSTED, "after the step: posted");
    (void)uds_exec_get_stats(&g_ex, &st);
    zassert_equal(st.reject_busy, 1U, "one fast reject");
    zassert_equal(st.dropped, 1U, "one drop");
    drain();
}

/** TC-EXEC-BUSY-002: one slot per producer. A second physical request
 *  before the first is released gets REJECT_BUSY; functional 3E 80 is
 *  dropped. Neither reaches the server. */
ZTEST(uds_exec_suite, test_backpressure_physical_0x21_functional_dropped)
{
    static const uint8_t tp80[] = { 0x3EU, 0x80U };
    static uds_msg_buf_t r2;
    static uds_msg_buf_t s2;

    setup();
    prod_reset();
    zassert_equal(submit(&g_p[IDX_CAN], k_3e_00, 2U, UDS_ADDRESSING_PHYSICAL), UDS_EXEC_POSTED, "");
    memcpy(r2.data, k_3e_00, 2U); r2.length = 2U;
    zassert_equal(uds_exec_submit(&g_ex, SRC_CAN, &r2, &s2, UDS_ADDRESSING_PHYSICAL),
                  UDS_EXEC_REJECT_BUSY, "second physical");
    memcpy(r2.data, tp80, 2U);
    zassert_equal(uds_exec_submit(&g_ex, SRC_CAN, &r2, &s2, UDS_ADDRESSING_FUNCTIONAL),
                  UDS_EXEC_DROPPED, "functional 3E 80");
    (void)uds_exec_run_once(&g_ex);
    zassert_true(uds_exec_done(&g_ex, SRC_CAN), "first done");
    /* Still busy until released. */
    zassert_equal(uds_exec_submit(&g_ex, SRC_CAN, &r2, &s2, UDS_ADDRESSING_PHYSICAL),
                  UDS_EXEC_REJECT_BUSY, "until release");
    uds_exec_release(&g_ex, SRC_CAN);
    zassert_equal(g_srv.request_count, 1U, "only one reached the server");
    zassert_equal(uds_exec_submit(&g_ex, 7U, &r2, &s2, UDS_ADDRESSING_PHYSICAL),
                  UDS_EXEC_INVALID, "unknown source");
}

/* =========================================================================
 * Routing by source
 * ========================================================================= */

/** TC-EXEC-ROUTE-001: a job's frames go only to the source that started it;
 *  a frame reported unsent is offered again, and the server is told only
 *  once it is sent. */
ZTEST(uds_exec_suite, test_job_frames_go_to_job_src_and_retry_when_unsent)
{
    const uds_msg_buf_t *f;

    setup();
    prod_reset();
    g_job_left = 0U;   /* the first step finishes */
    zassert_equal(submit(&g_p[IDX_CAN], k_31, 4U, UDS_ADDRESSING_PHYSICAL), UDS_EXEC_POSTED, "");
    (void)uds_exec_run_once(&g_ex);   /* dispatch + first poll: final frame */
    zassert_true(uds_exec_done(&g_ex, SRC_CAN), "78 ready");
    uds_exec_release(&g_ex, SRC_CAN);

    f = uds_exec_push_frame(&g_ex, SRC_CAN);
    zassert_not_null(f, "final routed to CAN");
    zassert_equal(f->data[0], 0x71U, "71 01 F0 00");
    zassert_is_null(uds_exec_push_frame(&g_ex, SRC_DOIP), "not to DoIP");

    uds_exec_push_done(&g_ex, SRC_CAN, false);       /* transport busy */
    (void)uds_exec_run_once(&g_ex);
    f = uds_exec_push_frame(&g_ex, SRC_CAN);
    zassert_not_null(f, "offered again");
    zassert_equal(f->data[0], 0x71U, "same frame");
    uds_exec_push_done(&g_ex, SRC_CAN, true);
    (void)uds_exec_run_once(&g_ex);
    (void)uds_exec_run_once(&g_ex);
    zassert_is_null(uds_exec_push_frame(&g_ex, SRC_CAN), "job over");
    zassert_false(uds_exec_run_once(&g_ex), "idle");
}

static const uint8_t k_2a_fast_ab[] = { 0x2AU, 0x03U, 0xABU };

/** TC-EXEC-ROUTE-002: a periodic subscription records its source; its
 *  frames are pushed to that producer only (decision ⬥E). */
ZTEST(uds_exec_suite, test_periodic_frames_go_to_subscriber)
{
    static const uint8_t frame[] = { 0xABU, 0x5AU };
    uint32_t i;
    uint32_t to_can = 0U;

    setup();
    prod_reset();
    (void)submit(&g_p[IDX_CAN], k_10_03, 2U, UDS_ADDRESSING_PHYSICAL);
    drain();
    (void)submit(&g_p[IDX_CAN], k_2a_fast_ab, 3U, UDS_ADDRESSING_PHYSICAL);
    drain();
    zassert_equal(g_p[IDX_CAN].got[1].data[0], 0x6AU, "6A");

    for (i = 0U; i < 25U; i++) {      /* 2.5 fast periods */
        const uds_msg_buf_t *f;
        g_now++;
        (void)uds_exec_run_once(&g_ex);
        zassert_is_null(uds_exec_push_frame(&g_ex, SRC_DOIP), "never to DoIP");
        f = uds_exec_push_frame(&g_ex, SRC_CAN);
        if (f != NULL) {
            zassert_true(frame_is(f, frame, 2U), "AB 5A");
            to_can++;
            uds_exec_push_done(&g_ex, SRC_CAN, true);
        }
    }
    zassert_true(to_can >= 2U, "periodic frames reached CAN");
}

/** TC-EXEC-ROUTE-003: ⬥E. The DoIP producer has no periodic TX: its 0x2A
 *  schedule request gets 7F 2A 22 and nothing is scheduled. */
ZTEST(uds_exec_suite, test_0x2a_from_producer_without_periodic_tx_gets_0x22)
{
    static const uint8_t nrc[] = { 0x7FU, 0x2AU, 0x22U };

    setup();
    prod_reset();
    (void)submit(&g_p[IDX_DOIP], k_10_03, 2U, UDS_ADDRESSING_PHYSICAL);
    drain();
    (void)submit(&g_p[IDX_DOIP], k_2a_fast_ab, 3U, UDS_ADDRESSING_PHYSICAL);
    drain();
    zassert_true(frame_is(&g_p[IDX_DOIP].got[1], nrc, 3U), "7F 2A 22");
    zassert_false(uds_periodic_is_subscribed(0xABU), "nothing scheduled");
    zassert_false(uds_periodic_src_can_tx(SRC_DOIP), "mask from producers");
    zassert_true(uds_periodic_src_can_tx(SRC_CAN), "CAN can");
}

/** TC-EXEC-ROUTE-004: a frame for a source no producer has is dropped and
 *  counted, not sent to another producer. */
ZTEST(uds_exec_suite, test_periodic_for_unknown_src_is_dropped)
{
    uds_exec_stats_t st;
    uint32_t i;

    setup();
    prod_reset();
    (void)uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    zassert_equal(uds_periodic_subscribe_src(0xABU, UDS_PERIODIC_MODE_FAST, 5U), UDS_STATUS_OK, "");
    for (i = 0U; i < 12U; i++) {
        g_now++;
        (void)uds_exec_run_once(&g_ex);
        zassert_is_null(uds_exec_push_frame(&g_ex, SRC_CAN), "not CAN");
        zassert_is_null(uds_exec_push_frame(&g_ex, SRC_DOIP), "not DoIP");
    }
    (void)uds_exec_get_stats(&g_ex, &st);
    zassert_true(st.periodic_dropped >= 1U, "counted");
}

/* =========================================================================
 * ECUReset
 * ========================================================================= */

/** TC-EXEC-RESET-001: 11 01's reset waits until its source has released
 *  (sent) the 51 01 response; no other request is served meanwhile. */
ZTEST(uds_exec_suite, test_reset_waits_for_release)
{
    static const uint8_t k_11_01[] = { 0x11U, 0x01U };

    setup();
    prod_reset();
    (void)submit(&g_p[IDX_DOIP], k_11_01, 2U, UDS_ADDRESSING_PHYSICAL);
    (void)uds_exec_run_once(&g_ex);
    zassert_true(uds_exec_done(&g_ex, SRC_DOIP), "answered");
    zassert_equal(g_p[IDX_DOIP].resp.data[0], 0x51U, "51 01");

    (void)submit(&g_p[IDX_CAN], k_3e_00, 2U, UDS_ADDRESSING_PHYSICAL);
    (void)uds_exec_run_once(&g_ex);
    (void)uds_exec_run_once(&g_ex);
    zassert_equal(g_resets, 0U, "not before the response is sent");
    zassert_false(uds_exec_done(&g_ex, SRC_CAN), "nothing served while a reset is owed");

    uds_exec_release(&g_ex, SRC_DOIP);
    zassert_equal(g_resets, 0U, "not on the producer's thread");
    (void)uds_exec_run_once(&g_ex);
    zassert_equal(g_resets, 1U, "reset once");
    zassert_equal(g_reset_type, 0x01U, "hardReset");
    (void)uds_exec_run_once(&g_ex);
    zassert_equal(g_resets, 1U, "only once");
}

/** TC-EXEC-REL-001: contract 1. A producer that gives up on a posted
 *  request (a timeout, a disconnect) releases it like any other exit. Not
 *  yet run, the request is withdrawn: it never runs, so it owes no reset,
 *  and the executor goes on serving the other producer. */
ZTEST(uds_exec_suite, test_release_before_run_withdraws_the_request)
{
    static const uint8_t k_11_01[] = { 0x11U, 0x01U };

    setup();
    prod_reset();
    (void)submit(&g_p[IDX_DOIP], k_11_01, 2U, UDS_ADDRESSING_PHYSICAL);
    uds_exec_release(&g_ex, SRC_DOIP);           /* timed out before the pass */
    (void)uds_exec_run_once(&g_ex);
    (void)submit(&g_p[IDX_CAN], k_3e_00, 2U, UDS_ADDRESSING_PHYSICAL);
    (void)uds_exec_run_once(&g_ex);
    (void)uds_exec_run_once(&g_ex);
    zassert_true(uds_exec_done(&g_ex, SRC_CAN), "the executor still serves CAN");
    zassert_equal(g_resets, 0U, "never ran, so no reset");
    zassert_equal(g_p[IDX_DOIP].resp.length, 0U, "nothing written to its buffers");
    uds_exec_release(&g_ex, SRC_CAN);
    zassert_equal(submit(&g_p[IDX_DOIP], k_3e_00, 2U, UDS_ADDRESSING_PHYSICAL),
                  UDS_EXEC_POSTED, "DoIP's slot is free again");
}

static bool                     g_drop_armed;
static uds_exec_submit_result_t g_drop_submit;
static uds_msg_buf_t            g_rx2;   /* the producer's second receive buffer */
static uds_msg_buf_t            g_tx2;

/* DoIP drops while its 11 01 runs: it releases (contract 1), receives the
 * next request into a second buffer and tries to post it (contract 2). */
static void drop_doip_while_running(uint32_t id)
{
    if ((id == EDS_RACE_EXEC_RAN) && g_drop_armed) {
        g_drop_armed = false;
        uds_exec_release(&g_ex, SRC_DOIP);
        g_rx2.data[0] = 0x3EU;
        g_rx2.data[1] = 0x00U;
        g_rx2.length  = 2U;
        g_drop_submit = uds_exec_submit(&g_ex, SRC_DOIP, &g_rx2, &g_tx2,
                                        UDS_ADDRESSING_PHYSICAL);
    }
}

/** TC-EXEC-REL-002: contracts 1 and 2. A producer that drops while its
 *  11 01 runs releases at once. The executor finishes with that slot's
 *  buffers, frees it without a DONE (no one is left to send), performs
 *  the reset the request owes and serves the other producer: the reset
 *  owed to a dropped producer does not stall it. Until then the slot is
 *  not free, so the producer's next request (in its second buffer) is
 *  refused and the executor writes only the first buffers. */
ZTEST(uds_exec_suite, test_reset_owed_to_dropped_producer_does_not_stall)
{
    static const uint8_t k_11_01[] = { 0x11U, 0x01U };

    setup();
    prod_reset();
    memset(&g_rx2, 0, sizeof(g_rx2));
    memset(&g_tx2, 0, sizeof(g_tx2));
    g_drop_armed  = true;
    g_drop_submit = UDS_EXEC_INVALID;
    eds_race_point_hook = drop_doip_while_running;
    (void)submit(&g_p[IDX_DOIP], k_11_01, 2U, UDS_ADDRESSING_PHYSICAL);
    (void)uds_exec_run_once(&g_ex);
    eds_race_point_hook = NULL;

    zassert_false(g_drop_armed, "the producer dropped while the request ran");
    zassert_equal(g_drop_submit, UDS_EXEC_REJECT_BUSY, "slot still the executor's");
    zassert_equal(g_p[IDX_DOIP].resp.data[0], 0x51U, "51 01 in the first buffer");
    zassert_equal(g_tx2.length, 0U, "the second buffer untouched");
    zassert_false(uds_exec_done(&g_ex, SRC_DOIP), "freed, nothing left to send");

    (void)submit(&g_p[IDX_CAN], k_3e_00, 2U, UDS_ADDRESSING_PHYSICAL);
    (void)uds_exec_run_once(&g_ex);
    zassert_equal(g_resets, 1U, "the owed reset is performed");
    (void)uds_exec_run_once(&g_ex);
    zassert_true(uds_exec_done(&g_ex, SRC_CAN), "and CAN is served");
    uds_exec_release(&g_ex, SRC_CAN);
    zassert_equal(uds_exec_submit(&g_ex, SRC_DOIP, &g_rx2, &g_tx2, UDS_ADDRESSING_PHYSICAL),
                  UDS_EXEC_POSTED, "DoIP posts its second buffer now");
}

/* =========================================================================
 * Calls, order, counters, configuration
 * ========================================================================= */

static uds_server_ctx_t *g_call_srv;
static uint32_t          g_call_runs;

static void call_end_doip_session(uds_server_ctx_t *srv, void *arg)
{
    g_call_srv = srv;
    g_call_runs++;
    *(bool *)arg = uds_server_end_session(srv, SRC_DOIP);
}

/** TC-EXEC-CALL-001: uds_exec_call() runs its function on the executor
 *  pass, with the server, and completes through the producer's slot. */
ZTEST(uds_exec_suite, test_call_runs_on_executor)
{
    static bool ended;
    uds_session_type_t s;

    setup();
    prod_reset();
    (void)submit(&g_p[IDX_DOIP], k_10_03, 2U, UDS_ADDRESSING_PHYSICAL);
    drain();
    g_call_runs = 0U;
    zassert_equal(uds_exec_call(&g_ex, SRC_DOIP, call_end_doip_session, &ended), UDS_STATUS_OK, "");
    zassert_equal(uds_exec_call(&g_ex, SRC_DOIP, call_end_doip_session, &ended),
                  UDS_STATUS_ERR_BUSY, "slot in use");
    zassert_equal(g_call_runs, 0U, "not on the caller's thread");
    (void)uds_exec_run_once(&g_ex);
    zassert_equal(g_call_runs, 1U, "ran on the pass");
    zassert_true(g_call_srv == &g_srv, "with the server");
    zassert_true(ended, "DoIP owned it");
    zassert_true(uds_exec_done(&g_ex, SRC_DOIP), "done");
    uds_exec_release(&g_ex, SRC_DOIP);
    (void)uds_session_get_active(&g_sess, &s);
    zassert_equal(s, UDS_SESSION_DEFAULT, "session ended");
}

/** TC-EXEC-RR-001: one request per slot per pass, the starting slot
 *  rotating; max_queue_wait_ms is submit-to-dispatch on the clock. */
ZTEST(uds_exec_suite, test_round_robin_and_queue_wait)
{
    static const uint8_t ba[] = { 0xBAU, 0x00U };
    uds_exec_stats_t st;

    setup();
    prod_reset();
    (void)submit(&g_p[IDX_CAN], ba, 2U, UDS_ADDRESSING_PHYSICAL);
    (void)submit(&g_p[IDX_DOIP], ba, 2U, UDS_ADDRESSING_PHYSICAL);
    g_now += 7U;
    (void)uds_exec_run_once(&g_ex);          /* pass 1 starts at CAN */
    service_all();
    (void)submit(&g_p[IDX_CAN], ba, 2U, UDS_ADDRESSING_PHYSICAL);
    (void)submit(&g_p[IDX_DOIP], ba, 2U, UDS_ADDRESSING_PHYSICAL);
    (void)uds_exec_run_once(&g_ex);          /* pass 2 starts at DoIP */
    service_all();

    zassert_equal(g_order_n, 4U, "four dispatches in two passes");
    zassert_equal(g_order[0], SRC_CAN, "pass 1: CAN");
    zassert_equal(g_order[1], SRC_DOIP, "pass 1: DoIP");
    zassert_equal(g_order[2], SRC_DOIP, "pass 2: DoIP first");
    zassert_equal(g_order[3], SRC_CAN, "pass 2: CAN");
    (void)uds_exec_get_stats(&g_ex, &st);
    zassert_equal(st.served, 4U, "served");
    zassert_equal(st.max_queue_wait_ms, 7U, "waited 7 ms");
    zassert_true(g_wakes >= 4U, "producers woke the executor");
    zassert_equal(g_notifies[IDX_CAN], 2U, "CAN notified per response");
}

/** TC-EXEC-INIT-001: configurations the executor refuses. */
ZTEST(uds_exec_suite, test_init_rejects_bad_cfg)
{
    uds_exec_producer_cfg_t p[5];
    uds_exec_cfg_t c;
    uds_exec_t     ex;

    setup();
    memset(p, 0, sizeof(p));
    c = exec_cfg();
    zassert_equal(uds_exec_init(NULL, &c), UDS_STATUS_ERR_NULL_PTR, "");
    zassert_equal(uds_exec_init(&ex, NULL), UDS_STATUS_ERR_NULL_PTR, "");
    c.enter = NULL;
    zassert_equal(uds_exec_init(&ex, &c), UDS_STATUS_ERR_INVALID_PARAM, "no critical section");
    c = exec_cfg();
    c.producers = p; c.producer_count = 2U;
    p[0].src = 3U; p[1].src = 3U;
    zassert_equal(uds_exec_init(&ex, &c), UDS_STATUS_ERR_INVALID_PARAM, "repeated source");
    p[1].src = 8U;
    zassert_equal(uds_exec_init(&ex, &c), UDS_STATUS_ERR_INVALID_PARAM, "source above 7");
    c.producer_count = 0U;
    zassert_equal(uds_exec_init(&ex, &c), UDS_STATUS_ERR_INVALID_PARAM, "no producer");
    c.producer_count = (uint8_t)(EDS_EXEC_MAX_PRODUCERS + 1U);
    zassert_equal(uds_exec_init(&ex, &c), UDS_STATUS_ERR_INVALID_PARAM, "too many");
    zassert_false(uds_exec_run_once(&ex), "uninitialized: no-op");
}

ZTEST_SUITE(uds_exec_suite, NULL, NULL, NULL, NULL, NULL);

/* =========================================================================
 * Unity runner infrastructure
 * ========================================================================= */

void setUp(void)    { }
void tearDown(void) { }

void run_all_tests(void)
{
    RUN_TEST(uds_exec_suite__test_every_order_session_and_tester_present);
    RUN_TEST(uds_exec_suite__test_every_order_job_and_other_request);
    RUN_TEST(uds_exec_suite__test_request_during_job_step_is_rejected_fast);
    RUN_TEST(uds_exec_suite__test_backpressure_physical_0x21_functional_dropped);
    RUN_TEST(uds_exec_suite__test_job_frames_go_to_job_src_and_retry_when_unsent);
    RUN_TEST(uds_exec_suite__test_periodic_frames_go_to_subscriber);
    RUN_TEST(uds_exec_suite__test_0x2a_from_producer_without_periodic_tx_gets_0x22);
    RUN_TEST(uds_exec_suite__test_periodic_for_unknown_src_is_dropped);
    RUN_TEST(uds_exec_suite__test_reset_waits_for_release);
    RUN_TEST(uds_exec_suite__test_release_before_run_withdraws_the_request);
    RUN_TEST(uds_exec_suite__test_reset_owed_to_dropped_producer_does_not_stall);
    RUN_TEST(uds_exec_suite__test_call_runs_on_executor);
    RUN_TEST(uds_exec_suite__test_round_robin_and_queue_wait);
    RUN_TEST(uds_exec_suite__test_init_rejects_bad_cfg);
}
