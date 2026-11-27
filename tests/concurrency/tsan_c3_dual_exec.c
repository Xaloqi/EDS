// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — host concurrency test (ThreadSanitizer)
 * FILE: tests/concurrency/tsan_c3_dual_exec.c
 * MODULE UNDER TEST: core/uds_exec.c with two producers
 *
 * PURPOSE: [EDS#197] Scenario C3: C2's two producers (a CAN and a DoIP
 *   thread, the same request sequences) submit through the executor
 *   instead of calling the server under their own locks. A third thread runs
 *   uds_exec_run_once(). The only lock is the executor's short critical
 *   section. Expected: no data race, and request_count is exactly the number
 *   of requests sent. C2 stays as it is (an XFAIL): it documents the
 *   lock-based path until that path is deprecated (J6).
 * =============================================================================
 */

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "services.h"
#include "uds_exec.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_security_algo.h"
#include "uds_comm_control.h"
#include "uds_periodic.h"

#define ROUNDS   (20000U)
#define SRC_CAN  (0U)
#define SRC_DOIP (1U)

static pthread_mutex_t s_cs = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t s_start;
static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;
static uds_exec_t         g_ex;
static int                s_stop;      /* __atomic builtins only */
static uint32_t           s_rejected;  /* __atomic builtins only */

static uint32_t now_ms(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));
}
static void cs_enter(void) { (void)pthread_mutex_lock(&s_cs); }
static void cs_exit(void)  { (void)pthread_mutex_unlock(&s_cs); }

static void producer(uint8_t src, const uint8_t *seq, uint32_t n)
{
    static __thread uds_msg_buf_t req;
    static __thread uds_msg_buf_t resp;
    uint32_t i;

    (void)pthread_barrier_wait(&s_start);
    for (i = 0U; i < ROUNDS; i++) {
        req.data[0] = seq[(i % n) * 2U];
        req.data[1] = seq[((i % n) * 2U) + 1U];
        req.length  = 2U;
        /* One request in flight per producer, so never REJECT_BUSY here;
         * count it if it happens, the request was not dispatched. */
        while (uds_exec_submit(&g_ex, src, &req, &resp, UDS_ADDRESSING_PHYSICAL) != UDS_EXEC_POSTED) {
            __atomic_add_fetch(&s_rejected, 1U, __ATOMIC_RELAXED);
            (void)sched_yield();
        }
        while (!uds_exec_done(&g_ex, src)) {
            (void)sched_yield();
        }
        uds_exec_release(&g_ex, src);
    }
}

static void *can_task(void *arg)
{
    static const uint8_t seq[] = { 0x10U, 0x03U, 0x3EU, 0x00U, 0x27U, 0x01U };
    (void)arg;
    producer(SRC_CAN, seq, 3U);
    return NULL;
}

static void *doip_task(void *arg)
{
    static const uint8_t seq[] = { 0x10U, 0x01U, 0x22U, 0xF1U };
    (void)arg;
    producer(SRC_DOIP, seq, 2U);
    return NULL;
}

static void *exec_task(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        if (!uds_exec_run_once(&g_ex)) {
            (void)sched_yield();
        }
    }
    return NULL;
}

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
    static const uds_exec_producer_cfg_t producers[2] = {
        { .src = SRC_CAN,  .periodic_tx = true  },
        { .src = SRC_DOIP, .periodic_tx = false },
    };
    uds_exec_cfg_t ec = {
        .srv = &g_srv, .now_ms = now_ms, .enter = cs_enter, .exit = cs_exit,
        .producers = producers, .producer_count = 2U,
    };
    uds_exec_stats_t st;
    pthread_t c;
    pthread_t d;
    pthread_t e;

    uds_security_algo_reset();
    (void)uds_session_init(&g_sess, 5000U);
    (void)uds_security_init(&g_sec, &sc);
    (void)uds_comm_control_init(NULL);
    (void)uds_periodic_init();
    (void)uds_server_init(&g_srv, &svc);
    (void)uds_exec_init(&g_ex, &ec);
    (void)pthread_barrier_init(&s_start, NULL, 2U);

    (void)pthread_create(&e, NULL, exec_task, NULL);
    (void)pthread_create(&c, NULL, can_task, NULL);
    (void)pthread_create(&d, NULL, doip_task, NULL);
    (void)pthread_join(c, NULL);
    (void)pthread_join(d, NULL);
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    (void)pthread_join(e, NULL);
    (void)pthread_barrier_destroy(&s_start);

    (void)uds_exec_get_stats(&g_ex, &st);
    printf("tsan_c3_dual_exec: rejected=%u served=%u max_queue_wait_ms=%u "
           "request_count=%u expected=%u\n",
           (unsigned)s_rejected, (unsigned)st.served, (unsigned)st.max_queue_wait_ms,
           (unsigned)g_srv.request_count, (unsigned)(2U * ROUNDS));
    return ((g_srv.request_count == (2U * ROUNDS)) && (st.served == (2U * ROUNDS))) ? 0 : 1;
}
