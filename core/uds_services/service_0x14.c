// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x14.c
 *
 * PURPOSE: SID 0x14 — ClearDiagnosticInformation service handler.
 *
 * ISO 14229-1:2026 §11.2 — ClearDiagnosticInformation
 *
 * The tester transmits a 3-byte groupOfDTC: a group or a single DTC
 * (§11.2.1). The ECU clears exactly what was asked, then persists it.
 *
 * SUPPORTED groupOfDTC VALUES (ISO 14229-1:2026 Annex D.1 Table D.1) [EDS#450]:
 *   0xFFFFFF          — all groups: every non-permanent DTC is cleared.
 *   a registered DTC  — that one DTC is cleared (unless permanent).
 *   anything else     — NRC 0x31, nothing cleared (Table 299). Includes the
 *                       reserved 0x000000-0x0000FF range and the functional
 *                       groups 0xFFFF00-0xFFFFFE, which need group membership
 *                       data EDS doesn't hold.
 *   Previously every non-zero value cleared everything, so clearing one
 *   repaired DTC erased every other fault record.
 *
 * SESSION REQUIREMENT (ISO 14229-1:2026 §9.2.1 Table 22):
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
 * @brief "Clear all" group-of-DTC identifier (ISO 14229-1:2026 Annex D.1, Table D.1).
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
    dtc_entry_t *entry;

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

    /* ── Clear exactly the requested group or DTC [EDS#450] ───────────── */

    if (group_of_dtc == (uint32_t)SVC_0x14_GROUP_ALL) {
        /* All groups. Positive even if no DTC is stored (§11.2.1). Permanent
         * DTCs survive, as before. */
        status = dtc_database_clear_non_permanent();
        if (status != UDS_STATUS_OK) {
            return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
        }
        if (dtc_mirror_is_ready()) {
            status = dtc_mirror_clear_all();
            if (status != UDS_STATUS_OK) {
                /* NVM write failure: the RAM database is already cleared, but
                 * the tester is told (Table 299 0x22: internal conditions
                 * prevent clearing the stored information). */
                return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
            }
        }
    } else {
        /* A single DTC. The reserved legislative range 0x000000-0x0000FF
         * (Table D.1) is never a DTC number; anything not registered is an
         * unsupported groupOfDTC: NRC 0x31 (Table 299), nothing cleared. */
        entry = (group_of_dtc <= (uint32_t)0xFFU) ? NULL : dtc_database_find(group_of_dtc);
        if (entry == NULL) {
            return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
        }
        if (!entry->is_permanent) {
            status = dtc_database_set_status(group_of_dtc, (uint8_t)0x00U);
            if (status != UDS_STATUS_OK) {
                return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
            }
            if (dtc_mirror_is_ready()) {
                status = dtc_mirror_save_one(group_of_dtc, (uint8_t)0x00U);
                if (status != UDS_STATUS_OK) {
                    return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
                }
            }
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
