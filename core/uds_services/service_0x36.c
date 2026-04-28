/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x36.c
 *
 * PURPOSE: SID 0x36 — TransferData service handler.
 *
 * ISO 14229-1 §14.3: TransferData carries firmware image blocks from the
 * tester to the ECU.  It is called repeatedly after a successful
 * RequestDownload (0x34) until all bytes described by memorySize have
 * been transferred.
 *
 * REQUEST FORMAT (ISO 14229-1 §14.3.2):
 *   [0x36, blockSequenceCounter, transferRequestParameterRecord...]
 *
 *   blockSequenceCounter (1 byte):
 *     Starts at 0x01 on the first block after RequestDownload.
 *     Increments by 1 per block.
 *     WRAPS from 0xFF to 0x01 — value 0x00 is never valid.
 *     This is the most common implementation mistake.
 *
 *   transferRequestParameterRecord (variable):
 *     Raw firmware image bytes for this block.
 *     Maximum length = maxNumberOfBlockLength - 1 (minus block counter byte).
 *
 * POSITIVE RESPONSE (ISO 14229-1 §14.3.3):
 *   [0x76, blockSequenceCounter]  (echo of the received counter)
 *
 * NRC BEHAVIOUR:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — request < 3 bytes
 *   NRC 0x24 (requestSequenceError)                  — no active transfer
 *   NRC 0x73 (wrongBlockSequenceCounter)              — counter mismatch
 *   NRC 0x72 (generalProgrammingFailure)              — flash write failed
 *
 * BLOCK COUNTER WRAP LOGIC (REQ-DL-001):
 *
 *   received == next_expected             → accept
 *   received == 0x00                      → NRC 0x73 (always invalid)
 *   received != next_expected             → NRC 0x73
 *
 *   After acceptance:
 *     if (next_expected == 0xFF) next_expected = 0x01   (wrap)
 *     else                       next_expected++
 *
 * WRITE FLUSH LOGIC:
 *   Payload bytes are accumulated in tctx->write_buf[].
 *   When write_buf reaches write_buf_capacity, flash_write_cb is invoked
 *   for that chunk.  The remaining bytes of an oversized block (which
 *   should not happen with a well-behaved tester) are also flushed
 *   immediately.  This handles the edge case where the tester sends
 *   exactly max_block_length - 1 payload bytes in the final block.
 *
 * SAFETY:
 *   REQ-DL-001: Block counter wrap enforced.
 *   REQ-DL-002: bytes_remaining decremented per payload byte written.
 *   REQ-FLASH-003: CRC accumulator updated for every payload byte.
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

/** Minimum request: [SID, blockSeqCounter, ≥1 data byte] = 3 bytes. */
#define SVC_0x36_MIN_REQ_LEN          (3U)

/** Offset of blockSequenceCounter in the request PDU. */
#define SVC_0x36_BLOCK_SEQ_OFFSET     (1U)

/** Offset of the first payload data byte. */
#define SVC_0x36_DATA_OFFSET          (2U)

/** Block counter value that is never valid (ISO 14229-1 §14.3.2). */
#define SVC_0x36_INVALID_BLOCK_SEQ    (0x00U)

/** Maximum valid block sequence counter value before wrap. */
#define SVC_0x36_MAX_BLOCK_SEQ        (0xFFU)

/** Value to which the counter wraps after reaching MAX (REQ-DL-001). */
#define SVC_0x36_WRAP_BLOCK_SEQ       (0x01U)

/* --------------------------------------------------------------------------
 * Static helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Flush accumulated bytes from write_buf to flash.
 *
 * Calls flash_write_cb for the filled portion of write_buf, advances
 * next_write_address, and resets write_buf_fill to zero.
 *
 * @param[in,out] tctx  Active transfer context.
 * @param[in]     ops   Flash operations table.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM on flash write failure.
 */
static uds_status_t s_flush_write_buf(uds_transfer_ctx_t    *tctx,
                                       const uds_flash_ops_t *ops)
{
    uds_status_t status;

    if (tctx->write_buf_fill == (uint16_t)0U) {
        return UDS_STATUS_OK;
    }

    status = ops->write_cb(
        tctx->next_write_address,
        tctx->write_buf,
        (uint32_t)tctx->write_buf_fill);

    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    tctx->next_write_address += (uint32_t)tctx->write_buf_fill;
    tctx->write_buf_fill      = (uint16_t)0U;

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * SID 0x36 handler
 * -------------------------------------------------------------------------- */

/**
 * @brief SID 0x36 — TransferData handler.
 *
 * Validates the block sequence counter, accumulates payload data, flushes
 * full write-buffer chunks to flash, and updates the CRC accumulator.
 */
uds_status_t uds_service_0x36_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t           status;
    uint8_t                block_seq;
    uint16_t               payload_len;
    const uint8_t         *payload;
    uds_transfer_ctx_t    *tctx;
    const uds_flash_ops_t *ops;
    uint16_t               src_offset;
    uint16_t               space_in_buf;
    uint16_t               chunk;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum length: [SID, blockSeq, ≥1 payload byte]. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x36_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* REQ-DL-SEQ: A transfer must be active. */
    tctx = uds_transfer_ctx_get();
    if (tctx->state != UDS_TRANSFER_ACTIVE) {
        /* NRC 0x24 requestSequenceError — no active download session. */
        return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION;
    }

    ops = uds_flash_ops_get();
    if (ops == NULL) {
        /* Flash ops de-registered after 0x34 was accepted — should not happen. */
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* --- Validate block sequence counter --- */
    block_seq = req->data[SVC_0x36_BLOCK_SEQ_OFFSET];

    /* 0x00 is always invalid (REQ-DL-001). */
    if (block_seq == (uint8_t)SVC_0x36_INVALID_BLOCK_SEQ) {
        return UDS_STATUS_ERR_TP_UNEXPECTED_PDU;   /* mapped → NRC 0x73 */
    }

    if (block_seq != tctx->next_expected_block_seq) {
        return UDS_STATUS_ERR_TP_UNEXPECTED_PDU;   /* mapped → NRC 0x73 */
    }

    /* --- Extract payload --- */
    payload     = &req->data[SVC_0x36_DATA_OFFSET];
    payload_len = (uint16_t)(req->length - (uint16_t)SVC_0x36_DATA_OFFSET);

    /* Do not accept more data than bytes_remaining. */
    if ((uint32_t)payload_len > tctx->bytes_remaining) {
        payload_len = (uint16_t)tctx->bytes_remaining;
    }

    /* --- Update running CRC-32 (REQ-FLASH-003) --- */
    tctx->crc_accumulator = uds_transfer_crc32_update(
        tctx->crc_accumulator,
        payload,
        (uint32_t)payload_len);

    /* --- Accumulate bytes into write_buf, flushing full chunks --- */
    src_offset = (uint16_t)0U;

    while (src_offset < payload_len) {
        space_in_buf = (uint16_t)(tctx->write_buf_capacity - tctx->write_buf_fill);
        chunk        = (uint16_t)(payload_len - src_offset);

        if (chunk > space_in_buf) {
            chunk = space_in_buf;
        }

        (void)memcpy(
            &tctx->write_buf[tctx->write_buf_fill],
            &payload[src_offset],
            (size_t)chunk);

        tctx->write_buf_fill += chunk;
        src_offset           += chunk;

        /* Flush when buffer is full. */
        if (tctx->write_buf_fill >= tctx->write_buf_capacity) {
            status = s_flush_write_buf(tctx, ops);
            if (status != UDS_STATUS_OK) {
                uds_transfer_ctx_reset(tctx); /* abort on failure */
                return UDS_STATUS_ERR_PLATFORM;
            }
        }
    }

    /* Update bytes_remaining. */
    tctx->bytes_remaining -= (uint32_t)payload_len;

    /* --- Advance block counter (with wrap, REQ-DL-001) --- */
    if (tctx->next_expected_block_seq == (uint8_t)SVC_0x36_MAX_BLOCK_SEQ) {
        tctx->next_expected_block_seq = (uint8_t)SVC_0x36_WRAP_BLOCK_SEQ;
    } else {
        tctx->next_expected_block_seq++;
    }

    /* --- Build positive response: [0x76, blockSequenceCounter] --- */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_TRANSFER_DATA, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->data[1U] = block_seq;
    resp->length   = (uint16_t)2U;

    return UDS_STATUS_OK;
}
