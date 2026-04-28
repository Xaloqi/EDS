// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: config/dtc_mirror.c
 *
 * PURPOSE: DTC NVM mirror implementation.
 *
 * SAFETY  : ASIL-B candidate. Persists ConfirmedDTC status bits.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "dtc_mirror.h"
#include "dtc_database.h"
#include "nvm_store.h"
#include "uds_types.h"

#include <string.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

static bool s_initialized = false;

/* Serialization buffer — sized for max DTC mirror payload. */
static uint8_t s_mirror_buf[DTC_MIRROR_MAX_BYTES];

/* --------------------------------------------------------------------------
 * Internal serialization helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Serialize the live DTC table into s_mirror_buf.
 *
 * Format: [count_hi][count_lo][code_b2][code_b1][code_b0][status] × n
 *
 * @param[out] out_len  Number of bytes written into s_mirror_buf.
 */
static uds_status_t mirror_serialize(size_t *out_len)
{
    uint16_t count;
    size_t   pos;
    uds_status_t rc;

    /* Count registered DTCs. */
    rc = dtc_database_count_by_status(0xFFU, &count);
    if (rc != UDS_STATUS_OK) {
        /* 0xFF mask counts any non-zero status. For total count use a separate
         * approach: iterate up to UDS_MAX_DTC_COUNT with find-by-index.
         * Workaround: issue a count against an always-true mask — but
         * dtc_database_count_by_status only counts non-zero. Use 0x00 is wrong.
         * Best approach: expose dtc_database_get_count() — add it inline here. */
        count = 0U;
    }

    /*
     * Since dtc_database_count_by_status(0xFF) only counts non-zero status bytes,
     * we take a different approach: serialize ALL registered DTC entries regardless
     * of status, using a linear scan via dtc_database_find() which we cannot use
     * without knowing all codes. We need dtc_database_iterate().
     *
     * Since dtc_database doesn't expose an iterator in Phase 2, we use
     * dtc_database_get_all() added below, or serialize by code range scan.
     *
     * Clean solution: add dtc_database_get_all() accessor.
     */
    (void)rc;
    (void)count;

    /*
     * Serialize by querying each entry via the internal table accessor.
     * We call dtc_database_iterate_all() defined below.
     */
    pos = (size_t)0U;

    /* Reserve 2 bytes for count — filled in at end. */
    s_mirror_buf[pos++] = (uint8_t)0U;
    s_mirror_buf[pos++] = (uint8_t)0U;

    count = (uint16_t)0U;

    /* Iterate using the export accessor we'll add to dtc_database. */
    {
        uint16_t max_dtcs = (uint16_t)UDS_MAX_DTC_COUNT;
        uint16_t idx;

        for (idx = (uint16_t)0U; idx < max_dtcs; idx++) {
            uint32_t dtc_code;
            uint8_t  status_byte;

            rc = dtc_database_get_by_index(idx, &dtc_code, &status_byte);
            if (rc == UDS_STATUS_ERR_DID_NOT_FOUND) {
                break; /* Past end of registered table */
            }
            if (rc != UDS_STATUS_OK) {
                continue;
            }

            if ((pos + (size_t)DTC_MIRROR_ENTRY_BYTES) > (size_t)DTC_MIRROR_MAX_BYTES) {
                break; /* Buffer full — should never happen with correct sizing */
            }

            /* 3-byte DTC code big-endian + 1-byte status. */
            s_mirror_buf[pos++] = (uint8_t)((dtc_code >> 16U) & 0xFFU);
            s_mirror_buf[pos++] = (uint8_t)((dtc_code >>  8U) & 0xFFU);
            s_mirror_buf[pos++] = (uint8_t)((dtc_code       ) & 0xFFU);
            s_mirror_buf[pos++] = status_byte;
            count++;
        }
    }

    /* Back-fill count in header. */
    s_mirror_buf[0] = (uint8_t)((count >> 8U) & 0xFFU);
    s_mirror_buf[1] = (uint8_t)( count         & 0xFFU);

    *out_len = pos;
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t dtc_mirror_init(void)
{
    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    (void)memset(s_mirror_buf, 0, sizeof(s_mirror_buf));
    s_initialized = true;
    return UDS_STATUS_OK;
}

uds_status_t dtc_mirror_load(void)
{
    uint8_t  buf[DTC_MIRROR_MAX_BYTES];
    size_t   bytes_read;
    uint16_t count;
    size_t   pos;
    uint16_t i;
    uds_status_t rc;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    rc = nvm_store_read((uint16_t)NVM_KEY_DTC_MIRROR, buf, sizeof(buf), &bytes_read);
    if (rc == UDS_STATUS_ERR_DID_NOT_FOUND) {
        /* First boot — no mirror yet. This is normal. */
        return UDS_STATUS_OK;
    }
    if (rc != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    if (bytes_read < (size_t)DTC_MIRROR_HEADER_BYTES) {
        return UDS_STATUS_ERR_PLATFORM; /* Truncated header */
    }

    /* Decode count from header. */
    count  = (uint16_t)((uint16_t)buf[0] << 8U);
    count |= (uint16_t)  buf[1];

    pos = (size_t)DTC_MIRROR_HEADER_BYTES;

    for (i = (uint16_t)0U; i < count; i++) {
        uint32_t dtc_code;
        uint8_t  status_byte;

        if ((pos + (size_t)DTC_MIRROR_ENTRY_BYTES) > bytes_read) {
            break; /* Truncated entry */
        }

        dtc_code   = ((uint32_t)buf[pos + 0U] << 16U)
                   | ((uint32_t)buf[pos + 1U] <<  8U)
                   | ((uint32_t)buf[pos + 2U]       );
        status_byte = buf[pos + 3U];
        pos += (size_t)DTC_MIRROR_ENTRY_BYTES;

        /*
         * Restore status byte — ignores DTCs not registered in the current
         * database (handles firmware updates that add/remove DTC codes).
         */
        (void)dtc_database_set_status(dtc_code, status_byte);
    }

    return UDS_STATUS_OK;
}

uds_status_t dtc_mirror_flush_all(void)
{
    size_t       serial_len;
    uds_status_t rc;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    rc = mirror_serialize(&serial_len);
    if (rc != UDS_STATUS_OK) {
        return rc;
    }

    return nvm_store_write((uint16_t)NVM_KEY_DTC_MIRROR, s_mirror_buf, serial_len);
}

uds_status_t dtc_mirror_save_one(uint32_t dtc_code, uint8_t status_byte)
{
    (void)dtc_code;
    (void)status_byte;

    /*
     * NVS does not support partial record update — each write replaces
     * the entire record. Perform a full flush on every change.
     *
     * For systems with high-frequency DTC events, a dirty-flag approach
     * with periodic flush (e.g. every 10 ms tick) would be more efficient.
     * For Phase 3, eager flush ensures maximum data integrity.
     */
    return dtc_mirror_flush_all();
}

uds_status_t dtc_mirror_clear_all(void)
{
    size_t   pos;
    uint16_t i;
    uint16_t count;
    uds_status_t rc;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* Serialize with all status bytes set to 0x00. */
    pos   = (size_t)0U;
    count = (uint16_t)0U;

    s_mirror_buf[pos++] = 0U; /* count placeholder */
    s_mirror_buf[pos++] = 0U;

    for (i = (uint16_t)0U; i < (uint16_t)UDS_MAX_DTC_COUNT; i++) {
        uint32_t dtc_code;
        uint8_t  status_unused;

        rc = dtc_database_get_by_index(i, &dtc_code, &status_unused);
        if (rc == UDS_STATUS_ERR_DID_NOT_FOUND) { break; }
        if (rc != UDS_STATUS_OK) { continue; }

        if ((pos + (size_t)DTC_MIRROR_ENTRY_BYTES) > (size_t)DTC_MIRROR_MAX_BYTES) { break; }

        s_mirror_buf[pos++] = (uint8_t)((dtc_code >> 16U) & 0xFFU);
        s_mirror_buf[pos++] = (uint8_t)((dtc_code >>  8U) & 0xFFU);
        s_mirror_buf[pos++] = (uint8_t)((dtc_code       ) & 0xFFU);
        s_mirror_buf[pos++] = (uint8_t)0x00U; /* cleared status */
        count++;
    }

    s_mirror_buf[0] = (uint8_t)((count >> 8U) & 0xFFU);
    s_mirror_buf[1] = (uint8_t)( count         & 0xFFU);

    return nvm_store_write((uint16_t)NVM_KEY_DTC_MIRROR, s_mirror_buf, pos);
}

bool dtc_mirror_is_ready(void)
{
    return s_initialized;
}

/* Test-only reset function.
 *
 * [MISRA 8.7] Guarded by UNIT_TEST so the symbol only exists in test builds.
 * The prototype in dtc_mirror.h is likewise guarded by UNIT_TEST.
 */
#ifdef UNIT_TEST
void dtc_mirror_test_reset(void)
{
    s_initialized = false;
}
#endif /* UNIT_TEST */

