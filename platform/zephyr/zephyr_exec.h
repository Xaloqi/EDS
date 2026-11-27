// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/zephyr_exec.h
 *
 * PURPOSE: [EDS#197] Zephyr binding of the diagnostic executor
 *          (core/uds_exec.h): the thread that owns the UDS server.
 *
 *          - One dedicated thread runs uds_exec_run_once(). Not k_work on
 *            the system workqueue: a job step can block for seconds, and
 *            that thread is shared.
 *          - One k_sem wakes it, given by producers (uds_exec's wake) and by
 *            a 1 ms k_timer, so the UDS timers run once per elapsed ms.
 *          - One k_sem per producer slot signals completion (uds_exec's
 *            notify). Not k_msgq: requests stay in the producer's buffers,
 *            nothing is copied.
 *          - The critical section is a k_spinlock around uds_exec's own
 *            small state.
 *          - after_pass, if set, runs on the executor thread after every
 *            pass. The example feeds its watchdog there (EDS#399), so a
 *            stalled executor stops the feed.
 *
 *  Producer contracts (uds_exec.h):
 *    1. After a submit or a call, uds_exec_release() on every exit path:
 *       response sent, send error, disconnect, timeout. zephyr_exec_wait()
 *       returning false (a timeout) is such an exit path.
 *    2. A posted slot's buffers are the executor's until it is done with
 *       them; receive the next request into a buffer of your own.
 *
 *  Transitional (until DoIP runs on the executor, EDS#197 J6): the DoIP
 *  server still calls the UDS server from its own thread, between its lock
 *  callbacks. zephyr_exec_lend() and zephyr_exec_lend_end() are those
 *  callbacks: lend() posts a call that parks the executor and returns once
 *  it is parked, so the DoIP thread holds the server alone; lend_end()
 *  unparks it and releases the slot. No mutex: the executor's slot is the
 *  exclusion.
 *
 * SAFETY  : No malloc. No recursion. One executor per image (static state).
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef ZEPHYR_EXEC_H
#define ZEPHYR_EXEC_H

#include "uds_exec.h"
#include "uds_server.h"

#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Executor thread stack. Ticks, the S3 reset and the periodic read run on it. */
#ifndef EDS_EXEC_STACK_SIZE
#define EDS_EXEC_STACK_SIZE   (1024U)
#endif

/** Executor thread priority: above the DoIP thread (7), like the tick task it replaces. */
#ifndef EDS_EXEC_PRIORITY
#define EDS_EXEC_PRIORITY     (5)
#endif

/** What the application gives the binding. */
typedef struct zephyr_exec_cfg {
    uds_server_ctx_t              *srv;            /**< Initialized server. Required.             */
    /** Sources and periodic_tx; notify and notify_arg are the binding's own. */
    const uds_exec_producer_cfg_t *producers;
    uint8_t                        producer_count;
    void                         (*reset)(uint8_t reset_type); /**< Optional (uds_exec_cfg_t).   */
    void                         (*after_pass)(void);          /**< Optional, executor thread.   */
    uint8_t                        lend_src;       /**< Slot zephyr_exec_lend() uses.            */
} zephyr_exec_cfg_t;

/**
 * @brief Initialize the executor and start its thread and 1 ms timer.
 *        Call once, before any producer runs.
 * @return UDS_STATUS_OK; UDS_STATUS_ERR_NULL_PTR; UDS_STATUS_ERR_ALREADY_INITIALIZED;
 *         UDS_STATUS_ERR_INVALID_PARAM (see uds_exec_init(), or lend_src is
 *         not one of the producers).
 */
uds_status_t zephyr_exec_start(const zephyr_exec_cfg_t *cfg);

/** @brief The executor, for uds_exec_submit() and the other producer calls. */
uds_exec_t *zephyr_exec_get(void);

/**
 * @brief Producer: wait until its request or call has run.
 * @return true when done (take the response, then uds_exec_release());
 *         false on timeout (still call uds_exec_release(), contract 1).
 */
bool zephyr_exec_wait(uint8_t src, k_timeout_t timeout);

/** @brief Transitional DoIP lock callback: park the executor (see above). */
void zephyr_exec_lend(void);

/** @brief Transitional DoIP unlock callback: unpark it and release the slot. */
void zephyr_exec_lend_end(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_EXEC_H */
