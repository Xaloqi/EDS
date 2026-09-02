// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_security_nvm.c
 *
 * PURPOSE: NVM adapter for UDS security attempt-counter persistence.
 *
 * [EDS#211] Persists the failed-attempt counter and lockout timer residual
 * as ONE versioned, CRC-32-checked NVM_KEY_SEC_STATE record, written with a
 * single nvm_store_write() call — see uds_security_nvm.h's WIRE FORMAT
 * section for the record layout and why this must be one write, not two.
 *
 * SAFETY  : ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_security_nvm.h"
#include "nvm_store.h"
#include "uds_transfer_ctx.h" /* uds_transfer_crc32_update/finalise — shared CRC-32 engine */
#include "uds_types.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Internal: CRC-32 helper
 *
 * Thin wrapper around the shared transfer-service CRC-32 engine, same
 * init/finalise convention as its other callers (config/dtc_mirror.c,
 * the flash_ops drivers under platform/zephyr and platform/freertos).
 * -------------------------------------------------------------------------- */
static uint32_t security_nvm_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = uds_transfer_crc32_update((uint32_t)0xFFFFFFFFUL, data, (uint32_t)len);

    return uds_transfer_crc32_finalise(crc);
}

/* --------------------------------------------------------------------------
 * Internal: serialize attempts/lockout_ms into a wire-format record
 * -------------------------------------------------------------------------- */
static void security_nvm_serialize(
    uint8_t  attempts,
    uint32_t lockout_ms,
    uint8_t  out_buf[UDS_SECURITY_NVM_RECORD_BYTES])
{
    uint32_t crc;

    out_buf[UDS_SECURITY_NVM_OFF_MAGIC0]  = UDS_SECURITY_NVM_MAGIC_0;
    out_buf[UDS_SECURITY_NVM_OFF_MAGIC1]  = UDS_SECURITY_NVM_MAGIC_1;
    out_buf[UDS_SECURITY_NVM_OFF_VERSION] = UDS_SECURITY_NVM_FORMAT_VERSION;
    out_buf[UDS_SECURITY_NVM_OFF_ATTEMPTS] = attempts;

    out_buf[UDS_SECURITY_NVM_OFF_LOCKOUT_MS + 0U] = (uint8_t)((lockout_ms >> 24U) & 0xFFU);
    out_buf[UDS_SECURITY_NVM_OFF_LOCKOUT_MS + 1U] = (uint8_t)((lockout_ms >> 16U) & 0xFFU);
    out_buf[UDS_SECURITY_NVM_OFF_LOCKOUT_MS + 2U] = (uint8_t)((lockout_ms >>  8U) & 0xFFU);
    out_buf[UDS_SECURITY_NVM_OFF_LOCKOUT_MS + 3U] = (uint8_t)( lockout_ms         & 0xFFU);

    /* CRC-32 over [version..lockout_ms], i.e. everything except the 2
     * magic bytes and the CRC field itself. */
    crc = security_nvm_crc32(
        &out_buf[UDS_SECURITY_NVM_OFF_VERSION],
        (size_t)(UDS_SECURITY_NVM_OFF_CRC - UDS_SECURITY_NVM_OFF_VERSION));

    out_buf[UDS_SECURITY_NVM_OFF_CRC + 0U] = (uint8_t)((crc >> 24U) & 0xFFU);
    out_buf[UDS_SECURITY_NVM_OFF_CRC + 1U] = (uint8_t)((crc >> 16U) & 0xFFU);
    out_buf[UDS_SECURITY_NVM_OFF_CRC + 2U] = (uint8_t)((crc >>  8U) & 0xFFU);
    out_buf[UDS_SECURITY_NVM_OFF_CRC + 3U] = (uint8_t)( crc         & 0xFFU);
}

uds_status_t uds_security_nvm_load(
    uint8_t  *out_attempts,
    uint32_t *out_lockout_ms)
{
    uint8_t      buf[UDS_SECURITY_NVM_RECORD_BYTES];
    size_t       bytes_read = 0U;
    uint32_t     crc_computed;
    uint32_t     crc_stored;
    uds_status_t rc;

    if ((out_attempts == NULL) || (out_lockout_ms == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    rc = nvm_store_read(
        (uint16_t)NVM_KEY_SEC_STATE,
        buf, sizeof(buf), &bytes_read);

    if (rc == UDS_STATUS_ERR_DID_NOT_FOUND) {
        /* First boot — no persisted state yet. Normal, not a fault. */
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }
    if (rc != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    /*
     * [EDS#211] A record shorter than the full wire format, or one whose
     * leading bytes don't match the v1 magic, cannot be a genuine
     * NVM_KEY_SEC_STATE record from this format — reported as corrupt
     * (a genuine fault), not treated as "no data" the way dtc_mirror.c's
     * legacy-record migration bucket does. Unlike the DTC mirror, there
     * is no pre-#211 header-less wire format to be backward compatible
     * with here: the two-independent-key layout this record replaces
     * used entirely different NVM keys (NVM_KEY_SEC_ATTEMPT_CTR /
     * NVM_KEY_SEC_LOCKOUT_MS), so nothing ever wrote raw, header-less
     * bytes under NVM_KEY_SEC_STATE — any record read back under this
     * key that fails validation is a real corruption, not a legacy
     * format.
     */
    if (bytes_read != (size_t)UDS_SECURITY_NVM_RECORD_BYTES) {
        return UDS_STATUS_ERR_NVM_DATA_CORRUPT;
    }
    if ((buf[UDS_SECURITY_NVM_OFF_MAGIC0] != UDS_SECURITY_NVM_MAGIC_0) ||
        (buf[UDS_SECURITY_NVM_OFF_MAGIC1] != UDS_SECURITY_NVM_MAGIC_1)) {
        return UDS_STATUS_ERR_NVM_DATA_CORRUPT;
    }
    if (buf[UDS_SECURITY_NVM_OFF_VERSION] != UDS_SECURITY_NVM_FORMAT_VERSION) {
        /* Known magic, unsupported/foreign version — cannot safely
         * reinterpret the layout. */
        return UDS_STATUS_ERR_NVM_DATA_CORRUPT;
    }

    /*
     * Validate the CRC-32 over the whole record (version+attempts+
     * lockout_ms) BEFORE trusting anything in it — all-or-nothing, same
     * reasoning as dtc_mirror.c's load path: no caller sees a partially-
     * valid result.
     */
    crc_computed = security_nvm_crc32(
        &buf[UDS_SECURITY_NVM_OFF_VERSION],
        (size_t)(UDS_SECURITY_NVM_OFF_CRC - UDS_SECURITY_NVM_OFF_VERSION));

    crc_stored = ((uint32_t)buf[UDS_SECURITY_NVM_OFF_CRC + 0U] << 24U)
               | ((uint32_t)buf[UDS_SECURITY_NVM_OFF_CRC + 1U] << 16U)
               | ((uint32_t)buf[UDS_SECURITY_NVM_OFF_CRC + 2U] <<  8U)
               | ((uint32_t)buf[UDS_SECURITY_NVM_OFF_CRC + 3U]       );

    if (crc_computed != crc_stored) {
        return UDS_STATUS_ERR_NVM_DATA_CORRUPT;
    }

    *out_attempts = buf[UDS_SECURITY_NVM_OFF_ATTEMPTS];
    *out_lockout_ms = ((uint32_t)buf[UDS_SECURITY_NVM_OFF_LOCKOUT_MS + 0U] << 24U)
                     | ((uint32_t)buf[UDS_SECURITY_NVM_OFF_LOCKOUT_MS + 1U] << 16U)
                     | ((uint32_t)buf[UDS_SECURITY_NVM_OFF_LOCKOUT_MS + 2U] <<  8U)
                     | ((uint32_t)buf[UDS_SECURITY_NVM_OFF_LOCKOUT_MS + 3U]       );

    return UDS_STATUS_OK;
}

uds_status_t uds_security_nvm_save(
    uint8_t  attempts,
    uint32_t lockout_ms)
{
    uint8_t buf[UDS_SECURITY_NVM_RECORD_BYTES];

    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    security_nvm_serialize(attempts, lockout_ms, buf);

    /*
     * [EDS#211] Single nvm_store_write() call — the entire fix. Before,
     * this was two independent writes (attempts, then lockout_ms) with an
     * unprotected window between them; now there is exactly one flash
     * operation, so either the whole record lands or the previous one
     * (still valid) is what a subsequent uds_security_nvm_load() reads
     * back — no intermediate state this function can produce is
     * observable as "half updated".
     */
    if (nvm_store_write((uint16_t)NVM_KEY_SEC_STATE, buf, sizeof(buf)) != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    return UDS_STATUS_OK;
}

uds_status_t uds_security_nvm_clear(void)
{
    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    return nvm_store_delete((uint16_t)NVM_KEY_SEC_STATE);
}
