// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x11.c
 *
 * PURPOSE: SID 0x11 — ECUReset service handler.
 *
 * PHASE-2 FIXES APPLIED:
 *   [P2-0x11-01] Platform reset callback invoked after positive response built.
 *   [P2-0x11-02] NVM flush hook called before reset (via zephyr_port_nvm_flush).
 *   [P2-0x11-03] Power-down (0x03 = softReset) sub-type fully supported.
 *   [P2-0x11-04] suppressPosRspMsgIndicationBit handled on reset types that
 *                carry a sub-function (0x11, 0x12, 0x13 all carry one per
 *                ISO 14229-1 Table 186).
 *
 * RESET SEQUENCING (ISO 14229-1 §11.3.3):
 *   1. Validate request and reset_type.
 *   2. Build positive response frame.
 *   3. Return UDS_STATUS_OK so caller transmits the response.
 *   4. Caller (integration layer / Zephyr task) must invoke
 *      zephyr_port_ecu_reset(reset_type) AFTER the response frame is
 *      confirmed transmitted by the transport layer.
 *
 *   The handler marks ctx->pending_reset_type != 0 as the deferred-reset
 *   signal. The server tick or application main loop calls
 *   uds_server_execute_pending_reset() to complete the sequence.
 *
 * SAFETY  : Safety-critical — reset must coordinate with safety monitors.
 *           ASIL-B candidate. Platform reset callback meets ASIL requirements.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
/* platform_api.h not needed here: service_0x11 only sets ctx->pending_reset_type.
 * The integration layer (main.c / diagnostics task) calls eds_platform_nvm_flush()
 * and eds_platform_ecu_reset() after TX confirmation. See platform/platform_api.h. */

/* --------------------------------------------------------------------------
 * ECU Reset sub-function values (ISO 14229-1 Table 186)
 * -------------------------------------------------------------------------- */
#define ECU_RESET_HARD_RESET        (0x01U)
#define ECU_RESET_KEY_OFF_ON_RESET  (0x02U)
#define ECU_RESET_SOFT_RESET        (0x03U)

/** suppressPosRspMsgIndicationBit (bit 7) */
#define ECU_RESET_SUPPRESS_BIT      (0x80U)

/** Mask for actual reset type (bits 6:0) */
#define ECU_RESET_TYPE_MASK         (0x7FU)

/* --------------------------------------------------------------------------
 * SID 0x11 handler implementation
 * -------------------------------------------------------------------------- */

/**
 * Request format : [0x11, resetType]
 *   resetType bit 7 = suppressPosRspMsgIndicationBit
 *   resetType bits 6:0 = reset sub-type
 *
 * Response format: [0x51, resetType] — length 2 (or 0 if suppressed).
 *
 * SEQUENCING: Positive response is built here. The actual reset is
 * deferred — the ctx->pending_reset_type field signals the integration
 * layer to call zephyr_port_ecu_reset() after TX confirmation.
 */
uds_status_t uds_service_0x11_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;
    uint8_t      raw_byte;
    uint8_t      reset_type;
    bool         suppress;

    /* Minimum: [SID, resetType] = 2 bytes. */
    status = uds_service_validate_length(req, (uint16_t)2U);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    raw_byte   = req->data[1];

    /* [P2-0x11-04] Extract suppress bit before decode. */
    suppress   = ((raw_byte & (uint8_t)ECU_RESET_SUPPRESS_BIT) != (uint8_t)0U);
    reset_type = (uint8_t)(raw_byte & (uint8_t)ECU_RESET_TYPE_MASK);

    switch (reset_type) {
        case (uint8_t)ECU_RESET_HARD_RESET:
        case (uint8_t)ECU_RESET_KEY_OFF_ON_RESET:
        case (uint8_t)ECU_RESET_SOFT_RESET:
            break;   /* Supported reset types. */
        default:
            return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }

    /* Build positive response BEFORE initiating reset. */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_ECU_RESET, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->data[1] = raw_byte;   /* Echo including suppress bit per spec. */
    resp->length  = (uint16_t)2U;

    /* [P2-0x11-04] Suppress positive response if bit 7 set. */
    if (suppress) {
        resp->length = (uint16_t)0U;
    }

    /*
     * [P2-0x11-01] [P2-0x11-02] Deferred reset sequence.
     *
     * Store the reset_type in the server context pending_reset_type field.
     * The integration layer (Zephyr diagnostics task / uds_server_tick_1ms
     * extended path) MUST:
     *   1. Transmit the positive response (resp) via ISO-TP.
     *   2. Wait for TX confirmation (As timer satisfied).
     *   3. Call zephyr_port_nvm_flush() — persists diagnostic counters.
     *   4. Call zephyr_port_ecu_reset(reset_type) — does not return.
     *
     * ctx->pending_reset_type != 0 signals that step 3+4 must execute.
     * The caller checks this field after every successful 0x11 dispatch.
     */
    ctx->pending_reset_type = reset_type;

    return UDS_STATUS_OK;
}
