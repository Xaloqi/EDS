/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/uds_flash_ops.c
 *
 * PURPOSE: Flash operations table global registration — shared singleton.
 *
 * This translation unit holds the single global pointer to the registered
 * flash_ops_t.  It is the only place where the pointer is modified, so
 * any future thread-safety requirement can be addressed here without
 * touching service code.
 *
 * SAFETY:
 *   REQ-FLASH-001: Registration must occur before any 0x34 request.
 *   No dynamic allocation.  No recursion.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "uds_flash_ops.h"
#include "uds_types.h"

#include <stddef.h>

/* --------------------------------------------------------------------------
 * Static singleton
 * -------------------------------------------------------------------------- */

/** @brief Currently registered flash operations table. NULL until registered. */
static const uds_flash_ops_t *s_flash_ops = NULL;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

uds_status_t uds_flash_ops_register(const uds_flash_ops_t *ops)
{
    if (ops != NULL) {
        /* Validate that all mandatory callbacks are populated. */
        if (ops->erase_cb == NULL) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
        if (ops->write_cb == NULL) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
        if (ops->verify_cb == NULL) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
        /* At least one memory region must be present. */
        if ((ops->memory_map == NULL) || (ops->region_count == (uint8_t)0U)) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
        /* max_block_length must leave room for SID + block counter bytes. */
        if (ops->max_block_length == (uint16_t)0U) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
    }

    s_flash_ops = ops;
    return UDS_STATUS_OK;
}

const uds_flash_ops_t *uds_flash_ops_get(void)
{
    return s_flash_ops;
}
