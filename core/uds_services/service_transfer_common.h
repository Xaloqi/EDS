/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_transfer_common.h
 *
 * PURPOSE: Shared static-inline helpers for memory-addressed UDS services.
 *
 * Included by:
 *   service_0x23.c (ReadMemoryByAddress)
 *   service_0x34.c (RequestDownload)
 *   service_0x35.c (RequestUpload)
 *   service_0x3D.c (WriteMemoryByAddress)
 *
 * All services share the addressAndLengthFormatIdentifier (ALFID) encoding
 * and the memory map range validation pattern.  Static inline avoids a
 * separate translation unit and keeps helpers adjacent to their callers.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#ifndef UDS_SERVICE_TRANSFER_COMMON_H
#define UDS_SERVICE_TRANSFER_COMMON_H

#include "uds_types.h"
#include "uds_flash_ops.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of bytes in a memoryAddress or memorySize field (ISO 14229-1). */
#define UDS_TRANSFER_MAX_FIELD_BYTES  (4U)

/** Minimum allowed value for ALFID address/size nibbles (0 = not present — rejected). */
#define UDS_TRANSFER_MIN_FIELD_BYTES  (1U)

/** Mask for the memoryAddressLength nibble in the ALFID byte (bits [3:0]). */
#define UDS_TRANSFER_ALFID_ADDR_MASK  (0x0FU)

/** Mask for the memorySizeLength nibble in the ALFID byte (bits [7:4]). */
#define UDS_TRANSFER_ALFID_SIZE_MASK  (0xF0U)

/** Right-shift to extract memorySizeLength from the upper nibble. */
#define UDS_TRANSFER_ALFID_SIZE_SHIFT (4U)

/**
 * @brief Parse a big-endian unsigned integer of 1–4 bytes from a buffer.
 *
 * @param[in]  buf   Pointer to the first byte.
 * @param[in]  n     Number of bytes (1–4).
 * @param[out] out   Parsed value.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_INVALID_PARAM if n > 4.
 */
static inline uds_status_t uds_transfer_parse_be(
    const uint8_t *buf,
    uint8_t        n,
    uint32_t      *out)
{
    uint32_t val = (uint32_t)0U;
    uint8_t  i;

    if (n > (uint8_t)UDS_TRANSFER_MAX_FIELD_BYTES) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    for (i = (uint8_t)0U; i < n; i++) {
        val = (val << (uint32_t)8U) | (uint32_t)buf[i];
    }

    *out = val;
    return UDS_STATUS_OK;
}

/**
 * @brief Decode an addressAndLengthFormatIdentifier byte.
 *
 * Extracts memoryAddressLength (bits [3:0]) and memorySizeLength (bits [7:4])
 * and validates both are in the range [1, 4].
 *
 * @param[in]  alfid_byte  Raw ALFID byte from the request PDU.
 * @param[out] addr_len    Decoded memoryAddressLength (1–4).
 * @param[out] size_len    Decoded memorySizeLength (1–4).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_INVALID_PARAM if either nibble is 0 or > 4.
 */
static inline uds_status_t uds_transfer_parse_alfid(
    uint8_t  alfid_byte,
    uint8_t *addr_len,
    uint8_t *size_len)
{
    uint8_t al = (uint8_t)(alfid_byte & (uint8_t)UDS_TRANSFER_ALFID_ADDR_MASK);
    uint8_t sl = (uint8_t)((alfid_byte & (uint8_t)UDS_TRANSFER_ALFID_SIZE_MASK) >>
                            (uint8_t)UDS_TRANSFER_ALFID_SIZE_SHIFT);

    if ((al < (uint8_t)UDS_TRANSFER_MIN_FIELD_BYTES) ||
        (al > (uint8_t)UDS_TRANSFER_MAX_FIELD_BYTES) ||
        (sl < (uint8_t)UDS_TRANSFER_MIN_FIELD_BYTES) ||
        (sl > (uint8_t)UDS_TRANSFER_MAX_FIELD_BYTES)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    *addr_len = al;
    *size_len = sl;
    return UDS_STATUS_OK;
}

/**
 * @brief Validate address + size against the writable regions of the flash memory map.
 *
 * Used by SID 0x34 (RequestDownload) and SID 0x3D (WriteMemoryByAddress).
 * REQ-FLASH-002.
 *
 * @param[in] ops      Registered flash ops table (not NULL).
 * @param[in] address  Start address of the requested region.
 * @param[in] size     Size in bytes of the requested region.
 *
 * @return UDS_STATUS_OK if the range lies within a writable region.
 * @return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE otherwise.
 */
static inline uds_status_t uds_transfer_validate_memory_range(
    const uds_flash_ops_t *ops,
    uint32_t               address,
    uint32_t               size)
{
    uint8_t  i;
    uint32_t req_end;
    uint32_t region_end;

    if (size == (uint32_t)0U) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    if (address > ((uint32_t)0xFFFFFFFFUL - size)) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    req_end = address + size;

    for (i = (uint8_t)0U; i < ops->region_count; i++) {
        const uds_flash_region_t *r = &ops->memory_map[i];

        if (!r->writable) {
            continue;
        }

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

/**
 * @brief Validate address + size against the readable regions of the flash memory map.
 *
 * Used by SID 0x23 (ReadMemoryByAddress).  Checks the `readable` flag so that
 * calibration ROM and configuration areas can be exposed for read without being
 * declared writable in the DFU memory map.
 * REQ-FLASH-003.
 *
 * @param[in] ops      Registered flash ops table (not NULL).
 * @param[in] address  Start address of the requested region.
 * @param[in] size     Size in bytes of the requested region.
 *
 * @return UDS_STATUS_OK if the range lies within a readable region.
 * @return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE otherwise.
 */
static inline uds_status_t uds_transfer_validate_readable_range(
    const uds_flash_ops_t *ops,
    uint32_t               address,
    uint32_t               size)
{
    uint8_t  i;
    uint32_t req_end;
    uint32_t region_end;

    if (size == (uint32_t)0U) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    if (address > ((uint32_t)0xFFFFFFFFUL - size)) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    req_end = address + size;

    for (i = (uint8_t)0U; i < ops->region_count; i++) {
        const uds_flash_region_t *r = &ops->memory_map[i];

        if (!r->readable) {
            continue;
        }

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

#ifdef __cplusplus
}
#endif

#endif /* UDS_SERVICE_TRANSFER_COMMON_H */
