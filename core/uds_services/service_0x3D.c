// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x3D.c
 *
 * PURPOSE: SID 0x3D — WriteMemoryByAddress service handler.
 *
 * ISO 14229-1:2020 §14.10: WriteMemoryByAddress allows a tester to write
 * directly to an ECU memory address without initiating a full 0x34/0x36/0x37
 * DFU transfer sequence.  Typical use cases: calibration constant patching,
 * post-production ECU configuration without a full firmware reflash.
 *
 * REQUEST FORMAT (ISO 14229-1 §14.10.2):
 *   [0x3D, addressAndLengthFormatIdentifier,
 *    memoryAddress[0..M], memorySize[0..N],
 *    dataRecord[0..memorySize-1]]
 *
 *   addressAndLengthFormatIdentifier (1 byte):
 *     Bits [7:4] = memorySizeLength  (1–4): bytes encoding memorySize
 *     Bits [3:0] = memoryAddressLength (1–4): bytes encoding memoryAddress
 *   memoryAddress[0..M]: destination write address, big-endian, M bytes.
 *   memorySize[0..N]:    number of bytes to write, big-endian, N bytes.
 *   dataRecord[0..]:     exactly memorySize bytes of data to write.
 *
 *   NOTE: No dataFormatIdentifier field (unlike SID 0x34/0x35).
 *
 * POSITIVE RESPONSE (ISO 14229-1 §14.10.3):
 *   [0x7D, addressAndLengthFormatIdentifier,
 *    memoryAddress[0..M], memorySize[0..N]]
 *
 * NRC BEHAVIOUR:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — request too short,
 *             ALFID nibbles out of range, or declared memorySize > data present
 *   NRC 0x22 (conditionsNotCorrect)                  — no flash ops or write_cb
 *   NRC 0x31 (requestOutOfRange)                     — address not in writable map,
 *             size zero, or address+size overflow
 *   NRC 0x72 (generalProgrammingFailure)             — write_cb returned error
 *
 * SESSION REQUIREMENT:
 *   Programming session (UDS_SESSION_PROGRAMMING) required.
 *   Security Level 1 unlock required.  ACL table enforces both.
 *
 * SAFETY:
 *   REQ-FLASH-002: Address + size validated against writable memory map before
 *   any write callback is invoked.  This is the ASIL-B gate for this service.
 *   Without memory map enforcement this service is equivalent to a raw memory
 *   write backdoor — the check must not be bypassed or made optional.
 *   write_cb must be non-NULL (checked explicitly before invocation).
 *   The 5-step ASIL-B safety chain in uds_server.c enforces session and
 *   security level before this handler is reached.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "services.h"
#include "service_transfer_common.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_flash_ops.h"

#include <stddef.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/** Minimum request length: [SID, ALFID] = 2 bytes.
 *  Actual minimum is longer — addr, size, and data fields follow. */
#define SVC_0x3D_MIN_REQ_LEN      (2U)

/** Offset of the ALFID byte in the request PDU. */
#define SVC_0x3D_ALFID_OFFSET     (1U)

/** Offset of the first memoryAddress byte in the request PDU. */
#define SVC_0x3D_MEM_ADDR_OFFSET  (2U)

/* --------------------------------------------------------------------------
 * SID 0x3D handler
 * -------------------------------------------------------------------------- */

/**
 * @brief SID 0x3D — WriteMemoryByAddress handler.
 *
 * Validates the request, checks the writable memory map, invokes write_cb,
 * and echoes address/size in the positive response per ISO 14229-1 §14.10.3.
 */
uds_status_t uds_service_0x3D_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t           status;
    uint8_t                alfid;
    uint8_t                addr_len;
    uint8_t                size_len;
    uint16_t               fields_end;
    uint16_t               data_offset;
    uint16_t               expected_total;
    uint32_t               mem_address = (uint32_t)0U;
    uint32_t               mem_size    = (uint32_t)0U;
    const uds_flash_ops_t *ops;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum length: [SID, ALFID]. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x3D_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* REQ-FLASH-001: Flash ops must be registered. */
    ops = uds_flash_ops_get();
    if (ops == NULL) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* write_cb must be populated. */
    if (ops->write_cb == NULL) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* --- Parse addressAndLengthFormatIdentifier --- */
    alfid  = req->data[SVC_0x3D_ALFID_OFFSET];
    status = uds_transfer_parse_alfid(alfid, &addr_len, &size_len);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* End of fixed-width address+size fields in the PDU. */
    fields_end = (uint16_t)((uint16_t)SVC_0x3D_MEM_ADDR_OFFSET +
                             (uint16_t)addr_len +
                             (uint16_t)size_len);
    if (req->length < fields_end) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* --- Parse memoryAddress (big-endian, addr_len bytes) --- */
    status = uds_transfer_parse_be(
        &req->data[SVC_0x3D_MEM_ADDR_OFFSET],
        addr_len,
        &mem_address);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* --- Parse memorySize (big-endian, size_len bytes) --- */
    status = uds_transfer_parse_be(
        &req->data[(uint16_t)SVC_0x3D_MEM_ADDR_OFFSET + (uint16_t)addr_len],
        size_len,
        &mem_size);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* data_offset is the byte index of the first data byte in req->data[]. */
    data_offset = fields_end;

    /* Request must contain exactly fields_end + mem_size bytes.
     * Guard against mem_size causing uint16_t overflow before comparing. */
    if (mem_size > (uint32_t)((uint16_t)0xFFFFU - data_offset)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    expected_total = (uint16_t)(data_offset + (uint16_t)mem_size);
    if (req->length < expected_total) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* REQ-FLASH-002: ASIL-B gate — validate address range against writable map.
     * This check must not be bypassed. */
    status = uds_transfer_validate_memory_range(ops, mem_address, mem_size);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* --- Invoke write_cb --- */
    status = ops->write_cb(mem_address, &req->data[data_offset], mem_size);
    if (status != UDS_STATUS_OK) {
        /* NRC 0x72 — generalProgrammingFailure (driver write error). */
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* --- Build positive response: [0x7D, ALFID, addr..., size...] --- */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_WRITE_MEMORY_BY_ADDRESS, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Echo ALFID. */
    resp->data[1U] = alfid;

    /* Echo memoryAddress bytes. */
    {
        uint8_t i;
        for (i = (uint8_t)0U; i < addr_len; i++) {
            resp->data[2U + i] =
                req->data[(uint16_t)SVC_0x3D_MEM_ADDR_OFFSET + (uint16_t)i];
        }
    }

    /* Echo memorySize bytes. */
    {
        uint8_t i;
        uint16_t size_src = (uint16_t)SVC_0x3D_MEM_ADDR_OFFSET + (uint16_t)addr_len;
        for (i = (uint8_t)0U; i < size_len; i++) {
            resp->data[2U + addr_len + i] = req->data[size_src + (uint16_t)i];
        }
    }

    resp->length = (uint16_t)(2U + (uint16_t)addr_len + (uint16_t)size_len);

    return UDS_STATUS_OK;
}
