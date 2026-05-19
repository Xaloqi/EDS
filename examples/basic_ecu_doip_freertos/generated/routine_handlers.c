// File: generated/routine_handlers.c
// GENERATED — do NOT edit manually.
// ECU: BasicECU_DoIP_FreeRTOS  v1.6.0  Generated: 2026-05-19T13:34:42Z

#include "routine_handlers.h"
#include "routine_database.h"
#include "uds_types.h"

/* =============================================================
 * Routine callback stubs
 * Replace each stub body with your ECU application logic.
 * ============================================================= */

/* RID 0xFF00 — ECU_SelfTest : startRoutine stub */
uds_status_t routine_start_ecu_selftest(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xFF00 — ECU_SelfTest : requestRoutineResults stub */
uds_status_t routine_results_ecu_selftest(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    /* TODO: return routine results */
    return UDS_STATUS_OK;
}

/* RID 0xFF01 — ResetToFactoryDefaults : startRoutine stub */
uds_status_t routine_start_resettofactorydefaults(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xFF10 — EraseNVM : startRoutine stub */
uds_status_t routine_start_erasenvm(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xFF10 — EraseNVM : requestRoutineResults stub */
uds_status_t routine_results_erasenvm(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    /* TODO: return routine results */
    return UDS_STATUS_OK;
}

/* Register all routines with routine_database */
uds_status_t routine_handlers_register_all(void)
{
    uds_status_t status;
    routine_entry_t entry;

    /* RID 0xFF00 — ECU_SelfTest */
    entry.rid            = (uint16_t)65280U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)0U;
    entry.start_cb       = routine_start_ecu_selftest;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_ecu_selftest;
    entry.description    = "ECU_SelfTest";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xFF01 — ResetToFactoryDefaults */
    entry.rid            = (uint16_t)65281U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_resettofactorydefaults;
    entry.stop_cb        = NULL;
    entry.results_cb     = NULL;
    entry.description    = "ResetToFactoryDefaults";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xFF10 — EraseNVM */
    entry.rid            = (uint16_t)65296U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_PROGRAMMING;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_erasenvm;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_erasenvm;
    entry.description    = "EraseNVM";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    return UDS_STATUS_OK;
}
