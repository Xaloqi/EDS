// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr_timer.c
 *
 * PURPOSE: 1 ms periodic timer backed by Zephyr k_timer + k_sem.
 *
 *          k_timer fires in ISR context → posts k_sem.
 *          Diagnostics thread calls diag_timer_wait_tick() → pends on k_sem
 *          → invokes callback.
 *
 *          This design keeps all tick processing in thread context while
 *          maintaining < 1 μs ISR overhead (semaphore post only).
 *
 * SAFETY  : ASIL-B candidate. Tick accuracy is safety-relevant.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "zephyr_timer.h"
#include "uds_types.h"

#include <zephyr/kernel.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal state embedded in the opaque storage
 * -------------------------------------------------------------------------- */
typedef struct diag_timer_internal {
    struct k_timer      timer;       /**< Zephyr periodic timer. */
    struct k_sem        sem;         /**< Semaphore posted by timer ISR. */
    diag_timer_tick_cb  cb;          /**< User callback. */
    void               *cb_arg;      /**< Opaque callback argument. */
    bool                started;     /**< Start guard. */
    uint32_t            overrun_count; /**< Ticks missed by slow thread. */
} diag_timer_internal_t;

/* --------------------------------------------------------------------------
 * Compile-time size guard
 * -------------------------------------------------------------------------- */
BUILD_ASSERT(sizeof(diag_timer_internal_t) <= DIAG_TIMER_OPAQUE_SIZE,
             "DIAG_TIMER_OPAQUE_SIZE too small — increase in zephyr_timer.h");

static diag_timer_internal_t *intern(diag_timer_t *t)
{
    return (diag_timer_internal_t *)(void *)t->_opaque;  /* NOLINT */
}

static const diag_timer_internal_t *intern_c(const diag_timer_t *t)
{
    return (const diag_timer_internal_t *)(const void *)t->_opaque;  /* NOLINT */
}

/* --------------------------------------------------------------------------
 * k_timer expiry callback (runs in ISR / system workqueue context)
 *
 * Posts the semaphore to signal the waiting diagnostics thread.
 * The semaphore count accumulates if the thread is slow — this is intentional
 * and allows overrun detection in diag_timer_wait_tick().
 * -------------------------------------------------------------------------- */
static void timer_expiry_fn(struct k_timer *timer_id)
{
    diag_timer_internal_t *ti =
        CONTAINER_OF(timer_id, diag_timer_internal_t, timer);

    (void)k_sem_give(&ti->sem);
}

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t diag_timer_init(diag_timer_t *t, diag_timer_tick_cb cb, void *arg)
{
    diag_timer_internal_t *ti;

    if ((t == NULL) || (cb == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    (void)memset(t->_opaque, 0, sizeof(t->_opaque));

    ti = intern(t);
    ti->cb          = cb;
    ti->cb_arg      = arg;
    ti->started     = false;
    ti->overrun_count = 0U;

    /*
     * Initialise semaphore:
     *   initial_count = 0  (no ticks pending)
     *   limit         = K_SEM_MAX_LIMIT  (allow accumulation for overrun detection)
     */
    k_sem_init(&ti->sem, 0U, K_SEM_MAX_LIMIT);

    /* Initialise timer — stop callback not needed. */
    k_timer_init(&ti->timer, timer_expiry_fn, NULL);

    return UDS_STATUS_OK;
}

uds_status_t diag_timer_start(diag_timer_t *t)
{
    diag_timer_internal_t *ti;

    if (t == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    ti = intern(t);

    if (ti->started) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    /*
     * Start a periodic timer with:
     *   duration = K_MSEC(1)   (first expiry)
     *   period   = K_MSEC(1)   (subsequent expiries)
     */
    k_timer_start(&ti->timer, K_MSEC(1), K_MSEC(1));
    ti->started = true;

    return UDS_STATUS_OK;
}

uds_status_t diag_timer_stop(diag_timer_t *t)
{
    diag_timer_internal_t *ti;

    if (t == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    ti = intern(t);
    k_timer_stop(&ti->timer);
    ti->started = false;

    return UDS_STATUS_OK;
}

uds_status_t diag_timer_wait_tick(diag_timer_t *t, uint32_t timeout_ms)
{
    diag_timer_internal_t *ti;
    int rc;

    if (t == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    ti = intern(t);

    /* Block until the next semaphore post (timer expiry). */
    rc = k_sem_take(&ti->sem, K_MSEC((int32_t)timeout_ms));
    if (rc == -EAGAIN) {
        return UDS_STATUS_ERR_TIMEOUT;
    }

    /* Drain any additional accumulated ticks (overrun detection). */
    while (k_sem_take(&ti->sem, K_NO_WAIT) == 0) {
        ti->overrun_count++;
    }

    /* Invoke the registered callback. */
    ti->cb(ti->cb_arg);

    return UDS_STATUS_OK;
}

uds_status_t diag_timer_pending_ticks(const diag_timer_t *t, uint32_t *out_count)
{
    const diag_timer_internal_t *ti;

    if ((t == NULL) || (out_count == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    ti = intern_c(t);
    *out_count = ti->overrun_count;

    return UDS_STATUS_OK;
}
