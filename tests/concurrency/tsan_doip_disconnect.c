// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — host concurrency test (ThreadSanitizer)
 * FILE: tests/concurrency/tsan_doip_disconnect.c
 * MODULE UNDER TEST: transport/doip/doip_server.c — the disconnect reset
 *
 * PURPOSE: [EDS#635] Two real threads, as in examples/basic_ecu_doip:
 *   - the DoIP thread runs eds_doip_server_run() with scripted platform ops:
 *     each connection activates routing, enters the extended session (10 03)
 *     and drops, so the server runs its return-to-DEFAULT reset;
 *   - the tick thread runs uds_server_tick_1ms() + uds_periodic_tick_1ms()
 *     under the same mutex the DoIP lock callbacks take.
 *   S3 is 3 ms, so the tick also returns the session to DEFAULT itself.
 *   Built with -fsanitize=thread by run_tsan.sh. ThreadSanitizer reports a
 *   data race if any UDS state is changed outside the lock; the exit status
 *   is non-zero then. native_sim cannot show this (one thread at a time).
 *
 * NOT IN CI YET: the CI job is the January plan's J1 (EDS#197).
 * =============================================================================
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "doip_server.h"
#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_security_algo.h"
#include "uds_comm_control.h"
#include "uds_periodic.h"

#define ECU_ADDR     (0x0E00U)
#define TESTER_ADDR  (0x0E80U)
#define CONNECTIONS  (5000U)
#define RECV_DEAD    (-104)   /* ECONNRESET, as a real binding reports it */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;
static doip_server_state_t g_doip;
static int s_stop;   /* __atomic builtins only */

static const uint8_t k_routing_activation[] = {
    0x02U, 0xFDU, 0x00U, 0x05U, 0x00U, 0x00U, 0x00U, 0x07U,
    (uint8_t)(TESTER_ADDR >> 8U), (uint8_t)(TESTER_ADDR & 0xFFU),
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
static const uint8_t k_extended_session[] = {
    0x02U, 0xFDU, 0x80U, 0x01U, 0x00U, 0x00U, 0x00U, 0x06U,
    (uint8_t)(TESTER_ADDR >> 8U), (uint8_t)(TESTER_ADDR & 0xFFU),
    (uint8_t)(ECU_ADDR >> 8U),    (uint8_t)(ECU_ADDR & 0xFFU),
    0x10U, 0x03U,
};

/* ---- scripted platform ops (DoIP thread only) --------------------------- */

static uint32_t s_conns;
static uint32_t s_frame;     /* 0: routing activation, 1: 10 03, 2: drop */
static size_t   s_off;

static int mock_tcp_listen(uint16_t port, void **server_ctx)
{
    (void)port;
    *server_ctx = (void *)(uintptr_t)1U;
    return 0;
}

static int mock_tcp_accept(void *server_ctx, void **conn_ctx, uint32_t timeout_ms)
{
    (void)server_ctx; (void)timeout_ms;
    if (s_conns >= CONNECTIONS) {
        __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
        pthread_exit(NULL);   /* the server loop never returns */
    }
    s_conns++;
    s_frame = 0U;
    s_off   = 0U;
    *conn_ctx = (void *)(uintptr_t)2U;
    return 0;
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
        f = k_extended_session;   len = sizeof(k_extended_session);
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

static int  mock_tcp_send(void *c, const uint8_t *d, size_t len) { (void)c; (void)d; return (int)len; }
static void mock_tcp_close(void *c)                               { (void)c; }
static void mock_tcp_server_close(void *c)                        { (void)c; }
static void mock_ecu_reset(uint8_t t)                             { (void)t; }

static uint32_t mock_uptime_ms(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));
}

static const eds_doip_platform_ops_t k_ops = {
    .tcp_listen       = mock_tcp_listen,
    .tcp_accept       = mock_tcp_accept,
    .tcp_send         = mock_tcp_send,
    .tcp_recv         = mock_tcp_recv,
    .tcp_close        = mock_tcp_close,
    .tcp_server_close = mock_tcp_server_close,
    .uptime_ms        = mock_uptime_ms,
    .ecu_reset        = mock_ecu_reset,
};

static void doip_lock(void)   { (void)pthread_mutex_lock(&s_lock); }
static void doip_unlock(void) { (void)pthread_mutex_unlock(&s_lock); }

/* ---- threads ------------------------------------------------------------- */

static void *doip_thread(void *arg)
{
    (void)arg;
    (void)eds_doip_server_run(&g_doip, &g_srv, DOIP_PORT);
    return NULL;
}

static void *tick_thread(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        doip_lock();
        (void)uds_server_tick_1ms(&g_srv);
        (void)uds_periodic_tick_1ms();
        doip_unlock();
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
    pthread_t d;
    pthread_t t;

    uds_security_algo_reset();
    (void)uds_session_init(&g_sess, 3U);
    (void)uds_security_init(&g_sec, &sc);
    (void)uds_comm_control_init(NULL);
    (void)uds_periodic_init();
    (void)uds_server_init(&g_srv, &svc);
    (void)eds_doip_register_platform(&k_ops);
    eds_doip_set_lock_callbacks(doip_lock, doip_unlock);
    (void)eds_doip_server_init(&g_doip, ECU_ADDR);

    (void)pthread_create(&t, NULL, tick_thread, NULL);
    (void)pthread_create(&d, NULL, doip_thread, NULL);
    (void)pthread_join(d, NULL);
    (void)pthread_join(t, NULL);
    printf("tsan_doip_disconnect: %u connections, %u requests\n",
           (unsigned)s_conns, (unsigned)g_srv.request_count);
    return 0;
}
