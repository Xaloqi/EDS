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
#include "uds_transfer_ctx.h" /* uds_transfer_crc32_update/finalise — shared CRC-32
                                * engine also used by the transfer/flashing path
                                * (core/uds_transfer_ctx.c). Reused here rather than
                                * reimplemented: config/ is downstream of core/ in the
                                * declared build-layer order (see CMakeLists.txt —
                                * "application -> core -> transport -> config ->
                                * platform"), core/ is already a global include dir
                                * for the app target, and core/ has no dependency back
                                * on config/, so this does not create a layering
                                * inversion. */

#include <string.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Build-time sizing guarantee (issue #123)
 * -------------------------------------------------------------------------- */

/*
 * DTC_MIRROR_MAX_BYTES (computed FROM DTC_MIRROR_MAX_PERSISTED_DTCS, see
 * dtc_mirror.h) must never exceed the platform's per-record NVM cap. This
 * is what makes DTC_MIRROR_MAX_PERSISTED_DTCS a real guarantee rather than
 * a comment: if a future change to any of the wire-format sizing constants
 * (header bytes, entry bytes, the persisted-DTC cap) breaks the inequality,
 * the BUILD fails here instead of nvm_store_write() silently rejecting the
 * mirror write at runtime — the original issue #123 failure mode.
 *
 * MISRA_ANALYSIS guard: cppcheck's MISRA addon reports _Static_assert as
 * "Rule N/A" (not covered by any MISRA rule), which would inflate the
 * open-violation count. misra_analysis.py defines MISRA_ANALYSIS=1 during
 * static analysis passes to exclude this assertion from that pass only;
 * GCC and Clang always see it. Same pattern as core/uds_types.h.
 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(MISRA_ANALYSIS)
_Static_assert(
    DTC_MIRROR_MAX_BYTES <= NVM_MAX_RECORD_BYTES,
    "DTC_MIRROR_MAX_BYTES exceeds NVM_MAX_RECORD_BYTES — the DTC mirror "
    "would silently fail to persist at worst-case fault load (issue #123). "
    "Lower DTC_MIRROR_MAX_PERSISTED_DTCS in dtc_mirror.h, or raise "
    "NVM_MAX_RECORD_BYTES in platform/nvm_store.h after checking every "
    "other NVM_KEY_* consumer of that shared cap."
);
#endif

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

static bool s_initialized = false;

/* Serialization buffer — sized for max DTC mirror payload. */
static uint8_t s_mirror_buf[DTC_MIRROR_MAX_BYTES];

/* --------------------------------------------------------------------------
 * Internal CRC helper
 * -------------------------------------------------------------------------- */

/**
 * @brief Compute the CRC-32 used to protect a serialized mirror record.
 *
 * Thin wrapper around the shared transfer-service CRC-32 engine, using the
 * same init/finalise convention as its other callers (the flash_ops drivers
 * under platform/zephyr and platform/freertos).
 */
static uint32_t mirror_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = uds_transfer_crc32_update((uint32_t)0xFFFFFFFFUL, data, (uint32_t)len);

    return uds_transfer_crc32_finalise(crc);
}

/* --------------------------------------------------------------------------
 * Internal serialization helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Serialize the live DTC table into s_mirror_buf, with the new v1
 *        integrity header (magic + version + count) and CRC-32 trailer.
 *
 * Format: [magic:2][version:1][count:2][code_b2][code_b1][code_b0][status] × n [crc32:4]
 *
 * @param[in]  force_status_zero  If true, every entry is written with a
 *                                 status byte of 0x00 regardless of the live
 *                                 database value (used by dtc_mirror_clear_all()).
 * @param[out] out_len            Number of bytes written into s_mirror_buf.
 */
static uds_status_t mirror_serialize_internal(bool force_status_zero, size_t *out_len)
{
    size_t   pos;
    uint16_t count;
    uint32_t crc;

    pos = (size_t)0U;

    s_mirror_buf[pos++] = DTC_MIRROR_MAGIC_0;
    s_mirror_buf[pos++] = DTC_MIRROR_MAGIC_1;
    s_mirror_buf[pos++] = DTC_MIRROR_FORMAT_VERSION;

    /* Reserve 2 bytes for count — filled in after the entry loop. */
    s_mirror_buf[pos++] = (uint8_t)0U;
    s_mirror_buf[pos++] = (uint8_t)0U;

    count = (uint16_t)0U;

    /* Iterate using the dtc_database export accessor. Bounded by
     * DTC_MIRROR_MAX_PERSISTED_DTCS (issue #123), not UDS_MAX_DTC_COUNT:
     * dtc_database may hold more entries than the mirror can persist within
     * NVM_MAX_RECORD_BYTES. Entries at index >= the cap are simply not
     * written — the buffer-full break below is a redundant safety net,
     * not the primary mechanism now that this loop bound already matches
     * DTC_MIRROR_MAX_BYTES's derivation. */
    {
        uint16_t max_dtcs = (uint16_t)DTC_MIRROR_MAX_PERSISTED_DTCS;
        uint16_t idx;

        for (idx = (uint16_t)0U; idx < max_dtcs; idx++) {
            uint32_t dtc_code;
            uint8_t  status_byte;
            uds_status_t rc;

            rc = dtc_database_get_by_index(idx, &dtc_code, &status_byte);
            if (rc == UDS_STATUS_ERR_DID_NOT_FOUND) {
                break; /* Past end of registered table */
            }
            if (rc != UDS_STATUS_OK) {
                continue;
            }

            /* Leave room for this entry AND the trailing CRC. */
            if ((pos + (size_t)DTC_MIRROR_ENTRY_BYTES + (size_t)DTC_MIRROR_CRC_BYTES)
                > (size_t)DTC_MIRROR_MAX_BYTES) {
                break; /* Buffer full — should never happen with correct sizing */
            }

            /* 3-byte DTC code big-endian + 1-byte status. */
            s_mirror_buf[pos++] = (uint8_t)((dtc_code >> 16U) & 0xFFU);
            s_mirror_buf[pos++] = (uint8_t)((dtc_code >>  8U) & 0xFFU);
            s_mirror_buf[pos++] = (uint8_t)((dtc_code       ) & 0xFFU);
            s_mirror_buf[pos++] = force_status_zero ? (uint8_t)0x00U : status_byte;
            count++;
        }
    }

    /* Back-fill count in header. */
    s_mirror_buf[3] = (uint8_t)((count >> 8U) & 0xFFU);
    s_mirror_buf[4] = (uint8_t)( count         & 0xFFU);

    /* CRC-32 over [version..last entry byte], i.e. everything written so
     * far except the 2 magic bytes. */
    crc = mirror_crc32(&s_mirror_buf[2], pos - (size_t)2U);

    s_mirror_buf[pos++] = (uint8_t)((crc >> 24U) & 0xFFU);
    s_mirror_buf[pos++] = (uint8_t)((crc >> 16U) & 0xFFU);
    s_mirror_buf[pos++] = (uint8_t)((crc >>  8U) & 0xFFU);
    s_mirror_buf[pos++] = (uint8_t)( crc         & 0xFFU);

    *out_len = pos;
    return UDS_STATUS_OK;
}

/**
 * @brief Serialize the live DTC table into s_mirror_buf (status bytes as-is).
 *
 * @param[out] out_len  Number of bytes written into s_mirror_buf.
 */
static uds_status_t mirror_serialize(size_t *out_len)
{
    return mirror_serialize_internal(false, out_len);
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
    size_t   entries_bytes;
    size_t   expected_total;
    size_t   pos;
    uint16_t i;
    uint32_t crc_computed;
    uint32_t crc_stored;
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

    /*
     * MIGRATION (issue #114, deliberate decision — see dtc_mirror.h): a
     * record too short to hold a v1 header, or whose leading bytes do not
     * match the v1 magic, is treated as "no valid v1 mirror present" and
     * discarded exactly like the first-boot case above (OK, nothing
     * applied). This bucket also covers a legacy pre-header record written
     * by firmware built before this fix — it cannot be safely told apart
     * from unrelated short/garbage data by shape alone, so it is not
     * specially parsed. Losing legacy DTC history once, on first boot
     * after upgrading to this fix, is an accepted, documented trade-off.
     * A record that DOES carry the v1 magic is fully validated below —
     * any failure from here on is reported distinctly as
     * UDS_STATUS_ERR_NVM_DATA_CORRUPT rather than folded into this bucket.
     */
    if (bytes_read < (size_t)DTC_MIRROR_HEADER_BYTES) {
        return UDS_STATUS_OK;
    }
    if ((buf[0] != DTC_MIRROR_MAGIC_0) || (buf[1] != DTC_MIRROR_MAGIC_1)) {
        return UDS_STATUS_OK;
    }

    if (buf[2] != DTC_MIRROR_FORMAT_VERSION) {
        /* Known magic, unsupported/foreign version — cannot safely
         * reinterpret the layout. Distinct corrupt condition. */
        return UDS_STATUS_ERR_NVM_DATA_CORRUPT;
    }

    count  = (uint16_t)((uint16_t)buf[3] << 8U);
    count |= (uint16_t)  buf[4];

    if (count > (uint16_t)DTC_MIRROR_MAX_PERSISTED_DTCS) {
        /* Declared count exceeds the largest count this build could ever
         * have WRITTEN (issue #123: the mirror now caps what it persists
         * at DTC_MIRROR_MAX_PERSISTED_DTCS, not UDS_MAX_DTC_COUNT) —
         * cannot be a genuine record from this format. */
        return UDS_STATUS_ERR_NVM_DATA_CORRUPT;
    }

    entries_bytes  = (size_t)count * (size_t)DTC_MIRROR_ENTRY_BYTES;
    expected_total = (size_t)DTC_MIRROR_HEADER_BYTES + entries_bytes
                    + (size_t)DTC_MIRROR_CRC_BYTES;

    if (bytes_read != expected_total) {
        /* Truncated (or overlong) relative to what the header declares. */
        return UDS_STATUS_ERR_NVM_DATA_CORRUPT;
    }

    /*
     * Validate the CRC-32 over the whole record (version+count+entries)
     * BEFORE applying anything. This is what makes a corrupt record
     * all-or-nothing instead of the original partial-apply bug: no
     * dtc_database_set_status() call happens until every check above and
     * this CRC check have passed.
     */
    crc_computed = mirror_crc32(&buf[2], (size_t)3U + entries_bytes);
    crc_stored   = ((uint32_t)buf[expected_total - 4U] << 24U)
                 | ((uint32_t)buf[expected_total - 3U] << 16U)
                 | ((uint32_t)buf[expected_total - 2U] <<  8U)
                 | ((uint32_t)buf[expected_total - 1U]       );

    if (crc_computed != crc_stored) {
        return UDS_STATUS_ERR_NVM_DATA_CORRUPT;
    }

    /* Record fully validated — apply every entry. */
    pos = (size_t)DTC_MIRROR_HEADER_BYTES;

    for (i = (uint16_t)0U; i < count; i++) {
        uint32_t dtc_code;
        uint8_t  status_byte;

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
    size_t       serial_len;
    uds_status_t rc;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* Serialize with all status bytes forced to 0x00 (same v1 header/CRC
     * writer as dtc_mirror_flush_all() — one code path for the on-disk
     * format keeps the two writers from ever diverging). */
    rc = mirror_serialize_internal(true, &serial_len);
    if (rc != UDS_STATUS_OK) {
        return rc;
    }

    return nvm_store_write((uint16_t)NVM_KEY_DTC_MIRROR, s_mirror_buf, serial_len);
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

