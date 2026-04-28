// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: config/dtc_mirror.h
 *
 * PURPOSE: DTC NVM mirror — persist and restore DTC status bytes across resets.
 *
 *          DTC status bytes reside in the RAM-based dtc_database (s_dtc_table).
 *          Without persistence, all fault history is lost on ECU reset or
 *          power-cycle — a violation of ISO 14229-1 Annex D requirements for
 *          ConfirmedDTC and TestFailedSinceLastClear status bits.
 *
 *          This module:
 *            1. Loads the DTC mirror from NVM on boot (dtc_mirror_load).
 *            2. Saves the mirror to NVM on any status change (dtc_mirror_save).
 *            3. Provides a scheduled flush for bulk writes at service 0x14
 *               (ClearDiagnosticInformation) and ECU reset (0x11).
 *
 *          Wire points:
 *            - dtc_mirror_load()        → called from uds_stack_init()
 *            - dtc_mirror_save()        → called from dtc_database_set_status()
 *            - dtc_mirror_flush_all()   → called from zephyr_port_nvm_flush()
 *
 * WIRE FORMAT (NVM_KEY_DTC_MIRROR):
 *   [count:2][entry_0:4][entry_1:4]...[entry_n:4]
 *   Each entry: [dtc_code:3 big-endian][status_byte:1]
 *   Maximum payload: 2 + 128 × 4 = 514 bytes.
 *
 * SAFETY  : ASIL-B candidate. Confirmed DTC bits are safety-relevant.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef DTC_MIRROR_H
#define DTC_MIRROR_H

#include "uds_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Wire format constants
 * -------------------------------------------------------------------------- */

/** Header: 2-byte entry count. */
#define DTC_MIRROR_HEADER_BYTES  (2U)

/** Per-entry size: 3-byte DTC code + 1-byte status. */
#define DTC_MIRROR_ENTRY_BYTES   (4U)

/** Maximum mirror payload: header + max_entries × entry_size. */
#define DTC_MIRROR_MAX_BYTES     ((uint16_t)(DTC_MIRROR_HEADER_BYTES + \
                                  (UDS_MAX_DTC_COUNT * DTC_MIRROR_ENTRY_BYTES)))

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the DTC mirror module.
 *
 * Must be called after nvm_store_init() and before dtc_database_init().
 * Stores internal state — does not perform the load (call dtc_mirror_load
 * after dtc_database_init() to populate status bytes).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 */
uds_status_t dtc_mirror_init(void);

/**
 * @brief Load persisted DTC status bytes from NVM into the live database.
 *
 * Reads the NVM mirror and calls dtc_database_set_status() for each entry
 * found. Entries in the mirror for DTCs not registered in the database are
 * silently skipped (e.g. after a firmware update removes a DTC).
 *
 * Must be called after both nvm_store_init() and dtc_database_init()
 * (and after all DTCs have been registered via dtc_database_register()).
 *
 * @return UDS_STATUS_OK if mirror loaded (or no mirror found — first boot).
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if dtc_mirror_init() not called.
 * @return UDS_STATUS_ERR_PLATFORM if NVM read error.
 */
uds_status_t dtc_mirror_load(void);

/**
 * @brief Persist all current DTC status bytes to NVM.
 *
 * Serializes the entire dtc_database status array and writes it as a
 * single atomic record to NVM_KEY_DTC_MIRROR.
 *
 * Called from:
 *   - zephyr_port_nvm_flush() (before ECU reset)
 *   - SID 0x14 (ClearDiagnosticInformation) after clearing all DTCs
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if dtc_mirror_init() not called.
 * @return UDS_STATUS_ERR_PLATFORM if NVM write failed.
 */
uds_status_t dtc_mirror_flush_all(void);

/**
 * @brief Persist a single DTC status update to NVM immediately.
 *
 * Called by dtc_database_set_status() whenever a status byte changes.
 * Performs a full flush (atomic overwrite of the whole mirror record)
 * since NVS does not support partial record update.
 *
 * @param[in] dtc_code    The DTC code whose status changed.
 * @param[in] status_byte The new status byte value.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if not initialized.
 * @return UDS_STATUS_ERR_PLATFORM if NVM write failed.
 *
 * @note PERFORMANCE: This writes the entire mirror on each status change.
 *       For high-frequency DTC events, consider debouncing via a dirty flag
 *       and periodic flush (see dtc_mirror_flush_all). For Phase 3 the
 *       eager write strategy is used for simplicity and data integrity.
 */
uds_status_t dtc_mirror_save_one(uint32_t dtc_code, uint8_t status_byte);

/**
 * @brief Mark all DTC status bytes as cleared in NVM.
 *
 * Called after SID 0x14 (ClearDiagnosticInformation) completes.
 * Writes a mirror record with all status bytes set to 0x00.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if not initialized.
 * @return UDS_STATUS_ERR_PLATFORM if NVM write failed.
 */
uds_status_t dtc_mirror_clear_all(void);

/**
 * @brief Check whether the DTC mirror module is initialized.
 *
 * @return true if dtc_mirror_init() has completed.
 * @return false otherwise.
 */
bool dtc_mirror_is_ready(void);

#ifdef UNIT_TEST
/**
 * @brief Reset DTC mirror to power-on defaults.
 *
 * FOR TEST USE ONLY. Not available in production builds.
 * [MISRA 8.7] Prototype provided here so all callers have a visible
 * declaration; guarded so the symbol is unreachable in production.
 */
void dtc_mirror_test_reset(void);
#endif /* UNIT_TEST */

#ifdef __cplusplus
}
#endif

#endif /* DTC_MIRROR_H */
