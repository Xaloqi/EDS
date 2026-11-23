// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/eds_nvm_diag_nvs.h
 *
 * PURPOSE: One call that gives an example its NVM store on every board: the
 *          board overlay's "diag_nvs" partition (DTS node label eds_nvs_slot)
 *          on hardware, nothing to configure on native_sim [EDS#643].
 *
 *          Before this, canfd_ecu, sensor_ecu, bms_ecu and motor_controller_ecu
 *          called nvm_store_init(NULL), which the NVS backend rejects, so on a
 *          board the DTC mirror and the SecurityAccess counters lived in RAM.
 *          basic_ecu has its own copy of this logic (EDS#200); new examples use
 *          this one.
 * =============================================================================
 */

#ifndef EDS_NVM_DIAG_NVS_H
#define EDS_NVM_DIAG_NVS_H

#include "uds_types.h"
#include "zephyr_wdt.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the NVM store on the board's diag_nvs partition.
 *
 * The backend is the one cmake/eds_nvm_backend.cmake linked: the append backend
 * (DIAG_NVM_BACKEND_APPEND, two banks of half the partition) where NVS cannot
 * mount, NVS elsewhere. On native_sim, and on mr_canhubk3 before Zephyr 4.4
 * (no flash driver for the S32K344), the store takes no partition and this is
 * nvm_store_init(NULL).
 *
 * Call after diag_wdt_init() and before uds_generated_init().
 *
 * @param[in] wdt  Watchdog to feed across the append backend's bank erase
 *                 (up to 4 s on STM32H7), or NULL if the example arms none.
 *                 Ignored by the other backends.
 * @return UDS_STATUS_OK, or the status nvm_store_init() or the flash page
 *         lookup returned.
 */
uds_status_t eds_nvm_init_diag_nvs(diag_wdt_t *wdt);

#ifdef __cplusplus
}
#endif

#endif /* EDS_NVM_DIAG_NVS_H */
