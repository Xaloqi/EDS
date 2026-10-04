/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.h
 *
 * ECU       : VehicleStateECU
 * Version   : 0.1.0
 * Generated : 2026-10-04T10:24:58Z
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
#define GEN_DID_HANDLER_COUNT (5U)

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
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECUSerialNumber.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecuserialnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xF189 — ECUSoftwareVersionNumber.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECUSoftwareVersionNumber.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecusoftwareversionnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x0C00 — VehicleState_EngineSpeed_rpm.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for VehicleState_EngineSpeed_rpm.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_vehiclestate_enginespeed_rpm(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x0D00 — VehicleState_VehicleSpeed_kph.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for VehicleState_VehicleSpeed_kph.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_vehiclestate_vehiclespeed_kph(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/* --------------------------------------------------------------------------
 * Generated DID write handler declarations
 * -------------------------------------------------------------------------- */

/**
 * @brief Write handler for DID 0x0C00 — VehicleState_EngineSpeed_rpm.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for VehicleState_EngineSpeed_rpm.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_vehiclestate_enginespeed_rpm(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0x0D00 — VehicleState_VehicleSpeed_kph.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for VehicleState_VehicleSpeed_kph.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_vehiclestate_vehiclespeed_kph(
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
