// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x85.c
 *
 * PURPOSE: SID 0x85 — ControlDTCSetting service handler.
 *
 * ISO 14229-1 §15 — ControlDTCSetting
 *
 * This service allows the tester to stop (or restart) the ECU's DTC
 * detection logic. When DTC setting is disabled, the application layer
 * must suppress any updates to DTC status bytes (i.e. not call
 * dtc_database_set_status()). This is used during reprogramming sequences
 * to prevent spurious DTCs from being set while actuators are forced into
 * non-nominal states by the programmer.
 *
 * TYPICAL USE SEQUENCE:
 *   1. Enter extended or programming session
 *   2. Disable DTC setting: [0x85, 0x02]
 *   3. Perform actuator tests or reprogramming
 *   4. Re-enable DTC setting: [0x85, 0x01]
 *
 * IMPLEMENTED SUB-FUNCTIONS (ISO 14229-1 Table 225):
 *
 *   0x01 — DTCSettingOn
 *     Re-enables DTC fault detection. Application layer resumes calling
 *     dtc_database_set_status() when fault conditions are detected.
 *
 *   0x02 — DTCSettingOff
 *     Disables DTC fault detection. Application must check
 *     uds_comm_control_dtc_setting_is_on() before setting any DTC.
 *
 * DTCSetting CONTROL RECORD (optional extension bytes):
 *   Bytes after the sub-function byte are the dtcSettingControlOptionRecord.
 *   This implementation ignores these bytes (accepted but not processed).
 *   OEM-specific extensions can be added by parsing req->data[2..n].
 *
 * SESSION REQUIREMENT (ISO 14229-1 §15.2.2):
 *   ControlDTCSetting is only available in Extended or Programming session.
 *   The server dispatcher enforces this. Handler double-checks.
 *
 * ON SESSION RETURN TO DEFAULT:
 *   DTC setting must be automatically re-enabled when the session returns
 *   to Default (ISO 14229-1 §15.1). This is handled by:
 *     uds_comm_control_restore_defaults() called from service_0x10.c
 *   when a transition to DEFAULT session is processed.
 *
 * REQUEST FORMAT:
 *   [0x85, dtcSettingType, {optionalControlRecord...}]   (≥ 2 bytes)
 *
 * POSITIVE RESPONSE FORMAT:
 *   [0xC5, dtcSettingType]   (2 bytes, echoes setting type)
 *
 * NEGATIVE RESPONSE CONDITIONS:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — request < 2 bytes
 *   NRC 0x12 (subFunctionNotSupported) — dtcSettingType not in {0x01, 0x02}
 *   NRC 0x7E (subFunctionNotSupportedInActiveSession) — in DEFAULT session
 *   NRC 0x22 (conditionsNotCorrect) — platform callback failed
 *
 * SAFETY  : ASIL-B candidate. Disabling DTC setting suppresses safety fault
 *           recording. Must only be used in controlled programming sessions.
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

/** Minimum request length: [SID, dtcSettingType] = 2 bytes. */
#define SVC_0x85_MIN_LEN              (2U)

/** Sub-function mask (bits 6:0); bit 7 = suppressPosRspMsgIndicationBit. */
#define SVC_0x85_SUBFN_MASK           (0x7FU)

/* --------------------------------------------------------------------------
 * Sub-function values (ISO 14229-1 Table 225)
 * -------------------------------------------------------------------------- */
#define SVC_0x85_DTC_SETTING_ON       (0x01U) /* Re-enable DTC fault detection  */
#define SVC_0x85_DTC_SETTING_OFF      (0x02U) /* Disable DTC fault detection     */

/* --------------------------------------------------------------------------
 * SID 0x85 handler implementation
 * -------------------------------------------------------------------------- */

/**
 * Request : [0x85, dtcSettingType, {optionalRecord...}]
 * Response: [0xC5, dtcSettingType]
 */
uds_status_t uds_service_0x85_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t       status;
    uint8_t            setting_type;
    uds_session_type_t active_session;

    /* ── Length validation ──────────────────────────────────────────────── */

    status = uds_service_validate_length(req, (uint16_t)SVC_0x85_MIN_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Strip suppressPosRspMsgIndicationBit — server dispatcher handles suppress */
    setting_type = (uint8_t)(req->data[1] & (uint8_t)SVC_0x85_SUBFN_MASK);

    /* ── Session gate (defence-in-depth) ────────────────────────────────── */

    /*
     * ISO 14229-1 §15.2.2: ControlDTCSetting requires non-default session.
     * Double-checking here as defence-in-depth (ASIL-B requirement).
     */
    status = uds_session_get_active(ctx->cfg.session_ctx, &active_session);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }
    if (active_session == UDS_SESSION_DEFAULT) {
        return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION;
    }

    /* ── Sub-function validation ─────────────────────────────────────────── */

    if ((setting_type != (uint8_t)SVC_0x85_DTC_SETTING_ON)
     && (setting_type != (uint8_t)SVC_0x85_DTC_SETTING_OFF)) {
        return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }

    /*
     * Note: any optional dtcSettingControlOptionRecord bytes in req->data[2..]
     * are accepted but silently ignored. OEM-specific extensions can parse
     * these bytes here.
     */

    /* ── Apply DTC setting mode via comm_control module ─────────────────── */

    status = uds_comm_control_set_dtc_setting(setting_type);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* ── Build positive response [0xC5, dtcSettingType] ─────────────────── */

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_CONTROL_DTC_SETTING, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->data[1] = setting_type; /* Echo the setting type (without suppress bit) */
    resp->length  = (uint16_t)2U;

    return UDS_STATUS_OK;
}
