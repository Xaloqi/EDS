// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/nvm_store.h
 *
 * PURPOSE: Non-Volatile Memory (NVM) storage abstraction layer.
 *
 *          Provides a portable key-value store backed by Zephyr NVS
 *          (Non-Volatile Storage) on production targets, and by a
 *          RAM-backed mock for host-side unit tests.
 *
 *          Each stored record is identified by a uint16_t key (NVS ID).
 *          Key layout:
 *
 *            0x0001  NVM_KEY_SEC_ATTEMPT_CTR   Security failed-attempt counter
 *            0x0002  NVM_KEY_SEC_LOCKOUT_MS    Security lockout timer residual
 *            0x0003  NVM_KEY_DTC_MIRROR        DTC status-byte mirror block
 *            0x0004  NVM_KEY_SESSION_STATS     Session statistics block
 *            0x0005  NVM_KEY_LIFECYCLE_CNT     ECU lifecycle (reset) counter
 *            0x0006  NVM_KEY_SCHEMA_VERSION    NVM schema version (migration)
 *
 * DESIGN CONSTRAINTS:
 *   - No dynamic memory. All buffers are caller-allocated.
 *   - Calls may be made from task context only (not ISR).
 *   - Write granularity: one record at a time.
 *   - Max record size: NVM_MAX_RECORD_BYTES (512 bytes).
 *   - NVS sector layout: see nvm_store.c for flash partition configuration.
 *
 * SAFETY  : ASIL-B candidate. Persists security-relevant counters.
 *           Write failures are non-fatal but must be logged.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef NVM_STORE_H
#define NVM_STORE_H

#include "uds_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * NVM record key identifiers
 * -------------------------------------------------------------------------- */

/** Security failed-attempt counter (uint8_t, 1 byte). */
#define NVM_KEY_SEC_ATTEMPT_CTR    ((uint16_t)0x0001U)

/** Security lockout timer residual in ms (uint32_t, 4 bytes).
 *  Persisting this prevents lockout bypass by power-cycling the ECU. */
#define NVM_KEY_SEC_LOCKOUT_MS     ((uint16_t)0x0002U)

/** DTC status-byte mirror: array of (dtc_code[3] + status_byte[1]) * count.
 *  Maximum size: UDS_MAX_DTC_COUNT * 4 bytes = 512 bytes. */
#define NVM_KEY_DTC_MIRROR         ((uint16_t)0x0003U)

/** Session statistics block (nvm_session_stats_t, ~16 bytes). */
#define NVM_KEY_SESSION_STATS      ((uint16_t)0x0004U)

/** ECU lifecycle (reset) counter (uint32_t, 4 bytes). */
#define NVM_KEY_LIFECYCLE_CNT      ((uint16_t)0x0005U)

/** NVM schema version — used for migration checks (uint16_t, 2 bytes). */
#define NVM_KEY_SCHEMA_VERSION     ((uint16_t)0x0006U)

/** Current NVM schema version. Increment when layout changes. */
#define NVM_SCHEMA_VERSION_CURRENT ((uint16_t)0x0003U)

/** Maximum single record size in bytes. */
#define NVM_MAX_RECORD_BYTES       (512U)

/* --------------------------------------------------------------------------
 * Session statistics record (persisted as NVM_KEY_SESSION_STATS)
 * -------------------------------------------------------------------------- */

/**
 * @brief Persistent session statistics block.
 *
 * Survives ECU resets. Useful for field diagnostics and security audit.
 */
typedef struct nvm_session_stats {
    uint32_t total_resets;                /**< Total ECU reset count (all types). */
    uint32_t programming_session_count;   /**< Times PROGRAMMING session entered. */
    uint32_t extended_session_count;      /**< Times EXTENDED session entered. */
    uint32_t security_unlock_count;       /**< Times security was successfully unlocked. */
    uint32_t security_lockout_count;      /**< Times security lockout was triggered. */
} nvm_session_stats_t;

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

/**
 * @brief NVM store configuration block.
 *
 * On Zephyr: flash_dev and sector parameters must match the DTS flash
 * partition configured for diagnostics NVS.
 * On host tests: all fields are ignored — RAM backend is used.
 */
typedef struct nvm_store_cfg {
    /** Flash device (Zephyr: from DEVICE_DT_GET for the NVS partition).
     *  Cast to void* to avoid including zephyr/device.h in this header. */
    const void *flash_dev;

    /** Flash storage offset (NVS sector start, from DTS). */
    uint32_t    flash_offset;

    /** NVS sector size in bytes (typically 4096 for most flash). */
    uint16_t    sector_size;

    /** Number of NVS sectors to use for diagnostics NVM (minimum 2). */
    uint8_t     sector_count;
} nvm_store_cfg_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the NVM store.
 *
 * On Zephyr: mounts the NVS filesystem at the configured flash partition.
 * On host tests: clears the RAM-backed mock store.
 *
 * Also performs schema version check and migration if the stored version
 * does not match NVM_SCHEMA_VERSION_CURRENT.
 *
 * @param[in] cfg  NVM configuration (may be NULL on host — uses defaults).
 *
 * @return UDS_STATUS_OK on success (including first-time format).
 * @return UDS_STATUS_ERR_PLATFORM if NVS mount failed.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 *
 * @note Must be called before any other nvm_store_* function.
 * @note SAFETY: Must complete before diagnostics stack initializes.
 */
uds_status_t nvm_store_init(const nvm_store_cfg_t *cfg);

/**
 * @brief Write a record to NVM.
 *
 * Writes len bytes from data to the record identified by key.
 * Overwrites any existing record with the same key.
 *
 * @param[in] key   Record key (NVM_KEY_* constants).
 * @param[in] data  Pointer to data to write.
 * @param[in] len   Number of bytes to write (max NVM_MAX_RECORD_BYTES).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if data is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len is 0 or > NVM_MAX_RECORD_BYTES.
 * @return UDS_STATUS_ERR_PLATFORM if flash write failed.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if nvm_store_init() not called.
 *
 * @note SAFETY: Best-effort — caller must handle write failure gracefully.
 *               Security-critical data (attempt counter) should be written
 *               eagerly; data loss on power-cut is an accepted risk for
 *               non-safety records.
 */
uds_status_t nvm_store_write(uint16_t key, const void *data, size_t len);

/**
 * @brief Read a record from NVM.
 *
 * Reads up to len bytes into data from the record identified by key.
 * If the stored record is shorter than len, only stored bytes are copied
 * and out_read_len reflects the actual count.
 *
 * @param[in]  key          Record key (NVM_KEY_* constants).
 * @param[out] data         Buffer to receive the stored data.
 * @param[in]  len          Size of buffer in bytes.
 * @param[out] out_read_len Actual bytes read (may be NULL if not needed).
 *
 * @return UDS_STATUS_OK if record found and read.
 * @return UDS_STATUS_ERR_NULL_PTR if data is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len is 0.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND if no record exists for key.
 * @return UDS_STATUS_ERR_PLATFORM if flash read failed.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if nvm_store_init() not called.
 */
uds_status_t nvm_store_read(
    uint16_t  key,
    void     *data,
    size_t    len,
    size_t   *out_read_len);

/**
 * @brief Delete a record from NVM.
 *
 * Marks the record as deleted. Reclaimed on next NVS garbage collection.
 *
 * @param[in] key  Record key to delete.
 *
 * @return UDS_STATUS_OK if deleted or not found (idempotent).
 * @return UDS_STATUS_ERR_PLATFORM if flash operation failed.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if nvm_store_init() not called.
 */
uds_status_t nvm_store_delete(uint16_t key);

/**
 * @brief Erase all NVM records.
 *
 * Factory-reset the NVM partition — clears all diagnostics-related
 * persistent data. Must not be called during normal operation.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM if flash erase failed.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if nvm_store_init() not called.
 *
 * @note SAFETY: Clears security attempt counters — invoke only after
 *               validated authorization (e.g. factory NRC sequence).
 */
uds_status_t nvm_store_erase_all(void);

/**
 * @brief Check whether the NVM store is initialized and operational.
 *
 * @return true if nvm_store_init() has completed successfully.
 * @return false otherwise.
 */
bool nvm_store_is_ready(void);

#ifdef NVM_STORE_HOST_MOCK
/**
 * @brief Simulate a power-cycle by erasing all in-memory NVM data.
 *
 * FOR HOST/TEST USE ONLY (NVM_STORE_HOST_MOCK builds). Not linked in
 * production firmware.
 * [MISRA 8.7] Prototype provided here so all callers have a visible
 * declaration; guarded so the symbol is unreachable in production.
 */
void nvm_mock_reset(void);

/**
 * @brief Clear the mock's initialized flag without erasing data.
 *
 * FOR HOST/TEST USE ONLY (NVM_STORE_HOST_MOCK builds). Not linked in
 * production firmware.
 * [MISRA 8.7] Prototype provided here so all callers have a visible
 * declaration; guarded so the symbol is unreachable in production.
 */
void nvm_mock_deinit(void);
#endif /* NVM_STORE_HOST_MOCK */

#ifdef __cplusplus
}
#endif

#endif /* NVM_STORE_H */
