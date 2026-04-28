// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/nvm_store.c
 *
 * PURPOSE: NVM store — Zephyr NVS backend implementation.
 *
 *          Provides production flash-backed storage for:
 *            - Security attempt counters (security vulnerability mitigation)
 *            - Security lockout timer residual (lockout survives power cycle)
 *            - DTC status-byte mirror (fault history survives reset)
 *            - Session statistics (diagnostic lifecycle counters)
 *            - ECU lifecycle counter (total reset count)
 *
 *          This file contains the real Zephyr NVS implementation.
 *          For host-side tests, nvm_store_mock.c provides a RAM-backed
 *          drop-in replacement.
 *
 * FLASH PARTITION CONFIGURATION:
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
 * WEAR LEVELING:
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
#include "uds_types.h"

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

/** Initialization flag. */
static bool s_initialized = false;

/** Cached copy of configuration. */
static nvm_store_cfg_t s_cfg;

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

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t nvm_store_init(const nvm_store_cfg_t *cfg)
{
    int rc;

    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    if (cfg == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (cfg->flash_dev == NULL) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (cfg->sector_count < (uint8_t)2U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    s_cfg = *cfg;

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

uds_status_t nvm_store_write(uint16_t key, const void *data, size_t len)
{
    ssize_t written;

    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if ((len == (size_t)0U) || (len > (size_t)NVM_MAX_RECORD_BYTES)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    written = nvs_write(&s_nvs_fs, key, data, len);
    if (written < (ssize_t)0) {
        LOG_ERR("NVM store: write key=0x%04X failed (rc=%d)", key, (int)written);
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* written == 0 means data was identical to stored value (no flash wear). */
    return UDS_STATUS_OK;
}

uds_status_t nvm_store_read(
    uint16_t  key,
    void     *data,
    size_t    len,
    size_t   *out_read_len)
{
    ssize_t bytes_read;

    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len == (size_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    bytes_read = nvs_read(&s_nvs_fs, key, data, len);
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

uds_status_t nvm_store_delete(uint16_t key)
{
    int rc;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    rc = nvs_delete(&s_nvs_fs, key);
    if ((rc < 0) && (rc != -ENOENT)) {
        LOG_ERR("NVM store: delete key=0x%04X failed (rc=%d)", key, rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    return UDS_STATUS_OK;
}

uds_status_t nvm_store_erase_all(void)
{
    int rc;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
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

    LOG_WRN("NVM store: all records erased (factory reset)");
    return UDS_STATUS_OK;
}

bool nvm_store_is_ready(void)
{
    return s_initialized;
}

#endif /* !NVM_STORE_HOST_MOCK */
