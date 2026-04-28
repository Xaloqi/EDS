/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: config/routine_database.h
 *
 * PURPOSE: Public API for the Routine Identifier (RID) database.
 *          Stores descriptor records for all supported routines including
 *          access control metadata and start/stop/result callbacks.
 *
 *          The database is a fixed-capacity static array. Entries are
 *          populated at runtime via routine_database_register(), called from
 *          the generated routine_handlers_register_all() during stack init.
 *          This decouples the storage engine (this file) from the routine
 *          content (generated/routine_handlers.c), enabling YAML-driven code
 *          generation without modifying this file.
 *
 * ISO 14229-1 §13: RoutineControl (SID 0x31)
 *   Sub-functions:
 *     0x01 startRoutine        — Start execution of a routine.
 *     0x02 stopRoutine         — Stop an in-progress routine.
 *     0x03 requestRoutineResults — Read the results of a completed routine.
 *
 * CAPACITY: UDS_MAX_ROUTINE_COUNT entries (defined in uds_types.h, default 32).
 *           Bounded at compile time — no dynamic memory.
 *
 * SAFETY  : Access to routine callbacks must be gated by session and security
 *           level checks (service_0x31.c). Routines can modify ECU state;
 *           this is a safety-relevant module.
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#ifndef ROUTINE_DATABASE_H
#define ROUTINE_DATABASE_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

/**
 * @brief Maximum data length for routine option records and result records.
 *
 * Applies to both the routineOptionRecord (in startRoutine request) and the
 * routineStatusRecord (in requestRoutineResults response).
 *
 * SAFETY: Buffer sizes for option and result data are bounded by this value.
 *         Increasing it increases static RAM consumption for the routine callbacks.
 */
#ifndef ROUTINE_MAX_OPTION_LEN
#define ROUTINE_MAX_OPTION_LEN  (64U)
#endif

#ifndef ROUTINE_MAX_RESULT_LEN
#define ROUTINE_MAX_RESULT_LEN  (64U)
#endif

/* --------------------------------------------------------------------------
 * Routine access control flags
 * -------------------------------------------------------------------------- */

/** Routine supports 0x31/0x01 startRoutine. */
#define ROUTINE_SUPPORT_START   (0x01U)

/** Routine supports 0x31/0x02 stopRoutine (optional per ISO 14229-1). */
#define ROUTINE_SUPPORT_STOP    (0x02U)

/** Routine supports 0x31/0x03 requestRoutineResults (optional per ISO 14229-1). */
#define ROUTINE_SUPPORT_RESULTS (0x04U)

/* --------------------------------------------------------------------------
 * Routine callback prototypes
 * -------------------------------------------------------------------------- */

/**
 * @brief Routine start callback — initiates routine execution.
 *
 * Called when SID 0x31 sub-fn 0x01 (startRoutine) is received.
 *
 * SAFETY: Must complete within P2server_max. No blocking calls.
 *         Long-running routines should start async work and return immediately.
 *
 * @param[in]  opt_buf      Option record from the request (may be NULL if none).
 * @param[in]  opt_len      Number of bytes in opt_buf (0 if none).
 * @param[out] result_buf   Buffer to write the routineStatusRecord into.
 * @param[in]  result_buf_len  Size of result_buf.
 * @param[out] result_len   Number of bytes written to result_buf.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_ROUTINE_FAILED on execution failure.
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET if pre-conditions not satisfied.
 */
typedef uds_status_t (*routine_start_cb_fn)(
    const uint8_t *opt_buf,
    uint8_t        opt_len,
    uint8_t       *result_buf,
    uint8_t        result_buf_len,
    uint8_t       *result_len
);

/**
 * @brief Routine stop callback — halts an in-progress routine.
 *
 * Called when SID 0x31 sub-fn 0x02 (stopRoutine) is received.
 * Only invoked if ROUTINE_SUPPORT_STOP is set in support_flags.
 *
 * @param[in]  opt_buf      Option record from the stop request (may be NULL).
 * @param[in]  opt_len      Number of bytes in opt_buf.
 * @param[out] result_buf   Buffer for status record.
 * @param[in]  result_buf_len Size of result_buf.
 * @param[out] result_len   Bytes written.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_ROUTINE_FAILED if routine cannot be stopped.
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET if not currently running.
 */
typedef uds_status_t (*routine_stop_cb_fn)(
    const uint8_t *opt_buf,
    uint8_t        opt_len,
    uint8_t       *result_buf,
    uint8_t        result_buf_len,
    uint8_t       *result_len
);

/**
 * @brief Routine results callback — returns results of a completed routine.
 *
 * Called when SID 0x31 sub-fn 0x03 (requestRoutineResults) is received.
 * Only invoked if ROUTINE_SUPPORT_RESULTS is set in support_flags.
 *
 * @param[out] result_buf       Buffer for routineStatusRecord.
 * @param[in]  result_buf_len   Size of result_buf.
 * @param[out] result_len       Bytes written.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET if no results available yet.
 * @return UDS_STATUS_ERR_ROUTINE_FAILED if results indicate failure.
 */
typedef uds_status_t (*routine_results_cb_fn)(
    uint8_t  *result_buf,
    uint8_t   result_buf_len,
    uint8_t  *result_len
);

/* --------------------------------------------------------------------------
 * Routine descriptor record
 * -------------------------------------------------------------------------- */

/**
 * @brief Descriptor for a single Routine Identifier entry.
 *
 * Registered with routine_database_register() during stack initialisation.
 * All pointer fields must remain valid for the application lifetime.
 *
 * SAFETY: min_session and security_level govern who may execute this routine.
 *         An incorrect security_level may allow privileged operations to be
 *         triggered without authentication.
 */
typedef struct routine_entry {
    uint16_t                rid;            /**< 16-bit Routine Identifier. */
    uint8_t                 support_flags;  /**< ROUTINE_SUPPORT_START | STOP | RESULTS. */
    uint8_t                 min_session;    /**< Minimum session (uds_session_type_t cast). */
    uint8_t                 security_level; /**< Minimum security level (0 = no lock). */
    routine_start_cb_fn     start_cb;       /**< Start callback; must not be NULL. */
    routine_stop_cb_fn      stop_cb;        /**< Stop callback; NULL if stop not supported. */
    routine_results_cb_fn   results_cb;     /**< Results callback; NULL if not supported. */
    const char             *description;    /**< Human-readable label (tooling/logging). */
} routine_entry_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the routine database.
 *
 * Clears the internal registration table. Must be called exactly once
 * before routine_database_register() or routine_database_find().
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 */
uds_status_t routine_database_init(void);

/**
 * @brief Register one routine entry with the database.
 *
 * Copies the descriptor into the internal static table.
 *
 * @param[in] entry  Fully-populated routine descriptor. Must not be NULL.
 *                   start_cb must not be NULL.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if entry is NULL or start_cb is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if routine_database_init() not called.
 * @return UDS_STATUS_ERR_INVALID_PARAM if rid already registered.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if UDS_MAX_ROUTINE_COUNT exceeded.
 */
uds_status_t routine_database_register(const routine_entry_t *entry);

/**
 * @brief Look up a routine entry by its 16-bit identifier.
 *
 * @param[in] rid  16-bit Routine Identifier to search for.
 *
 * @return Pointer to the internal routine_entry_t on success.
 * @return NULL if not found or database not initialized.
 */
const routine_entry_t *routine_database_find(uint16_t rid);

/**
 * @brief Return the number of registered routine entries.
 *
 * @param[out] out_count  Receives the current count.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if out_count is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 */
uds_status_t routine_database_get_count(uint16_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* ROUTINE_DATABASE_H */
