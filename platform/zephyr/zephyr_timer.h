// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr_timer.h
 *
 * PURPOSE: 1 ms periodic timer abstraction over Zephyr k_timer.
 *
 *          Drives the ISO-TP N_Cr/N_As/N_Bs timeout counters and the UDS
 *          S3server session timeout. A single k_timer fires at 1 ms intervals
 *          and posts to a k_sem, which the diagnostics thread consumes to
 *          tick both isotp_tick_1ms() and uds_server_tick_1ms().
 *
 *          Using a semaphore (rather than a direct ISR callback) keeps all
 *          stack processing in thread context, which is required for:
 *            - Stack watchdog supervision
 *            - Deterministic worst-case execution time analysis
 *            - Mutex acquisition in tick paths
 *
 * TIMING ACCURACY:
 *          Zephyr k_timer resolution matches CONFIG_SYS_CLOCK_TICKS_PER_SEC.
 *          For 1 ms accuracy, set:
 *            CONFIG_SYS_CLOCK_TICKS_PER_SEC=1000
 *          or use CONFIG_TICKLESS_KERNEL=y with a platform that supports it.
 *
 * SAFETY  : ASIL-B candidate. Timer accuracy is safety-relevant for ISO-TP
 *           N_Cr (150 ms) and session S3server (5000 ms) timeouts.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef ZEPHYR_TIMER_H
#define ZEPHYR_TIMER_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Timer context (opaque to callers)
 * -------------------------------------------------------------------------- */
#define DIAG_TIMER_OPAQUE_SIZE   (128U)

typedef struct diag_timer {
    uint8_t _opaque[DIAG_TIMER_OPAQUE_SIZE]; /**< Storage for k_timer + k_sem. */
} diag_timer_t;

/* --------------------------------------------------------------------------
 * Callback type
 *
 * Invoked from the diagnostics thread context (NOT from ISR) once per 1 ms
 * tick. The callback must complete within 1 ms to avoid tick accumulation.
 *
 * @param[in] arg  Opaque argument registered with diag_timer_start().
 * -------------------------------------------------------------------------- */
typedef void (*diag_timer_tick_cb)(void *arg);

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialise a periodic 1 ms timer.
 *
 * @param[out] t    Caller-allocated timer object.
 * @param[in]  cb   Callback invoked once per tick from thread context.
 * @param[in]  arg  Opaque argument passed to cb.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if t or cb is NULL.
 */
uds_status_t diag_timer_init(diag_timer_t *t, diag_timer_tick_cb cb, void *arg);

/**
 * @brief Start the 1 ms periodic timer.
 *
 * After this call, the timer fires every 1 ms and the callback is invoked
 * each time the diagnostics thread calls diag_timer_wait_tick().
 *
 * @param[in] t  Initialised timer.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if t is NULL.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already started.
 */
uds_status_t diag_timer_start(diag_timer_t *t);

/**
 * @brief Stop the periodic timer.
 *
 * @param[in] t  Initialised and started timer.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if t is NULL.
 */
uds_status_t diag_timer_stop(diag_timer_t *t);

/**
 * @brief Block until the next timer tick, then invoke the callback.
 *
 * Called from the diagnostics thread poll loop. Blocks for at most 1 ms
 * (until the k_sem posted by the k_timer expiry ISR is signalled).
 *
 * If the thread is late (missed tick), the semaphore count will be > 1.
 * This function drains all pending ticks to prevent unbounded accumulation.
 *
 * @param[in] t        Initialised and started timer.
 * @param[in] timeout  Maximum time to wait (K_MSEC(1) recommended).
 *
 * @return UDS_STATUS_OK on tick delivered.
 * @return UDS_STATUS_ERR_TIMEOUT if no tick arrived within timeout.
 * @return UDS_STATUS_ERR_NULL_PTR if t is NULL.
 */
uds_status_t diag_timer_wait_tick(diag_timer_t *t, uint32_t timeout_ms);

/**
 * @brief Query number of ticks that have fired since last drain.
 *
 * Useful for overrun detection — if > 1, the previous tick was not
 * processed in time.
 *
 * @param[in]  t          Initialised timer.
 * @param[out] out_count  Set to pending tick count.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 */
uds_status_t diag_timer_pending_ticks(const diag_timer_t *t, uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_TIMER_H */
