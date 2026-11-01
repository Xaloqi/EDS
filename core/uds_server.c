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
#include "uds_comm_control.h"
#include "services.h"   /* uds_service_0x34_abort_transfer() [EDS#442] */
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

/* Dispatch one request; the addressing-independent part of
 * uds_server_process_request_ex(). */
static uds_status_t srv_process(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp);

/* [EDS#444] True for a negative response that ISO 14229-1:2026 §7.7.1 says
 * shall not be sent to a functionally addressed request. */
static bool srv_is_functional_suppressed_nrc(const uds_msg_buf_t *resp);

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

/* [EDS#282] Response-pending helpers. */
static uds_status_t srv_reject_while_pending(uds_server_ctx_t    *ctx,
                                             const uds_msg_buf_t *req,
                                             uds_msg_buf_t       *resp);
static uint32_t srv_p2_star_ms(const uds_server_ctx_t *ctx);
static bool srv_keepalive_due(const uds_server_ctx_t *ctx);
static void srv_clock_add(uint32_t *clock_ms, uint32_t add_ms);
static void srv_pending_owe_nrc(uds_server_ctx_t *ctx, uds_nrc_t nrc, bool is_final);
static void srv_pending_run_step(uds_server_ctx_t *ctx);

/* --------------------------------------------------------------------------
 * Sub-function suppress-bit mask (ISO 14229-1:2026 §7.7.3, Tables 3–4)
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

    (void)memset(&ctx->pending, 0, sizeof(uds_pending_slot_t));

    ctx->request_count           = 0U;
    ctx->negative_response_count = 0U;
    ctx->rx_addressing           = UDS_ADDRESSING_PHYSICAL;
    ctx->initialized             = true;

    return UDS_STATUS_OK;
}

uds_status_t uds_server_process_request(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    return uds_server_process_request_ex(ctx, req, resp, UDS_ADDRESSING_PHYSICAL);
}

uds_status_t uds_server_process_request_ex(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp,
    uds_addressing_t     addressing)
{
    uds_status_t status;

    if (ctx != NULL) {
        ctx->rx_addressing = addressing;  /* [EDS#282] read by uds_server_defer() */
    }
    status = srv_process(ctx, req, resp);

    /* [EDS#444] ISO 14229-1:2026 §7.7.1, Table 4: with functional
     * addressing, NRC 0x11, 0x12, 0x31, 0x7E and 0x7F are never sent,
     * whatever the suppress bit. Other NRCs (0x13, 0x22, 0x78, ...) are. */
    if ((status == UDS_STATUS_OK) &&
        (addressing == UDS_ADDRESSING_FUNCTIONAL) &&
        srv_is_functional_suppressed_nrc(resp)) {
        resp->length = (uint16_t)0U;
    }

    return status;
}

static uds_status_t srv_process(
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

    /* [EDS#282] A step calling back into the server fails its job. */
    if (ctx->pending.in_step) {
        ctx->pending.reentered = true;
        return UDS_STATUS_ERR_BUSY;
    }

    ctx->request_count++;

    service_id = req->data[0];

    /* [EDS#282] One job at a time: nothing else runs while one is pending. */
    if (ctx->pending.active) {
        return srv_reject_while_pending(ctx, req, resp);
    }

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

    /* [EDS#282] The handler deferred: resp is 7F sid 78. ISO 14229-1 never
     * suppresses a negative response, and the final response follows. */
    if (ctx->pending.active) {
        return UDS_STATUS_OK;
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

    /* [EDS#282] Keepalive and cap clocks of a pending job. */
    if (ctx->pending.active) {
        srv_clock_add(&ctx->pending.since_frame_ms, 1U);
        srv_clock_add(&ctx->pending.total_ms, 1U);
    }

    status = uds_session_tick_1ms(ctx->cfg.session_ctx);
    if (status != UDS_STATUS_OK) {
        /*
         * [SEC-S3-RESET-01] An S3 timeout forces the session back to
         * DEFAULT exactly like an explicit 0x10 DiagnosticSessionControl
         * (default) request does — and that explicit path always resets
         * SecurityAccess (see uds_services/service_0x10.c). This path must
         * do the same. Without it, SecurityAccess stayed unlocked across
         * the forced return to DEFAULT: a tester that went silent long
         * enough for S3 to expire, followed by ANY tester (not necessarily
         * the same one) re-entering a non-default session with a bare
         * DiagnosticSessionControl request, inherited the previous
         * unlock — no seed/key exchange required.
         *
         * Session timeout is still meaningful to the caller — propagate it.
         * The caller (integration task) uses ERR_SESSION_TIMEOUT to know
         * that any in-progress operation must be aborted cleanly.
         * Security tick still runs even on timeout (lockout must expire).
         */
        if (status == UDS_STATUS_ERR_SESSION_TIMEOUT) {
            (void)uds_security_reset(ctx->cfg.security_ctx);
            /* [SEC-SESS-COMMRESTORE-01] Same return to DEFAULT as an
             * explicit 10 01, so the same ISO 14229-1:2026 §9.2.1 d) reset. */
            (void)uds_comm_control_restore_defaults();
            /* [EDS#282] Session ended: drop a pending job. Only reachable
             * when nothing polls it, since every frame restarts S3. */
            uds_server_pending_abort(ctx);
            /* [EDS#442] Session ended: abort any transfer. */
            uds_service_0x34_abort_transfer();
        }
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

uds_status_t uds_server_take_pending_reset(uds_server_ctx_t *ctx,
                                           uint8_t          *out_reset_type)
{
    if ((ctx == NULL) || (out_reset_type == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    *out_reset_type         = ctx->pending_reset_type;
    ctx->pending_reset_type = (uint8_t)0U;

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * [EDS#282] Response-pending jobs
 * -------------------------------------------------------------------------- */

uds_status_t uds_server_defer(uds_server_ctx_t        *ctx,
                              const uds_pending_job_t *job,
                              uds_msg_buf_t           *resp)
{
    if ((ctx == NULL) || (job == NULL) || (resp == NULL) ||
        (job->step == NULL) || (job->abort == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* A frame cannot go out in the middle of a step, so one step has to
     * fit inside P2*server_max. */
    if (job->step_max_ms >= srv_p2_star_ms(ctx)) {
        return UDS_STATUS_ERR_GENERIC;   /* NRC 0x22, not 0x13: a server fault */
    }

    /* No functional deferral in v1: N ECUs answering 0x78 to a broadcast. */
    if (ctx->rx_addressing == UDS_ADDRESSING_FUNCTIONAL) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    if (ctx->pending.active) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    (void)memset(&ctx->pending, 0, sizeof(uds_pending_slot_t));
    ctx->pending.job             = *job;
    ctx->pending.keepalive_count = 1U;   /* the one in resp */
    ctx->pending.active          = true;

    return uds_server_build_negative_response(
        job->sid, UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING, resp);
}

uds_poll_result_t uds_server_poll(uds_server_ctx_t *ctx,
                                  uds_msg_buf_t    *out_frame)
{
    uds_poll_result_t result = UDS_POLL_IDLE;

    if ((ctx == NULL) || (out_frame == NULL) || !ctx->pending.active) {
        return UDS_POLL_IDLE;
    }

    if (ctx->pending.in_step) {
        ctx->pending.reentered = true;
        return UDS_POLL_NOTHING_NOW;
    }

    if (!ctx->pending.owed) {
        if (ctx->pending.total_ms >= (uint32_t)UDS_SERVER_MAX_PENDING_MS) {
            /* Cap: abort once, answer generalReject. */
            ctx->pending.job.abort();
            srv_pending_owe_nrc(ctx, UDS_NRC_GENERAL_REJECT, true);
        } else if (srv_keepalive_due(ctx)) {
            srv_pending_owe_nrc(ctx, UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING, false);
        } else {
            srv_pending_run_step(ctx);
        }

        /* [EDS#435] S3 restarted per frame, never held. */
        if (ctx->pending.owed && (ctx->cfg.session_ctx != NULL)) {
            (void)uds_session_reset_s3_timer(ctx->cfg.session_ctx);
        }
    }

    if (ctx->pending.owed) {
        if (out_frame != &ctx->tx_buf) {
            (void)memcpy(out_frame->data, ctx->tx_buf.data, (size_t)ctx->tx_buf.length);
            out_frame->length = ctx->tx_buf.length;
        }
        result = UDS_POLL_FRAME;
    } else {
        result = UDS_POLL_NOTHING_NOW;
    }

    return result;
}

void uds_server_frame_sent(uds_server_ctx_t *ctx)
{
    if ((ctx == NULL) || !ctx->pending.active || !ctx->pending.owed) {
        return;
    }

    if (ctx->pending.owed_is_final) {
        (void)memset(&ctx->pending, 0, sizeof(uds_pending_slot_t));
    } else {
        ctx->pending.owed           = false;
        ctx->pending.since_frame_ms = 0U;
    }
}

bool uds_server_pending_active(const uds_server_ctx_t *ctx)
{
    return (ctx != NULL) && ctx->pending.active;
}

void uds_server_pending_abort(uds_server_ctx_t *ctx)
{
    if ((ctx == NULL) || !ctx->pending.active) {
        return;
    }

    /* A final frame owed means the job already ended: nothing to abort. */
    if (!ctx->pending.owed_is_final) {
        ctx->pending.job.abort();
    }
    (void)memset(&ctx->pending, 0, sizeof(uds_pending_slot_t));
}

void uds_server_pending_release(uds_server_ctx_t *ctx)
{
    if (ctx != NULL) {
        (void)memset(&ctx->pending, 0, sizeof(uds_pending_slot_t));
    }
}

/* --------------------------------------------------------------------------
 * Internal helper implementations
 * -------------------------------------------------------------------------- */

/* [EDS#282] While a job is pending (AUTOSAR Dcm's default): 3E 80 is
 * accepted silently in any addressing, a functional request is dropped,
 * and any other physical request, 0x10 and 0x11 included, gets NRC 0x21
 * with no state touched. */
static uds_status_t srv_reject_while_pending(uds_server_ctx_t    *ctx,
                                             const uds_msg_buf_t *req,
                                             uds_msg_buf_t       *resp)
{
    if ((req->length == (uint16_t)2U) &&
        (req->data[0] == (uint8_t)UDS_SID_TESTER_PRESENT) &&
        (req->data[1] == (uint8_t)0x80U)) {
        (void)uds_session_reset_s3_timer(ctx->cfg.session_ctx);
        resp->length = (uint16_t)0U;
        return UDS_STATUS_OK;
    }

    if (ctx->rx_addressing == UDS_ADDRESSING_FUNCTIONAL) {
        resp->length = (uint16_t)0U;
        return UDS_STATUS_OK;
    }

    ctx->negative_response_count++;
    return uds_server_build_negative_response(
        req->data[0], UDS_NRC_BUSY_REPEAT_REQUEST, resp);
}

static uint32_t srv_p2_star_ms(const uds_server_ctx_t *ctx)
{
    uint32_t p2_star = ctx->cfg.p2_star_server_max_ms;

    if (p2_star == 0U) {
        p2_star = (uint32_t)UDS_SERVER_DEFAULT_P2_STAR_MS;
    }
    return p2_star;
}

static bool srv_keepalive_due(const uds_server_ctx_t *ctx)
{
    uint32_t interval_ms = (uint32_t)UDS_SERVER_PENDING_KEEPALIVE_MS;

    if (interval_ms == 0U) {
        interval_ms = srv_p2_star_ms(ctx) / 2U;
    }
    /* Each 0x78 restarts S3, so the keepalive must also beat S3. Clamped
     * rather than refused at init: a short S3 is a valid configuration. */
    if ((ctx->cfg.session_ctx != NULL) &&
        (ctx->cfg.session_ctx->s3_timeout_cfg_ms > 0U) &&
        (interval_ms > (ctx->cfg.session_ctx->s3_timeout_cfg_ms / 2U))) {
        interval_ms = ctx->cfg.session_ctx->s3_timeout_cfg_ms / 2U;
    }
    return ctx->pending.since_frame_ms >= interval_ms;
}

static void srv_clock_add(uint32_t *clock_ms, uint32_t add_ms)
{
    if (*clock_ms > (UINT32_MAX - add_ms)) {
        *clock_ms = UINT32_MAX;
    } else {
        *clock_ms += add_ms;
    }
}

/* Put 7F sid nrc in tx_buf and owe it. */
static void srv_pending_owe_nrc(uds_server_ctx_t *ctx, uds_nrc_t nrc, bool is_final)
{
    (void)uds_server_build_negative_response(ctx->pending.job.sid, nrc, &ctx->tx_buf);
    ctx->pending.owed          = true;
    ctx->pending.owed_is_final = is_final;
    if (!is_final) {
        ctx->pending.keepalive_count++;
    }
}

static void srv_pending_run_step(uds_server_ctx_t *ctx)
{
    uds_step_result_t step_rc;
    uds_nrc_t         nrc = UDS_NRC_GENERAL_REJECT;

    ctx->tx_buf.length    = (uint16_t)0U;
    ctx->pending.in_step  = true;
    step_rc = ctx->pending.job.step(&ctx->tx_buf, &nrc);
    ctx->pending.in_step  = false;

    /* The tick may not have run while the step blocked the loop: credit
     * the step's declared worst case to both clocks. */
    srv_clock_add(&ctx->pending.since_frame_ms, ctx->pending.job.step_max_ms);
    srv_clock_add(&ctx->pending.total_ms, ctx->pending.job.step_max_ms);

    if (ctx->pending.reentered) {
        ctx->pending.job.abort();
        srv_pending_owe_nrc(ctx, UDS_NRC_GENERAL_REJECT, true);
        return;
    }

    switch (step_rc) {
        case UDS_STEP_IN_PROGRESS:
            if (srv_keepalive_due(ctx)) {
                srv_pending_owe_nrc(ctx, UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING, false);
            }
            break;

        case UDS_STEP_DONE:
            /* Sent even if SPRMIB was set: a 0x78 went out already. */
            if ((ctx->tx_buf.length == (uint16_t)0U) ||
                (ctx->tx_buf.length > (uint16_t)UDS_MAX_PAYLOAD_LEN)) {
                srv_pending_owe_nrc(ctx, UDS_NRC_GENERAL_REJECT, true);
            } else {
                ctx->pending.owed          = true;
                ctx->pending.owed_is_final = true;
            }
            break;

        case UDS_STEP_FAILED:
            if (nrc == UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING) {
                nrc = UDS_NRC_GENERAL_REJECT;
            }
            ctx->negative_response_count++;
            srv_pending_owe_nrc(ctx, nrc, true);
            break;

        default:
            ctx->pending.job.abort();
            srv_pending_owe_nrc(ctx, UDS_NRC_GENERAL_REJECT, true);
            break;
    }
}

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

/* [P2-SRV-04] Complete status → NRC mapping per ISO 14229-1:2026 Annex A.1. */
static bool srv_is_functional_suppressed_nrc(const uds_msg_buf_t *resp)
{
    bool suppressed = false;

    if ((resp->length == (uint16_t)3U) &&
        (resp->data[0] == (uint8_t)UDS_SID_NEGATIVE_RESPONSE)) {
        switch (resp->data[2]) {
            case (uint8_t)UDS_NRC_SERVICE_NOT_SUPPORTED:
            case (uint8_t)UDS_NRC_SUBFUNCTION_NOT_SUPPORTED:
            case (uint8_t)UDS_NRC_REQUEST_OUT_OF_RANGE:
            case (uint8_t)UDS_NRC_SUBFUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION:
            case (uint8_t)UDS_NRC_SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION:
                suppressed = true;
                break;
            default:
                suppressed = false;
                break;
        }
    }
    return suppressed;
}

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

        case UDS_STATUS_ERR_SEC_DELAY_ACTIVE:
            return UDS_NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED; /* 0x37 [EDS#446] */

        case UDS_STATUS_ERR_RESPONSE_TOO_LONG:
            return UDS_NRC_RESPONSE_TOO_LONG;               /* 0x14 [EDS#465] */

        case UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE:
        case UDS_STATUS_ERR_REQUEST_SEQUENCE:     /* 0x24 [EDS#484] */
            return UDS_NRC_REQUEST_SEQUENCE_ERROR;     /* 0x24: sendKey without requestSeed (ISO 14229-1:2026 Table 45) */

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

        case UDS_STATUS_ERR_TRANSFER_SUSPENDED:
            return UDS_NRC_TRANSFER_DATA_SUSPENDED;        /* NRC 0x71 [EDS#441] */

        /*
         * [#232] DFU image policy gate.  Both map to NRC 0x22
         * (conditionsNotCorrect) — the same NRC the `default:` arm would
         * produce, but listed explicitly so the mapping is a reviewed
         * decision rather than a fall-through, and so a future change to
         * the default arm cannot silently move them.
         *
         *   ERR_IMAGE_POLICY_ABSENT   — no policy registered on a
         *                               production build (0x34 gate, and
         *                               the 0x37 defence-in-depth re-check)
         *   ERR_IMAGE_POLICY_REJECTED — finalise_cb returned
         *                               UDS_IMAGE_REJECT_POLICY
         */
        case UDS_STATUS_ERR_IMAGE_POLICY_ABSENT:
        case UDS_STATUS_ERR_IMAGE_POLICY_REJECTED:
            return UDS_NRC_CONDITIONS_NOT_CORRECT;         /* NRC 0x22 */

        /*
         * UDS_STATUS_ERR_PLATFORM is reused for flash write/verify failures
         * in the download services (0x34 erase failure does NOT use it: it
         * returns ERR_UPLOAD_DOWNLOAD_NOT_ACCEPTED, NRC 0x70 [EDS#529]):
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
 *   3. No entry found → uds_access_table_enforce(NULL, ...) decides: DENIED
 *      by default (NRC 0x7F serviceNotSupportedInActiveSession) as of
 *      [#113], unless UDS_ACL_ALLOW_UNLISTED_SERVICES=1 opts back into the
 *      pre-#113 permissive behaviour. See uds_access_table.h/.c.
 *   4. Entry found, session_mask does NOT cover active session
 *      → NRC 0x7F serviceNotSupportedInActiveSession.
 *   5. require_unlocked=true and required security level is not active
 *      → NRC 0x33 securityAccessDenied.
 *   6. All checks pass → dispatch handler.
 *
 * OEM CUSTOMISATION:
 *   Pass a custom uds_access_entry_t[] via uds_server_cfg_t.access_table.
 *   The stack never needs to be recompiled to change access policy.
 *   [#113] Remember: with the fail-closed default, any service_id NOT
 *   covered by your custom table is now DENIED rather than silently
 *   allowed — audit your table's SID coverage.
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

    /*
     * [#113 FIX] acl_entry == NULL ("not in table") used to short-circuit
     * straight to UDS_STATUS_OK right here — a second, independent copy of
     * the fail-open "no restriction" decision that uds_access_table.c's own
     * enforce() also made. That duplication was the actual production bug:
     * even after making uds_access_table_enforce() fail-closed, THIS
     * function never called it for the acl_entry == NULL case, so real
     * dispatch (every request that reaches here) stayed permissive
     * regardless of what enforce() decided.
     *
     * Fix: only run the session_mask check when there IS a matching entry
     * (unchanged from before), then ALWAYS fall through to
     * uds_access_table_enforce() — including with acl_entry == NULL — so
     * there is exactly one place (uds_access_table_enforce(), gated by
     * UDS_ACL_ALLOW_UNLISTED_SERVICES) that decides what "not in table"
     * means. See core/uds_access_table.c / uds_access_table.h for the
     * rationale.
     */
    if (acl_entry != NULL) {
        /*
         * Entry found. Verify that the active session is covered by the
         * entry's session_mask. The lookup populates acl_entry on any
         * service_id match, even when the session bit is missing — that
         * signals "wrong session".
         */
        session_bit = (uint8_t)((uint8_t)1U << ((uint8_t)active_session - (uint8_t)1U));
        if ((acl_entry->session_mask & session_bit) == (uint8_t)0U) {
            return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION;
        }
    }

    /*
     * Enforce security level requirement — or, if acl_entry is NULL, the
     * fail-closed-by-default "not in table" decision itself (see comment
     * above and uds_access_table_enforce()'s own doc comment).
     */
    rc = uds_access_table_enforce(acl_entry, ctx->cfg.security_ctx);
    if (rc != UDS_STATUS_OK) {
        if (rc == UDS_STATUS_ERR_SEC_NOT_UNLOCKED) {
            return UDS_STATUS_ERR_SEC_ACCESS_DENIED;
        }
        return rc;
    }

    return UDS_STATUS_OK;
}
