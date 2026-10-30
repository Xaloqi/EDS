// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_periodic.h
 *
 * PURPOSE: SID 0x2A — ReadDataByPeriodicIdentifier scheduler.
 *
 *          Manages a static subscription table for periodic DID push.
 *          The tester registers subscriptions via SID 0x2A; the ECU
 *          autonomously pushes [periodicId, data...] messages at the
 *          configured rate without waiting for further tester requests.
 *
 *          Integration (every example):
 *            1. Call uds_periodic_init() once at startup, after uds_session_init().
 *            2. Register uds_periodic_cancel_all as the session-change callback:
 *                 uds_session_register_change_cb(session_ctx, s_on_session_change)
 *               where s_on_session_change calls uds_periodic_cancel_all() on
 *               transition to UDS_SESSION_DEFAULT.
 *            3. In the 1 ms tick callback, after uds_server_tick_1ms():
 *                 (void)uds_periodic_tick_1ms();
 *            4. In the main poll loop, drain all due frames:
 *                 static uds_msg_buf_t s_periodic_frame;
 *                 while (uds_periodic_pop_due(&s_periodic_frame) == UDS_STATUS_OK) {
 *                     (void)isotp_transmit(tp, s_periodic_frame.data,
 *                                         s_periodic_frame.length);
 *                 }
 *
 * SAFETY  : No dynamic allocation. Static table, deterministic execution.
 *           ASIL-B compatible.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_PERIODIC_H
#define UDS_PERIODIC_H

#include "uds_types.h"
#include "uds_session.h"
#include "uds_security.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Configurable limits (override via prj.conf / CMake -D)
 * -------------------------------------------------------------------------- */

#ifndef UDS_PERIODIC_MAX_SUBSCRIPTIONS
#define UDS_PERIODIC_MAX_SUBSCRIPTIONS  (8U)
#endif

/* --------------------------------------------------------------------------
 * Configurable push rates in milliseconds
 * -------------------------------------------------------------------------- */

#ifndef UDS_PERIODIC_SLOW_MS
#define UDS_PERIODIC_SLOW_MS   (1000U)
#endif

#ifndef UDS_PERIODIC_MEDIUM_MS
#define UDS_PERIODIC_MEDIUM_MS (100U)
#endif

#ifndef UDS_PERIODIC_FAST_MS
#define UDS_PERIODIC_FAST_MS   (10U)
#endif

/* --------------------------------------------------------------------------
 * TransmissionMode values (ISO 14229-1:2026 Annex C.4, Table C.10;
 * 0x00 and 0x05-0xFF reserved)
 * -------------------------------------------------------------------------- */

#define UDS_PERIODIC_MODE_SLOW    (0x01U)
#define UDS_PERIODIC_MODE_MEDIUM  (0x02U)
#define UDS_PERIODIC_MODE_FAST    (0x03U)
#define UDS_PERIODIC_MODE_STOP    (0x04U)
#define UDS_PERIODIC_MODE_MAX     (0x04U)

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Reset the scheduler to idle — clear all subscriptions.
 *
 * Call once at startup (after uds_session_init()) and on return to Default
 * session via the on_session_change callback.
 *
 * @return UDS_STATUS_OK always.
 */
uds_status_t uds_periodic_init(void);

/**
 * @brief Add or update a periodic subscription.
 *
 * If periodic_id is already subscribed, updates its mode and resets the
 * countdown. If the table is full, returns ERR_REQUEST_OUT_OF_RANGE.
 *
 * @param[in] periodic_id  Low byte of 0xF2xx DID (0x00–0xFF).
 * @param[in] mode         UDS_PERIODIC_MODE_SLOW / MEDIUM / FAST.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE if table is full.
 */
uds_status_t uds_periodic_subscribe(uint8_t periodic_id, uint8_t mode);

/**
 * @brief Remove a subscription by periodic_id.
 *
 * No-op if periodic_id is not currently subscribed.
 *
 * @param[in] periodic_id  The identifier to unsubscribe.
 *
 * @return UDS_STATUS_OK always.
 */
uds_status_t uds_periodic_unsubscribe(uint8_t periodic_id);

/**
 * @brief Remove all active subscriptions.
 *
 * Called on return to Default session (via on_session_change).
 *
 * @return UDS_STATUS_OK always.
 */
uds_status_t uds_periodic_cancel_all(void);

/**
 * @brief Advance all subscription timers by one millisecond.
 *
 * Called from the 1 ms tick callback, after uds_server_tick_1ms().
 * Decrements each active subscription counter; sets due flag when counter
 * reaches zero and reloads with interval_ms.
 *
 * @note Does not transmit. No blocking. Deterministic constant-time execution.
 *
 * @return UDS_STATUS_OK always.
 */
uds_status_t uds_periodic_tick_1ms(void);

/**
 * @brief Build the next due periodic push frame.
 *
 * Returns at most one frame per call. The caller loops until ERR_NOT_FOUND
 * to drain all subscriptions that fired in the same tick.
 *
 * Frame format: [periodicDataIdentifier, dataRecord...] (ISO 14229-1:2026
 * Table 221; no 0x6A: only the initial positive response carries the SID).
 *
 * @param[out] out_frame  Caller-allocated buffer to receive the frame.
 *
 * @return UDS_STATUS_OK if out_frame is populated and ready to transmit.
 * @return UDS_STATUS_ERR_NOT_FOUND if no subscription is currently due.
 * @return UDS_STATUS_ERR_NULL_PTR if out_frame is NULL.
 */
uds_status_t uds_periodic_pop_due(uds_msg_buf_t *out_frame);

/**
 * @brief [EDS#466] Number of free subscription slots.
 * @return UDS_PERIODIC_MAX_SUBSCRIPTIONS minus the active subscriptions.
 */
uint8_t uds_periodic_free_slots(void);

/**
 * @brief [EDS#466] Whether periodic_id is currently subscribed (a
 *        re-subscribe updates it in place and needs no free slot).
 */
bool uds_periodic_is_subscribed(uint8_t periodic_id);

/**
 * @brief [SEC-PDID-ACCESS-01] Bind the session/security contexts that every
 *        push re-checks the DID against.
 *
 * Called by the 0x2A handler. uds_periodic_pop_due() drops a subscription
 * whose DID fails uds_safety_validate_did_access() (e.g. security re-locked
 * by a session change). Not bound (NULL): no re-check.
 */
void uds_periodic_set_access_ctx(const uds_session_ctx_t  *session_ctx,
                                 const uds_security_ctx_t *security_ctx);

/**
 * @brief [EDS#546] Record the CAN ID the periodic messages are sent on.
 *
 * ISO 14229-3:2022 Table 3: periodic messages use a CAN ID of their own. There
 * is no default: with 0 (never set) SID 0x2A answers NRC 0x22 to a request
 * that would schedule a periodic DID, instead of sending it on the diagnostic
 * response ID. Generated uds_init.c calls this with GEN_CAN_PERIODIC_TX_ID.
 * Survives uds_periodic_init() and a return to the default session.
 */
void uds_periodic_set_tx_can_id(uint32_t can_id);

/** @brief [EDS#546] True once a non-zero periodic CAN ID has been recorded. */
bool uds_periodic_tx_can_id_is_set(void);

/**
 * @brief [EDS#194] Re-queue the subscription most recently popped by
 *        uds_periodic_pop_due() for the next tick.
 *
 * Call this when a transmit of the frame uds_periodic_pop_due() just
 * returned fails (e.g. isotp_transmit() returns BUSY mid multi-frame
 * send) — without it, that period is silently lost rather than retried,
 * since uds_periodic_pop_due() already cleared the due flag before
 * returning. Consumes the "last popped" record: calling this twice in a
 * row without an intervening successful pop is a no-op the second time
 * (ERR_NOT_FOUND) — there is only ever one frame to re-queue.
 *
 * @return UDS_STATUS_OK if the subscription was found and re-armed.
 * @return UDS_STATUS_ERR_NOT_FOUND if there is nothing to re-queue (no
 *         prior pop, already consumed, or the subscription was cancelled
 *         between the pop and this call).
 */
uds_status_t uds_periodic_requeue_last(void);

#ifdef __cplusplus
}
#endif

#endif /* UDS_PERIODIC_H */
