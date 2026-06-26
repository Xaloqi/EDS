/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_registration.c
 *
 * PURPOSE: Defines the canonical UDS service registration table and
 *          implements shared service utility functions declared in services.h.
 *
 *          The table g_uds_service_table[] is the authoritative mapping of
 *          service identifiers to handler functions. It is referenced by
 *          uds_server.c during request dispatching and is const — no runtime
 *          mutation is permitted.
 *
 * ADDING A NEW SERVICE:
 *   1. Implement the handler in a new service_0xNN.c translation unit.
 *   2. Declare the handler prototype in services.h.
 *   3. Add one uds_service_entry_t row to g_uds_service_table[] below.
 *   4. Increment UDS_SERVICE_TABLE_COUNT in services.h.
 *   5. Add the new .c file to CMakeLists.txt DIAG_CORE_SOURCES.
 *
 * SAFETY  : Safety-relevant. The service table is the sole dispatch mechanism
 *           between the ISO-TP transport layer and service handler code.
 *           An incorrect entry will cause requests for the affected SID to be
 *           silently rejected with NRC 0x11 (serviceNotSupported).
 *           Any modification requires impact assessment.
 *
 * MISRA   : No dynamic memory. No recursion. Table is const at file scope.
 *           All functions return uds_status_t. NULL guards on all inputs.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * VERSION : 1.7.0
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"

/* =============================================================================
 * Static assertions — compile-time validation
 *
 * These assertions verify that the table size macro in services.h matches
 * the actual number of entries defined in this file. Mismatch is a build
 * error rather than a silent runtime fault.
 * ============================================================================= */

/*
 * Compile-time validation: table size macro must match actual table size.
 * Mismatch is a build error rather than a silent runtime fault.
 * [P2-SREG-01] Activated: C11 confirmed on all target toolchains.
 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(
    (sizeof(g_uds_service_table) / sizeof(g_uds_service_table[0]))
    == UDS_SERVICE_TABLE_COUNT,
    "UDS_SERVICE_TABLE_COUNT does not match actual table size"
);
#endif

/* =============================================================================
 * Canonical service dispatch table
 *
 * ORDERING: Table order does not affect correctness; the server performs a
 *           linear search by service_id. However, ordering by ascending SID
 *           is maintained for readability and auditability.
 *
 * SUPPRESS_POS_RESPONSE: Set to true for services where ISO 14229-1 mandates
 *   that the suppressPosRspMsgIndicationBit in the sub-function byte must be
 *   honoured (e.g. 0x10, 0x11, 0x3E). False for services with no sub-function.
 * ============================================================================= */

/**
 * @brief Canonical UDS service registration table.
 *
 * @note This object has internal linkage visibility via the extern declaration
 *       in services.h. It must not be modified after program startup.
 *
 * @note SAFETY: Const qualification prevents accidental runtime modification.
 *               MISRA C:2012 Rule 8.4: definition matches the extern declaration.
 */
const uds_service_entry_t g_uds_service_table[UDS_SERVICE_TABLE_COUNT] = {
    {
        /* SID 0x10 — DiagnosticSessionControl
         *
         * Controls transitions between Default, Extended, and Programming sessions.
         * Sub-function byte carries session type + suppressPosRspMsgIndicationBit.
         *
         * SAFETY: Session transitions gate access to all higher-privilege services.
         *         Incorrect handling may allow programming-session services in
         *         default-session context.
         */
        .service_id                    = UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
        .handler                       = uds_service_0x10_handler,
        .suppress_pos_response_supported = true,
    },
    {
        /* SID 0x11 — ECUReset
         *
         * Triggers a controlled hardware or software ECU reset.
         * Sub-function byte selects reset type (hardReset, keyOffOnReset, softReset).
         *
         * SAFETY: Reset must only execute after positive response is transmitted.
         *         Platform layer must coordinate with safety monitor.
         *         TIMING CRITICAL: platform must schedule reset post-TX.
         */
        .service_id                    = UDS_SID_ECU_RESET,
        .handler                       = uds_service_0x11_handler,
        .suppress_pos_response_supported = true,
    },
    {
        /* SID 0x22 — ReadDataByIdentifier
         *
         * Returns data records for one or more 16-bit Data Identifiers (DIDs).
         * No sub-function byte — suppress_pos_response is not applicable.
         *
         * SAFETY: DID access gated by session level and security unlock state.
         *         Safety-critical signals may be exposed via this service.
         */
        .service_id                    = UDS_SID_READ_DATA_BY_ID,
        .handler                       = uds_service_0x22_handler,
        .suppress_pos_response_supported = false,
    },
    {
        /* SID 0x2A — ReadDataByPeriodicIdentifier
         *
         * Registers subscriptions for periodic push of 0xF2xx DID data.
         * Subscriptions persist until stopSending (0x04) is received, the
         * session returns to Default, or the S3server timer expires.
         *
         * SESSION:  Non-default session required (Extended or Programming).
         * SECURITY: No mandatory unlock — DID-level access controls apply.
         *
         * SAFETY: Push data rate is bounded by UDS_PERIODIC_FAST_MS.
         *         ISO-TP must be idle before each push — the caller must
         *         serialise periodic transmits with request-response frames.
         *         suppressPosRspMsgIndicationBit not applicable — no sub-function.
         */
        .service_id                      = UDS_SID_READ_DATA_BY_PERIODIC_ID,
        .handler                         = uds_service_0x2A_handler,
        .suppress_pos_response_supported = false,
    },
    {
        /* SID 0x27 — SecurityAccess
         *
         * Implements seed/key challenge-response for security level escalation.
         * Sub-function byte carries level identifier and requestSeed/sendKey flag.
         *
         * SAFETY: Critical path for ECU security. Incorrect implementation may
         *         grant unauthorised access to reprogramming or calibration services.
         *         Seed entropy source must be validated. Key algorithm must be
         *         reviewed by security team before production deployment.
         */
        .service_id                    = UDS_SID_SECURITY_ACCESS,
        .handler                       = uds_service_0x27_handler,
        .suppress_pos_response_supported = false,
    },
    {
        /* SID 0x3E — TesterPresent
         *
         * Resets the S3server session keep-alive timer. Used by tester tools
         * to maintain an active non-default session without sending other requests.
         * Sub-function byte 0x00 is the only valid value; bit 7 is the suppress flag.
         *
         * SAFETY: Session maintenance. If TesterPresent is not processed, the
         *         S3server timeout will expire and the ECU returns to Default Session,
         *         which may abort in-progress calibration or reprogramming operations.
         */
        .service_id                    = UDS_SID_TESTER_PRESENT,
        .handler                       = uds_service_0x3E_handler,
        .suppress_pos_response_supported = true,
    },
    {
        /* SID 0x2E — WriteDataByIdentifier
         *
         * Writes a data record to a 16-bit Data Identifier (DID).
         * No sub-function byte. Access gated by session and security level
         * as declared per DID in diagnostics_config.yaml.
         *
         * SAFETY: Write operations modify ECU calibration or configuration data.
         *         Security level check mandatory before callback invocation.
         *         All writes route through did_safe_write_*() ASIL-B wrappers.
         */
        .service_id                      = UDS_SID_WRITE_DATA_BY_ID,
        .handler                         = uds_service_0x2E_handler,
        .suppress_pos_response_supported = false,
    },
    {
        /* SID 0x2F — InputOutputControlByIdentifier
         *
         * Controls ECU actuators and I/O during EOL, bench test, and field
         * diagnostics. inputOutputControlParameter selects returnControlToECU
         * (0x00), resetToDefault (0x01), freezeCurrentState (0x02), or
         * shortTermAdjustment (0x03).  The DID's io_control_cb is invoked
         * after session, security, and access-flag validation.
         *
         * SESSION:  Extended or Programming session (NON_DEFAULT).
         * SECURITY: Level 1 security unlock required.
         *
         * SAFETY: IO control modifies physical actuator state. Incorrect
         *         implementation may leave outputs in an unsafe state.
         *         returnControlToECU (0x00) must always restore ECU
         *         autonomous operation regardless of prior commands.
         *         suppressPosRspMsgIndicationBit not applicable — no
         *         sub-function byte.
         */
        .service_id                      = UDS_SID_INPUT_OUTPUT_CONTROL,
        .handler                         = uds_service_0x2F_handler,
        .suppress_pos_response_supported = false,
    },
    {
        /* SID 0x14 — ClearDiagnosticInformation
         *
         * Clears all DTC status bytes for the requested group-of-DTC and
         * persists the cleared state to NVM via dtc_mirror_clear_all().
         *
         * Available in all sessions.
         *
         * SAFETY: Erases fault history. Requires careful access control in
         *         production — consider restricting to Extended or Programming
         *         session in OEM-specific builds.
         */
        .service_id                    = UDS_SID_CLEAR_DIAGNOSTIC_INFO,
        .handler                       = uds_service_0x14_handler,
        .suppress_pos_response_supported = false,
    },
    {
        /* SID 0x19 — ReadDTCInformation
         *
         * The primary fault-reading interface for OEM diagnostic tools.
         * Supports sub-functions 0x01, 0x02, 0x03, 0x04, 0x06, 0x0A.
         *
         * Available in all sessions.
         *
         * suppressPosRspMsgIndicationBit is supported per ISO 14229-1 §13.2.4
         * for all ReadDTCInformation sub-functions.
         */
        .service_id                    = UDS_SID_READ_DTC_INFO,
        .handler                       = uds_service_0x19_handler,
        .suppress_pos_response_supported = true,
    },
    {
        /* SID 0x28 — CommunicationControl
         *
         * Controls enabling/disabling of ECU application-layer communication.
         * Typically used during reprogramming to suppress bus traffic.
         *
         * SESSION: Extended or Programming session required.
         *
         * SAFETY: Disabling communication can affect network management and
         *         safety monitors. OEM must validate use cases.
         */
        .service_id                    = UDS_SID_COMMUNICATION_CONTROL,
        .handler                       = uds_service_0x28_handler,
        .suppress_pos_response_supported = true,
    },
    {
        /* SID 0x31 — RoutineControl
         *
         * Executes ECU-defined routines: self-test, flash erase before
         * firmware download, calibration reset, actuator activation.
         * Sub-function byte selects startRoutine (0x01), stopRoutine (0x02),
         * or requestRoutineResults (0x03).
         *
         * SESSION: Configurable per routine (min_session in routine descriptor).
         *          Default: Extended or Programming session required.
         * SECURITY: Configurable per routine (security_level in descriptor).
         *
         * SAFETY: Routines can modify safety-relevant ECU state (e.g. NVM erase,
         *         output actuation). The service layer validates session and
         *         security level before invoking any callback.
         *         suppressPosRspMsgIndicationBit is NOT applicable —
         *         0x31 carries a routineControlType byte that is not a true
         *         sub-function byte; suppress-bit semantics do not apply per
         *         ISO 14229-1 §13.2.
         */
        .service_id                      = UDS_SID_ROUTINE_CONTROL,
        .handler                         = uds_service_0x31_handler,
        .suppress_pos_response_supported = false,
    },
    {
        /* SID 0x85 — ControlDTCSetting
         *
         * Enables or disables DTC fault detection logic.
         * When disabled, application must not update DTC status bytes.
         *
         * SESSION: Extended or Programming session required.
         *
         * SAFETY: Suppresses safety-relevant fault recording. Must be
         *         re-enabled before leaving the diagnostic session.
         *         Automatic restoration on Default session transition is
         *         enforced by service_0x10.c via uds_comm_control_restore_defaults().
         */
        .service_id                    = UDS_SID_CONTROL_DTC_SETTING,
        .handler                       = uds_service_0x85_handler,
        .suppress_pos_response_supported = true,
    },
    {
        /* SID 0x34 — RequestDownload
         *
         * Opens a firmware download session for DFU (Device Firmware Update).
         * Validates target memory range, erases flash, and initialises the
         * transfer state machine.
         *
         * SESSION:  Programming session required.
         * SECURITY: Level 1 security unlock required.
         *
         * SAFETY: First step of the DFU sequence. Incorrect implementation
         *         may allow firmware injection into safety-critical code.
         *         Session and security gates enforced by ACL table.
         *         suppressPosRspMsgIndicationBit not applicable — no
         *         sub-function byte.
         */
        .service_id                      = UDS_SID_REQUEST_DOWNLOAD,
        .handler                         = uds_service_0x34_handler,
        .suppress_pos_response_supported = false,
    },
    {
        /* SID 0x35 — RequestUpload
         *
         * Opens an upload session for ECU-to-tester data readback.
         * Validates target memory range, checks read_cb is registered, and
         * initialises the transfer state machine in upload direction.
         * Does NOT erase flash — upload is read-only.
         *
         * SESSION:  Programming session required.
         * SECURITY: Level 1 security unlock required.
         *
         * SAFETY: Upload exposes raw ECU memory content. Same access gates
         *         as RequestDownload.  suppressPosRspMsgIndicationBit not
         *         applicable — no sub-function byte.
         */
        .service_id                      = UDS_SID_REQUEST_UPLOAD,
        .handler                         = uds_service_0x35_handler,
        .suppress_pos_response_supported = false,
    },
    {
        /* SID 0x36 — TransferData
         *
         * Carries firmware image blocks from the tester to ECU flash.
         * Validates block sequence counter (wraps 0x01–0xFF), accumulates
         * data, and flushes write-buffer chunks to flash.
         *
         * SESSION:  Programming session required (enforced by ACL table).
         * SECURITY: Active transfer (started by 0x34) required.
         *
         * SAFETY: Block counter wrap 0xFF→0x01 enforced (REQ-DL-001).
         *         Incorrect counter validation is the most common
         *         implementation defect in DFU stacks.
         */
        .service_id                      = UDS_SID_TRANSFER_DATA,
        .handler                         = uds_service_0x36_handler,
        .suppress_pos_response_supported = false,
    },
    {
        /* SID 0x37 — RequestTransferExit
         *
         * Terminates the active firmware download.  Flushes remaining write
         * buffer, validates optional CRC-32, calls flash_verify_cb, and
         * resets the transfer state machine to IDLE.
         *
         * SESSION:  Programming session required (enforced by ACL table).
         *
         * SAFETY: bytes_remaining == 0 enforced (REQ-DL-002).
         *         Transfer context reset to IDLE on success or failure
         *         (REQ-DL-003).
         */
        .service_id                      = UDS_SID_REQUEST_TRANSFER_EXIT,
        .handler                         = uds_service_0x37_handler,
        .suppress_pos_response_supported = false,
    },
};

/* =============================================================================
 * Shared service utility functions
 * ============================================================================= */

/**
 * @brief Validate that a request buffer meets minimum length requirements.
 *
 * @details Called at the entry of every service handler before any field
 *          is accessed. Prevents out-of-bounds reads on under-length requests.
 *
 * @note MISRA C:2012 Rule 14.4: Boolean condition uses explicit comparison.
 */
uds_status_t uds_service_validate_length(
    const uds_msg_buf_t *req,
    uint16_t             min_length)
{
    if (req == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /*
     * A request with zero bytes is always invalid at the UDS layer —
     * even the SID byte must be present. Guard against pathological
     * callers before comparing against min_length.
     */
    if (req->length == (uint16_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (req->length < min_length) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    return UDS_STATUS_OK;
}

/**
 * @brief Write the positive response SID byte to the response buffer.
 *
 * @details Per ISO 14229-1 §7.5.2.2, every positive response frame begins
 *          with the service identifier OR-ed with 0x40 (UDS_SID_POSITIVE_RESPONSE_OFFSET).
 *
 *          The response buffer's length field is set to 1 after writing.
 *          Service handlers must then append sub-function echoes and data,
 *          incrementing resp->length as they write each byte.
 *
 * @note MISRA C:2012 Rule 10.1: The shift and OR operations use explicit
 *       uint8_t cast to prevent implicit integer promotion warnings.
 */
uds_status_t uds_service_write_pos_sid(
    uint8_t        service_id,
    uds_msg_buf_t *resp)
{
    if (resp == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /*
     * Positive response byte: SID + 0x40.
     * Cast to uint8_t to prevent implicit int promotion.
     * MISRA C:2012 Rule 10.3: result fits in uint8_t since SID ≤ 0xBF.
     */
    resp->data[0U] = (uint8_t)((uint8_t)service_id +
                                (uint8_t)UDS_SID_POSITIVE_RESPONSE_OFFSET);
    resp->length   = (uint16_t)1U;

    return UDS_STATUS_OK;
}
