// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x2F.c
 *
 * PURPOSE: SID 0x2F — InputOutputControlByIdentifier service handler.
 *          ISO 14229-1 §12.2: Controls ECU actuators and I/O for EOL
 *          production test, bench verification, and field diagnostics.
 *
 *          Architecture:
 *            ISO-TP layer -> uds_server.c -> service_0x2F.c
 *                                               |
 *                                    uds_safety.c  (ASIL-B gate)
 *                                               |
 *                                    io_control_cb (DID I/O callback)
 *                                               |
 *                                    did_database.c (DID registry)
 *
 *          This design mirrors service_0x22.c and service_0x2E.c: access
 *          control is centralised in uds_safety.c so new DIDs are protected
 *          automatically when added via YAML/codegen without touching this file.
 *
 * REQUEST FORMAT:
 *   [0x2F, DID_Hi, DID_Lo, inputOutputControlParameter {, controlOptionRecord...}]
 *
 *   inputOutputControlParameter:
 *     0x00 returnControlToECU   — ECU resumes autonomous control
 *     0x01 resetToDefault       — ECU resets to default/safe output
 *     0x02 freezeCurrentState   — ECU holds current output value
 *     0x03 shortTermAdjustment  — ECU applies tester-supplied value
 *
 *   controlOptionRecord (shortTermAdjustment only):
 *     Exactly entry->data_length bytes of tester-supplied target value.
 *     Absent for 0x00/0x01/0x02.
 *
 * POSITIVE RESPONSE:
 *   [0x6F, DID_Hi, DID_Lo, inputOutputControlParameter, controlStatusRecord...]
 *   controlStatusRecord = current actuator state after command (data_length bytes).
 *
 * NRC BEHAVIOUR:
 *   NRC 0x13 incorrectMessageLengthOrInvalidFormat — wrong request length
 *   NRC 0x12 subFunctionNotSupported               — controlParam not in {0x00–0x03}
 *   NRC 0x31 requestOutOfRange                     — DID not found or not IO-controllable
 *   NRC 0x22 conditionsNotCorrect                  — io_control_cb returned error
 *   NRC 0x7E/0x7F                                  — session check (ACL-enforced)
 *   NRC 0x33                                       — security level (ACL-enforced)
 *
 * SAFETY  : IO control modifies physical actuator state. All DID accesses enforce:
 *             REQ-SAFE-004 — NULL pointer checks on all contexts
 *             REQ-SAFE-001 — DID must exist in the database
 *             REQ-SAFE-002 — Active session >= DID min_session
 *             REQ-SAFE-003 — Security level >= DID write_access_level
 *           io_control_cb == NULL → NRC 0x31, not NRC 0x22.
 *           ASIL-B candidate per ISO 26262-6.
 * STANDARD: MISRA C:2012 alignment intended.
 *
 * CHANGE LOG:
 *   Added: SID 0x2F InputOutputControlByIdentifier (CR-010). Closes #47.
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_safety.h"
#include "did_database.h"

#include <stddef.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/** Minimum request: [SID, DID_Hi, DID_Lo, controlParam] = 4 bytes. */
#define SVC_0x2F_MIN_REQ_LEN       (4U)

/** Byte index of DID high byte in request. */
#define SVC_0x2F_DID_HI_OFFSET     (1U)

/** Byte index of DID low byte in request. */
#define SVC_0x2F_DID_LO_OFFSET     (2U)

/** Byte index of inputOutputControlParameter in request. */
#define SVC_0x2F_CTRL_PARAM_OFFSET (3U)

/** Byte index of controlOptionRecord start in request (shortTermAdjustment only). */
#define SVC_0x2F_CTRL_DATA_OFFSET  (4U)

#define SVC_0x2F_CTRL_PARAM_STA    (0x03U)  /**< shortTermAdjustment (0x03) — only value with a controlRecord payload. */
#define SVC_0x2F_CTRL_PARAM_MAX    (0x03U)  /**< Maximum valid value.  */

/** Fixed bytes in positive response: [0x6F, DID_Hi, DID_Lo, controlParam]. */
#define SVC_0x2F_RESP_FIXED_LEN    (4U)

/* --------------------------------------------------------------------------
 * Internal helper: safety-gated DID I/O control
 * -------------------------------------------------------------------------- */

/**
 * @brief Perform a fully safety-checked DID I/O control operation.
 *
 * Implements the 5-step ASIL-B check sequence, extending service_0x2E.c's
 * s_did_safe_write() with a fourth step that verifies the io_control_cb is
 * registered (a DID may be in the database without supporting IO control).
 *
 * For shortTermAdjustment (control_param == 0x03), the length of control_record
 * is validated against entry->data_length after DID resolution (step 2) so the
 * exact length requirement is known.
 *
 * SAFETY: Enforces REQ-SAFE-001/002/003/004 check chain.
 *
 * @param[in]  ctx            UDS server context (provides session/security).
 * @param[in]  did_id         16-bit DID identifier.
 * @param[in]  control_param  inputOutputControlParameter (0x00–0x03).
 * @param[in]  control_record Tester-supplied value; NULL for 0x00/0x01/0x02.
 * @param[in]  control_len    Bytes in control_record; 0 if NULL.
 * @param[out] status_record  Buffer to receive current actuator state.
 * @param[out] status_len     Bytes written to status_record.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND       → NRC 0x31
 * @return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE → NRC 0x31 (cb absent)
 * @return UDS_STATUS_ERR_INVALID_PARAM        → NRC 0x13 (wrong length)
 * @return UDS_STATUS_ERR_SESSION_INVALID      → NRC 0x22
 * @return UDS_STATUS_ERR_SEC_NOT_UNLOCKED     → NRC 0x33
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET   → NRC 0x22
 */
static uds_status_t s_did_safe_io_control(
    uds_server_ctx_t  *ctx,
    uint16_t           did_id,
    uint8_t            control_param,
    const uint8_t     *control_record,
    uint16_t           control_len,
    uint8_t           *status_record,
    uint16_t          *status_len)
{
    const did_entry_t        *entry        = NULL;
    const uds_session_ctx_t  *session_ctx  = ctx->cfg.session_ctx;
    const uds_security_ctx_t *security_ctx = ctx->cfg.security_ctx;

    /* Step 1: NULL-check all contexts and output pointers (REQ-SAFE-004). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(session_ctx,   "session_ctx"));
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(security_ctx,  "security_ctx"));
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(status_record,  "status_record"));
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(status_len,     "status_len"));

    /* Step 2: Resolve DID in database (REQ-SAFE-001). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_find_did(did_id, &entry));

    /* Validate shortTermAdjustment data length now that entry is known. */
    if (control_param == SVC_0x2F_CTRL_PARAM_STA) {
        if (control_len != entry->data_length) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
    }

    /* Step 3: Validate session and security access (REQ-SAFE-002/003). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_validate_did_access(
        entry, session_ctx, security_ctx, UDS_SAFETY_ACCESS_IO_CONTROL));

    /* Step 4: Confirm the DID has an io_control_cb registered.
     *         A NULL callback means the DID exists but does not support IO
     *         control for this operation — NRC 0x31 requestOutOfRange.       */
    if (entry->io_control_cb == NULL) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* Step 5: Invoke the DID I/O control callback. */
    return entry->io_control_cb(control_param,
                                control_record,
                                control_len,
                                status_record,
                                status_len);
}

/* --------------------------------------------------------------------------
 * SID 0x2F handler
 * -------------------------------------------------------------------------- */

/**
 * Request : [0x2F, DID_hi, DID_lo, controlParam {, controlOptionRecord...}]
 * Response: [0x6F, DID_hi, DID_lo, controlParam, controlStatusRecord...]
 *
 * ISO 14229-1 §12.2: One DID per request. controlParam 0x00–0x02 carry no
 * data record; controlParam 0x03 (shortTermAdjustment) requires exactly
 * entry->data_length additional bytes.
 *
 * NRC mapping (per ISO 14229-1):
 *   0x12 — subFunctionNotSupported  (controlParam > 0x03)
 *   0x13 — incorrectMessageLengthOrInvalidFormat (wrong length)
 *   0x22 — conditionsNotCorrect     (callback error or session not met)
 *   0x31 — requestOutOfRange        (DID not found or no io_control_cb)
 *   0x33 — securityAccessDenied     (security level not unlocked)
 */
uds_status_t uds_service_0x2F_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t   status;
    uint16_t       did_id;
    uint8_t        control_param;
    const uint8_t *control_record;
    uint16_t       control_len;
    uint8_t        status_buf[DID_MAX_DATA_LEN];
    uint16_t       status_len;
    uint16_t       i;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum: [SID, DID_Hi, DID_Lo, controlParam] = 4 bytes. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x2F_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Extract big-endian 16-bit DID from request. */
    did_id = (uint16_t)(
        ((uint16_t)req->data[SVC_0x2F_DID_HI_OFFSET] << (uint16_t)8U) |
         (uint16_t)req->data[SVC_0x2F_DID_LO_OFFSET]
    );

    /* Extract inputOutputControlParameter. */
    control_param = req->data[SVC_0x2F_CTRL_PARAM_OFFSET];

    /* Validate controlParam range — NRC 0x12 subFunctionNotSupported. */
    if (control_param > (uint8_t)SVC_0x2F_CTRL_PARAM_MAX) {
        return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }

    /* Set up control_record pointer and length.
     *
     * For 0x00/0x01/0x02: request must be exactly 4 bytes (no trailing data).
     * For 0x03:            request must be exactly 4 + data_length bytes;
     *                      the exact check against entry->data_length happens
     *                      inside s_did_safe_io_control() after DID resolution.
     */
    if (control_param == (uint8_t)SVC_0x2F_CTRL_PARAM_STA) {
        control_record = &req->data[SVC_0x2F_CTRL_DATA_OFFSET];
        control_len    = (uint16_t)(req->length - (uint16_t)SVC_0x2F_CTRL_DATA_OFFSET);
    } else {
        /* Exact length check: no trailing bytes permitted for 0x00/0x01/0x02. */
        if (req->length != (uint16_t)SVC_0x2F_MIN_REQ_LEN) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
        control_record = NULL;
        control_len    = (uint16_t)0U;
    }

    /* Safety-gated IO control: validates session, security, cb, then invokes. */
    status_len = (uint16_t)0U;
    status = s_did_safe_io_control(ctx, did_id,
                                   control_param,
                                   control_record,
                                   control_len,
                                   status_buf,
                                   &status_len);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Response overflow guard before writing controlStatusRecord. */
    if (((uint16_t)SVC_0x2F_RESP_FIXED_LEN + status_len) > (uint16_t)UDS_MAX_PAYLOAD_LEN) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* Build positive response: [0x6F, DID_Hi, DID_Lo, controlParam, status...] */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_INPUT_OUTPUT_CONTROL, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->data[1U] = (uint8_t)((did_id >> (uint16_t)8U) & (uint16_t)0xFFU);
    resp->data[2U] = (uint8_t)(did_id & (uint16_t)0xFFU);
    resp->data[3U] = control_param;

    for (i = (uint16_t)0U; i < status_len; i++) {
        resp->data[(uint16_t)SVC_0x2F_RESP_FIXED_LEN + i] = status_buf[i];
    }

    resp->length = (uint16_t)SVC_0x2F_RESP_FIXED_LEN + status_len;

    return UDS_STATUS_OK;
}
