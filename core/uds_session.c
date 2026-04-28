// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_session.c
 *
 * PURPOSE: UDS session management — state machine, S3server timeout, session
 *          transition matrix, and change-notification callbacks.
 *
 * PHASE-2 FIXES APPLIED:
 *   [P2-SESS-01] Full ISO 14229-1 session transition matrix enforced.
 *   [P2-SESS-02] Session change notification callback support added.
 *   [P2-SESS-03] on_session_change callback field added to ctx (binary compat
 *                maintained — new field appended after existing members).
 *
 * SAFETY  : Safety-relevant. ASIL-B candidate per ISO 26262-6.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_session.h"
#include "uds_types.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Validate that a session type value is within the defined enumeration.
 */
static bool session_type_is_valid(uds_session_type_t session);

/**
 * @brief ISO 14229-1 Table 6 — session transition matrix.
 *
 * Returns UDS_STATUS_OK if the transition from current_session to
 * new_session is permitted.
 *
 * Rules encoded (conservative — any OEM may tighten further):
 *   ANY  → DEFAULT         : always permitted (unconditional return to safe state)
 *   DEFAULT → PROGRAMMING  : permitted (tester initiates programming)
 *   DEFAULT → EXTENDED     : permitted
 *   DEFAULT → SAFETY_SYS   : permitted (requires separate security access)
 *   PROGRAMMING → DEFAULT  : permitted
 *   EXTENDED    → DEFAULT  : permitted
 *   EXTENDED    → PROGRAMMING : permitted (per ISO 14229-1)
 *   EXTENDED    → EXTENDED   : permitted (refresh / idempotent)
 *   PROGRAMMING → PROGRAMMING: permitted (refresh)
 *   SAFETY_SYS  → DEFAULT    : permitted
 *   All other cross-transitions: rejected with ERR_SESSION_TRANSITION.
 */
static uds_status_t session_validate_transition(
    uds_session_type_t current_session,
    uds_session_type_t new_session);

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t uds_session_init(
    uds_session_ctx_t *ctx,
    uint32_t           s3_timeout_ms)
{
    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (ctx->initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    if (s3_timeout_ms == 0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    (void)memset(ctx, 0, sizeof(uds_session_ctx_t));

    ctx->active_session       = UDS_SESSION_DEFAULT;
    ctx->s3_timeout_cfg_ms    = s3_timeout_ms;
    ctx->s3_timer_ms          = s3_timeout_ms;
    ctx->session_change_count = 0U;
    ctx->on_session_change    = NULL;   /* [P2-SESS-02] no callback by default */
    ctx->initialized          = true;

    return UDS_STATUS_OK;
}

uds_status_t uds_session_transition(
    uds_session_ctx_t  *ctx,
    uds_session_type_t  new_session)
{
    uds_status_t       rc;
    uds_session_type_t old_session;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (!session_type_is_valid(new_session)) {
        return UDS_STATUS_ERR_SESSION_INVALID;
    }

    /* [P2-SESS-01] Enforce ISO 14229-1 transition matrix. */
    rc = session_validate_transition(ctx->active_session, new_session);
    if (rc != UDS_STATUS_OK) {
        return rc;
    }

    old_session         = ctx->active_session;
    ctx->active_session = new_session;
    ctx->session_change_count++;

    /* Reset S3 timer on successful transition. */
    ctx->s3_timer_ms = ctx->s3_timeout_cfg_ms;

    /* [P2-SESS-02] Notify application of session change. */
    if (ctx->on_session_change != NULL) {
        ctx->on_session_change(old_session, new_session);
    }

    return UDS_STATUS_OK;
}

uds_status_t uds_session_get_active(
    const uds_session_ctx_t *ctx,
    uds_session_type_t      *out_session)
{
    if ((ctx == NULL) || (out_session == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    *out_session = ctx->active_session;

    return UDS_STATUS_OK;
}

uds_status_t uds_session_reset_s3_timer(uds_session_ctx_t *ctx)
{
    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    ctx->s3_timer_ms = ctx->s3_timeout_cfg_ms;

    return UDS_STATUS_OK;
}

uds_status_t uds_session_register_change_cb(
    uds_session_ctx_t           *ctx,
    uds_session_change_notify_fn cb)
{
    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* NULL is a valid value here (deregisters the callback). */
    ctx->on_session_change = cb;

    return UDS_STATUS_OK;
}

uds_status_t uds_session_tick_1ms(uds_session_ctx_t *ctx)
{
    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* S3 timer is only active in non-default sessions. */
    if (ctx->active_session == UDS_SESSION_DEFAULT) {
        return UDS_STATUS_OK;
    }

    if (ctx->s3_timer_ms > 0U) {
        ctx->s3_timer_ms--;
    }

    if (ctx->s3_timer_ms == 0U) {
        uds_session_type_t old_session = ctx->active_session;

        /* Timeout — force return to default session (ISO 14229-1 requirement). */
        ctx->active_session = UDS_SESSION_DEFAULT;
        ctx->s3_timer_ms    = ctx->s3_timeout_cfg_ms;

        /* Notify application. */
        if (ctx->on_session_change != NULL) {
            ctx->on_session_change(old_session, UDS_SESSION_DEFAULT);
        }

        return UDS_STATUS_ERR_SESSION_TIMEOUT;
    }

    return UDS_STATUS_OK;
}

bool uds_session_is_default(const uds_session_ctx_t *ctx)
{
    if (ctx == NULL) {
        /* SAFETY: Fail to safe default. */
        return true;
    }

    if (!ctx->initialized) {
        return true;
    }

    return (ctx->active_session == UDS_SESSION_DEFAULT);
}

/* --------------------------------------------------------------------------
 * Internal helper implementations
 * -------------------------------------------------------------------------- */

static bool session_type_is_valid(uds_session_type_t session)
{
    switch (session) {
        case UDS_SESSION_DEFAULT:
        case UDS_SESSION_PROGRAMMING:
        case UDS_SESSION_EXTENDED:
        case UDS_SESSION_SAFETY_SYSTEM:
            return true;
        default:
            return false;
    }
}

/* [P2-SESS-01] ISO 14229-1 §7.4.2.3 session transition validation. */
static uds_status_t session_validate_transition(
    uds_session_type_t current_session,
    uds_session_type_t new_session)
{
    /* Unconditional: any session can always return to DEFAULT. */
    if (new_session == UDS_SESSION_DEFAULT) {
        return UDS_STATUS_OK;
    }

    switch (current_session) {
        case UDS_SESSION_DEFAULT:
            /* DEFAULT can enter PROGRAMMING, EXTENDED, or SAFETY_SYSTEM. */
            if ((new_session == UDS_SESSION_PROGRAMMING) ||
                (new_session == UDS_SESSION_EXTENDED)    ||
                (new_session == UDS_SESSION_SAFETY_SYSTEM)) {
                return UDS_STATUS_OK;
            }
            break;

        case UDS_SESSION_PROGRAMMING:
            /* PROGRAMMING may refresh itself. All other non-DEFAULT transitions
             * require prior return to DEFAULT per ISO 14229-1 §7.4.2.3. */
            if (new_session == UDS_SESSION_PROGRAMMING) {
                return UDS_STATUS_OK;
            }
            break;

        case UDS_SESSION_EXTENDED:
            /* EXTENDED may refresh itself or enter PROGRAMMING. */
            if ((new_session == UDS_SESSION_EXTENDED) ||
                (new_session == UDS_SESSION_PROGRAMMING)) {
                return UDS_STATUS_OK;
            }
            break;

        case UDS_SESSION_SAFETY_SYSTEM:
            /* SAFETY_SYSTEM may refresh itself. */
            if (new_session == UDS_SESSION_SAFETY_SYSTEM) {
                return UDS_STATUS_OK;
            }
            break;

        default:
            break;
    }

    return UDS_STATUS_ERR_SESSION_TRANSITION;
}
