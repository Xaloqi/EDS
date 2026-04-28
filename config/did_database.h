// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: config/did_database.h
 *
 * PURPOSE: Public API for the Data Identifier (DID) database.
 *          Stores descriptor records for all supported DIDs including
 *          access control metadata and read/write callbacks.
 *
 *          The database is a fixed-capacity static array. Entries are
 *          populated at runtime via did_database_register(), called from
 *          the generated did_handlers_register_all() during stack init.
 *          This decouples the storage engine (this file) from the DID
 *          content (generated/did_handlers.c), enabling YAML-driven code
 *          generation without modifying this file.
 *
 * CAPACITY: UDS_MAX_DID_COUNT entries (defined in uds_types.h, default 64).
 *           Bounded at compile time — no dynamic memory.
 *
 * SAFETY  : Access to DID callbacks must be gated by session and security
 *           level checks (uds_safety.c). This module provides lookup only.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef DID_DATABASE_H
#define DID_DATABASE_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

/** Maximum data length for a single DID record in bytes. */
#ifndef DID_MAX_DATA_LEN
#define DID_MAX_DATA_LEN (64U)
#endif

/* --------------------------------------------------------------------------
 * DID access control flags
 * -------------------------------------------------------------------------- */

/** DID supports ReadDataByIdentifier (SID 0x22). */
#define DID_ACCESS_READ   (0x01U)

/** DID supports WriteDataByIdentifier (SID 0x2E). */
#define DID_ACCESS_WRITE  (0x02U)

/* --------------------------------------------------------------------------
 * DID read/write callback prototypes
 * -------------------------------------------------------------------------- */

/**
 * @brief DID read callback — populates buf with current DID value.
 *
 * SAFETY: Callback must complete within P2server_max. No blocking calls.
 *
 * @param[out] buf      Buffer to receive DID data.
 * @param[in]  buf_len  Size of buf in bytes (guaranteed >= data_length by caller).
 * @param[out] out_len  Number of bytes written (must equal data_length).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_DID_READ_FAILED on hardware/NVM read failure.
 */
typedef uds_status_t (*did_read_cb_fn)(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len
);

/**
 * @brief DID write callback — applies new value from buf.
 *
 * SAFETY: Callback must validate range before applying. No blocking calls.
 *
 * @param[in] buf  Buffer containing new DID value (exactly data_length bytes).
 * @param[in] len  Number of bytes in buf (always equal to data_length).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_DID_WRITE_FAILED on hardware/NVM write failure.
 */
typedef uds_status_t (*did_write_cb_fn)(
    const uint8_t *buf,
    uint16_t       len
);

/* --------------------------------------------------------------------------
 * DID descriptor record
 * -------------------------------------------------------------------------- */

/**
 * @brief Descriptor for a single Data Identifier entry.
 *
 * Registered with did_database_register() during stack initialisation.
 * All pointer fields must remain valid for the application lifetime.
 *
 * SAFETY: data_length governs all buffer sizing in the service and safety
 *         layers. Incorrect values directly cause buffer overflows or
 *         under-reads. Treat as safety-critical configuration data.
 */
typedef struct did_entry {
    uint16_t        did_id;             /**< 16-bit DID identifier (e.g. 0xF190). */
    uint8_t         access_flags;       /**< Bitfield: DID_ACCESS_READ | DID_ACCESS_WRITE. */
    uint8_t         min_session;        /**< Minimum session required (uds_session_type_t cast). */
    uint8_t         read_access_level;  /**< Minimum security level for read (0 = no lock). */
    uint8_t         write_access_level; /**< Minimum security level for write. */
    uint16_t        data_length;        /**< Data payload length in bytes (1–DID_MAX_DATA_LEN). */
    did_read_cb_fn  read_cb;            /**< Read callback; NULL if DID is write-only. */
    did_write_cb_fn write_cb;           /**< Write callback; NULL if DID is read-only. */
    const char     *description;        /**< Human-readable label (tooling/logging only). */
} did_entry_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the DID database.
 *
 * Clears the internal registration table. Must be called exactly once
 * before did_database_register() or did_database_find().
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 */
uds_status_t did_database_init(void);

/**
 * @brief Register one DID entry with the database.
 *
 * Copies the descriptor into the internal static table. The caller's
 * entry pointer need not remain valid after this call returns.
 *
 * Called exclusively from did_handlers_register_all() (generated/did_handlers.c)
 * during stack initialisation. Must not be called after the poll loop starts.
 *
 * @param[in] entry  Fully-populated DID descriptor. Must not be NULL.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if entry is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if did_database_init() not yet called.
 * @return UDS_STATUS_ERR_INVALID_PARAM if data_length is 0, exceeds
 *         DID_MAX_DATA_LEN, or did_id already exists in the database.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if UDS_MAX_DID_COUNT already registered.
 */
uds_status_t did_database_register(const did_entry_t *entry);

/**
 * @brief Look up a DID entry by its 16-bit identifier.
 *
 * Linear search — O(n) where n = number of registered DIDs.
 * Acceptable for UDS use: DID count is bounded by UDS_MAX_DID_COUNT (64).
 *
 * @param[in] did_id  16-bit DID identifier to search for.
 *
 * @return Pointer to the internal did_entry_t on success.
 * @return NULL if not found or if the database is not initialized.
 *
 * @note SAFETY: Returned pointer is const — callers must not modify the entry.
 *               The pointed-to object is valid for the application lifetime.
 */
const did_entry_t *did_database_find(uint16_t did_id);

/**
 * @brief Return the number of registered DID entries.
 *
 * @param[out] out_count  Receives the current entry count.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if out_count is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if database not initialized.
 */
uds_status_t did_database_get_count(uint16_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* DID_DATABASE_H */

// ✅ End of did_database.h
