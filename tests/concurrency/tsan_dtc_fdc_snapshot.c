// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — host concurrency test (ThreadSanitizer)
 * FILE: tests/concurrency/tsan_dtc_fdc_snapshot.c
 * MODULE UNDER TEST: core/uds_services/service_0x19.c — 0x19 0x14
 *
 * PURPOSE: [EDS#637] Two real threads, as in examples/sensor_ecu:
 *   - a monitor thread moves a DTC's fault detection counter between 0
 *     (not reported by 0x19 0x14) and 50 (reported), through
 *     dtc_database_set_fault_counter(), under the DTC lock callbacks;
 *   - the dispatch thread sends 19 14 and checks every reported counter is
 *     in 1..126, the range the handler filters on.
 *   ThreadSanitizer reports a data race if the handler reads the counter
 *   outside the DTC lock, and the test also fails on any counter reported
 *   outside its own filter. Built by run_tsan.sh.
 * =============================================================================
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "dtc_database.h"
#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_security_algo.h"
#include "uds_comm_control.h"

#define DTC_UNDER_TEST  (0xC00100UL)
#define REQUESTS        (20000U)

static pthread_mutex_t s_dtc_lock = PTHREAD_MUTEX_INITIALIZER;
static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;
static int s_stop;   /* __atomic builtins only */

static void dtc_lock(void)   { (void)pthread_mutex_lock(&s_dtc_lock); }
static void dtc_unlock(void) { (void)pthread_mutex_unlock(&s_dtc_lock); }

static void *monitor_thread(void *arg)
{
    uint8_t fdc = 0U;

    (void)arg;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        fdc = (fdc == 0U) ? 50U : 0U;
        (void)dtc_database_set_fault_counter(DTC_UNDER_TEST, fdc);
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
    pthread_t m;
    uint32_t i;
    uint32_t reported = 0U;
    uint32_t out_of_filter = 0U;

    uds_security_algo_reset();
    (void)uds_session_init(&g_sess, 5000U);
    (void)uds_security_init(&g_sec, &sc);
    (void)uds_comm_control_init(NULL);
    (void)uds_server_init(&g_srv, &svc);
    (void)dtc_database_init();
    (void)dtc_database_register(DTC_UNDER_TEST, 0x00U, "DTC under test");
    dtc_database_set_lock_callbacks(dtc_lock, dtc_unlock);

    (void)pthread_create(&m, NULL, monitor_thread, NULL);
    for (i = 0U; i < REQUESTS; i++) {
        uds_msg_buf_t req  = { .data = { 0x19U, 0x14U }, .length = 2U };
        uds_msg_buf_t resp = { 0 };

        (void)uds_server_process_request(&g_srv, &req, &resp);
        if ((resp.length >= 6U) && (resp.data[0] == 0x59U)) {
            int8_t fdc = (int8_t)resp.data[5];
            reported++;
            if ((fdc < (int8_t)1) || (fdc > (int8_t)126)) {
                out_of_filter++;
            }
        }
    }
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    (void)pthread_join(m, NULL);

    printf("tsan_dtc_fdc_snapshot: %u requests, %u records, %u outside 1..126\n",
           (unsigned)REQUESTS, (unsigned)reported, (unsigned)out_of_filter);
    return (out_of_filter == 0U) ? 0 : 1;
}
