// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_session.h
 *
 * PURPOSE: Public API for UDS session management.
 *
 * PHASE-2 ADDITIONS:
 *   [P2-SESS-02] uds_session_change_notify_fn callback type.
 *   [P2-SESS-02] uds_session_register_change_cb() API.
 *   [P2-SESS-02] on_session_change field in uds_session_ctx_t.
 *
 * SAFETY  : Session state gates service access. ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_SESSION_H
#define UDS_SESSION_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Configuration constants
 * -------------------------------------------------------------------------- */

/** S3server default timeout in milliseconds (ISO 14229-1 default: 5000 ms). */
#ifndef UDS_SESSION_S3_TIMEOUT_MS
#define UDS_SESSION_S3_TIMEOUT_MS (5000U)
#endif

/* --------------------------------------------------------------------------
 * Session change notification callback
 *
 * [P2-SESS-02] Application callback invoked on every session transition,
 * including S3server timeout-forced returns to DEFAULT.
 * -------------------------------------------------------------------------- */

/**
 * @brief Session change notification callback type.
 *
 * @param[in] old_session  Session type before the transition.
 * @param[in] new_session  Session type after the transition.
 *
 * @note Called synchronously from uds_session_transition() and
 *       uds_session_tick_1ms() — must not call back into session API.
 */
typedef void (*uds_session_change_notify_fn)(
    uds_session_type_t old_session,
    uds_session_type_t new_session
);

/* --------------------------------------------------------------------------
 * Session context
 * -------------------------------------------------------------------------- */

/**
 * @brief UDS session management context.
 *
 * SAFETY: Caller must not modify fields directly.
 */
typedef struct uds_session_ctx {
    bool                          initialized;           /**< Initialization guard. */
    uds_session_type_t            active_session;        /**< Currently active session type. */
    uint32_t                      s3_timer_ms;           /**< S3server countdown timer (ms). */
    uint32_t                      s3_timeout_cfg_ms;     /**< Configured S3server timeout (ms). */
    uint32_t                      session_change_count;  /**< Number of session transitions. */
    uds_session_change_notify_fn  on_session_change;     /**< [P2-SESS-02] change notification callback. */
} uds_session_ctx_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the session management module.
 *
 * @param[out] ctx               Caller-allocated session context.
 * @param[in]  s3_timeout_ms     S3server timeout in milliseconds.
 *
 * @return UDS_STATUS_OK on success.
 */
uds_status_t uds_session_init(
    uds_session_ctx_t *ctx,
    uint32_t           s3_timeout_ms
);

/**
 * @brief Request a session transition.
 *
 * Enforces ISO 14229-1 §7.4.2.3 transition matrix. Returns
 * UDS_STATUS_ERR_SESSION_TRANSITION if the transition is not permitted.
 *
 * @param[in] ctx           Initialized session context.
 * @param[in] new_session   Requested target session type.
 *
 * @return UDS_STATUS_OK if transition accepted and applied.
 * @return UDS_STATUS_ERR_SESSION_TRANSITION if matrix rejects transition.
 * @return UDS_STATUS_ERR_SESSION_INVALID if new_session is not a valid type.
 */
uds_status_t uds_session_transition(
    uds_session_ctx_t  *ctx,
    uds_session_type_t  new_session
);

/**
 * @brief Register a session change notification callback.
 *
 * Callback is invoked on every transition including S3server timeout.
 * Pass NULL to deregister.
 *
 * @param[in] ctx  Initialized session context.
 * @param[in] cb   Callback function pointer (may be NULL to deregister).
 *
 * @return UDS_STATUS_OK on success.
 */
uds_status_t uds_session_register_change_cb(
    uds_session_ctx_t           *ctx,
    uds_session_change_notify_fn cb
);

/**
 * @brief Return the currently active session type.
 */
uds_status_t uds_session_get_active(
    const uds_session_ctx_t *ctx,
    uds_session_type_t      *out_session
);

/**
 * @brief Reset the S3server inactivity timer.
 */
uds_status_t uds_session_reset_s3_timer(uds_session_ctx_t *ctx);

/**
 * @brief 1 ms periodic tick for session timeout management.
 *
 * @return UDS_STATUS_ERR_SESSION_TIMEOUT if timeout occurred.
 */
uds_status_t uds_session_tick_1ms(uds_session_ctx_t *ctx);

/**
 * @brief Check if the active session is the default session.
 */
bool uds_session_is_default(const uds_session_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* UDS_SESSION_H */
