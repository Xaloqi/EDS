// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/nvm_store.c
 *
 * PURPOSE: NVM store — Zephyr backend implementation.
 *
 *          TWO BACKENDS, selected at runtime (not build time):
 *
 *          1. Customer NVM ops ([#304] Stage 1, registered via
 *             zephyr_nvm_store_register_ops() — called from
 *             platform/zephyr/zephyr_platform_api.c's eds_platform_init()
 *             with the eds_nvm_ops_t a Zephyr application supplies in
 *             eds_platform_cfg_t.nvm). Mirrors
 *             platform/freertos/freertos_nvm.c's existing customer-ops
 *             contract exactly, including the NVM_KEY_SEC_STATE
 *             delete-sentinel translation ([#285]) and the explicit-
 *             allowlist erase_all() ([#280]). Lets a Zephyr integrator
 *             whose flash needs different treatment than built-in NVS can
 *             provide (e.g. STM32H7's 128 KB / up-to-4000ms-erase sectors —
 *             see platform/zephyr/nvm_store_append.c, this issue's Stage 2)
 *             plug in their own driver without writing a whole replacement
 *             nvm_store.c and re-wiring an example's CMakeLists.txt by hand.
 *
 *          2. Built-in Zephyr NVS (default — used whenever no ops are
 *             registered, which is every Zephyr target that does not call
 *             eds_platform_init() with a non-empty .nvm — i.e. every
 *             existing Zephyr target as of this Stage's introduction, since
 *             no in-tree Zephyr example called eds_platform_init() at all
 *             before it). This is this file's ORIGINAL, unchanged behaviour.
 *
 *          For host-side tests, nvm_store_mock.c provides a RAM-backed
 *          drop-in replacement for the whole file (built WITHOUT this one —
 *          see tests/CMakeLists.txt's duplicate-symbol note on
 *          test_nvm_store_zephyr_ops, which links this file directly
 *          instead, to test the ops path on host).
 *
 * FLASH PARTITION CONFIGURATION (built-in NVS backend only):
 *   The NVS partition must be defined in the board DTS:
 *
 *     &flash0 {
 *         partitions {
 *             diag_nvs: partition@7E000 {
 *                 label = "diag_nvs";
 *                 reg = <0x7E000 DT_SIZE_K(8)>;
 *             };
 *         };
 *     };
 *
 *   NVS requires at minimum 2 sectors. With sector_size=4096 and 2 sectors,
 *   the partition must be at least 8 KB. The above example uses 8 KB at
 *   offset 0x7E000 (last 8 KB of 512 KB flash).
 *
 * WEAR LEVELING (built-in NVS backend only):
 *   Zephyr NVS performs automatic sector-level wear leveling. With 2 sectors,
 *   NVS alternates writes between sectors once the active sector fills up.
 *   Expected write frequency: low (security events, session changes, resets).
 *   With typical NOR flash endurance of 100,000 erase cycles, 2 sectors of
 *   4 KB = ~100,000 × 4096 / (NVM_MAX_RECORD_BYTES × 6 records) ≈ millions
 *   of record updates before sector exhaustion.
 *
 * SAFETY  : ASIL-B candidate. Security-relevant data must persist.
 *           Write failures are non-fatal but logged.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "nvm_store.h"
#include "platform_api.h"   /* eds_nvm_ops_t — [#304] Stage 1 */
#include "uds_types.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Customer-ops module state — shared between both backends (and compiled
 * regardless of NVM_STORE_HOST_MOCK), so the dispatch below is reachable on
 * host tests that exercise only this path.
 * -------------------------------------------------------------------------- */

/** Customer-supplied NVM operations ([#304] Stage 1). All-NULL (the
 *  static-storage default, and what zephyr_nvm_store_register_ops(NULL)
 *  restores) means "not registered" — the built-in NVS backend below is
 *  used, preserving this file's original behaviour exactly for every
 *  Zephyr target that does not register ops. Set by
 *  platform/zephyr/zephyr_platform_api.c's eds_platform_init(). */
static eds_nvm_ops_t s_ops;

/** True after nvm_store_init() completes successfully, via EITHER
 *  backend. */
static bool s_initialized = false;

/** The delete-sentinel byte pattern — see NVM_STORE_DELETE_SENTINEL_LEN/_BYTE
 *  in nvm_store.h and platform/freertos/freertos_nvm.c's identical use.
 *  Only used on the customer-ops path: the built-in NVS backend has a
 *  native nvs_delete() and never needs this fallback. */
static const uint8_t s_delete_sentinel[NVM_STORE_DELETE_SENTINEL_LEN] = {
    NVM_STORE_DELETE_SENTINEL_BYTE
};

static bool ops_registered(void)
{
    return (s_ops.read != NULL) && (s_ops.write != NULL) && (s_ops.is_ready != NULL);
}

/* ============================================================================
 * zephyr_nvm_store_register_ops
 *
 * Called from platform/zephyr/zephyr_platform_api.c's eds_platform_init()
 * with either the application's ops or NULL. Mirrors
 * freertos_nvm_register_ops() — [#304] Stage 1: closes the "Ignored on
 * Zephyr" asymmetry documented on eds_nvm_ops_t in platform_api.h. Not part
 * of the public nvm_store_* API (nvm_store.h) — forward-declared directly
 * by its one production caller and by tests/unit_runnable/
 * test_nvm_store_zephyr_ops.c, the same pattern
 * platform/freertos/freertos_platform_api.c already uses for
 * freertos_nvm_register_ops().
 * ========================================================================== */
void zephyr_nvm_store_register_ops(const eds_nvm_ops_t *ops)
{
    if (ops != NULL) {
        s_ops = *ops;
    } else {
        (void)memset(&s_ops, 0, sizeof(s_ops));
    }
}

/* Zephyr NVS and flash headers — conditionally included for host build */
#ifndef NVM_STORE_HOST_MOCK

#include <zephyr/fs/nvs.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

/* [FIX] Was LOG_MODULE_DECLARE(basic_ecu) — see platform/zephyr/zephyr_can.c for rationale. */
LOG_MODULE_REGISTER(nvm_store, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * Internal NVS state
 * -------------------------------------------------------------------------- */

/** NVS filesystem instance. */
static struct nvs_fs s_nvs_fs;

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Perform schema version migration if the stored version is older.
 *
 * Called from nvm_store_init() after successful NVS mount. If no schema
 * record exists (first power-on), writes the current version. If an older
 * version is found, clears all records to force clean-slate initialization
 * (conservative migration strategy — counters reset to zero rather than
 * risk reading misaligned data).
 */
static void nvm_migrate_schema(void)
{
    uint16_t stored_version = (uint16_t)0U;
    ssize_t  rc;

    rc = nvs_read(&s_nvs_fs, (uint16_t)NVM_KEY_SCHEMA_VERSION,
                  &stored_version, sizeof(stored_version));

    if (rc == (ssize_t)sizeof(stored_version)) {
        if (stored_version == (uint16_t)NVM_SCHEMA_VERSION_CURRENT) {
            return; /* Schema matches — no migration needed. */
        }
        /* Older schema: erase all records. Counters will start from zero. */
        LOG_WRN("NVM schema migration: %u → %u (clearing store)",
                (unsigned)stored_version,
                (unsigned)NVM_SCHEMA_VERSION_CURRENT);
        (void)nvs_clear(&s_nvs_fs);
    }

    /* Write current schema version (first boot or after migration). */
    uint16_t current = (uint16_t)NVM_SCHEMA_VERSION_CURRENT;
    (void)nvs_write(&s_nvs_fs, (uint16_t)NVM_KEY_SCHEMA_VERSION,
                    &current, sizeof(current));
}

#endif /* !NVM_STORE_HOST_MOCK */

/* --------------------------------------------------------------------------
 * Customer-ops backend — schema check helper, mirrors
 * platform/freertos/freertos_nvm.c's nvm_check_schema().
 * -------------------------------------------------------------------------- */

static void nvm_check_schema_via_ops(void)
{
    uint16_t     stored_ver = 0U;
    uint16_t     current    = (uint16_t)NVM_SCHEMA_VERSION_CURRENT;
    uds_status_t rc;
    size_t       out_len    = 0U;

    rc = nvm_store_read((uint16_t)NVM_KEY_SCHEMA_VERSION,
                        &stored_ver, sizeof(stored_ver), &out_len);

    if ((rc == UDS_STATUS_OK) && (stored_ver == current)) {
        return; /* Schema matches — no action needed. */
    }

    /* Absent, unreadable, or mismatched: write the current version. Unlike
     * platform/freertos/freertos_nvm.c's equivalent, this does NOT call
     * nvm_store_erase_all() first — there is no evidence here (unlike that
     * backend) that a version mismatch implies stale/misformatted data
     * requiring a wipe; a fresh, never-before-initialized customer backend
     * simply has no schema record yet, which is the overwhelmingly common
     * case this path exists for. A customer driver whose own migration
     * needs differ should perform it in its own ops.write()/is_ready()
     * before registering, same as nvm_store_append.c owns its own
     * migration from first principles rather than this file dictating it. */
    (void)nvm_store_write((uint16_t)NVM_KEY_SCHEMA_VERSION,
                          &current, sizeof(current));
}

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t nvm_store_init(const nvm_store_cfg_t *cfg)
{
    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    if (ops_registered()) {
        /* [#304] Stage 1: customer-supplied ops take precedence over the
         * built-in NVS backend below, exactly as
         * platform/freertos/freertos_nvm.c already does for FreeRTOS. cfg
         * is accepted but unused on this path — flash geometry is the
         * customer driver's own concern. */
        (void)cfg;
        s_initialized = true;
        nvm_check_schema_via_ops();
        return UDS_STATUS_OK;
    }

#if defined(NVM_STORE_HOST_MOCK)
    /* No ops registered, and no real NVS available on this host build. */
    (void)cfg;
    return UDS_STATUS_ERR_NOT_INITIALIZED;
#else
    {
        int rc;

        if (cfg == NULL) {
            return UDS_STATUS_ERR_NULL_PTR;
        }
        if (cfg->flash_dev == NULL) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
        if (cfg->sector_count < (uint8_t)2U) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }

        s_nvs_fs.flash_device = (const struct device *)cfg->flash_dev;
        s_nvs_fs.offset       = (off_t)cfg->flash_offset;
        s_nvs_fs.sector_size  = (uint32_t)cfg->sector_size;
        s_nvs_fs.sector_count = (uint16_t)cfg->sector_count;

        rc = nvs_mount(&s_nvs_fs);
        if (rc < 0) {
            LOG_ERR("NVM store: nvs_mount failed (rc=%d)", rc);
            return UDS_STATUS_ERR_PLATFORM;
        }

        nvm_migrate_schema();

        s_initialized = true;

        LOG_INF("NVM store: mounted at offset 0x%08X (%u × %u B sectors)",
                (unsigned)cfg->flash_offset,
                (unsigned)cfg->sector_count,
                (unsigned)cfg->sector_size);

        return UDS_STATUS_OK;
    }
#endif
}

uds_status_t nvm_store_write(uint16_t key, const void *data, size_t len)
{
    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if ((len == (size_t)0U) || (len > (size_t)NVM_MAX_RECORD_BYTES)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (ops_registered()) {
        return s_ops.write(key, (const uint8_t *)data, len);
    }

#if defined(NVM_STORE_HOST_MOCK)
    return UDS_STATUS_ERR_NOT_INITIALIZED;
#else
    {
        ssize_t written = nvs_write(&s_nvs_fs, key, data, len);

        if (written < (ssize_t)0) {
            LOG_ERR("NVM store: write key=0x%04X failed (rc=%d)", key, (int)written);
            return UDS_STATUS_ERR_PLATFORM;
        }

        /* written == 0 means data was identical to stored value (no flash wear). */
        return UDS_STATUS_OK;
    }
#endif
}

uds_status_t nvm_store_read(
    uint16_t  key,
    void     *data,
    size_t    len,
    size_t   *out_read_len)
{
    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len == (size_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (ops_registered()) {
        uds_status_t rc;
        size_t       read_len = 0U;

        rc = s_ops.read(key, (uint8_t *)data, len, &read_len);

        /* [#285] Mirrors platform/freertos/freertos_nvm.c's identical
         * translation — see that file's own, much longer comment for the
         * full rationale. Scoped to NVM_KEY_SEC_STATE only: the one key
         * nvm_store_delete() is ever called with in this codebase, and the
         * only key whose fixed wire format (12 bytes,
         * UDS_SECURITY_NVM_RECORD_BYTES) is wider than the 1-byte sentinel,
         * so this can never misinterpret a customer's own genuine 1-byte
         * record at some other key. */
        if ((key == (uint16_t)NVM_KEY_SEC_STATE) &&
            (rc == UDS_STATUS_OK) &&
            (read_len == NVM_STORE_DELETE_SENTINEL_LEN) &&
            (((const uint8_t *)data)[0] == NVM_STORE_DELETE_SENTINEL_BYTE)) {
            rc = UDS_STATUS_ERR_DID_NOT_FOUND;
        }

        if ((rc == UDS_STATUS_OK) && (out_read_len != NULL)) {
            *out_read_len = read_len;
        }

        return rc;
    }

#if defined(NVM_STORE_HOST_MOCK)
    return UDS_STATUS_ERR_NOT_INITIALIZED;
#else
    {
        ssize_t bytes_read = nvs_read(&s_nvs_fs, key, data, len);

        if (bytes_read == (ssize_t)(-ENOENT)) {
            return UDS_STATUS_ERR_DID_NOT_FOUND;
        }

        if (bytes_read < (ssize_t)0) {
            LOG_ERR("NVM store: read key=0x%04X failed (rc=%d)", key, (int)bytes_read);
            return UDS_STATUS_ERR_PLATFORM;
        }

        if (out_read_len != NULL) {
            *out_read_len = (size_t)bytes_read;
        }

        return UDS_STATUS_OK;
    }
#endif
}

uds_status_t nvm_store_delete(uint16_t key)
{
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (ops_registered()) {
        /* [#287] Prefer the backend's own native delete when it has one —
         * see platform/freertos/freertos_nvm.c's identical preference and
         * rationale. */
        if (s_ops.remove != NULL) {
            return s_ops.remove(key);
        }

        /* FALLBACK — sentinel write (NVM_STORE_DELETE_SENTINEL_LEN/_BYTE,
         * nvm_store.h). nvm_store_read() above translates it back to
         * UDS_STATUS_ERR_DID_NOT_FOUND for NVM_KEY_SEC_STATE ([#285]). */
        return s_ops.write(key, s_delete_sentinel, sizeof(s_delete_sentinel));
    }

#if defined(NVM_STORE_HOST_MOCK)
    return UDS_STATUS_ERR_NOT_INITIALIZED;
#else
    {
        int rc = nvs_delete(&s_nvs_fs, key);

        if ((rc < 0) && (rc != -ENOENT)) {
            LOG_ERR("NVM store: delete key=0x%04X failed (rc=%d)", key, rc);
            return UDS_STATUS_ERR_PLATFORM;
        }

        return UDS_STATUS_OK;
    }
#endif
}

uds_status_t nvm_store_erase_all(void)
{
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (ops_registered()) {
        /* [#280] Explicit allowlist — mirrors
         * platform/freertos/freertos_nvm.c's erase_all() exactly (an
         * arbitrary customer ops table has no nvs_clear()-equivalent
         * primitive to wipe-and-restore around, unlike the NVS path
         * below). NVM_KEY_SEC_STATE is deliberately NOT in this list — see
         * the contract in nvm_store.h.
         *
         * MAINTENANCE: a new NVM_KEY_* added to nvm_store.h that should be
         * diagnostics-resettable must be added here too, or it silently
         * survives this backend's erase_all() call — same maintenance note
         * as freertos_nvm.c carries for its own copy of this list. */
        static const uint16_t keys[] = {
            (uint16_t)NVM_KEY_DTC_MIRROR,
            (uint16_t)NVM_KEY_SESSION_STATS,
            (uint16_t)NVM_KEY_LIFECYCLE_CNT,
            (uint16_t)NVM_KEY_SCHEMA_VERSION,
        };
        uint8_t  i;
        uint16_t current = (uint16_t)NVM_SCHEMA_VERSION_CURRENT;

        for (i = 0U; i < (uint8_t)(sizeof(keys) / sizeof(keys[0])); i++) {
            (void)s_ops.write(keys[i], s_delete_sentinel, sizeof(s_delete_sentinel));
        }

        (void)s_ops.write((uint16_t)NVM_KEY_SCHEMA_VERSION,
                          (const uint8_t *)&current, sizeof(current));

        return UDS_STATUS_OK;
    }

#if defined(NVM_STORE_HOST_MOCK)
    return UDS_STATUS_ERR_NOT_INITIALIZED;
#else
    {
        int          rc;
        uint8_t      sec_state_buf[NVM_MAX_RECORD_BYTES];
        size_t       sec_state_len = (size_t)0U;
        bool         sec_state_present;
        uds_status_t read_rc;

        /* [#280] NVM_KEY_SEC_STATE must survive this call — see the
         * contract in nvm_store.h. nvs_clear() has no per-key selectivity,
         * so preserve the record's raw bytes across the clear and restore
         * them after.
         *
         * Only a CONFIRMED absence (DID_NOT_FOUND) is safe to proceed on.
         * Any other read outcome (a flash/ECC read error on an otherwise-
         * valid record, say) is ambiguous — we cannot tell "never written"
         * from "written, but unreadable right now" — and proceeding could
         * silently destroy a real lockout record. Fail closed: refuse the
         * whole erase rather than risk that, exactly the class of trap
         * #280 exists to remove. */
        read_rc = nvm_store_read((uint16_t)NVM_KEY_SEC_STATE, sec_state_buf,
                                 sizeof(sec_state_buf), &sec_state_len);
        if (read_rc == UDS_STATUS_OK) {
            sec_state_present = true;
        } else if (read_rc == UDS_STATUS_ERR_DID_NOT_FOUND) {
            sec_state_present = false;
        } else {
            LOG_ERR("NVM store: erase_all aborted — could not confirm "
                    "NVM_KEY_SEC_STATE absence (rc=%d)", (int)read_rc);
            return UDS_STATUS_ERR_PLATFORM;
        }

        rc = nvs_clear(&s_nvs_fs);
        if (rc < 0) {
            LOG_ERR("NVM store: erase_all failed (rc=%d)", rc);
            return UDS_STATUS_ERR_PLATFORM;
        }

        /* Re-write schema version after full erase. */
        uint16_t current = (uint16_t)NVM_SCHEMA_VERSION_CURRENT;
        (void)nvs_write(&s_nvs_fs, (uint16_t)NVM_KEY_SCHEMA_VERSION,
                        &current, sizeof(current));

        if (sec_state_present) {
            rc = nvs_write(&s_nvs_fs, (uint16_t)NVM_KEY_SEC_STATE,
                           sec_state_buf, sec_state_len);
            if (rc < 0) {
                /* The clear already happened and cannot be undone; report
                 * the failure honestly rather than claim OK while the #280
                 * invariant silently did not hold. */
                LOG_ERR("NVM store: erase_all cleared but NVM_KEY_SEC_STATE "
                        "restore failed (rc=%d)", rc);
                return UDS_STATUS_ERR_PLATFORM;
            }
        }

        LOG_WRN("NVM store: all records erased except NVM_KEY_SEC_STATE (factory reset)");
        return UDS_STATUS_OK;
    }
#endif
}

bool nvm_store_is_ready(void)
{
    if (!s_initialized) {
        return false;
    }

    if (ops_registered()) {
        return s_ops.is_ready();
    }

    return true;
}

#if defined(NVM_STORE_HOST_MOCK)
/*
 * [#304] Stage 1 host-test hooks — this file carries no backing store of
 * its own on the ops path (the registered ops' data lives in whatever the
 * test or customer driver provides), so unlike
 * platform/zephyr/nvm_store_append.c's identically-named functions,
 * nvm_mock_reset() here only drops in-RAM state; there is no "flash" of
 * this file's own to erase. nvm_mock_corrupt_byte() is deliberately not
 * implemented for the same reason — see its own doc comment in
 * nvm_store.h.
 */
void nvm_mock_reset(void)
{
    (void)memset(&s_ops, 0, sizeof(s_ops));
    s_initialized = false;
}

void nvm_mock_deinit(void)
{
    s_initialized = false;
}
#endif /* NVM_STORE_HOST_MOCK */
