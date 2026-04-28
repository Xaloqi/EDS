/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x31.c
 *
 * PURPOSE: SID 0x31 — RoutineControl service handler.
 *
 * ISO 14229-1 §13: RoutineControl provides a generic mechanism to execute
 * ECU-defined routines via diagnostic tools. Typical use cases include:
 *   - Flash erase before firmware download (prerequisite for 0x34)
 *   - Self-test / end-of-line (EOL) test execution
 *   - Calibration reset to factory defaults
 *   - Actuator activation for workshop diagnostics
 *   - NVM clear and integrity check
 *
 * Sub-functions implemented:
 *   0x01 — startRoutine
 *     Request : [0x31, 0x01, RID_hi, RID_lo {, routineOptionRecord...}]
 *     Response: [0x71, 0x01, RID_hi, RID_lo {, routineStatusRecord...}]
 *
 *   0x02 — stopRoutine  (optional per ISO 14229-1 §13.3)
 *     Request : [0x31, 0x02, RID_hi, RID_lo {, routineOptionRecord...}]
 *     Response: [0x71, 0x02, RID_hi, RID_lo {, routineStatusRecord...}]
 *
 *   0x03 — requestRoutineResults  (optional per ISO 14229-1 §13.4)
 *     Request : [0x31, 0x03, RID_hi, RID_lo]
 *     Response: [0x71, 0x03, RID_hi, RID_lo {, routineStatusRecord...}]
 *
 * NRC behaviour:
 *   NRC 0x12 (subFunctionNotSupported)  — routineControlType not in {0x01..0x03}
 *   NRC 0x31 (requestOutOfRange)        — RID not in routine database
 *   NRC 0x7F (serviceNotSupportedInActiveSession) — session below min_session
 *   NRC 0x33 (securityAccessDenied)    — security level not unlocked
 *   NRC 0x12 (subFunctionNotSupported) — stop/results requested but not supported
 *   NRC 0x22 (conditionsNotCorrect)    — routine execution failed
 *
 * Architecture:
 *   ISO-TP layer → uds_server.c → service_0x31.c
 *                                      │
 *                              routine_database.c (RID registry)
 *                                      │
 *                              routine_handlers.c (application callbacks)
 *
 * SAFETY  : Session and security checks are performed here before any
 *           callback is invoked.
 *           REQ-SAFE-002 — session validation before routine execution
 *           REQ-SAFE-003 — security level validation before routine execution
 *           REQ-SAFE-004 — NULL pointer guards on all entry points
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_safety.h"
#include "uds_session.h"
#include "uds_security.h"
#include "routine_database.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/** Minimum request: [SID, subFn, RID_hi, RID_lo] = 4 bytes. */
#define SVC_0x31_MIN_REQ_LEN        (4U)

/** Sub-function byte offset in the request PDU. */
#define SVC_0x31_SUBFN_OFFSET       (1U)

/** RID high-byte offset in the request PDU. */
#define SVC_0x31_RID_HI_OFFSET      (2U)

/** RID low-byte offset in the request PDU. */
#define SVC_0x31_RID_LO_OFFSET      (3U)

/** Option record starts at byte 4 (may be absent). */
#define SVC_0x31_OPT_OFFSET         (4U)

/** Sub-function values (ISO 14229-1 §13) */
#define SVC_0x31_SUBFN_START        (0x01U)
#define SVC_0x31_SUBFN_STOP         (0x02U)
#define SVC_0x31_SUBFN_RESULTS      (0x03U)

/* --------------------------------------------------------------------------
 * Internal helper: validate session and security for a routine
 * -------------------------------------------------------------------------- */

/**
 * @brief Check that the active session and security level satisfy the
 *        routine's access requirements.
 *
 * Mirrors the DID access check pattern from service_0x22.c / uds_safety.c
 * but applied to routine entries.
 *
 * @param[in] entry         Routine descriptor from the database.
 * @param[in] session_ctx   Active session context.
 * @param[in] security_ctx  Active security context.
 *
 * @return UDS_STATUS_OK if all checks pass.
 * @return UDS_STATUS_ERR_ROUTINE_NOT_SUPPORTED_IN_SESSION → NRC 0x7F
 * @return UDS_STATUS_ERR_SEC_NOT_UNLOCKED                → NRC 0x33
 */
static uds_status_t s_validate_routine_access(
    const routine_entry_t    *entry,
    const uds_session_ctx_t  *session_ctx,
    const uds_security_ctx_t *security_ctx)
{
    bool is_unlocked;

    /* [REQ-SAFE-004] NULL guards. */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(entry,        "entry"));
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(session_ctx,  "session_ctx"));
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(security_ctx, "security_ctx"));

    /* [REQ-SAFE-002] Session check: active session >= min_session. */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_validate_session(
        session_ctx,
        (uds_session_type_t)entry->min_session));

    /* [REQ-SAFE-003] Security check (only if the routine requires auth). */
    if (entry->security_level > (uint8_t)0U) {
        is_unlocked = false;
        UDS_SAFETY_RETURN_IF_ERR(
            uds_security_is_unlocked(security_ctx,
                                     entry->security_level,
                                     &is_unlocked));
        if (!is_unlocked) {
            return UDS_STATUS_ERR_SEC_NOT_UNLOCKED;
        }
    }

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * SID 0x31 handler
 * -------------------------------------------------------------------------- */

/**
 * @brief SID 0x31 — RoutineControl handler.
 *
 * Dispatches startRoutine, stopRoutine, and requestRoutineResults sub-functions
 * to the registered callback in the routine database.
 *
 * Request format:
 *   [0x31, subFn, RID_hi, RID_lo {, routineOptionRecord...}]
 *
 * Response format:
 *   [0x71, subFn, RID_hi, RID_lo {, routineStatusRecord...}]
 */
uds_status_t uds_service_0x31_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t              status;
    uint8_t                   sub_fn;
    uint16_t                  rid;
    const routine_entry_t    *entry         = NULL;
    const uds_session_ctx_t  *session_ctx;
    const uds_security_ctx_t *security_ctx;
    const uint8_t            *opt_buf       = NULL;
    uint8_t                   opt_len       = (uint8_t)0U;
    uint8_t                   result_buf[ROUTINE_MAX_RESULT_LEN];
    uint8_t                   result_len    = (uint8_t)0U;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* [REQ-SAFE-004] Minimum length: [SID, subFn, RID_hi, RID_lo]. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x31_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    sub_fn = req->data[SVC_0x31_SUBFN_OFFSET];

    /* Validate sub-function: only 0x01, 0x02, 0x03 are defined. */
    if ((sub_fn < (uint8_t)SVC_0x31_SUBFN_START) ||
        (sub_fn > (uint8_t)SVC_0x31_SUBFN_RESULTS)) {
        return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }

    /* Decode big-endian 16-bit RID. */
    rid = (uint16_t)(
        ((uint16_t)req->data[SVC_0x31_RID_HI_OFFSET] << (uint16_t)8U) |
         (uint16_t)req->data[SVC_0x31_RID_LO_OFFSET]
    );

    /* Locate the routine in the database. */
    entry = routine_database_find(rid);
    if (entry == NULL) {
        return UDS_STATUS_ERR_ROUTINE_NOT_FOUND;
    }

    /* Validate session and security access. */
    session_ctx  = ctx->cfg.session_ctx;
    security_ctx = ctx->cfg.security_ctx;

    status = s_validate_routine_access(entry, session_ctx, security_ctx);
    if (status != UDS_STATUS_OK) {
        if (status == UDS_STATUS_ERR_SESSION_INVALID) {
            return UDS_STATUS_ERR_ROUTINE_NOT_SUPPORTED_IN_SESSION;
        }
        return status;
    }

    /* Extract option record (bytes after [SID, subFn, RID_hi, RID_lo]). */
    if (req->length > (uint16_t)SVC_0x31_OPT_OFFSET) {
        opt_buf = &req->data[SVC_0x31_OPT_OFFSET];
        opt_len = (uint8_t)(req->length - (uint16_t)SVC_0x31_OPT_OFFSET);
    }

    /* Zero the result buffer before invoking callback. */
    (void)memset(result_buf, 0, sizeof(result_buf));

    /* Dispatch to the appropriate callback. */
    switch (sub_fn) {

        case SVC_0x31_SUBFN_START:
            /* startRoutine — mandatory sub-function. */
            status = entry->start_cb(
                opt_buf, opt_len,
                result_buf, (uint8_t)sizeof(result_buf),
                &result_len);
            break;

        case SVC_0x31_SUBFN_STOP:
            /* stopRoutine — optional. */
            if ((entry->stop_cb == NULL) ||
                ((entry->support_flags & (uint8_t)ROUTINE_SUPPORT_STOP) == (uint8_t)0U)) {
                return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
            }
            status = entry->stop_cb(
                opt_buf, opt_len,
                result_buf, (uint8_t)sizeof(result_buf),
                &result_len);
            break;

        case SVC_0x31_SUBFN_RESULTS:
            /* requestRoutineResults — optional. */
            if ((entry->results_cb == NULL) ||
                ((entry->support_flags & (uint8_t)ROUTINE_SUPPORT_RESULTS) == (uint8_t)0U)) {
                return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
            }
            status = entry->results_cb(
                result_buf, (uint8_t)sizeof(result_buf),
                &result_len);
            break;

        default:
            /* Unreachable — sub_fn was validated above. */
            return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }

    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Build positive response: [0x71, subFn, RID_hi, RID_lo {, statusRecord}] */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_ROUTINE_CONTROL, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->data[1U] = sub_fn;
    resp->data[2U] = (uint8_t)((rid >> (uint16_t)8U) & (uint16_t)0xFFU);
    resp->data[3U] = (uint8_t)( rid                  & (uint16_t)0xFFU);
    resp->length   = (uint16_t)4U;

    /* Append routine status record (may be zero bytes). */
    if (result_len > (uint8_t)0U) {
        if (((uint16_t)4U + (uint16_t)result_len) > (uint16_t)UDS_MAX_PAYLOAD_LEN) {
            return UDS_STATUS_ERR_BUFFER_OVERFLOW;
        }
        {
            uint8_t i;
            for (i = (uint8_t)0U; i < result_len; i++) {
                resp->data[(uint16_t)4U + (uint16_t)i] = result_buf[i];
            }
        }
        resp->length += (uint16_t)result_len;
    }

    return UDS_STATUS_OK;
}
