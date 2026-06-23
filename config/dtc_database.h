// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: config/dtc_database.h
 *
 * PURPOSE: Public API for the Diagnostic Trouble Code (DTC) database.
 *          Stores DTC descriptor records per ISO 14229-1 and SAE J2012-DA,
 *          with support for status byte management per ISO 14229-1 Annex D.
 *
 *          The database is a fixed-capacity static array. Entries are
 *          populated at runtime via dtc_database_register(), called from
 *          the generated uds_init.c during stack initialisation. This
 *          decouples storage (this file) from content (generated/uds_init.c),
 *          enabling YAML-driven code generation without modifying this file.
 *
 * CAPACITY: UDS_MAX_DTC_COUNT entries (defined in uds_types.h, default 128).
 *           Bounded at compile time — no dynamic memory.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef DTC_DATABASE_H
#define DTC_DATABASE_H

#include "uds_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * DTC status byte bit definitions (ISO 14229-1 Table D.1)
 * -------------------------------------------------------------------------- */
#define DTC_STATUS_TEST_FAILED                          (0x01U)
#define DTC_STATUS_TEST_FAILED_THIS_OPERATION_CYCLE     (0x02U)
#define DTC_STATUS_PENDING_DTC                          (0x04U)
#define DTC_STATUS_CONFIRMED_DTC                        (0x08U)
#define DTC_STATUS_TEST_NOT_COMPLETED_SINCE_LAST_CLEAR  (0x10U)
#define DTC_STATUS_TEST_FAILED_SINCE_LAST_CLEAR         (0x20U)
#define DTC_STATUS_TEST_NOT_COMPLETED_THIS_OP_CYCLE     (0x40U)
#define DTC_STATUS_WARNING_INDICATOR_REQUESTED          (0x80U)

/* --------------------------------------------------------------------------
 * DTC descriptor record
 * -------------------------------------------------------------------------- */

/**
 * @brief Descriptor for a single Diagnostic Trouble Code entry.
 *
 * Registered via dtc_database_register() during stack initialisation.
 * status_byte is mutable — updated by dtc_database_set_status() at runtime.
 */
typedef struct dtc_entry {
    uint32_t    dtc_code;                 /**< 3-byte DTC code (e.g. 0xC00100). */
    uint8_t     status_byte;              /**< Current DTC status byte (ISO 14229-1 Table D.1). */
    uint8_t     severity;                 /**< Severity class (0x00 = not available). */
    uint8_t     fault_detection_counter;  /**< Debounce counter 0x00–0xFE; 0xFF = confirmed (excluded from 0x0B). */
    bool        is_permanent;             /**< true = SID 0x19/0x0B-eligible, NOT clearable by SID 0x14. */
    const char *description;              /**< Human-readable description (tooling/logging only). */
} dtc_entry_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the DTC database.
 *
 * Clears the internal registration table and resets all status bytes.
 * Must be called exactly once before dtc_database_register() or any
 * other dtc_database_* call.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 */
uds_status_t dtc_database_init(void);

/**
 * @brief Register one DTC entry with the database.
 *
 * Copies the descriptor into the internal static table with status_byte
 * initialised to 0x00 (no fault active). The caller's pointer need not
 * remain valid after this call returns.
 *
 * Called exclusively from generated uds_init.c during stack initialisation.
 * Must not be called after the poll loop starts.
 *
 * @param[in] dtc_code     24-bit DTC code to register (must be unique).
 * @param[in] severity     Severity class byte (0x00 if not applicable).
 * @param[in] description  Human-readable label (may be NULL).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if dtc_database_init() not called.
 * @return UDS_STATUS_ERR_INVALID_PARAM if dtc_code is 0 or already registered.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if UDS_MAX_DTC_COUNT already registered.
 */
uds_status_t dtc_database_register(
    uint32_t    dtc_code,
    uint8_t     severity,
    const char *description
);

/**
 * @brief Look up a DTC entry by DTC code.
 *
 * @param[in] dtc_code  24-bit DTC code to search for.
 *
 * @return Pointer to the matching dtc_entry_t, or NULL if not found
 *         or if the database is not initialized.
 *
 * @note Returned pointer is mutable — status_byte may be updated via
 *       dtc_database_set_status(). All other fields are read-only after
 *       registration.
 */
dtc_entry_t *dtc_database_find(uint32_t dtc_code);

/**
 * @brief Set the status byte for a registered DTC.
 *
 * @param[in] dtc_code    24-bit DTC code to update.
 * @param[in] status_byte New status byte value (ISO 14229-1 Annex D).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND if dtc_code is not registered.
 */
uds_status_t dtc_database_set_status(uint32_t dtc_code, uint8_t status_byte);

/**
 * @brief Clear all DTC status bytes (set to 0x00).
 *
 * Implements the ClearDiagnosticInformation (SID 0x14) data path.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 */
uds_status_t dtc_database_clear_all(void);

/**
 * @brief Get a DTC entry by sequential index.
 *
 * Provides ordered iteration over all registered DTCs.
 * Used by the DTC mirror serializer to persist status bytes.
 *
 * @param[in]  index        Zero-based index (0..registered_count-1).
 * @param[out] out_dtc_code Receives the DTC code at this index.
 * @param[out] out_status   Receives the current status byte.
 *
 * @return UDS_STATUS_OK if entry found and populated.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND if index >= registered count.
 * @return UDS_STATUS_ERR_NULL_PTR if any output pointer is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 */
uds_status_t dtc_database_get_by_index(
    uint16_t  index,
    uint32_t *out_dtc_code,
    uint8_t  *out_status);

/**
 * @brief Get the number of registered DTC entries.
 *
 * @param[out] out_count  Receives the registered DTC count.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if out_count is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 */
uds_status_t dtc_database_get_count(uint16_t *out_count);

/**
 * @brief Count DTC entries matching a status mask.
 *
 * @param[in]  status_mask  Bitmask — count entries where
 *                          (status_byte & status_mask) != 0.
 * @param[out] out_count    Receives the count.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if out_count is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 */
uds_status_t dtc_database_count_by_status(
    uint8_t   status_mask,
    uint16_t *out_count
);

/**
 * @brief Set the fault detection counter for a registered DTC.
 *
 * Used by the application to track debouncing state (ISO 14229-1 §11.3.11).
 * 0x00–0xFE: counter value. 0xFF is reserved (confirmed) — excluded from
 * the 0x19/0x0B reportDTCFaultDetectionCounter response.
 *
 * @param[in] dtc_code  24-bit DTC code to update.
 * @param[in] counter   New fault detection counter value.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND if dtc_code is not registered.
 */
uds_status_t dtc_database_set_fault_counter(uint32_t dtc_code, uint8_t counter);

/**
 * @brief Mark or unmark a DTC as permanent.
 *
 * Permanent DTCs are returned by SID 0x19 sub-function 0x19 and are NOT
 * cleared by SID 0x14 ClearDiagnosticInformation. The application is
 * responsible for clearing the permanent flag after a successful drive cycle
 * without fault recurrence.
 *
 * @param[in] dtc_code   24-bit DTC code to update.
 * @param[in] permanent  true = mark permanent; false = clear permanent.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND if dtc_code is not registered.
 */
uds_status_t dtc_database_set_permanent(uint32_t dtc_code, bool permanent);

/**
 * @brief Clear status bytes for all non-permanent DTCs.
 *
 * Called by SID 0x14 ClearDiagnosticInformation. DTCs marked is_permanent
 * are skipped — their status bytes are preserved until the application
 * calls dtc_database_set_permanent(dtc_code, false) after drive-cycle healing.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 */
uds_status_t dtc_database_clear_non_permanent(void);

#ifdef UNIT_TEST
/**
 * @brief Reset DTC database to power-on defaults.
 *
 * FOR TEST USE ONLY. Not available in production builds.
 * [MISRA 8.7] Prototype provided here so all callers have a visible
 * declaration; guarded so the symbol is unreachable in production.
 */
void dtc_database_test_reset(void);
#endif /* UNIT_TEST */

#ifdef __cplusplus
}
#endif

#endif /* DTC_DATABASE_H */

// ✅ End of dtc_database.h
