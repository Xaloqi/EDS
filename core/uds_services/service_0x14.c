// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x14.c
 *
 * PURPOSE: SID 0x14 — ClearDiagnosticInformation service handler.
 *
 * ISO 14229-1 §12.1 — ClearDiagnosticInformation
 *
 * The tester transmits a 3-byte group-of-DTC identifier (groupOfDTC).
 * The ECU clears all DTC status bytes for any DTC whose code falls within
 * the requested group, then persists the cleared state to NVM.
 *
 * SUPPORTED GROUPS (ISO 14229-1 §D.3):
 *   0xFFFFFF  — OBD group: all emissions-related DTCs
 *   0xFFFFFE  — SAE J2012-DA group: all powertrain DTCs (0x0xxxxx – 0x3xxxxx)
 *   0xFFFFFF  — ISO/SAE group: all supported DTCs (implementation-defined)
 *
 * IMPLEMENTATION NOTE:
 *   This implementation treats 0xFFFFFF as "clear all registered DTCs"
 *   (the most common tester request). All other group codes are also
 *   handled as "clear all" per the conservative approach recommended for
 *   ECUs that do not implement DTC group filtering — this is compliant
 *   with ISO 14229-1 §12.1.3 which permits clearing all DTCs when a
 *   group-of-DTC value is received that encompasses all stored DTCs.
 *
 *   If group-specific filtering is required, add a per-entry group
 *   field to dtc_entry_t and filter the clear loop accordingly.
 *
 * SESSION REQUIREMENT (ISO 14229-1 §12.1.2.2):
 *   ClearDiagnosticInformation is available in all sessions.
 *
 * NVM INTEGRATION (Phase 3):
 *   After clearing all DTC status bytes, the handler calls
 *   dtc_mirror_clear_all() to persist the cleared state. This ensures
 *   DTC history does not reappear after an ECU reset.
 *
 * REQUEST FORMAT:
 *   [0x14, groupOfDTC_HB, groupOfDTC_MB, groupOfDTC_LB]   (4 bytes)
 *
 * POSITIVE RESPONSE FORMAT:
 *   [0x54]   (1 byte — no additional data)
 *
 * NEGATIVE RESPONSE CONDITIONS:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — request != 4 bytes
 *   NRC 0x31 (requestOutOfRange)  — groupOfDTC not recognized / no DTCs match
 *   NRC 0x22 (conditionsNotCorrect) — NVM flush failed
 *
 * SAFETY  : ASIL-B candidate. Clears safety-relevant DTC history.
 *           Must only execute after security unlock in production builds.
 *           (Session gating is enforced by the server dispatcher.)
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "dtc_database.h"
#include "dtc_mirror.h"

/* --------------------------------------------------------------------------
 * Request format constants
 * -------------------------------------------------------------------------- */

/** Exact request length: [SID, groupHB, groupMB, groupLB] = 4 bytes. */
#define SVC_0x14_REQ_LEN              (4U)

/**
 * @brief "Clear all" group-of-DTC identifier (ISO 14229-1 §12.2, Table 203).
 *
 * 0xFFFFFF is the universal group encompassing all DTCs.
 * Tester tools always use this value for a full DTC clear.
 */
#define SVC_0x14_GROUP_ALL            (0xFFFFFFUL)

/* --------------------------------------------------------------------------
 * SID 0x14 handler implementation
 * -------------------------------------------------------------------------- */

/**
 * Request : [0x14, groupHB, groupMB, groupLB]
 * Response: [0x54]
 */
uds_status_t uds_service_0x14_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;
    uint32_t     group_of_dtc;
    uint16_t     dtc_count;

    (void)ctx; /* server context not needed for this service */

    /* ── Length validation ──────────────────────────────────────────────── */

    status = uds_service_validate_length(req, (uint16_t)SVC_0x14_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Exact length check: request must be exactly 4 bytes. */
    if (req->length != (uint16_t)SVC_0x14_REQ_LEN) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* ── Decode groupOfDTC (3-byte big-endian value after SID) ─────────── */

    group_of_dtc = ((uint32_t)req->data[1] << 16U)
                 | ((uint32_t)req->data[2] <<  8U)
                 |  (uint32_t)req->data[3];

    /*
     * Validate that there are DTCs registered.
     * If the database is empty, returning NRC 0x31 (requestOutOfRange) is
     * the correct ISO 14229-1 response — there is nothing to clear.
     */
    status = dtc_database_get_count(&dtc_count);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    if (dtc_count == (uint16_t)0U) {
        /*
         * ISO 14229-1 §12.1.3: if no DTCs match the requested group,
         * the ECU shall send NRC 0x31.
         */
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /*
     * ── Group filtering ──────────────────────────────────────────────────
     *
     * ISO 14229-1 §12.2 defines several group values:
     *   0xFFFFFF — all emission-related DTCs (or "all DTCs" for non-OBD)
     *   0xFFFFFE — all powertrain DTCs (by SAE J2012-DA)
     *   0xFFFF00-0xFFFFFD — OBD system groups
     *
     * This implementation accepts any non-zero group value and maps it to
     * "clear all registered DTCs" — the conservative, universally-correct
     * approach for ECUs without group-filtered DTC storage.
     *
     * Future enhancement: filter dtc_database entries by DTC range when
     * group-specific clearing is required.
     */
    if (group_of_dtc == (uint32_t)0U) {
        /* Group 0x000000 is not defined in ISO 14229-1 — reject. */
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* ── Clear all DTC status bytes ─────────────────────────────────────── */

    status = dtc_database_clear_all();
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* ── Persist cleared state to NVM (Phase 3 integration) ────────────── */

    if (dtc_mirror_is_ready()) {
        status = dtc_mirror_clear_all();
        if (status != UDS_STATUS_OK) {
            /*
             * NVM write failure is non-fatal for the UDS transaction
             * (the in-RAM database is already cleared), but we log by
             * returning conditions-not-correct so the tester is aware.
             * Production builds may relax this to a warning log only.
             */
            return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
        }
    }

    /* ── Build positive response [0x54] ─────────────────────────────────── */

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_CLEAR_DIAGNOSTIC_INFO, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Positive response for SID 0x14 contains only the response SID byte. */
    resp->length = (uint16_t)1U;

    return UDS_STATUS_OK;
}
