// File: generated/routine_handlers.c
// GENERATED — do NOT edit manually.
// ECU: SensorECU  v1.0.0  Generated: 2026-05-19T13:34:41Z

#include "routine_handlers.h"
#include "routine_database.h"
#include "uds_types.h"

/* =============================================================
 * Routine callback stubs
 * Replace each stub body with your ECU application logic.
 * ============================================================= */

/* RID 0xFF00 — ResetSensorCalibration : startRoutine stub */
uds_status_t routine_start_resetsensorcalibration(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xFF00 — ResetSensorCalibration : requestRoutineResults stub */
uds_status_t routine_results_resetsensorcalibration(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    /* TODO: return routine results */
    return UDS_STATUS_OK;
}

/* RID 0xFF01 — SensorSelfTest : startRoutine stub */
uds_status_t routine_start_sensorselftest(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xFF01 — SensorSelfTest : requestRoutineResults stub */
uds_status_t routine_results_sensorselftest(
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

    /* RID 0xFF00 — ResetSensorCalibration */
    entry.rid            = (uint16_t)65280U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_resetsensorcalibration;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_resetsensorcalibration;
    entry.description    = "ResetSensorCalibration";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xFF01 — SensorSelfTest */
    entry.rid            = (uint16_t)65281U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)0U;
    entry.start_cb       = routine_start_sensorselftest;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_sensorselftest;
    entry.description    = "SensorSelfTest";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    return UDS_STATUS_OK;
}
