// File: generated/routine_handlers.c
// GENERATED — do NOT edit manually.
// ECU: SafeBootECU  v1.0.0  Generated: 2026-05-20T07:21:49Z

#include "routine_handlers.h"
#include "routine_database.h"
#include "uds_types.h"

#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(safeboot_ecu, LOG_LEVEL_INF);

/* MCUboot image header magic: IMAGE_MAGIC = 0x96f3b83d (little-endian bytes). */
#define MCUBOOT_MAGIC_B0  (0x3DU)
#define MCUBOOT_MAGIC_B1  (0xB8U)
#define MCUBOOT_MAGIC_B2  (0xF3U)
#define MCUBOOT_MAGIC_B3  (0x96U)

/* Cached results for requestRoutineResults callbacks. */
static uint8_t s_precond_result[2U]  = { 0x00U, 0x00U };
static uint8_t s_precond_result_len  = 0U;
static uint8_t s_verifybl_result[2U] = { 0x00U, 0x00U };
static uint8_t s_verifybl_result_len = 0U;

/* =============================================================
 * RID 0xFF00 — CheckProgrammingPreconditions
 *
 * Verifies the ECU is safe to enter programming mode.
 * Returns a 2-byte result: byte 0 = status (0x01=PASS, 0x02=FAIL),
 *                          byte 1 = failure sub-code (0x00=none).
 *
 * Real integration: check supply voltage, engine-off status, etc.
 * This example always reports PASS.
 * ============================================================= */
uds_status_t routine_start_checkprogrammingpreconditions(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len;

    if ((result_buf == NULL) || (result_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (result_buf_len < (uint8_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    result_buf[0] = 0x01U; /* PASS */
    result_buf[1] = 0x00U; /* no failure sub-code */
    *result_len   = 2U;

    s_precond_result[0]  = result_buf[0];
    s_precond_result[1]  = result_buf[1];
    s_precond_result_len = 2U;

    LOG_INF("[0xFF00] CheckProgrammingPreconditions: PASS");
    return UDS_STATUS_OK;
}

uds_status_t routine_results_checkprogrammingpreconditions(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    if ((result_buf == NULL) || (result_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (result_buf_len < (uint8_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    result_buf[0] = s_precond_result[0];
    result_buf[1] = s_precond_result[1];
    *result_len   = s_precond_result_len;
    return UDS_STATUS_OK;
}

/* =============================================================
 * RID 0xFF01 — VerifyBootloaderIntegrity
 *
 * Opens flash area image_0, reads the first 4 bytes, and checks
 * the MCUboot image header magic (0x96f3b83d, little-endian).
 *
 * Result byte 0: 0x01=PASS  0x02=FAIL
 * Result byte 1: 0x00=OK  0x01=flash open error  0x02=read error
 *                0x03=magic mismatch
 * ============================================================= */
uds_status_t routine_start_verifybootloaderintegrity(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    const struct flash_area *fa;
    uint8_t hdr[4U];
    int rc;

    (void)opt_buf; (void)opt_len;

    if ((result_buf == NULL) || (result_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (result_buf_len < (uint8_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    rc = flash_area_open(FLASH_AREA_ID(image_0), &fa);
    if (rc != 0) {
        LOG_ERR("[0xFF01] flash_area_open(image_0) err %d", rc);
        result_buf[0] = 0x02U;
        result_buf[1] = 0x01U; /* flash open error */
        *result_len   = 2U;
        goto cache_and_return;
    }

    rc = flash_area_read(fa, (off_t)0, hdr, sizeof(hdr));
    flash_area_close(fa);

    if (rc != 0) {
        LOG_ERR("[0xFF01] flash_area_read err %d", rc);
        result_buf[0] = 0x02U;
        result_buf[1] = 0x02U; /* read error */
        *result_len   = 2U;
        goto cache_and_return;
    }

    if ((hdr[0U] == MCUBOOT_MAGIC_B0) &&
        (hdr[1U] == MCUBOOT_MAGIC_B1) &&
        (hdr[2U] == MCUBOOT_MAGIC_B2) &&
        (hdr[3U] == MCUBOOT_MAGIC_B3)) {
        LOG_INF("[0xFF01] MCUboot image magic OK.");
        result_buf[0] = 0x01U; /* PASS */
        result_buf[1] = 0x00U;
    } else {
        LOG_WRN("[0xFF01] Magic mismatch: %02X %02X %02X %02X",
                hdr[0U], hdr[1U], hdr[2U], hdr[3U]);
        result_buf[0] = 0x02U; /* FAIL */
        result_buf[1] = 0x03U; /* magic mismatch */
    }
    *result_len = 2U;

cache_and_return:
    s_verifybl_result[0]  = result_buf[0];
    s_verifybl_result[1]  = result_buf[1];
    s_verifybl_result_len = *result_len;
    return UDS_STATUS_OK;
}

uds_status_t routine_results_verifybootloaderintegrity(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    if ((result_buf == NULL) || (result_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (result_buf_len < (uint8_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    result_buf[0] = s_verifybl_result[0];
    result_buf[1] = s_verifybl_result[1];
    *result_len   = s_verifybl_result_len;
    return UDS_STATUS_OK;
}

/* Register all routines with routine_database */
uds_status_t routine_handlers_register_all(void)
{
    uds_status_t status;
    routine_entry_t entry;

    /* RID 0xFF00 — CheckProgrammingPreconditions */
    entry.rid            = (uint16_t)65280U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)0U;
    entry.start_cb       = routine_start_checkprogrammingpreconditions;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_checkprogrammingpreconditions;
    entry.description    = "CheckProgrammingPreconditions";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xFF01 — VerifyBootloaderIntegrity */
    entry.rid            = (uint16_t)65281U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_PROGRAMMING;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_verifybootloaderintegrity;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_verifybootloaderintegrity;
    entry.description    = "VerifyBootloaderIntegrity";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    return UDS_STATUS_OK;
}
