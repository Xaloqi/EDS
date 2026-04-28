// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x28.c
 *
 * PURPOSE: SID 0x28 — CommunicationControl service handler.
 *
 * ISO 14229-1 §14 — CommunicationControl
 *
 * This service allows a tester to enable or disable transmission and/or
 * reception of certain communication messages. It is widely used during
 * ECU programming sequences to suppress application-layer bus traffic that
 * would otherwise interfere with the reprogramming session.
 *
 * TYPICAL USE SEQUENCE:
 *   1. Open programming session (SID 0x10, subFn 0x02)
 *   2. Disable all non-diagnostic communication (SID 0x28, subFn 0x03)
 *   3. Perform flash programming sequence
 *   4. Enable all communication (SID 0x28, subFn 0x00)
 *   5. Reset ECU (SID 0x11)
 *
 * IMPLEMENTED SUB-FUNCTIONS (ISO 14229-1 Table 207):
 *
 *   0x00 — enableRxAndTx
 *     Enables transmission and reception of all application messages.
 *     Restores normal communication mode.
 *
 *   0x01 — enableRxAndDisableTx
 *     Enables reception, disables transmission of application messages.
 *     ECU continues to receive CAN frames but suppresses its own TX.
 *
 *   0x02 — disableRxAndEnableTx
 *     Disables reception, enables transmission of application messages.
 *     ECU suppresses RX filtering for non-diagnostic traffic.
 *
 *   0x03 — disableRxAndTx
 *     Disables both transmission and reception of application messages.
 *     Most commonly used during reprogramming to isolate the ECU.
 *
 * COMMUNICATION TYPE BYTE (ISO 14229-1 Table 208):
 *   Bits 3:0 = communication sub-network type
 *     0x01 — normalCommunicationMessages (application bus)
 *     0x02 — nmCommunicationMessages (network management)
 *     0x03 — networkManagementCommunicationMessages (both)
 *   Bits 7:4 = reserved / subnet mask (0x00 for all subnets)
 *
 *   This implementation accepts any communication type byte. The
 *   platform callback (comm_control_cb) is responsible for applying
 *   the filter to the appropriate CAN message classes.
 *
 * PLATFORM INTEGRATION:
 *   The application must provide a uds_comm_control_cb_t callback in the
 *   uds_comm_control_cfg_t, which is linked into the server context via the
 *   service registration mechanism. In the Zephyr integration, this callback
 *   calls zephyr_port_comm_control() which sets the appropriate CAN filter.
 *
 *   For host-side tests, the callback is a stub that tracks state.
 *
 * SESSION REQUIREMENT (ISO 14229-1 §14.2.2):
 *   CommunicationControl is only available in Extended or Programming session.
 *   The server dispatcher enforces this — the handler also double-checks.
 *
 * ON SESSION TIMEOUT / DEFAULT TRANSITION:
 *   ISO 14229-1 §14.1: The ECU shall restore full communication on any
 *   session return to Default session. The handler in service_0x10.c is
 *   responsible for calling the comm_restore callback. This handler
 *   records state changes only.
 *
 * REQUEST FORMAT:
 *   [0x28, controlType, communicationType]   (3 bytes)
 *
 * POSITIVE RESPONSE FORMAT:
 *   [0x68, controlType]   (2 bytes, echoes control type)
 *
 * NEGATIVE RESPONSE CONDITIONS:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — request != 3 bytes
 *   NRC 0x12 (subFunctionNotSupported) — controlType not in {0x00..0x03}
 *   NRC 0x7E (subFunctionNotSupportedInActiveSession) — in DEFAULT session
 *   NRC 0x22 (conditionsNotCorrect) — platform callback failed
 *
 * SAFETY  : ASIL-B candidate. Disabling communication affects network management
 *           and safety monitors. Only permitted in non-default sessions.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_comm_control.h"

/* --------------------------------------------------------------------------
 * Request format constants
 * -------------------------------------------------------------------------- */

/** Exact request length: [SID, controlType, communicationType] = 3 bytes. */
#define SVC_0x28_REQ_LEN              (3U)

/** Sub-function mask (bits 6:0); bit 7 = suppressPosRspMsgIndicationBit. */
#define SVC_0x28_SUBFN_MASK           (0x7FU)

/* --------------------------------------------------------------------------
 * Sub-function values (ISO 14229-1 Table 207)
 * -------------------------------------------------------------------------- */
#define SVC_0x28_ENABLE_RX_TX         (0x00U)
#define SVC_0x28_ENABLE_RX_DISABLE_TX (0x01U)
#define SVC_0x28_DISABLE_RX_ENABLE_TX (0x02U)
#define SVC_0x28_DISABLE_RX_TX        (0x03U)

/* --------------------------------------------------------------------------
 * SID 0x28 handler implementation
 * -------------------------------------------------------------------------- */

/**
 * Request : [0x28, controlType, communicationType]
 * Response: [0x68, controlType]
 */
uds_status_t uds_service_0x28_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t       status;
    uint8_t            control_type;
    uint8_t            comm_type;
    uds_session_type_t active_session;

    /* ── Length validation ──────────────────────────────────────────────── */

    status = uds_service_validate_length(req, (uint16_t)SVC_0x28_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    if (req->length != (uint16_t)SVC_0x28_REQ_LEN) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Strip suppressPosRspMsgIndicationBit — server dispatcher handles suppress */
    control_type = (uint8_t)(req->data[1] & (uint8_t)SVC_0x28_SUBFN_MASK);
    comm_type    = req->data[2];

    /* ── Session gate (defence-in-depth) ────────────────────────────────── */

    /*
     * ISO 14229-1 §14.2.2: CommunicationControl is NOT available in
     * DEFAULT session. The server dispatcher enforces this via
     * srv_check_access_rights(), but we double-check here as ASIL-B
     * defence-in-depth.
     */
    status = uds_session_get_active(ctx->cfg.session_ctx, &active_session);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }
    if (active_session == UDS_SESSION_DEFAULT) {
        return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION;
    }

    /* ── Sub-function validation ─────────────────────────────────────────── */

    switch (control_type) {
        case SVC_0x28_ENABLE_RX_TX:
        case SVC_0x28_ENABLE_RX_DISABLE_TX:
        case SVC_0x28_DISABLE_RX_ENABLE_TX:
        case SVC_0x28_DISABLE_RX_TX:
            break; /* valid */
        default:
            return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }

    /* ── Apply communication control via platform callback ──────────────── */

    /*
     * Invoke the platform-supplied callback to apply the requested
     * communication control mode. The callback may call a Zephyr CAN
     * filter API or other platform mechanism.
     *
     * The callback is optional — if not registered (e.g. in minimal builds
     * or tests without a full platform), the state change is recorded but
     * no hardware action is taken.
     */
    status = uds_comm_control_apply(control_type, comm_type);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* ── Build positive response [0x68, controlType] ────────────────────── */

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_COMMUNICATION_CONTROL, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->data[1] = control_type; /* Echo the control type (without suppress bit) */
    resp->length  = (uint16_t)2U;

    return UDS_STATUS_OK;
}
