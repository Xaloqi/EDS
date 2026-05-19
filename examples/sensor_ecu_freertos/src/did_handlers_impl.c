// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/sensor_ecu_freertos/src/did_handlers_impl.c
 *
 * PURPOSE: Real sensor DID handler implementations for SensorECU FreeRTOS.
 *
 *          All handler functions use internal (static) names to avoid
 *          multiple-definition conflicts with the generated stubs in
 *          generated/did_handlers.c, which is always compiled.
 *
 *          did_handlers_register_all() is defined here as a strong symbol,
 *          overriding the generated version. It registers the real internal
 *          callbacks directly into the DID database — the generated stubs are
 *          compiled but never registered and therefore never called.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#include "did_handlers.h"
#include "did_database.h"
#include "sensor_ecu.h"
#include "uds_types.h"
#include "generated_config.h"

#include <string.h>
#include <stdint.h>

/* Temperature offset encoding: raw = T_°C + 40 */
#define TEMP_ENCODE(t)   ((uint8_t)((int16_t)(t) + 40))
#define TEMP_DECODE(raw) ((int8_t)((int16_t)(raw) - 40))

/* ---------------------------------------------------------------------------
 * Internal implementations — static to avoid symbol conflicts with
 * generated/did_handlers.c stubs which share the public names.
 * ---------------------------------------------------------------------------*/

static uds_status_t s_read_ambient_temp(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    sensor_state_t state;
    if ((buf == NULL) || (out_len == NULL) || (buf_len < 1U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    sensor_state_get(&state);
    buf[0]   = TEMP_ENCODE(state.temp_deg_c);
    *out_len = 1U;
    return UDS_STATUS_OK;
}

static uds_status_t s_read_supply_voltage(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    sensor_state_t state;
    if ((buf == NULL) || (out_len == NULL) || (buf_len < 2U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    sensor_state_get(&state);
    buf[0]   = (uint8_t)(state.voltage_mv >> 8U);
    buf[1]   = (uint8_t)(state.voltage_mv & 0xFFU);
    *out_len = 2U;
    return UDS_STATUS_OK;
}

static uds_status_t s_read_sensor_status(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    sensor_state_t state;
    if ((buf == NULL) || (out_len == NULL) || (buf_len < 1U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    sensor_state_get(&state);
    buf[0]   = state.status;
    *out_len = 1U;
    return UDS_STATUS_OK;
}

static uds_status_t s_read_temp_high(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    sensor_state_t state;
    if ((buf == NULL) || (out_len == NULL) || (buf_len < 1U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    sensor_state_get(&state);
    buf[0]   = TEMP_ENCODE(state.temp_threshold_high_deg_c);
    *out_len = 1U;
    return UDS_STATUS_OK;
}

static uds_status_t s_write_temp_high(const uint8_t *buf, uint16_t len)
{
    if ((buf == NULL) || (len != 1U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    sensor_set_temp_threshold_high(TEMP_DECODE(buf[0]));
    return UDS_STATUS_OK;
}

static uds_status_t s_read_temp_low(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    sensor_state_t state;
    if ((buf == NULL) || (out_len == NULL) || (buf_len < 1U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    sensor_state_get(&state);
    buf[0]   = TEMP_ENCODE(state.temp_threshold_low_deg_c);
    *out_len = 1U;
    return UDS_STATUS_OK;
}

static uds_status_t s_write_temp_low(const uint8_t *buf, uint16_t len)
{
    if ((buf == NULL) || (len != 1U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    sensor_set_temp_threshold_low(TEMP_DECODE(buf[0]));
    return UDS_STATUS_OK;
}

static uds_status_t s_read_vin(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    static const uint8_t vin[17U] = {
        '1','H','G','B','H','4','1','J','X','M','N','1','0','9','1','8','6'
    };
    if ((buf == NULL) || (out_len == NULL) || (buf_len < 17U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    (void)memcpy(buf, vin, 17U);
    *out_len = 17U;
    return UDS_STATUS_OK;
}

static uds_status_t s_read_serial(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    static const uint8_t sn[8U] = {
        0x58,0x41,0x4C,0x4F,0x51,0x49,0x30,0x31
    };
    if ((buf == NULL) || (out_len == NULL) || (buf_len < 8U)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    (void)memcpy(buf, sn, 8U);
    *out_len = 8U;
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * DID registration table — points to real internal callbacks above.
 * ---------------------------------------------------------------------------*/

static const did_entry_t s_did_table[GEN_DID_HANDLER_COUNT] = {
    {
        .did_id = 0xF190U, .access_flags = DID_ACCESS_READ,
        .min_session = UDS_SESSION_DEFAULT,
        .read_access_level = 0U, .write_access_level = 1U,
        .data_length = 17U, .read_cb = s_read_vin, .write_cb = NULL,
        .description = "VehicleIdentificationNumber"
    },
    {
        .did_id = 0xF18CU, .access_flags = DID_ACCESS_READ,
        .min_session = UDS_SESSION_DEFAULT,
        .read_access_level = 0U, .write_access_level = 1U,
        .data_length = 8U, .read_cb = s_read_serial, .write_cb = NULL,
        .description = "ECUSerialNumber"
    },
    {
        .did_id = 0xD001U, .access_flags = DID_ACCESS_READ,
        .min_session = UDS_SESSION_DEFAULT,
        .read_access_level = 0U, .write_access_level = 0U,
        .data_length = 1U, .read_cb = s_read_ambient_temp, .write_cb = NULL,
        .description = "AmbientTemperature"
    },
    {
        .did_id = 0xD002U, .access_flags = DID_ACCESS_READ,
        .min_session = UDS_SESSION_DEFAULT,
        .read_access_level = 0U, .write_access_level = 0U,
        .data_length = 2U, .read_cb = s_read_supply_voltage, .write_cb = NULL,
        .description = "SupplyVoltage"
    },
    {
        .did_id = 0xD003U, .access_flags = DID_ACCESS_READ,
        .min_session = UDS_SESSION_DEFAULT,
        .read_access_level = 0U, .write_access_level = 0U,
        .data_length = 1U, .read_cb = s_read_sensor_status, .write_cb = NULL,
        .description = "SensorStatusBitmask"
    },
    {
        .did_id = 0xD010U,
        .access_flags = (uint8_t)(DID_ACCESS_READ | DID_ACCESS_WRITE),
        .min_session = UDS_SESSION_EXTENDED,
        .read_access_level = 0U, .write_access_level = 1U,
        .data_length = 1U,
        .read_cb = s_read_temp_high, .write_cb = s_write_temp_high,
        .description = "TemperatureThresholdHigh"
    },
    {
        .did_id = 0xD011U,
        .access_flags = (uint8_t)(DID_ACCESS_READ | DID_ACCESS_WRITE),
        .min_session = UDS_SESSION_EXTENDED,
        .read_access_level = 0U, .write_access_level = 1U,
        .data_length = 1U,
        .read_cb = s_read_temp_low, .write_cb = s_write_temp_low,
        .description = "TemperatureThresholdLow"
    },
};

/* ---------------------------------------------------------------------------
 * did_handlers_register_all — strong symbol, overrides generated stub version.
 * Registers real sensor callbacks; generated stubs are compiled but unused.
 * ---------------------------------------------------------------------------*/

uds_status_t did_handlers_register_all(void)
{
    uint16_t     i;
    uds_status_t status;

    for (i = 0U; i < (uint16_t)GEN_DID_HANDLER_COUNT; i++) {
        status = did_database_register(&s_did_table[i]);
        if (status != UDS_STATUS_OK) {
            return status;
        }
    }
    return UDS_STATUS_OK;
}
