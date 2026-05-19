/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.h
 *
 * ECU       : SensorECU
 * Version   : 1.0.0
 * Generated : 2026-05-19T13:34:42Z
 *
 * PURPOSE: Declarations for all generated DID read/write handler functions
 *          and the generated DID registration entry point.
 *
 *          Architecture:
 *            diagnostics_config.yaml (dids section)
 *              -> tools/codegen.py (build_did_handlers_context)
 *                -> tools/templates/did_handlers.h.j2
 *                  -> generated/did_handlers.h  (this file)
 *
 *          This design allows DID handler stubs to be automatically generated
 *          from YAML configuration without modifying any core stack headers.
 *
 * WARNING: DO NOT EDIT MANUALLY.
 *          Regenerate: python3 tools/codegen.py --config <yaml> --out generated/
 *
 * SAFETY  : DID handlers provide read access to ECU measurement data and
 *           write access to configuration data. Session and security gating
 *           is enforced at the service layer (service_0x22.c / service_0x2E.c).
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef DID_HANDLERS_H
#define DID_HANDLERS_H

#include "uds_types.h"
#include "did_database.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generated DID count for this ECU configuration.
 * Must match GEN_DID_COUNT in generated_config.h.
 */
#define GEN_DID_HANDLER_COUNT (7U)

/* --------------------------------------------------------------------------
 * Generated DID read handler declarations
 *
 * One function per DID that has 'read' in its access list.
 * All conform to the did_read_cb_fn prototype from did_database.h.
 * -------------------------------------------------------------------------- */

/**
 * @brief Read handler for DID 0xF190 — VehicleIdentificationNumber.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 17 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for VehicleIdentificationNumber.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 17).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_vehicleidentificationnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xF18C — ECUSerialNumber.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 8 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECUSerialNumber.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 8).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecuserialnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xD001 — AmbientTemperature.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for AmbientTemperature.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ambienttemperature(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xD002 — SupplyVoltage.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for SupplyVoltage.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_supplyvoltage(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xD003 — SensorStatusBitmask.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for SensorStatusBitmask.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_sensorstatusbitmask(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xD010 — TemperatureThresholdHigh.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for TemperatureThresholdHigh.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_temperaturethresholdhigh(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xD011 — TemperatureThresholdLow.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for TemperatureThresholdLow.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_temperaturethresholdlow(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/* --------------------------------------------------------------------------
 * Generated DID write handler declarations
 * -------------------------------------------------------------------------- */

/**
 * @brief Write handler for DID 0xD010 — TemperatureThresholdHigh.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for TemperatureThresholdHigh.
 *
 * @param[in] buf  Buffer containing new DID data (1 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_temperaturethresholdhigh(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0xD011 — TemperatureThresholdLow.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for TemperatureThresholdLow.
 *
 * @param[in] buf  Buffer containing new DID data (1 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_temperaturethresholdlow(
    const uint8_t *buf,
    uint16_t       len);

/* --------------------------------------------------------------------------
 * Registration entry point
 * -------------------------------------------------------------------------- */

/**
 * @brief Register all generated DID entries with the DID database.
 *
 * Must be called after did_database_init() and before any UDS requests
 * are processed. Called by uds_generated_init().
 *
 * @return UDS_STATUS_OK if all DIDs registered successfully.
 * @return UDS_STATUS_ERR_* if any registration fails.
 */
uds_status_t did_handlers_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* DID_HANDLERS_H */
