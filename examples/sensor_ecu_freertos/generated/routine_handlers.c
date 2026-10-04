// File: generated/routine_handlers.c
// GENERATED — do NOT edit manually, with ONE documented exception below.
// ECU: SensorECU (FreeRTOS)  v1.0.0  Generated: 2026-08-30T13:16:46Z
//
// [APP HOOK] Same pattern and rationale as examples/sensor_ecu/generated/
// routine_handlers.c (EDS#339) and examples/vehicle_state_ecu (EDS#341):
// the four bodies below delegate to sensor_monitor_freertos.c's real
// state instead of returning an empty stub result — routine_entry_t's
// callbacks are bare function pointers copied by value at
// routine_database_register() time, so the callback bodies here are the
// only place live behaviour can run. [EDS#349] Reapply if regenerated.

#include "routine_handlers.h"
#include "routine_database.h"
#include "uds_types.h"
#include "sensor_ecu.h"

/* [APP HOOK] RID 0xFF00 — ResetSensorCalibration : startRoutine.
 * Resets both thresholds to the factory defaults diagnostics_config.yaml
 * documents (-40 / +85 degC) via sensor_monitor_freertos.c's real setters. */
uds_status_t routine_start_resetsensorcalibration(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;

    sensor_set_temp_threshold_high((int8_t)SENSOR_TEMP_THRESHOLD_HIGH_DEFAULT_DEG_C);
    sensor_set_temp_threshold_low((int8_t)SENSOR_TEMP_THRESHOLD_LOW_DEFAULT_DEG_C);

    *result_len = 0U;
    return UDS_STATUS_OK;
}

/* RID 0xFF00 — ResetSensorCalibration : requestRoutineResults.
 * No results record defined in diagnostics_config.yaml beyond success. */
uds_status_t routine_results_resetsensorcalibration(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    return UDS_STATUS_OK;
}

/* [APP HOOK] RID 0xFF01 — SensorSelfTest : startRoutine.
 * Plausibility check against sensor_monitor_freertos.c's real state:
 * temperature within the physical bounds the YAML documents
 * (-40..+215 degC) and voltage > 0. Result byte: 0x00 pass, 0x01 fail. */
uds_status_t routine_start_sensorselftest(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    sensor_state_t state;
    bool           pass;

    (void)opt_buf; (void)opt_len;

    if ((result_buf == NULL) || (result_len == NULL) || (result_buf_len < 1U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    sensor_state_get(&state);
    pass = (state.temp_deg_c >= (int16_t)-40) &&
           (state.temp_deg_c <= (int16_t)215) &&
           (state.voltage_mv > 0U);

    result_buf[0] = pass ? (uint8_t)0x00U : (uint8_t)0x01U;
    *result_len   = 1U;

    return UDS_STATUS_OK;
}

/* RID 0xFF01 — SensorSelfTest : requestRoutineResults.
 * startRoutine already wrote the pass/fail byte directly into its own
 * result_buf (no asynchronous self-test to poll) — return the same
 * check so a results-only request after start still gets a real answer. */
uds_status_t routine_results_sensorselftest(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    sensor_state_t state;
    bool           pass;

    if ((result_buf == NULL) || (result_len == NULL) || (result_buf_len < 1U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    sensor_state_get(&state);
    pass = (state.temp_deg_c >= (int16_t)-40) &&
           (state.temp_deg_c <= (int16_t)215) &&
           (state.voltage_mv > 0U);

    result_buf[0] = pass ? (uint8_t)0x00U : (uint8_t)0x01U;
    *result_len   = 1U;

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
