/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x37.c
 *
 * PURPOSE: SID 0x37 — RequestTransferExit service handler.
 *
 * ISO 14229-1 §14.4: RequestTransferExit terminates an active firmware
 * download initiated by RequestDownload (0x34).  It flushes any remaining
 * bytes from the write accumulation buffer, optionally validates a CRC-32
 * supplied in the transferRequestParameterRecord, and invokes flash_verify_cb
 * to confirm image integrity.
 *
 * REQUEST FORMAT (ISO 14229-1 §14.4.2):
 *   [0x37 {, transferRequestParameterRecord...}]
 *
 *   The request may carry an optional 4-byte CRC-32 record:
 *     [0x37, CRC_byte3, CRC_byte2, CRC_byte1, CRC_byte0]   (big-endian)
 *   If the record is absent (length == 1), no CRC check is performed.
 *   If present and 4 bytes, it is compared against the accumulated CRC.
 *
 * POSITIVE RESPONSE (ISO 14229-1 §14.4.3):
 *   [0x77]
 *   (No additional data fields required for this implementation.)
 *
 * NRC BEHAVIOUR:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — bad CRC record length
 *   NRC 0x24 (requestSequenceError)                  — no active transfer
 *   NRC 0x72 (generalProgrammingFailure)              — flash write or verify fail
 *   NRC 0x31 (requestOutOfRange)                      — bytes_remaining != 0
 *
 * INCOMPLETE TRANSFER DETECTION (REQ-DL-002):
 *   If bytes_remaining > 0 when 0x37 is received, the tester sent fewer
 *   bytes than declared in the RequestDownload memorySize.  The transfer
 *   is aborted with NRC 0x31 (requestOutOfRange).
 *
 * SAFETY:
 *   REQ-DL-002: bytes_remaining must be 0 before accepting exit.
 *   REQ-DL-003: Transfer context reset to IDLE after successful or failed exit.
 *   REQ-FLASH-003: CRC verified before positive response is sent.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_transfer_ctx.h"
#include "uds_flash_ops.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/** Minimum valid request: [SID] only = 1 byte. */
#define SVC_0x37_MIN_REQ_LEN          (1U)

/** Size of the optional CRC-32 transferRequestParameterRecord (4 bytes). */
#define SVC_0x37_CRC_RECORD_LEN       (4U)

/** Offset of first CRC byte within the request (after SID). */
#define SVC_0x37_CRC_OFFSET           (1U)

/* --------------------------------------------------------------------------
 * SID 0x37 handler
 * -------------------------------------------------------------------------- */

/**
 * @brief SID 0x37 — RequestTransferExit handler.
 *
 * Flushes the remaining write buffer, validates the optional CRC record,
 * invokes flash_verify_cb, resets the transfer state machine, and returns
 * a positive response.
 */
uds_status_t uds_service_0x37_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t           status;
    uds_transfer_ctx_t    *tctx;
    const uds_flash_ops_t *ops;
    bool                   crc_supplied  = false;
    uint32_t               supplied_crc  = (uint32_t)0U;
    uint32_t               computed_crc;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum length: just [SID]. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x37_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* REQ-DL-SEQ: A transfer must be active. */
    tctx = uds_transfer_ctx_get();
    if (tctx->state != UDS_TRANSFER_ACTIVE) {
        /* NRC 0x24 requestSequenceError. */
        return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION;
    }

    ops = uds_flash_ops_get();
    if (ops == NULL) {
        uds_transfer_ctx_reset(tctx);
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* --- Parse optional CRC record --- */
    if (req->length == (uint16_t)(SVC_0x37_MIN_REQ_LEN + SVC_0x37_CRC_RECORD_LEN)) {
        /* Exactly 5 bytes: [SID, CRC3, CRC2, CRC1, CRC0]. */
        supplied_crc = ((uint32_t)req->data[SVC_0x37_CRC_OFFSET    ] << (uint32_t)24U) |
                       ((uint32_t)req->data[SVC_0x37_CRC_OFFSET + 1U] << (uint32_t)16U) |
                       ((uint32_t)req->data[SVC_0x37_CRC_OFFSET + 2U] << (uint32_t)8U)  |
                        (uint32_t)req->data[SVC_0x37_CRC_OFFSET + 3U];
        crc_supplied = true;
    } else if (req->length != (uint16_t)SVC_0x37_MIN_REQ_LEN) {
        /* Any other length is malformed. */
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    /* else: length == 1 — no CRC record, no check. */

    /* --- REQ-DL-002: All declared bytes must have been received --- */
    if (tctx->bytes_remaining != (uint32_t)0U) {
        /* Tester sent fewer bytes than declared in RequestDownload. */
        uds_transfer_ctx_reset(tctx);
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* --- Flush any remaining bytes in the write accumulation buffer --- */
    if (tctx->write_buf_fill > (uint16_t)0U) {
        status = ops->write_cb(
            tctx->next_write_address,
            tctx->write_buf,
            (uint32_t)tctx->write_buf_fill);

        if (status != UDS_STATUS_OK) {
            uds_transfer_ctx_reset(tctx); /* REQ-DL-003 */
            return UDS_STATUS_ERR_PLATFORM;
        }

        tctx->next_write_address += (uint32_t)tctx->write_buf_fill;
        tctx->write_buf_fill      = (uint16_t)0U;
    }

    /* --- CRC validation (if supplied) --- */
    if (crc_supplied) {
        computed_crc = uds_transfer_crc32_finalise(tctx->crc_accumulator);

        if (computed_crc != supplied_crc) {
            uds_transfer_ctx_reset(tctx); /* REQ-DL-003 */
            /* NRC 0x72 generalProgrammingFailure — CRC mismatch. */
            return UDS_STATUS_ERR_PLATFORM;
        }
    }

    /* --- REQ-FLASH-003: Platform verify callback --- */
    status = ops->verify_cb(
        tctx->target_address,
        tctx->total_size_bytes,
        uds_transfer_crc32_finalise(tctx->crc_accumulator));

    if (status != UDS_STATUS_OK) {
        uds_transfer_ctx_reset(tctx); /* REQ-DL-003 */
        /* NRC 0x72 generalProgrammingFailure — platform verify failed. */
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* --- REQ-DL-003: Reset transfer context --- */
    uds_transfer_ctx_reset(tctx);

    /* --- Build positive response: [0x77] --- */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_REQUEST_TRANSFER_EXIT, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->length = (uint16_t)1U;

    return UDS_STATUS_OK;
}
