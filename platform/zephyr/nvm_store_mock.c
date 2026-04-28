// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/nvm_store_mock.c
 *
 * PURPOSE: RAM-backed NVM store mock for host-side unit tests.
 *
 *          Compiled when NVM_STORE_HOST_MOCK is defined. Provides the full
 *          nvm_store_* API with a 16-slot in-memory key-value table.
 *          Thread-safety is intentionally omitted — tests are single-threaded.
 *
 * DESIGN:
 *   - 16 record slots keyed by uint16_t NVM_KEY_*.
 *   - Each slot holds up to NVM_MAX_RECORD_BYTES bytes.
 *   - nvm_store_erase_all() clears all slots.
 *   - Simulates power-cycle by allowing controlled resets via nvm_mock_reset().
 *
 * SAFETY  : Test-only. NOT for production firmware.
 * =============================================================================
 */

#define NVM_STORE_HOST_MOCK 1
#include "nvm_store.h"
#include "uds_types.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Mock storage
 * -------------------------------------------------------------------------- */

#define MOCK_MAX_RECORDS (16U)

typedef struct mock_record {
    uint16_t key;
    uint8_t  data[NVM_MAX_RECORD_BYTES];
    size_t   len;
    bool     used;
} mock_record_t;

static mock_record_t s_records[MOCK_MAX_RECORDS];
static bool          s_initialized = false;

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static mock_record_t *mock_find(uint16_t key)
{
    uint8_t i;
    for (i = 0U; i < (uint8_t)MOCK_MAX_RECORDS; i++) {
        if (s_records[i].used && (s_records[i].key == key)) {
            return &s_records[i];
        }
    }
    return NULL;
}

static mock_record_t *mock_alloc(uint16_t key)
{
    uint8_t i;
    for (i = 0U; i < (uint8_t)MOCK_MAX_RECORDS; i++) {
        if (!s_records[i].used) {
            s_records[i].key  = key;
            s_records[i].used = true;
            s_records[i].len  = 0U;
            return &s_records[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Public API — mock implementations
 * -------------------------------------------------------------------------- */

uds_status_t nvm_store_init(const nvm_store_cfg_t *cfg)
{
    (void)cfg; /* ignored in mock */

    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    /*
     * NOTE: Records are intentionally NOT cleared here.
     * nvm_store_init() simulates "remounting" the flash driver — flash
     * retains its data across power-cycles. Only nvm_mock_reset() simulates
     * a factory erase (or first-ever boot with blank flash).
     *
     * On true first boot, s_records was already zeroed at program start.
     * After nvm_mock_deinit() + nvm_store_init(), records from the previous
     * "session" are still present — exactly as real NVS behaves.
     */
    s_initialized = true;

    /*
     * Write schema version only if not already present (first boot behavior).
     * On "remount" (deinit + reinit), the stored version must not be overwritten
     * since that would simulate a schema migration on every power-cycle.
     */
    uint16_t existing_ver = 0U;
    uds_status_t rc = nvm_store_read(
        (uint16_t)NVM_KEY_SCHEMA_VERSION,
        &existing_ver, sizeof(existing_ver), NULL);

    if (rc == UDS_STATUS_ERR_DID_NOT_FOUND) {
        /* First boot: write current schema version. */
        uint16_t ver = (uint16_t)NVM_SCHEMA_VERSION_CURRENT;
        (void)nvm_store_write((uint16_t)NVM_KEY_SCHEMA_VERSION, &ver, sizeof(ver));
    }
    /* If already present (reinit after deinit): leave existing version as-is. */

    return UDS_STATUS_OK;
}

uds_status_t nvm_store_write(uint16_t key, const void *data, size_t len)
{
    mock_record_t *rec;

    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if ((len == (size_t)0U) || (len > (size_t)NVM_MAX_RECORD_BYTES)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    rec = mock_find(key);
    if (rec == NULL) {
        rec = mock_alloc(key);
    }

    if (rec == NULL) {
        return UDS_STATUS_ERR_PLATFORM; /* Mock store full — increase MOCK_MAX_RECORDS */
    }

    (void)memcpy(rec->data, data, len);
    rec->len = len;

    return UDS_STATUS_OK;
}

uds_status_t nvm_store_read(
    uint16_t  key,
    void     *data,
    size_t    len,
    size_t   *out_read_len)
{
    mock_record_t *rec;
    size_t         copy_len;

    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len == (size_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    rec = mock_find(key);
    if (rec == NULL) {
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }

    copy_len = (rec->len < len) ? rec->len : len;
    (void)memcpy(data, rec->data, copy_len);

    if (out_read_len != NULL) {
        *out_read_len = copy_len;
    }

    return UDS_STATUS_OK;
}

uds_status_t nvm_store_delete(uint16_t key)
{
    mock_record_t *rec;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    rec = mock_find(key);
    if (rec != NULL) {
        (void)memset(rec, 0, sizeof(mock_record_t));
    }

    return UDS_STATUS_OK; /* Idempotent — not-found is OK */
}

uds_status_t nvm_store_erase_all(void)
{
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    (void)memset(s_records, 0, sizeof(s_records));

    /* Re-write schema version to maintain initialized state. */
    uint16_t ver = (uint16_t)NVM_SCHEMA_VERSION_CURRENT;
    (void)nvm_store_write((uint16_t)NVM_KEY_SCHEMA_VERSION, &ver, sizeof(ver));

    return UDS_STATUS_OK;
}

bool nvm_store_is_ready(void)
{
    return s_initialized;
}

/* --------------------------------------------------------------------------
 * Test helper API (not declared in nvm_store.h — test-internal only)
 * -------------------------------------------------------------------------- */

#ifdef NVM_STORE_HOST_MOCK
/**
 * @brief Reset the mock NVM entirely — simulates a power cycle.
 *
 * After this call, nvm_store_is_ready() returns false and all records
 * are erased. The next nvm_store_init() call restores the ready state
 * but reads back whatever was written before the reset (if the test
 * wants to simulate "data persisted across reset", it should NOT call
 * this function — instead re-initialize with the same store state).
 *
 * For a "hard reset" that also clears all data:
 *   nvm_mock_reset();
 * For a "soft reset" that preserves data but needs re-init:
 *   nvm_mock_deinit();    // just clears initialized flag
 */
void nvm_mock_reset(void)
{
    (void)memset(s_records, 0, sizeof(s_records));
    s_initialized = false;
}

/**
 * @brief Deinitialize the mock without clearing data (simulate re-init cycle).
 *
 * Sets s_initialized = false so nvm_store_init() can be called again.
 * Existing records are preserved, simulating a power-cycle where flash
 * retains data but the driver must be re-mounted.
 */
void nvm_mock_deinit(void)
{
    s_initialized = false;
}
#endif /* NVM_STORE_HOST_MOCK */

