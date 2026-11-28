// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/zephyr_exec.c
 *
 * PURPOSE: [EDS#197] Zephyr binding of the diagnostic executor. See
 *          zephyr_exec.h.
 *
 * SAFETY  : No malloc. No recursion. One executor per image.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "zephyr_exec.h"
#include "platform_api.h"   /* eds_platform_uptime_ms() */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <stdint.h>

K_THREAD_STACK_DEFINE(s_exec_stack, EDS_EXEC_STACK_SIZE);
static struct k_thread s_exec_thread;

static uds_exec_t              s_ex;
static zephyr_exec_cfg_t       s_cfg;
static uds_exec_producer_cfg_t s_producers[EDS_EXEC_MAX_PRODUCERS];
static struct k_sem            s_done[EDS_EXEC_MAX_PRODUCERS];
static struct k_sem            s_wake;
static struct k_timer          s_tick;
static struct k_spinlock       s_cs_lock;
static k_spinlock_key_t        s_cs_key;
static bool                    s_started;

/* --------------------------------------------------------------------------
 * uds_exec bindings
 * -------------------------------------------------------------------------- */

static void cs_enter(void)
{
    s_cs_key = k_spin_lock(&s_cs_lock);
}

static void cs_exit(void)
{
    k_spin_unlock(&s_cs_lock, s_cs_key);
}

static void wake(void *arg)
{
    ARG_UNUSED(arg);
    k_sem_give(&s_wake);
}

static void slot_notify(void *arg)
{
    k_sem_give(&s_done[(uintptr_t)arg]);
}

static void tick_expiry(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    k_sem_give(&s_wake);
}

/* --------------------------------------------------------------------------
 * The executor thread
 * -------------------------------------------------------------------------- */

static void exec_entry(void *p1, void *p2, void *p3)
{
    bool more;

    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;) {
        (void)k_sem_take(&s_wake, K_FOREVER);
        do {
            more = uds_exec_run_once(&s_ex);
            if (s_cfg.after_pass != NULL) {
                s_cfg.after_pass();
            }
        } while (more);
    }
}

static uint8_t slot_of(uint8_t src)
{
    uint8_t i;
    uint8_t idx = EDS_EXEC_MAX_PRODUCERS;

    for (i = 0U; i < s_cfg.producer_count; i++) {
        if (s_producers[i].src == src) {
            idx = i;
        }
    }
    return idx;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

uds_status_t zephyr_exec_start(const zephyr_exec_cfg_t *cfg)
{
    uds_exec_cfg_t ec = { 0 };
    uds_status_t   status;
    uint8_t        i;

    if ((cfg == NULL) || (cfg->srv == NULL) || (cfg->producers == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (s_started) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }
    if ((cfg->producer_count == 0U) || (cfg->producer_count > EDS_EXEC_MAX_PRODUCERS)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    s_cfg = *cfg;
    for (i = 0U; i < cfg->producer_count; i++) {
        s_producers[i]            = cfg->producers[i];
        s_producers[i].notify     = slot_notify;
        s_producers[i].notify_arg = (void *)(uintptr_t)i;
        (void)k_sem_init(&s_done[i], 0U, 1U);
    }
    (void)k_sem_init(&s_wake, 0U, 1U);

    ec.srv            = cfg->srv;
    ec.now_ms         = eds_platform_uptime_ms;
    ec.enter          = cs_enter;
    ec.exit           = cs_exit;
    ec.wake           = wake;
    ec.wake_arg       = NULL;
    ec.reset          = cfg->reset;
    ec.producers      = s_producers;
    ec.producer_count = cfg->producer_count;
    status = uds_exec_init(&s_ex, &ec);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    (void)k_thread_create(&s_exec_thread, s_exec_stack,
                          K_THREAD_STACK_SIZEOF(s_exec_stack),
                          exec_entry, NULL, NULL, NULL,
                          EDS_EXEC_PRIORITY, 0U, K_NO_WAIT);
    (void)k_thread_name_set(&s_exec_thread, "eds_exec");

    k_timer_init(&s_tick, tick_expiry, NULL);
    k_timer_start(&s_tick, K_MSEC(1), K_MSEC(1));
    s_started = true;
    return UDS_STATUS_OK;
}

uds_exec_t *zephyr_exec_get(void)
{
    return s_started ? &s_ex : NULL;
}

bool zephyr_exec_wait(uint8_t src, k_timeout_t timeout)
{
    k_timepoint_t end = sys_timepoint_calc(timeout);
    uint8_t       idx = slot_of(src);
    bool          done = false;

    if (s_started && (idx < s_cfg.producer_count)) {
        done = uds_exec_done(&s_ex, src);
        while (!done && (k_sem_take(&s_done[idx], sys_timepoint_timeout(end)) == 0)) {
            done = uds_exec_done(&s_ex, src);
        }
        /* The last give may have come with the deadline. */
        done = done || uds_exec_done(&s_ex, src);
    }
    return done;
}

bool zephyr_exec_wait_event(uint8_t src, uint32_t timeout_ms)
{
    uint8_t idx = slot_of(src);
    bool    got = false;

    if (s_started && (idx < s_cfg.producer_count)) {
        got = (k_sem_take(&s_done[idx], K_MSEC(timeout_ms)) == 0);
    }
    return got;
}
