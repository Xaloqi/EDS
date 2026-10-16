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
 * @return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE  DID not supported in the
 *         active session: unknown, not readable, or min_session not met.
 *         The caller skips it (ISO 14229-1:2026 Figure 21) [EDS#465].
 * @return UDS_STATUS_ERR_SEC_NOT_UNLOCKED → NRC 0x33
 * @return any read_cb error, e.g. UDS_STATUS_ERR_CONDITIONS_NOT_MET → NRC 0x22
 *         (Figure 21 per-DID condition check).
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

    /* Step 2: Resolve DID in database (REQ-SAFE-001). [EDS#465] Figure 21
     * step "DID supported in active session?": unknown, not readable, or
     * not in this session all mean "not supported" (skipped by the caller). */
    if (uds_safety_find_did(did_id, &entry) != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
    if ((entry->access_flags & (uint8_t)DID_ACCESS_READ) == (uint8_t)0U) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
#if UDS_SAFETY_ENABLE_SESSION_CHECKS
    if (uds_safety_validate_session(session_ctx,
            (uds_session_type_t)entry->min_session) != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
#endif

    /* Step 3: Full access validation (REQ-SAFE-002/003). Readability and
     * session passed above, so a failure here is security (NRC 0x33). */
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
 * ISO 14229-1:2026 §10.2.4, Table 190, Figure 21 [EDS#465]:
 *   0x13 — length < 3 or not SID + n DIDs
 *   per DID, in order: not supported in the active session → skipped;
 *   0x33 — security; 0x22 — DID condition (read_cb)
 *   0x31 — none of the requested DIDs is supported in the active session
 *   0x14 — total response exceeds the transport limit
 * The positive response carries every supported DID.
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
    uint16_t     supported = (uint16_t)0U;

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
        if (status == UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE) {
            /* [EDS#465] Not supported in the active session: skip it. */
            continue;
        }
        if (status != UDS_STATUS_OK) {
            return status;   /* 0x33 security, 0x22 condition, ... */
        }
        supported++;

        /* [EDS#465] Total response length exceeded → NRC 0x14 (Table 190). */
        if ((write_pos + (uint16_t)SVC_0x22_DID_BYTE_LEN + did_data_len) >
            (uint16_t)UDS_MAX_PAYLOAD_LEN) {
            return UDS_STATUS_ERR_RESPONSE_TOO_LONG;
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

    /* [EDS#465] At least one DID must be supported (Figure 21). */
    if (supported == (uint16_t)0U) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    resp->length = write_pos;

    return UDS_STATUS_OK;
}
