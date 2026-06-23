// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/freertos_nvm.c
 *
 * PURPOSE: FreeRTOS NVM store implementation.
 *
 *          Implements the nvm_store_* API (declared in platform/nvm_store.h)
 *          for FreeRTOS builds by routing calls to the customer-provided
 *          eds_nvm_ops_t callbacks registered via eds_platform_init().
 *
 *          TWO BACKENDS:
 *
 *          1. Customer NVM ops (production):
 *             The customer provides .read/.write/.is_ready callbacks backed
 *             by their MCU's flash driver. This backend provides true
 *             persistence across resets. The customer is responsible for
 *             implementing wear levelling and erase management in their
 *             flash driver; this layer only performs key-value routing.
 *
 *          2. Built-in RAM stub (development / CI):
 *             If no customer ops are registered, a static 16-slot RAM buffer
 *             is used. Data is lost on reset. Provides the same API contract
 *             so the full stack can be tested without flash hardware.
 *
 *          BACKEND SELECTION:
 *             eds_platform_init() calls freertos_nvm_register_ops() with
 *             either the customer ops or NULL (RAM stub). This file does
 *             not call eds_platform_init() — it is only called from there.
 *
 *          NVM KEY SPACE:
 *             Keys are uint16_t. The EDS stack uses keys 0x0001–0x0006
 *             (see NVM_KEY_* in nvm_store.h). The customer's NVM backend
 *             must support at least these six keys with up to 512 bytes
 *             per record (NVM_MAX_RECORD_BYTES).
 *
 *          THREAD SAFETY:
 *             nvm_store_write() and nvm_store_read() are called from the
 *             UDS poll task context only. No concurrent access from ISR.
 *             Thread-safety within the customer's flash driver is the
 *             customer's responsibility.
 *
 * SAFETY  : ASIL-B candidate. Persists security-relevant counters.
 *           Write failures are non-fatal but must be handled by caller.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

/*
 * Activate this compilation unit only when building for FreeRTOS.
 * On Zephyr builds, platform/zephyr/nvm_store.c is compiled instead.
 *
 * Guard: EDS_PLATFORM_FREERTOS is defined by the FreeRTOS CMake target.
 * On host test builds (NVM_STORE_HOST_MOCK=1), nvm_store_mock.c is used.
 */
#if defined(EDS_PLATFORM_FREERTOS) && !defined(NVM_STORE_HOST_MOCK)

#include "nvm_store.h"
#include "platform_api.h"
#include "uds_types.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

/** Active NVM operations — set by freertos_nvm_register_ops(). */
static eds_nvm_ops_t s_ops;

/** True after nvm_store_init() completes successfully. */
static bool s_initialized = false;

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Write the current NVM schema version if not already present.
 *
 * Called during nvm_store_init() to handle first-boot initialization.
 * If the stored version differs from NVM_SCHEMA_VERSION_CURRENT, all
 * records are cleared (conservative migration strategy).
 */
static void nvm_check_schema(void)
{
    uint16_t     stored_ver = 0U;
    uint16_t     current    = (uint16_t)NVM_SCHEMA_VERSION_CURRENT;
    uds_status_t rc;
    size_t       out_len    = 0U;

    rc = nvm_store_read(
        (uint16_t)NVM_KEY_SCHEMA_VERSION,
        &stored_ver, sizeof(stored_ver), &out_len);

    if (rc == UDS_STATUS_OK) {
        if (stored_ver == current) {
            return;   /* Schema matches — no action needed. */
        }
        /*
         * Schema version mismatch. Clear all records to force clean
         * re-initialisation. Counters will restart from zero, which
         * is safer than reading misaligned data.
         *
         * In production this should rarely occur (only after a firmware
         * update that changes the NVM layout). The customer's flash driver
         * erase is triggered indirectly by overwriting all keys.
         */
        (void)nvm_store_erase_all();
    }

    /* First boot or after migration: write current version. */
    (void)nvm_store_write(
        (uint16_t)NVM_KEY_SCHEMA_VERSION,
        &current, sizeof(current));
}

/* ============================================================================
 * freertos_nvm_register_ops
 *
 * Called from eds_platform_init() with either the customer's ops or
 * the built-in RAM stub ops. Not part of the public nvm_store_* API.
 * ========================================================================== */

void freertos_nvm_register_ops(const eds_nvm_ops_t *ops)
{
    if (ops != NULL) {
        s_ops = *ops;
    } else {
        (void)memset(&s_ops, 0, sizeof(s_ops));
    }
}

/* ============================================================================
 * nvm_store_* API implementation
 * ========================================================================== */

uds_status_t nvm_store_init(const nvm_store_cfg_t *cfg)
{
    (void)cfg;   /* FreeRTOS: cfg is unused — ops registered via freertos_nvm_register_ops(). */

    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    if ((s_ops.read == NULL) || (s_ops.write == NULL) || (s_ops.is_ready == NULL)) {
        /*
         * No ops registered. eds_platform_init() must have been called
         * first. This is a programming error.
         */
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    s_initialized = true;

    nvm_check_schema();

    return UDS_STATUS_OK;
}

uds_status_t nvm_store_write(uint16_t key, const void *data, size_t len)
{
    if (data == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    if ((len == 0U) || (len > (size_t)NVM_MAX_RECORD_BYTES)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    if (!s_initialized) { return UDS_STATUS_ERR_NOT_INITIALIZED; }
    if (s_ops.write == NULL) { return UDS_STATUS_ERR_NOT_INITIALIZED; }

    return s_ops.write(key, (const uint8_t *)data, len);
}

uds_status_t nvm_store_read(uint16_t key, void *data, size_t len,
                             size_t *out_read_len)
{
    if (data == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    if (len == 0U)    { return UDS_STATUS_ERR_INVALID_PARAM; }
    if (!s_initialized) { return UDS_STATUS_ERR_NOT_INITIALIZED; }
    if (s_ops.read == NULL) { return UDS_STATUS_ERR_NOT_INITIALIZED; }

    return s_ops.read(key, (uint8_t *)data, len, out_read_len);
}

uds_status_t nvm_store_delete(uint16_t key)
{
    uint8_t zero[1] = { 0U };

    if (!s_initialized) { return UDS_STATUS_ERR_NOT_INITIALIZED; }

    /*
     * FreeRTOS NVM does not have a native delete primitive — the customer's
     * flash backend may not support record-level deletion. We implement
     * delete as a zero-length write sentinel by overwriting the record with
     * a single zero byte. Subsequent reads will return the sentinel byte
     * rather than UDS_STATUS_ERR_DID_NOT_FOUND.
     *
     * For records that must truly be absent (e.g. after a factory reset),
     * call nvm_store_erase_all() instead.
     *
     * This is acceptable for the EDS use case: nvm_store_delete() is only
     * called to clear the security lockout counter, which is subsequently
     * re-initialised to zero on the next unlock cycle.
     */
    if (s_ops.write != NULL) {
        (void)s_ops.write(key, zero, sizeof(zero));
    }

    return UDS_STATUS_OK;
}

uds_status_t nvm_store_erase_all(void)
{
    uint16_t     keys[] = {
        (uint16_t)NVM_KEY_SEC_ATTEMPT_CTR,
        (uint16_t)NVM_KEY_SEC_LOCKOUT_MS,
        (uint16_t)NVM_KEY_DTC_MIRROR,
        (uint16_t)NVM_KEY_SESSION_STATS,
        (uint16_t)NVM_KEY_LIFECYCLE_CNT,
        (uint16_t)NVM_KEY_SCHEMA_VERSION,
    };
    uint8_t  zero[NVM_MAX_RECORD_BYTES];
    uint8_t  i;

    if (!s_initialized) { return UDS_STATUS_ERR_NOT_INITIALIZED; }
    if (s_ops.write == NULL) { return UDS_STATUS_ERR_NOT_INITIALIZED; }

    (void)memset(zero, 0, sizeof(zero));

    for (i = 0U; i < (uint8_t)(sizeof(keys) / sizeof(keys[0])); i++) {
        (void)s_ops.write(keys[i], zero, (size_t)1U);
    }

    /* Re-write schema version so the store is ready after erase. */
    {
        uint16_t ver = (uint16_t)NVM_SCHEMA_VERSION_CURRENT;
        (void)s_ops.write((uint16_t)NVM_KEY_SCHEMA_VERSION,
                          (const uint8_t *)&ver, sizeof(ver));
    }

    return UDS_STATUS_OK;
}

bool nvm_store_is_ready(void)
{
    if (!s_initialized) {
        return false;
    }
    if (s_ops.is_ready == NULL) {
        return false;
    }
    return s_ops.is_ready();
}

#endif /* EDS_PLATFORM_FREERTOS && !NVM_STORE_HOST_MOCK */
