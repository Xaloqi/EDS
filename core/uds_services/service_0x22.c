// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x22.c
 *
 * PURPOSE: SID 0x22 — ReadDataByIdentifier service handler.
 *          Resolves one or more DIDs against the DID database and packs
 *          their data records into the response PDU. All DID access routes
 *          through the ASIL-B 5-step safety validation sequence enforced
 *          by uds_safety.c before invoking any DID read callback.
 *
 *          Architecture:
 *            ISO-TP layer -> uds_server.c -> service_0x22.c
 *                                               |
 *                                    uds_safety.c  (ASIL-B gate)
 *                                               |
 *                                    did_handlers.c (DID callbacks)
 *                                               |
 *                                    did_database.c (DID registry)
 *
 *          This design allows session/security/bounds validation to be
 *          concentrated in uds_safety.c and reused across all service
 *          handlers rather than duplicated per-service.
 *
 * SAFETY  : All DID reads enforce:
 *             REQ-SAFE-004 — NULL pointer checks on all contexts
 *             REQ-SAFE-001 — DID must exist in the database
 *             REQ-SAFE-002 — Active session >= DID min_session
 *             REQ-SAFE-003 — Active security level >= DID read_access_level
 *             REQ-SAFE-006 — buf_len >= DID data_length before callback
 *           service_0x22.c never calls did_database_find() directly;
 *           all access routes through the safety module.
 *           ASIL-B candidate per ISO 26262-6.
 * STANDARD: MISRA C:2012 alignment intended.
 *
 * CHANGE LOG (D-03 fix):
 *   Phase 2 stub replaced with full functional implementation:
 *   - Added full 5-step safety validation via uds_safety.c.
 *   - DID read callback invoked correctly (entry->read_cb).
 *   - Response writes exactly entry->data_length bytes.
 *   - Multi-DID iteration supported (all DID pairs in one request).
 *   - Per-DID overflow guard before writing to response buffer.
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

/** Minimum request: [SID, DID_hi, DID_lo] = 3 bytes. */
#define SVC_0x22_MIN_REQ_LEN     (3U)

/** Width of each DID in the PDU (2 bytes, big-endian). */
#define SVC_0x22_DID_BYTE_LEN    (2U)

/* --------------------------------------------------------------------------
 * Internal helper: safety-gated DID read
 * -------------------------------------------------------------------------- */

/**
 * @brief Perform a fully safety-checked DID read.
 *
 * Implements the same 5-step sequence as a generated did_safe_read_*()
 * wrapper but dispatches dynamically via the DID database, which allows
 * this non-generated handler to support any number of DIDs without
 * modification when the YAML config changes.
 *
 * SAFETY: Reproduces the REQ-SAFE-001/002/003/004/006 check chain.
 *
 * @param[in]  ctx      UDS server context (provides session/security).
 * @param[in]  did_id   16-bit DID identifier.
 * @param[out] buf      Buffer to receive DID payload.
 * @param[in]  buf_len  Size of buf in bytes.
 * @param[out] out_len  Bytes written to buf.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND  → NRC 0x31
 * @return UDS_STATUS_ERR_SESSION_INVALID → NRC 0x22
 * @return UDS_STATUS_ERR_SEC_NOT_UNLOCKED → NRC 0x33
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW → NRC 0x14
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET → NRC 0x22
 */
static uds_status_t s_did_safe_read(
    uds_server_ctx_t *ctx,
    uint16_t          did_id,
    uint8_t          *buf,
    uint16_t          buf_len,
    uint16_t         *out_len)
{
    const did_entry_t        *entry        = NULL;
    const uds_session_ctx_t  *session_ctx  = ctx->cfg.session_ctx;
    const uds_security_ctx_t *security_ctx = ctx->cfg.security_ctx;

    /* Step 1: NULL-check session and security contexts (REQ-SAFE-004). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(session_ctx,  "session_ctx"));
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(security_ctx, "security_ctx"));
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(buf,          "buf"));
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(out_len,      "out_len"));

    /* Step 2: Resolve DID in database (REQ-SAFE-001). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_find_did(did_id, &entry));

    /* Step 3: Validate session and security access level (REQ-SAFE-002/003). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_validate_did_access(
        entry, session_ctx, security_ctx, UDS_SAFETY_ACCESS_READ));

    /* Step 4: Bounds-check buffer capacity (REQ-SAFE-006). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_did_data_length(entry, buf_len));

    /* Step 5: Invoke the DID read callback. */
    return entry->read_cb(buf, buf_len, out_len);
}

/* --------------------------------------------------------------------------
 * SID 0x22 handler
 * -------------------------------------------------------------------------- */

/**
 * Request : [0x22, DID1_hi, DID1_lo {, DID2_hi, DID2_lo, ...}]
 * Response: [0x62, DID1_hi, DID1_lo, <data1> {, DID2_hi, DID2_lo, <data2>}]
 *
 * ISO 14229-1 §11.4: Multiple DIDs may be requested in a single PDU.
 * Each DID is processed in order; the first failure causes a NRC response.
 */
uds_status_t uds_service_0x22_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;
    uint16_t     req_pos;
    uint16_t     write_pos;
    uint16_t     did_id;
    uint8_t      did_data_buf[DID_MAX_DATA_LEN];
    uint16_t     did_data_len;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum: [SID, DID_hi, DID_lo] = 3 bytes. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x22_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Request payload after SID must contain complete 2-byte DID pairs. */
    if (((req->length - (uint16_t)1U) % (uint16_t)SVC_0x22_DID_BYTE_LEN) != (uint16_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Write positive-response SID byte (0x62). */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DATA_BY_ID, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    write_pos = (uint16_t)1U;   /* Next available byte in resp->data[]. */
    req_pos   = (uint16_t)1U;   /* Skip SID byte in req->data[].        */

    while (req_pos < req->length) {

        /* Extract big-endian 16-bit DID from request. */
        did_id = (uint16_t)(
            ((uint16_t)req->data[req_pos]                  << (uint16_t)8U) |
             (uint16_t)req->data[req_pos + (uint16_t)1U]
        );
        req_pos += (uint16_t)SVC_0x22_DID_BYTE_LEN;

        /* Safety-gated read: validates session, security, bounds. */
        did_data_len = (uint16_t)0U;
        status = s_did_safe_read(ctx, did_id,
                                 did_data_buf,
                                 (uint16_t)sizeof(did_data_buf),
                                 &did_data_len);
        if (status != UDS_STATUS_OK) {
            /*
             * ISO 14229-1 §14.4 specifies NRC 0x31 (requestOutOfRange)
             * when a DID is not accessible in the current session or is
             * simply not supported. Remap session-gate failures here so
             * that the generic srv_status_to_nrc() path in uds_server.c
             * sees the correct status (ERR_REQUEST_OUT_OF_RANGE → 0x31).
             */
            if ((status == UDS_STATUS_ERR_SESSION_INVALID) ||
                (status == UDS_STATUS_ERR_SESSION_TRANSITION) ||
                (status == UDS_STATUS_ERR_CONDITIONS_NOT_MET)) {
                return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
            }
            return status;
        }

        /*
         * Response overflow guard:
         * write_pos + DID_echo(2) + data_len must not exceed UDS_MAX_PAYLOAD_LEN.
         */
        if ((write_pos + (uint16_t)SVC_0x22_DID_BYTE_LEN + did_data_len) >
            (uint16_t)UDS_MAX_PAYLOAD_LEN) {
            return UDS_STATUS_ERR_BUFFER_OVERFLOW;
        }

        /* Write DID echo (big-endian). */
        resp->data[write_pos] = (uint8_t)((did_id >> (uint16_t)8U) & (uint16_t)0xFFU);
        write_pos++;
        resp->data[write_pos] = (uint8_t)(did_id & (uint16_t)0xFFU);
        write_pos++;

        /* Copy exactly did_data_len bytes (as reported by the callback). */
        {
            uint16_t i;
            for (i = (uint16_t)0U; i < did_data_len; i++) {
                resp->data[write_pos] = did_data_buf[i];
                write_pos++;
            }
        }
    }

    resp->length = write_pos;

    return UDS_STATUS_OK;
}
