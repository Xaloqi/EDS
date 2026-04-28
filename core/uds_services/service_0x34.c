/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x34.c
 *
 * PURPOSE: SID 0x34 — RequestDownload service handler.
 *
 * ISO 14229-1 §14.2: RequestDownload initiates a data transfer from the
 * external test tool (tester) to the ECU.  It is the mandatory first step
 * in a firmware update (DFU) sequence:
 *
 *   1. Erase flash  — SID 0x31 routine 0xFF10 (EraseApplicationFlash)
 *   2. Open session — SID 0x34 RequestDownload         ← THIS FILE
 *   3. Send data    — SID 0x36 TransferData (repeated)
 *   4. Commit       — SID 0x37 RequestTransferExit
 *   5. Verify       — SID 0x31 routine 0xFF11 (VerifyApplicationImage)
 *
 * REQUEST FORMAT (ISO 14229-1 §14.2.2):
 *   [0x34, dataFormatIdentifier, addressAndLengthFormatIdentifier,
 *    memoryAddress[0..M], memorySize[0..N]]
 *
 *   dataFormatIdentifier (1 byte):
 *     Bits [7:4] = compressionMethod  (0x0 = uncompressed)
 *     Bits [3:0] = encryptingMethod   (0x0 = unencrypted)
 *     This implementation only accepts 0x00.
 *
 *   addressAndLengthFormatIdentifier (1 byte):
 *     Bits [7:4] = memorySizeLength  (number of bytes encoding memorySize)
 *     Bits [3:0] = memoryAddressLength (number of bytes encoding memoryAddress)
 *     Both fields must be 1–4.
 *
 *   memoryAddress[0..M]:  big-endian, M = memoryAddressLength bytes.
 *   memorySize[0..N]:     big-endian, N = memorySizeLength bytes.
 *
 * POSITIVE RESPONSE (ISO 14229-1 §14.2.3):
 *   [0x74, lengthFormatIdentifier, maxNumberOfBlockLength[0..K]]
 *
 *   lengthFormatIdentifier:
 *     Bits [7:4] = number of bytes encoding maxNumberOfBlockLength (always 2)
 *     Bits [3:0] = reserved (0x0)
 *   maxNumberOfBlockLength: 16-bit value including the 1-byte block counter.
 *
 * NRC BEHAVIOUR:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — request too short
 *   NRC 0x22 (conditionsNotCorrect)                  — no flash ops registered
 *   NRC 0x31 (requestOutOfRange)                     — address/size invalid
 *   NRC 0x70 (uploadDownloadNotAccepted)              — flash erase failed
 *   NRC 0x7F (serviceNotSupportedInActiveSession)     — wrong session
 *
 * SESSION REQUIREMENT:
 *   Programming session (UDS_SESSION_PROGRAMMING) required.
 *   The default ACL table enforces this.
 *
 * SAFETY:
 *   REQ-DL-001: Block counter initialised to 0x01 on successful 0x34.
 *   REQ-DL-002: bytes_remaining set to total_size_bytes from request.
 *   REQ-FLASH-002: address + size validated against flash memory map.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_transfer_ctx.h"
#include "uds_flash_ops.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/** Minimum request length: [SID, dataFmt, addrLenFmt] = 3 bytes.
 *  Actual minimum depends on addrLenFmt fields — validated dynamically. */
#define SVC_0x34_MIN_REQ_LEN          (3U)

/** Offset of dataFormatIdentifier in the request PDU. */
#define SVC_0x34_DATA_FMT_OFFSET      (1U)

/** Offset of addressAndLengthFormatIdentifier in the request PDU. */
#define SVC_0x34_ALFID_OFFSET         (2U)

/** Offset of the first memoryAddress byte in the request PDU. */
#define SVC_0x34_MEM_ADDR_OFFSET      (3U)

/** Only uncompressed / unencrypted data is accepted. */
#define SVC_0x34_ACCEPTED_DATA_FMT    (0x00U)

/** Mask for addressLength nibble (bits [3:0]). */
#define SVC_0x34_ADDR_LEN_MASK        (0x0FU)

/** Mask for sizeLength nibble (bits [7:4]) — shift right by 4 after masking. */
#define SVC_0x34_SIZE_LEN_MASK        (0xF0U)
#define SVC_0x34_SIZE_LEN_SHIFT       (4U)

/** Maximum allowed value for addressLength and sizeLength fields. */
#define SVC_0x34_MAX_FIELD_BYTES      (4U)

/** Minimum allowed value (0 means "not present" — rejected). */
#define SVC_0x34_MIN_FIELD_BYTES      (1U)

/** Number of bytes used to encode maxNumberOfBlockLength in the response
 *  (always 2 bytes: one uint16_t). Stored in bits [7:4] of
 *  lengthFormatIdentifier. */
#define SVC_0x34_MXBL_BYTE_COUNT      (2U)

/* --------------------------------------------------------------------------
 * Static helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Parse a big-endian unsigned integer of 1–4 bytes from a buffer.
 *
 * @param[in]  buf    Pointer to the first byte.
 * @param[in]  n      Number of bytes (1–4).
 * @param[out] out    Parsed value.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_INVALID_PARAM if n > 4.
 */
static uds_status_t s_parse_big_endian(
    const uint8_t *buf,
    uint8_t        n,
    uint32_t      *out)
{
    uint32_t val = (uint32_t)0U;
    uint8_t  i;

    if (n > (uint8_t)SVC_0x34_MAX_FIELD_BYTES) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    for (i = (uint8_t)0U; i < n; i++) {
        val = (val << (uint32_t)8U) | (uint32_t)buf[i];
    }

    *out = val;
    return UDS_STATUS_OK;
}

/**
 * @brief Validate address + size against the registered flash memory map.
 *
 * REQ-FLASH-002.
 *
 * @param[in] ops      Registered flash ops table (not NULL).
 * @param[in] address  Start address of the requested download region.
 * @param[in] size     Size in bytes of the requested download region.
 *
 * @return UDS_STATUS_OK if the range lies within a writable region.
 * @return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE otherwise.
 */
static uds_status_t s_validate_memory_range(
    const uds_flash_ops_t *ops,
    uint32_t               address,
    uint32_t               size)
{
    uint8_t  i;
    uint32_t req_end;
    uint32_t region_end;

    /* Prevent address + size overflow. */
    if (size == (uint32_t)0U) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* Check for address arithmetic overflow. */
    if (address > ((uint32_t)0xFFFFFFFFUL - size)) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    req_end = address + size;

    for (i = (uint8_t)0U; i < ops->region_count; i++) {
        const uds_flash_region_t *r = &ops->memory_map[i];

        if (!r->writable) {
            continue;
        }

        /* Guard region_end overflow. */
        if (r->base_address > ((uint32_t)0xFFFFFFFFUL - r->size_bytes)) {
            continue;
        }

        region_end = r->base_address + r->size_bytes;

        if ((address >= r->base_address) && (req_end <= region_end)) {
            return UDS_STATUS_OK;
        }
    }

    return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
}

/* --------------------------------------------------------------------------
 * SID 0x34 handler
 * -------------------------------------------------------------------------- */

/**
 * @brief SID 0x34 — RequestDownload handler.
 *
 * Validates the request, erases the target flash region, initialises the
 * transfer state machine, and returns maxNumberOfBlockLength.
 */
uds_status_t uds_service_0x34_handler(
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
    status = uds_service_validate_length(req, (uint16_t)SVC_0x34_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* REQ-FLASH-001: Flash ops must be registered. */
    ops = uds_flash_ops_get();
    if (ops == NULL) {
        /* NRC 0x22 — conditions not correct (flash ops not registered). */
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* --- Parse dataFormatIdentifier --- */
    data_fmt = req->data[SVC_0x34_DATA_FMT_OFFSET];
    if (data_fmt != (uint8_t)SVC_0x34_ACCEPTED_DATA_FMT) {
        /* Compression or encryption requested — not supported. */
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* --- Parse addressAndLengthFormatIdentifier --- */
    alfid    = req->data[SVC_0x34_ALFID_OFFSET];
    addr_len = (uint8_t)( alfid & (uint8_t)SVC_0x34_ADDR_LEN_MASK);
    size_len = (uint8_t)((alfid & (uint8_t)SVC_0x34_SIZE_LEN_MASK) >>
                          (uint8_t)SVC_0x34_SIZE_LEN_SHIFT);

    /* Both fields must be 1–4 bytes. */
    if ((addr_len < (uint8_t)SVC_0x34_MIN_FIELD_BYTES) ||
        (addr_len > (uint8_t)SVC_0x34_MAX_FIELD_BYTES) ||
        (size_len < (uint8_t)SVC_0x34_MIN_FIELD_BYTES) ||
        (size_len > (uint8_t)SVC_0x34_MAX_FIELD_BYTES)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Check the request is long enough to hold address + size fields.
     * fields_end = SVC_0x34_MEM_ADDR_OFFSET + addr_len + size_len. */
    fields_end = (uint16_t)((uint16_t)SVC_0x34_MEM_ADDR_OFFSET +
                             (uint16_t)addr_len +
                             (uint16_t)size_len);

    if (req->length < fields_end) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* --- Parse memoryAddress (big-endian, addr_len bytes) --- */
    status = s_parse_big_endian(
        &req->data[SVC_0x34_MEM_ADDR_OFFSET],
        addr_len,
        &mem_address);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* --- Parse memorySize (big-endian, size_len bytes) --- */
    status = s_parse_big_endian(
        &req->data[(uint16_t)SVC_0x34_MEM_ADDR_OFFSET + (uint16_t)addr_len],
        size_len,
        &mem_size);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* REQ-FLASH-002: Validate address range against registered memory map. */
    status = s_validate_memory_range(ops, mem_address, mem_size);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* --- Erase the target flash region --- */
    status = ops->erase_cb(mem_address, mem_size);
    if (status != UDS_STATUS_OK) {
        /* NRC 0x70 — uploadDownloadNotAccepted (erase failure). */
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* --- Initialise transfer state machine --- */
    tctx = uds_transfer_ctx_get();
    uds_transfer_ctx_reset(tctx);   /* abort any in-progress transfer */

    tctx->state                   = UDS_TRANSFER_ACTIVE;
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
    /* [0x74, lengthFormatIdentifier, maxNumberOfBlockLength_Hi, maxNumberOfBlockLength_Lo] */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_REQUEST_DOWNLOAD, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* lengthFormatIdentifier: bits [7:4] = number of maxBlockLen bytes = 2,
     * bits [3:0] = 0 (reserved). */
    resp->data[1U] = (uint8_t)((uint8_t)SVC_0x34_MXBL_BYTE_COUNT << (uint8_t)4U);

    /* maxNumberOfBlockLength includes the 1-byte block counter field. */
    {
        uint16_t max_block = (uint16_t)(tctx->write_buf_capacity + (uint16_t)1U);
        resp->data[2U] = (uint8_t)((max_block >> (uint16_t)8U) & (uint16_t)0xFFU);
        resp->data[3U] = (uint8_t)( max_block                  & (uint16_t)0xFFU);
    }
    resp->length = (uint16_t)4U;

    return UDS_STATUS_OK;
}
