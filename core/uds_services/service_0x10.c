// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x10.c
 *
 * PURPOSE: SID 0x10 — DiagnosticSessionControl service handler.
 *
 * PHASE-2 FIXES APPLIED:
 *   [P2-0x10-01] suppressPosRspMsgIndicationBit (bit 7) masked from
 *                sub-function before session-type decode.
 *   [P2-0x10-02] P2server_max and P2*server_max encoded from ctx->cfg
 *                (falls back to ISO defaults if cfg values are zero).
 *   [P2-0x10-03] Security access reset on transition to DEFAULT session.
 *   [P2-0x10-04] OEM session transition matrix enforced via uds_session_transition
 *                (matrix implemented in uds_session.c [P2-SESS-01]).
 *
 * SAFETY  : Session transitions gate access to higher-privilege services.
 *           ASIL-B candidate per ISO 26262-6.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"

/* --------------------------------------------------------------------------
 * ISO 14229-1 default timing values (§7.2)
 * -------------------------------------------------------------------------- */

/** Default P2Server_max: 25 ms (0x0019). */
#define SVC_0x10_P2_DEFAULT_MS       (25U)

/** Default P2*Server_max: 5000 ms (0x01F4). */
#define SVC_0x10_P2STAR_DEFAULT_MS   (5000U)

/** suppressPosRspMsgIndicationBit position in sub-function byte. */
#define SVC_0x10_SUPPRESS_BIT        (0x80U)

/** Mask for actual session type value (bits 6..0). */
#define SVC_0x10_SUBFN_MASK          (0x7FU)

/* --------------------------------------------------------------------------
 * SID 0x10 handler implementation
 * -------------------------------------------------------------------------- */

/**
 * Request format : [0x10, sessionType]
 *   sessionType bit 7 = suppressPosRspMsgIndicationBit
 *   sessionType bits 6:0 = session sub-type
 *
 * Response format: [0x50, sessionType, P2_hi, P2_lo, P2s_hi, P2s_lo]
 *   Response is suppressed (length=0) if suppressPosRspMsgIndicationBit=1.
 */
uds_status_t uds_service_0x10_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t       status;
    uint8_t            raw_byte;
    uint8_t            session_type_byte;
    bool               suppress;
    uds_session_type_t new_session;
    uint32_t           p2_ms;
    uint32_t           p2star_ms;

    /* Minimum: [SID, subFunction] = 2 bytes. */
    status = uds_service_validate_length(req, (uint16_t)2U);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    raw_byte = req->data[1];

    /* [P2-0x10-01] Extract suppress bit BEFORE decoding session type. */
    suppress          = ((raw_byte & (uint8_t)SVC_0x10_SUPPRESS_BIT) != (uint8_t)0U);
    session_type_byte = (uint8_t)(raw_byte & (uint8_t)SVC_0x10_SUBFN_MASK);

    switch (session_type_byte) {
        case (uint8_t)UDS_SESSION_DEFAULT:
            new_session = UDS_SESSION_DEFAULT;
            break;
        case (uint8_t)UDS_SESSION_PROGRAMMING:
            new_session = UDS_SESSION_PROGRAMMING;
            break;
        case (uint8_t)UDS_SESSION_EXTENDED:
            new_session = UDS_SESSION_EXTENDED;
            break;
        case (uint8_t)UDS_SESSION_SAFETY_SYSTEM:
            new_session = UDS_SESSION_SAFETY_SYSTEM;
            break;
        default:
            return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }

    /* [P2-0x10-03] Reset security context on return to DEFAULT session. */
    if (new_session == UDS_SESSION_DEFAULT) {
        (void)uds_security_reset(ctx->cfg.security_ctx);
    }

    /* [P2-0x10-04] Transition enforces ISO 14229-1 matrix (uds_session.c). */
    status = uds_session_transition(ctx->cfg.session_ctx, new_session);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Build positive response: [0x50, sessionType, P2_hi, P2_lo, P2s_hi, P2s_lo] */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_DIAGNOSTIC_SESSION_CONTROL, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Echo the RAW sub-function byte (including suppress bit) per ISO 14229-1 §7.5.3. */
    resp->data[1] = raw_byte;

    /* [P2-0x10-02] Encode P2/P2* from server config; use ISO defaults if zero. */
    p2_ms    = ctx->cfg.p2_server_max_ms;
    p2star_ms = ctx->cfg.p2_star_server_max_ms;

    if (p2_ms == 0U) {
        p2_ms = (uint32_t)SVC_0x10_P2_DEFAULT_MS;
    }
    if (p2star_ms == 0U) {
        p2star_ms = (uint32_t)SVC_0x10_P2STAR_DEFAULT_MS;
    }

    /* P2server_max: 2 bytes big-endian (milliseconds, max 65535 ms). */
    resp->data[2] = (uint8_t)((p2_ms >> 8U) & (uint8_t)0xFFU);
    resp->data[3] = (uint8_t)(p2_ms & (uint8_t)0xFFU);

    /* P2*server_max: 2 bytes big-endian (unit = 10 ms, range 0..65535*10 ms).
     * ISO 14229-1 §7.2: P2*server_max encoded as p2star_ms / 10 to fit 16 bits.
     * Clamp to 0xFFFF to prevent truncation on very large values. */
    {
        uint32_t p2star_encoded = p2star_ms / 10U;
        if (p2star_encoded > (uint32_t)0xFFFFU) {
            p2star_encoded = (uint32_t)0xFFFFU;
        }
        resp->data[4] = (uint8_t)((p2star_encoded >> 8U) & (uint8_t)0xFFU);
        resp->data[5] = (uint8_t)(p2star_encoded & (uint8_t)0xFFU);
    }

    resp->length = (uint16_t)6U;

    /* [P2-0x10-01] Suppress positive response if bit 7 was set. */
    if (suppress) {
        resp->length = (uint16_t)0U;
    }

    return UDS_STATUS_OK;
}
