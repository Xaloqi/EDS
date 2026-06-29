/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x35.c
 *
 * PURPOSE: SID 0x35 — RequestUpload service handler.
 *
 * ISO 14229-1 §14.5: RequestUpload initiates a data transfer from the ECU
 * to the external test tool (tester).  It is the symmetric counterpart to
 * RequestDownload (0x34).  Primary use cases: calibration data readback,
 * NVM log extraction, flash image verification readout.
 *
 * REQUEST FORMAT (ISO 14229-1 §14.5.2 — identical field layout to 0x34):
 *   [0x35, dataFormatIdentifier, addressAndLengthFormatIdentifier,
 *    memoryAddress[0..M], memorySize[0..N]]
 *
 *   dataFormatIdentifier (1 byte):
 *     Only 0x00 accepted (uncompressed / unencrypted).
 *
 *   addressAndLengthFormatIdentifier (1 byte):
 *     Bits [7:4] = memorySizeLength   (1–4)
 *     Bits [3:0] = memoryAddressLength (1–4)
 *
 *   memoryAddress[0..M]:  big-endian, M = memoryAddressLength bytes.
 *   memorySize[0..N]:     big-endian, N = memorySizeLength bytes.
 *
 * POSITIVE RESPONSE (ISO 14229-1 §14.5.3 — same structure as 0x74):
 *   [0x75, lengthFormatIdentifier, maxNumberOfBlockLength_Hi, maxNumberOfBlockLength_Lo]
 *
 *   lengthFormatIdentifier:
 *     Bits [7:4] = 2  (number of bytes encoding maxNumberOfBlockLength)
 *     Bits [3:0] = 0  (reserved)
 *   maxNumberOfBlockLength: uint16_t, includes the 1-byte blockSequenceCounter.
 *
 * TRANSFER EXCHANGE:
 *   Tester                           ECU
 *     0x35 [addr, size]             →
 *                                   ← 0x75 [LFI=0x20, maxBlock=0x0101]
 *     0x36 [blockSeq=0x01]          →
 *                                   ← 0x76 [blockSeq=0x01, data[0..255]]
 *     0x37                          →
 *                                   ← 0x77
 *
 * NRC BEHAVIOUR:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — request too short
 *   NRC 0x22 (conditionsNotCorrect)                  — read_cb not registered
 *   NRC 0x31 (requestOutOfRange)                     — invalid address/size
 *   NRC 0x7F (serviceNotSupportedInActiveSession)    — wrong session
 *
 * SESSION REQUIREMENT:
 *   Programming session (UDS_SESSION_PROGRAMMING) required.
 *   Security Level 1 unlock required.  ACL table enforces both.
 *   Upload exposes raw ECU memory — equivalent access risk to download.
 *
 * SAFETY:
 *   REQ-DL-001: Block counter initialised to 0x01 on successful 0x35.
 *   REQ-DL-002: bytes_remaining set to total_size_bytes from request.
 *   REQ-FLASH-002: address + size validated against flash memory map.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi

#include "services.h"
#include "service_transfer_common.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_transfer_ctx.h"
#include "uds_flash_ops.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Constants — mirror exactly from service_0x34.c with SVC_0x35_ prefix.
 * -------------------------------------------------------------------------- */

/** Minimum request length: [SID, dataFmt, addrLenFmt] = 3 bytes. */
#define SVC_0x35_MIN_REQ_LEN          (3U)

/** Offset of dataFormatIdentifier in the request PDU. */
#define SVC_0x35_DATA_FMT_OFFSET      (1U)

/** Offset of addressAndLengthFormatIdentifier in the request PDU. */
#define SVC_0x35_ALFID_OFFSET         (2U)

/** Offset of the first memoryAddress byte in the request PDU. */
#define SVC_0x35_MEM_ADDR_OFFSET      (3U)

/** Only uncompressed / unencrypted data is accepted. */
#define SVC_0x35_ACCEPTED_DATA_FMT    (0x00U)

/** Number of bytes used to encode maxNumberOfBlockLength in the response. */
#define SVC_0x35_MXBL_BYTE_COUNT      (2U)

/* --------------------------------------------------------------------------
 * SID 0x35 handler
 * -------------------------------------------------------------------------- */

/**
 * @brief SID 0x35 — RequestUpload handler.
 *
 * Validates the request, checks that read_cb is registered, initialises the
 * transfer state machine in upload direction, and returns maxNumberOfBlockLength.
 * Does NOT call erase_cb — upload is read-only.
 */
uds_status_t uds_service_0x35_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t           status;
    uint8_t                data_fmt;
    uint8_t                alfid;
    uint8_t                addr_len;
    uint8_t                size_len;
    uint16_t               fields_end;
    uint32_t               mem_address = (uint32_t)0U;
    uint32_t               mem_size    = (uint32_t)0U;
    const uds_flash_ops_t *ops;
    uds_transfer_ctx_t    *tctx;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum length: [SID, dataFmt, addrLenFmt]. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x35_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Flash ops must be registered. */
    ops = uds_flash_ops_get();
    if (ops == NULL) {
        /* NRC 0x22 — conditions not correct (flash ops not registered). */
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* read_cb must be registered — upload requires flash read capability. */
    if (ops->read_cb == NULL) {
        /* NRC 0x22 — conditions not correct (read_cb not populated). */
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* --- Parse dataFormatIdentifier --- */
    data_fmt = req->data[SVC_0x35_DATA_FMT_OFFSET];
    if (data_fmt != (uint8_t)SVC_0x35_ACCEPTED_DATA_FMT) {
        /* Compression or encryption requested — not supported. */
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* --- Parse addressAndLengthFormatIdentifier --- */
    alfid  = req->data[SVC_0x35_ALFID_OFFSET];
    status = uds_transfer_parse_alfid(alfid, &addr_len, &size_len);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Check the request is long enough to hold address + size fields. */
    fields_end = (uint16_t)((uint16_t)SVC_0x35_MEM_ADDR_OFFSET +
                             (uint16_t)addr_len +
                             (uint16_t)size_len);

    if (req->length < fields_end) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* --- Parse memoryAddress (big-endian, addr_len bytes) --- */
    status = uds_transfer_parse_be(
        &req->data[SVC_0x35_MEM_ADDR_OFFSET],
        addr_len,
        &mem_address);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* --- Parse memorySize (big-endian, size_len bytes) --- */
    status = uds_transfer_parse_be(
        &req->data[(uint16_t)SVC_0x35_MEM_ADDR_OFFSET + (uint16_t)addr_len],
        size_len,
        &mem_size);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* REQ-FLASH-002: Validate address range against registered memory map. */
    status = uds_transfer_validate_memory_range(ops, mem_address, mem_size);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* Upload does NOT erase flash — read-only access. */

    /* --- Initialise transfer state machine (upload direction) --- */
    tctx = uds_transfer_ctx_get();
    uds_transfer_ctx_reset(tctx);   /* abort any in-progress transfer */

    tctx->state                   = UDS_TRANSFER_ACTIVE;
    tctx->direction               = UDS_TRANSFER_DIR_UPLOAD;
    tctx->target_address          = mem_address;
    tctx->total_size_bytes        = mem_size;
    tctx->bytes_remaining         = mem_size;
    tctx->next_write_address      = mem_address;
    tctx->next_expected_block_seq = (uint8_t)0x01U; /* REQ-DL-001 */
    tctx->crc_accumulator         = (uint32_t)0xFFFFFFFFUL; /* CRC init */
    tctx->write_buf_fill          = (uint16_t)0U;

    /* Capacity is min(ops->max_block_length, sizeof(write_buf)).
     * Subtract 1 for the block counter byte carried in the 0x36 request. */
    {
        uint16_t raw_cap = ops->max_block_length;
        if (raw_cap > (uint16_t)sizeof(tctx->write_buf)) {
            raw_cap = (uint16_t)sizeof(tctx->write_buf);
        }
        tctx->write_buf_capacity = raw_cap;
    }

    /* --- Build positive response --- */
    /* [0x75, lengthFormatIdentifier, maxNumberOfBlockLength_Hi, maxNumberOfBlockLength_Lo] */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_REQUEST_UPLOAD, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* lengthFormatIdentifier: bits [7:4] = 2 (2 bytes for maxBlockLen), [3:0] = 0. */
    resp->data[1U] = (uint8_t)((uint8_t)SVC_0x35_MXBL_BYTE_COUNT << (uint8_t)4U);

    /* maxNumberOfBlockLength includes the 1-byte block counter field. */
    {
        uint16_t max_block = (uint16_t)(tctx->write_buf_capacity + (uint16_t)1U);
        resp->data[2U] = (uint8_t)((max_block >> (uint16_t)8U) & (uint16_t)0xFFU);
        resp->data[3U] = (uint8_t)( max_block                  & (uint16_t)0xFFU);
    }
    resp->length = (uint16_t)4U;

    return UDS_STATUS_OK;
}
