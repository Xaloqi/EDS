// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/freertos_exec.h
 *
 * PURPOSE: [EDS#197] FreeRTOS binding of the diagnostic executor
 *          (core/uds_exec.h): the task that owns the UDS server.
 *
 *          - One static task (xTaskCreateStatic) runs uds_exec_run_once().
 *          - It is woken by a task notification (xTaskNotify, eSetBits)
 *            from producers, and otherwise every tick: it waits with a
 *            one-tick timeout, so it needs no software timer
 *            (configUSE_TIMERS may be 0). The UDS timers run once per
 *            elapsed ms whatever the tick rate (eds_elapsed).
 *          - Completion goes back the same way: one notification bit per
 *            producer slot, set on the task waiting in freertos_exec_wait().
 *          - The critical section is taskENTER_CRITICAL()/taskEXIT_CRITICAL().
 *          - after_pass, if set, runs on the executor task after every pass
 *            (supervision; the Zephyr example feeds its watchdog there).
 *          No queue, no mutex, and no new generic primitive (diag_queue_t,
 *          diag_lock_t): native primitives only, as for the Zephyr binding.
 *
 *  Notification bits, on the default index (configTASK_NOTIFICATION_ARRAY_ENTRIES
 *  may be 1): the executor task uses bit 0 (wake); a producer task uses bit
 *  8 + slot (its completion, or a frame routed to it). A producer task must
 *  not use these bits for anything else.
 *
 *  Producer contracts (uds_exec.h):
 *    1. After a submit or a call, uds_exec_release() on every exit path:
 *       response sent, send error, disconnect, timeout. freertos_exec_wait()
 *       returning false (a timeout) is such an exit path.
 *    2. A posted slot's buffers are the executor's until it is done with
 *       them; receive the next request into a buffer of your own.
 *
 *  DoIP (EDS#197 J6): eds_doip_set_executor(freertos_exec_get(),
 *  freertos_exec_wait_event) makes the DoIP task a producer.
 *
 * SAFETY  : No malloc. No recursion. One executor per image (static state).
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef FREERTOS_EXEC_H
#define FREERTOS_EXEC_H

#include "uds_exec.h"
#include "uds_server.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Executor task stack, in words. Every UDS dispatch runs on it (EDS#197 J6):
 *  4096 bytes, as the Zephyr binding and the DoIP task. */
#ifndef EDS_EXEC_STACK_WORDS
#define EDS_EXEC_STACK_WORDS  (1024U)
#endif

/** Executor task priority: the tick task's it replaces, below the DoIP task (6). */
#ifndef EDS_EXEC_PRIORITY
#define EDS_EXEC_PRIORITY     (5U)
#endif

/** What the application gives the binding. */
typedef struct freertos_exec_cfg {
    uds_server_ctx_t              *srv;            /**< Initialized server. Required.             */
    /** Sources and periodic_tx; notify and notify_arg are the binding's own. */
    const uds_exec_producer_cfg_t *producers;
    uint8_t                        producer_count;
    void                         (*reset)(uint8_t reset_type); /**< Optional (uds_exec_cfg_t).   */
    void                         (*after_pass)(void);          /**< Optional, executor task.     */
} freertos_exec_cfg_t;

/**
 * @brief Initialize the executor and create its task. Call once, before
 *        vTaskStartScheduler() or before any producer runs.
 * @return UDS_STATUS_OK; UDS_STATUS_ERR_NULL_PTR; UDS_STATUS_ERR_ALREADY_INITIALIZED;
 *         UDS_STATUS_ERR_INVALID_PARAM (see uds_exec_init()).
 */
uds_status_t freertos_exec_start(const freertos_exec_cfg_t *cfg);

/** @brief The executor, for uds_exec_submit() and the other producer calls. */
uds_exec_t *freertos_exec_get(void);

/**
 * @brief Producer task: wait until its request or call has run.
 * @return true when done (take the response, then uds_exec_release());
 *         false on timeout (still call uds_exec_release(), contract 1).
 */
bool freertos_exec_wait(uint8_t src, TickType_t timeout);

/**
 * @brief Producer task: wait until the executor signals src (its request or
 *        call ran, or a frame was routed to it), or timeout_ms. An early
 *        return is possible; check the slot again. eds_doip_exec_wait_cb_t.
 * @return true if signalled.
 */
bool freertos_exec_wait_event(uint8_t src, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_EXEC_H */
