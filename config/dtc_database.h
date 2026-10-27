// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: config/dtc_database.h
 *
 * PURPOSE: Public API for the Diagnostic Trouble Code (DTC) database.
 *          Stores DTC descriptor records per ISO 14229-1 and SAE J2012-DA,
 *          with support for status byte management per ISO 14229-1:2026 Annex D.2.
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

/**
 * [EDS#448] Status of a DTC that was never tested or was just cleared: the two
 * testNotCompleted bits set, everything else clear (ISO 14229-1:2026 Annex
 * D.2: Tables D.6 and D.8 reset bits 4 and 6 to 1 on ClearDiagnosticInformation
 * and Figures D.5/D.7 start them at 1 on Init; bits 0-3, 5, 7 reset to 0).
 * EDS used 0x00, which claims the test has completed and passed.
 */
#define DTC_STATUS_INIT_AND_CLEARED                     (0x50U)

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
    uint8_t     severity;                 /**< DTCSeverity byte, ISO 14229-1:2013 Annex D.3 (bits 7-5 severity, 4-0 class). */
    uint8_t     fault_detection_counter;  /**< Debounce counter 0x00–0xFE; 0xFF = confirmed (excluded from 0x14). */
    bool        is_permanent;             /**< true = SID 0x19/0x15-eligible, NOT clearable by SID 0x14. */
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
 *       dtc_database_set_status() and the clear/restore functions. All other fields are read-only after
 *       registration.
 */
dtc_entry_t *dtc_database_find(uint32_t dtc_code);

/**
 * @brief Set the status byte for a registered DTC (monitor result).
 *
 * [EDS#448] Refused while ControlDTCSetting is off: ISO 14229-1:2026 Annex
 * D.2.1 ("DTC Status Update Condition") allows no status transition then,
 * except a ClearDiagnosticInformation reset. Core used to leave that check
 * to the application.
 *
 * [EDS#145] The only producer of the first/most recent records: a 0->1 edge
 * of testFailed or confirmedDTC here updates them (see dtc_record_t).
 *
 * @param[in] dtc_code    24-bit DTC code to update.
 * @param[in] status_byte New status byte value (ISO 14229-1:2026 Annex D.2).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND if dtc_code is not registered.
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET if DTC setting is off (unchanged).
 */
uds_status_t dtc_database_set_status(uint32_t dtc_code, uint8_t status_byte);

/**
 * @brief [EDS#448] Restore a persisted status byte at boot (NVM mirror load).
 *
 * Not gated on ControlDTCSetting: it restores state, it is not a monitor
 * transition. For dtc_mirror_load() only.
 */
uds_status_t dtc_database_restore_status(uint32_t dtc_code, uint8_t status_byte);

/**
 * @brief [EDS#448, EDS#450] Clear one DTC: status to DTC_STATUS_INIT_AND_CLEARED
 *        and fault-detection counter to 0. Not gated on ControlDTCSetting (a
 *        ClearDiagnosticInformation reset is exempt, Annex D.2.1).
 *
 * @return UDS_STATUS_OK, UDS_STATUS_ERR_NOT_INITIALIZED or
 *         UDS_STATUS_ERR_DID_NOT_FOUND.
 */
uds_status_t dtc_database_clear_one(uint32_t dtc_code);

/**
 * @brief Clear all DTCs: status to DTC_STATUS_INIT_AND_CLEARED (0x50), fault-
 *        detection counter to 0 [EDS#448, EDS#451].
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
 * Used by the application to track debouncing state (ISO 14229-1:2026 §11.3.1.14, Table 317).
 * 0x00–0xFE: counter value. 0xFF is reserved (confirmed) — excluded from
 * the 0x19/0x14 reportDTCFaultDetectionCounter response.
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
 * Permanent DTCs are returned by SID 0x19 sub-function 0x15 and are NOT
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

/* --------------------------------------------------------------------------
 * [EDS#145] First / most recent failed and confirmed DTC records
 * -------------------------------------------------------------------------- */

/**
 * @brief The four records ReadDTCInformation 0x0B-0x0E report.
 *
 * ISO 14229-1:2013 §11.3.1.12 (0x0B/0x0D) and §11.3.1.13 (0x0C/0x0E). A
 * record is set when dtc_database_set_status() raises a DTC's testFailed
 * (bit 0) or confirmedDTC (bit 3) from 0 to 1; "first" is set only while
 * empty, "most recent" every time. Records do not age: they keep naming the
 * DTC whatever its current status. A successful ClearDiagnosticInformation
 * of all groups empties all four; clearing one DTC empties the records that
 * name it. Persisted with the status mirror (config/dtc_mirror.c).
 */
typedef enum {
    DTC_RECORD_FIRST_TEST_FAILED       = 0,
    DTC_RECORD_FIRST_CONFIRMED         = 1,
    DTC_RECORD_MOST_RECENT_TEST_FAILED = 2,
    DTC_RECORD_MOST_RECENT_CONFIRMED   = 3
} dtc_record_t;

/** Number of dtc_record_t values. */
#define DTC_RECORD_COUNT  (4U)

/**
 * @brief Read one first/most recent record.
 * @param[in]  which         Record to read.
 * @param[out] out_dtc_code  Receives the DTC code, or 0 if the record is empty
 *                           (0 is never a registered DTC).
 * @return UDS_STATUS_OK, UDS_STATUS_ERR_NULL_PTR,
 *         UDS_STATUS_ERR_INVALID_PARAM (which out of range) or
 *         UDS_STATUS_ERR_NOT_INITIALIZED.
 */
uds_status_t dtc_database_get_record(dtc_record_t which, uint32_t *out_dtc_code);

/**
 * @brief Restore one record at boot (NVM mirror load). Not a status
 *        transition. For dtc_mirror_load() only.
 * @param[in] which     Record to restore.
 * @param[in] dtc_code  A registered DTC, or 0 for empty.
 * @return UDS_STATUS_OK, UDS_STATUS_ERR_INVALID_PARAM (which out of range),
 *         UDS_STATUS_ERR_DID_NOT_FOUND (DTC no longer registered; record left
 *         empty) or UDS_STATUS_ERR_NOT_INITIALIZED.
 */
uds_status_t dtc_database_restore_record(dtc_record_t which, uint32_t dtc_code);

/* --------------------------------------------------------------------------
 * [EDS#354] Optional lock callbacks
 * -------------------------------------------------------------------------- */

/**
 * @brief Lock/unlock callback signatures for dtc_database's critical sections.
 *
 * This module is a singleton with no caller-visible context object — unlike
 * uds_server_ctx_t (where the application already holds a pointer it can
 * hang its own external locking discipline on), there is nothing for an
 * integrator to lock around a call site with. Every dtc_database_* function
 * that reads or writes a dtc_entry_t field at runtime wraps its critical
 * section with these callbacks when registered.
 */
typedef void (*dtc_database_lock_cb_t)(void);
typedef void (*dtc_database_unlock_cb_t)(void);

/**
 * @brief Register lock/unlock callbacks for dtc_database's internal state.
 *
 * Optional. Not registering (the default) is correct and sufficient for
 * any build where every dtc_database_* call comes from one thread — true
 * of every example except one with a dedicated sensor/monitor thread that
 * calls dtc_database_set_status() independently of the UDS dispatch
 * thread (see EDS#354: config/dtc_database.c has no internal locking,
 * documented as "application-managed," and two threads touching the same
 * record with zero synchronization is a real race, not a style question).
 *
 * This module stays platform-agnostic (no RTOS header dependency) by
 * leaving the actual lock object and primitive entirely up to the
 * caller — register two small static wrapper functions around whatever
 * mutex/semaphore your platform and example already use.
 *
 * Call once during initialisation, before starting any thread that calls
 * a dtc_database_* function.
 *
 * SAFETY: lock_cb/unlock_cb must not block for longer than this example's
 *         own P2server_max budget allows — the UDS dispatch thread may be
 *         waiting on the same lock inside SID 0x14/0x19 handling.
 *
 * @param[in] lock_cb    Called before entering a critical section. NULL clears.
 * @param[in] unlock_cb  Called after leaving a critical section. NULL clears.
 */
void dtc_database_set_lock_callbacks(
    dtc_database_lock_cb_t   lock_cb,
    dtc_database_unlock_cb_t unlock_cb
);

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
