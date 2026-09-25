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
#include "zephyr_wdt.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Zephyr flash operations for the UDS download services.
 *
 * Opens FIXED_PARTITION_ID(image_1), reads the base address and size into
 * the memory map descriptor, and registers the flash_ops_t table.
 *
 * Must be called before uds_generated_init().
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM if flash_area_open() fails.
 */
uds_status_t zephyr_flash_ops_init(void);

/**
 * @brief Register the watchdog instance to feed across a single bounded
 *        erase_step_cb() call [#312].
 *
 * OPTIONAL. Without it, erase_step_cb() still works but a single sector
 * erase (STM32H7: up to 4000 ms worst case) is not bridged and may exceed
 * a short poll-loop watchdog window. Call once from main(), after
 * diag_wdt_init(), and before uds_generated_init() (which calls
 * zephyr_flash_ops_init()) so the handle is in place before any 0x34 can
 * reach it. Pass NULL to stop feeding (e.g. in a test harness).
 *
 * @param[in] wdt  Watchdog instance to feed, or NULL.
 */
void zephyr_flash_ops_set_wdt(diag_wdt_t *wdt);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_FLASH_OPS_H */
