// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x27.c
 *
 * PURPOSE: SID 0x27 — SecurityAccess service handler.
 *
 * PHASE-2 FIXES APPLIED:
 *   [P2-0x27-01] Full security module error → NRC mapping (ISO 14229-1 Annex A).
 *   [P2-0x27-02] Session constraint enforced: SecurityAccess requires
 *                Extended or Programming session (server dispatcher also
 *                enforces this via srv_check_access_rights; double-checked
 *                here for defence-in-depth).
 *   [P2-0x27-03] RequestSequenceError (NRC 0x24) returned if key submitted
 *                without prior seed request.
 *
 * SAFETY  : Safety-critical path. ASIL-B candidate per ISO 26262-6.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_security.h"
#include "uds_session.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/** Minimum seed request length: [SID, subFunction] = 2 bytes. */
#define SVC_0x27_SEED_MIN_LEN  (2U)

/** Minimum key submission length: [SID, subFunction, key...] = 3 bytes. */
#define SVC_0x27_KEY_MIN_LEN   (3U)

/* --------------------------------------------------------------------------
 * SID 0x27 handler implementation
 * -------------------------------------------------------------------------- */

/**
 * Seed request  : [0x27, oddSubFn]                 → [0x67, oddSubFn, seed...]
 * Key submission: [0x27, evenSubFn, key...]         → [0x67, evenSubFn]
 */
uds_status_t uds_service_0x27_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t       status;
    uint8_t            sub_function;
    uint8_t            seed_buf[UDS_SECURITY_SEED_LEN];
    uint8_t            seed_len;
    bool               is_seed_request;
    uds_session_type_t active_session;

    status = uds_service_validate_length(req, (uint16_t)SVC_0x27_SEED_MIN_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    sub_function = req->data[1];

    /* [P2-0x27-02] Session constraint: SecurityAccess not allowed in DEFAULT.
     * The server dispatcher already enforces this, but we double-check here
     * as defence-in-depth (ASIL-B requirement for safety-critical path). */
    status = uds_session_get_active(ctx->cfg.session_ctx, &active_session);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }
    if (active_session == UDS_SESSION_DEFAULT) {
        return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION;
    }

    /* Odd sub-function = seed request; even = key submission. */
    is_seed_request = ((sub_function & (uint8_t)0x01U) != (uint8_t)0U);

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_SECURITY_ACCESS, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->data[1] = sub_function;

    if (is_seed_request) {
        /* ---------- Seed request path ---------- */
        status = uds_security_request_seed(
            ctx->cfg.security_ctx,
            sub_function,
            seed_buf,
            (uint8_t)sizeof(seed_buf),
            &seed_len);

        if (status != UDS_STATUS_OK) {
            /* [P2-0x27-01] Return error; server dispatch maps to correct NRC. */
            return status;
        }

        if (((uint16_t)2U + (uint16_t)seed_len) > (uint16_t)UDS_MAX_PAYLOAD_LEN) {
            return UDS_STATUS_ERR_BUFFER_OVERFLOW;
        }

        {
            uint8_t i;
            for (i = (uint8_t)0U; i < seed_len; i++) {
                resp->data[2U + i] = seed_buf[i];
            }
        }

        resp->length = (uint16_t)(2U + (uint16_t)seed_len);

    } else {
        /* ---------- Key submission path ---------- */
        const uint8_t *key;
        uint8_t        key_len;

        status = uds_service_validate_length(req, (uint16_t)SVC_0x27_KEY_MIN_LEN);
        if (status != UDS_STATUS_OK) {
            return status;
        }

        key     = &req->data[2];
        key_len = (uint8_t)(req->length - (uint16_t)2U);

        status = uds_security_send_key(
            ctx->cfg.security_ctx,
            sub_function,
            key,
            key_len);

        if (status != UDS_STATUS_OK) {
            /* [P2-0x27-01] Return error; server dispatch maps to correct NRC. */
            return status;
        }

        resp->length = (uint16_t)2U;
    }

    return UDS_STATUS_OK;
}
