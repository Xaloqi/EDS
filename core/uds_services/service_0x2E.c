// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x2E.c
 *
 * PURPOSE: SID 0x2E — WriteDataByIdentifier service handler.
 *          Accepts a 16-bit DID and a data record from the tester,
 *          performs full ASIL-B validation, then invokes the DID write
 *          callback to persist or apply the value.
 *
 *          Architecture:
 *            ISO-TP layer -> uds_server.c -> service_0x2E.c
 *                                               |
 *                                    uds_safety.c  (ASIL-B gate)
 *                                               |
 *                                    did_handlers.c (write callbacks)
 *                                               |
 *                                    did_database.c (DID registry)
 *
 *          This design mirrors service_0x22.c: access control is
 *          centralised in uds_safety.c so new DIDs are protected
 *          automatically when added via YAML/codegen without touching
 *          this file.
 *
 * SAFETY  : Write operations can modify calibration and safety-critical
 *           configuration data. All writes enforce:
 *             REQ-SAFE-004 — NULL pointer checks
 *             REQ-SAFE-001 — DID must exist in the database
 *             REQ-SAFE-002 — Active session >= DID min_session
 *             REQ-SAFE-003 — Security level >= DID write_access_level
 *             REQ-SAFE-006 — Write data length == DID data_length exactly
 *           service_0x2E.c never calls did_database_find() directly.
 *           ASIL-B candidate per ISO 26262-6.
 * STANDARD: MISRA C:2012 alignment intended.
 *
 * NEW FILE (D-04 fix):
 *   GEN_SERVICE_ENABLED_0X2E=1 was declared in safety_config.h and a
 *   safe write accessor generated in did_safety_wrappers.c, but no
 *   service_0x2E.c existed. Write requests returned NRC 0x11 (service
 *   not supported). This file provides the missing handler.
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

/**
 * Minimum request: [SID, DID_hi, DID_lo, data_byte_0] = 4 bytes.
 * ISO 14229-1 §12.4.1: at least one data byte must be present.
 */
#define SVC_0x2E_MIN_REQ_LEN     (4U)

/** Width of the DID field in the request PDU (2 bytes, big-endian). */
#define SVC_0x2E_DID_BYTE_LEN    (2U)

/** Byte index of DID high byte in request. */
#define SVC_0x2E_DID_HI_OFFSET   (1U)

/** Byte index of DID low byte in request. */
#define SVC_0x2E_DID_LO_OFFSET   (2U)

/** Byte index of write data start in request. */
#define SVC_0x2E_DATA_OFFSET      (3U)

/** Positive response length: [0x6E, DID_hi, DID_lo] = 3 bytes. */
#define SVC_0x2E_RESP_LEN         (3U)

/* --------------------------------------------------------------------------
 * Internal helper: safety-gated DID write
 * -------------------------------------------------------------------------- */

/**
 * @brief Perform a fully safety-checked DID write.
 *
 * Implements the 5-step ASIL-B check sequence equivalent to the generated
 * did_safe_write_*() wrappers. Dynamic dispatch via the DID database allows
 * this non-generated handler to support any number of writable DIDs without
 * modification when the YAML config changes.
 *
 * SAFETY: Enforces REQ-SAFE-001/002/003/004/006 check chain for writes.
 *
 * @param[in] ctx      UDS server context.
 * @param[in] did_id   16-bit DID identifier.
 * @param[in] buf      Pointer to write data bytes.
 * @param[in] len      Number of bytes in buf.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND    → NRC 0x31
 * @return UDS_STATUS_ERR_SESSION_INVALID  → NRC 0x22
 * @return UDS_STATUS_ERR_SEC_NOT_UNLOCKED → NRC 0x33
 * @return UDS_STATUS_ERR_INVALID_PARAM    → NRC 0x13 (wrong data length)
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET → NRC 0x22
 */
static uds_status_t s_did_safe_write(
    uds_server_ctx_t *ctx,
    uint16_t          did_id,
    const uint8_t    *buf,
    uint16_t          len)
{
    const did_entry_t        *entry        = NULL;
    const uds_session_ctx_t  *session_ctx  = ctx->cfg.session_ctx;
    const uds_security_ctx_t *security_ctx = ctx->cfg.security_ctx;

    /* Step 1: NULL-check all contexts (REQ-SAFE-004). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(session_ctx,  "session_ctx"));
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(security_ctx, "security_ctx"));
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(buf,          "buf"));

    /* Step 2: Resolve DID in database (REQ-SAFE-001). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_find_did(did_id, &entry));

    /* Step 3: Validate session and security access (REQ-SAFE-002/003). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_validate_did_access(
        entry, session_ctx, security_ctx, UDS_SAFETY_ACCESS_WRITE));

    /* Step 4: Validate write data length == DID data_length (REQ-SAFE-006). */
    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_write_data_length(entry, len));

    /* Step 5: Invoke the DID write callback. */
    return entry->write_cb(buf, len);
}

/* --------------------------------------------------------------------------
 * SID 0x2E handler
 * -------------------------------------------------------------------------- */

/**
 * Request : [0x2E, DID_hi, DID_lo, data_byte_0 {, data_byte_1, ...}]
 * Response: [0x6E, DID_hi, DID_lo]
 *
 * ISO 14229-1 §12.4: WriteDataByIdentifier writes one DID per request.
 * The data record length must exactly match the DID's declared data_length.
 *
 * NRC mapping (per ISO 14229-1 §12.6):
 *   0x13 — incorrectMessageLengthOrInvalidFormat  (data length mismatch)
 *   0x22 — conditionsNotCorrect                   (session not met)
 *   0x31 — requestOutOfRange                      (DID not found)
 *   0x33 — securityAccessDenied                   (security level)
 *   0x72 — generalProgrammingFailure               (write callback error)
 */
uds_status_t uds_service_0x2E_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t   status;
    uint16_t       did_id;
    const uint8_t *write_data;
    uint16_t       write_len;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum: [SID, DID_hi, DID_lo, data] = 4 bytes. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x2E_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Extract big-endian 16-bit DID from request. */
    did_id = (uint16_t)(
        ((uint16_t)req->data[SVC_0x2E_DID_HI_OFFSET] << (uint16_t)8U) |
         (uint16_t)req->data[SVC_0x2E_DID_LO_OFFSET]
    );

    /* Data record starts at byte 3; length is everything after SID+DID. */
    write_data = &req->data[SVC_0x2E_DATA_OFFSET];
    write_len  = (uint16_t)(req->length - (uint16_t)SVC_0x2E_DATA_OFFSET);

    /* Safety-gated write: validates session, security, length, then writes. */
    status = s_did_safe_write(ctx, did_id, write_data, write_len);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Build positive response: [0x6E, DID_hi, DID_lo]. */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_WRITE_DATA_BY_ID, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->data[1] = (uint8_t)((did_id >> (uint16_t)8U) & (uint16_t)0xFFU);
    resp->data[2] = (uint8_t)(did_id & (uint16_t)0xFFU);
    resp->length  = (uint16_t)SVC_0x2E_RESP_LEN;

    return UDS_STATUS_OK;
}
