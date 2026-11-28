// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — host concurrency test (ThreadSanitizer)
 * FILE: tests/concurrency/tsan_c2_dual_transport.c
 * MODULE UNDER TEST: examples/dual_transport_ecu's two producers on one server
 *
 * PURPOSE: [EDS#197] Scenario C2, the #197 race and the theme's fail-first
 *   test, written against dual_transport_ecu's two producers (J7):
 *     - CAN: the example's own src/can_producer.c, stepped on the CAN
 *       thread, over the real ISO-TP and a mock CAN bus. The bus's tester
 *       sends 10 03, 3E 00, 27 01, 10 01 in turn, each after the previous
 *       answer (it sends Flow Control for a multi-frame answer);
 *     - DoIP: the real doip_server.c with eds_doip_set_executor(), over
 *       mock TCP. Its tester sends 10 03, 22 F1 90, 10 01 in turn on one
 *       connection;
 *   so the two compete for the session: whichever opens 10 03 owns it, and
 *   the other is answered 0x21 until it returns to default (decision ⬥A);
 *     - the executor thread runs uds_exec_run_once(), as zephyr_exec.c does.
 *   Every request must get exactly one answer (a response, or 7F sid 21),
 *   and the server's own request_count must equal the requests the executor
 *   dispatched (sent minus those answered 0x21 without reaching it). Any
 *   data race fails it.
 *
 *   -DC2_J1_WIRING=1 runs the same testers against the producers as they
 *   were on the J1 base (EDS#657): the CAN loop of examples/basic_ecu (the
 *   server called from the ISO-TP callback under the CAN thread's lock)
 *   and basic_ecu_doip's DoIP thread with its lock callbacks and tick task
 *   on a second lock. That is the red run: data races, and request_count
 *   below the number sent. Not built by run_tsan.sh; the PR shows it.
 *
 * CI: the concurrency-tsan job (EDS#197 J1).
 * =============================================================================
 */

#define EDS_DOIP_ALLOW_DEPRECATED   /* C2_J1_WIRING: the J1 base's lock callbacks */

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "doip_server.h"
#include "uds_exec.h"
#include "isotp.h"
#include "can_transport.h"
#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_security_algo.h"
#include "uds_comm_control.h"
#include "uds_periodic.h"
/* The example's CAN producer itself, not a copy (run_tsan.sh builds one file). */
#include "../../examples/dual_transport_ecu/src/can_producer.c"

#ifndef C2_J1_WIRING
#define C2_J1_WIRING 0
#endif

#define ROUNDS       (5000U)     /* requests per transport */
#define ECU_ADDR     (0x0E00U)
#define TESTER_ADDR  (0x0E80U)
#define CAN_RX_ID    (0x7E0U)
#define CAN_TX_ID    (0x7E8U)
#define RECV_DEAD    (-104)
#define SRC_CAN      (0U)
#define IDX_CAN      (0U)
#define IDX_DOIP     (1U)

static uds_session_ctx_t   g_sess;
static uds_security_ctx_t  g_sec;
static uds_server_ctx_t    g_srv;
static uds_exec_t          g_ex;
static doip_server_state_t g_doip;
static isotp_ctx_t         g_tp;
static pthread_barrier_t   s_start;
static int                 s_stop;   /* __atomic builtins only */

static uint32_t now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));
}

/* ---- executor bindings (as exec_states) ---------------------------------- */

static pthread_mutex_t s_cs = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_ev = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;
static uint32_t        s_wake_seq;   /* s_ev */
static uint32_t        s_sig[2];     /* s_ev */

static void cs_enter(void) { (void)pthread_mutex_lock(&s_cs); }
static void cs_exit(void)  { (void)pthread_mutex_unlock(&s_cs); }

static void cv_wait_ms(uint32_t ms)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec  += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    (void)pthread_cond_timedwait(&s_cv, &s_ev, &ts);
}

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

static const uds_exec_producer_cfg_t k_producers[2] = {
    { .src = SRC_CAN,                   .periodic_tx = true,
      .notify = ex_notify, .notify_arg = (void *)(uintptr_t)IDX_CAN },
    { .src = (uint8_t)EDS_DOIP_UDS_SRC, .periodic_tx = false,
      .notify = ex_notify, .notify_arg = (void *)(uintptr_t)IDX_DOIP },
};

static void *exec_thread(void *arg)
{
    uint32_t seen = 0U;

    (void)arg;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        (void)pthread_mutex_lock(&s_ev);
        if (s_wake_seq == seen) {
            cv_wait_ms(1U);
        }
        seen = s_wake_seq;
        (void)pthread_mutex_unlock(&s_ev);
        while (uds_exec_run_once(&g_ex)) {
        }
    }
    return NULL;
}

static bool doip_wait(uint8_t src, uint32_t timeout_ms)
{
    uint32_t seen;
    bool     got;

    (void)src;
    (void)timeout_ms;
    (void)pthread_mutex_lock(&s_ev);
    seen = s_sig[IDX_DOIP];
    cv_wait_ms(2U);
    got = (s_sig[IDX_DOIP] != seen);
    (void)pthread_mutex_unlock(&s_ev);
    return got;
}

/* ---- the J1 base's locks (C2_J1_WIRING) ---------------------------------- */

static pthread_mutex_t s_can_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_doip_lock = PTHREAD_MUTEX_INITIALIZER;
static void doip_lock(void)   { (void)pthread_mutex_lock(&s_doip_lock); }
static void doip_unlock(void) { (void)pthread_mutex_unlock(&s_doip_lock); }

/* ---- CAN bus with its tester (CAN thread only) --------------------------- */

#define CAN_SEQ_N (4U)
static const uint8_t k_can_seq[CAN_SEQ_N][2] = {
    { 0x10U, 0x03U }, { 0x3EU, 0x00U }, { 0x27U, 0x01U }, { 0x10U, 0x01U },
};

static uds_can_frame_t s_rxq[4];       /* frames for the ECU to receive */
static uint32_t        s_rxq_n;
static uint32_t        s_can_sent;     /* requests the tester sent      */
static uint32_t        s_can_answers;  /* complete answers it received  */
static uint32_t        s_can_busy;     /* of which 7F xx 21             */
static bool            s_can_waiting;
static uint32_t        s_mf_left;      /* bytes still owed by a multi-frame answer */
static uint8_t         s_mf_first[3];

static void can_rx_push(const uint8_t *d, uint8_t n)
{
    uds_can_frame_t *f = &s_rxq[s_rxq_n];

    (void)memset(f, 0, sizeof(*f));
    f->id  = CAN_RX_ID;
    f->dlc = 8U;
    (void)memcpy(f->data, d, n);
    s_rxq_n++;
}

static void can_answer(const uint8_t *pdu)
{
    s_can_answers++;
    if ((pdu[0] == 0x7FU) && (pdu[2] == 0x21U)) {
        s_can_busy++;
    }
    s_can_waiting = false;
}

static uds_status_t mock_can_tx(can_transport_t *self, const uds_can_frame_t *f)
{
    uint8_t pci = (uint8_t)(f->data[0] >> 4U);
    uint32_t n;

    (void)self;
    if (f->id != CAN_TX_ID) {
        return UDS_STATUS_OK;                       /* a periodic UUDT frame */
    }
    if (pci == 0U) {                                /* Single Frame */
        can_answer(&f->data[1]);
    } else if (pci == 1U) {                         /* First Frame: send FC CTS */
        static const uint8_t fc[3] = { 0x30U, 0x00U, 0x00U };
        s_mf_left = ((uint32_t)(f->data[0] & 0x0FU) << 8U) | f->data[1];
        (void)memcpy(s_mf_first, &f->data[2], 3U);
        s_mf_left -= 6U;
        can_rx_push(fc, 3U);
    } else if (pci == 2U) {                         /* Consecutive Frame */
        n = (s_mf_left > 7U) ? 7U : s_mf_left;
        s_mf_left -= n;
        if (s_mf_left == 0U) {
            can_answer(s_mf_first);
        }
    } else {
        /* Flow Control from the ECU: not expected here */
    }
    return UDS_STATUS_OK;
}

static uds_status_t mock_can_rx(can_transport_t *self, uds_can_frame_t *out, bool *ready)
{
    uint32_t i;
    uint8_t  sf[3];

    (void)self;
    *ready = false;
    if (s_rxq_n == 0U && !s_can_waiting && (s_can_sent < ROUNDS)) {
        sf[0] = 0x02U;
        sf[1] = k_can_seq[s_can_sent % CAN_SEQ_N][0];
        sf[2] = k_can_seq[s_can_sent % CAN_SEQ_N][1];
        can_rx_push(sf, 3U);
        s_can_sent++;
        s_can_waiting = true;
    }
    if (s_rxq_n > 0U) {
        *out = s_rxq[0];
        for (i = 1U; i < s_rxq_n; i++) {
            s_rxq[i - 1U] = s_rxq[i];
        }
        s_rxq_n--;
        *ready = true;
    }
    return UDS_STATUS_OK;
}

static uds_status_t mock_can_status(can_transport_t *self, bool *bus_off)
{
    (void)self;
    *bus_off = false;
    return UDS_STATUS_OK;
}

static const can_transport_ops_t k_can_ops = {
    .transmit = mock_can_tx, .receive = mock_can_rx, .get_status = mock_can_status,
};
static can_transport_t g_can = { .ops = &k_can_ops, .platform = NULL, .ready = true };

/* ---- the J1 base's CAN loop (examples/basic_ecu), C2_J1_WIRING ----------- */

static uds_msg_buf_t s_j1_req;
static uds_msg_buf_t s_j1_resp;

static void j1_on_rx(const uint8_t *data, uint32_t length, void *arg)
{
    (void)arg;
    (void)memcpy(s_j1_req.data, data, length);
    s_j1_req.length  = (uint16_t)length;
    s_j1_resp.length = 0U;
    (void)pthread_mutex_lock(&s_can_lock);
    (void)uds_server_process_request_ex(&g_srv, &s_j1_req, &s_j1_resp, UDS_ADDRESSING_PHYSICAL);
    (void)pthread_mutex_unlock(&s_can_lock);
    if (s_j1_resp.length > 0U) {
        (void)isotp_transmit(&g_tp, s_j1_resp.data, s_j1_resp.length);
    }
}

static void j1_can_step(void)
{
    uds_can_frame_t f;
    bool            ready = true;

    while (ready) {
        (void)can_transport_receive(&g_can, &f, &ready);
        if (ready) {
            (void)isotp_process_rx_frame(&g_tp, &f, j1_on_rx, NULL);
        }
    }
    (void)isotp_tick_1ms(&g_tp);
    (void)pthread_mutex_lock(&s_can_lock);
    (void)uds_server_tick_1ms(&g_srv);
    (void)pthread_mutex_unlock(&s_can_lock);
}

static can_producer_t s_can_prod;

static void *can_thread(void *arg)
{
    (void)arg;
    (void)pthread_barrier_wait(&s_start);
    while (s_can_answers < ROUNDS) {
        if (C2_J1_WIRING != 0) {
            j1_can_step();
        } else {
            can_producer_step(&s_can_prod);
        }
        (void)sched_yield();
    }
    return NULL;
}

/* ---- DoIP: mock TCP with its tester (DoIP thread only) ------------------- */

static const uint8_t k_routing_activation[] = {
    0x02U, 0xFDU, 0x00U, 0x05U, 0x00U, 0x00U, 0x00U, 0x07U,
    (uint8_t)(TESTER_ADDR >> 8U), (uint8_t)(TESTER_ADDR & 0xFFU),
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
#define DOIP_SEQ_N (3U)
static const uint8_t k_doip_seq[DOIP_SEQ_N][3] = {
    { 0x10U, 0x03U, 0U }, { 0x22U, 0xF1U, 0x90U }, { 0x10U, 0x01U, 0U },
};
static const uint8_t k_doip_len[DOIP_SEQ_N] = { 2U, 3U, 2U };

static uint8_t  s_frame[16];
static size_t   s_frame_len;
static size_t   s_off;
static bool     s_ra_done;
static uint32_t s_doip_sent;
static uint32_t s_doip_answers;
static uint32_t s_doip_busy;
static uint32_t s_accepts;

static int mock_tcp_listen(uint16_t port, void **server_ctx)
{
    (void)port;
    *server_ctx = (void *)(uintptr_t)1U;
    return 0;
}

static int mock_tcp_accept(void *server_ctx, void **conn_ctx, uint32_t timeout_ms)
{
    (void)server_ctx; (void)timeout_ms;
    if (s_accepts > 0U) {
        pthread_exit(NULL);   /* the tester is done; the server loop never returns */
    }
    s_accepts++;
    (void)pthread_barrier_wait(&s_start);
    *conn_ctx = (void *)(uintptr_t)2U;
    return 0;
}

static void doip_next_frame(void)
{
    static const uint8_t hdr[] = {
        0x02U, 0xFDU, 0x80U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
        (uint8_t)(TESTER_ADDR >> 8U), (uint8_t)(TESTER_ADDR & 0xFFU),
        (uint8_t)(ECU_ADDR >> 8U),    (uint8_t)(ECU_ADDR & 0xFFU),
    };
    uint32_t k = s_doip_sent % DOIP_SEQ_N;

    (void)memcpy(s_frame, hdr, sizeof(hdr));
    s_frame[7] = (uint8_t)(4U + k_doip_len[k]);
    (void)memcpy(&s_frame[sizeof(hdr)], k_doip_seq[k], k_doip_len[k]);
    s_frame_len = sizeof(hdr) + k_doip_len[k];
    s_doip_sent++;
}

static int mock_tcp_recv(void *conn_ctx, uint8_t *buf, size_t buf_len, uint32_t timeout_ms)
{
    const uint8_t *f;
    size_t len;
    size_t n;

    /* The server answers each request before it reads again, so the tester
     * sends the next one on every read: lockstep. */
    (void)conn_ctx; (void)timeout_ms;
    if (!s_ra_done) {
        f = k_routing_activation; len = sizeof(k_routing_activation);
    } else if ((s_off > 0U) || (s_doip_sent < ROUNDS)) {
        if (s_off == 0U) {
            doip_next_frame();
        }
        f = s_frame; len = s_frame_len;
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
        s_off = 0U;
        s_ra_done = true;
    }
    return (int)n;
}

static int mock_tcp_send(void *c, const uint8_t *d, size_t len)
{
    (void)c;
    if ((len >= 13U) && (d[2] == 0x80U) && (d[3] == 0x01U)) {   /* diagnostic message */
        s_doip_answers++;
        if ((d[12] == 0x7FU) && (len >= 15U) && (d[14] == 0x21U)) {
            s_doip_busy++;
        }
    }
    return (int)len;
}

static void mock_tcp_close(void *c)        { (void)c; }
static void mock_tcp_server_close(void *c) { (void)c; }
static void mock_ecu_reset(uint8_t t)      { (void)t; }
static void mock_sleep_ms(uint32_t ms)     { (void)ms; (void)sched_yield(); }

static const eds_doip_platform_ops_t k_ops = {
    .tcp_listen       = mock_tcp_listen,
    .tcp_accept       = mock_tcp_accept,
    .tcp_send         = mock_tcp_send,
    .tcp_recv         = mock_tcp_recv,
    .tcp_close        = mock_tcp_close,
    .tcp_server_close = mock_tcp_server_close,
    .uptime_ms        = now_ms,
    .ecu_reset        = mock_ecu_reset,
    .sleep_ms         = mock_sleep_ms,
};

static void *doip_thread(void *arg)
{
    (void)arg;
    (void)eds_doip_server_run(&g_doip, &g_srv, DOIP_PORT);
    return NULL;
}

/* The J1 base's DoIP tick task (basic_ecu_doip at EDS#657), under the DoIP lock. */
static void *j1_tick_thread(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        doip_lock();
        (void)uds_server_tick_1ms(&g_srv);
        (void)uds_periodic_tick_1ms();
        doip_unlock();
        (void)sched_yield();
    }
    return NULL;
}

/* ---- main ---------------------------------------------------------------- */

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    return uds_security_algo_generate_seed(l, b, n, o);
}

static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl, const uint8_t *k, uint8_t kl)
{
    return uds_security_algo_validate_key(l, s, sl, k, kl);
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
        .service_table = g_uds_service_table,
        .service_table_count = (uint8_t)UDS_SERVICE_TABLE_COUNT,
    };
    const isotp_cfg_t tc = {
        .rx_can_id = CAN_RX_ID, .tx_can_id = CAN_TX_ID,
        .block_size = 0U, .stmin_ms = 0U, .can = &g_can,
    };
    uds_exec_cfg_t ec = {
        .srv = &g_srv, .now_ms = now_ms, .enter = cs_enter, .exit = cs_exit,
        .wake = ex_wake, .wake_arg = NULL, .reset = NULL,
        .producers = k_producers, .producer_count = 2U,
    };
    uds_exec_stats_t st = { 0 };
    uint32_t  expected;
    uint32_t  count;
    pthread_t c;
    pthread_t d;
    pthread_t e;
    int       fail = 0;

    uds_security_algo_reset();
    (void)uds_session_init(&g_sess, 5000U);
    (void)uds_security_init(&g_sec, &sc);
    (void)uds_comm_control_init(NULL);
    (void)uds_periodic_init();
    (void)uds_server_init(&g_srv, &svc);
    (void)isotp_init(&g_tp, &tc);
    (void)eds_doip_register_platform(&k_ops);
    (void)eds_doip_server_init(&g_doip, ECU_ADDR);
    (void)pthread_barrier_init(&s_start, NULL, 2U);

    if (C2_J1_WIRING != 0) {
        eds_doip_set_lock_callbacks(doip_lock, doip_unlock);
        (void)pthread_create(&e, NULL, j1_tick_thread, NULL);
    } else {
        (void)uds_exec_init(&g_ex, &ec);
        can_producer_init(&s_can_prod, &g_ex, SRC_CAN, &g_tp, &g_can);
        (void)eds_doip_set_executor(&g_ex, doip_wait);
        (void)pthread_create(&e, NULL, exec_thread, NULL);
    }
    (void)pthread_create(&d, NULL, doip_thread, NULL);
    (void)pthread_create(&c, NULL, can_thread, NULL);
    (void)pthread_join(c, NULL);
    (void)pthread_join(d, NULL);
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    (void)pthread_join(e, NULL);

    count = g_srv.request_count;
    if (C2_J1_WIRING != 0) {
        expected = 2U * ROUNDS;
    } else {
        (void)uds_exec_get_stats(&g_ex, &st);
        expected = (2U * ROUNDS) - st.reject_busy - st.dropped;
        if (st.served != count) {
            printf("FAIL: the executor dispatched %u, the server counted %u\n",
                   (unsigned)st.served, (unsigned)count);
            fail = 1;
        }
    }
    if ((s_can_answers != ROUNDS) || (s_doip_answers != ROUNDS)) {
        printf("FAIL: answers CAN %u DoIP %u of %u each\n",
               (unsigned)s_can_answers, (unsigned)s_doip_answers, (unsigned)ROUNDS);
        fail = 1;
    }
    if (count != expected) {
        fail = 1;
    }
    printf("tsan_c2_dual_transport%s: CAN %u answers (%u busy), DoIP %u answers (%u busy), "
           "request_count=%u expected=%u\n",
           (C2_J1_WIRING != 0) ? " [J1 wiring]" : "",
           (unsigned)s_can_answers, (unsigned)s_can_busy,
           (unsigned)s_doip_answers, (unsigned)s_doip_busy, (unsigned)count, (unsigned)expected);
    return fail;
}
