/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr_flash_ops.h
 *
 * PURPOSE: Zephyr platform flash operations initialisation API.
 *
 * Call zephyr_flash_ops_init() from main() before uds_generated_init().
 * This populates the memory region descriptor from the MCUboot secondary
 * slot (image_1) flash map and registers the flash ops table.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#ifndef ZEPHYR_FLASH_OPS_H
#define ZEPHYR_FLASH_OPS_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Zephyr flash operations for the UDS download services.
 *
 * Opens FLASH_AREA_ID(image_1), reads the base address and size into the
 * memory map descriptor, and registers the flash_ops_t table.
 *
 * Must be called before uds_generated_init().
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM if flash_area_open() fails.
 */
uds_status_t zephyr_flash_ops_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_FLASH_OPS_H */
