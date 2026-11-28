// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — host concurrency test (ThreadSanitizer)
 * FILE: tests/concurrency/tsan_doip_exec_states.c
 * MODULE UNDER TEST: transport/doip/doip_server.c on the executor (EDS#197 J6)
 *
 * PURPOSE: A DoIP disconnect in every state of its executor slot leaves the
 *   executor serving CAN (uds_exec.h contract 1: release on every exit path).
 *   Three real threads:
 *     - the DoIP thread runs eds_doip_server_run() with eds_doip_set_executor()
 *       and scripted platform ops; each connection activates routing, sends
 *       one request and drops;
 *     - the executor thread runs uds_exec_run_once(), woken by producers and
 *       every 1 ms;
 *     - the main thread arms each scenario, then acts as the CAN producer
 *       (source 0): 3E 00 must come back 7E 00 after every scenario.
 *   Scenarios, one connection each, by the state the DoIP request is in when
 *   the connection goes:
 *     A posted:     the executor is held; DoIP times out (DOIP_EXEC_WAIT_MS)
 *                   and withdraws the request, which never runs;
 *     B running:    the request's handler is still running when DoIP times
 *                   out; the executor frees the slot when it finishes;
 *     C done:       10 03 ran, its response send fails; the disconnect ends
 *                   DoIP's session, so CAN is not refused 0x21;
 *     D reset owed: 11 01 ran, its response send fails; the executor still
 *                   performs the reset and goes on serving;
 *     E job:        a deferred 31 01's final frame fails to send and is
 *                   offered to DoIP again before the disconnect; the
 *                   executor drops it, and a CAN job then runs to its own
 *                   final frame (the push mailbox is free).
 *   Time: the platform clock is the monotonic clock plus a skew. While the
 *   executor is held (A, B) the DoIP wait advances the skew by its timeout
 *   instead of sleeping, so DOIP_EXEC_WAIT_MS passes at once. Any data race,
 *   a failed check or a hang (the main thread gives up after 10 s) fails it.
 *
 * CI: the concurrency-tsan job (EDS#197 J1).
 * =============================================================================
 */

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "doip_server.h"
#include "uds_exec.h"
#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_comm_control.h"
#include "uds_periodic.h"

#define ECU_ADDR     (0x0E00U)
#define TESTER_ADDR  (0x0E80U)
#define RECV_DEAD    (-104)   /* ECONNRESET, as a real binding reports it */
#define SRC_CAN      (0U)
#define IDX_CAN      (0U)
#define IDX_DOIP     (1U)

static uds_session_ctx_t   g_sess;
static uds_security_ctx_t  g_sec;
static uds_server_ctx_t    g_srv;
static uds_exec_t          g_ex;
static doip_server_state_t g_doip;
static int                 g_fail;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL scenario %c: %s (line %d)\n", s_name, (what), __LINE__); \
            g_fail = 1;                                                     \
        }                                                                   \
    } while (0)

/* ---- shared state: s_ev guards everything below ------------------------- */

static pthread_mutex_t s_ev = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;
static uint32_t s_wake_seq;     /* executor wakes                          */
static uint32_t s_sig[2];       /* per-producer notifications              */
static int      s_paused;       /* scenario A: the executor runs no pass   */
static int      s_armed;        /* main armed a connection                 */
static int      s_idle;         /* DoIP is back in accept                  */
static int      s_closed;       /* the scenario's connection was closed    */
static int      s_handler_go;   /* scenario B: the handler may finish      */
static int      s_stop;
static char     s_name = '-';

/* Read by several threads: __atomic builtins only. */
static int      s_held;         /* DoIP waits advance the clock (A, B)     */
static uint32_t s_skew_ms;

static uint32_t now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000)) +
           __atomic_load_n(&s_skew_ms, __ATOMIC_ACQUIRE);
}

/* Wait on s_cv (s_ev held) for at most ms of real time. */
static void cv_wait_ms(uint32_t ms)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec  += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    (void)pthread_cond_timedwait(&s_cv, &s_ev, &ts);
}

/* ---- executor bindings --------------------------------------------------- */

static pthread_mutex_t s_cs = PTHREAD_MUTEX_INITIALIZER;
static void cs_enter(void) { (void)pthread_mutex_lock(&s_cs); }
static void cs_exit(void)  { (void)pthread_mutex_unlock(&s_cs); }

static void ex_wake(void *arg)
{
    (void)arg;
    (void)pthread_mutex_lock(&s_ev);
    s_wake_seq++;
    (void)pthread_cond_broadcast(&s_cv);
    (void)pthread_mutex_unlock(&s_ev);
}

static void ex_notify(void *arg)
{
    (void)pthread_mutex_lock(&s_ev);
    s_sig[(uintptr_t)arg]++;
    (void)pthread_cond_broadcast(&s_cv);
    (void)pthread_mutex_unlock(&s_ev);
}

static uint32_t s_resets;   /* executor thread only, read after joins/waits */
static void ex_reset(uint8_t type)
{
    (void)type;
    __atomic_add_fetch(&s_resets, 1U, __ATOMIC_ACQ_REL);
}

static const uds_exec_producer_cfg_t k_producers[2] = {
    { .src = SRC_CAN,                    .periodic_tx = true,
      .notify = ex_notify, .notify_arg = (void *)(uintptr_t)IDX_CAN },
    { .src = (uint8_t)EDS_DOIP_UDS_SRC,  .periodic_tx = false,
      .notify = ex_notify, .notify_arg = (void *)(uintptr_t)IDX_DOIP },
};

static void *exec_thread(void *arg)
{
    uint32_t seen = 0U;
    int      run;

    (void)arg;
    for (;;) {
        (void)pthread_mutex_lock(&s_ev);
        if ((s_wake_seq == seen) && !s_stop) {
            cv_wait_ms(1U);              /* the 1 ms tick */
        }
        seen = s_wake_seq;
        run  = !s_paused;
        if (s_stop) {
            (void)pthread_mutex_unlock(&s_ev);
            break;
        }
        (void)pthread_mutex_unlock(&s_ev);
        if (run) {
            while (uds_exec_run_once(&g_ex)) {
            }
        }
    }
    return NULL;
}

/* eds_doip_exec_wait_cb_t: the DoIP thread's wait. */
static bool doip_wait(uint8_t src, uint32_t timeout_ms)
{
    uint32_t seen;
    bool     got;

    (void)src;
    if (__atomic_load_n(&s_held, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&s_skew_ms, timeout_ms, __ATOMIC_ACQ_REL);
        (void)sched_yield();
        return false;
    }
    (void)pthread_mutex_lock(&s_ev);
    seen = s_sig[IDX_DOIP];
    cv_wait_ms(5U);
    got = (s_sig[IDX_DOIP] != seen);
    (void)pthread_mutex_unlock(&s_ev);
    return got;
}

/* ---- server: 10, 11, 3E real; 31 defers; BA blocks (scenario B) ---------- */

static uint32_t s_job_left;    /* executor thread only */
static uint32_t s_job_aborts;
static uint32_t s_ba_runs;

static uds_step_result_t job_step(uds_msg_buf_t *final_resp, uds_nrc_t *nrc)
{
    (void)nrc;
    if (s_job_left > 0U) {
        s_job_left--;
        return UDS_STEP_IN_PROGRESS;
    }
    final_resp->data[0] = 0x71U;
    final_resp->data[1] = 0x01U;
    final_resp->data[2] = 0xF0U;
    final_resp->data[3] = 0x00U;
    final_resp->length  = 4U;
    return UDS_STEP_DONE;
}

static void job_abort(void) { __atomic_add_fetch(&s_job_aborts, 1U, __ATOMIC_ACQ_REL); }

static uds_status_t h_defer_0x31(uds_server_ctx_t *ctx, const uds_msg_buf_t *req,
                                 uds_msg_buf_t *resp)
{
    static const uds_pending_job_t job = {
        .sid = 0x31U, .step_max_ms = 10U, .step = job_step, .abort = job_abort,
    };
    (void)req;
    s_job_left = 3U;
    return uds_server_defer(ctx, &job, resp);
}

/* Runs on the executor: held until DoIP has given up and closed. */
static uds_status_t h_block_0xba(uds_server_ctx_t *ctx, const uds_msg_buf_t *req,
                                 uds_msg_buf_t *resp)
{
    (void)ctx; (void)req;
    __atomic_add_fetch(&s_ba_runs, 1U, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_held, 1, __ATOMIC_RELEASE);
    (void)pthread_mutex_lock(&s_ev);
    while (!s_handler_go) {
        cv_wait_ms(5U);
    }
    (void)pthread_mutex_unlock(&s_ev);
    resp->data[0] = 0xFAU;
    resp->length  = 1U;
    return UDS_STATUS_OK;
}

static const uds_service_entry_t k_table[] = {
    { 0x10U, uds_service_0x10_handler, true  },
    { 0x11U, uds_service_0x11_handler, true  },
    { 0x3EU, uds_service_0x3E_handler, true  },
    { 0x31U, h_defer_0x31,             true  },
    { 0xBAU, h_block_0xba,             false },
};
static const uds_access_entry_t k_acl[] = {
    { 0x10U, UDS_ACL_SESSION_ALL, 0U, false },
    { 0x11U, UDS_ACL_SESSION_ALL, 0U, false },
    { 0x3EU, UDS_ACL_SESSION_ALL, 0U, false },
    { 0x31U, UDS_ACL_SESSION_ALL, 0U, false },
    { 0xBAU, UDS_ACL_SESSION_ALL, 0U, false },
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

/* ---- scripted platform ops (DoIP thread) --------------------------------- */

static const uint8_t k_routing_activation[] = {
    0x02U, 0xFDU, 0x00U, 0x05U, 0x00U, 0x00U, 0x00U, 0x07U,
    (uint8_t)(TESTER_ADDR >> 8U), (uint8_t)(TESTER_ADDR & 0xFFU),
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

static uint8_t  s_req[16];      /* this scenario's diagnostic message */
static size_t   s_req_len;
static uint32_t s_fail_diag_from; /* fail the n-th 0x8001 send (1-based), 0: none */
static uint32_t s_frame;
static size_t   s_off;
static uint32_t s_diag_sent;    /* 0x8001 frames handed to the stack */
static uint32_t s_diag_tried;

static void set_request(const uint8_t *uds, size_t n)
{
    static const uint8_t hdr[] = {
        0x02U, 0xFDU, 0x80U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
        (uint8_t)(TESTER_ADDR >> 8U), (uint8_t)(TESTER_ADDR & 0xFFU),
        (uint8_t)(ECU_ADDR >> 8U),    (uint8_t)(ECU_ADDR & 0xFFU),
    };
    (void)memcpy(s_req, hdr, sizeof(hdr));
    s_req[7] = (uint8_t)(4U + n);
    (void)memcpy(&s_req[sizeof(hdr)], uds, n);
    s_req_len = sizeof(hdr) + n;
}

static int mock_tcp_listen(uint16_t port, void **server_ctx)
{
    (void)port;
    *server_ctx = (void *)(uintptr_t)1U;
    return 0;
}

static int mock_tcp_accept(void *server_ctx, void **conn_ctx, uint32_t timeout_ms)
{
    int rc = -1;

    (void)server_ctx; (void)timeout_ms;
    (void)pthread_mutex_lock(&s_ev);
    s_idle = 1;
    (void)pthread_cond_broadcast(&s_cv);
    while (!s_armed && !s_stop) {
        cv_wait_ms(5U);
    }
    if (s_stop) {
        (void)pthread_mutex_unlock(&s_ev);
        pthread_exit(NULL);   /* the server loop never returns */
    }
    s_armed = 0;
    s_idle  = 0;
    (void)pthread_mutex_unlock(&s_ev);
    s_frame = 0U;
    s_off   = 0U;
    s_diag_sent = 0U;
    s_diag_tried = 0U;
    *conn_ctx = (void *)(uintptr_t)2U;
    rc = 0;
    return rc;
}

static int mock_tcp_recv(void *conn_ctx, uint8_t *buf, size_t buf_len, uint32_t timeout_ms)
{
    const uint8_t *f;
    size_t len;
    size_t n;

    (void)conn_ctx; (void)timeout_ms;
    if (s_frame == 0U) {
        f = k_routing_activation; len = sizeof(k_routing_activation);
    } else if (s_frame == 1U) {
        f = s_req;                len = s_req_len;
    } else {
        return RECV_DEAD;
    }
    n = len - s_off;
    if (n > buf_len) {
        n = buf_len;
    }
    (void)memcpy(buf, &f[s_off], n);
    s_off += n;
    if (s_off >= len) {
        s_frame++;
        s_off = 0U;
    }
    return (int)n;
}

static int mock_tcp_send(void *c, const uint8_t *d, size_t len)
{
    (void)c;
    if ((len >= 4U) && (d[2] == 0x80U) && (d[3] == 0x01U)) {
        s_diag_tried++;
        if ((s_fail_diag_from != 0U) && (s_diag_tried >= s_fail_diag_from)) {
            return -1;   /* the peer is gone */
        }
        s_diag_sent++;
    }
    return (int)len;
}

static int s_await_reoffer;   /* E: close only once the frame is offered again */

static void mock_tcp_close(void *c)
{
    uint32_t i;

    (void)c;
    /* E: the executor offers the unsent final frame to DoIP again before
     * the disconnect reaches it. Without uds_exec_disconnect() that frame
     * would hold the push mailbox for good. */
    for (i = 0U; s_await_reoffer && (i < 1000U) &&
                 (uds_exec_push_frame(&g_ex, (uint8_t)EDS_DOIP_UDS_SRC) == NULL); i++) {
        (void)sched_yield();
    }
    __atomic_store_n(&s_held, 0, __ATOMIC_RELEASE);
    (void)pthread_mutex_lock(&s_ev);
    s_closed     = 1;
    s_handler_go = 1;
    (void)pthread_cond_broadcast(&s_cv);
    (void)pthread_mutex_unlock(&s_ev);
}

static void mock_tcp_server_close(void *c) { (void)c; }
static void mock_ecu_reset(uint8_t t)      { (void)t; g_fail = 1; } /* executor resets */

static const eds_doip_platform_ops_t k_ops = {
    .tcp_listen       = mock_tcp_listen,
    .tcp_accept       = mock_tcp_accept,
    .tcp_send         = mock_tcp_send,
    .tcp_recv         = mock_tcp_recv,
    .tcp_close        = mock_tcp_close,
    .tcp_server_close = mock_tcp_server_close,
    .uptime_ms        = now_ms,
    .ecu_reset        = mock_ecu_reset,
};

static void *doip_thread(void *arg)
{
    (void)arg;
    (void)eds_doip_server_run(&g_doip, &g_srv, DOIP_PORT);
    return NULL;
}

/* ---- main: the scenarios, and the CAN producer --------------------------- */

/* Wait (s_ev held) until *flag, up to 10 s; false on timeout. */
static bool wait_flag(const int *flag)
{
    uint32_t i;

    for (i = 0U; (i < 2000U) && !*flag; i++) {
        cv_wait_ms(5U);
    }
    return *flag != 0;
}

/* CAN sends a request on source 0 and collects what comes back: its
 * response, then any job frames, until a final one or 2 s. */
static uds_msg_buf_t s_can_req;
static uds_msg_buf_t s_can_resp;
static uds_msg_buf_t s_can_last;

static bool can_request(const uint8_t *b, uint16_t n)
{
    const uds_msg_buf_t *f;
    uint32_t i;
    bool     done = false;

    (void)memcpy(s_can_req.data, b, n);
    s_can_req.length = n;
    if (uds_exec_submit(&g_ex, SRC_CAN, &s_can_req, &s_can_resp,
                        UDS_ADDRESSING_PHYSICAL) != UDS_EXEC_POSTED) {
        return false;
    }
    for (i = 0U; (i < 400U) && !done; i++) {
        (void)pthread_mutex_lock(&s_ev);
        cv_wait_ms(5U);
        (void)pthread_mutex_unlock(&s_ev);
        done = uds_exec_done(&g_ex, SRC_CAN);
    }
    if (!done) {
        uds_exec_release(&g_ex, SRC_CAN);
        return false;
    }
    s_can_last = s_can_resp;
    uds_exec_release(&g_ex, SRC_CAN);
    for (i = 0U; (i < 400U) && (s_can_last.length == 3U) && (s_can_last.data[2] == 0x78U); i++) {
        f = uds_exec_push_frame(&g_ex, SRC_CAN);
        if (f != NULL) {
            s_can_last = *f;
            uds_exec_push_done(&g_ex, SRC_CAN, true);
        } else {
            (void)pthread_mutex_lock(&s_ev);
            cv_wait_ms(5U);
            (void)pthread_mutex_unlock(&s_ev);
        }
    }
    return true;
}

static void check_can_served(void)
{
    static const uint8_t k_3e_00[] = { 0x3EU, 0x00U };

    CHECK(can_request(k_3e_00, 2U), "CAN 3E 00 answered");
    CHECK((s_can_last.length == 2U) && (s_can_last.data[0] == 0x7EU), "CAN got 7E 00");
}

/* One connection: arm it, wait for its close and for DoIP back in accept. */
static void run_connection(char name, const uint8_t *uds, size_t n, uint32_t fail_from,
                           int pause_exec)
{
    s_name = name;
    set_request(uds, n);
    s_fail_diag_from = fail_from;
    (void)pthread_mutex_lock(&s_ev);
    s_closed     = 0;
    s_handler_go = 0;
    s_paused     = pause_exec;
    if (pause_exec) {
        __atomic_store_n(&s_held, 1, __ATOMIC_RELEASE);
    }
    s_armed = 1;
    (void)pthread_cond_broadcast(&s_cv);
    CHECK(wait_flag(&s_closed), "connection closed");
    s_paused = 0;                 /* A: the executor resumes after the drop */
    s_wake_seq++;
    (void)pthread_cond_broadcast(&s_cv);
    /* accept() cleared s_idle when it took this connection. */
    CHECK(wait_flag(&s_idle), "DoIP back in accept: its disconnect finished");
    (void)pthread_mutex_unlock(&s_ev);
}

int main(void)
{
    static const uds_security_cfg_t sc = {
        .max_attempts = 3U, .lockout_ms = 100U,
        .key_validate_cb = t_key, .seed_generate_cb = t_seed,
    };
    static const uds_server_cfg_t svc = {
        .p2_server_max_ms = 50U, .p2_star_server_max_ms = 5000U,
        .session_ctx = &g_sess, .security_ctx = &g_sec,
        .service_table = k_table, .service_table_count = 5U,
        .access_table = k_acl, .access_table_count = 5U,
    };
    static const uint8_t k_3e_00[] = { 0x3EU, 0x00U };
    static const uint8_t k_ba[]    = { 0xBAU };
    static const uint8_t k_10_03[] = { 0x10U, 0x03U };
    static const uint8_t k_11_01[] = { 0x11U, 0x01U };
    static const uint8_t k_31[]    = { 0x31U, 0x01U, 0xF0U, 0x00U };
    uds_exec_cfg_t   ec = {
        .srv = &g_srv, .now_ms = now_ms, .enter = cs_enter, .exit = cs_exit,
        .wake = ex_wake, .wake_arg = NULL, .reset = ex_reset,
        .producers = k_producers, .producer_count = 2U,
    };
    uds_exec_stats_t st0;
    uds_exec_stats_t st1;
    pthread_t d;
    pthread_t e;

    (void)uds_session_init(&g_sess, 5000U);
    (void)uds_security_init(&g_sec, &sc);
    (void)uds_comm_control_init(NULL);
    (void)uds_periodic_init();
    (void)uds_server_init(&g_srv, &svc);
    if (uds_exec_init(&g_ex, &ec) != UDS_STATUS_OK) {
        printf("FAIL: uds_exec_init\n");
        return 1;
    }
    (void)eds_doip_register_platform(&k_ops);
    (void)eds_doip_set_executor(&g_ex, doip_wait);
    (void)eds_doip_server_init(&g_doip, ECU_ADDR);
    (void)pthread_create(&e, NULL, exec_thread, NULL);
    (void)pthread_create(&d, NULL, doip_thread, NULL);

    /* A posted: never runs, no response; the disconnect's call does run. */
    (void)uds_exec_get_stats(&g_ex, &st0);
    run_connection('A', k_3e_00, 2U, 0U, 1);
    (void)uds_exec_get_stats(&g_ex, &st1);
    CHECK(st1.served == st0.served, "the withdrawn request never ran");
    CHECK(st1.calls == st0.calls + 1U, "the disconnect's end of session ran");
    CHECK(s_diag_sent == 0U, "no answer sent");
    check_can_served();

    /* B running: the executor finishes it and frees the slot. */
    run_connection('B', k_ba, 1U, 0U, 0);
    CHECK(__atomic_load_n(&s_ba_runs, __ATOMIC_ACQUIRE) == 1U, "BA ran once");
    CHECK(s_diag_sent == 0U, "its late answer is not sent");
    check_can_served();

    /* C done: DoIP's extended session ends with it; CAN is not refused. */
    run_connection('C', k_10_03, 2U, 1U, 0);
    check_can_served();

    /* D reset owed: performed once, and the executor carries on. */
    run_connection('D', k_11_01, 2U, 1U, 0);
    CHECK(__atomic_load_n(&s_resets, __ATOMIC_ACQUIRE) == 1U, "the owed reset was performed");
    check_can_served();

    /* E job: 7F 31 78 sent, the final frame's send fails. */
    s_await_reoffer = 1;
    run_connection('E', k_31, 4U, 2U, 0);
    s_await_reoffer = 0;
    CHECK(s_diag_sent == 1U, "the 78 went out");
    check_can_served();
    s_name = 'E';
    CHECK(can_request(k_31, 4U), "CAN starts its own job");
    CHECK((s_can_last.length == 4U) && (s_can_last.data[0] == 0x71U),
          "CAN's job reaches its final frame: the mailbox is free");

    (void)pthread_mutex_lock(&s_ev);
    s_stop = 1;
    (void)pthread_cond_broadcast(&s_cv);
    (void)pthread_mutex_unlock(&s_ev);
    (void)pthread_join(d, NULL);
    (void)pthread_join(e, NULL);
    (void)uds_exec_get_stats(&g_ex, &st1);
    printf("tsan_doip_exec_states: 5 scenarios, served=%u calls=%u resets=%u aborts=%u %s\n",
           (unsigned)st1.served, (unsigned)st1.calls, (unsigned)s_resets,
           (unsigned)s_job_aborts, g_fail ? "FAILED" : "ok");
    return g_fail;
}
