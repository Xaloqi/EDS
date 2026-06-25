// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/services.h
 *
 * PURPOSE: Service handler registration table and shared service utilities.
 *          Provides the canonical service entry table used to initialize
 *          the UDS server, and declares shared helpers used across
 *          individual service implementation files.
 *
 * SAFETY  : Safety-relevant — service table gates all diagnostic access.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_SERVICES_H
#define UDS_SERVICES_H

#include "uds_types.h"
#include "uds_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Service handler declarations
 *
 * Each service_0xXX.c file implements one of these functions.
 * All functions conform to the uds_service_handler_fn prototype.
 * -------------------------------------------------------------------------- */

/**
 * @brief Handler for SID 0x10 — DiagnosticSessionControl.
 *
 * Validates and executes session transitions.
 * SAFETY: Session transitions can expose higher-privilege services.
 */
uds_status_t uds_service_0x10_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x11 — ECUReset.
 *
 * Triggers controlled ECU reset after preparing NVM and peripherals.
 * SAFETY: Must coordinate with safety monitor before asserting reset.
 */
uds_status_t uds_service_0x11_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x22 — ReadDataByIdentifier.
 *
 * Returns data associated with one or more DIDs from the DID database.
 * SAFETY: DID read access must be gated by session and security level.
 */
uds_status_t uds_service_0x22_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x27 — SecurityAccess.
 *
 * Orchestrates seed/key exchange for security level unlocking.
 * SAFETY: Safety-critical. Incorrect implementation may grant unauthorized access.
 */
uds_status_t uds_service_0x27_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x3E — TesterPresent.
 *
 * Resets the S3server session timeout timer.
 */
uds_status_t uds_service_0x3E_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x2E — WriteDataByIdentifier.
 *
 * Writes a data record to a 16-bit DID, enforcing session and security
 * gating via the ASIL-B did_safe_write_*() wrappers.
 *
 * SAFETY: Write operations can modify calibration and configuration data.
 *         Security-level check is mandatory before any write callback.
 */
uds_status_t uds_service_0x2E_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x2F — InputOutputControlByIdentifier.
 *
 * Controls ECU actuators and I/O for EOL production test, bench
 * verification, and field diagnostics. inputOutputControlParameter
 * selects returnControlToECU (0x00), resetToDefault (0x01),
 * freezeCurrentState (0x02), or shortTermAdjustment (0x03).
 *
 * SAFETY: IO control modifies physical actuator state. returnControlToECU
 *         (0x00) must always restore ECU autonomous operation regardless of
 *         prior commands. Security-level check mandatory before callback.
 */
uds_status_t uds_service_0x2F_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x14 — ClearDiagnosticInformation.
 *
 * Clears all DTC status bytes for the requested group-of-DTC and persists
 * the cleared state to NVM via dtc_mirror_clear_all().
 *
 * Available in all sessions.
 */
uds_status_t uds_service_0x14_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x19 — ReadDTCInformation.
 *
 * Provides the complete fault-reading interface. Supports sub-functions:
 *   0x01 reportNumberOfDTCByStatusMask
 *   0x02 reportDTCByStatusMask
 *   0x03 reportDTCSnapshotIdentification (empty list)
 *   0x04 reportDTCSnapshotRecordByDTCNumber (no freeze-frame data)
 *   0x06 reportDTCExtDataRecordByDTCNumber (no extended data)
 *   0x0A reportSupportedDTCs
 *
 * Available in all sessions.
 */
uds_status_t uds_service_0x19_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x28 — CommunicationControl.
 *
 * Controls enabling/disabling of ECU communication messages.
 * Requires Extended or Programming session.
 *
 * SAFETY: Disabling communication affects network management.
 */
uds_status_t uds_service_0x28_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x31 — RoutineControl.
 *
 * Executes ECU-defined routines (self-test, flash erase, calibration reset,
 * actuator activation). Sub-functions: 0x01 startRoutine, 0x02 stopRoutine,
 * 0x03 requestRoutineResults.
 *
 * SAFETY: Routines can modify ECU state. Session and security level
 *         are validated before any callback is invoked.
 */
uds_status_t uds_service_0x31_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x85 — ControlDTCSetting.
 *
 * Enables or disables DTC fault detection.
 * Requires Extended or Programming session.
 *
 * SAFETY: Suppresses safety fault recording when DTC setting is OFF.
 */
uds_status_t uds_service_0x85_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x34 — RequestDownload.
 *
 * Opens a firmware download transfer session.  Validates dataFormatIdentifier,
 * parses addressAndLengthFormatIdentifier, validates the target memory range
 * against the registered flash memory map, erases the target region, and
 * initialises the transfer state machine.
 *
 * SESSION:  Programming session required.
 * SECURITY: Level 1 unlock required (enforced by ACL table).
 *
 * SAFETY: The first step of the DFU sequence (0x34 → 0x36 → 0x37).
 *         Incorrect implementation may allow firmware injection.
 */
uds_status_t uds_service_0x34_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x35 — RequestUpload.
 *
 * Opens an upload transfer session for ECU-to-tester data readback.
 * Validates dataFormatIdentifier, parses addressAndLengthFormatIdentifier,
 * validates the target memory range, checks that read_cb is registered, and
 * initialises the transfer state machine in upload direction.
 * Does NOT call erase_cb — upload is read-only.
 *
 * SESSION:  Programming session required.
 * SECURITY: Level 1 unlock required (enforced by ACL table).
 *           Upload exposes raw ECU memory — same access risk as download.
 */
uds_status_t uds_service_0x35_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x36 — TransferData.
 *
 * Receives firmware image blocks from the tester.  Validates the block
 * sequence counter (wraps 0x01–0xFF), accumulates data in the write buffer,
 * flushes full chunks via flash_write_cb, and updates the running CRC-32.
 *
 * SESSION:  Programming session required (enforced by ACL table).
 * SECURITY: Active transfer (initiated by 0x34) required.
 *
 * SAFETY: Block counter wrap 0xFF→0x01 strictly enforced.
 */
uds_status_t uds_service_0x36_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Handler for SID 0x37 — RequestTransferExit.
 *
 * Terminates an active firmware download.  Flushes remaining write buffer,
 * optionally validates a CRC-32 record, calls flash_verify_cb, and resets
 * the transfer state machine to IDLE.
 *
 * SESSION:  Programming session required (enforced by ACL table).
 * SECURITY: Active transfer (initiated by 0x34) required.
 *
 * SAFETY: bytes_remaining must equal 0 (incomplete transfer rejected NRC 0x31).
 */
uds_status_t uds_service_0x37_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/* --------------------------------------------------------------------------
 * Canonical service registration table
 *
 * This table is passed to uds_server_init() via uds_server_cfg_t.
 * It must remain in scope for the lifetime of the server instance.
 * -------------------------------------------------------------------------- */

/**
 * @brief Number of services registered in g_uds_service_table[].
 */
#define UDS_SERVICE_TABLE_COUNT (15U)

/**
 * @brief Canonical UDS service registration table.
 *
 * Defined in services.c (or directly in service_0x10.c for single-TU builds).
 * Extern declaration only — do not define here.
 */
extern const uds_service_entry_t g_uds_service_table[UDS_SERVICE_TABLE_COUNT];

/* --------------------------------------------------------------------------
 * Shared service utility API
 * -------------------------------------------------------------------------- */

/**
 * @brief Validate minimum request length for a given service.
 *
 * @param[in] req         Request buffer to check.
 * @param[in] min_length  Minimum required length in bytes (including SID).
 *
 * @return UDS_STATUS_OK if req->length >= min_length.
 * @return UDS_STATUS_ERR_INVALID_PARAM if request is too short.
 * @return UDS_STATUS_ERR_NULL_PTR if req is NULL.
 */
uds_status_t uds_service_validate_length(
    const uds_msg_buf_t *req,
    uint16_t             min_length
);

/**
 * @brief Write a positive response SID byte to the response buffer.
 *
 * Sets resp->data[0] = service_id + UDS_SID_POSITIVE_RESPONSE_OFFSET.
 *
 * @param[in]  service_id  Original request SID.
 * @param[out] resp        Response buffer to populate.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if resp is NULL.
 */
uds_status_t uds_service_write_pos_sid(
    uint8_t        service_id,
    uds_msg_buf_t *resp
);

#ifdef __cplusplus
}
#endif

#endif /* UDS_SERVICES_H */
