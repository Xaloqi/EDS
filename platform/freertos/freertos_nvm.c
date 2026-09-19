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

/** The delete-sentinel byte pattern (NVM_STORE_DELETE_SENTINEL_LEN/_BYTE,
 *  nvm_store.h), as a single shared buffer rather than a duplicate
 *  local + memset() at each of this file's two write sites
 *  (nvm_store_delete(), nvm_store_erase_all()). */
static const uint8_t s_delete_sentinel[NVM_STORE_DELETE_SENTINEL_LEN] = {
    NVM_STORE_DELETE_SENTINEL_BYTE
};

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
         *
         * [#280] nvm_store_erase_all() deliberately preserves
         * NVM_KEY_SEC_STATE — this call site is a schema MIGRATION, the
         * one privileged case that must still wipe it (an old-format
         * record under a new schema won't parse under the new one either).
         * Zephyr's equivalent migration (nvm_migrate_schema() in
         * platform/zephyr/nvm_store.c) gets this for free because it calls
         * nvs_clear() directly rather than this function; this backend
         * routes through nvm_store_erase_all() instead, so it must
         * explicitly re-assert the wipe here — restoring exactly the
         * pre-#280 behaviour for this call site (nvm_store_delete() is
         * the same single-zero-byte write the old erase_all() loop used
         * to perform on every key here, SEC_STATE included).
         *
         * NOTE: on this backend nvm_store_delete() is a sentinel write,
         * not a true delete (see its own doc comment and
         * NVM_STORE_DELETE_SENTINEL_LEN/_BYTE in nvm_store.h) — but
         * nvm_store_read() above now translates that sentinel back to
         * DID_NOT_FOUND itself (fixed by #285), so the subsequent
         * uds_security_nvm_load() sees a clean zero-state exactly as it
         * would after a true delete. Before #285, that read back as
         * CORRUPT and, with nvm_load_fail_closed configured true, as a
         * lockout rather than a clean zero-state.
         */
        (void)nvm_store_erase_all();
        (void)nvm_store_delete((uint16_t)NVM_KEY_SEC_STATE);
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
    uds_status_t rc;
    size_t       read_len = 0U;

    if (data == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    if (len == 0U)    { return UDS_STATUS_ERR_INVALID_PARAM; }
    if (!s_initialized) { return UDS_STATUS_ERR_NOT_INITIALIZED; }
    if (s_ops.read == NULL) { return UDS_STATUS_ERR_NOT_INITIALIZED; }

    rc = s_ops.read(key, (uint8_t *)data, len, &read_len);

    /* [#285] When this backend's nvm_store_delete() has no native delete
     * to call (s_ops.remove == NULL, [#287]), it falls back to
     * overwriting the record with the documented sentinel pattern
     * (NVM_STORE_DELETE_SENTINEL_LEN/_BYTE, nvm_store.h) — a raw
     * pass-through read would return that sentinel as ordinary
     * UDS_STATUS_OK data, indistinguishable to the caller from a genuine
     * 1-byte record. Translate it back to UDS_STATUS_ERR_DID_NOT_FOUND
     * here, so nvm_store_delete()+nvm_store_read() honour the same
     * "deleted reads as absent" contract every backend gets for free
     * from a true delete — no caller needs to know this one sometimes
     * fakes it.
     *
     * When s_ops.remove IS available, this check is simply never
     * triggered by a genuine delete: the record is truly gone from the
     * backend's storage, so s_ops.read() above already returns
     * DID_NOT_FOUND directly and rc never reaches UDS_STATUS_OK here.
     * This block exists purely for the no-native-delete fallback case.
     *
     * Scoped to key == NVM_KEY_SEC_STATE, not applied globally: that is
     * the only key nvm_store_delete() is ever called with anywhere in
     * this codebase, and this stack's "at least six keys" contract
     * (see this file's own PURPOSE banner) leaves room for a customer
     * application to define further keys of its own with formats this
     * stack knows nothing about — one of which could legitimately be a
     * real 1-byte record whose value happens to be 0x00 (a boolean
     * default-off flag, say). Narrowing to the one key this stack
     * itself ever deletes avoids reinterpreting a customer's own
     * genuine data as "deleted".
     *
     * KNOWN, ACCEPTED RESIDUAL LIMITATION (fallback path only, i.e. only
     * when s_ops.remove is NULL): a sentinel can only ever be a
     * heuristic, never a true delete. A genuinely corrupted
     * NVM_KEY_SEC_STATE record that happens to land on exactly this
     * 1-byte, 0x00 pattern (a torn/partial flash write on the customer's
     * driver that commits only the first byte, which happens to read
     * back as 0x00) is indistinguishable from a deliberate delete and is
     * now ALSO reported as DID_NOT_FOUND rather than failing closed as
     * corrupt. Every OTHER possible corruption signature — any other
     * length, or a full 12 bytes with a bad magic/version/CRC — still
     * correctly fails closed via core/uds_security_nvm.c's unchanged
     * checks; only this exact byte pattern is affected. [#287] closes
     * this gap entirely for any backend that implements s_ops.remove —
     * for one that doesn't, the accepted trade-off is unchanged from
     * #285: before that fix, EVERY delete was a guaranteed,
     * 100%-reproducible lockout; after it, only a narrow, driver-
     * dependent corruption coincidence could produce one.
     */
    if ((key == (uint16_t)NVM_KEY_SEC_STATE) &&
        (rc == UDS_STATUS_OK) &&
        (read_len == NVM_STORE_DELETE_SENTINEL_LEN) &&
        (((const uint8_t *)data)[0] == NVM_STORE_DELETE_SENTINEL_BYTE)) {
        rc = UDS_STATUS_ERR_DID_NOT_FOUND;
    }

    /* Only set *out_read_len on a genuine OK — matches
     * platform/zephyr/nvm_store.c's convention of leaving it untouched on
     * DID_NOT_FOUND (including the sentinel translation above, and
     * whatever s_ops.read() itself already left it as on its own
     * DID_NOT_FOUND path, which this must not override). */
    if ((rc == UDS_STATUS_OK) && (out_read_len != NULL)) {
        *out_read_len = read_len;
    }

    return rc;
}

uds_status_t nvm_store_delete(uint16_t key)
{
    if (!s_initialized) { return UDS_STATUS_ERR_NOT_INITIALIZED; }

    /*
     * [#287] Prefer the backend's own native delete when it has one: a
     * real flash driver (or the built-in RAM stub — see nvm_stub_delete()
     * in freertos_platform_api.c) that implements this makes the record
     * genuinely, unambiguously absent, with none of the corruption-
     * coincidence residual risk the sentinel fallback below carries.
     */
    if (s_ops.remove != NULL) {
        return s_ops.remove(key);
    }

    /*
     * FALLBACK — no native delete primitive on this backend (s_ops.remove
     * is NULL): the customer's flash driver may not support record-level
     * deletion. Implement delete as a documented sentinel write
     * (NVM_STORE_DELETE_SENTINEL_LEN/_BYTE in nvm_store.h) rather than
     * true removal. nvm_store_read() above translates that sentinel back
     * to UDS_STATUS_ERR_DID_NOT_FOUND itself ([#285]), so callers of this
     * backend's read/delete pair never observe the raw sentinel bytes —
     * "deleted reads as absent" holds the same way it would with a true
     * delete, with no caller-side awareness needed of how this backend
     * fakes it (or whether it needs to fake it at all — that's exactly
     * what s_ops.remove being available above skips).
     *
     * For records that must truly be absent (e.g. after a factory reset),
     * call nvm_store_erase_all() instead.
     */
    if (s_ops.write != NULL) {
        (void)s_ops.write(key, s_delete_sentinel, sizeof(s_delete_sentinel));
    }

    return UDS_STATUS_OK;
}

uds_status_t nvm_store_erase_all(void)
{
    /* [#280] NVM_KEY_SEC_STATE is deliberately NOT in this list — see the
     * contract in nvm_store.h. This primitive has no authorization concept
     * of its own, so it must not be able to clear the SecurityAccess
     * lockout record as an unreviewed side effect of a diagnostics-related
     * reset. Formerly included [EDS#211]; removed here, not renamed.
     *
     * MAINTENANCE: unlike the Zephyr/mock backends (which wipe everything
     * and restore NVM_KEY_SEC_STATE by exception), this backend wipes by
     * explicit allowlist. A new NVM_KEY_* added to nvm_store.h that should
     * be diagnostics-resettable must be added here too, or it silently
     * survives every FreeRTOS erase_all() call. */
    uint16_t     keys[] = {
        (uint16_t)NVM_KEY_DTC_MIRROR,
        (uint16_t)NVM_KEY_SESSION_STATS,
        (uint16_t)NVM_KEY_LIFECYCLE_CNT,
        (uint16_t)NVM_KEY_SCHEMA_VERSION,
    };
    uint8_t      i;

    if (!s_initialized) { return UDS_STATUS_ERR_NOT_INITIALIZED; }
    if (s_ops.write == NULL) { return UDS_STATUS_ERR_NOT_INITIALIZED; }

    /* Same s_delete_sentinel nvm_store_delete() writes — see
     * NVM_STORE_DELETE_SENTINEL_LEN/_BYTE in nvm_store.h. */
    for (i = 0U; i < (uint8_t)(sizeof(keys) / sizeof(keys[0])); i++) {
        (void)s_ops.write(keys[i], s_delete_sentinel, sizeof(s_delete_sentinel));
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
