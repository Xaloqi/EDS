/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.c
 *
 * ECU       : SensorECU
 * Version   : 1.0.0
 * Generated : 2026-05-20T07:21:49Z
 *
 * PURPOSE: DID read/write handler stubs and static DID registration table.
 *          Each handler returns deterministic stub data (zeros) until the
 *          application developer replaces the stub body with real sensor or
 *          NVM access code.
 *
 *          Architecture:
 *            diagnostics_config.yaml (dids section)
 *              -> tools/codegen.py (build_did_handlers_context)
 *                -> tools/templates/did_handlers.c.j2
 *                  -> generated/did_handlers.c  (this file)
 *
 *          This design allows the DID table to be YAML-driven without
 *          modifying the core DID database engine (config/did_database.c).
 *
 * WARNING: DO NOT EDIT MANUALLY.
 *          Regenerate: python3 tools/codegen.py --config <yaml> --out generated/
 *
 * SAFETY  : Stub implementations return zeros, which is safe but not
 *           functionally correct. Replace each handler body before integration
 *           testing. Handlers that expose safety-relevant signals must be
 *           assessed for the required ASIL level.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "did_handlers.h"
#include "did_database.h"
#include "uds_types.h"
#include "generated_config.h"

#include <string.h>
#include <stddef.h>

/* =============================================================================
 * Stub sensor / NVM backing stores
 *
 * One static array per readable DID. These arrays act as the "sensor" in stub
 * mode. In production, replace the memcpy in each handler with a call to the
 * real hardware driver, signal bus (e.g. AUTOSAR Port interface), or NVM driver.
 *
 * MISRA C:2012 Rule 8.9: These are at file scope rather than function scope to
 * allow test harnesses to inject mock values from outside this translation unit.
 * ============================================================================= */

/** Stub backing store for DID 0xF190 — VehicleIdentificationNumber (17 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'VehicleIdentificationNumber'. */
static uint8_t s_mock_vehicleidentificationnumber[17U] = { 0U };

/** Stub backing store for DID 0xF18C — ECUSerialNumber (8 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECUSerialNumber'. */
static uint8_t s_mock_ecuserialnumber[8U] = { 0U };

/** Stub backing store for DID 0xD001 — AmbientTemperature (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'AmbientTemperature'. */
static uint8_t s_mock_ambienttemperature[1U] = { 0U };

/** Stub backing store for DID 0xD002 — SupplyVoltage (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'SupplyVoltage'. */
static uint8_t s_mock_supplyvoltage[2U] = { 0U };

/** Stub backing store for DID 0xD003 — SensorStatusBitmask (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'SensorStatusBitmask'. */
static uint8_t s_mock_sensorstatusbitmask[1U] = { 0U };

/** Stub backing store for DID 0xD010 — TemperatureThresholdHigh (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'TemperatureThresholdHigh'. */
static uint8_t s_mock_temperaturethresholdhigh[1U] = { 0U };

/** Stub backing store for DID 0xD011 — TemperatureThresholdLow (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'TemperatureThresholdLow'. */
static uint8_t s_mock_temperaturethresholdlow[1U] = { 0U };


/* =============================================================================
 * DID Read handlers
 *
 * Each function conforms to the did_read_cb_fn prototype:
 *   uds_status_t fn(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
 *
 * The UDS service_0x22.c handler calls these callbacks after verifying session
 * and security level constraints. Callbacks need only return data — access
 * control is enforced by the service layer.
 * ============================================================================= */

/**
 * @brief Read handler for DID 0xF190 — VehicleIdentificationNumber.
 *
 * Stub: copies 17 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 17 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_vehicleidentificationnumber, 17U);
 *     *out_len = 17U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 17.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 17.
 */
uds_status_t did_read_vehicleidentificationnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)17U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for VehicleIdentificationNumber. */
    (void)memcpy(buf, s_mock_vehicleidentificationnumber, (size_t)17U);
    *out_len = (uint16_t)17U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xF18C — ECUSerialNumber.
 *
 * Stub: copies 8 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 8 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_ecuserialnumber, 8U);
 *     *out_len = 8U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 8.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 8.
 */
uds_status_t did_read_ecuserialnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)8U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for ECUSerialNumber. */
    (void)memcpy(buf, s_mock_ecuserialnumber, (size_t)8U);
    *out_len = (uint16_t)8U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xD001 — AmbientTemperature.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_ambienttemperature, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_ambienttemperature(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for AmbientTemperature. */
    (void)memcpy(buf, s_mock_ambienttemperature, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xD002 — SupplyVoltage.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_supplyvoltage, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_supplyvoltage(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for SupplyVoltage. */
    (void)memcpy(buf, s_mock_supplyvoltage, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xD003 — SensorStatusBitmask.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_sensorstatusbitmask, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_sensorstatusbitmask(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for SensorStatusBitmask. */
    (void)memcpy(buf, s_mock_sensorstatusbitmask, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xD010 — TemperatureThresholdHigh.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_temperaturethresholdhigh, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_temperaturethresholdhigh(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for TemperatureThresholdHigh. */
    (void)memcpy(buf, s_mock_temperaturethresholdhigh, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xD011 — TemperatureThresholdLow.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_temperaturethresholdlow, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_temperaturethresholdlow(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for TemperatureThresholdLow. */
    (void)memcpy(buf, s_mock_temperaturethresholdlow, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}


/* =============================================================================
 * DID Write handlers
 *
 * Each function conforms to the did_write_cb_fn prototype:
 *   uds_status_t fn(const uint8_t *buf, uint16_t len)
 *
 * Stub implementations store the value into the mock backing store.
 * Production implementations must validate the incoming data and write to NVM
 * or apply the value to the appropriate actuator or calibration record.
 * ============================================================================= */

/**
 * @brief Write handler for DID 0xD010 — TemperatureThresholdHigh.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 1 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'TemperatureThresholdHigh'.
 *
 * SAFETY: Write handlers that affect persistent calibration or actuators
 *         must include range checks and error handling. Review required.
 *
 * @param[in] buf  Pointer to new DID data (1 byte(s)).
 * @param[in] len  Number of bytes in buf. Must equal 1.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len != 1.
 */
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

    /* TODO [APPLICATION]: Validate range, then write to NVM/actuator. */
    (void)memcpy(s_mock_temperaturethresholdhigh, buf, (size_t)1U);

    return UDS_STATUS_OK;
}

/**
 * @brief Write handler for DID 0xD011 — TemperatureThresholdLow.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 1 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'TemperatureThresholdLow'.
 *
 * SAFETY: Write handlers that affect persistent calibration or actuators
 *         must include range checks and error handling. Review required.
 *
 * @param[in] buf  Pointer to new DID data (1 byte(s)).
 * @param[in] len  Number of bytes in buf. Must equal 1.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len != 1.
 */
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

    /* TODO [APPLICATION]: Validate range, then write to NVM/actuator. */
    (void)memcpy(s_mock_temperaturethresholdlow, buf, (size_t)1U);

    return UDS_STATUS_OK;
}


/* =============================================================================
 * Static DID registration table
 *
 * Maps DID identifiers to their handlers and access control metadata.
 * Passed to the DID database during initialisation via did_handlers_register_all().
 *
 * MISRA C:2012 Rule 8.4: Definition matches extern declaration in did_handlers.h.
 * ============================================================================= */

static const did_entry_t s_generated_did_table[GEN_DID_HANDLER_COUNT] = {
    /* ── DID 0xF190 — VehicleIdentificationNumber ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)61840U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)17U,
        .read_cb            = did_read_vehicleidentificationnumber,
        .write_cb           = NULL,
        .description        = "VehicleIdentificationNumber"
    },
    /* ── DID 0xF18C — ECUSerialNumber ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)61836U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)8U,
        .read_cb            = did_read_ecuserialnumber,
        .write_cb           = NULL,
        .description        = "ECUSerialNumber"
    },
    /* ── DID 0xD001 — AmbientTemperature ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)53249U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_ambienttemperature,
        .write_cb           = NULL,
        .description        = "AmbientTemperature"
    },
    /* ── DID 0xD002 — SupplyVoltage ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)53250U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_supplyvoltage,
        .write_cb           = NULL,
        .description        = "SupplyVoltage"
    },
    /* ── DID 0xD003 — SensorStatusBitmask ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)53251U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_sensorstatusbitmask,
        .write_cb           = NULL,
        .description        = "SensorStatusBitmask"
    },
    /* ── DID 0xD010 — TemperatureThresholdHigh ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)53264U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_temperaturethresholdhigh,
        .write_cb           = did_write_temperaturethresholdhigh,
        .description        = "TemperatureThresholdHigh"
    },
    /* ── DID 0xD011 — TemperatureThresholdLow ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)53265U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_temperaturethresholdlow,
        .write_cb           = did_write_temperaturethresholdlow,
        .description        = "TemperatureThresholdLow"
    }
};

/* =============================================================================
 * DID registration entry point
 * ============================================================================= */

/**
 * @brief Register all generated DIDs with the DID database.
 *
 * Iterates s_generated_did_table[] and calls did_database_register() for
 * each entry. Must be called from uds_generated_init() after did_database_init()
 * and before any UDS request processing.
 *
 * @return UDS_STATUS_OK if all 7 DID(s) registered successfully.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if database already contains this DID.
 * @return UDS_STATUS_ERR_* on other registration failure.
 */
uds_status_t did_handlers_register_all(void)
{
    uint16_t     i;
    uds_status_t status;

    for (i = (uint16_t)0U; i < (uint16_t)GEN_DID_HANDLER_COUNT; i++) {
        status = did_database_register(&s_generated_did_table[i]);
        if (status != UDS_STATUS_OK) {
            return status;
        }
    }

    return UDS_STATUS_OK;
}
