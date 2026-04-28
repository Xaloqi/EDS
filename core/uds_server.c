// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_server.c
 *
 * PURPOSE: UDS server — request dispatcher and response coordinator.
 *
 * PHASE-2 FIXES APPLIED:
 *   [P2-SRV-01] Session access rights validated before dispatch.
 *   [P2-SRV-02] Security access rights validated before dispatch.
 *   [P2-SRV-03] suppressPosRspMsgIndicationBit checked at dispatch level.
 *   [P2-SRV-04] Full uds_status_t → NRC mapping table.
 *   [P2-SRV-05] S3server timer reset on every valid incoming request.
 *
 * SAFETY  : Safety-relevant. ASIL-B candidate per ISO 26262-6.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_server.h"
#include "uds_types.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_access_table.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helper prototypes
 * -------------------------------------------------------------------------- */

static const uds_service_entry_t *srv_find_handler(
    const uds_server_ctx_t *ctx,
    uint8_t                 service_id);

static uds_status_t srv_validate_request(const uds_msg_buf_t *req);

/**
 * [P2-SRV-04] Map uds_status_t internal error code to ISO 14229-1 NRC byte.
 */
static uds_nrc_t srv_status_to_nrc(uds_status_t status);

/**
 * [P2-SRV-01/02] Validate session and security constraints for the service.
 *
 * Returns UDS_STATUS_OK if the current session and security context allow
 * the service to be executed. Returns an error status (which maps to a
 * specific NRC) if access is denied.
 */
static uds_status_t srv_check_access_rights(
    uds_server_ctx_t         *ctx,
    const uds_service_entry_t *entry,
    const uds_msg_buf_t       *req);

/* --------------------------------------------------------------------------
 * Sub-function suppress-bit mask (ISO 14229-1 §7.5.2.4)
 * -------------------------------------------------------------------------- */

/** Bit 7 of the sub-function byte: set → suppress positive response. */
#define UDS_SUB_FUNCTION_SUPPRESS_BIT  (0x80U)

/** Mask to extract the actual sub-function value (bits 6..0). */
#define UDS_SUB_FUNCTION_VALUE_MASK    (0x7FU)

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t uds_server_init(
    uds_server_ctx_t       *ctx,
    const uds_server_cfg_t *cfg)
{
    if ((ctx == NULL) || (cfg == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (ctx->initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    if ((cfg->session_ctx == NULL) || (cfg->security_ctx == NULL)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if ((cfg->service_table == NULL) && (cfg->service_table_count > (uint8_t)0U)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (cfg->service_table_count > (uint8_t)UDS_SERVER_MAX_HANDLERS) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    (void)memcpy(&ctx->cfg, cfg, sizeof(uds_server_cfg_t));
    (void)memset(&ctx->tx_buf, 0, sizeof(uds_msg_buf_t));
    (void)memset(&ctx->rx_buf, 0, sizeof(uds_msg_buf_t));

    ctx->request_count           = 0U;
    ctx->negative_response_count = 0U;
    ctx->initialized             = true;

    return UDS_STATUS_OK;
}

uds_status_t uds_server_process_request(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t                status;
    uint8_t                     service_id;
    const uds_service_entry_t  *entry;
    uds_nrc_t                   nrc;
    bool                        suppress;
    uint8_t                     raw_sub_fn;

    if ((ctx == NULL) || (req == NULL) || (resp == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    status = srv_validate_request(req);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    ctx->request_count++;

    service_id = req->data[0];

    /* Locate registered handler. */
    entry = srv_find_handler(ctx, service_id);
    if (entry == NULL) {
        ctx->negative_response_count++;
        nrc = UDS_NRC_SERVICE_NOT_SUPPORTED;
        return uds_server_build_negative_response(service_id, nrc, resp);
    }

    /* [P2-SRV-05] Reset S3 timer on any recognised service request.       */
    (void)uds_session_reset_s3_timer(ctx->cfg.session_ctx);

    /* [P2-SRV-03] Extract suppressPosRspMsgIndicationBit before dispatch. */
    suppress = false;
    if (entry->suppress_pos_response_supported && (req->length >= (uint16_t)2U)) {
        raw_sub_fn = req->data[1];
        suppress   = ((raw_sub_fn & (uint8_t)UDS_SUB_FUNCTION_SUPPRESS_BIT) != (uint8_t)0U);
    }

    /* [P2-SRV-01/02] Validate session + security access rights.           */
    status = srv_check_access_rights(ctx, entry, req);
    if (status != UDS_STATUS_OK) {
        ctx->negative_response_count++;
        nrc = srv_status_to_nrc(status);
        return uds_server_build_negative_response(service_id, nrc, resp);
    }

    /* Dispatch to handler. */
    status = entry->handler(ctx, req, resp);

    if (status != UDS_STATUS_OK) {
        ctx->negative_response_count++;
        nrc = srv_status_to_nrc(status);
        return uds_server_build_negative_response(service_id, nrc, resp);
    }

    /*
     * [P2-SRV-03] Suppress positive response if bit 7 was set.
     * Signal to caller by setting resp->length to 0 — caller must
     * not transmit a zero-length response.
     */
    if (suppress) {
        resp->length = (uint16_t)0U;
    }

    return UDS_STATUS_OK;
}

uds_status_t uds_server_tick_1ms(uds_server_ctx_t *ctx)
{
    uds_status_t status;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    status = uds_session_tick_1ms(ctx->cfg.session_ctx);
    if (status != UDS_STATUS_OK) {
        /*
         * Session timeout is meaningful to the caller — propagate it.
         * The caller (integration task) uses ERR_SESSION_TIMEOUT to know
         * that any in-progress operation must be aborted cleanly.
         * Security tick still runs even on timeout (lockout must expire).
         */
        (void)uds_security_tick_1ms(ctx->cfg.security_ctx);
        return status;
    }

    status = uds_security_tick_1ms(ctx->cfg.security_ctx);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    return UDS_STATUS_OK;
}

uds_status_t uds_server_build_negative_response(
    uint8_t        service_id,
    uds_nrc_t      nrc,
    uds_msg_buf_t *resp)
{
    if (resp == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    resp->data[0] = (uint8_t)UDS_SID_NEGATIVE_RESPONSE;
    resp->data[1] = service_id;
    resp->data[2] = (uint8_t)nrc;
    resp->length  = (uint16_t)3U;

    return UDS_STATUS_OK;
}

uds_status_t uds_server_get_counters(
    const uds_server_ctx_t *ctx,
    uint32_t               *out_request_count,
    uint32_t               *out_neg_response_count)
{
    if ((ctx == NULL) || (out_request_count == NULL) || (out_neg_response_count == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    *out_request_count      = ctx->request_count;
    *out_neg_response_count = ctx->negative_response_count;

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Internal helper implementations
 * -------------------------------------------------------------------------- */

static const uds_service_entry_t *srv_find_handler(
    const uds_server_ctx_t *ctx,
    uint8_t                 service_id)
{
    uint8_t i;

    for (i = (uint8_t)0U; i < ctx->cfg.service_table_count; i++) {
        if (ctx->cfg.service_table[i].service_id == service_id) {
            return &ctx->cfg.service_table[i];
        }
    }

    return NULL;
}

static uds_status_t srv_validate_request(const uds_msg_buf_t *req)
{
    if (req == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (req->length == (uint16_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (req->length > (uint16_t)UDS_MAX_PAYLOAD_LEN) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    return UDS_STATUS_OK;
}

/* [P2-SRV-04] Complete status → NRC mapping per ISO 14229-1 Annex A. */
static uds_nrc_t srv_status_to_nrc(uds_status_t status)
{
    switch (status) {
        case UDS_STATUS_ERR_INVALID_PARAM:
            return UDS_NRC_INCORRECT_MSG_LEN_OR_FORMAT;

        case UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED:
            return UDS_NRC_SERVICE_NOT_SUPPORTED;

        case UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP:
            return UDS_NRC_SUBFUNCTION_NOT_SUPPORTED;

        case UDS_STATUS_ERR_SESSION_INVALID:
        case UDS_STATUS_ERR_SESSION_TRANSITION:
            return UDS_NRC_CONDITIONS_NOT_CORRECT;

        case UDS_STATUS_ERR_SESSION_TIMEOUT:
            return UDS_NRC_SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION;

        case UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION:
            return UDS_NRC_SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION;

        case UDS_STATUS_ERR_SEC_ACCESS_DENIED:
        case UDS_STATUS_ERR_SEC_NOT_UNLOCKED:
            return UDS_NRC_SECURITY_ACCESS_DENIED;

        case UDS_STATUS_ERR_SEC_INVALID_KEY:
            return UDS_NRC_INVALID_KEY;

        case UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED:
            return UDS_NRC_EXCEEDED_NUM_OF_ATTEMPTS;

        case UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE:
            return UDS_NRC_REQUEST_SEQUENCE_ERROR;     /* 0x24 per ISO 14229-1 §10.1.2 */

        case UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE:
            return UDS_NRC_REQUEST_OUT_OF_RANGE;

        case UDS_STATUS_ERR_BUFFER_OVERFLOW:
        case UDS_STATUS_ERR_TP_OVERFLOW:
            return UDS_NRC_INCORRECT_MSG_LEN_OR_FORMAT;

        case UDS_STATUS_ERR_DID_NOT_FOUND:
        case UDS_STATUS_ERR_DID_READ_FAILED:
        case UDS_STATUS_ERR_DID_WRITE_FAILED:
            return UDS_NRC_REQUEST_OUT_OF_RANGE;

        case UDS_STATUS_ERR_ROUTINE_NOT_FOUND:
            return UDS_NRC_REQUEST_OUT_OF_RANGE;         /* NRC 0x31 */

        case UDS_STATUS_ERR_ROUTINE_FAILED:
            return UDS_NRC_CONDITIONS_NOT_CORRECT;       /* NRC 0x22 */

        case UDS_STATUS_ERR_ROUTINE_NOT_SUPPORTED_IN_SESSION:
            return UDS_NRC_SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION; /* NRC 0x7F */

        /* --- Download / Transfer service mappings (0x34/0x36/0x37) --- */

        case UDS_STATUS_ERR_TRANSFER_WRONG_SEQ:
            return UDS_NRC_WRONG_BLOCK_SEQUENCE_COUNTER;  /* NRC 0x73 */

        case UDS_STATUS_ERR_TRANSFER_ABORTED:
            return UDS_NRC_GENERAL_PROG_FAILURE;           /* NRC 0x72 */

        case UDS_STATUS_ERR_UPLOAD_DOWNLOAD_NOT_ACCEPTED:
            return UDS_NRC_UPLOAD_DOWNLOAD_NOT_ACCEPTED;   /* NRC 0x70 */

        /*
         * UDS_STATUS_ERR_PLATFORM is reused for flash write/verify failures
         * and erase failures in the download services:
         *   service_0x34: erase failure        → NRC 0x70
         *   service_0x36: write failure         → NRC 0x72
         *   service_0x37: write/verify failure  → NRC 0x72
         *
         * Map to NRC 0x72 (generalProgrammingFailure) as the most descriptive
         * catch-all for flash driver errors.
         */
        case UDS_STATUS_ERR_PLATFORM:
            return UDS_NRC_GENERAL_PROG_FAILURE;           /* NRC 0x72 */

        /*
         * UDS_STATUS_ERR_TP_UNEXPECTED_PDU is reused for wrong block sequence
         * counter in service_0x36 to avoid adding a new status code.
         */
        case UDS_STATUS_ERR_TP_UNEXPECTED_PDU:
            return UDS_NRC_WRONG_BLOCK_SEQUENCE_COUNTER;  /* NRC 0x73 */

        default:
            return UDS_NRC_CONDITIONS_NOT_CORRECT;
    }
}

/*
 * [P5-ACL-01] Data-driven access rights check.
 *
 * Replaces the hardcoded switch introduced in Phase 2. The server now
 * consults an access rights table — either the caller-supplied custom table
 * (cfg.access_table / cfg.access_table_count) or the built-in ISO 14229-1
 * default table (uds_access_table_get_default()).
 *
 * Flow:
 *   1. Select table: use cfg.access_table if provided, else default.
 *   2. Look up (service_id, active_session) in the table.
 *   3. No entry found → access granted (permissive default).
 *   4. Entry found, session_mask does NOT cover active session
 *      → NRC 0x7F serviceNotSupportedInActiveSession.
 *   5. require_unlocked=true and required security level is not active
 *      → NRC 0x33 securityAccessDenied.
 *   6. All checks pass → dispatch handler.
 *
 * OEM CUSTOMISATION:
 *   Pass a custom uds_access_entry_t[] via uds_server_cfg_t.access_table.
 *   The stack never needs to be recompiled to change access policy.
 */
static uds_status_t srv_check_access_rights(
    uds_server_ctx_t          *ctx,
    const uds_service_entry_t *entry,
    const uds_msg_buf_t       *req)
{
    uds_session_type_t         active_session;
    uds_status_t               rc;
    const uds_access_entry_t  *acl_entry;
    const uds_access_entry_t  *acl_table;
    uint8_t                    acl_count;
    uint8_t                    session_bit;

    (void)req; /* reserved for future per-DID gating */

    rc = uds_session_get_active(ctx->cfg.session_ctx, &active_session);
    if (rc != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* [P5-ACL-01] Select table: custom (if provided) else built-in default. */
    if ((ctx->cfg.access_table != NULL) && (ctx->cfg.access_table_count > (uint8_t)0U)) {
        acl_table = ctx->cfg.access_table;
        acl_count = ctx->cfg.access_table_count;
    } else {
        acl_table = uds_access_table_get_default();
        acl_count = (uint8_t)UDS_ACCESS_TABLE_DEFAULT_COUNT;
    }

    /* Look up (service_id, active_session) in the table. */
    rc = uds_access_table_lookup(acl_table, acl_count,
                                  entry->service_id, active_session,
                                  &acl_entry);
    if (rc != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    if (acl_entry == NULL) {
        /* No entry in table — no restriction. */
        return UDS_STATUS_OK;
    }

    /*
     * Entry found. Verify that the active session is covered by the entry's
     * session_mask. The lookup populates acl_entry on any service_id match,
     * even when the session bit is missing — that signals "wrong session".
     */
    session_bit = (uint8_t)((uint8_t)1U << ((uint8_t)active_session - (uint8_t)1U));
    if ((acl_entry->session_mask & session_bit) == (uint8_t)0U) {
        return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION;
    }

    /* Enforce security level requirement. */
    rc = uds_access_table_enforce(acl_entry, ctx->cfg.security_ctx);
    if (rc != UDS_STATUS_OK) {
        if (rc == UDS_STATUS_ERR_SEC_NOT_UNLOCKED) {
            return UDS_STATUS_ERR_SEC_ACCESS_DENIED;
        }
        return rc;
    }

    return UDS_STATUS_OK;
}
