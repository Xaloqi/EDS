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
 *            0x0001  NVM_KEY_SEC_STATE         Security attempt/lockout state (EDS#211)
 *            0x0003  NVM_KEY_DTC_MIRROR        DTC status-byte mirror block
 *            0x0004  NVM_KEY_SESSION_STATS     Session statistics block
 *            0x0005  NVM_KEY_LIFECYCLE_CNT     ECU lifecycle (reset) counter
 *            0x0006  NVM_KEY_SCHEMA_VERSION    NVM schema version (migration)
 *
 *          [EDS#211] 0x0002 (formerly NVM_KEY_SEC_LOCKOUT_MS) is
 *          deliberately retired, not reused: the failed-attempt counter
 *          and lockout residual used to be two independent keys/writes
 *          (NVM_KEY_SEC_ATTEMPT_CTR + NVM_KEY_SEC_LOCKOUT_MS), which left
 *          an unprotected window between them — a power loss after the
 *          first write and before the second could silently drop an
 *          engaged lockout on reboot. Both fields now live in one
 *          versioned, CRC-32-checked record under NVM_KEY_SEC_STATE,
 *          written with a single nvm_store_write() call, following the
 *          same single-atomic-record-with-integrity-check shape
 *          config/dtc_mirror.c already uses for NVM_KEY_DTC_MIRROR. See
 *          core/uds_security_nvm.h for the wire format.
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

/** Security attempt/lockout state — one versioned, CRC-32-checked record
 *  holding the failed-attempt counter and lockout timer residual together
 *  (EDS#211: replaces the former two-independent-key NVM_KEY_SEC_ATTEMPT_CTR
 *  + NVM_KEY_SEC_LOCKOUT_MS pair). Persisting this prevents lockout bypass
 *  by power-cycling the ECU. See core/uds_security_nvm.h for the wire
 *  format. */
#define NVM_KEY_SEC_STATE          ((uint16_t)0x0001U)

/** DTC status-byte mirror: array of (dtc_code[3] + status_byte[1]) * count,
 *  plus a 5-byte header and 4-byte CRC-32 trailer (see config/dtc_mirror.h).
 *  Maximum size: DTC_MIRROR_MAX_BYTES = 509 bytes, deliberately capped
 *  below NVM_MAX_RECORD_BYTES and enforced by a _Static_assert in
 *  config/dtc_mirror.c — NOT simply UDS_MAX_DTC_COUNT * 4 (issue #123:
 *  that naive formula, plus the header/CRC overhead, is 9 bytes over this
 *  record's cap). */
#define NVM_KEY_DTC_MIRROR         ((uint16_t)0x0003U)

/** Session statistics block (nvm_session_stats_t, ~16 bytes). */
#define NVM_KEY_SESSION_STATS      ((uint16_t)0x0004U)

/** ECU lifecycle (reset) counter (uint32_t, 4 bytes). */
#define NVM_KEY_LIFECYCLE_CNT      ((uint16_t)0x0005U)

/** NVM schema version — used for migration checks (uint16_t, 2 bytes). */
#define NVM_KEY_SCHEMA_VERSION     ((uint16_t)0x0006U)

/** Current NVM schema version. Increment when layout changes.
 *  [EDS#211] Bumped 0x0003 -> 0x0004: NVM_KEY_SEC_ATTEMPT_CTR/
 *  NVM_KEY_SEC_LOCKOUT_MS collapsed into the single NVM_KEY_SEC_STATE
 *  record. The existing schema-migration path (nvm_migrate_schema() in
 *  platform/zephyr/nvm_store.c) erases the whole store on a version
 *  mismatch — the same "counters restart from zero" behavior every prior
 *  schema bump has used, no per-field migration needed. A device
 *  upgrading across this bump loses any in-progress lockout exactly once,
 *  which is a strict improvement over the bug this schema change fixes
 *  (silently losing an engaged lockout to a mistimed power-cut). */
#define NVM_SCHEMA_VERSION_CURRENT ((uint16_t)0x0004U)

/** Maximum single record size in bytes. */
#define NVM_MAX_RECORD_BYTES       (512U)

/**
 * [#285] A backend without a native delete primitive (see the
 * nvm_store_delete() doc comment, and eds_nvm_ops_t's optional `remove`
 * callback — [#287], platform_api.h) may implement it as a sentinel
 * write instead of true removal: a record consisting of exactly
 * NVM_STORE_DELETE_SENTINEL_LEN byte(s), each equal to
 * NVM_STORE_DELETE_SENTINEL_BYTE.
 *
 * Such a backend's OWN nvm_store_read() should translate that pattern
 * back to UDS_STATUS_ERR_DID_NOT_FOUND itself, for the specific key(s) it
 * is used on, so "deleted reads as absent" holds the same way it would
 * with a true delete and callers do not need to know the backend faked
 * it — see platform/freertos/freertos_nvm.c's nvm_store_read() for the
 * one implementation that does this today, scoped to NVM_KEY_SEC_STATE
 * (the only key nvm_store_delete() is ever called with in this
 * codebase). Deliberately NOT applied to every key a backend might ever
 * store: a customer application may define further keys of its own
 * whose format this stack knows nothing about, one of which could
 * legitimately be a real record equal to this exact pattern — the
 * translation is safe only for a key whose fixed wire format is known
 * wider than NVM_STORE_DELETE_SENTINEL_LEN (NVM_KEY_SEC_STATE's is
 * UDS_SECURITY_NVM_RECORD_BYTES = 12 bytes — see
 * core/uds_security_nvm.h), which is exactly the set of keys this
 * translation should ever be applied to.
 *
 * KNOWN, ACCEPTED LIMITATION (applies only when the backend has no
 * native delete, i.e. eds_nvm_ops_t.remove is NULL) — a sentinel can
 * only ever be a heuristic, not a true delete: a genuinely corrupted
 * record (e.g. a torn flash write on the customer's own driver) that
 * happens to land on exactly this byte pattern is indistinguishable
 * from a deliberate delete and is ALSO reported as absent rather than
 * failing closed as corrupt. Every other corruption signature (any
 * other length, or a full-length record with a bad magic/version/CRC)
 * is unaffected and still fails closed. This is a real, accepted
 * trade-off, not an oversight: before this convention existed, EVERY
 * delete on such a backend was a guaranteed, 100%-reproducible false
 * failure; after it, only a narrow, driver-dependent corruption
 * coincidence could produce one. [#287] closes this gap entirely for a
 * backend that implements eds_nvm_ops_t.remove — the built-in FreeRTOS
 * RAM stub does; a real customer flash driver only does if the customer
 * implements it, which remains optional and non-breaking (existing
 * driver code with no `remove` field, or `remove = NULL`, keeps working
 * exactly as it did before this callback existed).
 */
#define NVM_STORE_DELETE_SENTINEL_LEN  ((size_t)1U)
#define NVM_STORE_DELETE_SENTINEL_BYTE ((uint8_t)0x00U)

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

    /** NVS sector size in bytes (typically 4096 for most flash).
     *  [EDS#200] uint32_t, not uint16_t: some real targets' physical erase
     *  pages exceed 65535 bytes (e.g. STM32H743ZI and NXP MCX N947 both
     *  use 128 KB = 0x20000 sectors — see boards/nucleo_h743zi and
     *  boards/frdm_mcxn947's board overlay files). A uint16_t here
     *  silently truncated 0x20000 to 0, which nvs_mount() would reject. */
    uint32_t    sector_size;

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
 * [#285] On a backend without a native delete primitive, this may instead
 * be a sentinel write — see NVM_STORE_DELETE_SENTINEL_LEN/_BYTE above. A
 * well-behaved such backend translates that pattern back to
 * UDS_STATUS_ERR_DID_NOT_FOUND inside its own nvm_store_read(), for the
 * key(s) it is used on, so callers see the same "deleted reads as
 * absent" contract regardless of backend — see
 * platform/freertos/freertos_nvm.c for the one implementation that does
 * this today.
 *
 * @param[in] key  Record key to delete.
 *
 * @return UDS_STATUS_OK if deleted or not found (idempotent).
 * @return UDS_STATUS_ERR_PLATFORM if flash operation failed.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if nvm_store_init() not called.
 */
uds_status_t nvm_store_delete(uint16_t key);

/**
 * @brief Erase all NVM records, EXCEPT NVM_KEY_SEC_STATE.
 *
 * Factory-reset the NVM partition — clears all diagnostics-related
 * persistent data. Must not be called during normal operation.
 *
 * [#280] NVM_KEY_SEC_STATE (the SecurityAccess attempt-counter/lockout
 * record, EDS#211) is deliberately EXCLUDED and survives this call. This
 * primitive has no authorization concept of its own — it is a plain C
 * function, not a UDS service with a SecurityAccess level attached — so
 * any future diagnostic-reachable reset routine that calls it must not be
 * able to clear the lockout state as an unreviewed side effect. Of this
 * function's own callers, only the schema-migration path is privileged
 * enough to still wipe it, and does so
 * explicitly rather than relying on this function's blast radius — on
 * Zephyr, nvm_migrate_schema() (platform/zephyr/nvm_store.c) calls
 * nvs_clear() directly and never calls this function at all; on FreeRTOS,
 * nvm_check_schema() (platform/freertos/freertos_nvm.c) calls this
 * function for its general wipe and then separately re-asserts the
 * NVM_KEY_SEC_STATE wipe itself. A caller that genuinely needs to reset
 * NVM_KEY_SEC_STATE alongside a call to this one must do so explicitly,
 * with nvm_store_delete(NVM_KEY_SEC_STATE) or nvm_store_write(), under its
 * own authorization check — never implicitly via this primitive's blast
 * radius.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM if flash erase failed.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if nvm_store_init() not called.
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

/**
 * @brief Flip one bit of one byte of the mock's raw backing store.
 *
 * FOR HOST/TEST USE ONLY (NVM_STORE_HOST_MOCK builds). Not linked in
 * production firmware. Lets a test corrupt a specific byte of whatever the
 * backend under test has already written, to exercise its CRC/integrity
 * checking on the next read or re-init — see
 * platform/zephyr/nvm_store_append.c's implementation (issue #304).
 * platform/zephyr/nvm_store_mock.c does not implement this; only link it
 * into a test binary that doesn't also link that file.
 * [MISRA 8.7] Prototype provided here so all callers have a visible
 * declaration; guarded so the symbol is unreachable in production.
 *
 * @param[in] byte_offset  Offset into the mock's raw backing buffer.
 */
void nvm_mock_corrupt_byte(uint32_t byte_offset);
#endif /* NVM_STORE_HOST_MOCK */

#ifdef __cplusplus
}
#endif

#endif /* NVM_STORE_H */
