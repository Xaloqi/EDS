/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x36.c
 *
 * PURPOSE: SID 0x36 — TransferData service handler.
 *
 * ISO 14229-1:2026 §14.4: TransferData carries firmware image blocks from the
 * tester to the ECU.  It is called repeatedly after a successful
 * RequestDownload (0x34) until all bytes described by memorySize have
 * been transferred.
 *
 * REQUEST FORMAT (ISO 14229-1:2026 §14.4.2):
 *   [0x36, blockSequenceCounter, transferRequestParameterRecord...]
 *
 *   blockSequenceCounter (1 byte):
 *     Starts at 0x01 on the first block after RequestDownload.
 *     Increments by 1 per block.
 *     Rolls over from 0xFF to 0x00 (ISO 14229-1, TransferData) [EDS#433].
 *     0x00 is invalid only as the first block, where 0x01 is expected.
 *
 *   transferRequestParameterRecord (variable):
 *     Raw firmware image bytes for this block.
 *     Maximum length = maxNumberOfBlockLength - 2 (SID and counter); a longer
 *     request is NRC 0x13 (ISO 14229-1:2026 Table 454) [EDS#442].
 *
 * POSITIVE RESPONSE (ISO 14229-1:2026 §14.4.3):
 *   [0x76, blockSequenceCounter]  (echo of the received counter)
 *
 * NRC BEHAVIOUR:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — request < 3 bytes
 *   NRC 0x24 (requestSequenceError)                  — no active transfer, or
 *                                                        all data already
 *                                                        transferred [EDS#441]
 *   NRC 0x71 (transferDataSuspended)                 — payload exceeds
 *                                                        bytes_remaining;
 *                                                        transfer is aborted
 *                                                        [EDS#441, was 0x31]
 *   NRC 0x73 (wrongBlockSequenceCounter)              — counter mismatch
 *   NRC 0x72 (generalProgrammingFailure)              — flash write failed, the
 *                                                        written block read
 *                                                        back differently
 *                                                        (EDS#543), or
 *                                                        the registered image
 *                                                        policy's update_cb
 *                                                        refused the block
 *                                                        (issue #232)
 *
 * BLOCK COUNTER WRAP LOGIC (REQ-DL-001):
 *
 *   received == last accepted             → positive response, nothing
 *                                            written (download) / previous
 *                                            data resent (upload) [EDS#441]
 *   received == next_expected             → accept
 *   received != next_expected             → NRC 0x73
 *
 *   After acceptance:
 *     if (next_expected == 0xFF) next_expected = 0x00   (wrap)
 *     else                       next_expected++
 *
 *   [EDS#433] This used to wrap to 0x01 and reject 0x00 outright, so every
 *   conformant tester failed a download longer than 255 blocks with NRC
 *   0x73.
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
 *               A block whose payload exceeds bytes_remaining is rejected
 *               (NRC 0x31) and the transfer context is aborted rather than
 *               being silently truncated to fit.
 *   REQ-FLASH-003: CRC accumulator updated for every payload byte. With
 *               the flash ops' verify_step_cb, each flushed chunk is read
 *               back and a mismatch is NRC 0x72 at this block [EDS#543].
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
#include "uds_image_policy.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/** Minimum request: [SID, blockSeqCounter, ≥1 data byte] = 3 bytes (download direction). */
#define SVC_0x36_MIN_REQ_LEN          (3U)

/** Minimum upload request: [SID, blockSeqCounter] = 2 bytes (no payload from tester). */
#define SVC_0x36_UL_MIN_REQ_LEN       (2U)

/** Offset of blockSequenceCounter in the request PDU. */
#define SVC_0x36_BLOCK_SEQ_OFFSET     (1U)

/** Offset of the first payload data byte. */
#define SVC_0x36_DATA_OFFSET          (2U)

/** Maximum valid block sequence counter value before wrap. */
#define SVC_0x36_MAX_BLOCK_SEQ        (0xFFU)

/** Value to which the counter rolls over after MAX (REQ-DL-001, ISO
 *  14229-1) [EDS#433]. */
#define SVC_0x36_WRAP_BLOCK_SEQ       (0x00U)

/* --------------------------------------------------------------------------
 * Forward declaration
 * -------------------------------------------------------------------------- */

static uds_status_t s_handle_upload_block(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp);

/* --------------------------------------------------------------------------
 * Static helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Flush accumulated bytes from write_buf to flash, then read them back.
 *
 * Calls flash_write_cb for the filled portion of write_buf. When the flash
 * ops register verify_step_cb [EDS#543], reads the same range back and
 * advances two running CRC-32s: one over the bytes written, one over the
 * bytes flash returned. They start equal at 0x34 and cover the same bytes
 * after every flush, so the first block whose readback differs makes them
 * differ, and is refused here, at the 0x36 that wrote it. 0x37 then checks
 * the read-back region's CRC-32 against the received data without reading
 * the region again (P2server_max: EDS#543).
 *
 * Advances next_write_address and resets write_buf_fill to zero.
 *
 * @param[in,out] tctx  Active transfer context.
 * @param[in]     ops   Flash operations table.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM on flash write or readback failure, or
 *         when the readback does not match what was written.
 */
uds_status_t uds_service_0x36_flush(uds_transfer_ctx_t    *tctx,
                                    const uds_flash_ops_t *ops)
{
    uds_status_t status;
    uint32_t     length;
    uint32_t     readback_crc;

    if ((tctx == NULL) || (ops == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (tctx->write_buf_fill == (uint16_t)0U) {
        return UDS_STATUS_OK;
    }

    length = (uint32_t)tctx->write_buf_fill;

    status = ops->write_cb(tctx->next_write_address, tctx->write_buf, length);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* [EDS#543] REQ-FLASH-003: read back what was just written. */
    if (ops->verify_step_cb != NULL) {
        readback_crc = tctx->readback_crc_accumulator;
        status = ops->verify_step_cb(tctx->next_write_address, length,
                                     tctx->readback_crc_accumulator,
                                     &readback_crc);
        if (status != UDS_STATUS_OK) {
            return UDS_STATUS_ERR_PLATFORM;
        }

        tctx->flushed_crc_accumulator = uds_transfer_crc32_update(
            tctx->flushed_crc_accumulator, tctx->write_buf, length);
        tctx->readback_crc_accumulator = readback_crc;

        if (tctx->readback_crc_accumulator != tctx->flushed_crc_accumulator) {
            return UDS_STATUS_ERR_PLATFORM;   /* block reads back wrong */
        }

        tctx->readback_bytes += length;
    }

    tctx->next_write_address += length;
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
    uint16_t               min_len;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    tctx = uds_transfer_ctx_get();

    /* Upload requests carry no payload from the tester — minimum is 2 bytes.
     * Download requests require at least 1 payload byte — minimum is 3 bytes. */
    if ((tctx->state == UDS_TRANSFER_ACTIVE) &&
        (tctx->direction == UDS_TRANSFER_DIR_UPLOAD)) {
        min_len = (uint16_t)SVC_0x36_UL_MIN_REQ_LEN;
    } else {
        min_len = (uint16_t)SVC_0x36_MIN_REQ_LEN;
    }

    status = uds_service_validate_length(req, min_len);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* REQ-DL-SEQ: A transfer must be active. */
    if (tctx->state != UDS_TRANSFER_ACTIVE) {
        /* NRC 0x24 requestSequenceError — no active transfer session. */
        return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
    }

    /* [EDS#442] A download request longer than the advertised
     * maxNumberOfBlockLength (write_buf_capacity + SID + counter) is NRC 0x13
     * (ISO 14229-1:2026 Table 454). Previously any length was accepted and
     * split across the write buffer. */
    if ((tctx->direction == UDS_TRANSFER_DIR_DOWNLOAD) &&
        ((uint32_t)req->length > ((uint32_t)tctx->write_buf_capacity + 2U))) {
        return UDS_STATUS_ERR_INVALID_PARAM;   /* mapped → NRC 0x13 */
    }

    ops = uds_flash_ops_get();
    if (ops == NULL) {
        /* Flash ops de-registered after 0x34/0x35 was accepted — should not happen. */
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* Dispatch to upload path if an upload session is active. */
    if (tctx->direction == UDS_TRANSFER_DIR_UPLOAD) {
        return s_handle_upload_block(ctx, req, resp);
    }

    block_seq   = req->data[SVC_0x36_BLOCK_SEQ_OFFSET];
    payload     = &req->data[SVC_0x36_DATA_OFFSET];
    payload_len = (uint16_t)(req->length - (uint16_t)SVC_0x36_DATA_OFFSET);

    /* [EDS#441] A repeat of the last accepted block is the client's retry
     * after a lost positive response. ISO 14229-1:2026 §14.4.4 Table 454
     * (under NRC 0x24 and 0x73): it "shall be accepted by the server".
     * Table 451: answer positively without writing the data again, so no
     * write, CRC or image-policy update, and bytes_remaining is untouched. */
    if (tctx->last_block_valid && (block_seq == tctx->last_block_seq)) {
        status = uds_service_write_pos_sid((uint8_t)UDS_SID_TRANSFER_DATA, resp);
        if (status != UDS_STATUS_OK) {
            return status;
        }
        resp->data[1U] = block_seq;
        resp->length   = (uint16_t)2U;
        return UDS_STATUS_OK;
    }

    /* [EDS#441] Evaluation order per Figure 34: sequence (0x24), then
     * memorySize (0x71), then blockSequenceCounter (0x73).
     * All data already received: Table 454 NRC 0x24. */
    if (tctx->bytes_remaining == (uint32_t)0U) {
        return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;   /* mapped → NRC 0x24 */
    }

    /* REQ-DL-002: A block must not carry more data than bytes_remaining.
     * Silently truncating and continuing would discard tester bytes while
     * still advancing the block sequence and CRC as if the block were
     * well-formed. Reject and abort instead (mirrors the 0x37 handling of
     * REQ-DL-002 for the "too few bytes" case). */
    if ((uint32_t)payload_len > tctx->bytes_remaining) {
        uds_image_policy_notify_abort();            /* [#232] drop partial state */
        uds_transfer_ctx_reset(tctx); /* abort — REQ-DL-003 */
        /* [EDS#441] Table 454: "the download module length does not meet the
         * requirements of the memorySize parameter" is NRC 0x71
         * transferDataSuspended (was 0x31). */
        return UDS_STATUS_ERR_TRANSFER_SUSPENDED;   /* NRC 0x71 */
    }

    /* --- Validate block sequence counter --- */
    if (block_seq != tctx->next_expected_block_seq) {
        return UDS_STATUS_ERR_TP_UNEXPECTED_PDU;   /* mapped → NRC 0x73 */
    }

    /* --- Update running CRC-32 (REQ-FLASH-003) --- */
    tctx->crc_accumulator = uds_transfer_crc32_update(
        tctx->crc_accumulator,
        payload,
        (uint32_t)payload_len);

    /* ----------------------------------------------------------------------
     * [#232 Phase 1] Stream the SAME byte range to the image policy.
     *
     * The digest is accumulated here, block by block, rather than computed
     * over the whole image at 0x37, because 0x36/0x37 have no generic NRC
     * 0x78 (responsePending) mechanism to reach and P2server_max is 50 ms
     * (issue #282) — a whole-image hash at transfer exit would blow the
     * timer on any realistic image size. (0x34 has since grown a
     * service-specific 0x78 path of its own, but it is not reachable from
     * here — see issue #282.)
     *
     * OFFSET DERIVATION: bytes_remaining is decremented for this block
     * further down, AFTER the write-buffer loop.  It therefore still holds
     * the pre-block value here, and
     *
     *     image_offset = total_size_bytes - bytes_remaining
     *
     * is the offset at which THIS chunk starts, not the one at which it
     * ends.  Ranges handed to update_cb are consequently contiguous,
     * non-overlapping, and cover [0, total_size_bytes) exactly once:
     * bytes_remaining only ever moves by exactly the number of payload
     * bytes just passed to update_cb, and an over-long block is rejected
     * above before reaching this point.
     * -------------------------------------------------------------------- */
    {
        const uds_image_policy_t *policy = uds_image_policy_get();

        if (policy != NULL) {
            uint32_t image_offset =
                tctx->total_size_bytes - tctx->bytes_remaining;

            status = policy->update_cb(image_offset,
                                        payload,
                                        (uint32_t)payload_len);
            if (status != UDS_STATUS_OK) {
                uds_image_policy_notify_abort();
                uds_transfer_ctx_reset(tctx); /* abort — REQ-DL-003 */
                /* NRC 0x72 generalProgrammingFailure — same NRC a flash
                 * write failure produces below, reached through the status
                 * code that says "transfer aborted" rather than the one
                 * that says "flash driver fault", so a policy refusal stays
                 * distinguishable from a hardware failure internally while
                 * being indistinguishable on the wire. */
                return UDS_STATUS_ERR_TRANSFER_ABORTED;
            }
        }
    }

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
            status = uds_service_0x36_flush(tctx, ops);
            if (status != UDS_STATUS_OK) {
                uds_image_policy_notify_abort();  /* [#232] drop partial state */
                uds_transfer_ctx_reset(tctx); /* abort on failure */
                return UDS_STATUS_ERR_PLATFORM;
            }
        }
    }

    /* Update bytes_remaining. */
    tctx->bytes_remaining -= (uint32_t)payload_len;

    /* [EDS#441] Remember it so a repeat is recognised. */
    tctx->last_block_valid = true;
    tctx->last_block_seq   = block_seq;

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

/* --------------------------------------------------------------------------
 * Upload block handler (SID 0x35 direction)
 * -------------------------------------------------------------------------- */

/**
 * @brief Handle one TransferData block in upload direction.
 *
 * The tester sends [0x36, blockSeqCounter] with no payload data; the ECU
 * reads the next chunk from flash via read_cb and returns it in the response.
 *
 * write_buf is reused as the read buffer — the field name is misleading in
 * the upload direction but renaming it would break 0x34/0x36 tests.
 *
 * @param[in]  ctx   Server context (validated by caller).
 * @param[in]  req   Upload request buffer.
 * @param[out] resp  Response buffer to populate.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_TP_UNEXPECTED_PDU on block sequence counter error.
 * @return UDS_STATUS_ERR_PLATFORM on read_cb failure.
 */
static uds_status_t s_handle_upload_block(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t           status;
    uint8_t                block_seq;
    uint32_t               bytes_to_read;
    const uds_flash_ops_t *ops;
    uds_transfer_ctx_t    *tctx;

    (void)ctx; /* validated by caller — not needed in upload path */

    tctx = uds_transfer_ctx_get();
    ops  = uds_flash_ops_get();

    block_seq = req->data[SVC_0x36_BLOCK_SEQ_OFFSET];

    /* [EDS#441] Repeat of the last block (lost response): Table 451 has the
     * server resend the previous data. It's still in write_buf, which is
     * only refilled by the next new block. */
    if (tctx->last_block_valid && (block_seq == tctx->last_block_seq)) {
        status = uds_service_write_pos_sid((uint8_t)UDS_SID_TRANSFER_DATA, resp);
        if (status != UDS_STATUS_OK) {
            return status;
        }
        resp->data[1U] = block_seq;
        (void)memcpy(&resp->data[2U], tctx->write_buf, (size_t)tctx->last_block_len);
        resp->length = (uint16_t)(2U + tctx->last_block_len);
        return UDS_STATUS_OK;
    }

    /* [EDS#441] All data already sent: NRC 0x24 (Table 454). */
    if (tctx->bytes_remaining == (uint32_t)0U) {
        return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE; /* mapped → NRC 0x24 */
    }

    /* --- Validate block sequence counter (same logic as download) --- */
    if (block_seq != tctx->next_expected_block_seq) {
        return UDS_STATUS_ERR_TP_UNEXPECTED_PDU; /* mapped → NRC 0x73 */
    }

    /* --- Determine how many bytes to read this block --- */
    bytes_to_read = (uint32_t)tctx->write_buf_capacity;
    if (bytes_to_read > tctx->bytes_remaining) {
        bytes_to_read = tctx->bytes_remaining;
    }

    /* --- Read from flash into write_buf (doubles as read buffer for upload) --- */
    status = ops->read_cb(tctx->next_write_address,
                          tctx->write_buf,
                          bytes_to_read);
    if (status != UDS_STATUS_OK) {
        uds_transfer_ctx_reset(tctx); /* abort on failure */
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* Advance read address and decrement remaining. */
    tctx->next_write_address += bytes_to_read;
    tctx->bytes_remaining    -= bytes_to_read;

    /* [EDS#441] Remember it so a repeat can be resent. */
    tctx->last_block_valid = true;
    tctx->last_block_seq   = block_seq;
    tctx->last_block_len   = (uint16_t)bytes_to_read;

    /* --- Advance block counter with wrap (REQ-DL-001 applies to upload too) --- */
    if (tctx->next_expected_block_seq == (uint8_t)SVC_0x36_MAX_BLOCK_SEQ) {
        tctx->next_expected_block_seq = (uint8_t)SVC_0x36_WRAP_BLOCK_SEQ;
    } else {
        tctx->next_expected_block_seq++;
    }

    /* --- Build positive response: [0x76, blockSeqCounter, data...] --- */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_TRANSFER_DATA, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->data[1U] = block_seq;
    (void)memcpy(&resp->data[2U], tctx->write_buf, (size_t)bytes_to_read);
    resp->length = (uint16_t)(2U + (uint16_t)bytes_to_read);

    return UDS_STATUS_OK;
}
