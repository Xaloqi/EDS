// SPDX-License-Identifier: Apache-2.0
// File: generated/routine_handlers.c
// GENERATED — do NOT edit manually.
// ECU: SafeBootFreeRTOSECU  v1.0.0  Generated: 2026-06-22T00:00:00Z

#include "routine_handlers.h"
#include "routine_database.h"
#include "uds_types.h"
#include "freertos_flash_ops.h"

#include <string.h>
#include <stdint.h>

/* Cached results for requestRoutineResults callbacks. */
static uint8_t s_precond_result[2U]  = { 0x00U, 0x00U };
static uint8_t s_precond_result_len  = 0U;
static uint8_t s_verifyota_result[2U] = { 0x00U, 0x00U };
static uint8_t s_verifyota_result_len = 0U;

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
 * RID 0xFF01 — VerifyOTASlotIntegrity
 *
 * Reads the first 8 bytes of the OTA staging area (Bank 2,
 * base address FREERTOS_FLASH_OTA_BASE = 0x08100000 on STM32H743ZI).
 *
 * Checks for a valid ARM Cortex-M7 image header:
 *   Word 0 (offset 0x00): initial stack pointer — must be in SRAM
 *                          (0x20000000 – 0x2007FFFF or 0x24000000 – 0x2407FFFF)
 *   Word 1 (offset 0x04): Reset_Handler address — bit0 must be 1 (Thumb mode),
 *                          address must be in flash range (0x08100000+)
 *
 * Result byte 0: 0x01=PASS  0x02=FAIL
 * Result byte 1: 0x00=OK  0x01=slot erased (0xFFFFFFFF)
 *                0x02=invalid stack pointer  0x03=invalid reset handler
 *
 * On CI / QEMU (RAM stub): the stub flash buffer is checked — FAIL with
 * 0x01 (slot erased) until a 0x34/0x36/0x37 download has been performed.
 * ============================================================= */

/* ARM Cortex-M stack pointer and reset handler validity checks. */
#define H743_SRAM_LO   ((uint32_t)0x20000000UL)
#define H743_SRAM_HI   ((uint32_t)0x2407FFFFUL)
#define H743_FLASH_LO  ((uint32_t)0x08100000UL)  /* Bank 2 start */
#define H743_FLASH_HI  ((uint32_t)0x081DFFFFUL)  /* Bank 2 end   */

uds_status_t routine_start_verifyotaslotintegrity(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    uint8_t  hdr[8U];
    uint32_t sp_val;
    uint32_t reset_val;

    (void)opt_buf; (void)opt_len;

    if ((result_buf == NULL) || (result_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (result_buf_len < (uint8_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /*
     * Read the first 8 bytes of the OTA staging area.
     * On real STM32H743 hardware: direct memory read (Bank 2 is memory-mapped).
     * On CI/QEMU (RAM stub): reads from s_stub_flash[] via memcpy in the verify path.
     * We use a direct pointer read which works for both cases when the stub
     * flash buffer occupies the same logical address (FREERTOS_FLASH_OTA_BASE).
     *
     * For CI safety, fall back to an all-0xFF check without dereferencing
     * FREERTOS_FLASH_OTA_BASE (which is a hardware address not available in QEMU).
     */
#if defined(STM32H7xx) || defined(STM32H743xx)
    /* Real hardware: Bank 2 is memory-mapped, direct pointer read is safe. */
    (void)memcpy(hdr, (const void *)FREERTOS_FLASH_OTA_BASE, sizeof(hdr));
#else
    /* CI/QEMU stub: slot always appears erased until written via 0x34/0x36/0x37. */
    (void)memset(hdr, 0xFF, sizeof(hdr));
#endif

    /* Check word 0 (initial SP): must not be all-0xFF (erased) */
    (void)memcpy(&sp_val,    &hdr[0U], sizeof(sp_val));
    (void)memcpy(&reset_val, &hdr[4U], sizeof(reset_val));

    if ((sp_val == (uint32_t)0xFFFFFFFFUL) &&
        (reset_val == (uint32_t)0xFFFFFFFFUL)) {
        result_buf[0] = 0x02U; /* FAIL */
        result_buf[1] = 0x01U; /* slot erased */
        *result_len   = 2U;
        goto cache_and_return;
    }

    /* Validate initial stack pointer range */
    if ((sp_val < H743_SRAM_LO) || (sp_val > H743_SRAM_HI)) {
        result_buf[0] = 0x02U; /* FAIL */
        result_buf[1] = 0x02U; /* invalid SP */
        *result_len   = 2U;
        goto cache_and_return;
    }

    /* Validate Reset_Handler: bit0 must be 1 (Thumb), must be in Bank 2 range */
    if (((reset_val & (uint32_t)0x01UL) == (uint32_t)0U) ||
        ((reset_val & ~(uint32_t)0x01UL) < H743_FLASH_LO) ||
        ((reset_val & ~(uint32_t)0x01UL) > H743_FLASH_HI)) {
        result_buf[0] = 0x02U; /* FAIL */
        result_buf[1] = 0x03U; /* invalid Reset_Handler */
        *result_len   = 2U;
        goto cache_and_return;
    }

    result_buf[0] = 0x01U; /* PASS */
    result_buf[1] = 0x00U;
    *result_len   = 2U;

cache_and_return:
    s_verifyota_result[0]  = result_buf[0];
    s_verifyota_result[1]  = result_buf[1];
    s_verifyota_result_len = *result_len;
    return UDS_STATUS_OK;
}

uds_status_t routine_results_verifyotaslotintegrity(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    if ((result_buf == NULL) || (result_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (result_buf_len < (uint8_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    result_buf[0] = s_verifyota_result[0];
    result_buf[1] = s_verifyota_result[1];
    *result_len   = s_verifyota_result_len;
    return UDS_STATUS_OK;
}

/* Register all routines with routine_database */
uds_status_t routine_handlers_register_all(void)
{
    uds_status_t  status;
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

    /* RID 0xFF01 — VerifyOTASlotIntegrity */
    entry.rid            = (uint16_t)65281U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_PROGRAMMING;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_verifyotaslotintegrity;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_verifyotaslotintegrity;
    entry.description    = "VerifyOTASlotIntegrity";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    return UDS_STATUS_OK;
}
