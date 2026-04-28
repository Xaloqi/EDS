// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x3E.c
 *
 * PURPOSE: SID 0x3E — TesterPresent service handler.
 *
 * PHASE-2 FIXES APPLIED:
 *   [P2-0x3E-01] suppressPosRspMsgIndicationBit (bit 7) handled:
 *                response length set to 0 when bit is set.
 *   [P2-0x3E-02] Sub-function 0x00 and 0x80 both accepted; only the
 *                lower 7 bits validated for sub-function value = 0x00.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/** Valid sub-function value (bits 6:0 must be zero). */
#define SVC_0x3E_SUBFUNCTION_ZERO        (0x00U)

/** suppressPosRspMsgIndicationBit. */
#define SVC_0x3E_SUPPRESS_BIT            (0x80U)

/** Mask for sub-function value (bits 6:0). */
#define SVC_0x3E_SUBFN_MASK              (0x7FU)

/** Minimum request length: [SID, subFunction] = 2 bytes. */
#define SVC_0x3E_MIN_REQ_LEN             (2U)

/* --------------------------------------------------------------------------
 * SID 0x3E handler implementation
 * -------------------------------------------------------------------------- */

/**
 * Request format : [0x3E, subFn]
 *   subFn bit 7  = suppressPosRspMsgIndicationBit
 *   subFn bits 6:0 must be 0x00 (only valid sub-function per ISO 14229-1)
 *
 * Response format: [0x7E, 0x00]  (length=0 if suppress bit set)
 */
uds_status_t uds_service_0x3E_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;
    uint8_t      raw_byte;
    uint8_t      sub_fn_value;
    bool         suppress;

    status = uds_service_validate_length(req, (uint16_t)SVC_0x3E_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    raw_byte = req->data[1];

    /* [P2-0x3E-01] Extract suppress bit and sub-function value separately. */
    suppress     = ((raw_byte & (uint8_t)SVC_0x3E_SUPPRESS_BIT) != (uint8_t)0U);
    sub_fn_value = (uint8_t)(raw_byte & (uint8_t)SVC_0x3E_SUBFN_MASK);

    /* [P2-0x3E-02] Only sub-function value 0x00 is valid (bits 6:0). */
    if (sub_fn_value != (uint8_t)SVC_0x3E_SUBFUNCTION_ZERO) {
        return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }

    /* Reset S3server timer regardless of suppress bit. */
    status = uds_session_reset_s3_timer(ctx->cfg.session_ctx);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Build positive response. */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_TESTER_PRESENT, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->data[1] = (uint8_t)SVC_0x3E_SUBFUNCTION_ZERO;
    resp->length  = (uint16_t)2U;

    /* [P2-0x3E-01] Suppress positive response if bit 7 was set. */
    if (suppress) {
        resp->length = (uint16_t)0U;
    }

    return UDS_STATUS_OK;
}
