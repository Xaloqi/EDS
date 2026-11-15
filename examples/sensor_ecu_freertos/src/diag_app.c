/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/sensor_ecu_freertos/src/diag_app.c
 *
 * PURPOSE: The application's diagnostic handlers:
 *          DIDs 0xD001-0xD003 and 0xD010/0xD011, and routines
 *          0xFF00/0xFF01, over sensor_monitor_freertos.c's sensor state.
 *          diagnostics_config.yaml marks these 'source: app', so codegen
 *          declares them in generated/did_handlers.h and
 *          routine_handlers.h and registers them behind the same session
 *          and security checks, but emits no body; the bodies live here,
 *          outside generated/, and survive regeneration.
 *          A missing one is a link error. Before EDS#613 these bodies were
 *          hand edits inside generated/, which regeneration overwrote.
 * =============================================================================
 */

#include "did_handlers.h"
#include "routine_handlers.h"
#include "uds_types.h"
#include "sensor_ecu.h"

#include <stdbool.h>
#include <stddef.h>

/* ── DID read/write ─────────────────────────────────────────────────────── */

/* Delegates to sensor_monitor_freertos.c's real sensor_state_get(). */
uds_status_t did_read_ambienttemperature(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    sensor_state_t state;

    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    sensor_state_get(&state);
    buf[0]   = SENSOR_TEMP_ENCODE(state.temp_deg_c);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/* Delegates to sensor_monitor_freertos.c's real sensor_state_get(). */
uds_status_t did_read_supplyvoltage(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    sensor_state_t state;

    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    sensor_state_get(&state);
    /* uint16 big-endian, LSB = 1 mV (diagnostics_config.yaml 0xD002). */
    buf[0]   = (uint8_t)(state.voltage_mv >> 8U);
    buf[1]   = (uint8_t)(state.voltage_mv & 0xFFU);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/* Delegates to sensor_monitor_freertos.c's real sensor_state_get(). */
uds_status_t did_read_sensorstatusbitmask(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    sensor_state_t state;

    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    sensor_state_get(&state);
    buf[0]   = state.status;
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/* Delegates to sensor_monitor_freertos.c's real sensor_state_get(). */
uds_status_t did_read_temperaturethresholdhigh(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    sensor_state_t state;

    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    sensor_state_get(&state);
    buf[0]   = SENSOR_TEMP_ENCODE(state.temp_threshold_high_deg_c);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/* Delegates to sensor_monitor_freertos.c's real sensor_state_get(). */
uds_status_t did_read_temperaturethresholdlow(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    sensor_state_t state;

    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    sensor_state_get(&state);
    buf[0]   = SENSOR_TEMP_ENCODE(state.temp_threshold_low_deg_c);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/* Delegates to sensor_monitor_freertos.c's real sensor_set_temp_threshold_high(). */
uds_status_t did_write_temperaturethresholdhigh(
    const uint8_t *buf,
    uint16_t       len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len != (uint16_t)1U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    sensor_set_temp_threshold_high((int8_t)SENSOR_TEMP_DECODE(buf[0]));

    return UDS_STATUS_OK;
}

/* Delegates to sensor_monitor_freertos.c's real sensor_set_temp_threshold_low(). */
uds_status_t did_write_temperaturethresholdlow(
    const uint8_t *buf,
    uint16_t       len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len != (uint16_t)1U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    sensor_set_temp_threshold_low((int8_t)SENSOR_TEMP_DECODE(buf[0]));

    return UDS_STATUS_OK;
}

/* ── RoutineControl ─────────────────────────────────────────────────────── */

/* RID 0xFF00 — ResetSensorCalibration : startRoutine.
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

/* RID 0xFF01 — SensorSelfTest : startRoutine.
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
