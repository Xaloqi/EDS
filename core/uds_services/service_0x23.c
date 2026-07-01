// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x23.c
 *
 * PURPOSE: SID 0x23 — ReadMemoryByAddress service handler.
 *
 * ISO 14229-1:2020 §14.9: ReadMemoryByAddress allows a tester to read from an
 * arbitrary ECU memory address without going through the DID database.  Primary
 * use cases: calibration constant inspection, configuration register readback,
 * embedded RAM variable monitoring during bench testing.
 *
 * REQUEST FORMAT (ISO 14229-1 §14.9.2):
 *   [0x23, addressAndLengthFormatIdentifier,
 *    memoryAddress[0..M], memorySize[0..N]]
 *
 *   addressAndLengthFormatIdentifier (1 byte):
 *     Bits [7:4] = memorySizeLength  (1–4): bytes encoding memorySize
 *     Bits [3:0] = memoryAddressLength (1–4): bytes encoding memoryAddress
 *   memoryAddress[0..M]: target read address, big-endian, M bytes.
 *   memorySize[0..N]:    number of bytes to read, big-endian, N bytes.
 *
 *   NOTE: No dataFormatIdentifier field (unlike SID 0x34/0x35).
 *
 * POSITIVE RESPONSE (ISO 14229-1 §14.9.3):
 *   [0x63, dataRecord[0..memorySize-1]]
 *
 * NRC BEHAVIOUR:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — request too short or
 *             ALFID nibbles out of range
 *   NRC 0x22 (conditionsNotCorrect)                  — no flash ops or read_cb
 *   NRC 0x31 (requestOutOfRange)                     — address not in readable map,
 *             size zero, or overflow, or response would overflow payload buffer
 *   NRC 0x72 (generalProgrammingFailure)             — read_cb returned error
 *
 * SESSION REQUIREMENT:
 *   Programming session (UDS_SESSION_PROGRAMMING) required.
 *   Security Level 1 unlock required.  ACL table enforces both.
 *
 * SAFETY:
 *   REQ-FLASH-003: Address + size validated against readable memory map before
 *   any read callback is invoked.  This is the sole ASIL-B gate for this service —
 *   without it the service becomes an unconstrained raw memory read interface.
 *   read_cb must be non-NULL (checked explicitly before invocation).
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
 *  Actual minimum is longer — addr and size fields depend on ALFID nibbles. */
#define SVC_0x23_MIN_REQ_LEN      (2U)

/** Offset of the ALFID byte in the request PDU. */
#define SVC_0x23_ALFID_OFFSET     (1U)

/** Offset of the first memoryAddress byte in the request PDU. */
#define SVC_0x23_MEM_ADDR_OFFSET  (2U)

/** Maximum bytes readable in a single request: response is [0x63, data...],
 *  so data payload must fit in UDS_MAX_PAYLOAD_LEN - 1 bytes. */
#define SVC_0x23_MAX_READ_BYTES   ((uint32_t)(UDS_MAX_PAYLOAD_LEN - 1U))

/* --------------------------------------------------------------------------
 * SID 0x23 handler
 * -------------------------------------------------------------------------- */

/**
 * @brief SID 0x23 — ReadMemoryByAddress handler.
 *
 * Validates the request, checks the readable memory map, invokes read_cb,
 * and returns the memory contents in the positive response.
 */
uds_status_t uds_service_0x23_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t           status;
    uint8_t                alfid;
    uint8_t                addr_len;
    uint8_t                size_len;
    uint16_t               fields_end;
    uint32_t               mem_address = (uint32_t)0U;
    uint32_t               mem_size    = (uint32_t)0U;
    const uds_flash_ops_t *ops;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum length: [SID, ALFID]. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x23_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* REQ-FLASH-001: Flash ops must be registered. */
    ops = uds_flash_ops_get();
    if (ops == NULL) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* read_cb must be populated — read without it is impossible. */
    if (ops->read_cb == NULL) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* --- Parse addressAndLengthFormatIdentifier --- */
    alfid  = req->data[SVC_0x23_ALFID_OFFSET];
    status = uds_transfer_parse_alfid(alfid, &addr_len, &size_len);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Check the request is long enough to hold address + size fields. */
    fields_end = (uint16_t)((uint16_t)SVC_0x23_MEM_ADDR_OFFSET +
                             (uint16_t)addr_len +
                             (uint16_t)size_len);
    if (req->length < fields_end) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* --- Parse memoryAddress (big-endian, addr_len bytes) --- */
    status = uds_transfer_parse_be(
        &req->data[SVC_0x23_MEM_ADDR_OFFSET],
        addr_len,
        &mem_address);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* --- Parse memorySize (big-endian, size_len bytes) --- */
    status = uds_transfer_parse_be(
        &req->data[(uint16_t)SVC_0x23_MEM_ADDR_OFFSET + (uint16_t)addr_len],
        size_len,
        &mem_size);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Guard against response buffer overflow: [0x63, data...] must fit. */
    if (mem_size > SVC_0x23_MAX_READ_BYTES) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* REQ-FLASH-003: ASIL-B gate — validate address range against readable map. */
    status = uds_transfer_validate_readable_range(ops, mem_address, mem_size);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* --- Build positive response header --- */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_MEMORY_BY_ADDRESS, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* --- Invoke read_cb to populate response data --- */
    status = ops->read_cb(mem_address, &resp->data[1U], mem_size);
    if (status != UDS_STATUS_OK) {
        /* NRC 0x72 — generalProgrammingFailure (driver read error). */
        return UDS_STATUS_ERR_PLATFORM;
    }

    resp->length = (uint16_t)(1U + (uint16_t)mem_size);

    return UDS_STATUS_OK;
}
