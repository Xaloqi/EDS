/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.h
 *
 * ECU       : BMS_MainController
 * Version   : 1.0.0
 * Generated : 2026-05-20T07:21:48Z
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
#define GEN_DID_HANDLER_COUNT (24U)

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
 * @brief Read handler for DID 0xF187 — VehicleManufacturerSparePartNumber.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 11 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for VehicleManufacturerSparePartNumber.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 11).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_vehiclemanufacturersparepartnumber(
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
 * @brief Read handler for DID 0xDB00 — BMS_PackVoltage_mV.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_PackVoltage_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_packvoltage_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDB01 — BMS_PackCurrent_100mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_PackCurrent_100mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_packcurrent_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDB02 — BMS_PackPower_10W.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_PackPower_10W.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_packpower_10w(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDB03 — BMS_ContactorState.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_ContactorState.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_contactorstate(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDB04 — BMS_InsulationResistance_kOhm.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_InsulationResistance_kOhm.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_insulationresistance_kohm(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDC01 — BMS_CellGroup1_Voltage_mV.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_CellGroup1_Voltage_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_cellgroup1_voltage_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDC02 — BMS_CellGroup2_Voltage_mV.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_CellGroup2_Voltage_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_cellgroup2_voltage_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDC03 — BMS_CellGroup3_Voltage_mV.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_CellGroup3_Voltage_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_cellgroup3_voltage_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDC04 — BMS_CellGroup4_Voltage_mV.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_CellGroup4_Voltage_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_cellgroup4_voltage_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDD00 — BMS_MaxCellTemperature_degC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_MaxCellTemperature_degC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_maxcelltemperature_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDD01 — BMS_MinCellTemperature_degC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_MinCellTemperature_degC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_mincelltemperature_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDD02 — BMS_CoolantInletTemperature_degC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_CoolantInletTemperature_degC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_coolantinlettemperature_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDD03 — BMS_BMSBoardTemperature_degC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_BMSBoardTemperature_degC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_bmsboardtemperature_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDE00 — BMS_StateOfCharge_04pct.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_StateOfCharge_04pct.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_stateofcharge_04pct(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDE01 — BMS_StateOfHealth_04pct.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_StateOfHealth_04pct.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_stateofhealth_04pct(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xDE02 — BMS_BalancingStateBitmask.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_BalancingStateBitmask.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_balancingstatebitmask(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xD900 — BMS_FaultFlagsBitmask.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_FaultFlagsBitmask.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_faultflagsbitmask(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xD901 — BMS_FullChargeCapacity_Wh.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_FullChargeCapacity_Wh.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_fullchargecapacity_wh(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xD910 — BMS_OVThreshold_mV.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_OVThreshold_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_ovthreshold_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xD911 — BMS_UVThreshold_mV.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for BMS_UVThreshold_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_bms_uvthreshold_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/* --------------------------------------------------------------------------
 * Generated DID write handler declarations
 * -------------------------------------------------------------------------- */

/**
 * @brief Write handler for DID 0xF187 — VehicleManufacturerSparePartNumber.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 11 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for VehicleManufacturerSparePartNumber.
 *
 * @param[in] buf  Buffer containing new DID data (11 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_vehiclemanufacturersparepartnumber(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0xD910 — BMS_OVThreshold_mV.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for BMS_OVThreshold_mV.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_bms_ovthreshold_mv(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0xD911 — BMS_UVThreshold_mV.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for BMS_UVThreshold_mV.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_bms_uvthreshold_mv(
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
