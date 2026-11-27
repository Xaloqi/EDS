// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — host concurrency test body (ThreadSanitizer), shared by
 * tsan_c1_dual_shared_lock.c and tsan_c2_dual_separate_locks.c.
 *
 * PURPOSE: [EDS#197] Two producers into one uds_server_ctx_t, the #197
 *   condition:
 *     - a CAN producer, as examples/basic_ecu's diag_task: its own lock
 *       around uds_server_process_request_ex() and the tick;
 *     - a DoIP producer, as doip_server.c's dispatch with lock callbacks.
 *   DUAL_SHARED_LOCK = 1: both producers take one lock (today's documented
 *   integrator duty). 0: each takes its own (what an integrator writes from
 *   two examples). A batch is ROUNDS requests from each producer; the
 *   server's own request_count must grow by 2 * ROUNDS per batch.
 *   dual_producers_run() runs up to max_batches batches and stops after the
 *   first one that lost an update. The last line printed is
 *     "<name>: request_count=<n> expected=<batches * 2 * ROUNDS>"
 *   and run_tsan.sh reads it. Not a tsan_*.c file: run_tsan.sh builds those.
 * =============================================================================
 */

#ifndef DUAL_PRODUCERS_H
#define DUAL_PRODUCERS_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_security_algo.h"
#include "uds_comm_control.h"
#include "uds_periodic.h"

#ifndef DUAL_SHARED_LOCK
#error "define DUAL_SHARED_LOCK (0 or 1) before including dual_producers.h"
#endif

#define ROUNDS (20000U)

static pthread_mutex_t s_can_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_doip_lock = PTHREAD_MUTEX_INITIALIZER;
/* Both producers start together: under TSan's slow thread start, one could
 * otherwise finish most of its rounds before the other runs at all. */
static pthread_barrier_t s_start;
static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

static void producer(pthread_mutex_t *m, const uint8_t *seq, uint32_t n, uds_addressing_t a)
{
    uint32_t i;

    (void)pthread_barrier_wait(&s_start);
    for (i = 0U; i < ROUNDS; i++) {
        uds_msg_buf_t req = { .length = 2U };
        uds_msg_buf_t resp = { 0 };

        req.data[0] = seq[(i % n) * 2U];
        req.data[1] = seq[((i % n) * 2U) + 1U];
        (void)pthread_mutex_lock(m);
        (void)uds_server_process_request_ex(&g_srv, &req, &resp, a);
        (void)uds_server_tick_1ms(&g_srv);
        (void)pthread_mutex_unlock(m);
    }
}

/* CAN: extended session, tester present, a seed. */
static void *can_task(void *arg)
{
    static const uint8_t seq[] = { 0x10U, 0x03U, 0x3EU, 0x00U, 0x27U, 0x01U };

    (void)arg;
    producer(&s_can_lock, seq, 3U, UDS_ADDRESSING_PHYSICAL);
    return NULL;
}

/* DoIP: back to default, a DID read. */
static void *doip_task(void *arg)
{
    static const uint8_t seq[] = { 0x10U, 0x01U, 0x22U, 0xF1U };

    (void)arg;
    producer((DUAL_SHARED_LOCK != 0) ? &s_can_lock : &s_doip_lock, seq, 2U, UDS_ADDRESSING_PHYSICAL);
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

/* Runs up to max_batches batches of both producers; stops after the first
 * batch that lost a request_count update. Returns true if none was lost. */
static bool dual_producers_run(const char *name, uint32_t max_batches)
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
    pthread_t c;
    pthread_t d;
    uint32_t  batches = 0U;
    uint32_t  expected = 0U;

    uds_security_algo_reset();
    (void)uds_session_init(&g_sess, 5000U);
    (void)uds_security_init(&g_sec, &sc);
    (void)uds_comm_control_init(NULL);
    (void)uds_periodic_init();
    (void)uds_server_init(&g_srv, &svc);
    (void)pthread_barrier_init(&s_start, NULL, 2U);

    while ((batches < max_batches) && (g_srv.request_count == expected)) {
        (void)pthread_create(&c, NULL, can_task, NULL);
        (void)pthread_create(&d, NULL, doip_task, NULL);
        (void)pthread_join(c, NULL);
        (void)pthread_join(d, NULL);
        batches++;
        expected += 2U * ROUNDS;
    }
    (void)pthread_barrier_destroy(&s_start);
    printf("%s: batches=%u request_count=%u expected=%u\n", name, (unsigned)batches,
           (unsigned)g_srv.request_count, (unsigned)expected);
    return g_srv.request_count == expected;
}

#endif /* DUAL_PRODUCERS_H */
