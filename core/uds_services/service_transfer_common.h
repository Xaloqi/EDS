/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_transfer_common.h
 *
 * PURPOSE: Shared static-inline helpers for SID 0x34 and 0x35 request parsing.
 *
 * Included by service_0x34.c (RequestDownload) and service_0x35.c
 * (RequestUpload).  Both services parse identical PDU layouts for
 * addressAndLengthFormatIdentifier, memoryAddress, and memorySize fields.
 *
 * Declaring the helpers as static inline avoids a separate translation unit
 * and keeps the functions close to their callers without duplication.
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
 * @brief Validate address + size against the registered flash memory map.
 *
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

#ifdef __cplusplus
}
#endif

#endif /* UDS_SERVICE_TRANSFER_COMMON_H */
